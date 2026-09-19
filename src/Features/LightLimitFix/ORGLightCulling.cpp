#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGLightCulling.h"

#include "Globals.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
	constexpr const char* kExtensionId = "cs.llf.light-culling";
	constexpr const wchar_t* kBuildShader = L"Data\\Shaders\\LightLimitFix\\ORG\\ClusterBuildingCS.spv";
	constexpr const wchar_t* kCullShader = L"Data\\Shaders\\LightLimitFix\\ORG\\ClusterCullingCS.spv";

	// Push-constant block of both shaders; must match LLFOrgConstants in OrgBindless.hlsli.
	struct alignas(16) OrgClusterConstants
	{
		float cameraMatrix[16];
		float lightsNear;
		float lightsFar;
		uint32_t lightCount;
		uint32_t pad0;
		uint32_t clusterSize[4];
		uint32_t clustersIndex;
		uint32_t lightsIndex;
		uint32_t lightIndexCounterIndex;
		uint32_t lightIndexListIndex;
		uint32_t lightGridIndex;
		uint32_t pad1[3];
	};
	static_assert(sizeof(OrgClusterConstants) == 128);
	constexpr uint32_t kConstantWords = sizeof(OrgClusterConstants) / sizeof(uint32_t);

	// Thread-group sizes of the two shaders (LightLimitFix/Common.hlsli).
	constexpr uint32_t kCullGroupX = 16, kCullGroupY = 16, kCullGroupZ = 4;

	struct Program
	{
		rhi::PipelineLayoutPtr layout;
		rhi::PipelinePtr pipeline;
	};

	std::shared_ptr<const Program> LoadProgram(rhi::Device a_device, const wchar_t* a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		std::vector<char> spirv((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (spirv.empty()) {
			logger::error("[ORG] Missing SPIR-V {}", std::filesystem::path(a_path).string());
			return {};
		}
		auto program = std::make_shared<Program>();
		rhi::PushConstantRangeDesc constants{};
		constants.visibility = rhi::ShaderStage::Compute;
		constants.num32BitValues = kConstantWords;
		constants.set = 0;
		constants.binding = 0;
		if (a_device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None },
				program->layout) != rhi::Result::Ok)
			return {};
		rhi::SubobjLayout layout{ program->layout->GetHandle() };
		rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { spirv.data(), static_cast<uint32_t>(spirv.size()) }, "main" };
		const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
		if (a_device.CreatePipeline(items, 2, program->pipeline) != rhi::Result::Ok)
			return {};
		return program;
	}

	// Camera and cluster parameters of the frame being prepared. Published by the
	// render thread before the epoch; passes read it in Prepare. Immutable once
	// published, so no lock guards it.
	struct Snapshot
	{
		ORGLightCulling::FrameInputs inputs;
	};

	struct Resources
	{
		uint32_t clusterCount = 0;
		std::shared_ptr<org::Buffer> lights;
		std::shared_ptr<org::Buffer> clusters;
		std::shared_ptr<org::Buffer> lightIndexCounter;
		std::shared_ptr<org::Buffer> lightIndexList;
		std::shared_ptr<org::Buffer> lightGrid;
		std::shared_ptr<const Program> build;
		std::shared_ptr<const Program> cull;
		std::atomic<std::shared_ptr<const Snapshot>> snapshot;
	};

	struct DispatchFrame
	{
		std::shared_ptr<const Program> program;
		OrgClusterConstants constants{};
		uint32_t groups[3]{};
	};

	void RecordDispatch(const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
	{
		if (!a_frame.program || !a_frame.groups[0] || !a_frame.groups[1] || !a_frame.groups[2])
			return;
		auto& commands = a_recording.Commands();
		commands.BindLayout(a_frame.program->layout->GetHandle());
		commands.BindPipeline(a_frame.program->pipeline->GetHandle());
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const uint32_t*>(&a_frame.constants));
		commands.Dispatch(a_frame.groups[0], a_frame.groups[1], a_frame.groups[2]);
	}

	void AppendConstantsRevision(const OrgClusterConstants& a_constants, const Program* a_program, std::vector<uint64_t>& a_out)
	{
		a_out.push_back(reinterpret_cast<uintptr_t>(a_program));
		uint32_t words[kConstantWords];
		std::memcpy(words, &a_constants, sizeof(words));
		for (uint32_t i = 0; i < kConstantWords; i += 2)
			a_out.push_back((uint64_t(words[i]) << 32) | words[i + 1]);
	}

	OrgClusterConstants BaseConstants(const ORGLightCulling::FrameInputs& a_inputs, const std::array<float, 16>& a_matrix)
	{
		OrgClusterConstants constants{};
		std::memcpy(constants.cameraMatrix, a_matrix.data(), sizeof(constants.cameraMatrix));
		constants.lightsNear = a_inputs.lightsNear;
		constants.lightsFar = a_inputs.lightsFar;
		constants.lightCount = a_inputs.lightCount;
		constants.clusterSize[0] = a_inputs.clusterSize[0];
		constants.clusterSize[1] = a_inputs.clusterSize[1];
		constants.clusterSize[2] = a_inputs.clusterSize[2];
		return constants;
	}

	// ClusterBuildingCS: one group per cluster; also resets the culling counter.
	struct BuildBindings
	{
		org::ResourceBindingToken clusters, counter;
	};

	class BuildClustersPass final : public org::TypedRenderGraphPass<BuildClustersPass, DispatchFrame, BuildBindings>
	{
	public:
		explicit BuildClustersPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		BuildBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.BindUnorderedAccess(resources->clusters), a_builder.BindUnorderedAccess(resources->lightIndexCounter) };
		}

		// The cluster grid depends only on projection and cluster layout, so the packet
		// is reused while they are unchanged.
		void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>& a_out) const
		{
			const auto snapshot = resources->snapshot.load(std::memory_order_acquire);
			if (!snapshot) {
				a_out.push_back(0);
				return;
			}
			auto constants = BaseConstants(snapshot->inputs, snapshot->inputs.cameraProjInverse);
			constants.lightCount = 0;  // not read by this shader
			AppendConstantsRevision(constants, resources->build.get(), a_out);
		}

		DispatchFrame Prepare(const BuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			DispatchFrame frame{};
			const auto snapshot = resources->snapshot.load(std::memory_order_acquire);
			if (!snapshot)
				return frame;
			frame.program = resources->build;
			frame.constants = BaseConstants(snapshot->inputs, snapshot->inputs.cameraProjInverse);
			frame.constants.lightCount = 0;
			frame.constants.clustersIndex = a_preparation.ResolveView(a_bindings.clusters, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.constants.lightIndexCounterIndex = a_preparation.ResolveView(a_bindings.counter, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.groups[0] = snapshot->inputs.clusterSize[0];
			frame.groups[1] = snapshot->inputs.clusterSize[1];
			frame.groups[2] = snapshot->inputs.clusterSize[2];
			return frame;
		}

		static void Record(const BuildBindings&, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			RecordDispatch(a_frame, a_recording);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	// ClusterCullingCS: per-cluster light lists into lightIndexList / lightGrid.
	struct CullBindings
	{
		org::ResourceBindingToken clusters, lights, counter, lightIndexList, lightGrid;
	};

	class CullLightsPass final : public org::TypedRenderGraphPass<CullLightsPass, DispatchFrame, CullBindings>
	{
	public:
		explicit CullLightsPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		CullBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return {
				a_builder.BindShaderResource(resources->clusters),
				a_builder.BindShaderResource(resources->lights),
				a_builder.BindUnorderedAccess(resources->lightIndexCounter),
				a_builder.BindUnorderedAccess(resources->lightIndexList),
				a_builder.BindUnorderedAccess(resources->lightGrid),
			};
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>& a_out) const
		{
			const auto snapshot = resources->snapshot.load(std::memory_order_acquire);
			if (!snapshot) {
				a_out.push_back(0);
				return;
			}
			AppendConstantsRevision(BaseConstants(snapshot->inputs, snapshot->inputs.cameraView), resources->cull.get(), a_out);
		}

		DispatchFrame Prepare(const CullBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			DispatchFrame frame{};
			const auto snapshot = resources->snapshot.load(std::memory_order_acquire);
			if (!snapshot)
				return frame;
			const auto& inputs = snapshot->inputs;
			frame.program = resources->cull;
			frame.constants = BaseConstants(inputs, inputs.cameraView);
			frame.constants.clustersIndex = a_preparation.ResolveView(a_bindings.clusters, { org::BindlessViewKind::ShaderResource }).index;
			frame.constants.lightsIndex = a_preparation.ResolveView(a_bindings.lights, { org::BindlessViewKind::ShaderResource }).index;
			frame.constants.lightIndexCounterIndex = a_preparation.ResolveView(a_bindings.counter, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.constants.lightIndexListIndex = a_preparation.ResolveView(a_bindings.lightIndexList, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.constants.lightGridIndex = a_preparation.ResolveView(a_bindings.lightGrid, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.groups[0] = (inputs.clusterSize[0] + kCullGroupX - 1) / kCullGroupX;
			frame.groups[1] = (inputs.clusterSize[1] + kCullGroupY - 1) / kCullGroupY;
			frame.groups[2] = (inputs.clusterSize[2] + kCullGroupZ - 1) / kCullGroupZ;
			return frame;
		}

		static void Record(const CullBindings&, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			RecordDispatch(a_frame, a_recording);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	class LightCullingExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit LightCullingExtension(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			a_graph.RegisterResource(org::ResourceIdentifier("cs.llf.lights"), resources->lights);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.llf.clusters"), resources->clusters);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.llf.light-index-counter"), resources->lightIndexCounter);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.llf.light-index-list"), resources->lightIndexList);
			a_graph.RegisterResource(org::ResourceIdentifier("cs.llf.light-grid"), resources->lightGrid);
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.llf.build-clusters",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildClustersPass>(resources)))
					.PreferQueue(org::QueueKind::Graphics));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.llf.cull-lights",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<CullLightsPass>(resources)))
					.PreferQueue(org::QueueKind::Graphics));
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	std::shared_ptr<org::Buffer> CreateStructured(uint32_t a_elements, uint32_t a_stride, bool a_unorderedAccess, const char* a_name)
	{
		auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, a_unorderedAccess);
		buffer->SetName(a_name);
		// Outputs are read by D3D11 through wrappers; every buffer here lives as long
		// as the graph resources, so materialize now rather than at graph setup.
		buffer->Materialize();
		return buffer;
	}

	winrt::com_ptr<ID3D11ShaderResourceView> WrapStructured(org::Buffer& a_buffer, uint32_t a_elements, uint32_t a_stride)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = a_elements * a_stride;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = a_stride;
		auto buffer = RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		if (!buffer)
			return srv;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = a_elements;
		if (FAILED(globals::d3d::device->CreateShaderResourceView(buffer.get(), &srvDesc, srv.put())))
			srv = nullptr;
		return srv;
	}
}

struct ORGLightCulling::Impl
{
	std::shared_ptr<Resources> resources;
	uint32_t maxLights = 0;
	uint32_t lightStride = 0;
	winrt::com_ptr<ID3D11ShaderResourceView> lightIndexListSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> lightGridSRV;
};

ORGLightCulling& ORGLightCulling::Get()
{
	static ORGLightCulling s_culling;
	return s_culling;
}

ORGLightCulling::~ORGLightCulling() = default;

bool ORGLightCulling::Setup(uint32_t a_clusterCount, uint32_t a_maxLights, uint32_t a_lightStride, uint32_t a_maxLightsPerCluster)
{
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host) {
		impl.reset();
		return false;
	}
	if (impl && impl->resources && impl->resources->clusterCount == a_clusterCount)
		return true;

	try {
		auto state = std::make_unique<Impl>();
		state->maxLights = a_maxLights;
		state->lightStride = a_lightStride;
		auto resources = std::make_shared<Resources>();
		auto device = host->GetDesc().device;
		resources->build = LoadProgram(device, kBuildShader);
		resources->cull = LoadProgram(device, kCullShader);
		if (!resources->build || !resources->cull) {
			logger::warn("[ORG] Light culling stays on D3D11: the SPIR-V programs could not be created");
			impl.reset();
			return false;
		}
		const uint32_t indexCount = a_clusterCount * a_maxLightsPerCluster;
		resources->clusterCount = a_clusterCount;
		resources->lights = CreateStructured(a_maxLights, a_lightStride, false, "cs.llf.lights");
		resources->clusters = CreateStructured(a_clusterCount, sizeof(float) * 8, true, "cs.llf.clusters");
		resources->lightIndexCounter = CreateStructured(1, sizeof(uint32_t), true, "cs.llf.light-index-counter");
		resources->lightIndexList = CreateStructured(indexCount, sizeof(uint32_t), true, "cs.llf.light-index-list");
		resources->lightGrid = CreateStructured(a_clusterCount, sizeof(uint32_t) * 4, true, "cs.llf.light-grid");
		state->lightIndexListSRV = WrapStructured(*resources->lightIndexList, indexCount, sizeof(uint32_t));
		state->lightGridSRV = WrapStructured(*resources->lightGrid, a_clusterCount, sizeof(uint32_t) * 4);
		if (!state->lightIndexListSRV || !state->lightGridSRV) {
			logger::warn("[ORG] Light culling stays on D3D11: its outputs could not be shared with D3D11");
			impl.reset();
			return false;
		}
		state->resources = resources;
		// Registering (or replacing) the extension rebuilds the graph on the next epoch;
		// the previous graph and its resources retire first.
		host->AddExtension(kExtensionId, [resources] { return std::make_unique<LightCullingExtension>(resources); });
		impl = std::move(state);
		logger::info("[ORG] Light culling runs on the render graph ({} clusters)", a_clusterCount);
		return true;
	} catch (const std::exception& e) {
		logger::error("[ORG] Light culling setup failed, staying on D3D11: {}", e.what());
		host->RemoveExtension(kExtensionId);
		impl.reset();
		return false;
	}
}

bool ORGLightCulling::Execute(const FrameInputs& a_inputs)
{
	if (!IsActive())
		return false;
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->inputs = a_inputs;
	snapshot->inputs.lightCount = (std::min)(a_inputs.lightCount, impl->maxLights);
	snapshot->inputs.lights = nullptr;  // uploaded below, never read from the snapshot
	impl->resources->snapshot.store(std::move(snapshot), std::memory_order_release);

	const auto resources = impl->resources;
	const size_t bytes = size_t((std::min)(a_inputs.lightCount, impl->maxLights)) * impl->lightStride;
	const bool ok = RenderGraphRuntime::Get().ExecuteEpoch([&](org::RenderGraph&) {
		if (bytes && a_inputs.lights)
			BUFFER_UPLOAD(a_inputs.lights, bytes, org::runtime::UploadTarget::FromShared(resources->lights), 0);
	});
	if (!ok)
		impl.reset();  // graph faulted: LLF returns to its D3D11 dispatches
	return ok;
}

bool ORGLightCulling::IsActive() const
{
	return impl && impl->resources && RenderGraphRuntime::Get().IsActive();
}

ID3D11ShaderResourceView* ORGLightCulling::GetLightIndexListSRV() const
{
	return impl ? impl->lightIndexListSRV.get() : nullptr;
}

ID3D11ShaderResourceView* ORGLightCulling::GetLightGridSRV() const
{
	return impl ? impl->lightGridSRV.get() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Light Limit Fix always culls with D3D11.
#	include "ORGLightCulling.h"

struct ORGLightCulling::Impl
{};

ORGLightCulling& ORGLightCulling::Get()
{
	static ORGLightCulling s_culling;
	return s_culling;
}

ORGLightCulling::~ORGLightCulling() = default;
bool ORGLightCulling::Setup(uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
bool ORGLightCulling::Execute(const FrameInputs&) { return false; }
bool ORGLightCulling::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGLightCulling::GetLightIndexListSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGLightCulling::GetLightGridSRV() const { return nullptr; }
#endif
