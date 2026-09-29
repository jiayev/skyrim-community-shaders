#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	namespace Draws
	{
		/**
		 * @brief A shadow view's latch as every view has it: the dispatch over its inputs, every one drawn (a single phase,
		 * no engine-visibility gate), its view-projection with the eye folded in, and the frame's stamp. The culling's
		 * flags and planes are the caller's.
		 */
		BuildDrawsLatch ShadowViewLatch(const PendingView& a_view, std::uint32_t a_inputs, std::uint32_t a_frame)
		{
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (a_inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = a_inputs;
			latch.visibilityStamp = a_frame & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
			FoldEyeIntoViewProj(a_view.viewProj, a_view.eye, latch.viewProj);
			return latch;
		}

		/**
		 * @brief Points a_latch at its rasterizer state's row of the pipeline map, and writes the row into the frame slot's
		 * latch block when a_write (once per state and frame). The shader reads the row at an offset into the whole latch
		 * block, so the offset carries the frame slot's base: a slot-relative one read slot 0's rows, which async epochs never
		 * write (every draw got pipeline 0).
		 */
		void UseShadowMapRow(ShadowResources& a_resources, const Lookups& a_lookups, std::uint32_t a_latchSlot, std::uint32_t a_rasterState, bool a_write,
			BuildDrawsLatch& a_latch)
		{
			const std::uint32_t mapRowOffset = kShadowPipelineMapOffset + (a_rasterState - 1) * kShadowPipelineMapRowBytes;
			a_latch.pipelineMapOffset = static_cast<std::uint32_t>(a_resources.latch->Offset(a_latchSlot)) + mapRowOffset;
			if (!a_write)
				return;
			const auto& row = a_lookups.shadowMapRows[a_rasterState];
			if (!row.empty())
				a_resources.latch->Write(a_latchSlot, mapRowOffset, std::as_bytes(std::span(row.data(), std::min<std::size_t>(row.size(), kMaxShadowSlots))));
		}

		/**
		 * @brief Where a view draws: its slot's buffers, its viewport and depth range in its target's slice, and its push data
		 * (DrawPipelines.h, kShadowPushWords): the frame record, its own blocks at its slot of the arena's head, and the build's.
		 */
		ShadowFrameView FrameViewOf(const PendingView& a_view, std::uint32_t a_slot, std::uint32_t a_mode, std::uint32_t a_target, std::uint32_t a_capacity,
			const ShadowResources& a_resources, const ShadowPayload& a_payload)
		{
			ShadowFrameView out{};
			out.slot = a_slot;
			out.modeIndex = a_mode;
			out.capacity = a_capacity;
			out.x = a_view.x;
			out.y = a_view.y;
			out.width = a_view.width;
			out.height = a_view.height;
			out.minDepth = a_view.minDepth;
			out.maxDepth = a_view.maxDepth;
			out.target = a_target;
			out.slice = a_view.slice;
			out.materialRows = a_resources.materialRows.address;
			const std::uint64_t base = a_resources.constantsAddress;
			const std::uint64_t viewBlock = base + std::uint64_t(a_slot) * kShadowViewSlotBytes;
			auto push = [&](std::uint32_t a_word, std::uint64_t a_address) {
				out.push[a_word] = static_cast<std::uint32_t>(a_address);
				out.push[a_word + 1] = static_cast<std::uint32_t>(a_address >> 32);
			};
			push(kShadowPushFrameRecord, base + kShadowFrameRecordOffset);
			push(kShadowPushViewBlock, viewBlock);
			push(kShadowPushPerFrame, viewBlock + kShadowPerFrameOffset);
			push(kShadowPushZeros, a_payload.zerosAddress);
			push(kShadowPushSharedData, a_payload.sharedDataAddress);
			push(kShadowPushFeatureData, a_payload.featureDataAddress);
			return out;
		}

		/**
		 * @brief The id DCLF's pipelines give the engine's rasterizer state at (fill, cull, bias, scissor) for a_renderMode: 0
		 * when an index is out of the engine's table or its entry is empty.
		 */
		std::uint32_t EngineRasterStateId(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_bias, std::uint32_t a_scissor, std::uint32_t a_renderMode)
		{
			if (a_fill >= 2 || a_cull >= 3 || a_bias >= 12 || a_scissor >= 2)
				return 0;
			auto* engineState = EngineRasterStates()[a_fill][a_cull][a_bias][a_scissor];
			if (!engineState)
				return 0;
			D3D11_RASTERIZER_DESC desc{};
			engineState->GetDesc(&desc);
			return DrawPipelines::Get().ShadowRasterStateId(desc, a_renderMode);
		}

		/** @brief Where the engine has just drawn a view, from the renderer's state: the depth target's format, the viewport and the eye. */
		void CaptureViewTarget(PendingView& a_view, std::uint32_t a_target)
		{
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsv->GetDesc(&dsvDesc);
				a_view.dsvFormat = dsvDesc.Format;
			}
			a_view.x = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftX));
			a_view.y = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftY));
			a_view.width = static_cast<std::uint32_t>(shadowState.viewPort.Width);
			a_view.height = static_cast<std::uint32_t>(shadowState.viewPort.Height);
			a_view.minDepth = shadowState.viewPort.MinDepth;
			a_view.maxDepth = shadowState.viewPort.MaxDepth;
			a_view.eye = shadowState.posAdjust.getEye();
		}

		/**
		 * @brief VS_PerFrame (b12) as the engine wrote it for the view, taken now because the next view rewrites it: from the
		 * mirror, or from Community Shaders' copy of the same buffer (Globals: CacheFramebuffer) until the mirror has seen a
		 * write. Its view-projection is what the view's draws use.
		 */
		void CapturePerFrame(PendingView& a_view)
		{
			auto& mirror = ConstantMirror::Get();
			if (auto* perFrame = *globals::game::perFrame.get()) {
				mirror.Watch(perFrame);
				const auto contents = mirror.Contents(perFrame);
				if (contents.size() >= 48 * sizeof(float)) {
					a_view.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), a_view.perFrame.size()));
					std::memcpy(a_view.perFrame.data(), contents.data(), a_view.perFrameBytes);
				}
			}
			if (!a_view.perFrameBytes) {
				const auto& cached = globals::game::frameBufferCached.data;
				static_assert(sizeof(cached) >= 48 * sizeof(float) && sizeof(cached) <= 1024);
				a_view.perFrameBytes = sizeof(cached);
				std::memcpy(a_view.perFrame.data(), &cached, sizeof(cached));
			}
			std::memcpy(a_view.viewProj.data(), reinterpret_cast<const float*>(a_view.perFrame.data()) + 32, sizeof(float) * 16);
			a_view.hasViewProj = true;
		}
	}

	void IndirectDraws::BeginShadowFrame()
	{
		impl->pendingViews.clear();
	}

	void IndirectDraws::CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode)
	{
		// The hook's half: capture the view - where the engine has just drawn, and the constants it drew
		// with - for the frame's single epoch (ExecuteShadowFrame). Nothing is drawn here.
		if (!ActiveToggles().shadows || failed)
			return;
		ScopedPerfEvent event("CS DCLF: shadow view capture");
		const auto start = std::chrono::steady_clock::now();
		++shadowStats.views;
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		auto notReady = [&](ShadowNotReady a_reason) {
			++shadowStats.notReady;
			++shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)];
		};
		if (!pipelines.Enabled() || !utility || !impl->SetupShadow())
			return notReady(ShadowNotReady::Setup);
		// The tables are not read here: under CS_DCLF_ASYNC the scene walk is still writing them while the
		// engine draws the shadow maps. ExecuteShadowFrame checks them after the join.
		const auto* shadowView = ShadowViews::Get().At(a_viewId);
		if (!shadowView)
			return notReady(ShadowNotReady::Tables);
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + kShadowModeCount)
			return notReady(ShadowNotReady::Tables);
		// A focus shadow holds one actor's casters, not the scene's, and DCLF has no caster set for it: it stays native.
		if (shadowView->focus) {
			++shadowStats.focusSkipped;
			return;
		}
		// Where the engine has just drawn: the target and slice come from the renderer's state, because the
		// descriptor's own fields are filled only when the draw allocates them (engine notes: shadow maps).
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t target = shadowState.depthStencil;
		const std::uint32_t slice = shadowState.depthStencilSlice;
		const std::uint32_t targetIndex = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM                     ? 0u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS                           ? 1u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM ? 2u :
		                                                                                                                     ~0u;
		// The volumetric lighting copy holds only the volumetric-only casters (batch group 15, which is all the
		// engine's flag-0x100 draw of the view renders): the view draws those alone (casterClass 1).
		const bool volumetricCopy = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;
		if (targetIndex == ~0u || !impl->ImportShadowDepth(targetIndex, target)) {
			// Which target, once per target: anything here is a view the design has not met.
			if (target < 32 && !((impl->shadowLoggedTargets >> target) & 1)) {
				impl->shadowLoggedTargets |= 1u << target;
				logger::info("[DCLF] shadow view {} ({}, light {} descriptor {}, mode {:#x}) draws into depth target {} slice {}; it stays native",
					a_viewId, ShadowViews::KindName(shadowView->kind), shadowView->lightIndex, shadowView->descriptor, a_renderMode, target, slice);
			}
			return notReady(ShadowNotReady::Depth);
		}
		if (impl->pendingViews.size() >= kSkySlot)
			return notReady(ShadowNotReady::Capacity);
		// The rasterizer state the engine draws this view with: its table entry for the renderer's modes, read
		// now, while the view is drawn - Community Shaders' ShadowmapCascadeRasterizerFix swaps per-cascade
		// copies with their own depth bias into that table for exactly this window, and the volumetric copy
		// draws with culling off. DCLF's pipelines for the view are built with it.
		const std::uint32_t rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, shadowState.rasterStateCullMode,
			shadowState.rasterStateDepthBiasMode, shadowState.rasterStateScissorMode, a_renderMode);
		if (rasterState == 0)
			return notReady(ShadowNotReady::Pipelines);

		auto& view = impl->pendingViews.emplace_back();
		view.viewId = a_viewId;
		view.renderMode = a_renderMode;
		view.modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		view.targetIndex = targetIndex;
		view.slice = slice;
		view.rasterState = rasterState;
		view.casterClass = volumetricCopy ? 1u : 0u;
		view.sunView = shadowView->kind == ShadowViews::Kind::Directional;
		CaptureViewTarget(view, target);
		// The caster volume the engine culled this view's casters against: BSShadowDirectionalLight::UpdateCamera
		// builds it from the main camera frustum's corners and the light direction into the descriptor's culling
		// process, and the accumulation's cull (FUN_1414f0920) tests it on top of the shadow camera's frustum. It
		// is much tighter than the orthographic box: without it DCLF drew into the far cascade the casters of a
		// whole slice's worth of terrain that no visible receiver can see a shadow from.
		if (auto& lightData = const_cast<RE::BSShadowLight*>(shadowView->light)->GetRuntimeData(); shadowView->descriptor < lightData.shadowmapDescriptors.size()) {
			const auto* process = lightData.shadowmapDescriptors[shadowView->descriptor].cullingProcess;
			if (process && process->doCustomCullPlanes) {
				const auto& planes = process->customCullPlanes;
				for (std::uint32_t p = 0; p < RE::NiFrustumPlanes::Planes::kTotal; ++p) {
					view.cullPlanes[p][0] = planes.cullingPlanes[p].normal.x;
					view.cullPlanes[p][1] = planes.cullingPlanes[p].normal.y;
					view.cullPlanes[p][2] = planes.cullingPlanes[p].normal.z;
					view.cullPlanes[p][3] = planes.cullingPlanes[p].constant;
				}
				view.cullPlaneMask = planes.activePlanes.underlying() & 0x3Fu;
			}
		}
		// The view's PerTechnique block (b0): HighDetailRange (LOD landscape only; zero until owned),
		// ParabolaParam from the engine's two globals as its SetupTechnique reads them, and the eye delta
		// from the frame's reference eye to this view's.
		{
			static const REL::Relocation<float*> parabolaRadius{ REL::Offset(0x2035df8) };
			static const REL::Relocation<float*> parabolaSide{ REL::Offset(0x2035dfc) };
			const float radius = *parabolaRadius.get();
			view.viewBlock[4] = radius != 0.0f ? 1.0f / radius : 0.0f;
			view.viewBlock[5] = *parabolaSide.get();
			// c2 (DCLFEyeDelta) is not read: the records are absolute and Utility.hlsl subtracts the view's own
			// CameraPosAdjust. Left zero.
			view.viewBlock[8] = view.viewBlock[9] = view.viewBlock[10] = 0.0f;
		}
		CapturePerFrame(view);
		shadowStats.captureMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void IndirectDraws::CaptureSkyOcclusion()
	{
		// The hook's half, as CaptureShadowView: where the engine's RenderMask has just drawn Skylighting's map (the
		// clear, and whatever SetupMask registered), with which camera and state. Taken whether or not DCLF draws the
		// map this frame: the state and format are what next frame's build prepares the pipelines for.
		if (!ActiveToggles().shadows || failed || !SceneStore::SkyOcclusionEnabled() || !impl->SetupShadow())
			return;
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t target = shadowState.depthStencil;
		// The renderer's state at this hook is what the view's last pass left, or whatever came before when nothing
		// drew; the Utility shader sets the cull mode per pass (engine notes, shadow maps: 0 for a two-sided
		// property, 1 otherwise). So the view's state is back-face culling at the renderer's fill, bias and scissor
		// modes, and a two-sided occluder's key draws without culling, as a two-sided caster's does.
		const std::uint32_t rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 1, shadowState.rasterStateDepthBiasMode,
			shadowState.rasterStateScissorMode, kSkyRenderMode);
		if (!rasterState || !impl->ImportShadowDepth(kSkyDepthTarget, target))
			return;
		auto& view = impl->skyView;
		view = {};
		view.viewId = ~0u;
		view.renderMode = kSkyRenderMode;
		view.modeIndex = kSkyMode;
		view.targetIndex = kSkyDepthTarget;
		view.slice = shadowState.depthStencilSlice;
		view.rasterState = rasterState;
		CaptureViewTarget(view, target);
		CapturePerFrame(view);
		impl->skyRasterState = rasterState;
		impl->skyDsvFormat = view.dsvFormat;
		impl->skyCapturedFrame = SceneStore::Get().GetFrame();
	}

	bool IndirectDraws::SkyOcclusionReady() const
	{
		// This frame's shadow commit uploaded every occluder (none left out for a pipeline or a texture not yet
		// resolved), and the map's target is imported.
		return !failed && ActiveToggles().shadows && SceneStore::SkyOcclusionEnabled() && impl->shadow && impl->skyRasterState &&
		       impl->skyCommittedFrame == SceneStore::Get().GetFrame() && impl->skySkipped == 0 && impl->shadow->depth[kSkyDepthTarget];
	}

	bool IndirectDraws::ExecuteSkyOcclusion()
	{
		ZoneScopedN("CS.DCLF.ExecuteSkyOcclusion");
		const auto start = std::chrono::steady_clock::now();
		auto& store = SceneStore::Get();
		const std::uint32_t frameNumber = store.GetFrame();
		if (!SkyOcclusionReady() || impl->skyCapturedFrame != frameNumber || impl->skyView.rasterState != impl->skyRasterState) {
			++shadowStats.skyNotReady;
			return false;
		}
		const auto indirect = GetShadowIndirectState();
		if (!indirect.valid) {
			++shadowStats.skyNotReady;
			return false;
		}
		ScopedPerfEvent event("CS DCLF: Skylighting occlusion (CPU)");
		auto resources = impl->shadow;
		auto& payload = impl->shadowPayload;
		const auto& view = impl->skyView;
		const auto inputCount = static_cast<std::uint32_t>(payload.ModeInputs(kSkyMode));
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::SkyOcclusion, [&](org::RenderGraph&) {
			resources->skyFrame.store(nullptr, std::memory_order_release);
			CommitUploads uploads(impl->commitStagedPool);
			// The view slot's blocks, which its push data names, and its count zeroed. The occluders, material rows, frame
			// record, objects and geometries were uploaded by this frame's shadow commit.
			const std::uint64_t viewBlockOffset = std::uint64_t(kSkySlot) * kShadowViewSlotBytes;
			const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
			uploads(resources->constants, view.viewBlock, sizeof(view.viewBlock), viewBlockOffset);
			uploads(resources->constants, view.perFrame.data(), view.perFrameBytes, perFrameOffset);
			uploads(resources->count[kSkySlot], kZeroCounts, sizeof(kZeroCounts), 0);
			// Its latch: frustum culling alone, near plane included as the rasterizer clips, every input drawn.
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			auto latch = ShadowViewLatch(view, inputCount, frameNumber);
			latch.cullFlags = 1u;
			UseShadowMapRow(*resources, store.GetLookups(), latchSlot, view.rasterState, true, latch);
			resources->latch->WriteValue(latchSlot, kSkySlot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
			auto frame = std::make_shared<ShadowFrame>();
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = indirect;
			const auto& previousShape = resources->skyPublished;
			const std::uint32_t capacity = GrowCapacity(previousShape && !previousShape->views.empty() ? previousShape->views.front().capacity : 0u, inputCount, kMaxDraws);
			frame->views.push_back(FrameViewOf(view, kSkySlot, kSkyMode, kSkyDepthTarget, capacity, *resources, payload));
			PublishShape(std::move(frame), resources->skyPublished, resources->skyFrame, resources->shapeGenerations);
		}, impl->shadowExecutionOwner);
		shadowStats.skyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (!ok) {
			++shadowStats.skyNotReady;
			return false;
		}
		++shadowStats.skyDrawn;
		shadowStats.skyInputs = inputCount;
		return true;
	}

	void IndirectDraws::ExecuteShadowFrame()
	{
		ZoneScopedN("CS.DCLF.ExecuteShadowFrame");
		auto& pending = impl->pendingViews;
		if (!ActiveToggles().shadows || failed || pending.empty()) {
			pending.clear();
			impl->DropShadowJob(stats);
			return;
		}
		ScopedPerfEvent event("CS DCLF: shadow views (CPU)");
		const auto start = std::chrono::steady_clock::now();
		auto notReady = [&](ShadowNotReady a_reason) {
			shadowStats.notReady += static_cast<std::uint32_t>(pending.size());
			shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)] += static_cast<std::uint32_t>(pending.size());
			pending.clear();
			impl->DropShadowJob(stats);
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !impl->shadow)
			return notReady(ShadowNotReady::Setup);
		const auto indirect = GetShadowIndirectState();
		if (!indirect.valid) {
			impl->ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		const std::uint32_t frameNumber = store.GetFrame();
		auto resources = impl->shadow;
		std::array<bool, kShadowModeCount> modeUsed{};
		std::array<std::uint32_t, kShadowModeCount> modeRasterStates{};
		for (const auto& view : pending) {
			modeUsed[view.modeIndex] = true;
			modeRasterStates[view.modeIndex] |= 1u << (view.rasterState + (view.casterClass ? 16u : 0u));
		}
		// Skylighting's occlusion map is drawn later in the frame, by its own epoch, from this build: its occluders
		// under the state its view drew with last (known once the engine's own draw of the map has been captured).
		if (SceneStore::SkyOcclusionEnabled() && impl->skyRasterState && impl->skyDsvFormat != DXGI_FORMAT_UNKNOWN) {
			modeUsed[kSkyMode] = true;
			modeRasterStates[kSkyMode] = 1u << impl->skyRasterState;
		}
		// One pipeline set per shadow map format: every target the engine has is D16 (engine notes), and a
		// view whose target differed would rebuild the set on every epoch, so the first view's is taken.
		const DXGI_FORMAT dsvFormat = pending.front().dsvFormat;
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0;

		impl->ReserveShadowRows();
		ShadowInputs in = impl->PrepareShadowInputs(store, *resources, modeUsed, modeRasterStates);
		impl->shadowJob.modes = modeUsed;
		impl->shadowJob.rasterStates = modeRasterStates;
		impl->shadowJob.modesKnown = true;
		impl->shadowJob.views = static_cast<std::uint32_t>(pending.size());
		auto& payload = impl->shadowPayload;
		auto& async = stats.async[kAsyncShadow];
		bool usedWorkerBuild = false;
		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup)
			return;
		auto frameOwners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();

		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::ShadowView, [&](org::RenderGraph&) {
			ZoneScopedN("CS.DCLF.ShadowInputs");
			struct BodyTimer
			{
				std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				double& out;
				~BodyTimer() { out = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
			} bodyTimer{ {}, bodyMs };
			bodyTimer.start = std::chrono::steady_clock::now();

			// The worker's build if one was kicked and it was built for exactly these inputs, else the build
			// here. Joined before the lookups are refreshed: the worker reads them until it is done.
			bool useAsync = false;
			TracyCZoneN(shadowPrepareZone, "CS.DCLF.ShadowInputs.Prepare", true);
			const auto prepareStart = std::chrono::steady_clock::now();
			auto& job = impl->shadowJob;
			const auto joined = JoinJob(job.handle);
			auto& lookups = store.MutableLookups();
			RefreshMaterialLookups(store, tables, frameNumber, store.GetProjectedTextures(), lookups);
			RefreshShadowLookups(store, tables, modeUsed, modeRasterStates, dsvFormat, impl->skyDsvFormat, lookups);
			in.lookupGeneration = lookups.generation;
			if (job.handle) {
				useAsync = TakeJob(
					joined, async, [&] { return SameShadowInputs(job.inputs, in); },
					[&] {
						if (job.loggedStale++ < 4) {
							const auto& k = job.inputs;
							logger::info("[DCLF] async shadow: the job's inputs are stale (frame {} vs {}, modes {}{}{} vs {}{}{}, shared data {}, feature data {}, tables {} vs {}, lookups {} vs {}, resources {})",
								k.frameNumber, in.frameNumber, int(k.modeUsed[0]), int(k.modeUsed[1]), int(k.modeUsed[2]),
								int(in.modeUsed[0]), int(in.modeUsed[1]), int(in.modeUsed[2]), k.sharedData == in.sharedData ? "same" : "differs",
								k.featureData == in.featureData ? "same" : "differs", k.tablesGeneration, in.tablesGeneration, k.lookupGeneration,
								in.lookupGeneration, k.addresses == in.addresses ? "same" : "changed");
						}
					});
				job.handle = {};
			}
			usedWorkerBuild = useAsync;
			if (useAsync) {
				++async.used;
				ProbeWorkerBuild(payload, impl->shadowProbePayload, async, "shadow",
					[&](ShadowPayload& a_probe) { BuildShadowPayload(job.inputs, tables, lookups, a_probe); });
			} else {
				++async.builtInline;
				BuildShadowPayload(in, tables, lookups, payload, impl->ShadowObjects(), impl->ShadowBones(), impl->ShadowKeptState(), impl->ShadowGeometries());
			}
			*frameOwners = std::move(payload.bindingOwners);
			impl->shadowExecutionOwner = frameOwners;
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
			TracyCZoneEnd(shadowPrepareZone);

			// Nothing to draw until this commit publishes the shape again (so a failed one draws nothing, rather
			// than a reused recording reading latch values this execution never wrote).
			resources->frame.store(nullptr, std::memory_order_release);
			CommitUploads uploads(impl->commitStagedPool);
			// ---- The commit: the shared uploads (the material rows among them), the per-mode inputs, then per view its blocks
			// at its slot of the arena's head, its count buffer zeroed, and the view.
			auto& arena = payload.arena;
			bool staged = false;
			std::uint32_t stagedSlots = 0;
			TracyCZoneN(shadowCommitZone, "CS.DCLF.ShadowInputs.CommitShared", true);
			const auto inputsStart = std::chrono::steady_clock::now();
			// The worker's build staged what does not depend on the views (StageShadowPayload): one submission,
			// ahead of this commit's own uploads. A build made here, or staged against resources since recreated,
			// is uploaded from its vectors.
			staged = useAsync && payload.staged && payload.stagedFor == resources.get();
			stagedSlots = staged ? payload.stagedSlots : 0;
			if (staged) {
				org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(payload.staged));
			} else {
				payload.objects.Emit(resources->tablesHeld.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->objects, a_data, a_bytes, a_offset);
				});
				EmitBones(payload.bones, resources->tablesHeld.bones, nullptr, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->bones, a_data, a_bytes, a_offset);
				});
				EmitGeometryDraws(payload.geometries, resources->tablesHeld.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->geometries, a_data, a_bytes, a_offset);
				});
				payload.materialRows.Emit(resources->materialRowsHeld, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->materialRows.buffer, a_data, a_bytes, a_offset);
				});
			}
			// The rows the table holds (none past its capacity, which the build left waiting), and what the next frame's
			// Reserve grows it to. A build without the kept state wrote them whole: the table holds no journal version.
			resources->materialRowsHeld = payload.kept ? payload.materialRows.Version() : 0;
			impl->shadowRowsWanted = payload.rowsWanted;
			shadowStats.waitingRows = payload.waitingRows;
			// Either path uploaded the object records, the bone rows and the geometry slots' draws the buffers did not hold.
			if (payload.objects.Version())
				resources->tablesHeld.objects = payload.objects.Version();
			if (payload.geometries.Version())
				resources->tablesHeld.geometries = payload.geometries.Version();
			if (PersistentParityEnabled() && payload.bones.Version()) {
				auto& bonesStore = impl->shadowBones;
				EmitBones(payload.bones, resources->tablesHeld.bones, &bonesStore, [](const void*, std::size_t, std::size_t) {});
				if (ParityDue(payload.inputs.frameNumber))
					CheckBones(bonesStore, payload.bones);
			}
			if (payload.bones.Version())
				resources->tablesHeld.bones = payload.bones.Version();
			shadowStats.faceUploads += UploadFaceStreams(payload.faceStreams, resources->facePositions, resources->faceUploaded, uploads);
			shadowStats.records = static_cast<std::uint32_t>(payload.materialRows.Count());
			shadowStats.skippedTexture = payload.skippedTexture;
			shadowStats.skippedPipeline = payload.skippedPipeline;
			shadowStats.deferredTextures = payload.deferredTextures;
			shadowStats.deferredPipelines = payload.deferredPipelines;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!modeUsed[m])
					continue;
				if (!staged)
					EmitShadowInputs(payload, m, payload.kept ? resources->inputsUploaded[m] : 0,
						[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { uploads(resources->inputs[m], a_data, a_bytes, a_offset); });
				// Either path wrote what the buffer did not hold; a build without the kept state wrote it whole, which no
				// version of the kept state is.
				resources->inputsUploaded[m] = payload.kept ? payload.regionInputs[m].Version() : 0;
				shadowStats.inputs = static_cast<std::uint32_t>(payload.ModeInputs(m));
			}
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
			TracyCZoneEnd(shadowCommitZone);

			TracyCZoneN(shadowViewsZone, "CS.DCLF.ShadowInputs.BuildViews", true);
			const auto blocksStart = std::chrono::steady_clock::now();
			auto frame = std::make_shared<ShadowFrame>();
			const auto& previousShape = resources->published;
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			resources->labels.clear();
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = indirect;
			std::uint32_t mapRowsWritten = 0;  // bit per view rasterizer state whose map row is in the latch
			for (std::uint32_t slot = 0; slot < pending.size(); ++slot) {
				const auto& view = pending[slot];
				const std::uint64_t viewBlockOffset = std::uint64_t(slot) * kShadowViewSlotBytes;
				const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
				std::memcpy(arena.At(viewBlockOffset, sizeof(view.viewBlock)).data(), view.viewBlock, sizeof(view.viewBlock));
				std::memcpy(arena.At(perFrameOffset, view.perFrameBytes).data(), view.perFrame.data(), view.perFrameBytes);
				if (slot >= stagedSlots)
					uploads(resources->count[slot], kZeroCounts, sizeof(kZeroCounts), 0);
				const auto inputCount = static_cast<std::uint32_t>(payload.ModeInputs(view.modeIndex));
				// The view's values into its latch: frustum culling alone (mode 1), the single phase, and no
				// engine-visibility gate - a caster is drawn whether or not the main camera kept it.
				auto latch = ShadowViewLatch(view, inputCount, frameNumber);
				latch.cullFlags = (view.hasViewProj ? (1u | (view.renderMode == 0xE ? kCullNoNearPlane : 0u)) : 0u) |
				                  (view.casterClass ? kCullVolumetricOnly : kCullCastersOnly) | (view.sunView ? kCullSunEntry : 0u);
				latch.cullPlaneMask = view.cullPlaneMask;
				std::memcpy(latch.cullPlanes, view.cullPlanes, sizeof(latch.cullPlanes));
				// A sun view's entry rule on the GPU: the frame's full-frustum processes (six at the sun's usual setup), which
				// BuildDraws tests every input's entry sphere against (SetSunEntryRow).
				const auto& entryMasks = payload.inputs.sunEntryPlaneMasks;
				if (view.sunView && entryMasks.size() <= kMaxSunEntryProcesses) {
					latch.sunEntryCount = static_cast<std::uint32_t>(entryMasks.size());
					for (std::size_t process = 0; process < entryMasks.size(); ++process) {
						latch.sunEntryMasks[process] = entryMasks[process];
						for (std::uint32_t plane = 0; plane < 6; ++plane)
							std::memcpy(latch.sunEntryPlanes[process][plane], payload.inputs.sunEntryPlanes[process * 6 + plane].data(), 4 * sizeof(float));
					}
					// CS_DCLF_PERSISTENT_PARITY: the GPU's test on the latch as written, against the CPU's verdict, per input.
					if (PersistentParityEnabled() && ParityDue(frameNumber)) {
						const auto& tablesNow = store.GetTables();
						for (const auto& input : payload.Flat(view.modeIndex)) {
							const bool cpu = OutsideSunEntry(payload.inputs, tablesNow, input.objectIndex);
							++shadowStats.sunEntryChecks;
							shadowStats.sunEntryMismatches += cpu != OutsideSunEntryLatch(latch, input.fade) ? 1 : 0;
						}
					}
				}
				const bool rowWritten = (mapRowsWritten >> view.rasterState) & 1;
				mapRowsWritten |= 1u << view.rasterState;
				UseShadowMapRow(*resources, store.GetLookups(), latchSlot, view.rasterState, !rowWritten, latch);
				resources->latch->WriteValue(latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				resources->labels.push_back({ view.viewId, view.renderMode, slot });
				std::uint32_t previousCapacity = 0;
				if (previousShape)
					for (const auto& previous : previousShape->views)
						if (previous.slot == slot)
							previousCapacity = previous.capacity;
				frame->views.push_back(FrameViewOf(view, slot, view.modeIndex, view.targetIndex, GrowCapacity(previousCapacity, inputCount, kMaxDraws),
					*resources, payload));
			}
			// Staged, only the view head this epoch wrote goes up from here; the rest of the arena is the worker's.
			const auto& bytes = arena.Bytes();
			const std::size_t arenaBytes = staged ? std::min<std::size_t>(bytes.size(), pending.size() * kShadowViewSlotBytes) : bytes.size();
			if (arenaBytes)
				uploads(resources->constants, bytes.data(), arenaBytes, 0);
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			static std::uint32_t logged = 0;
			if (logged++ % 600 == 0) {
				std::string views;
				for (std::size_t v = 0; v < frame->views.size() && v < pending.size(); ++v) {
					const auto& view = frame->views[v];
					views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", pending[v].viewId,
						pending[v].renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.ModeInputs(view.modeIndex), view.capacity);
				}
				logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} material rows ({} waiting for the table to grow, {} held): {}",
					frame->views.size(), shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, payload.waitingRows,
					resources->materialRows.capacity, views);
			}
			PublishShape(std::move(frame), resources->published, resources->frame, resources->shapeGenerations);
			TracyCZoneEnd(shadowViewsZone);
		}, frameOwners);
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own compile, prepare and record
		if (ok) {
			++shadowStats.epochs;
			shadowStats.viewsDrawn += static_cast<std::uint32_t>(pending.size());
			// Skylighting's occluders are uploaded: its map can be DCLF's this frame if none was left out.
			if (modeUsed[kSkyMode]) {
				impl->skyCommittedFrame = frameNumber;
				impl->skyInputs = static_cast<std::uint32_t>(payload.ModeInputs(kSkyMode));
				impl->skySkipped = payload.skySkipped;
			}
			impl->ReadShadowCullCounters(frameNumber, shadowStats);
			// Static shadow ownership: what this frame's epoch drew for a mode is what that mode's views'
			// registrations are withheld for, from the next frame on.
			if (PassCapture::ShadowWithholdingEnabled()) {
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (modeUsed[m] && m != kSkyMode)
						impl->PublishShadowClaims(PassCapture::kFirstShadowMode + m, payload.inputList[m], usedWorkerBuild || payload.kept ? payload.claims[m] : nullptr,
							shadowStats);
				}
				// The cascades' claims decide which sun entries the next frame's cascade culls may skip.
				if (modeUsed[kSunShadowMode])
					SunAccumulation::Get().PublishExclusion(usedWorkerBuild ? payload.sunExclusion :
					                                                          BuildSunExclusion(payload.inputs.sunCandidates, payload, kSunShadowMode, SceneStore::Get().GetTables(), &impl->sunExclusionCache));
			}
			pending.clear();
		} else {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			notReady(ShadowNotReady::Epoch);
		}
		shadowStats.cpuMs += totalMs;
	}

	void IndirectDraws::Impl::ReserveShadowRows()
	{
		// What the last build wanted, with a quarter more: the table grows ahead of the scene, not a frame behind it.
		if (!shadow || !shadowRowsWanted)
			return;
		if (shadow->materialRows.Reserve(shadowRowsWanted + shadowRowsWanted / 4))
			shadow->materialRowsHeld = 0;  // a new backing holds nothing
	}

	ShadowInputs IndirectDraws::Impl::PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<std::uint32_t, kShadowModeCount>& a_modeRasterStates) const
	{
		ZoneScopedN("CS.DCLF.PrepareShadowInputs");
		ShadowInputs in;
		in.frameNumber = a_store.GetFrame();
		in.modeUsed = a_modeUsed;
		in.modeRasterStates = a_modeRasterStates;
		// The sun's full-frustum planes, read on the render thread after the full-frustum cull has run
		// (NiCamera::CalculateAndDrawShadowCasterLights precedes both the build's kick and the views).
		if (auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr)
			if (auto* sun = node->GetRuntimeData().sunShadowDirLight)
				for (const auto& process : sun->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray) {
					if (!process)
						continue;
					const auto& planes = process->planes;
					for (std::uint32_t p = 0; p < 6; ++p)
						in.sunEntryPlanes.push_back({ planes.cullingPlanes[p].normal.x, planes.cullingPlanes[p].normal.y, planes.cullingPlanes[p].normal.z,
							planes.cullingPlanes[p].constant });
					in.sunEntryPlaneMasks.push_back(planes.activePlanes.underlying() & 0x3Fu);
				}
		in.sunCandidates = a_store.GetSunCandidates();
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.records = a_resources.materialRows.address;
		in.addresses.objectsIndex = a_resources.objectsIndex;
		in.addresses.bonesIndex = a_resources.bonesIndex;
		in.addresses.facePositions = FaceSnapshots::Enabled() ? a_resources.facePositionsAddress : 0;
		in.addresses.recordCapacity = a_resources.materialRows.capacity;
		in.addresses.identity = &a_resources;
		in.tablesGeneration = a_store.GetTablesGeneration();
		in.lookupGeneration = a_store.GetLookups().generation;
		in.tablesHeld = a_resources.tablesHeld;
		in.inputsHeld = a_resources.inputsUploaded;
		in.materialRowsHeld = a_resources.materialRowsHeld;
		if (auto* csState = globals::state) {
			const auto* shared = reinterpret_cast<const std::byte*>(&csState->lastSharedData);
			in.sharedData.assign(shared, shared + sizeof(State::SharedDataCB));
			if (!csState->lastFeatureData.empty()) {
				const auto* feature = reinterpret_cast<const std::byte*>(csState->lastFeatureData.data());
				in.featureData.assign(feature, feature + csState->lastFeatureData.size());
			}
		}
		return in;
	}

	void IndirectDraws::KickShadowBuild()
	{
		// The shadow epoch's inputs are final from here to AfterShadowMaps: the scene phase has just built the
		// tables and the frame's reference eye is set. The build runs on the worker while the engine draws
		// the shadow maps, for last frame's render modes (a change is stale, and built inline).
		impl->DropShadowJob(stats);
		if (!ActiveToggles().shadows || failed || !AsyncEnabled())
			return;
		auto& async = stats.async[kAsyncShadow];
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		auto& job = impl->shadowJob;
		const bool tablesReady = !tables.objects.empty() && tables.shadowTechnique.size() == tables.objects.size();
		if (!impl->shadow || !job.modesKnown || !DrawPipelines::Get().Enabled() || !globals::game::utilityShader || !tablesReady) {
			++async.notKicked;
			return;
		}
		// The material rows' table grows here, on the render thread before the worker reads it, if the last build wanted more.
		impl->ReserveShadowRows();
		job.inputs = impl->PrepareShadowInputs(store, *impl->shadow, job.modes, job.rasterStates);
		++async.kicked;
		const auto* tablesPtr = &tables;
		const auto* lookups = &store.GetLookups();
		auto* payload = &impl->shadowPayload;
		auto* pool = &job.stagedPool;
		auto* objects = impl->ShadowObjects();
		auto* bonesStore = impl->ShadowBones();
		auto* geometriesStore = impl->ShadowGeometries();
		auto* exclusionCache = &impl->sunExclusionCache;
		auto* kept = impl->ShadowKeptState();
		const ShadowInputs inputs = job.inputs;
		const bool claims = PassCapture::ShadowWithholdingEnabled();
		job.handle = AsyncWorker::Get().Submit("shadow", [inputs, tablesPtr, lookups, payload, pool, objects, bonesStore, kept, geometriesStore, exclusionCache, target = impl->shadow, slots = job.views, claims](std::stop_token) {
			BuildShadowPayload(inputs, *tablesPtr, *lookups, *payload, objects, bonesStore, kept, geometriesStore);
			StageShadowPayload(*payload, *target, slots, *pool);
			if (claims) {
				ZoneScopedN("CS.DCLF.BuildShadow.Claims");
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					if (inputs.modeUsed[m] && m != kSkyMode && !payload->kept)
						payload->claims[m] = ShadowClaimSet(payload->inputList[m], *tablesPtr);
				if (inputs.modeUsed[kSunShadowMode])
					payload->sunExclusion = BuildSunExclusion(inputs.sunCandidates, *payload, kSunShadowMode, *tablesPtr, exclusionCache);
			}
		});
	}

	void IndirectDraws::Impl::DropShadowJob(IndirectDraws::Stats& a_stats)
	{
		if (!shadowJob.handle)
			return;
		AsyncWorker::Get().Cancel(shadowJob.handle);
		++a_stats.async[kAsyncShadow].dropped;
		shadowJob.handle = {};
	}

	void IndirectDraws::Impl::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
	{
		auto* context = globals::d3d::context;
		if (shadowCullReadback) {
			// Frames, not epochs: several views run per frame, and the copy needs the GPU to have finished
			// the sampled view's epoch, which three Presents later it has.
			if (a_frame - shadowCullReadback->copiedFrame < 3)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(shadowCullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullSampledView = shadowCullReadback->view;
				a_stats.cullSampledMode = shadowCullReadback->mode;
				context->Unmap(shadowCullReadback->count.get(), 0);
			}
			shadowCullReadback.reset();
			return;
		}
		// One epoch in 127, and within it the views in turn, so every view of the frame gets sampled.
		const auto epoch = shadowCullEpochs++;
		if ((epoch % 127) != 0 || !shadow || shadow->labels.empty())
			return;
		const auto& view = shadow->labels[(epoch / 127) % shadow->labels.size()];
		if (view.slot >= kMaxShadowViews || !shadow->countD3D11[view.slot])
			return;
		D3D11_BUFFER_DESC desc{};
		shadow->countD3D11[view.slot]->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ShadowCullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		ScopedPerfEvent event("CS DCLF: shadow culling readback");
		context->CopyResource(readback.count.get(), shadow->countD3D11[view.slot].get());
		readback.copiedFrame = a_frame;
		readback.view = view.viewId;
		readback.mode = view.renderMode;
		shadowCullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::PublishShadowClaims(std::uint32_t a_renderMode, const std::vector<DrawInput>& a_inputs, std::shared_ptr<const PassCapture::ClaimSet> a_built,
		IndirectDraws::ShadowStats& a_stats)
	{
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + PassCapture::kShadowModes)
			return;
		const auto start = std::chrono::steady_clock::now();
		const auto modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		// The worker's set when its build was the one drawn, else built here from the same inputs.
		auto claims = a_built ? std::move(a_built) : ShadowClaimSet(a_inputs, SceneStore::Get().GetTables());
		a_stats.claimed[modeIndex] = static_cast<std::uint32_t>(claims->size());
		PassCapture::Get().PublishShadowClaims(modeIndex, std::move(claims));
		a_stats.claimMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}
}

#endif
