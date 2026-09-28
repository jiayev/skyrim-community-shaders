#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	// The graph resources: the main pass's (Setup) and the shadow views' (SetupShadow, ImportShadowDepth).

	bool IndirectDraws::Impl::Setup(const Capture& a_capture, bool a_depthOnly)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return NotReady(0, "no render graph");
		if (a_depthOnly) {
			// The native depth pass binds no colour targets, so there is nothing to build from here. The
			// Z-prepass reuses what the main pass built last frame; on the first frame there is none yet
			// and the prepass simply does not run.
			return resources != nullptr;
		}
		if (a_capture.targetCount == 0)
			return NotReady(1, "no render targets bound");
		D3D11_TEXTURE2D_DESC target{};
		a_capture.targets[0]->GetDesc(&target);
		const auto& targets = DrawPipelines::Get().Targets();
		std::uint32_t lights, list, grid;
		const bool lightLimitFix = ORGLightCulling::Get().GetShaderResourceIndices(lights, list, grid);
		auto frameBuffers = FrameBuffersOf(a_capture, lightLimitFix);
		const bool sameFrameBuffers = resources && resources->frameBuffers.size() == frameBuffers.size() &&
		                              std::equal(frameBuffers.begin(), frameBuffers.end(), resources->frameBuffers.begin(), [](const auto& a, const auto& b) { return a.SameShape(b); });
		if (resources && targets == formats && target.Width == width && target.Height == height && sameFrameBuffers &&
			resources->lightLimitFix == lightLimitFix)
			return true;

		auto device = host->GetDesc().device;
		auto state = std::make_shared<Resources>();
		auto buffer = [&](std::uint64_t a_bytes, const char* a_name) {
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		};
		state->constants = buffer(kConstantBytes, "cs.dclf.constants");
		state->recordCapacity = kMaxRecordsDeduplicated;
		state->records = buffer(std::uint64_t(state->recordCapacity) * sizeof(DrawBindings), "cs.dclf.records");
		state->constantsDepth = buffer(kDepthConstantBytes, "cs.dclf.constants-depth");
		state->recordsDepth = buffer(std::uint64_t(state->recordCapacity) * sizeof(DrawBindings), "cs.dclf.records-depth");
		// Structured rather than raw, because the shaders read it through an SRV at t127 instead of
		// through a device address the way the constants and the binding records are read.
		state->objects = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxObjects, sizeof(BindlessObject), false);
		state->objects->SetName("cs.dclf.objects");
		state->objects->Materialize();
		state->objectsIndex = state->objects->GetSRVInfo(0).slot.index;
		state->bones = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxBoneRows, 16, false);
		state->bones->SetName("cs.dclf.bones");
		state->bones->Materialize();
		state->bonesIndex = state->bones->GetSRVInfo(0).slot.index;
		state->facePositions = buffer(std::uint64_t(kFacePositionVertices) * 16, "cs.dclf.face-positions");
		state->facePositionsAddress = device.GetBufferDeviceAddress({ state->facePositions->GetAPIResource().GetHandle(), 0 });
		// Twice kMaxDraws: phase 1 and the colour segment write the first half, phase 2 the second. The
		// CPU records where phase 2's draw starts, so the two need ranges fixed in advance rather than
		// one range shared through an atomic counter.
		state->sequences = CreateWords(std::uint64_t(kSequenceSlots) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences");
		state->count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
		// One word per object in the frame's tables: what the depth segment's culling decided, read by
		// the colour segment so that it draws exactly the same set.
		state->visibility = CreateWords(kMaxObjects, true, "cs.dclf.visibility");
		state->frustum = CreateWords(kMaxObjects, true, "cs.dclf.frustum");
		{
			// The feedback ring: at least as many slots as frames in flight, and at least 4.
			auto feedback = std::make_shared<Resources::Feedback>();
			auto timeline = std::make_shared<rhi::TimelinePtr>();
			if (rhi::Failed(device.CreateTimeline(*timeline, 0, "cs.dclf.feedback")) || !*timeline) {
				logger::warn("[DCLF] visibility feedback: no timeline; the feedback is off");
			} else {
				feedback->timeline = std::move(timeline);
				const std::uint32_t slots = std::max<std::uint32_t>(host->FrameSlots(), 4u);
				for (std::uint32_t i = 0; i < slots; ++i) {
					auto slot = std::make_unique<Resources::Feedback::Slot>();
					slot->staging = org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(kMaxObjects) * sizeof(std::uint32_t));
					slot->staging->SetName(fmt::format("cs.dclf.feedback{}", i).c_str());
					feedback->slots.push_back(std::move(slot));
				}
				state->feedback = std::move(feedback);
			}
		}
		if (PassStats::Enabled()) {
			auto passStats = std::make_shared<PassStats>();
			const std::uint32_t slots = host->FrameSlots();
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::PipelineStatistics;
			desc.count = slots * PassStats::kKinds;
			desc.statsMask = rhi::PipelineStatBits::PS_IAVertices | rhi::PipelineStatBits::PS_IAPrimitives | rhi::PipelineStatBits::PS_VSInvocations |
			                 rhi::PipelineStatBits::PS_PSInvocations;
			desc.requireAllStats = true;
			if (device.CreateQueryPool(desc, passStats->pool) != rhi::Result::Ok || !passStats->pool) {
				logger::warn("[DCLF] pass statistics: no pipeline statistics query pool");
			} else {
				for (std::uint32_t i = 0; i < slots; ++i)
					passStats->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(PassStats::kKinds) * PassStats::kFields * sizeof(std::uint64_t)));
				passStats->written.resize(slots);
				state->passStats = std::move(passStats);
				logger::info("[DCLF] pass statistics on: {} frame slots", slots);
			}
		}
		state->preprocessMain = PreprocessStates::Create(device, host->FrameSlots(), "the main segment");
		state->inputs = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs");
		state->inputsDepth = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs-depth");
		state->geometries = CreateWords(std::uint64_t(kMaxGeometries) * sizeof(GeometryDraw) / 4, false, "cs.dclf.geometries");
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (!state->buildDraws)
			return NotReady(7, "the BuildDraws program could not be created");
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature)
			return NotReady(7, "the BuildDraws dispatch signature could not be created");
		state->latch = std::make_shared<org::LatchBlock>("cs.dclf.latch", static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), host->FrameSlots());
		// The phase-1 and colour draws executed grouped by pipeline: BuildDraws appends in whatever order its threads
		// finish, which made nearly every sequence of the indirect draw switch pipeline.
		state->sort = DrawSort::Create(device);
		if (BuildParityEnabled()) {
			auto wrap = [](org::Buffer& a_buffer, std::uint64_t a_bytes) {
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = static_cast<UINT>(a_bytes);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				return RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
			};
			state->sequencesD3D11 = wrap(*state->sequences, std::uint64_t(kSequenceSlots) * sizeof(DrawSequence));

		}
		if (!SwitchValue(Switch::GBufferProbe).empty()) {
			state->probe = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, std::uint64_t(kProbeSlots) * kProbeSlotBytes, false);
			state->probe->SetName("cs.dclf.gbuffer-probe");
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kProbeSlots * kProbeSlotBytes;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			state->probeD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->probe, desc);
		}
		{
			// The draw-count buffer is read back for the culling counters whether or not parity is on. The
			// width must cover every counter word: a view that stops short reads zeros for the rest, and
			// a counter that is always zero reads exactly like a clean result.
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kCountWords * sizeof(std::uint32_t);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			state->countD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->count, desc);
		}
		if (SetParityEnabled()) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kMaxObjects * sizeof(std::uint32_t);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			state->visibilityD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->visibility, desc);
		}
		state->frameConstants = buffer(kFrameConstantBytes, "cs.dclf.frame-constants");
		state->constantsAddress = device.GetBufferDeviceAddress({ state->constants->GetAPIResource().GetHandle(), 0 });
		state->recordsAddress = device.GetBufferDeviceAddress({ state->records->GetAPIResource().GetHandle(), 0 });
		state->constantsDepthAddress = device.GetBufferDeviceAddress({ state->constantsDepth->GetAPIResource().GetHandle(), 0 });
		state->recordsDepthAddress = device.GetBufferDeviceAddress({ state->recordsDepth->GetAPIResource().GetHandle(), 0 });
		state->frameConstantsAddress = device.GetBufferDeviceAddress({ state->frameConstants->GetAPIResource().GetHandle(), 0 });

		if (!state->constantsAddress || !state->recordsAddress || !state->frameConstantsAddress) {
			logger::error("[DCLF] Draw data buffers have no device address");
			return false;
		}

		state->width = target.Width;
		state->height = target.Height;

		// The main pass's targets and depth, imported.
		state->targetCount = targets.colorCount;
		for (std::uint32_t i = 0; i < targets.colorCount; ++i) {
			DxvkOrgInteropResourceInfo info{};
			if (!a_capture.targets[i] || !RenderGraphRuntime::Get().DescribeResource(a_capture.targets[i].get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(5, "a main-pass target cannot be described");
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(targets.colors[i]);
			desc.channels = 4;
			desc.hasRTV = true;
			desc.rtvFormat = desc.format;
			state->native[i] = ImportImage(device, info.image, desc, "DCLF native target");
			if (!state->native[i])
				return NotReady(6, "a main-pass target could not be imported");
		}
		{
			DxvkOrgInteropResourceInfo info{};
			if (!a_capture.depth || !RenderGraphRuntime::Get().DescribeResource(a_capture.depth.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(10, "the main-pass depth cannot be described");
			// Attachable and readable, from one import. The HZB build reads the same image the draws
			// write, and importing it twice would give the graph two resources it believes are unrelated:
			// it would order nothing between the depth draws and the read, and insert no barrier, so the
			// build would reduce whatever happened to be there - in practice the cleared far plane.
			org::TextureDescription depthDesc{};
			depthDesc.imageDimensions.push_back({ target.Width, target.Height, 0, 0 });
			depthDesc.format = rhi::helpers::ToRHI(targets.depth);
			depthDesc.channels = 1;
			depthDesc.hasDSV = true;
			depthDesc.dsvFormat = depthDesc.format;
			depthDesc.hasSRV = true;
			depthDesc.srvFormat = DepthReadFormat(depthDesc.format);
			state->nativeDepth = ImportImage(device, info.image, depthDesc, "DCLF native depth");
			if (!state->nativeDepth)
				return NotReady(11, "the main-pass depth could not be imported");
		}

		// The hierarchical depth buffer.
		//
		// Mip 0 is half the next power of two of the depth, so the chain is a clean sequence of halvings
		// and a mip level can be chosen from a screen-space extent by log2 alone. Padding to a power of
		// two is what makes that true; the padded texels are outside the real depth and the build fills
		// them with the far plane, which suppresses culling rather than causing it.
		auto nextPowerOfTwo = [](std::uint32_t a_value) {
			std::uint32_t result = 1;
			while (result < a_value)
				result <<= 1;
			return result;
		};
		state->hzbWidth = std::max(1u, nextPowerOfTwo(state->width) / 2);
		state->hzbHeight = std::max(1u, nextPowerOfTwo(state->height) / 2);
		state->hzbMips = 1;
		for (std::uint32_t size = std::max(state->hzbWidth, state->hzbHeight); size > 1; size >>= 1)
			++state->hzbMips;

		org::TextureDescription hzbDesc{};
		for (std::uint32_t mip = 0; mip < state->hzbMips; ++mip)
			hzbDesc.imageDimensions.push_back({ std::max(1u, state->hzbWidth >> mip), std::max(1u, state->hzbHeight >> mip), 0, 0 });
		hzbDesc.format = rhi::Format::R32_Float;
		hzbDesc.channels = 1;
		hzbDesc.hasUAV = true;
		hzbDesc.uavFormat = hzbDesc.format;
		hzbDesc.hasSRV = true;
		hzbDesc.srvFormat = hzbDesc.format;
		state->hzb = org::PixelBuffer::CreateSharedUnmaterialized(hzbDesc);
		state->hzb->SetName("cs.dclf.hzb");

		state->hzbProgram = ComputeProgram::Load(device, { .source = kHzbShader, .constantWords = kHzbConstantWords });
		if (!state->hzbProgram) {
			// The culling falls back to frustum only; the frame is unaffected.
			logger::warn("[DCLF] The HZB compute program could not be created; occlusion culling stays off");
			state->hzb.reset();
		}

		state->lightLimitFix = lightLimitFix;
		for (auto& frameBuffer : frameBuffers) {
			frameBuffer.copy = org::Buffer::CreateUnmaterializedStructuredBuffer(frameBuffer.elements, frameBuffer.stride, false);
			frameBuffer.copy->SetName(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister));
			frameBuffer.copy->Materialize();
		}
		state->frameBuffers = std::move(frameBuffers);

		resources = state;
		formats = targets;
		width = target.Width;
		height = target.Height;
		// The graph is rebuilt with the new resources on its next epoch.
		host->AddExtension(kExtensionId, [state] { return MakeMainOpaqueExtension(state); });
		logger::info("[DCLF] Main-pass graph resources: {} targets {}x{}, depth format {}", targets.colorCount, width, height, static_cast<int>(targets.depth));
		return true;
	}

	bool IndirectDraws::Impl::SetupShadow()
	{
		if (shadow)
			return true;
		if (shadowSetupFailed)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return ShadowNotReady(0, "no render graph");
		auto device = host->GetDesc().device;
		auto state = std::make_shared<ShadowResources>();
		auto buffer = [&](std::uint64_t a_bytes, const char* a_name) {
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		};
		state->constants = buffer(kShadowConstantBytes, "cs.dclf.shadow.constants");
		state->records = buffer(std::uint64_t(kMaxShadowViews) * kShadowRecordCapacity * sizeof(DrawBindings), "cs.dclf.shadow.records");
		state->objects = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxObjects, sizeof(BindlessObject), false);
		state->objects->SetName("cs.dclf.shadow.objects");
		state->objects->Materialize();
		state->objectsIndex = state->objects->GetSRVInfo(0).slot.index;
		state->bones = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxBoneRows, 16, false);
		state->bones->SetName("cs.dclf.shadow.bones");
		state->bones->Materialize();
		state->bonesIndex = state->bones->GetSRVInfo(0).slot.index;
		state->facePositions = buffer(std::uint64_t(kFacePositionVertices) * 16, "cs.dclf.shadow.face-positions");
		for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
			state->sequences[s] = CreateWords(std::uint64_t(kMaxDraws) * sizeof(DrawSequence) / 4, true, fmt::format("cs.dclf.shadow.sequences{}", s).c_str());
			state->count[s] = CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str());
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kCountWords * sizeof(std::uint32_t);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			state->countD3D11[s] = RenderGraphRuntime::Get().WrapBuffer(*state->count[s], desc);
		}
		state->visibility = CreateWords(kMaxObjects, true, "cs.dclf.shadow.visibility");
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			state->inputs[m] = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, fmt::format("cs.dclf.shadow.draw-inputs{}", m).c_str());
		state->geometries = CreateWords(std::uint64_t(kMaxGeometries) * sizeof(GeometryDraw) / 4, false, "cs.dclf.shadow.geometries");
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (!state->buildDraws) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws compute program could not be created");
		}
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws dispatch signature could not be created");
		}
		state->latch = std::make_shared<org::LatchBlock>("cs.dclf.shadow.latch", kShadowLatchBytes, host->FrameSlots());
		state->preprocessShadow = PreprocessStates::Create(device, host->FrameSlots(), "the shadow views");
		state->preprocessSky = PreprocessStates::Create(device, host->FrameSlots(), "Skylighting's occlusion map");
		state->facePositionsAddress = device.GetBufferDeviceAddress({ state->facePositions->GetAPIResource().GetHandle(), 0 });
		state->constantsAddress = device.GetBufferDeviceAddress({ state->constants->GetAPIResource().GetHandle(), 0 });
		state->recordsAddress = device.GetBufferDeviceAddress({ state->records->GetAPIResource().GetHandle(), 0 });
		if (!state->constantsAddress || !state->recordsAddress) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow buffers");
		}
		shadow = state;
		host->AddExtension(kShadowExtensionId, [state] { return MakeShadowExtension(state); });
		logger::info("[DCLF] shadow view graph resources created");
		return true;
	}

	bool IndirectDraws::Impl::ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!renderer || !host || !shadow || a_index >= kShadowDepthTargets)
			return false;
		const auto& data = renderer->GetDepthStencilData().depthStencils[a_target];
		if (!data.texture || !data.views[0])
			return ShadowNotReady(3, "the shadow map has no texture");
		if (shadow->depth[a_index] && shadow->depthTexture[a_index] == data.texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(data.texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return ShadowNotReady(4, "the shadow map cannot be described");
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		data.views[0]->GetDesc(&dsvDesc);
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
		desc.channels = 1;
		desc.hasDSV = true;
		desc.dsvFormat = desc.format;
		static constexpr const char* kNames[kShadowDepthTargets] = { "DCLF shadow maps (ESRAM)", "DCLF shadow maps", "DCLF volumetric shadow maps (ESRAM)",
			"DCLF Skylighting occlusion map" };
		auto imported = ImportImage(host->GetDesc().device, info.image, desc, kNames[a_index]);
		if (!imported)
			return ShadowNotReady(5, "the shadow map could not be imported (not in the general layout?)");
		shadow->depth[a_index] = std::move(imported);
		shadow->depthTexture[a_index] = data.texture;
		shadow->depthLayers[a_index] = info.image.arrayLayers;
		// A new resource for the passes to bind: the graph is rebuilt on the next epoch.
		host->AddExtension(kShadowExtensionId, [state = shadow] { return MakeShadowExtension(state); });
		logger::info("[DCLF] shadow map {} imported: {}x{}, {} slices, format {}", a_target, info.image.extent.width, info.image.extent.height,
			info.image.arrayLayers, static_cast<int>(dsvDesc.Format));
		return true;
	}
}

#endif
