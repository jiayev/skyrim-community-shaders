#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGLightCulling.h"

#include "Globals.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <vector>

namespace
{
	constexpr const char* kExtensionId = "cs.llf.light-culling";
	constexpr const char* kBuildShader = "LightLimitFix/ClusterBuildingCS.hlsl";
	constexpr const char* kCullShader = "LightLimitFix/ClusterCullingCS.hlsl";

	// Push-constant block of both shaders: only what is fixed across executions. Must match LLFOrgConstants
	// in OrgBindless.hlsli.
	struct alignas(16) OrgClusterConstants
	{
		uint32_t clusterSize[4];
		uint32_t clustersIndex;
		uint32_t lightsIndex;
		uint32_t lightIndexCounterIndex;
		uint32_t lightIndexListIndex;
		uint32_t lightGridIndex;
		uint32_t latchIndex;
		uint32_t latchOffset;   // set at record time from the frame slot
		uint32_t matrixOffset;  // offsetof(LLFLatch, cameraProjInverse) or offsetof(LLFLatch, cameraView)
	};
	static_assert(sizeof(OrgClusterConstants) == 48);
	constexpr uint32_t kConstantWords = sizeof(OrgClusterConstants) / sizeof(uint32_t);

	// One execution's values, in its slot of the latch block (the host writes them in the epoch, after the
	// slot's previous use completed): what used to be pushed, and the lights, which a copy recorded once
	// moves into the lights buffer D3D11 also reads.
	struct LLFLatch
	{
		float cameraProjInverse[16];
		float cameraView[16];
		float lightsNear;
		float lightsFar;
		uint32_t lightCount;
		uint32_t padding;
	};
	static_assert(sizeof(LLFLatch) == 144);
	constexpr uint32_t kLatchLightsOffset = 256;

	// Thread-group sizes of the two shaders (LightLimitFix/Common.hlsli).
	constexpr uint32_t kCullGroupX = 16, kCullGroupY = 16, kCullGroupZ = 4;

	// What the recorded dispatches depend on: the cluster grid, which follows the resolution, and how many
	// light bytes the latch copy moves (grow-only, in powers of two, so it settles). Published by the render
	// thread before the epoch, and replaced only when it changes: its identity is the passes' revision.
	// Immutable once published, so no lock guards it.
	struct Shape
	{
		uint32_t clusterSize[3]{};
		uint32_t lightBytes = 0;

		bool operator==(const Shape& a_other) const
		{
			return std::memcmp(clusterSize, a_other.clusterSize, sizeof(clusterSize)) == 0 && lightBytes == a_other.lightBytes;
		}
	};

	struct Resources
	{
		uint32_t clusterCount = 0;
		std::shared_ptr<org::Buffer> lights;
		std::shared_ptr<org::Buffer> clusters;
		std::shared_ptr<org::Buffer> lightIndexCounter;
		std::shared_ptr<org::Buffer> lightIndexList;
		std::shared_ptr<org::Buffer> lightGrid;
		std::shared_ptr<const ComputeProgram> build;
		std::shared_ptr<const ComputeProgram> cull;
		std::shared_ptr<org::LatchBlock> latch;
		std::atomic<std::shared_ptr<const Shape>> shape;
	};

	// The shape the passes record against. With epochs, these passes run only in the light culling epoch;
	// without, every epoch runs them and they are empty outside it.
	std::shared_ptr<const Shape> CurrentShape(const Resources& a_resources)
	{
		if (!RenderGraphRuntime::EpochsEnabled() && RenderGraphRuntime::Get().CurrentSegment() != RenderGraphRuntime::Segment::LightCulling)
			return nullptr;
		return a_resources.shape.load(std::memory_order_acquire);
	}

	struct DispatchFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		OrgClusterConstants constants{};
		uint32_t groups[3]{};
		uint32_t latchStride = 0;  // LatchBlock::Offset(slot) = slot * stride
		uint32_t latchSlots = 0;
	};

	void RecordDispatch(const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
	{
		if (!a_frame.program || !a_frame.groups[0] || !a_frame.groups[1] || !a_frame.groups[2])
			return;
		auto& commands = a_recording.Commands();
		commands.BindLayout(a_frame.program->layout->GetHandle());
		commands.BindPipeline(a_frame.program->pipeline->GetHandle());
		auto constants = a_frame.constants;
		constants.latchOffset = (a_recording.FrameSlot() % a_frame.latchSlots) * a_frame.latchStride;
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const uint32_t*>(&constants));
		commands.Dispatch(a_frame.groups[0], a_frame.groups[1], a_frame.groups[2]);
	}

	void AppendShapeRevision(const Shape* a_shape, const ComputeProgram* a_program, std::vector<uint64_t>& a_out)
	{
		a_out.push_back(reinterpret_cast<uintptr_t>(a_shape));
		a_out.push_back(reinterpret_cast<uintptr_t>(a_program));
	}

	OrgClusterConstants BaseConstants(const Resources& a_resources, const Shape& a_shape, uint32_t a_matrixOffset)
	{
		OrgClusterConstants constants{};
		constants.clusterSize[0] = a_shape.clusterSize[0];
		constants.clusterSize[1] = a_shape.clusterSize[1];
		constants.clusterSize[2] = a_shape.clusterSize[2];
		constants.latchIndex = a_resources.latch->SrvIndex();
		constants.matrixOffset = a_matrixOffset;
		return constants;
	}

	// Moves the lights from the execution's latch region into the lights buffer (read by the culling, by
	// DCLF's draws and by D3D11)
	struct LatchCopyBindings
	{
		org::ResourceBindingToken lights;
	};

	struct LatchCopyFrame
	{
		std::shared_ptr<const org::LatchBlock> latch;
		uint32_t bytes = 0;
	};

	class LatchLightsPass final : public org::TypedRenderGraphPass<LatchLightsPass, LatchCopyFrame, LatchCopyBindings>
	{
	public:
		explicit LatchLightsPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		LatchCopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.BindCopyDestination(resources->lights) };
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<uintptr_t>(CurrentShape(*resources).get()));
		}

		LatchCopyFrame Prepare(const LatchCopyBindings&, const org::PassPrepareContext&) const
		{
			const auto shape = CurrentShape(*resources);
			if (!shape || !shape->lightBytes)
				return {};
			return { resources->latch, shape->lightBytes };
		}

		static void Record(const LatchCopyBindings& a_bindings, const LatchCopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.latch || !a_frame.bytes)
				return;
			a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.lights).GetHandle(), 0,
				a_frame.latch->Resource()->GetAPIResource().GetHandle(), a_frame.latch->Offset(a_recording.FrameSlot()) + kLatchLightsOffset, a_frame.bytes);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

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

		// The recorded dispatch depends only on the cluster grid; the projection is latched.
		void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>& a_out) const
		{
			AppendShapeRevision(CurrentShape(*resources).get(), resources->build.get(), a_out);
		}

		DispatchFrame Prepare(const BuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			DispatchFrame frame{};
			const auto shape = CurrentShape(*resources);
			if (!shape)
				return frame;
			frame.program = resources->build;
			frame.constants = BaseConstants(*resources, *shape, offsetof(LLFLatch, cameraProjInverse));
			frame.constants.clustersIndex = a_preparation.ResolveView(a_bindings.clusters, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.constants.lightIndexCounterIndex = a_preparation.ResolveView(a_bindings.counter, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.groups[0] = shape->clusterSize[0];
			frame.groups[1] = shape->clusterSize[1];
			frame.groups[2] = shape->clusterSize[2];
			frame.latchStride = resources->latch->Stride();
			frame.latchSlots = resources->latch->Slots();
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
			AppendShapeRevision(CurrentShape(*resources).get(), resources->cull.get(), a_out);
		}

		DispatchFrame Prepare(const CullBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			DispatchFrame frame{};
			const auto shape = CurrentShape(*resources);
			if (!shape)
				return frame;
			frame.program = resources->cull;
			frame.constants = BaseConstants(*resources, *shape, offsetof(LLFLatch, cameraView));
			frame.latchStride = resources->latch->Stride();
			frame.latchSlots = resources->latch->Slots();
			frame.constants.clustersIndex = a_preparation.ResolveView(a_bindings.clusters, { org::BindlessViewKind::ShaderResource }).index;
			frame.constants.lightsIndex = a_preparation.ResolveView(a_bindings.lights, { org::BindlessViewKind::ShaderResource }).index;
			frame.constants.lightIndexCounterIndex = a_preparation.ResolveView(a_bindings.counter, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.constants.lightIndexListIndex = a_preparation.ResolveView(a_bindings.lightIndexList, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.constants.lightGridIndex = a_preparation.ResolveView(a_bindings.lightGrid, { org::BindlessViewKind::UnorderedAccess }).index;
			frame.groups[0] = (shape->clusterSize[0] + kCullGroupX - 1) / kCullGroupX;
			frame.groups[1] = (shape->clusterSize[1] + kCullGroupY - 1) / kCullGroupY;
			frame.groups[2] = (shape->clusterSize[2] + kCullGroupZ - 1) / kCullGroupZ;
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
			const auto epoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::LightCulling);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.llf.latch-lights",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<LatchLightsPass>(resources)))
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.llf.build-clusters",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildClustersPass>(resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.llf.cull-lights",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<CullLightsPass>(resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
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
		// The descriptor-heap build of the D3D11 shaders (LLF_ORG_BINDLESS).
		const auto load = [&](const char* a_source) {
			return ComputeProgram::Load(device, { .source = a_source, .constantWords = kConstantWords, .defines = { { L"LLF_ORG_BINDLESS", L"1" } } });
		};
		resources->build = load(kBuildShader);
		resources->cull = load(kCullShader);
		if (!resources->build || !resources->cull) {
			logger::warn("[ORG] Light culling stays on D3D11: the SPIR-V programs could not be created");
			impl.reset();
			return false;
		}
		const uint32_t indexCount = a_clusterCount * a_maxLightsPerCluster;
		resources->clusterCount = a_clusterCount;
		resources->lights = CreateStructured(a_maxLights, a_lightStride, false, "cs.llf.lights");
		resources->latch = std::make_shared<org::LatchBlock>("cs.llf.latch", kLatchLightsOffset + a_maxLights * a_lightStride, host->FrameSlots());
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
	RenderGraphRuntime::EpochBodyScope body(RenderGraphRuntime::Segment::LightCulling);
	const auto resources = impl->resources;
	const uint32_t lightCount = a_inputs.lights ? (std::min)(a_inputs.lightCount, impl->maxLights) : 0u;
	const uint32_t bytes = lightCount * impl->lightStride;
	{
		// A new shape only when the grid changes or the lights outgrow the copy (grow-only, powers of two).
		const auto current = resources->shape.load(std::memory_order_acquire);
		Shape shape{};
		std::copy(a_inputs.clusterSize, a_inputs.clusterSize + 3, shape.clusterSize);
		const uint32_t capacity = impl->maxLights * impl->lightStride;
		shape.lightBytes = current ? current->lightBytes : 0u;
		if (bytes > shape.lightBytes)
			shape.lightBytes = (std::min)(capacity, (std::max)(4096u, std::bit_ceil(bytes)));
		if (!current || !(*current == shape))
			resources->shape.store(std::make_shared<const Shape>(shape), std::memory_order_release);
	}
	const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::LightCulling, [&](org::RenderGraph&) {
		// The slot this execution records against is free from here (the host waited for it).
		const uint32_t slot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		LLFLatch latch{};
		std::memcpy(latch.cameraProjInverse, a_inputs.cameraProjInverse.data(), sizeof(latch.cameraProjInverse));
		std::memcpy(latch.cameraView, a_inputs.cameraView.data(), sizeof(latch.cameraView));
		latch.lightsNear = a_inputs.lightsNear;
		latch.lightsFar = a_inputs.lightsFar;
		latch.lightCount = lightCount;
		resources->latch->WriteValue(slot, 0, latch);
		if (bytes)
			resources->latch->Write(slot, kLatchLightsOffset, { reinterpret_cast<const std::byte*>(a_inputs.lights), bytes });
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

bool ORGLightCulling::GetShaderResourceIndices(uint32_t& a_lights, uint32_t& a_lightIndexList, uint32_t& a_lightGrid) const
{
	if (!IsActive())
		return false;
	const auto& resources = *impl->resources;
	a_lights = resources.lights->GetSRVInfo(0).slot.index;
	a_lightIndexList = resources.lightIndexList->GetSRVInfo(0).slot.index;
	a_lightGrid = resources.lightGrid->GetSRVInfo(0).slot.index;
	return true;
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
bool ORGLightCulling::GetShaderResourceIndices(uint32_t&, uint32_t&, uint32_t&) const { return false; }
#endif
