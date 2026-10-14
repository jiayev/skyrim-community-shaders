// The shadow and occlusion epochs (render thread): their commits (the installed payload, the index pool, the views' latches,
// the arena and the latched CS blocks), the epochs' inputs and the culling counters' readback.

#include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws/Epochs/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"

namespace DCLF
{
	namespace Draws
	{
		/**
		 * @brief A shadow view's common latch: a single phase over every input (no engine-visibility gate), the eye-folded
		 * view-projection and the frame's stamp. Culling flags and planes are the caller's.
		 */
		BuildDrawsLatch ShadowViewLatch(const PendingView& a_view, std::uint32_t a_inputs, std::uint32_t a_frame)
		{
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (a_inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = a_inputs;
			latch.visibilityStamp = a_frame & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			latch.fadeSeedsIndex = FrameValues::Get().FadeSeedsIndex();
			FoldEyeIntoViewProj(a_view.viewProj, a_view.eye, latch.viewProj);
			return latch;
		}

		/**
		 * @brief Points a_latch at a view's bucket table in its slot's latch region, writing the table when a_write (the region does not
		 * hold this revision's yet: LatchRegionsHeld).
		 */
		void WriteBucketTable(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, std::uint32_t a_latchSlot, std::uint32_t a_slot,
			std::span<const ShadowBucket> a_buckets, bool a_write, BuildDrawsLatch& a_latch)
		{
			// At most a bucket per key slot, which the layout reserves (ReserveShadowLatch).
			if (a_buckets.size() > a_layout.keySlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} shadow buckets past the latch's {} key slots", a_buckets.size(), a_layout.keySlots));
			const std::uint32_t offset = a_layout.BucketOffset(a_slot);
			a_latch.bucketTableOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchSlot)) + offset;
			if (!a_write || a_buckets.empty())
				return;
			std::vector<std::uint32_t> table;
			table.reserve(a_buckets.size() * 2);
			for (const auto& bucket : a_buckets) {
				table.push_back(bucket.first);
				table.push_back(bucket.capacity);
			}
			LatchWrite(a_latchBlock, "shadow bucket tables", a_latchSlot, offset, std::as_bytes(std::span(table)));
		}

		// The shadow latch's regions the shadow commits wrote their revision's bucket tables and map rows into (render thread).
		// Parts: 2 * view slot (its bucket table), 2 * map row + 1 (the row). The occlusion epoch's slots and modes are its own.
		LatchRegionsHeld shadowLatchRegions;
		// What the shadow constants buffer holds of the arena past the frame record, as staged (render thread): the build it was last
		// brought up to, and its bytes. Only these commits write that part of the buffer.
		struct ShadowArenaHeld
		{
			std::weak_ptr<const void> target;
			std::uint64_t serial = 0;
			std::vector<std::byte> bytes;  // from the record's end
		} shadowArenaHeld;
		// A new build's arena mostly repeats the last one's: it is compared in chunks of this many bytes, and only differing runs staged.
		constexpr std::size_t kArenaChunk = 256;

		/**
		 * @brief Points a_latch at its rasterizer state's pipeline-map row (each key slot's bucket), writing the row when a_write
		 * (once per state and frame). The offset is into the whole latch block, so it includes the frame slot's base.
		 */
		void UseShadowMapRow(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const RowBuckets& a_buckets, std::uint32_t a_latchSlot,
			std::uint32_t a_mode, std::uint32_t a_rasterState, bool a_write, BuildDrawsLatch& a_latch)
		{
			const auto& layout = a_layout;
			// ReserveShadowLatch gives every registered state a row in every mode: past it is a defect of that reserve.
			if (a_rasterState == 0 || a_rasterState > layout.rasterStates || a_mode >= kShadowModeCount)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: shadow view rasterizer state {} of mode {} past the latch's {} rows", a_rasterState, a_mode,
					layout.rasterStates));
			const std::uint32_t mapRowOffset = layout.MapOffset() + layout.MapRowOf(a_mode, a_rasterState) * layout.MapRowBytes();
			a_latch.pipelineMapOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchSlot)) + mapRowOffset;
			if (!a_write)
				return;
			// ReserveShadowLatch sized the row for every key slot.
			const auto& row = a_buckets.bucketOfSlot;
			if (row.size() > layout.keySlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} shadow key slots past the latch's {}", row.size(), layout.keySlots));
			if (!row.empty())
				LatchWrite(a_latchBlock, "shadow map rows", a_latchSlot, mapRowOffset, std::as_bytes(std::span(row)));
		}

		/** @brief Writes the sun's full-frustum processes into the frame slot's latch region; returns their offset into the block. */
		std::uint32_t WriteSunEntryRegion(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, std::uint32_t a_latchSlot,
			std::span<const SunEntryProcess> a_processes)
		{
			const auto& layout = a_layout;
			// ReserveShadowLatch sized the region for every process.
			if (a_processes.size() > layout.sunProcesses)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} sun full-frustum processes past the latch's {}", a_processes.size(), layout.sunProcesses));
			const std::uint32_t header[4] = { static_cast<std::uint32_t>(a_processes.size()), 0, 0, 0 };
			LatchWrite(a_latchBlock, "shadow sun processes", a_latchSlot, layout.SunEntryOffset(), std::as_bytes(std::span(header)));
			if (!a_processes.empty())
				LatchWrite(a_latchBlock, "shadow sun processes", a_latchSlot, layout.SunEntryOffset() + kSunRegionHeader, std::as_bytes(a_processes));
			return static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchSlot)) + layout.SunEntryOffset();
		}

		/** @brief Writes a view's view and per-frame blocks into its latch region, for ShadowLatchedCopiesPass to copy. */
		void WriteViewBlocks(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, std::uint32_t a_latchSlot, std::uint32_t a_slot,
			const PendingView& a_view)
		{
			const std::uint32_t offset = a_layout.ViewBlockOffset(a_slot);
			LatchWrite(a_latchBlock, "shadow view blocks", a_latchSlot, offset, std::as_bytes(std::span(a_view.viewBlock)));
			// PerTechnique c3, DCLFDepthRange (Utility.hlsl): the draws apply the depth range under a [0, 1] viewport, so a change
			// in it changes no recording.
			const float depthRange[4] = { a_view.minDepth, a_view.maxDepth - a_view.minDepth, 0.0f, 0.0f };
			LatchWrite(a_latchBlock, "shadow view blocks", a_latchSlot, offset + static_cast<std::uint32_t>(sizeof(a_view.viewBlock)), std::as_bytes(std::span(depthRange)));
			LatchWrite(a_latchBlock, "shadow per-frame view data", a_latchSlot, offset + static_cast<std::uint32_t>(kShadowPerFrameOffset), std::span(a_view.perFrame.data(), a_view.perFrameBytes));
		}
	}

	bool IndirectDraws::OcclusionReady(std::uint32_t a_view) const
	{
		// Ready when this frame's shadow commit uploaded the occluders and the map's target is imported.
		if (a_view >= kOcclusionViews || failed || !ActiveToggles().shadows || !OcclusionEnabled(a_view) || !impl->shadow)
			return false;
		const auto& occlusion = impl->occlusion[a_view];
		// Not while the built graph lacks the map's import, nor without the selected revision's recorded shape for the maps' layout
		// (the engine draws the map meanwhile).
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || host->RebuildRequested())
			return false;
		const auto layouts = impl->PredictedOcclusion();
		bool shaped = false;
		if (impl->EpochCovered(Epoch::Occlusion))
			if (const auto& active = impl->revisions.active)
				if (const auto& fragment = active->Fragment(Impl::SceneRevisions::kShapeSlot + Epoch::Occlusion))
					if (const auto variants = fragment->Value<ShadowVariants>())
						for (const auto& shape : variants->shapes)
							shaped = shaped || (shape && LayoutOf(*shape) == layouts);
		if (!shaped) {
			++impl->occlusionUnrecorded;
			return false;
		}
		return occlusion.rasterState && occlusion.committedFrame == Scene::FrameView::Get().GetFrame() && impl->shadow->depth[OcclusionDepthTarget(a_view)];
	}

	std::uint32_t IndirectDraws::ExecuteOcclusion(std::uint32_t a_views)
	{
		ZoneScopedN("CS.DCLF.ExecuteOcclusion");
		const auto start = std::chrono::steady_clock::now();
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		// The requested views that are ready and captured this frame; a map the engine did not draw draws nothing.
		std::uint32_t drawable = 0;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (!(a_views & (1u << v)))
				continue;
			if (OcclusionReady(v) && impl->occlusion[v].capturedFrame == frameNumber)
				drawable |= 1u << v;
			else
				++shadowStats.occlusionNotReady[v];
		}
		const auto indirect = FrameShadowIndirectState();
		if (!drawable || !indirect.valid) {
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				shadowStats.occlusionNotReady[v] += (drawable & (1u << v)) ? 1 : 0;
			return 0;
		}
		ScopedPerfEvent event("CS DCLF: occlusion maps (CPU)");
		auto resources = impl->shadow;
		// The revision's variant for the drawable maps and the payload the shadow commit read: trusted, nothing is checked here.
		std::vector<ShadowViewLayout> layouts;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (drawable & (1u << v))
				layouts.push_back(LayoutOf(impl->occlusion[v].view, OcclusionSlot(v), OcclusionModeOf(v), OcclusionDepthTarget(v)));
		const auto revision = impl->RevisionOf(Epoch::Occlusion);
		std::shared_ptr<const ShadowFrame> revisionShape;
		std::size_t revisionVariant = 0;
		if (revision)
			if (const auto variants = revision.shape->Value<ShadowVariants>())
				for (std::size_t v = 0; v < variants->shapes.size() && !revisionShape; ++v)
					if (variants->shapes[v] && LayoutOf(*variants->shapes[v]) == layouts) {
						revisionShape = variants->shapes[v];
						revisionVariant = v;
					}
		const auto committed = impl->committedShadow;
		if (!revisionShape || !committed) {
			// A layout the revision lacks (one map drawn without the other): the engine draws them.
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				shadowStats.occlusionNotReady[v] += (drawable & (1u << v)) ? 1 : 0;
			return 0;
		}
		const auto& payload = *committed;
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::SkyOcclusion, [&](org::RenderGraph&) {
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			const ShadowFrame& target = *revisionShape;
			const auto& latchBlock = *target.latch;
			const auto& latchLayout = target.latchLayout;
			auto& scene = *resources->scene;
			std::size_t index = 0;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				if (!(drawable & (1u << v)))
					continue;
				const auto& view = impl->occlusion[v].view;
				const auto& shapeView = target.views[index];
				const auto& buckets = (*target.rows)[index++];
				const std::uint32_t slot = OcclusionSlot(v), mode = OcclusionModeOf(v);
				const auto inputCount = static_cast<std::uint32_t>(payload.Inputs());  // the whole shadow list
				// Occluders, material rows, frame record, objects and geometries are the installed payload's, which the shadow commit read
				// this frame (OcclusionReady).
				WriteViewBlocks(latchBlock, latchLayout, latchSlot, slot, view);
				// Frustum culling (near plane included), Skylighting::OcclusionTechnique's size test (bound radius above 32), and fade
				// roots as the engine's cull leaves them (kCullFadeOnVisible): never a root the main camera faded out or has not seen yet.
				auto latch = ShadowViewLatch(view, inputCount, frameNumber);
				static const REL::Relocation<const std::uint8_t*> fadesOn{ REL::Offset(Engine::Address::kFadesOn) };
				latch.cullFlags = 1u | kCullMinRadius | (*fadesOn.get() ? kCullFadeOnVisible : 0u) | KeyWordOf(mode);
				latch.viewBits = ShadowViewBits(mode, false);
				latch.fadeStatesIndex = scene.FadeStatesReadIndex(impl->Adopted(), frameNumber);
				ShadowPayloadLatch(*impl->InstalledPayload(), latch);
				UseShadowMapRow(latchBlock, latchLayout, buckets, latchSlot, mode, view.rasterState, true, latch);
				WriteBucketTable(latchBlock, latchLayout, latchSlot, slot, shapeView.buckets, true, latch);
				LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				shadowStats.occlusionInputs[v] = inputCount;
				++shadowStats.occlusionDrawn[v];
			}
			impl->SubmitRevisionRecording(Epoch::Occlusion, *revision.recordings, revisionVariant);
		}, impl->shadowExecutionOwner);
		shadowStats.occlusionMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++shadowStats.occlusionEpochs;
		if (!ok) {
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				if (drawable & (1u << v)) {
					--shadowStats.occlusionDrawn[v];
					++shadowStats.occlusionNotReady[v];
				}
			return 0;
		}
		return drawable;
	}

	void IndirectDraws::ExecuteShadowFrame()
	{
		ZoneScopedN("CS.DCLF.ExecuteShadowFrame");
		auto& pending = impl->pendingViews;
		if (!ActiveToggles().shadows || failed || pending.empty()) {
			pending.clear();
			return;
		}
		ScopedPerfEvent event("CS DCLF: shadow views (CPU)");
		const auto start = std::chrono::steady_clock::now();
		auto notReady = [&](ShadowNotReady a_reason) {
			shadowStats.notReady += static_cast<std::uint32_t>(pending.size());
			shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)] += static_cast<std::uint32_t>(pending.size());
			if (!pending.empty())
				impl->ShadowsNotDrawn(shadowStats);
			pending.clear();
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !impl->shadow || impl->shadowPlacements.empty())
			return notReady(ShadowNotReady::Setup);
		const auto indirect = FrameShadowIndirectState();
		if (!indirect.valid) {
			impl->ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto resources = impl->shadow;
		std::array<bool, kShadowModeCount> modeUsed{};
		// Inputs and pipelines cover every catalog state per mode (UpdateShadowCapability), so which views a frame draws (cascades
		// alternate depth-bias states, point-light views come and go) changes nothing the build reads. An unforeseen mode or state
		// is a defect, added to the catalog now.
		for (const auto& view : pending) {
			modeUsed[view.modeIndex] = true;
			if (!impl->readyModes[view.modeIndex]) {
				impl->readyModes[view.modeIndex] = true;
				impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("a view of render mode {:#x}", view.renderMode));
			}
			if (const auto& states = impl->readyStates[view.modeIndex].Of(view.casterClass != 0); !std::binary_search(states.begin(), states.end(), view.rasterState)) {
				impl->readyStates[view.modeIndex].Add(view.rasterState, view.casterClass != 0);
				impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("view {} (mode {:#x}) draws with rasterizer state {}, not in the catalog", view.viewId, view.renderMode, view.rasterState));
			}
		}
		// The occlusion epoch draws later from this build, whether or not the engine draws the map this frame.
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (OcclusionEnabled(v) && impl->readyModes[OcclusionModeOf(v)] && impl->occlusion[v].dsvFormat != DXGI_FORMAT_UNKNOWN)
				modeUsed[OcclusionModeOf(v)] = true;
		std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			if (modeUsed[m])
				modeRasterStates[m] = impl->readyStates[m];
		// One pipeline set per shadow map format: every engine target is D16.
		const DXGI_FORMAT dsvFormat = pending.front().dsvFormat;
		if (dsvFormat != impl->shadowTargetFormat) {
			impl->shadowTargetFormat = dsvFormat;
			impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("a view draws into format {}", static_cast<int>(dsvFormat)));
		}
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0;

		ShadowInputs in = impl->PrepareShadowInputs(*resources, modeUsed, modeRasterStates);
		// The revision's shape and recording and the installed payload: trusted, nothing is checked here. The epoch covers every
		// placement; a slot without a view does no work (zero latch, counters zeroed, its render pass loads and stores the engine's).
		const std::vector<ShadowViewLayout>& layouts = impl->shadowPlacements;
		const auto revision = impl->RevisionOf(Epoch::Shadow);
		const std::shared_ptr<ShadowPayload> ahead = impl->installedDraws ? impl->installedDraws->shadow : nullptr;
		if (!revision || !ahead || ahead->inputs.addresses.identity != in.addresses.identity) {
			if (revision && impl->shadowUnbuilt++ == 0)
				logger::warn("[DCLF] the shadow epoch of frame {} has no payload for its resources in the installed publication: not submitted <- UNBUILT", frameNumber);
			return notReady(ShadowNotReady::Epoch);
		}
		const auto revisionShape = revision.shape->Value<ShadowVariants>()->shapes.front();
		const ShadowFrame& target = *revisionShape;
		// Clamped to the latch the producer sized (ReserveShadowLatch).
		if (in.sunEntryProcesses.size() > target.latchLayout.sunProcesses) {
			++impl->revisions.latchClamped[Epoch::Shadow];
			in.sunEntryProcesses.resize(target.latchLayout.sunProcesses);
		}
		ShadowPayload* committed = ahead.get();
		auto& async = stats.async[kAsyncShadow];
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

			TracyCZoneN(shadowPrepareZone, "CS.DCLF.ShadowInputs.Prepare", true);
			const auto prepareStart = std::chrono::steady_clock::now();
			++async.used;
			// The publication was built before the frame's full-frustum cull.
			ahead->inputs.sunEntryProcesses = in.sunEntryProcesses;
			impl->committedShadow = ahead;
			ShadowPayload& payload = *ahead;
			// The payload's buffers are the installed publication's (a covered frame has a ready one, with its shadow parts); only the
			// arena is uploaded here. The occlusion epoch reads the same payload.
			const PayloadVersions& parts = *impl->InstalledPayload();
			// Copied: a frame that keeps the publication commits the payload again.
			*frameOwners = payload.bindingOwners;
			// The adopted snapshot lives until this execution (and the occlusion epoch's, which shares these owners) retires.
			if (impl->adoptedSnapshot)
				frameOwners->push_back(impl->adoptedSnapshot);
			// Cleared geometry slots' buffers, held until this execution retires.
			for (auto& owner : Scene::FrameView::Get().TakeRetiredImports())
				frameOwners->push_back(std::move(owner));
			impl->shadowExecutionOwner = frameOwners;
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
			TracyCZoneEnd(shadowPrepareZone);

			// The adopted set: the versions the commit writes into, and the SRV indices it writes.
			const auto& adopted = impl->Adopted();
			CommitUploads uploads(impl->commitStagedPool, adopted);
			auto& arena = payload.arena;
			TracyCZoneN(shadowCommitZone, "CS.DCLF.ShadowInputs.CommitShared", true);
			const auto inputsStart = std::chrono::steady_clock::now();
			auto& scene = *resources->scene;
			shadowStats.faceUploads += UploadFaceStreams(adopted, payload.faceStreams, scene.facePositions, scene.faceUploaded, uploads);
			UploadTrees(adopted, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetFrame(), scene, uploads, false);
			shadowStats.records = static_cast<std::uint32_t>(payload.materialRows.Count());
			shadowStats.skippedTexture = payload.skippedTexture;
			shadowStats.skippedPipeline = payload.skippedPipeline;
			shadowStats.deferredTextures = payload.deferredTextures;
			shadowStats.deferredPipelines = payload.deferredPipelines;
			std::size_t casterInputs = 0;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				if (modeUsed[m])
					casterInputs += payload.ModeInputs(m);
			shadowStats.inputs = static_cast<std::uint32_t>(casterInputs);
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
			TracyCZoneEnd(shadowCommitZone);

			TracyCZoneN(shadowViewsZone, "CS.DCLF.ShadowInputs.BuildViews", true);
			const auto blocksStart = std::chrono::steady_clock::now();
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			resources->labels.clear();
			const auto& latchBlock = *target.latch;
			const auto& latchLayout = target.latchLayout;
			// The index pool both this epoch and the occlusion epoch draw from.
			{
				ZoneScopedN("CS.DCLF.ShadowInputs.IndexPool");
				UpdateIndexPool(adopted, *resources->pool, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetTablesGeneration(), latchBlock, latchSlot,
					latchLayout.PoolOffset(), uploads);
			}
			// The revision's bucket tables and map rows: written into this frame slot's region only where it does not hold them yet.
			auto& held = Draws::shadowLatchRegions.Of(latchSlot, target.latch, revisionShape, 0);
			std::uint32_t sunEntryOffset = 0;  // the slot's sun entry region, once written
			std::vector<bool> slotDrawn(layouts.size());
			for (const auto& view : pending) {
				ZoneScopedN("CS.DCLF.ShadowInputs.View");
				const std::uint32_t slot = view.slot;
				const std::uint32_t index = slot - kFirstShadowViewSlot;
				slotDrawn[index] = true;
				WriteViewBlocks(latchBlock, latchLayout, latchSlot, slot, view);
				// The whole shadow list: the view bits select the mode's inputs.
				const auto inputCount = static_cast<std::uint32_t>(payload.Inputs());
				// Frustum culling, no engine-visibility gate. Only class-split modes test the caster class (not the paraboloids).
				auto latch = ShadowViewLatch(view, inputCount, frameNumber);
				const std::uint32_t classFlags = view.casterClass ? kCullVolumetricOnly : view.modeIndex == kParabolicShadowMode ? 0u : kCullCastersOnly;
				latch.cullFlags = (view.hasViewProj ? (1u | (view.renderMode == 0xE ? kCullNoNearPlane : 0u)) : 0u) | classFlags |
				                  (view.sunView ? kCullSunEntry : 0u) | KeyWordOf(view.modeIndex);
				latch.viewBits = ShadowViewBits(view.modeIndex, view.sunView);
				// A focus view draws its target's members alone (root slot + 1; an unlisted target draws nothing).
				if (view.focus) {
					const auto& rootIndex = Scene::FrameView::Get().GetTables().fadeRootIndex;
					const auto root = view.focusTarget ? rootIndex.find(view.focusTarget) : rootIndex.end();
					latch.focusRoot = root != rootIndex.end() ? root->second + 1 : kFocusRootNone;
				}
				latch.cullPlaneMask = view.cullPlaneMask;
				std::memcpy(latch.cullPlanes, view.cullPlanes, sizeof(latch.cullPlanes));
				latch.fadeStatesIndex = resources->scene->FadeStatesReadIndex(adopted, frameNumber);
				ShadowPayloadLatch(parts, latch);
				// Sun entry rule: BuildDraws tests each input's entry sphere against the full-frustum processes (written once per frame).
				if (view.sunView) {
					if (!sunEntryOffset)
						sunEntryOffset = WriteSunEntryRegion(latchBlock, latchLayout, latchSlot, payload.inputs.sunEntryProcesses);
					latch.sunEntryOffset = sunEntryOffset;
				}
				const std::uint32_t mapRow = view.rasterState ? latchLayout.MapRowOf(view.modeIndex, view.rasterState) : ~0u;
				const bool rowHeld = mapRow < latchLayout.MapRows() && Draws::LatchRegionsHeld::Hold(held, std::size_t(mapRow) * 2 + 1);
				UseShadowMapRow(latchBlock, latchLayout, (*target.rows)[index], latchSlot, view.modeIndex, view.rasterState, !rowHeld, latch);
				WriteBucketTable(latchBlock, latchLayout, latchSlot, slot, target.views[index].buckets, !Draws::LatchRegionsHeld::Hold(held, std::size_t(slot) * 2), latch);
				LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				Diagnostics::Notify(Diagnostics::ShadowViewLatched{ view, latch, frameNumber, payload, shadowStats });
				resources->labels.push_back({ view.viewId, view.renderMode, slot });
			}
			// Placements without a view this frame: a zero latch, no work.
			for (std::size_t index = 0; index < layouts.size(); ++index) {
				if (slotDrawn[index])
					continue;
				LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, layouts[index].slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), BuildDrawsLatch{});
				++shadowStats.retainedViews;
			}
			// The arena, with the frame record patched to this frame's per-frame buffers (the build ahead has another frame's).
			// Uploaded around the record, not over it: copies within one batch are unordered.
			if (const auto& bytes = arena.Bytes(); !bytes.empty()) {
				DrawBindings record = payload.frameRecord;
				record.textures[kObjectBufferRegister] = parts[PayloadPart::Objects].srvIndex;
				record.textures[kExtrasBufferRegister] = parts[PayloadPart::Extras].srvIndex;
				record.textures[kPlacementBufferRegister] = in.addresses.placementsIndex;
				record.textures[kPaletteBufferRegister] = in.addresses.palettesIndex;
				record.textures[kShadingBufferRegister] = in.addresses.shadingIndex;
				record.textures[kTreeWindRegister] = resources->scene->TreeWindReadIndex(adopted, frameNumber);
				constexpr std::size_t recordEnd = kShadowFrameRecordOffset + sizeof(DrawBindings);
				if (bytes.size() < recordEnd)
					stl::report_and_fail("Drawcall Limit Fix: a shadow arena without its frame record");
				// The arena around the record is the build's: nothing is staged while the buffer holds this build's (a kept publication
				// commits the same build every frame), and of a new build only the chunks that differ from what the buffer holds. The
				// record is the frame's.
				auto& staged = Draws::shadowArenaHeld;
				const std::shared_ptr<const void> arenaTarget = resources->constants;
				const bool sameTarget = Draws::LatchRegionsHeld::Same(staged.target, arenaTarget);
				const bool arenaHeld = sameTarget && staged.serial == payload.arenaSerial;
				if (kShadowFrameRecordOffset && !arenaHeld)
					uploads(resources->constants, bytes.data(), kShadowFrameRecordOffset, 0);
				uploads(resources->constants, &record, sizeof(record), kShadowFrameRecordOffset);
				if (!arenaHeld) {
					const auto tail = std::span<const std::byte>(bytes).subspan(recordEnd);
					if (!sameTarget)
						staged.bytes.clear();
					const std::size_t compared = std::min(staged.bytes.size(), tail.size());
					std::size_t run = 0;  // the first differing chunk's start, while in a run
					bool inRun = false;
					for (std::size_t at = 0; at < tail.size(); at += Draws::kArenaChunk) {
						const std::size_t size = std::min(Draws::kArenaChunk, tail.size() - at);
						const bool differs = at + size > compared || std::memcmp(tail.data() + at, staged.bytes.data() + at, size) != 0;
						if (differs && !inRun) {
							run = at;
							inRun = true;
						} else if (!differs && inRun) {
							uploads(resources->constants, tail.data() + run, at - run, recordEnd + run);
							inRun = false;
						}
					}
					if (inRun)
						uploads(resources->constants, tail.data() + run, tail.size() - run, recordEnd + run);
					staged.bytes.assign(tail.begin(), tail.end());
					staged.target = arenaTarget;
					staged.serial = payload.arenaSerial;
				}
			}
			// CS's SharedData and FeatureData as of now (the water reflection prepasses refreshed them after the build), latched: the
			// epoch's first pass copies them after the arena.
			auto latched = LatchedUploads(target.latched[ShadowFrame::kCommitList], uploads, latchSlot);
			// At their fixed places (ShadowArenaBlocksOf); a size mismatch with CS's block is a contract violation.
			const auto blocks = ShadowArenaBlocksOf();
			const auto writeBlock = [&](std::uint64_t a_offset, std::uint32_t a_size, const std::vector<std::byte>& a_bytes) {
				if (!a_size)
					return;
				if (a_bytes.size() != a_size)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: a shadow commit's CS block of {} bytes for its place of {}", a_bytes.size(), a_size));
				latched(resources->constants, a_bytes.data(), a_bytes.size(), a_offset);
			};
			writeBlock(blocks.sharedData, blocks.sharedBytes, in.sharedData);
			writeBlock(blocks.featureData, blocks.featureBytes, in.featureData);
			(void)latched.Finish();
			impl->revisions.latchedMisses += latched.Misses();
			impl->SubmitRevisionRecording(Epoch::Shadow, *revision.recordings, 0);
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			static std::uint32_t logged = 0;
			if (logged++ % 600 == 0) {
				std::string views;
				for (const auto& p : pending) {
					const auto& view = target.views[p.slot - kFirstShadowViewSlot];
					views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", p.viewId,
						p.renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.ModeInputs(view.modeIndex), view.capacity);
				}
				logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} material rows ({} in the payload): {}",
					target.views.size(), shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, parts[PayloadPart::ShadowRows].units, views);
			}
			TracyCZoneEnd(shadowViewsZone);
		}, frameOwners);
		auto& payload = *committed;
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own compile, prepare and record
		if (ok) {
			++shadowStats.epochs;
			shadowStats.viewsDrawn += static_cast<std::uint32_t>(pending.size());
			// The occlusion maps' occluders are uploaded: each map can be DCLF's this frame.
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				if (!modeUsed[OcclusionModeOf(v)])
					continue;
				auto& occlusion = impl->occlusion[v];
				occlusion.committedFrame = frameNumber;
				occlusion.inputs = static_cast<std::uint32_t>(payload.ModeInputs(OcclusionModeOf(v)));
			}
			impl->ReadShadowCullCounters(frameNumber, shadowStats);
			for (std::uint32_t m = 0; m < kFirstOcclusionMode && m < shadowStats.casters.size(); ++m)
				if (modeUsed[m])
					shadowStats.casters[m] = static_cast<std::uint32_t>(payload.ModeInputs(m));
			shadowStats.setWaiting += payload.setWaiting;
			if (shadowStats.setWaitingFirst.empty())
				shadowStats.setWaitingFirst = payload.setWaitingFirst;
			if (PassCapture::ShadowWithholdingEnabled()) {
				// The cascades' casters decide which sun entries the next frame's cascade culls may skip.
				if (modeUsed[kSunShadowMode])
					SunAccumulation::Get().PublishExclusion(payload.sunExclusion);
				// Likewise the paraboloids for point-light culls (none when no point light drew: a new light is the engine's).
				LocalLightCull::Publish(modeUsed[kParabolicShadowMode] ? payload.parabolicExclusion : nullptr);
			}
			pending.clear();
		} else {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			notReady(ShadowNotReady::Epoch);
		}
		shadowStats.cpuMs += totalMs;
	}

	ShadowInputs IndirectDraws::Impl::PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const
	{
		ZoneScopedN("CS.DCLF.PrepareShadowInputs");
		ShadowInputs in;
		in.frameNumber = Scene::FrameView::Get().GetFrame();
		in.modeUsed = a_modeUsed;
		in.modeRasterStates = a_modeRasterStates;
		// The sun's full-frustum planes (kCullSunEntry), DCLF's as UpdateCamera set up each process (SunViews): this frame's at the
		// shadow epoch, last frame's for the build ahead (posted before this frame's UpdateCamera), as the engine held them there.
		if (const auto& sunViews = SunViews::Get().Current();
			sunViews.valid && (sunViews.sceneFrame == in.frameNumber || sunViews.sceneFrame + 1 == in.frameNumber) && !sunViews.fullFrusta.empty()) {
			for (const auto& mine : sunViews.fullFrusta) {
				auto& out = in.sunEntryProcesses.emplace_back();
				for (std::uint32_t p = 0; p < 6; ++p)
					for (std::uint32_t i = 0; i < 4; ++i)
						out.planes[p][i] = mine.plane[p][i];
				out.mask = mine.mask;
			}
			Diagnostics::Notify(Diagnostics::SunEntryPlanesTaken{ in.frameNumber, true });
		} else {
			Diagnostics::Notify(Diagnostics::SunEntryPlanesTaken{ in.frameNumber, false });
		}
		in.sunCandidates = Scene::FrameView::Get().GetSunCandidates();
		in.lightCandidates = Scene::FrameView::Get().GetLightCandidates();
		// The versioned buffers' addresses and SRV indices are the sealed set's (BuildSnapshotDraws: AddressesOf).
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.placementsIndex = FrameValues::Get().PlacementsIndex();
		in.addresses.palettesIndex = FrameValues::Get().PalettesIndex();
		in.addresses.shadingIndex = FrameValues::Get().ShadingIndex();
		in.addresses.identity = &a_resources;
		in.tablesGeneration = Scene::FrameView::Get().GetTablesGeneration();
		in.lookupGeneration = Scene::FrameView::Get().GetLookups().shadowGeneration;
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

	std::shared_ptr<ShadowPayload> IndirectDraws::Impl::AcquireShadowPayload()
	{
		// Idle payloads are reset so they hold nothing, as AcquirePayload does.
		std::shared_ptr<ShadowPayload> free;
		for (auto& payload : shadowPayloadPool)
			if (payload.use_count() == 1) {
				payload->Reset();
				if (!free)
					free = payload;
			}
		return free ? free : shadowPayloadPool.emplace_back(std::make_shared<ShadowPayload>());
	}

	void IndirectDraws::Impl::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
	{
		// Right after the epoch's submission: its counters are copied each execution into the frame slot's host buffer
		// (CounterReadback, the cs.dclf.shadow.cull-counters pass). A sample is read once its execution's GPU point is reached,
		// provided no later shadow execution took its slot: every execution passes here, its slot still the host's current one.
		auto* host = RenderGraphRuntime::Get().Host();
		const auto readback = CullCounterReadback(CullCounterView::kShadow);
		if (!host || !readback)
			return;
		auto& armed = readback->armed;
		const std::uint32_t frameSlot = host->CurrentFrameSlot();
		const auto epoch = shadowCullEpochs++;
		bool lost = false;
		if (armed.armed) {
			if (frameSlot == armed.frameSlot) {
				// This execution writes the slot the sample was in before it was read: lost, and sampled again now.
				armed.armed = false;
				++readback->missed;
				lost = true;
			} else if (!armed.done.Reached()) {
				return;
			} else {
				armed.armed = false;
				const bool read = readback->Read(armed.frameSlot, armed.source, armed.stamp, [&](const std::uint32_t* a_words) {
					a_stats.cullDrawn = a_words[0];
					a_stats.cullRejected = a_words[1];
					a_stats.cullTested = a_words[2];
					a_stats.cullSunEntryOut = a_words[3];
					a_stats.cullMinRadius = a_words[4];
					a_stats.cullRootFading = a_words[5];
					a_stats.cullClass = a_words[16];
					a_stats.cullSampledView = armed.view;
					a_stats.cullSampledMode = armed.mode;
				});
				++(read ? readback->harvested : readback->missed);
				return;
			}
		}
		// One epoch in 127 (or the one after a lost sample), cycling through the views.
		if (((epoch % 127) != 0 && !lost) || !shadow || shadow->labels.empty())
			return;
		const auto& view = shadow->labels[(epoch / 127) % shadow->labels.size()];
		if (view.slot >= readback->sources)
			return;
		armed = { true, frameSlot, view.slot, CounterReadback::StampOf(a_frame), view.viewId, view.renderMode, host->SubmittedPoint() };
	}

	void IndirectDraws::Impl::ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats)
	{
		if (const auto set = Scene::FrameView::Get().FrameClaims(); set && (set->drawn & (kSetCaster | kSetCasterPoint)) && PassCapture::ShadowWithholdingEnabled())
			++a_stats.notReadyWithheld;
	}
}
