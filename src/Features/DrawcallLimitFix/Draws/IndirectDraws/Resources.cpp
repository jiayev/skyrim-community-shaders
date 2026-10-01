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
		std::shared_ptr<org::Buffer> StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, std::uint32_t& a_index, bool a_unorderedAccess = false)
		{
			auto created = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, a_unorderedAccess);
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
				draws += PartitionDraws(partitions);
			}
			return static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX));
		}

		/** @brief A shadow view slot's buffers for every slot up to a_slots (small: a slot's sequences grow the first time a view uses it). */
		void AddShadowViewSlots(ShadowResources& a_state, std::uint32_t a_slots)
		{
			for (auto s = static_cast<std::uint32_t>(a_state.sequences.size()); s < a_slots; ++s) {
				a_state.sequenceDraws.push_back(64u);
				a_state.sequences.push_back(CreateWords(64ull * sizeof(DrawSequence) / 4, true, fmt::format("cs.dclf.shadow.sequences{}", s).c_str()));
				a_state.count.push_back(CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str()));
				a_state.countD3D11.push_back(WrapWords(*a_state.count.back(), kCountWords * sizeof(std::uint32_t)));
			}
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
		// The rows both segments' draws name (MainRows): grown before each epoch to the scene's material and pipeline slots.
		const bool smallRows = SwitchValue(Switch::TableStart) == "small";
		if (!state->materialRows.Create(kMaterialRowBytes, smallRows ? 4u : 1024u, "cs.dclf.material-rows") ||
			!state->pipelineRows.Create(kPipelineRowBytes, smallRows ? 4u : 256u, "cs.dclf.pipeline-rows")) {
			logger::error("[DCLF] The main rows have no device address");
			return false;
		}
		if (!EnsureSceneBuffers(device)) {
			logger::error("[DCLF] The face positions buffer has no device address");
			return false;
		}
		state->scene = scene;
		// The draws' range, phase 2's and the decal groups' (GpuLayouts.h, SequenceSlots), grown before each epoch to hold every
		// draw the scene can produce (ReserveMainSequences).
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		state->sequenceDraws = smallTables ? 64u : kInitialSequenceDraws;
		state->sequenceDecals = smallTables ? 16u : kInitialDecalDraws;
		state->sequences = CreateWords(SequenceSlots(state->sequenceDraws, state->sequenceDecals) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences");
		state->count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
		// One word per object in the frame's tables: what the depth segment's culling decided, read by
		// the colour segment so that it draws exactly the same set.
		state->objectCapacity = scene->objectCapacity;
		state->visibility = CreateWords(state->objectCapacity, true, "cs.dclf.visibility");
		state->frustum = CreateWords(state->objectCapacity, true, "cs.dclf.frustum");
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
		state->inputs = CreateWords(std::uint64_t(state->objectCapacity) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs");
		state->inputsDepth = CreateWords(std::uint64_t(state->objectCapacity) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs-depth");
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (!state->buildDraws)
			return NotReady(7, "the BuildDraws program could not be created");
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature)
			return NotReady(7, "the BuildDraws dispatch signature could not be created");
		state->latchLayout.cascades = SwitchValue(Switch::TableStart) == "small" ? 1u : kInitialSunCascades;
		state->latchLayout.shadowVolumes = SwitchValue(Switch::TableStart) == "small" ? 1u : kInitialShadowVolumes;
		state->latch = std::make_shared<org::LatchBlock>("cs.dclf.latch", state->latchLayout.Bytes(), host->FrameSlots());
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
			state->visibilityD3D11 = WrapWords(*state->visibility, std::uint64_t(state->objectCapacity) * sizeof(std::uint32_t));
		state->frameConstants = DeviceBuffer(kFrameConstantBytes, "cs.dclf.frame-constants");
		state->frameConstantsAddress = AddressOf(device, *state->frameConstants);

		if (!state->frameConstantsAddress) {
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
		for (std::uint32_t slot = a_first; slot < std::min<std::size_t>(a_first + a_slots, shadow->sequences.size()); ++slot) {
			auto& capacity = shadow->sequenceDraws[slot];
			if (draws <= capacity)
				continue;
			const std::uint32_t grown = Doubled(capacity, draws);
			shadow->sequences[slot]->ResizeStructured(static_cast<std::uint32_t>(std::uint64_t(grown) * sizeof(DrawSequence) / 4));
			logger::info("[DCLF] shadow view slot {} sequences: {} draws grown to {} ({} KB)", slot, capacity, grown, std::uint64_t(grown) * sizeof(DrawSequence) / 1024);
			capacity = grown;
		}
	}

	bool IndirectDraws::Impl::EnsureSceneBuffers(rhi::Device a_device)
	{
		if (scene)
			return true;
		auto buffers = std::make_shared<SceneBuffers>();
		const bool smallStart = SwitchValue(Switch::TableStart) == "small";
		buffers->objectCapacity = smallStart ? 64u : kInitialObjects;
		buffers->geometryRows = smallStart ? 64u : kInitialGeometries;
		buffers->boneRows = smallStart ? 256u : kInitialBoneRows;
		buffers->faceVertices = smallStart ? 1024u : kInitialFaceVertices;
		// Structured, because the shaders read the object records and the bone rows through SRVs (t127, t126); the geometry
		// table is BuildDraws' (raw words), and the face positions are a vertex buffer.
		// The object records take TreeWindCS's writes of the trees' wind (unordered access).
		buffers->objects = StructuredBuffer(buffers->objectCapacity, sizeof(BindlessObject), "cs.dclf.objects", buffers->objectsIndex, true);
		buffers->bones = StructuredBuffer(buffers->boneRows, 16, "cs.dclf.bones", buffers->bonesIndex);
		buffers->geometries = CreateWords(std::uint64_t(buffers->geometryRows) * sizeof(GeometryDraw) / 4, false, "cs.dclf.geometries");
		buffers->facePositions = DeviceBuffer(std::uint64_t(buffers->faceVertices) * 16, "cs.dclf.face-positions");
		buffers->facePositionsAddress = AddressOf(a_device, *buffers->facePositions);
		if (!buffers->facePositionsAddress)
			return false;
		// Tree wind (TreeWindCS): without its program the members' records keep the wind they joined with.
		buffers->treeWind = ComputeProgram::Load(a_device, { .source = kTreeWindShader, .constantWords = kTreeWindConstantWords });
		if (buffers->treeWind) {
			std::uint32_t unused = 0;
			buffers->treeCapacity = smallStart ? 4u : kInitialTrees;
			buffers->treeObjectCapacity = smallStart ? 16u : kInitialTreeObjects;
			buffers->trees = StructuredBuffer(buffers->treeCapacity, sizeof(TreeStatic), "cs.dclf.trees", unused);
			buffers->treeClocks = StructuredBuffer(buffers->treeCapacity, sizeof(TreeClock), "cs.dclf.tree-clocks", unused, true);
			buffers->treeObjects = StructuredBuffer(buffers->treeObjectCapacity, sizeof(TreeObject), "cs.dclf.tree-objects", unused);
			buffers->treeFrameBuffer = StructuredBuffer(1, sizeof(TreeWindFrameRow), "cs.dclf.tree-frame", unused);
		} else {
			logger::warn("[DCLF] The tree wind program could not be created; trees keep the wind they joined with");
		}
		// Fade roots (FadeStateCS): without its program the fades stay the CPU's alone.
		buffers->fadeState = ComputeProgram::Load(a_device, { .source = kFadeStateShader, .constantWords = kFadeStateConstantWords });
		if (buffers->fadeState) {
			std::uint32_t unused = 0;
			buffers->fadeRootCapacity = smallStart ? 4u : kInitialFadeRoots;
			buffers->fadeRoots = StructuredBuffer(buffers->fadeRootCapacity, sizeof(FadeRootStatic), "cs.dclf.fade-roots", unused);
			buffers->fadeStates = StructuredBuffer(buffers->fadeRootCapacity, sizeof(FadeNodeState), "cs.dclf.fade-states", unused, true);
			buffers->fadeFrameBuffer = StructuredBuffer(1, sizeof(FadeFrame), "cs.dclf.fade-frame", unused);
			buffers->fadeVisibility = CreateWords(kFadeVisibilityBytes / 4, false, "cs.dclf.fade-visibility");
			buffers->fadeLog = StructuredBuffer(kFadeLogEntries, sizeof(FadeLogEntry), "cs.dclf.fade-log", unused, true);
		} else {
			logger::warn("[DCLF] The fade state program could not be created; fades stay the CPU's");
		}
		scene = std::move(buffers);
		return true;
	}

	void IndirectDraws::Impl::ReserveShadowLatch(std::uint32_t a_views, std::uint32_t a_keys, std::uint32_t a_rasterStates, std::uint32_t a_sunProcesses)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!shadow || !host)
			return;
		auto& r = *shadow;
		ShadowLatchLayout layout = r.latchLayout;
		const std::uint32_t slots = kFirstShadowViewSlot + a_views;
		const bool newSlots = slots > layout.viewSlots;
		if (newSlots) {
			layout.viewSlots = Doubled(layout.viewSlots, slots);
			AddShadowViewSlots(r, layout.viewSlots);
			r.viewBlocks.Reserve(layout.viewSlots);  // rewritten whole by every epoch that uses a slot: nothing to send again
			logger::info("[DCLF] shadow view slots: {} grown to {}", r.latchLayout.viewSlots, layout.viewSlots);
		}
		if (a_keys > layout.keySlots) {
			layout.keySlots = Doubled(layout.keySlots, a_keys);
			logger::info("[DCLF] shadow key slots: {} grown to {}", r.latchLayout.keySlots, layout.keySlots);
		}
		if (a_rasterStates > layout.rasterStates) {
			layout.rasterStates = Doubled(layout.rasterStates, a_rasterStates);
			logger::info("[DCLF] shadow view rasterizer state rows: {} grown to {}", r.latchLayout.rasterStates, layout.rasterStates);
		}
		if (a_sunProcesses > layout.sunProcesses) {
			layout.sunProcesses = Doubled(layout.sunProcesses, a_sunProcesses);
			logger::info("[DCLF] sun full-frustum processes: {} grown to {}", r.latchLayout.sunProcesses, layout.sunProcesses);
		}
		if (layout == r.latchLayout)
			return;
		r.latchLayout = layout;
		// Every execution writes its frame slot's region whole (the views' latches, the map rows of their states, the sun's
		// processes), and a block
		// frames in flight still read stays alive in the frames they prepared.
		r.latch = std::make_shared<org::LatchBlock>("cs.dclf.shadow.latch", layout.Bytes(), host->FrameSlots());
		// The passes declare every slot's buffers: the graph is built again, with them, on this epoch.
		if (newSlots)
			host->AddExtension(kShadowExtensionId, [state = shadow] { return MakeShadowExtension(state); });
	}

	void IndirectDraws::Impl::ReserveMainLatch(Resources& a_resources, std::uint32_t a_cascades, std::uint32_t a_shadowVolumes)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto& layout = a_resources.latchLayout;
		if (!host || (a_cascades <= layout.cascades && a_shadowVolumes <= layout.shadowVolumes))
			return;
		if (a_cascades > layout.cascades) {
			const std::uint32_t grown = Doubled(layout.cascades, a_cascades);
			logger::info("[DCLF] sun cascades in the main latch: {} grown to {}", layout.cascades, grown);
			layout.cascades = grown;
		}
		if (a_shadowVolumes > layout.shadowVolumes) {
			const std::uint32_t grown = Doubled(layout.shadowVolumes, a_shadowVolumes);
			logger::info("[DCLF] local shadow light volumes in the main latch: {} grown to {}", layout.shadowVolumes, grown);
			layout.shadowVolumes = grown;
		}
		// Every execution writes its frame slot's region whole; the frames in flight keep the old block (PassFrame::latch).
		a_resources.latch = std::make_shared<org::LatchBlock>("cs.dclf.latch", a_resources.latchLayout.Bytes(), host->FrameSlots());
	}

	void IndirectDraws::Impl::ReserveSceneTables(const SceneStore::Tables& a_tables)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!scene || !host)
			return;
		auto& s = *scene;
		// Doubling, so a scene filling up grows a handful of times. A new backing holds nothing: the held version goes to 0, and
		// the next commit sends the table whole.
		auto grow = [&](const char* a_name, std::uint32_t& a_capacity, std::uint64_t a_needed, std::uint32_t a_rowBytes, auto&& a_resize) {
			if (a_needed <= a_capacity)
				return false;
			const std::uint32_t rows = Doubled(a_capacity, static_cast<std::uint32_t>(std::min<std::uint64_t>(a_needed, UINT32_MAX)));
			a_resize(rows);
			logger::info("[DCLF] scene {}: {} grown to {} ({} KB)", a_name, a_capacity, rows, std::uint64_t(rows) * a_rowBytes / 1024);
			a_capacity = rows;
			++s.generation;
			s.layout.fetch_add(1, std::memory_order_release);
			++s.growths;
			return true;
		};
		if (grow("object records", s.objectCapacity, a_tables.objects.size(), sizeof(BindlessObject), [&](std::uint32_t a_rows) {
				s.objects->ResizeStructured(a_rows);
				s.objectsIndex = s.objects->GetSRVInfo(0).slot.index;
			}))
			s.held.objects = 0;
		// The slots, then one row per face stream (AppendFaceStreams).
		if (grow("geometry rows", s.geometryRows, a_tables.geometries.size() + a_tables.faceStreams.size(), sizeof(GeometryDraw),
				[&](std::uint32_t a_rows) { s.geometries->ResizeStructured(static_cast<std::uint32_t>(std::uint64_t(a_rows) * sizeof(GeometryDraw) / 4)); }))
			s.held.geometries = 0;
		// Every palette, current then previous, then the extras (BonesOut::Rows).
		if (grow("bone rows", s.boneRows, 2ull * a_tables.BoneCapacity() + a_tables.extraRows.size() / 4, 16, [&](std::uint32_t a_rows) {
				s.bones->ResizeStructured(a_rows);
				s.bonesIndex = s.bones->GetSRVInfo(0).slot.index;
			}))
			s.held.bones = 0;
		std::uint64_t faceVertices = 0;
		for (const auto& stream : a_tables.faceStreams)
			if (stream.object != SceneStore::Tables::kNoFaceObject)
				faceVertices = std::max<std::uint64_t>(faceVertices, std::uint64_t(stream.region) + stream.vertexCount);
		// The tree rows and clocks by tree slot, the members' list. A new clock backing holds no clock: every tree starts again
		// from the values it was listed with (its static row), once.
		if (s.trees) {
			if (grow("tree rows", s.treeCapacity, a_tables.trees.size(), sizeof(TreeStatic) + sizeof(TreeClock), [&](std::uint32_t a_rows) {
					s.trees->ResizeStructured(a_rows);
					s.treeClocks->ResizeStructured(a_rows);
				}))
				s.treesHeld = ~0ull;
			if (grow("tree members", s.treeObjectCapacity, a_tables.treeObjects.size(), sizeof(TreeObject), [&](std::uint32_t a_rows) { s.treeObjects->ResizeStructured(a_rows); }))
				s.treeObjectsHeld = ~0ull;
		}
		// The fade roots' static and state rows by root slot. A new state backing holds no state: every root is seeded again
		// from its static row (a zero generation is never a listing's).
		if (s.fadeRoots) {
			if (grow("fade roots", s.fadeRootCapacity, a_tables.fadeRoots.size(), sizeof(FadeRootStatic) + sizeof(FadeNodeState), [&](std::uint32_t a_rows) {
					s.fadeRoots->ResizeStructured(a_rows);
					s.fadeStates->ResizeStructured(a_rows);
				}))
				s.fadeRootsHeld = ~0ull;
		}
		if (grow("face position vertices", s.faceVertices, faceVertices, 16, [&](std::uint32_t a_rows) {
				s.facePositions->ResizeBytes(std::uint64_t(a_rows) * 16);
				s.facePositionsAddress = AddressOf(host->GetDesc().device, *s.facePositions);
			}))
			s.faceUploaded.clear();  // every region again
		ReserveObjectBuffers();
	}

	void IndirectDraws::Impl::ReserveObjectBuffers()
	{
		if (!scene)
			return;
		const std::uint32_t objects = scene->objectCapacity;
		const auto inputWords = [&] { return static_cast<std::uint32_t>(std::uint64_t(objects) * sizeof(DrawInput) / 4); };
		if (resources && resources->objectCapacity < objects) {
			auto& r = *resources;
			r.visibility->ResizeStructured(objects);
			if (r.frustum)
				r.frustum->ResizeStructured(objects);
			r.inputs->ResizeStructured(inputWords());
			if (r.inputsDepth)
				r.inputsDepth->ResizeStructured(inputWords());
			r.residentUploaded = {};  // the new input buffers hold no region
			if (r.visibilityD3D11)
				r.visibilityD3D11 = WrapWords(*r.visibility, std::uint64_t(objects) * sizeof(std::uint32_t));
			r.objectCapacity = objects;
		}
		if (shadow && shadow->objectCapacity < objects) {
			auto& r = *shadow;
			r.visibility->ResizeStructured(objects);
			for (auto& inputs : r.inputs)
				inputs->ResizeStructured(inputWords());
			r.inputsUploaded = {};  // the new input buffers hold none of the kept state's inputs
			r.objectCapacity = objects;
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
		if (!EnsureSceneBuffers(device)) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the face positions");
		}
		state->scene = scene;
		// CS_DCLF_TABLE_START=small: room for Skylighting's map and one view, and a few key slots.
		const bool smallSlots = SwitchValue(Switch::TableStart) == "small";
		state->latchLayout = { smallSlots ? 2u : kInitialShadowViewSlots, smallSlots ? 16u : kInitialShadowKeySlots, smallSlots ? 1u : kInitialShadowRasterStates,
			smallSlots ? 1u : kInitialSunProcesses };
		AddShadowViewSlots(*state, state->latchLayout.viewSlots);
		if (!state->viewBlocks.Create(static_cast<std::uint32_t>(kShadowViewSlotBytes), state->latchLayout.viewSlots, "cs.dclf.shadow.view-blocks")) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow view blocks");
		}
		state->objectCapacity = scene->objectCapacity;
		state->visibility = CreateWords(state->objectCapacity, true, "cs.dclf.shadow.visibility");
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			state->inputs[m] = CreateWords(std::uint64_t(state->objectCapacity) * sizeof(DrawInput) / 4, false, fmt::format("cs.dclf.shadow.draw-inputs{}", m).c_str());
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
		state->latch = std::make_shared<org::LatchBlock>("cs.dclf.shadow.latch", state->latchLayout.Bytes(), host->FrameSlots());
		state->preprocessShadow = PreprocessStates::Create(device, host->FrameSlots(), "the shadow views");
		state->preprocessSky = PreprocessStates::Create(device, host->FrameSlots(), "Skylighting's occlusion map");
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
			"DCLF Skylighting occlusion map", "DCLF precipitation occlusion mask" };
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
