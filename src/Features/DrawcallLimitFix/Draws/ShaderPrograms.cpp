#include "ShaderPrograms.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <sstream>

#include <Tracy/Tracy.hpp>

#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Scene/ConstantEvaluator.h"

#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include <ORGModuleServices/ShaderCompiler.h>
#	define DCLF_HAS_SHADER_COMPILER 1
#endif

namespace DCLF
{
	namespace
	{
		constexpr const char* kSourcePath = "Data/Shaders/Lighting.hlsl";
		constexpr const char* kUtilitySourcePath = "Data/Shaders/Utility.hlsl";
		constexpr const char* kShaderDirectory = RenderGraphRuntime::kShaderDirectory;
		constexpr std::size_t kMaxLoggedFailures = 8;

		std::wstring Widen(const std::string& a_value)
		{
			return std::wstring(a_value.begin(), a_value.end());
		}

		// CS_DCLF_SHADER_DEBUG=1 builds the SPIR-V with source-level debug info (for Nsight and
		// RenderDoc; the source is embedded, and SnapshotSources' copy is there for editing), still optimized. Part of the
		// compile key, so the debug builds are cached beside the release ones. There is deliberately no
		// -Od form: unoptimized code reads per-frame constant buffers (VS b6, PS b7) that the epochs do
		// not supply, so every candidate is skipped and DCLF draws nothing.
		bool ShaderDebug()
		{
			return SwitchEnabled(Switch::ShaderDebug);
		}

		// The shader tree as the game sees it through MO2's virtual file system, copied to a real
		// directory a debugger outside the VFS can use as a source search path. The debug info names
		// files relative to the game directory (Data/Shaders/...), so the copy keeps that layout.
		void SnapshotSources(const std::vector<std::filesystem::path>& a_files)
		{
			auto root = std::filesystem::path(SwitchValue(Switch::ShaderSourceDir));
			if (root.empty()) {
				root = SwitchesDirectory();
				if (root.empty())
					return;
				root /= L"CommunityShaders-ShaderSource";
			}
			std::size_t copied = 0;
			for (const auto& file : a_files) {
				std::error_code ec;
				const auto destination = root / file.lexically_normal();
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (!ec && std::filesystem::copy_file(file, destination, std::filesystem::copy_options::overwrite_existing, ec))
					++copied;
			}
			logger::info("[DCLF] Shader debug info on; {} of {} shader files copied to {}", copied, a_files.size(), root.string());
		}
	}

	namespace
	{
		/**
		 * @brief Lighting.hlsl for a pulled build (DCLF_PULLED): its rows' inputs, which a device-generated draw's layout maps from
		 * the rows' addresses in its push data (DrawPipelines.cpp, the main layout), read from the rows its sequence names instead
		 * (Common/DCLFObjects.hlsli, DCLFMaterialRowAddress and DCLFPipelineRowAddress). A plain draw call binds once for draws
		 * of many pipeline slots (the Z-prepass's buckets are its depth pipelines'), so nothing it binds may be a row's:
		 * - the material row's: the PerMaterial blocks (b1), the material textures (t0-t15 but t14, and the features' t71 and
		 *   t74) and their samplers (s0-s15 but s14);
		 * - the pipeline row's: the PerTechnique blocks (b0), the PerGeometry template (b2) and the shadow mask (t14, s14); its
		 *   permutation block (b4) is Permutation.hlsli's (DCLF_PULLED_ROWS).
		 * A block's members become statics, which its function (DCLFPer<Block>Statics) assigns from the row; a texture or sampler
		 * a function of its own, behind a macro of its name. Line by line, so each declaration's replacement stays in its #if
		 * branch. Empty, logged, when a block's line is not one it knows.
		 */
		std::vector<std::byte> PulledLightingSource(std::span<const std::byte> a_source)
		{
			const std::string text(reinterpret_cast<const char*>(a_source.data()), a_source.size());
			static const std::regex texture(R"(^(\s*)((?:Texture2D|TextureCube|Texture2DArray|Texture3D)(?:\s*<[^>]*>)?)\s+(\w+)\s*:\s*register\(\s*t(\d+)\s*\)\s*;(.*)$)");
			static const std::regex sampler(R"(^(\s*)(SamplerState|SamplerComparisonState)\s+(\w+)\s*:\s*register\(\s*s(\d+)\s*\)\s*;(.*)$)");
			static const std::regex cbuffer(R"(^\s*cbuffer\s+(PerTechnique|PerMaterial|PerGeometry)\s*:\s*register\(\s*b([012])\s*\).*$)");
			static const std::regex member(
				R"(^(\s*)(row_major\s+)?(float|float2|float3|float4|uint|int|float3x3|float3x4|float4x4)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?\s*:\s*packoffset\(\s*c(\d+)(?:\.([xyzw]))?\s*\)\s*;(.*)$)");
			auto materialTexture = [](std::uint32_t a_register) {
				return (a_register < kPixelTextureSlots && a_register != kShadowMaskSlot) || FeatureMaterialSlot(a_register) >= 0;
			};
			// A block's row and its stage's place in it (DCLFObjects.hlsli).
			struct Block
			{
				const char* row = nullptr;
				const char* offset = nullptr;
			};
			auto blockOf = [](const std::string& a_name) {
				return a_name == "PerTechnique" ? Block{ "DCLFPipelineRowAddress", "kDCLFTechniqueBlock" } :
				       a_name == "PerMaterial"  ? Block{ "DCLFMaterialRowAddress", "kDCLFMaterialBlock" } :
				                                  Block{ "DCLFPipelineRowAddress", "kDCLFGeometryBlock" };
			};
			auto load = [](const std::string& a_type) -> const char* {
				return a_type == "float4"   ? "DCLFRowFloat4" :
				       a_type == "float3"   ? "DCLFRowFloat3" :
				       a_type == "float2"   ? "DCLFRowFloat2" :
				       a_type == "float"    ? "DCLFRowFloat" :
				       a_type == "uint"     ? "DCLFRowUint" :
				       a_type == "int"      ? "(int)DCLFRowUint" :
				       a_type == "float3x3" ? "DCLFRowFloat3x3" :
				       a_type == "float3x4" ? "DCLFRowFloat3x4" :
				                              "DCLFRowFloat4x4";
			};
			std::istringstream in(text);
			std::ostringstream out;
			std::string line, blockName;
			Block block{};
			bool inBlock = false, opened = false;
			std::ostringstream assignments;  // the block's DCLFPer<Block>Statics body, its #if lines with it
			std::uint32_t textures = 0, samplers = 0, members = 0;
			auto fail = [&](const std::string& a_line) {
				logger::error("[DCLF] Pulled Lighting source: {} line not understood: {}", blockName, a_line);
				return std::vector<std::byte>{};
			};
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				std::smatch m;
				if (inBlock) {
					const auto first = line.find_first_not_of(" \t");
					const std::string trimmed = first == std::string::npos ? std::string{} : line.substr(first);
					if (!opened && trimmed == "{") {
						opened = true;
						out << "\n";
					} else if (trimmed.rfind("};", 0) == 0) {
						// The block's members as statics, assigned from the row by the pulled entry points (Lighting.hlsl,
						// DCLFRowStatics) once the row's address is known: a member's name is one other code reuses (a struct
						// field), so not a macro.
						inBlock = false;
						out << "void DCLF" << blockName << "Statics()\n{\n" << assignments.str() << "}\n";
					} else if (trimmed.empty() || trimmed[0] == '#' || trimmed.rfind("//", 0) == 0) {
						out << line << "\n";
						if (!trimmed.empty() && trimmed[0] == '#')
							assignments << line << "\n";
					} else if (std::regex_match(line, m, member)) {
						const std::string type = m[3].str(), name = m[4].str();
						const bool matrix = type.find('x') != std::string::npos;
						// A column-major matrix's registers are its columns, and an array of matrices is none the shader has.
						if (matrix && (!m[2].matched || m[5].matched))
							return fail(line);
						const std::uint32_t component = m[7].matched ? static_cast<std::uint32_t>(std::string("xyzw").find(m[7].str()[0])) : 0u;
						const std::uint32_t offset = static_cast<std::uint32_t>(std::stoul(m[6].str())) * 16 + component * 4;
						const auto source = [&](std::uint32_t a_offset) { return fmt::format("{}({}, {} + {})", load(type), block.row, block.offset, a_offset); };
						out << m[1].str() << "static " << m[2].str() << type << " " << name << (m[5].matched ? "[" + m[5].str() + "]" : std::string{}) << ";" << m[8].str()
							<< "\n";
						if (m[5].matched) {
							// An array's elements: a register each.
							const auto count = static_cast<std::uint32_t>(std::stoul(m[5].str()));
							for (std::uint32_t e = 0; e < count; ++e)
								assignments << "\t" << name << "[" << e << "] = " << source(offset + 16 * e) << ";\n";
						} else {
							assignments << "\t" << name << " = " << source(offset) << ";\n";
						}
						++members;
					} else {
						return fail(line);
					}
					continue;
				}
				if (std::regex_match(line, m, cbuffer)) {
					inBlock = true;
					opened = false;
					blockName = m[1].str();
					block = blockOf(blockName);
					assignments.str({});
					out << "// " << blockName << ": the draw's " << (blockName == "PerMaterial" ? "material" : "pipeline") << " row's (DCLF_PULLED)\n";
				} else if (std::regex_match(line, m, texture) &&
						   (materialTexture(static_cast<std::uint32_t>(std::stoul(m[4].str()))) || std::stoul(m[4].str()) == kShadowMaskSlot)) {
					const std::string type = m[2].str(), name = m[3].str(), slot = m[4].str();
					const std::string index = std::stoul(slot) == kShadowMaskSlot ? std::string("DCLFPipelineTextureIndex()") : "DCLFMaterialTextureIndex(" + slot + ")";
					out << m[1].str() << type << " DCLFRow_" << name << "() { " << type << " r = ResourceDescriptorHeap[" << index << "]; return r; }\n#define " << name
						<< " DCLFRow_" << name << "()" << m[5].str() << "\n";
					++textures;
				} else if (std::regex_match(line, m, sampler) && std::stoul(m[4].str()) < 16) {  // s0-s15
					const std::string type = m[2].str(), name = m[3].str(), slot = m[4].str();
					const std::string index = std::stoul(slot) == kShadowMaskSlot ? std::string("DCLFPipelineSamplerIndex()") : "DCLFMaterialSamplerIndex(" + slot + ")";
					out << m[1].str() << type << " DCLFRow_" << name << "() { " << type << " r = SamplerDescriptorHeap[" << index << "]; return r; }\n#define " << name
						<< " DCLFRow_" << name << "()" << m[5].str() << "\n";
					++samplers;
				} else {
					out << line << "\n";
				}
			}
			const std::string result = out.str();
			logger::info("[DCLF] Pulled Lighting source: {} texture, {} sampler and {} block member declarations read from the draw's rows", textures, samplers, members);
			std::vector<std::byte> bytes(result.size());
			std::memcpy(bytes.data(), result.data(), result.size());
			return bytes;
		}
	}

	struct ShaderPrograms::ShadowEntry
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		std::shared_future<org::services::ShaderArtifact> vertex;
		std::shared_future<org::services::ShaderArtifact> pixel;
#endif
		std::unique_ptr<ShadowProgram> program;
		bool failed = false;
		std::uint8_t onDemand = 0;  // kOnDemand*: the stages no precompile had asked for
		std::chrono::steady_clock::time_point requested;
	};

	struct ShaderPrograms::Entry
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		std::shared_future<org::services::ShaderArtifact> vertex;
		std::shared_future<org::services::ShaderArtifact> pixel;
		std::shared_future<org::services::ShaderArtifact> depthPixel;
		std::shared_future<org::services::ShaderArtifact> pulledVertex;
		std::shared_future<org::services::ShaderArtifact> pulledDepthPixel;
#endif
		std::unique_ptr<Program> program;
		bool failed = false;
		std::uint8_t onDemand = 0;  // kOnDemand*: the stages no precompile had asked for
		std::chrono::steady_clock::time_point requested;
	};

	std::string ShaderPrograms::OnDemandStages(std::uint8_t a_stages)
	{
		std::string text;
		for (const auto& [bit, name] : { std::pair{ kOnDemandVertex, "VS" }, std::pair{ kOnDemandPixel, "PS" }, std::pair{ kOnDemandDepthPixel, "depth PS" },
				 std::pair{ kOnDemandPulledVertex, "pulled VS" }, std::pair{ kOnDemandPulledDepthPixel, "pulled depth PS" } })
			if (a_stages & bit)
				text += fmt::format("{}{}", text.empty() ? "" : ", ", name);
		return text;
	}

	ShaderPrograms& ShaderPrograms::Get()
	{
		static ShaderPrograms programs;
		return programs;
	}

	bool ShaderPrograms::Enabled() const
	{
		return !sourcesMissing && RenderGraphRuntime::Get().ShaderCompiler() != nullptr;
	}

	bool ShaderPrograms::LoadSources()
	{
		if (sourcesLoaded)
			return true;
		std::ifstream file(kSourcePath, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (bytes.empty()) {
			logger::error("[DCLF] {} is missing; no SPIR-V builds", kSourcePath);
			sourcesMissing = true;
			return false;
		}
		source.resize(bytes.size());
		std::memcpy(source.data(), bytes.data(), bytes.size());
		pulledSource = PulledLightingSource(source);
		// Every shader file is a potential include; the compiler re-hashes one only when it changes.
		dependencies = RenderGraphRuntime::ShaderSourceFiles();
		{
			std::ifstream utility(kUtilitySourcePath, std::ios::binary);
			std::vector<char> utilityBytes((std::istreambuf_iterator<char>(utility)), std::istreambuf_iterator<char>());
			utilitySource.resize(utilityBytes.size());
			if (!utilityBytes.empty())
				std::memcpy(utilitySource.data(), utilityBytes.data(), utilityBytes.size());
			else
				logger::warn("[DCLF] {} is missing; the shadow views stay native", kUtilitySourcePath);
		}
		sourcesLoaded = true;
		logger::info("[DCLF] SPIR-V builds of {}: {} shader files tracked as dependencies", kSourcePath, dependencies.size());
		if (ShaderDebug())
			SnapshotSources(dependencies);
		return true;
	}

#if defined(DCLF_HAS_SHADER_COMPILER)
	namespace
	{
		std::shared_future<org::services::ShaderArtifact> RequestStage(std::span<const std::byte> a_source, const std::vector<std::filesystem::path>& a_dependencies,
			const RE::BSShader& a_lighting, bool a_pixel, std::uint32_t a_descriptor, bool a_depthOnly, bool a_pulled, const char* a_sourceName)
		{
			org::services::ShaderCompileRequest request{};
			ZoneScopedN("CS.DCLF.Shaders.RequestStage");
			request.sourceName = a_sourceName;
			request.source = a_source;
			request.entryPoint = L"main";
			request.target = a_pixel ? L"ps_6_6" : L"vs_6_6";
			request.format = org::services::ShaderBinaryFormat::Spirv;
			request.warningsAsErrors = false;
			request.debugInfo = ShaderDebug();
			request.languageVersion = L"2018";  // CS's shaders are written for FXC's semantics
			request.includeDirectories = { kShaderDirectory };
			request.dependencyFiles = a_dependencies;
			for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(a_lighting, a_pixel ? SIE::ShaderClass::Pixel : SIE::ShaderClass::Vertex, a_descriptor))
				request.defines.push_back({ Widen(name), Widen(value) });
			request.defines.push_back({ L"DCLF_ORG", L"1" });
			// The Z-prepass build of the pixel stage: everything past the alpha test is compiled away, so it
			// reads only what the discard needs and none of the per-frame pixel bindings.
			if (a_depthOnly)
				request.defines.push_back({ L"DCLF_DEPTH_ONLY", L"1" });
			// The five per-object PerGeometry variables come from a GPU-resident table indexed by the draw's
			// own object index instead of from its constant buffer (DCLF_BINDLESS), and so do the three registers
			// that would otherwise make the binding record per-object (DCLF_BINDLESS_DRAW): the alpha test
			// reference, the emissive multiplier, and Light Limit Fix's room index and shadow bit mask.
			request.defines.push_back({ L"DCLF_BINDLESS", L"1" });
			request.defines.push_back({ L"DCLF_BINDLESS_DRAW", L"1" });
			// The shadow views' Utility stages and the Z-prepass's Lighting ones draw with plain indirect draws (ShadowViewPass,
			// MainOpaquePass) and fetch their own indices, vertices and per-draw words (Utility.hlsl, Lighting.hlsl).
			if (a_pulled)
				request.defines.push_back({ L"DCLF_PULLED", L"1" });
			// A pulled Lighting stage reads its pipeline row's permutation block itself (Permutation.hlsli), as it does the rows'
			// other inputs (PulledLightingSource).
			if (a_pulled && a_sourceName == kSourcePath)
				request.defines.push_back({ L"DCLF_PULLED_ROWS", L"1" });
			// CS_DCLF_FOLIAGE_PARITY: the colour pass's alpha-tested draws write what each pixel shows, and the Z-prepass's which of
			// them owns its depth (Lighting.hlsl).
			if (a_pixel && a_depthOnly == a_pulled && a_sourceName == kSourcePath && FoliageParityOn()) {
				request.defines.push_back({ L"DCLF_FOLIAGE_PARITY", L"1" });
				if (SwitchValue(Switch::FoliageParity) == "land")
					request.defines.push_back({ L"DCLF_FOLIAGE_PARITY_ALL", L"1" });
			}
			// The first few define sets, to reproduce builds with the DXC command line.
			static std::atomic<std::uint32_t> logged = 0;
			if (logged.fetch_add(1) < 4) {
				std::string text;
				for (const auto& define : request.defines)
					text += fmt::format(" -D {}{}{}", Util::WStringToString(define.name), define.value.empty() ? "" : "=", Util::WStringToString(define.value));
				logger::info("[DCLF] SPIR-V build of {} {}{} {:08X} defines:{}", a_sourceName == kSourcePath ? "Lighting" : "Utility",
					a_pixel ? (a_depthOnly ? "PS (depth)" : "PS") : "VS", a_pulled ? " (pulled)" : "", a_descriptor, text);
			}
			const auto shift = [&](const wchar_t* a_flag, std::uint32_t a_value) {
				request.arguments.insert(request.arguments.end(), { a_flag, std::to_wstring(a_value), L"0" });
			};
			shift(L"-fvk-b-shift", kBindingShiftB);
			shift(L"-fvk-t-shift", kBindingShiftT);
			shift(L"-fvk-s-shift", kBindingShiftS);
			shift(L"-fvk-u-shift", kBindingShiftU);
			return RenderGraphRuntime::Get().ShaderCompiler()->CompileAsync(std::move(request));
		}
	}
#endif

	struct ShaderPrograms::StageCache
	{
		std::mutex mutex;
		std::mutex requestMutex;
		bool started = false;
		struct Pending { const RE::BSShader* shader; bool pixel; std::uint32_t descriptor; };
		std::vector<Pending> pending;
#if defined(DCLF_HAS_SHADER_COMPILER)
		ankerl::unordered_dense::map<std::uint64_t, std::shared_future<org::services::ShaderArtifact>> futures;
		// a_created: set when this call made the request (no earlier request, precompile or runtime, had).
		// pulled: a Lighting stage's pulled build (Program::pulledVertex, pulledDepthPixel); a Utility stage's always is.
		std::shared_future<org::services::ShaderArtifact> Request(ShaderPrograms& owner, const RE::BSShader& shader,
			bool pixel, std::uint32_t descriptor, bool depth = false, bool* a_created = nullptr, bool pulled = false)
		{
			const bool utility = shader.shaderType.get() == RE::BSShader::Type::Utility;
			pulled = pulled || utility;
			const std::uint64_t key = descriptor | (std::uint64_t(pixel) << 32) | (std::uint64_t(depth) << 33) | (std::uint64_t(utility) << 34) |
			                          (std::uint64_t(pulled) << 35);
			{
				std::lock_guard lock(mutex);
				if (const auto found = futures.find(key); found != futures.end())
					return found->second;
			}
			// Cache hits must not wait behind another permutation's filesystem key construction.
			std::lock_guard requestLock(requestMutex);
			{
				std::lock_guard lock(mutex);
				if (const auto found = futures.find(key); found != futures.end())
					return found->second;
			}
			if (!owner.Enabled() || !owner.LoadSources() || (utility && owner.utilitySource.empty()) || (pulled && !utility && owner.pulledSource.empty()))
				return {};
			// Like the program entries, stage futures live for this source set's lifetime.
			// A VS shared by several PS permutations must not rescan the shader tree each time.
			auto future = RequestStage(utility ? owner.utilitySource : pulled ? owner.pulledSource : owner.source, owner.dependencies, shader,
				pixel, descriptor, depth, pulled, utility ? kUtilitySourcePath : kSourcePath);
			{
				std::lock_guard lock(mutex);
				futures.emplace(key, future);
			}
			if (a_created)
				*a_created = future.valid();
			return future;
		}
#endif
	};

	ShaderPrograms::ShaderPrograms() : stages(std::make_unique<StageCache>()) {}
	ShaderPrograms::~ShaderPrograms() = default;

	void ShaderPrograms::Precompile(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		const bool enabled = SwitchValue(Switch::Precompile) != "0";
		if (!enabled)
			return;
		const auto type = a_shader.shaderType.get();
		if (type != RE::BSShader::Type::Lighting && type != RE::BSShader::Type::Utility)
			return;
		{
			std::lock_guard lock(stages->mutex);
			if (!stages->started) {
				stages->pending.push_back({ &a_shader, a_pixel, a_descriptor });
				return;
			}
		}
		ZoneScopedN("CS.DCLF.Shaders.Precompile");
		auto stage = stages->Request(*this, a_shader, a_pixel, a_descriptor);
		const bool lighting = type == RE::BSShader::Type::Lighting;
		auto depth = a_pixel && lighting ? stages->Request(*this, a_shader, true, a_descriptor, true) : decltype(stage){};
		// The Z-prepass's pulled builds: the vertex stage, and the depth pixel stage.
		auto pulled = lighting ? stages->Request(*this, a_shader, a_pixel, a_descriptor, a_pixel, nullptr, true) : decltype(stage){};
		// Stay inside CS's bounded compilation workers until this task is done, including cache hits.
		for (const auto* future : { &stage, &depth, &pulled }) {
			if (!future->valid()) continue;
			const auto& artifact = future->get();
			if (!artifact) {
				static std::atomic<std::uint32_t> failures{ 0 };
				if (failures.fetch_add(1) < kMaxLoggedFailures)
					logger::warn("[DCLF] ORG precompile {} {} {:08X} failed: {}", type == RE::BSShader::Type::Lighting ? "Lighting" : "Utility",
						a_pixel ? "PS" : "VS", a_descriptor, artifact.diagnostics.substr(0, 1500));
			}
		}
#endif
	}

	void ShaderPrograms::StartPrecompile()
	{
		std::vector<StageCache::Pending> pending;
		{
			std::lock_guard lock(stages->mutex);
			if (stages->started) return;
			stages->started = true;
			pending.swap(stages->pending);
		}
		for (const auto request : pending)
			SIE::ShaderCache::Instance().compilationPool.detach_task([this, request] {
				try { Precompile(*request.shader, request.pixel, request.descriptor); }
				catch (const std::exception& e) { logger::warn("[DCLF] ORG shader precompile failed: {}", e.what()); }
				SIE::ShaderCache::Instance().NotifyPoolProgress();
			});
	}

	const ShaderPrograms::Program* ShaderPrograms::Find(const PipelineKey& a_key, RE::BSShader& a_lighting, std::uint8_t* a_onDemand)
	{
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled())
			return nullptr;
		const std::uint64_t id = (static_cast<std::uint64_t>(a_key.vertexDescriptor) << 32) | a_key.pixelDescriptor;
		auto [it, inserted] = entries.try_emplace(id);
		if (inserted) {
			it->second = std::make_unique<Entry>();
#if defined(DCLF_HAS_SHADER_COMPILER)
			bool created[5]{};
			it->second->vertex = stages->Request(*this, a_lighting, false, a_key.vertexDescriptor, false, &created[0]);
			it->second->pixel = stages->Request(*this, a_lighting, true, a_key.pixelDescriptor, false, &created[1]);
			it->second->depthPixel = stages->Request(*this, a_lighting, true, a_key.pixelDescriptor, true, &created[2]);
			it->second->pulledVertex = stages->Request(*this, a_lighting, false, a_key.vertexDescriptor, false, &created[3], true);
			it->second->pulledDepthPixel = stages->Request(*this, a_lighting, true, a_key.pixelDescriptor, true, &created[4], true);
			it->second->onDemand = static_cast<std::uint8_t>((created[0] ? kOnDemandVertex : 0) | (created[1] ? kOnDemandPixel : 0) | (created[2] ? kOnDemandDepthPixel : 0) |
															 (created[3] ? kOnDemandPulledVertex : 0) | (created[4] ? kOnDemandPulledDepthPixel : 0));
			it->second->requested = std::chrono::steady_clock::now();
			if (a_onDemand)
				*a_onDemand = it->second->onDemand;
#else
			(void)a_lighting;
			it->second->failed = true;
#endif
			++stats.requested;
		}
		return it->second->program.get();
	}

	const ShaderPrograms::ShadowProgram* ShaderPrograms::FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility, bool a_allowRequest, bool* a_requested,
		std::uint8_t* a_onDemand)
	{
		if (a_requested)
			*a_requested = false;
		if (a_onDemand)
			*a_onDemand = 0;
		if (!Enabled())
			return nullptr;
		auto it = shadowEntries.find(a_technique);
		if (it == shadowEntries.end() && !a_allowRequest)
			return nullptr;
		const bool inserted = it == shadowEntries.end();
		if (inserted) {
			it = shadowEntries.try_emplace(a_technique).first;
			if (a_requested)
				*a_requested = true;
			it->second = std::make_unique<ShadowEntry>();
#if defined(DCLF_HAS_SHADER_COMPILER)
			// Both stages take the same technique: Utility's descriptor is the technique itself, not a
			// pair of vertex and pixel descriptors as the Lighting shader's is.
			bool created[2]{};
			it->second->vertex = stages->Request(*this, a_utility, false, a_technique, false, &created[0]);
			it->second->pixel = stages->Request(*this, a_utility, true, a_technique, false, &created[1]);
			it->second->onDemand = static_cast<std::uint8_t>((created[0] ? kOnDemandVertex : 0) | (created[1] ? kOnDemandPixel : 0));
			it->second->requested = std::chrono::steady_clock::now();
			if (a_onDemand)
				*a_onDemand = it->second->onDemand;
#else
			(void)a_utility;
			it->second->failed = true;
#endif
			++stats.shadowRequested;
		}
		return it->second->program.get();
	}

	void ShaderPrograms::Update()
	{
#if defined(DCLF_HAS_SHADER_COMPILER)
		const auto ready = [](const auto& a_future) {
			return a_future.valid() && a_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		};
		// The outcome of a stage no precompile had asked for (logged with its reason when it was requested): compiled now, or
		// found in the disk cache, which a precompile run before would also have filled.
		const auto onDemandOutcome = [](const char* a_what, std::uint8_t a_stages, std::chrono::steady_clock::time_point a_requested,
										 std::initializer_list<std::pair<std::uint8_t, const org::services::ShaderArtifact*>> a_artifacts) {
			if (!a_stages)
				return;
			std::uint8_t compiled = 0, cached = 0, failed = 0;
			for (const auto& [bit, artifact] : a_artifacts) {
				if (!(a_stages & bit))
					continue;
				(!*artifact ? failed : artifact->fromCache ? cached : compiled) |= bit;
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_requested).count();
			logger::warn("[DCLF] on-demand SPIR-V {} done after {:.0f} ms: compiled now [{}], from the disk cache [{}], failed [{}]", a_what, ms,
				OnDemandStages(compiled), OnDemandStages(cached), OnDemandStages(failed));
		};
		for (auto& [id, entryPointer] : entries) {
			auto& entry = *entryPointer;
			if (entry.program || entry.failed || !ready(entry.vertex) || !ready(entry.pixel) || !ready(entry.depthPixel) || !ready(entry.pulledVertex) ||
				!ready(entry.pulledDepthPixel))
				continue;
			const auto& vertex = entry.vertex.get();
			const auto& pixel = entry.pixel.get();
			const auto& depthPixel = entry.depthPixel.get();
			const auto& pulledVertex = entry.pulledVertex.get();
			const auto& pulledDepthPixel = entry.pulledDepthPixel.get();
			onDemandOutcome(fmt::format("Lighting VS {:08X} PS {:08X}", static_cast<std::uint32_t>(id >> 32), static_cast<std::uint32_t>(id)).c_str(), entry.onDemand,
				entry.requested, { { kOnDemandVertex, &vertex }, { kOnDemandPixel, &pixel }, { kOnDemandDepthPixel, &depthPixel },
									 { kOnDemandPulledVertex, &pulledVertex }, { kOnDemandPulledDepthPixel, &pulledDepthPixel } });
			if (!vertex || !pixel || !depthPixel || !pulledVertex || !pulledDepthPixel) {
				entry.failed = true;
				++stats.failed;
				if (loggedFailures++ < kMaxLoggedFailures) {
					const auto& failed = !vertex ? vertex : !pixel ? pixel : !depthPixel ? depthPixel : !pulledVertex ? pulledVertex : pulledDepthPixel;
					const char* stage = !vertex ? "VS" : !pixel ? "PS" : !depthPixel ? "PS (depth)" : !pulledVertex ? "VS (pulled)" : "PS (pulled depth)";
					std::string diagnostics = failed.diagnostics.substr(0, 1500);
					logger::warn("[DCLF] SPIR-V build of Lighting {} {:08X} failed:\n{}", stage,
						static_cast<std::uint32_t>(!vertex || !pulledVertex ? (id >> 32) : id), diagnostics);
				}
				continue;
			}
			entry.program = std::make_unique<Program>(Program{ vertex.binary, pixel.binary, depthPixel.binary, pulledVertex.binary, pulledDepthPixel.binary });
			++stats.ready;
			stats.fromCache += (vertex.fromCache ? 1 : 0) + (pixel.fromCache ? 1 : 0) + (depthPixel.fromCache ? 1 : 0) + (pulledVertex.fromCache ? 1 : 0) +
			                   (pulledDepthPixel.fromCache ? 1 : 0);
		}
		for (auto& [technique, entryPointer] : shadowEntries) {
			auto& entry = *entryPointer;
			if (entry.program || entry.failed || !ready(entry.vertex) || !ready(entry.pixel))
				continue;
			const auto& vertex = entry.vertex.get();
			const auto& pixel = entry.pixel.get();
			onDemandOutcome(fmt::format("Utility technique {:08X}", technique).c_str(), entry.onDemand, entry.requested,
				{ { kOnDemandVertex, &vertex }, { kOnDemandPixel, &pixel } });
			if (!vertex || !pixel) {
				entry.failed = true;
				++stats.shadowFailed;
				if (loggedFailures++ < kMaxLoggedFailures) {
					const auto& failed = !vertex ? vertex : pixel;
					logger::warn("[DCLF] SPIR-V build of Utility {} {:08X} failed:\n{}", !vertex ? "VS" : "PS", technique, failed.diagnostics.substr(0, 1500));
				}
				continue;
			}
			entry.program = std::make_unique<ShadowProgram>(ShadowProgram{ vertex.binary, pixel.binary });
			++stats.shadowReady;
			stats.fromCache += (vertex.fromCache ? 1 : 0) + (pixel.fromCache ? 1 : 0);
		}
#endif
	}
}
