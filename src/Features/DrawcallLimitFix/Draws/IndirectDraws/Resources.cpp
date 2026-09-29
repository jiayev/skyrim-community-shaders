#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief A device-local buffer the shaders read by device address. */
		std::shared_ptr<org::Buffer> DeviceBuffer(std::uint64_t a_bytes, const char* a_name)
		{
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		}

		/** @brief A structured buffer the shaders read through its SRV; a_index is the SRV's descriptor slot. */
		std::shared_ptr<org::Buffer> StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, std::uint32_t& a_index)
		{
			auto created = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, false);
			created->SetName(a_name);
			created->Materialize();
			a_index = created->GetSRVInfo(0).slot.index;
			return created;
		}

		/**
		 * @brief A D3D11 view of the first a_bytes of a buffer of 32-bit words, for the readbacks through the immediate
		 * context. It must cover every word read: a view that stops short reads zeros for the rest.
		 */
		auto WrapWords(org::Buffer& a_buffer, std::uint64_t a_bytes)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(a_bytes);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			return RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
		}

		std::uint64_t AddressOf(rhi::Device a_device, org::Buffer& a_buffer)
		{
			return a_device.GetBufferDeviceAddress({ a_buffer.GetAPIResource().GetHandle(), 0 });
		}
	}
}

namespace DCLF::Draws
{
	bool GrowableRows::Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		stride = a_stride;
		capacity = std::max(a_rows, 1u);
		name = a_name;
		buffer = DeviceBuffer(std::uint64_t(capacity) * stride, a_name);
		address = AddressOf(host->GetDesc().device, *buffer);
		++generation;
		return address != 0;
	}

	bool GrowableRows::Reserve(std::uint32_t a_rows)
	{
		if (!buffer || a_rows <= capacity)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		// Doubling, so that a table filling up grows a handful of times, not every frame.
		std::uint32_t rows = capacity;
		while (rows < a_rows)
			rows *= 2;
		// A new backing for the same graph resource: the old one is released through ORG's deletion queue, frames in flight
		// after the GPU last used it.
		buffer->ResizeBytes(std::uint64_t(rows) * stride);
		buffer->SetName(name.c_str());
		address = AddressOf(host->GetDesc().device, *buffer);
		logger::info("[DCLF] {}: {} rows grown to {} ({} KB)", name, capacity, rows, std::uint64_t(rows) * stride / 1024);
		capacity = rows;
		++generation;
		++growths;
		return true;
	}

	namespace
	{
		/** @brief The largest max count one indirect draw may have on this device (IndirectCommandsFeatureInfo::maxSequenceCount). */
		std::uint32_t DeviceMaxSequences()
		{
			static const std::uint32_t max = [] {
				::IndirectCommandsFeatureInfo indirect{};
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host || host->GetDesc().device.QueryFeatureInfo(&indirect.header) != rhi::Result::Ok)
					return 0u;
				logger::info("[DCLF] indirect draws: up to {} sequences per call", indirect.maxSequenceCount);
				return indirect.maxSequenceCount;
			}();
			return max;
		}

		/**
		 * @brief Every draw the scene's tracked objects can produce: one per object, or one per partition a skin draws. A view's
		 * draws are a part of it, whatever the culling keeps, so a sequence buffer that holds it holds any epoch's.
		 */
		std::uint32_t SceneDrawBound(const SceneStore::Tables& a_tables)
		{
			std::uint64_t draws = 0;
			const std::size_t objects = a_tables.objects.size();
			const bool partitioned = a_tables.skinPartitions.size() >= objects;
			for (std::size_t o = 0; o < objects; ++o) {
				if (a_tables.objects[o].flags & kObjectFree)
					continue;
				const std::uint32_t partitions = partitioned ? a_tables.skinPartitions[o] : 0u;
				draws += partitions ? std::popcount(partitions) : 1;
			}
			return static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX));
		}

		/** @brief Room for a_needed, doubling from a_current. */
		std::uint32_t Doubled(std::uint32_t a_current, std::uint32_t a_needed)
		{
			std::uint64_t capacity = std::max(a_current, 1u);
			while (capacity < a_needed)
				capacity *= 2;
			return static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity, UINT32_MAX));
		}

		/**
		 * @brief A draw range the device cannot execute in one call, or the sort cannot rank: a hard failure, until the draws
		 * are split over several calls (2D indexing).
		 */
		[[noreturn]] void SequenceLimitFailure(const char* a_what, std::uint32_t a_draws, std::uint32_t a_limit)
		{
			const auto message = fmt::format("Drawcall Limit Fix: {} needs {} draws in one indirect draw, over the limit of {}", a_what, a_draws, a_limit);
			logger::critical("[DCLF] {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
		}

		void CheckSequenceLimits(const char* a_what, std::uint32_t a_draws, bool a_sorted)
		{
			const std::uint32_t device = DeviceMaxSequences();
			if (device && a_draws > device)
				SequenceLimitFailure(a_what, a_draws, device);
			if (a_sorted && a_draws > kSortRankLimit)
				SequenceLimitFailure(a_what, a_draws, kSortRankLimit);
		}
	}
}

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
		state->constants = DeviceBuffer(kConstantBytes, "cs.dclf.constants");
		// The rows both segments' draws name (MainRows): grown before each epoch to the scene's material and pipeline slots.
		const bool smallRows = SwitchValue(Switch::TableStart) == "small";
		if (!state->materialRows.Create(kMaterialRowBytes, smallRows ? 4u : 1024u, "cs.dclf.material-rows") ||
			!state->pipelineRows.Create(kPipelineRowBytes, smallRows ? 4u : 256u, "cs.dclf.pipeline-rows")) {
			logger::error("[DCLF] The main rows have no device address");
			return false;
		}
		// Structured rather than raw, because the shaders read it through an SRV at t127 instead of
		// through a device address the way the constants and the binding records are read.
		state->objects = StructuredBuffer(kMaxObjects, sizeof(BindlessObject), "cs.dclf.objects", state->objectsIndex);
		state->bones = StructuredBuffer(kMaxBoneRows, 16, "cs.dclf.bones", state->bonesIndex);
		state->facePositions = DeviceBuffer(std::uint64_t(kFacePositionVertices) * 16, "cs.dclf.face-positions");
		state->facePositionsAddress = AddressOf(device, *state->facePositions);
		// The draws' range, phase 2's and the decal groups' (GpuLayouts.h, SequenceSlots), grown before each epoch to hold every
		// draw the scene can produce (ReserveMainSequences).
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		state->sequenceDraws = smallTables ? 64u : kInitialSequenceDraws;
		state->sequenceDecals = smallTables ? 16u : kInitialDecalDraws;
		state->sequences = CreateWords(SequenceSlots(state->sequenceDraws, state->sequenceDecals) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences");
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
		state->sort = DrawSort::Create(device, state->sequenceDraws);
		if (BuildParityEnabled())
			state->sequencesD3D11 = WrapWords(*state->sequences, SequenceSlots(state->sequenceDraws, state->sequenceDecals) * sizeof(DrawSequence));
		if (!SwitchValue(Switch::GBufferProbe).empty()) {
			state->probe = DeviceBuffer(std::uint64_t(kProbeSlots) * kProbeSlotBytes, "cs.dclf.gbuffer-probe");
			state->probeD3D11 = WrapWords(*state->probe, std::uint64_t(kProbeSlots) * kProbeSlotBytes);
		}
		// The draw-count buffer is read back for the culling counters whether or not parity is on: every counter word, as a
		// counter that is always zero reads exactly like a clean result.
		state->countD3D11 = WrapWords(*state->count, kCountWords * sizeof(std::uint32_t));
		if (SetParityEnabled())
			state->visibilityD3D11 = WrapWords(*state->visibility, kMaxObjects * sizeof(std::uint32_t));
		state->frameConstants = DeviceBuffer(kFrameConstantBytes, "cs.dclf.frame-constants");
		state->constantsAddress = AddressOf(device, *state->constants);
		state->frameConstantsAddress = AddressOf(device, *state->frameConstants);

		if (!state->constantsAddress || !state->frameConstantsAddress) {
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

	void IndirectDraws::Impl::ReserveMainSequences(const SceneStore::Tables& a_tables)
	{
		if (!resources)
			return;
		auto& r = *resources;
		const std::uint32_t draws = SceneDrawBound(a_tables);
		std::uint32_t decals = 0;
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			decals = std::max(decals, a_tables.decalCount[group]);
		CheckSequenceLimits("the main pass", draws, true);
		CheckSequenceLimits("a decal group", decals, false);
		// The rows: one per material and pipeline slot, with a quarter more so the tables grow ahead of the scene.
		const auto materialSlots = static_cast<std::uint32_t>(a_tables.materials.size()), pipelineSlots = static_cast<std::uint32_t>(a_tables.pipelines.size());
		if (r.materialRows.Reserve(materialSlots + materialSlots / 4))
			r.materialRowsHeld = 0;  // a new backing holds nothing
		if (r.pipelineRows.Reserve(pipelineSlots + pipelineSlots / 4))
			r.pipelineRowsHeld = 0;
		if (draws <= r.sequenceDraws && decals <= r.sequenceDecals)
			return;
		const std::uint32_t newDraws = Doubled(r.sequenceDraws, draws), newDecals = Doubled(r.sequenceDecals, decals);
		const std::uint64_t slots = SequenceSlots(newDraws, newDecals);
		// New backings for the same graph resources: the epochs rewrite them whole, and the old ones are released through ORG's
		// deletion queue once the GPU is done with them.
		r.sequences->ResizeStructured(static_cast<std::uint32_t>(slots * sizeof(DrawSequence) / 4));
		if (r.sort) {
			r.sort->staging->ResizeStructured(static_cast<std::uint32_t>(std::uint64_t(newDraws) * sizeof(DrawSequence) / 4));
			r.sort->ranks->ResizeStructured(newDraws);
		}
		if (r.sequencesD3D11)
			r.sequencesD3D11 = WrapWords(*r.sequences, slots * sizeof(DrawSequence));
		logger::info("[DCLF] main sequences: {} draws and {} per decal group grown to {} and {} ({} KB)", r.sequenceDraws, r.sequenceDecals, newDraws, newDecals,
			slots * sizeof(DrawSequence) / 1024);
		r.sequenceDraws = newDraws;
		r.sequenceDecals = newDecals;
	}

	void IndirectDraws::Impl::ReserveShadowSequences(const SceneStore::Tables& a_tables, std::uint32_t a_slots, std::uint32_t a_first)
	{
		if (!shadow)
			return;
		const std::uint32_t draws = SceneDrawBound(a_tables);
		CheckSequenceLimits("a shadow view", draws, false);
		for (std::uint32_t slot = a_first; slot < std::min(a_first + a_slots, kMaxShadowViews); ++slot) {
			auto& capacity = shadow->sequenceDraws[slot];
			if (draws <= capacity)
				continue;
			const std::uint32_t grown = Doubled(capacity, draws);
			shadow->sequences[slot]->ResizeStructured(static_cast<std::uint32_t>(std::uint64_t(grown) * sizeof(DrawSequence) / 4));
			logger::info("[DCLF] shadow view slot {} sequences: {} draws grown to {} ({} KB)", slot, capacity, grown, std::uint64_t(grown) * sizeof(DrawSequence) / 1024);
			capacity = grown;
		}
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
		state->constants = DeviceBuffer(kShadowConstantBytes, "cs.dclf.shadow.constants");
		// One table of material rows for every view (kShadowMaterialRowsInitial), grown as the kept state needs.
		const std::uint32_t initialRows = SwitchValue(Switch::TableStart) == "small" ? 4u : kShadowMaterialRowsInitial;
		if (!state->materialRows.Create(sizeof(ShadowMaterialRow), initialRows, "cs.dclf.shadow.material-rows")) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow material rows");
		}
		state->objects = StructuredBuffer(kMaxObjects, sizeof(BindlessObject), "cs.dclf.shadow.objects", state->objectsIndex);
		state->bones = StructuredBuffer(kMaxBoneRows, 16, "cs.dclf.shadow.bones", state->bonesIndex);
		state->facePositions = DeviceBuffer(std::uint64_t(kFacePositionVertices) * 16, "cs.dclf.shadow.face-positions");
		for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
			// Small: a slot's buffer grows to the scene's draws the first time a view uses it (ReserveShadowSequences), so the
			// slots no frame uses stay small.
			state->sequenceDraws[s] = 64u;
			state->sequences[s] = CreateWords(std::uint64_t(state->sequenceDraws[s]) * sizeof(DrawSequence) / 4, true, fmt::format("cs.dclf.shadow.sequences{}", s).c_str());
			state->count[s] = CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str());
			state->countD3D11[s] = WrapWords(*state->count[s], kCountWords * sizeof(std::uint32_t));
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
		state->facePositionsAddress = AddressOf(device, *state->facePositions);
		state->constantsAddress = AddressOf(device, *state->constants);
		if (!state->constantsAddress) {
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
