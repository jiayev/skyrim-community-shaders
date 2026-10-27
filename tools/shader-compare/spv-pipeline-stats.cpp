// spv-pipeline-stats: the GPU driver's statistics (VK_KHR_pipeline_executable_properties: registers, instructions, spills...) of a
// graphics pipeline made from a vertex and a fragment SPIR-V module, on the first device that supports them.
//
//   spv-pipeline-stats <vertex.spv> <fragment.spv> [--ir <dir>]
//
// One JSON object a line on stdout per executable: {"executable":..., "stage":..., "stats":{name: value}}. The pipeline's layout is
// reflected from the modules: every (set, binding) they declare, as the type the module gives it (a binding several variables alias
// with different types, as DXC's descriptor heap bindings are, is a mutable descriptor), runtime arrays as variable-count
// partially-bound arrays, and a 256-byte push constant range. The vertex input is the vertex module's locations, one binding; the
// colour attachments are the fragment module's output locations. Every feature the device supports is enabled, so any module the
// game's device accepts is accepted. What matters to the statistics is the code; the state around it is plausible, not the game's.
#include <vulkan/vulkan.h>
#include <spirv/unified1/spirv.hpp>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
	std::vector<std::uint32_t> ReadSpirv(const char* a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::vector<std::uint32_t> words(bytes.size() / 4);
		std::memcpy(words.data(), bytes.data(), words.size() * 4);
		return words;
	}

	struct Reflection
	{
		struct Binding
		{
			std::set<VkDescriptorType> types;
			std::uint32_t count = 1;
			bool runtime = false;
		};
		std::map<std::pair<std::uint32_t, std::uint32_t>, Binding> bindings;
		std::map<std::uint32_t, VkFormat> inputs, outputs;  // by location
		std::string entryPoint = "main";                    // the module's first entry point's name
		bool pushConstants = false;
	};

	VkFormat FormatOf(std::uint32_t a_component, std::uint32_t a_count, bool a_output)
	{
		// a_component: 0 float, 1 int, 2 uint
		static const VkFormat in[3][4] = { { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT },
			{ VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT },
			{ VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT } };
		static const VkFormat out[3][4] = { { VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT },
			{ VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SINT },
			{ VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT } };
		const std::uint32_t c = std::min(a_count, 4u) - 1;
		return a_output ? out[a_component][c] : in[a_component][c];
	}

	void Reflect(const std::vector<std::uint32_t>& a_words, Reflection& a_out)
	{
		struct Type
		{
			spv::Op op = spv::OpNop;
			std::vector<std::uint32_t> operands;
		};
		std::map<std::uint32_t, Type> types;
		std::map<std::uint32_t, std::uint32_t> constants;  // id -> value (32-bit ints)
		std::map<std::uint32_t, std::uint32_t> set, binding, location;
		std::set<std::uint32_t> builtin, block, bufferBlock;
		struct Variable
		{
			std::uint32_t id, type;
			spv::StorageClass storage;
		};
		std::vector<Variable> variables;
		std::uint32_t entryPoints = 0;
		for (std::size_t i = 5; i < a_words.size();) {
			const std::uint32_t count = a_words[i] >> 16;
			const auto op = static_cast<spv::Op>(a_words[i] & 0xFFFF);
			const std::uint32_t* w = &a_words[i];
			if (!count)
				break;
			switch (op) {
			case spv::OpDecorate:
				if (w[2] == spv::DecorationDescriptorSet)
					set[w[1]] = w[3];
				else if (w[2] == spv::DecorationBinding)
					binding[w[1]] = w[3];
				else if (w[2] == spv::DecorationLocation)
					location[w[1]] = w[3];
				else if (w[2] == spv::DecorationBuiltIn)
					builtin.insert(w[1]);
				else if (w[2] == spv::DecorationBlock)
					block.insert(w[1]);
				else if (w[2] == spv::DecorationBufferBlock)
					bufferBlock.insert(w[1]);
				break;
			case spv::OpMemberDecorate:
				if (w[3] == spv::DecorationBuiltIn)
					builtin.insert(w[1]);  // a struct of built-ins (gl_PerVertex)
				break;
			case spv::OpEntryPoint:
				if (entryPoints++ == 0)
					a_out.entryPoint = reinterpret_cast<const char*>(&w[3]);
				break;
			case spv::OpConstant:
				constants[w[2]] = w[3];
				break;
			case spv::OpVariable:
				variables.push_back({ w[2], w[1], static_cast<spv::StorageClass>(w[3]) });
				break;
			default:
				if (op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer || op == spv::OpTypeAccelerationStructureKHR) {
					Type t{ op, {} };
					for (std::uint32_t k = 2; k < count; ++k)
						t.operands.push_back(w[k]);
					types[w[1]] = t;
				}
				break;
			}
			i += count;
		}
		const auto pointee = [&](std::uint32_t a_pointer) { return types[a_pointer].operands.size() > 1 ? types[a_pointer].operands[1] : 0u; };
		// A scalar or vector's component kind and count.
		const auto scalar = [&](std::uint32_t a_type, std::uint32_t& a_component, std::uint32_t& a_count) {
			a_count = 1;
			auto t = types[a_type];
			if (t.op == spv::OpTypeVector) {
				a_count = t.operands[1];
				t = types[t.operands[0]];
			}
			a_component = t.op == spv::OpTypeFloat ? 0 : (t.operands.size() > 1 && t.operands[1] ? 1 : 2);
		};
		for (const auto& v : variables) {
			std::uint32_t type = pointee(v.type);
			if (v.storage == spv::StorageClassPushConstant) {
				a_out.pushConstants = true;
				continue;
			}
			if (v.storage == spv::StorageClassInput || v.storage == spv::StorageClassOutput) {
				if (builtin.contains(v.id) || builtin.contains(type) || !location.contains(v.id))
					continue;
				std::uint32_t component = 0, n = 1;
				scalar(type, component, n);
				(v.storage == spv::StorageClassInput ? a_out.inputs : a_out.outputs)[location[v.id]] = FormatOf(component, n, v.storage == spv::StorageClassOutput);
				continue;
			}
			if (!set.contains(v.id) || !binding.contains(v.id))
				continue;
			auto& b = a_out.bindings[{ set[v.id], binding[v.id] }];
			if (types[type].op == spv::OpTypeRuntimeArray) {
				b.runtime = true;
				type = types[type].operands[0];
			} else if (types[type].op == spv::OpTypeArray) {
				b.count = std::max(b.count, constants[types[type].operands[1]]);
				type = types[type].operands[0];
			}
			const auto& t = types[type];
			VkDescriptorType d = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			if (t.op == spv::OpTypeSampler)
				d = VK_DESCRIPTOR_TYPE_SAMPLER;
			else if (t.op == spv::OpTypeSampledImage)
				d = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			else if (t.op == spv::OpTypeImage) {
				const bool buffer = t.operands[1] == spv::DimBuffer, storage = t.operands[5] == 2;
				d = buffer ? (storage ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) :
				             (storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
			} else if (t.op == spv::OpTypeAccelerationStructureKHR)
				d = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
			else if (v.storage == spv::StorageClassUniform && block.contains(type))
				d = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			b.types.insert(d);
		}
	}

	int Fail(const char* a_what, VkResult a_result)
	{
		std::cerr << a_what << " failed: " << a_result << "\n";
		return 1;
	}
}

int main(int a_argc, char** a_argv)
{
	std::string irDirectory, heap;
	for (int i = 3; i + 1 < a_argc; i += 2) {
		const std::string option = a_argv[i];
		(option == "--ir" ? irDirectory : option == "--heap" ? heap : irDirectory) = a_argv[i + 1];
	}
	static const char* const kHeapSources[] = { "push-index", "push-address", "indirect-address", "constant-offset" };
	const bool heapKnown = heap.empty() || std::ranges::find(kHeapSources, heap) != std::end(kHeapSources);
	if (a_argc < 3 || a_argc % 2 == 0 || !heapKnown) {
		std::cerr << "usage: spv-pipeline-stats <vertex.spv> <fragment.spv> [--ir <dir>] [--heap push-index|push-address|indirect-address|constant-offset]\n"
					 "  --heap: the pipeline made under VK_EXT_descriptor_heap: every binding a heap descriptor at a constant offset, but the uniform\n"
					 "  buffers, which read from the given source (push-index: a heap descriptor at a pushed index, as DXVK binds them;\n"
					 "  push-address / indirect-address: an address in push data, or in memory a push data address points at, as DCLF binds its\n"
					 "  blocks). Without it, classic descriptor set layouts.\n";
		return 2;
	}
	const auto vertex = ReadSpirv(a_argv[1]), fragment = ReadSpirv(a_argv[2]);
	if (vertex.size() < 5 || fragment.size() < 5) {
		std::cerr << "cannot read the modules\n";
		return 1;
	}
	Reflection vs, fs;
	Reflect(vertex, vs);
	Reflect(fragment, fs);

	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.apiVersion = VK_API_VERSION_1_4;
	VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.pApplicationInfo = &app;
	VkInstance instance;
	if (const auto r = vkCreateInstance(&instanceInfo, nullptr, &instance))
		return Fail("vkCreateInstance", r);
	std::uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> physicals(count);
	vkEnumeratePhysicalDevices(instance, &count, physicals.data());

	// Every supported feature, of every struct the device reports, enabled.
	// The descriptor heap's features are chained only under --heap (its extension is enabled only then).
	VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
	VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executableFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
		heap.empty() ? nullptr : &heapFeatures };
	VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, &executableFeatures };
	VkPhysicalDeviceVulkan14Features v14{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, &mutableFeatures };
	VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &v14 };
	VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
	VkPhysicalDeviceVulkan11Features v11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12 };
	VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11 };
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	for (auto p : physicals) {
		vkGetPhysicalDeviceFeatures2(p, &features);
		if (executableFeatures.pipelineExecutableInfo && mutableFeatures.mutableDescriptorType) {
			physical = p;
			break;
		}
	}
	if (!physical) {
		std::cerr << "no device with pipeline executable properties and mutable descriptors\n";
		return 1;
	}
	if (!heap.empty() && !heapFeatures.descriptorHeap) {
		std::cerr << "--heap: the device has no VK_EXT_descriptor_heap\n";
		return 1;
	}
	VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT };
	VkPhysicalDeviceProperties2 properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, heap.empty() ? nullptr : &heapProperties };
	vkGetPhysicalDeviceProperties2(physical, &properties2);
	const VkPhysicalDeviceProperties& properties = properties2.properties;
	std::cerr << "device: " << properties.deviceName << ", driver " << properties.driverVersion << "\n";
	// Every extension the device has: a module's capability may need one.
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
	std::vector<const char*> extensionNames;
	for (const auto& e : extensions) {
		const std::string name = e.extensionName;
		// Extensions whose features this does not chain, or that need others it does not enable, are left out.
		if (name.find("video") != std::string::npos || name.find("VIDEO") != std::string::npos || name == "VK_NV_displacement_micromap" ||
			(name == "VK_EXT_descriptor_heap" && heap.empty()) || name == "VK_EXT_descriptor_buffer" || name == "VK_KHR_present_wait" || name == "VK_KHR_present_wait2" ||
			name.rfind("VK_KHR_swapchain", 0) == 0 || name.rfind("VK_EXT_full_screen", 0) == 0 || name == "VK_NVX_binary_import" ||
			name == "VK_NV_low_latency" || name == "VK_NV_low_latency2" || name == "VK_EXT_present_timing" || name == "VK_KHR_present_id" ||
			name == "VK_KHR_present_id2" || name == "VK_EXT_swapchain_maintenance1" || name == "VK_KHR_swapchain_maintenance1")
			continue;
		if (name == "VK_KHR_pipeline_executable_properties" || name == "VK_EXT_mutable_descriptor_type" || name == "VK_KHR_shader_float_controls2" ||
			name == "VK_EXT_shader_demote_to_helper_invocation" || name == "VK_KHR_shader_maximal_reconvergence" || name == "VK_KHR_shader_quad_control" ||
			name == "VK_KHR_shader_subgroup_uniform_control_flow" || name == "VK_KHR_compute_shader_derivatives" || name == "VK_NV_compute_shader_derivatives" ||
			name == "VK_EXT_fragment_shader_interlock" || name == "VK_KHR_shader_relaxed_extended_instruction" || name == "VK_KHR_shader_untyped_pointers" ||
			name == "VK_NV_raw_access_chains" || name == "VK_KHR_shader_expect_assume" || name == "VK_EXT_descriptor_heap")
			extensionNames.push_back(e.extensionName);
	}
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = 0;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;
	VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features };
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size());
	deviceInfo.ppEnabledExtensionNames = extensionNames.data();
	VkDevice device;
	if (const auto r = vkCreateDevice(physical, &deviceInfo, nullptr, &device))
		return Fail("vkCreateDevice", r);
	auto getExecutables = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
	auto getStatistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));

	// The layout: both modules' bindings, by set.
	std::map<std::uint32_t, std::vector<std::pair<std::uint32_t, Reflection::Binding>>> sets;
	{
		std::map<std::pair<std::uint32_t, std::uint32_t>, Reflection::Binding> all = vs.bindings;
		for (const auto& [key, b] : fs.bindings) {
			auto& merged = all[key];
			merged.types.insert(b.types.begin(), b.types.end());
			merged.count = std::max(merged.count, b.count);
			merged.runtime = merged.runtime || b.runtime;
		}
		for (const auto& [key, b] : all)
			sets[key.first].push_back({ key.second, b });
	}
	std::vector<VkDescriptorSetLayout> setLayouts;
	const std::uint32_t lastSet = sets.empty() ? 0 : sets.rbegin()->first;
	for (std::uint32_t s = 0; s <= lastSet && !sets.empty(); ++s) {
		std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
		std::vector<VkDescriptorBindingFlags> flags;
		std::vector<VkMutableDescriptorTypeListEXT> mutableLists;
		std::vector<std::vector<VkDescriptorType>> mutableTypes;
		bool updateAfterBind = false;
		const auto& list = sets[s];
		// A variable-count binding must be the set's last; the others with runtime arrays get a fixed large count.
		for (std::size_t i = 0; i < list.size(); ++i) {
			const auto& [number, b] = list[i];
			VkDescriptorSetLayoutBinding lb{};
			lb.binding = number;
			lb.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
			lb.descriptorCount = b.runtime ? 16384u : b.count;
			mutableTypes.emplace_back();
			if (b.types.size() > 1) {
				lb.descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT;
				for (auto t : b.types)
					if (t != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && t != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR && t != VK_DESCRIPTOR_TYPE_SAMPLER)
						mutableTypes.back().push_back(t);
			} else
				lb.descriptorType = *b.types.begin();
			layoutBindings.push_back(lb);
			VkDescriptorBindingFlags f = 0;
			if (b.runtime) {
				f = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
				updateAfterBind = true;
			}
			flags.push_back(f);
		}
		for (auto& t : mutableTypes)
			mutableLists.push_back({ static_cast<std::uint32_t>(t.size()), t.data() });
		VkMutableDescriptorTypeCreateInfoEXT mutableInfo{ VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT };
		mutableInfo.mutableDescriptorTypeListCount = static_cast<std::uint32_t>(mutableLists.size());
		mutableInfo.pMutableDescriptorTypeLists = mutableLists.data();
		VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, &mutableInfo };
		flagsInfo.bindingCount = static_cast<std::uint32_t>(flags.size());
		flagsInfo.pBindingFlags = flags.data();
		VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flagsInfo };
		layoutInfo.flags = updateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
		layoutInfo.bindingCount = static_cast<std::uint32_t>(layoutBindings.size());
		layoutInfo.pBindings = layoutBindings.data();
		VkDescriptorSetLayout setLayout;
		if (const auto r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout))
			return Fail("vkCreateDescriptorSetLayout", r);
		setLayouts.push_back(setLayout);
	}
	VkPushConstantRange push{ VK_SHADER_STAGE_ALL_GRAPHICS, 0, std::min(256u, properties.limits.maxPushConstantsSize) };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &push;
	VkPipelineLayout pipelineLayout;
	if (const auto r = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout))
		return Fail("vkCreatePipelineLayout", r);

	// --heap: every (set, binding) mapped. DXC's heap arrays (bindings 1000000 up, -fvk-bind-*-heap) and every other descriptor at a
	// constant heap offset; a uniform buffer from the chosen source, each its own push slot past the shader's push constants.
	std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
	if (!heap.empty()) {
		const auto resourceStride = static_cast<std::uint32_t>(std::max(heapProperties.imageDescriptorSize, heapProperties.bufferDescriptorSize));
		const auto bufferStride = static_cast<std::uint32_t>(heapProperties.bufferDescriptorSize);
		const auto samplerStride = static_cast<std::uint32_t>(heapProperties.samplerDescriptorSize);
		constexpr std::uint32_t kPushBase = 128;  // the mapped sources' push data, after the shader's own words
		std::uint32_t uniformBuffers = 0, element = 0;
		for (const auto& [set, list] : sets)
			for (const auto& [binding, b] : list) {
				VkDescriptorSetAndBindingMappingEXT mapping{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT };
				mapping.descriptorSet = set;
				mapping.firstBinding = binding;
				mapping.bindingCount = 1;
				mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
				const bool uniformOnly = b.types.size() == 1 && *b.types.begin() == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				if (uniformOnly && binding < 1000000 && heap != "constant-offset") {
					const std::uint32_t u = uniformBuffers++;
					mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
					if (heap == "push-index") {
						mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT;
						mapping.sourceData.pushIndex.heapOffset = u * bufferStride;
						mapping.sourceData.pushIndex.pushOffset = kPushBase + 4 * u;
						mapping.sourceData.pushIndex.heapIndexStride = bufferStride;
						mapping.sourceData.pushIndex.heapArrayStride = bufferStride;
					} else if (heap == "push-address") {
						mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT;
						mapping.sourceData.pushAddressOffset = kPushBase + 8 * u;
					} else {
						mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT;
						mapping.sourceData.indirectAddress.pushOffset = kPushBase;
						mapping.sourceData.indirectAddress.addressOffset = 8 * u;
					}
				} else {
					mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
					const bool heapArray = binding >= 1000000;
					mapping.sourceData.constantOffset.heapOffset = heapArray ? 0 : (element++) * resourceStride;
					mapping.sourceData.constantOffset.heapArrayStride = resourceStride;
					mapping.sourceData.constantOffset.samplerHeapOffset = heapArray ? 0 : element * samplerStride;
					mapping.sourceData.constantOffset.samplerHeapArrayStride = samplerStride;
					if (heapArray)
						mapping.bindingCount = 1;
				}
				mappings.push_back(mapping);
			}
		std::cerr << "heap: " << mappings.size() << " bindings mapped, " << uniformBuffers << " uniform buffers from " << heap << "\n";
	}
	VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{ VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT };
	mappingInfo.mappingCount = static_cast<std::uint32_t>(mappings.size());
	mappingInfo.pMappings = mappings.data();

	VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule modules[2];
	for (int m = 0; m < 2; ++m) {
		const auto& words = m ? fragment : vertex;
		moduleInfo.codeSize = words.size() * 4;
		moduleInfo.pCode = words.data();
		if (const auto r = vkCreateShaderModule(device, &moduleInfo, nullptr, &modules[m]))
			return Fail("vkCreateShaderModule", r);
	}
	VkPipelineShaderStageCreateInfo stages[2]{ { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = modules[0];
	stages[0].pName = vs.entryPoint.c_str();
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = modules[1];
	stages[1].pName = fs.entryPoint.c_str();
	if (!heap.empty())
		stages[0].pNext = stages[1].pNext = &mappingInfo;

	std::vector<VkVertexInputAttributeDescription> attributes;
	std::uint32_t offset = 0;
	for (const auto& [loc, format] : vs.inputs) {
		attributes.push_back({ loc, 0, format, offset });
		offset += 16;
	}
	VkVertexInputBindingDescription vertexBinding{ 0, std::max(offset, 16u), VK_VERTEX_INPUT_RATE_VERTEX };
	VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = attributes.empty() ? 0 : 1;
	vertexInput.pVertexBindingDescriptions = &vertexBinding;
	vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
	vertexInput.pVertexAttributeDescriptions = attributes.data();
	VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.cullMode = VK_CULL_MODE_BACK_BIT;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth.depthTestEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	std::vector<VkFormat> colourFormats;
	std::vector<VkPipelineColorBlendAttachmentState> blends;
	if (!fs.outputs.empty())
		for (std::uint32_t loc = 0; loc <= fs.outputs.rbegin()->first; ++loc) {
			const auto found = fs.outputs.find(loc);
			colourFormats.push_back(found == fs.outputs.end() ? VK_FORMAT_UNDEFINED : found->second);
			VkPipelineColorBlendAttachmentState blend{};
			blend.colorWriteMask = 0xF;
			blends.push_back(blend);
		}
	VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = static_cast<std::uint32_t>(blends.size());
	blend.pAttachments = blends.data();
	const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamics;
	VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	rendering.colorAttachmentCount = static_cast<std::uint32_t>(colourFormats.size());
	rendering.pColorAttachmentFormats = colourFormats.data();
	rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
	// The heap's pipelines take their flags from the flags2 struct (DESCRIPTOR_HEAP has no 32-bit flag) and have no layout.
	VkPipelineCreateFlags2CreateInfo flags2{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO, &rendering };
	flags2.flags = VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT |
	               (irDirectory.empty() ? 0 : VK_PIPELINE_CREATE_2_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR);
	VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, heap.empty() ? static_cast<const void*>(&rendering) : &flags2 };
	pipelineInfo.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | (irDirectory.empty() ? 0 : VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR);
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &assembly;
	pipelineInfo.pViewportState = &viewport;
	pipelineInfo.pRasterizationState = &raster;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depth;
	pipelineInfo.pColorBlendState = &blend;
	pipelineInfo.pDynamicState = &dynamic;
	pipelineInfo.layout = heap.empty() ? pipelineLayout : VK_NULL_HANDLE;
	VkPipeline pipeline;
	if (const auto r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline))
		return Fail("vkCreateGraphicsPipelines", r);

	VkPipelineInfoKHR info{ VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR };
	info.pipeline = pipeline;
	getExecutables(device, &info, &count, nullptr);
	std::vector<VkPipelineExecutablePropertiesKHR> executables(count, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR });
	getExecutables(device, &info, &count, executables.data());
	for (std::uint32_t e = 0; e < count; ++e) {
		VkPipelineExecutableInfoKHR executable{ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
		executable.pipeline = pipeline;
		executable.executableIndex = e;
		std::uint32_t statCount = 0;
		getStatistics(device, &executable, &statCount, nullptr);
		std::vector<VkPipelineExecutableStatisticKHR> stats(statCount, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR });
		getStatistics(device, &executable, &statCount, stats.data());
		std::string line = std::string("{\"executable\":\"") + executables[e].name + "\",\"stage\":" + std::to_string(executables[e].stages) +
		                   ",\"subgroupSize\":" + std::to_string(executables[e].subgroupSize) + ",\"stats\":{";
		for (std::uint32_t s = 0; s < statCount; ++s) {
			line += (s ? ",\"" : "\"") + std::string(stats[s].name) + "\":";
			switch (stats[s].format) {
			case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
				line += stats[s].value.b32 ? "true" : "false";
				break;
			case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
				line += std::to_string(stats[s].value.i64);
				break;
			case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
				line += std::to_string(stats[s].value.u64);
				break;
			default:
				line += std::to_string(stats[s].value.f64);
				break;
			}
		}
		std::cout << line << "}}\n";
		// --ir <dir>: the driver's internal representations of the executable (its machine code, where the driver gives it), a file each.
		if (!irDirectory.empty()) {
			auto getIr = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(vkGetDeviceProcAddr(device, "vkGetPipelineExecutableInternalRepresentationsKHR"));
			std::uint32_t irCount = 0;
			getIr(device, &executable, &irCount, nullptr);
			std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(irCount, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR });
			getIr(device, &executable, &irCount, irs.data());
			std::vector<std::vector<char>> data(irCount);
			for (std::uint32_t k = 0; k < irCount; ++k) {
				data[k].resize(irs[k].dataSize);
				irs[k].pData = data[k].data();
			}
			getIr(device, &executable, &irCount, irs.data());
			for (std::uint32_t k = 0; k < irCount; ++k) {
				std::string name = std::string(executables[e].name) + "-" + irs[k].name;
				for (auto& c : name)
					if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-')
						c = '_';
				std::ofstream(irDirectory + "/" + name + (irs[k].isText ? ".txt" : ".bin"), std::ios::binary).write(data[k].data(), static_cast<std::streamsize>(irs[k].dataSize));
				std::cerr << "ir: " << executables[e].name << " " << irs[k].name << " (" << irs[k].dataSize << " bytes" << (irs[k].isText ? ", text" : "") << ")\n";
			}
		}
	}
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	return 0;
}
