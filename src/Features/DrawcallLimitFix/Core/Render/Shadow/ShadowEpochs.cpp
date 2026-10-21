// The shadow and occlusion epochs (render thread): their commits (the installed payload, the index pool, the views' latches,
// the arena and the latched CS blocks), the epochs' inputs and the culling counters' readback. Submitted where Features/Shadows/Engine's
// handlers call them (ExecuteShadowFrame, ExecuteOcclusion: Api/ShadowEpochs.h), each in its epoch's segment (its traits).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	namespace Draws
	{
		/**
		 * @brief A shadow view's common latch: a single phase over every input (no engine-visibility gate), the eye-folded
		 * view-projection and the frame's stamp. Culling flags and planes are the caller's. a_seedsIndex: the root states' seeds (the cull
		 * binding RootSeeds).
		 */
		BuildDrawsLatch ShadowViewLatch(const PendingView& a_view, std::uint32_t a_inputs, std::uint32_t a_frame, std::uint32_t a_seedsIndex)
		{
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (a_inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = a_inputs;
			latch.visibilityStamp = a_frame & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			latch.fadeSeedsIndex = a_seedsIndex;
			FoldEyeIntoViewProj(a_view.viewProj, a_view.eye, latch.viewProj);
			return latch;
		}

		namespace
		{
			/**
			 * @brief A sun view's least caster radius in world units for a_minTexels (kCullMinTexels; 0: no test). A cascade's projection is
			 * orthographic (row 3 is (0, 0, 0, 1): clip w 1), so a sphere of radius r spans r * |row 0's xyz| of NDC either side of its centre,
			 * and NDC's 2 units span the viewport's width: r * |row 0's xyz| * width / 2 texels, row 1 and the height likewise. The radius is
			 * the threshold over the larger axis' texels per world unit (a caster either axis resolves is kept). The eye fold moves only the
			 * rows' w. A projection that is not orthographic has no one texel size: not tested.
			 */
			float SunCasterMinRadius(const float (&a_viewProj)[16], std::uint32_t a_width, std::uint32_t a_height, float a_minTexels)
			{
				if (!(a_minTexels > 0.0f) || !a_width || !a_height)
					return 0.0f;
				const auto rowLength = [&](std::uint32_t a_row) {
					const float* row = a_viewProj + a_row * 4;
					return std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
				};
				if (rowLength(3) > 1e-6f)
					return 0.0f;
				const float texelsPerUnit = std::max(rowLength(0) * 0.5f * static_cast<float>(a_width), rowLength(1) * 0.5f * static_cast<float>(a_height));
				return texelsPerUnit > 0.0f ? a_minTexels / texelsPerUnit : 0.0f;
			}
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

		/**
		 * @brief Points a_latch at the key slots' vertex layouts, writing them when a_write (once per frame slot and shape). Slots past the
		 * shape's table (or the latch's) stay zero: their draws take the bucket's pushed layout (Utility.hlsl, DCLFShadowVertex).
		 */
		void UseSlotLayouts(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const ShadowFrame& a_frame, std::uint32_t a_latchSlot,
			bool a_write, BuildDrawsLatch& a_latch)
		{
			a_latch.slotLayoutsOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchSlot)) + a_layout.SlotLayoutsOffset();
			if (!a_write || !a_frame.slotLayouts || a_frame.slotLayouts->empty())
				return;
			const auto& layouts = *a_frame.slotLayouts;
			const std::size_t count = std::min<std::size_t>(layouts.size(), a_layout.keySlots);
			LatchWrite(a_latchBlock, "shadow slot layouts", a_latchSlot, a_layout.SlotLayoutsOffset(), std::as_bytes(std::span(layouts.data(), count)));
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

		/**
		 * @brief Writes the epoch's one culling dispatch (ShadowBuildDrawsPass: every view in one, BuildDrawsCS's ShadowViews) into the frame
		 * slot's latch region: x the groups of a_inputs, the whole list every view culls (ShadowViewLatch's; a view's own drawCount bounds
		 * it), y a group row per view of a_shape, the variant submitted (ShadowBuildRows: its table's rows, or more, which the shader idles).
		 */
		void WriteBuildDispatch(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, std::uint32_t a_latchSlot, bool a_sky, std::uint32_t a_inputs,
			const ShadowFrame& a_shape)
		{
			const std::uint32_t dispatch[4] = { (a_inputs + 63) / 64, ShadowBuildRows(a_shape), 1, 0 };
			LatchWrite(a_latchBlock, "shadow culling dispatch", a_latchSlot, a_layout.BuildDispatchOffset(a_sky), std::as_bytes(std::span(dispatch)));
		}
	}

	bool IndirectDraws::OcclusionReady(std::uint32_t a_view) const
	{
		// Ready when this frame's shadow commit uploaded the occluders and the map's target is imported.
		const auto& engine = *impl->shadowEngine;
		const auto toggles = ActiveToggles();
		if (a_view >= kOcclusionViews || failed || !toggles.shadows || !Extensions::OcclusionOn(toggles, a_view) || !engine.shadow)
			return false;
		const auto& occlusion = engine.occlusion[a_view];
		// Not while the built graph lacks the map's import, nor without the selected revision's recorded shape for the maps' layout
		// (the engine draws the map meanwhile).
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || host->RebuildRequested())
			return false;
		const auto layouts = engine.PredictedOcclusion(&toggles);
		bool shaped = false;
		const std::uint32_t epoch = EpochOfShape(ShapeKind::Shadow, kOcclusionShape);
		if (impl->EpochCovered(epoch))
			if (const auto& active = impl->revisions.active)
				if (const auto& fragment = active->Fragment(Impl::SceneRevisions::kShapeSlot + epoch))
					if (const auto variants = fragment->Value<ShadowVariants>())
						for (const auto& shape : variants->shapes)
							shaped = shaped || (shape && LayoutOf(*shape) == layouts);
		if (!shaped) {
			++impl->occlusionUnrecorded;
			return false;
		}
		return occlusion.rasterState && occlusion.committedFrame == Scene::FrameView::Get().GetFrame() && engine.shadow->depth[OcclusionDepthTarget(a_view)];
	}

	std::uint32_t IndirectDraws::ExecuteOcclusion(std::uint32_t a_views)
	{
		ZoneScopedN("CS.DCLF.ExecuteOcclusion");
		const auto start = std::chrono::steady_clock::now();
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto& engine = *impl->shadowEngine;
		// The requested views that are ready and captured this frame; a map the engine did not draw draws nothing.
		std::uint32_t drawable = 0;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (!(a_views & (1u << v)))
				continue;
			if (OcclusionReady(v) && engine.occlusion[v].capturedFrame == frameNumber)
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
		auto resources = engine.shadow;
		// The revision's variant for the drawable maps and the payload the shadow commit read: trusted, nothing is checked here.
		std::vector<ShadowViewLayout> layouts;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (drawable & (1u << v))
				layouts.push_back(LayoutOf(engine.occlusion[v].view, OcclusionSlot(v), OcclusionModeOf(v), OcclusionDepthTarget(v)));
		const std::uint32_t epoch = EpochOfShape(ShapeKind::Shadow, kOcclusionShape);
		const auto revision = impl->RevisionOf(epoch);
		std::shared_ptr<const ShadowFrame> revisionShape;
		std::size_t revisionVariant = 0;
		if (revision)
			if (const auto variants = revision.shape->Value<ShadowVariants>())
				for (std::size_t v = 0; v < variants->shapes.size() && !revisionShape; ++v)
					if (variants->shapes[v] && LayoutOf(*variants->shapes[v]) == layouts) {
						revisionShape = variants->shapes[v];
						revisionVariant = v;
					}
		const auto committed = engine.committedShadow;
		if (!revisionShape || !committed) {
			// A layout the revision lacks (one map drawn without the other): the engine draws them.
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				shadowStats.occlusionNotReady[v] += (drawable & (1u << v)) ? 1 : 0;
			return 0;
		}
		const auto& payload = *committed;
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(EpochTraitsOf(epoch).segment, [&](org::RenderGraph&) {
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			const ShadowFrame& target = *revisionShape;
			const auto& latchBlock = *target.latch;
			const auto& latchLayout = target.latchLayout;
			auto& scene = *resources->scene;
			// The root states' seeds and the states the frame's cull reads (the cull bindings RootSeeds, RootStates), once for every map.
			const std::uint32_t seedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, scene, frameNumber, impl->Adopted());
			const std::uint32_t statesIndex = CullBindingIndex(Api::CullBindingSlot::RootStates, scene, frameNumber, impl->Adopted());
			std::size_t index = 0;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				if (!(drawable & (1u << v)))
					continue;
				const auto& view = engine.occlusion[v].view;
				const auto& shapeView = target.views[index];
				const auto& buckets = (*target.rows)[index++];
				const std::uint32_t slot = OcclusionSlot(v), mode = OcclusionModeOf(v);
				const auto inputCount = static_cast<std::uint32_t>(payload.Inputs());  // the whole shadow list
				// Occluders, material rows, frame record, objects and geometries are the installed payload's, which the shadow commit read
				// this frame (OcclusionReady).
				WriteViewBlocks(latchBlock, latchLayout, latchSlot, slot, view);
				// Frustum culling (near plane included), the map's size test while its entry asks (minRadius: Skylighting::
				// OcclusionTechnique's, bound radius above 32; both maps' do), and fade roots as the engine's cull leaves them
				// (kCullFadeOnVisible): never a root the main camera faded out or has not seen yet.
				auto latch = ShadowViewLatch(view, inputCount, frameNumber, seedsIndex);
				static const REL::Relocation<const std::uint8_t*> fadesOn{ REL::Offset(Engine::Address::kFadesOn) };
				const auto& map = Extensions::OcclusionMapOf(v);
				latch.cullFlags = 1u | (map.minRadius ? kCullMinRadius : 0u) | (*fadesOn.get() ? kCullFadeOnVisible : 0u) | KeyWordOf(mode);
				// The map's floor (Skylighting's probe grid test): a cull plane facing up, this frame's height. An occlusion view
				// leaves its cull planes otherwise unused (its frustum is the view-projection's).
				if (map.floorHeight) {
					latch.cullPlanes[0][0] = 0.0f;
					latch.cullPlanes[0][1] = 0.0f;
					latch.cullPlanes[0][2] = 1.0f;
					latch.cullPlanes[0][3] = map.floorHeight();
					latch.cullPlaneMask = 1u;
				}
				latch.viewBits = ShadowViewBits(mode, false);
				latch.fadeStatesIndex = statesIndex;
				ShadowPayloadLatch(*impl->InstalledPayload(), latch);
				UseShadowMapRow(latchBlock, latchLayout, buckets, latchSlot, mode, view.rasterState, true, latch);
				UseSlotLayouts(latchBlock, latchLayout, target, latchSlot, true, latch);
				WriteBucketTable(latchBlock, latchLayout, latchSlot, slot, shapeView.buckets, true, latch);
				LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				shadowStats.occlusionInputs[v] = inputCount;
				++shadowStats.occlusionDrawn[v];
			}
			// The epoch's one culling dispatch, over the whole shadow list (each map's latch above bounds its own).
			WriteBuildDispatch(latchBlock, latchLayout, latchSlot, true, static_cast<std::uint32_t>(payload.Inputs()), target);
			impl->SubmitRevisionRecording(epoch, *revision.recordings, revisionVariant);
		}, engine.shadowExecutionOwner);
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

	namespace Draws
	{
		namespace
		{
			/** @brief ExecuteShadowFrame's epoch body's shared locals: the committed payload and what its stages write it with. */
			struct ShadowCommit
			{
				ShadowResources& resources;
				ShadowPayload& payload;
				const PayloadVersions& parts;
				const NamedVersions& adopted;
				CommitUploads& uploads;
				const ShadowFrame& target;
				const ShadowInputs& in;
				const std::vector<PendingView>& pending;
				std::uint32_t frameNumber;
				IndirectDraws::ShadowStats& stats;
			};

			/** @brief The commit's shared uploads (vertex streams, the commit hooks) and the payload's counts into the stats. */
			void CommitShadowShared(const ShadowCommit& a_commit, const std::array<bool, kShadowModeCount>& a_modeUsed,
				std::vector<std::shared_ptr<const void>>& a_frameOwners)
			{
				auto& payload = a_commit.payload;
				auto& shadowStats = a_commit.stats;
				const auto& adopted = a_commit.adopted;
				auto& uploads = a_commit.uploads;
				auto& scene = *a_commit.resources.scene;
				shadowStats.faceUploads += Extensions::UploadVertexStream(adopted, payload.streamRows, scene, uploads);
				Extensions::RunCommitHooks({ Api::CommitKind::Shadow, adopted, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetFrame(), scene, uploads, nullptr,
					a_frameOwners });
				shadowStats.records = static_cast<std::uint32_t>(payload.materialRows.Count());
				shadowStats.skippedTexture = payload.skippedTexture;
				shadowStats.skippedPipeline = payload.skippedPipeline;
				shadowStats.deferredTextures = payload.deferredTextures;
				shadowStats.deferredPipelines = payload.deferredPipelines;
				std::size_t casterInputs = 0;
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					if (a_modeUsed[m])
						casterInputs += payload.ModeInputs(m);
				shadowStats.inputs = static_cast<std::uint32_t>(casterInputs);
			}

			/**
			 * @brief The index pool, then every pending view's blocks and culling latch (with the revision's bucket tables and map rows
			 * where the frame slot's region does not hold them yet), and a zero latch for each placement without a view.
			 */
			void LatchShadowViews(const ShadowCommit& a_commit, std::uint32_t a_latchSlot, const std::shared_ptr<const ShadowFrame>& a_revisionShape,
				const std::vector<ShadowViewLayout>& a_layouts)
			{
				auto& resources = a_commit.resources;
				auto& payload = a_commit.payload;
				auto& shadowStats = a_commit.stats;
				const auto& adopted = a_commit.adopted;
				const auto& target = a_commit.target;
				const std::uint32_t frameNumber = a_commit.frameNumber;
				const std::uint32_t latchSlot = a_latchSlot;
				const auto& layouts = a_layouts;
				auto& scene = *resources.scene;
				resources.labels.clear();
				const auto& latchBlock = *target.latch;
				const auto& latchLayout = target.latchLayout;
				// The index pool both this epoch and the occlusion epoch draw from.
				{
					ZoneScopedN("CS.DCLF.ShadowInputs.IndexPool");
					UpdateIndexPool(adopted, *resources.pool, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetTablesGeneration(), latchBlock, latchSlot,
						latchLayout.PoolOffset(), a_commit.uploads);
				}
				// The revision's bucket tables and map rows: written into this frame slot's region only where it does not hold them yet.
				auto& held = Draws::shadowLatchRegions.Of(latchSlot, target.latch, a_revisionShape, 0);
				std::uint32_t sunEntryOffset = 0;  // the slot's sun entry region, once written
				std::vector<bool> slotDrawn(layouts.size());
				// The root states' seeds, the states the frame's cull reads (the cull bindings RootSeeds, RootStates) and the listed roots (a
				// focus view's target), once for every view.
				const std::uint32_t seedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, scene, frameNumber, adopted);
				const std::uint32_t statesIndex = CullBindingIndex(Api::CullBindingSlot::RootStates, scene, frameNumber, adopted);
				const auto roots = Extensions::RootListOf(Scene::FrameView::Get().GetTables());
				// A portal-strict point light's casters pass its strict gates (the frame's atlas and gates; 0: none, nothing gated).
				const std::uint32_t contextsIndex = CullBindingIndex(Api::CullBindingSlot::LightContexts, scene, frameNumber, adopted);
				const std::uint32_t gatesIndex = CullBindingIndex(Api::CullBindingSlot::StrictGates, scene, frameNumber, adopted);
				// The sun views' contribution cull's threshold (live: a latch value, read once a commit).
				const float minTexels = shadowMinTexels.load(std::memory_order_relaxed);
				for (const auto& view : a_commit.pending) {
					ZoneScopedN("CS.DCLF.ShadowInputs.View");
					const std::uint32_t slot = view.slot;
					const std::uint32_t index = slot - kFirstShadowViewSlot;
					slotDrawn[index] = true;
					WriteViewBlocks(latchBlock, latchLayout, latchSlot, slot, view);
					// The whole shadow list: the view bits select the mode's inputs.
					const auto inputCount = static_cast<std::uint32_t>(payload.Inputs());
					// Frustum culling, no engine-visibility gate. Only class-split modes test the caster class (not the paraboloids).
					auto latch = ShadowViewLatch(view, inputCount, frameNumber, seedsIndex);
					const std::uint32_t classFlags = view.casterClass ? kCullVolumetricOnly : view.modeIndex == kParabolicShadowMode ? 0u : kCullCastersOnly;
					latch.cullFlags = (view.hasViewProj ? (1u | (view.renderMode == 0xE ? kCullNoNearPlane : 0u)) : 0u) | classFlags |
					                  (view.sunView ? kCullSunEntry : 0u) | KeyWordOf(view.modeIndex);
					// A sun view's casters below the size threshold (cascades and their volumetric copies; local lights and the occlusion
					// maps never: the maps keep their minRadius).
					if (view.sunView && view.hasViewProj)
						if (const float radius = SunCasterMinRadius(latch.viewProj, view.width, view.height, minTexels); radius > 0.0f) {
							latch.cullFlags |= kCullMinTexels;
							latch.minCasterRadius = radius;
						}
					latch.viewBits = ShadowViewBits(view.modeIndex, view.sunView);
					// A focus view draws its target's members alone (root slot + 1; an unlisted target draws nothing).
					if (view.focus) {
						const auto root = view.focusTarget ? roots.Find(view.focusTarget) : kNoFadeRoot;
						latch.focusRoot = root != kNoFadeRoot ? root + 1 : kFocusRootNone;
					}
					latch.cullPlaneMask = view.cullPlaneMask;
					std::memcpy(latch.cullPlanes, view.cullPlanes, sizeof(latch.cullPlanes));
					latch.fadeStatesIndex = statesIndex;
					if (view.portalContext && contextsIndex && gatesIndex) {
						latch.lightContextsIndex = contextsIndex;
						latch.strictGatesIndex = gatesIndex;
						latch.strictContext = view.portalContext;
					}
					ShadowPayloadLatch(a_commit.parts, latch);
					// Sun entry rule: BuildDraws tests each input's entry sphere against the full-frustum processes (written once per frame).
					if (view.sunView) {
						if (!sunEntryOffset)
							sunEntryOffset = WriteSunEntryRegion(latchBlock, latchLayout, latchSlot, payload.inputs.sunEntryProcesses);
						latch.sunEntryOffset = sunEntryOffset;
					}
					const std::uint32_t mapRow = view.rasterState ? latchLayout.MapRowOf(view.modeIndex, view.rasterState) : ~0u;
					const bool rowHeld = mapRow < latchLayout.MapRows() && Draws::LatchRegionsHeld::Hold(held, std::size_t(mapRow) * 2 + 1);
					UseShadowMapRow(latchBlock, latchLayout, (*target.rows)[index], latchSlot, view.modeIndex, view.rasterState, !rowHeld, latch);
					UseSlotLayouts(latchBlock, latchLayout, target, latchSlot, !Draws::LatchRegionsHeld::Hold(held, std::size_t(latchLayout.MapRows()) * 2 + 1), latch);
					WriteBucketTable(latchBlock, latchLayout, latchSlot, slot, target.views[index].buckets, !Draws::LatchRegionsHeld::Hold(held, std::size_t(slot) * 2), latch);
					LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
					Diagnostics::Notify(Diagnostics::ShadowViewLatched{ view, latch, frameNumber, payload, shadowStats });
					resources.labels.push_back({ view.viewId, view.renderMode, slot });
				}
				// Placements without a view this frame: a zero latch, no work.
				for (std::size_t index = 0; index < layouts.size(); ++index) {
					if (slotDrawn[index])
						continue;
					LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, layouts[index].slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), BuildDrawsLatch{});
					++shadowStats.retainedViews;
				}
			}

			/**
			 * @brief The arena, with the frame record patched to this frame's per-frame buffers (the build ahead has another frame's).
			 * Uploaded around the record, not over it: copies within one batch are unordered.
			 */
			void UploadShadowArena(const ShadowCommit& a_commit)
			{
				auto& resources = a_commit.resources;
				auto& payload = a_commit.payload;
				const auto& parts = a_commit.parts;
				const auto& in = a_commit.in;
				auto& uploads = a_commit.uploads;
				auto& arena = payload.arena;
				if (const auto& bytes = arena.Bytes(); !bytes.empty()) {
					DrawBindings record = payload.frameRecord;
					WriteRecordBuffers(record, parts, { in.addresses.placementsIndex, in.addresses.palettesIndex, in.addresses.shadingIndex }, a_commit.adopted,
						*resources.scene, a_commit.frameNumber);
					constexpr std::size_t recordEnd = kShadowFrameRecordOffset + sizeof(DrawBindings);
					if (bytes.size() < recordEnd)
						stl::report_and_fail("Drawcall Limit Fix: a shadow arena without its frame record");
					// The arena around the record is the build's: nothing is staged while the buffer holds this build's (a kept publication
					// commits the same build every frame), and of a new build only the chunks that differ from what the buffer holds. The
					// record is the frame's.
					auto& staged = Draws::shadowArenaHeld;
					const std::shared_ptr<const void> arenaTarget = resources.constants;
					const bool sameTarget = Draws::LatchRegionsHeld::Same(staged.target, arenaTarget);
					const bool arenaHeld = sameTarget && staged.serial == payload.arenaSerial;
					if (kShadowFrameRecordOffset && !arenaHeld)
						uploads(resources.constants, bytes.data(), kShadowFrameRecordOffset, 0);
					uploads(resources.constants, &record, sizeof(record), kShadowFrameRecordOffset);
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
								uploads(resources.constants, tail.data() + run, at - run, recordEnd + run);
								inRun = false;
							}
						}
						if (inRun)
							uploads(resources.constants, tail.data() + run, tail.size() - run, recordEnd + run);
						staged.bytes.assign(tail.begin(), tail.end());
						staged.target = arenaTarget;
						staged.serial = payload.arenaSerial;
					}
				}
			}

			/**
			 * @brief CS's SharedData and FeatureData as of now (the water reflection prepasses refreshed them after the build), latched:
			 * the epoch's first pass copies them after the arena. Returns the latched values' misses.
			 */
			std::uint32_t LatchShadowCsBlocks(const ShadowCommit& a_commit, std::uint32_t a_latchSlot)
			{
				auto& resources = a_commit.resources;
				const auto& in = a_commit.in;
				auto latched = LatchedUploads(a_commit.target.latched[ShadowFrame::kCommitList], a_commit.uploads, a_latchSlot);
				// At their fixed places (ShadowArenaBlocksOf); a size mismatch with CS's block is a contract violation.
				const auto blocks = ShadowArenaBlocksOf();
				const auto writeBlock = [&](std::uint64_t a_offset, std::uint32_t a_size, const std::vector<std::byte>& a_bytes) {
					if (!a_size)
						return;
					if (a_bytes.size() != a_size)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: a shadow commit's CS block of {} bytes for its place of {}", a_bytes.size(), a_size));
					latched(resources.constants, a_bytes.data(), a_bytes.size(), a_offset);
				};
				writeBlock(blocks.sharedData, blocks.sharedBytes, in.sharedData);
				writeBlock(blocks.featureData, blocks.featureBytes, in.featureData);
				(void)latched.Finish();
				return latched.Misses();
			}

			/** @brief The shadow epoch's periodic log line (one in 600). */
			void LogShadowEpoch(const ShadowCommit& a_commit)
			{
				const auto& target = a_commit.target;
				const auto& payload = a_commit.payload;
				const auto& shadowStats = a_commit.stats;
				static std::uint32_t logged = 0;
				if (logged++ % 600 == 0) {
					std::string views;
					for (const auto& p : a_commit.pending) {
						const auto& view = target.views[p.slot - kFirstShadowViewSlot];
						views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", p.viewId,
							p.renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.ModeInputs(view.modeIndex), view.capacity);
					}
					logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} material rows ({} in the payload): {}",
						target.views.size(), shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, a_commit.parts[PayloadPart::ShadowRows].units, views);
				}
			}
		}
	}

	void IndirectDraws::ExecuteShadowFrame()
	{
		ZoneScopedN("CS.DCLF.ExecuteShadowFrame");
		auto& engine = *impl->shadowEngine;
		auto& pending = engine.pendingViews;
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
				engine.ShadowsNotDrawn(shadowStats);
			pending.clear();
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !engine.shadow || engine.shadowPlacements.empty())
			return notReady(ShadowNotReady::Setup);
		const auto indirect = FrameShadowIndirectState();
		if (!indirect.valid) {
			engine.ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto resources = engine.shadow;
		std::array<bool, kShadowModeCount> modeUsed{};
		std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
		engine.NoteShadowViewModes(modeUsed, modeRasterStates);
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0;

		ShadowInputs in = engine.PrepareShadowInputs(*resources, modeUsed, modeRasterStates);
		// The revision's shape and recording and the installed payload: trusted, nothing is checked here. The epoch covers every
		// placement; a slot without a view does no work (zero latch, counters zeroed, its render pass loads and stores the engine's).
		const std::vector<ShadowViewLayout>& layouts = engine.shadowPlacements;
		const std::uint32_t epoch = EpochOfShape(ShapeKind::Shadow, kShadowShape);
		const auto revision = impl->RevisionOf(epoch);
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
			++impl->revisions.latchClamped[epoch];
			in.sunEntryProcesses.resize(target.latchLayout.sunProcesses);
		}
		ShadowPayload* committed = ahead.get();
		auto& async = stats.async[kAsyncShadow];
		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup)
			return;
		auto frameOwners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(EpochTraitsOf(epoch).segment, [&](org::RenderGraph&) {
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
			engine.committedShadow = ahead;
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
			engine.shadowExecutionOwner = frameOwners;
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
			TracyCZoneEnd(shadowPrepareZone);

			// The adopted set: the versions the commit writes into, and the SRV indices it writes.
			const auto& adopted = impl->Adopted();
			CommitUploads uploads(impl->commitStagedPool, adopted);
			const Draws::ShadowCommit commit{ *resources, payload, parts, adopted, uploads, target, in, pending, frameNumber, shadowStats };
			TracyCZoneN(shadowCommitZone, "CS.DCLF.ShadowInputs.CommitShared", true);
			const auto inputsStart = std::chrono::steady_clock::now();
			Draws::CommitShadowShared(commit, modeUsed, *frameOwners);
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
			TracyCZoneEnd(shadowCommitZone);

			TracyCZoneN(shadowViewsZone, "CS.DCLF.ShadowInputs.BuildViews", true);
			const auto blocksStart = std::chrono::steady_clock::now();
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			Draws::LatchShadowViews(commit, latchSlot, revisionShape, layouts);
			Draws::UploadShadowArena(commit);
			impl->revisions.latchedMisses += Draws::LatchShadowCsBlocks(commit, latchSlot);
			// The narrowest variant whose views cover the frame's (the full one, 0, always does): the others' indirect draws would only
			// fetch zero counts. The frame's set is posted for the revisions to record a variant of (ShadowFrame::activeViews).
			std::uint64_t active = 0;
			for (const auto& view : pending)
				if (const auto index = view.slot - kFirstShadowViewSlot; view.slot >= kFirstShadowViewSlot && index < 64)
					active |= 1ull << index;
			engine.lastActiveViews = active;
			std::size_t variant = 0;
			int narrowest = 65;
			const auto& variants = revision.shape->Value<ShadowVariants>()->shapes;
			for (std::size_t v = 0; v < variants.size(); ++v)
				if (const auto mask = variants[v]->activeViews; (active & ~mask) == 0 && std::popcount(mask) < narrowest) {
					narrowest = std::popcount(mask);
					variant = v;
				}
			if (variant != 0)
				++shadowStats.narrowedEpochs;
			// The epoch's one culling dispatch, over the whole shadow list (each view's latch bounds its own), a group row per view of the
			// variant submitted.
			WriteBuildDispatch(*target.latch, target.latchLayout, latchSlot, false, static_cast<std::uint32_t>(payload.Inputs()), *variants[variant]);
			impl->SubmitRevisionRecording(epoch, *revision.recordings, variant);
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			Draws::LogShadowEpoch(commit);
			TracyCZoneEnd(shadowViewsZone);
		}, frameOwners);
		auto& payload = *committed;
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own compile, prepare and record
		if (ok) {
			engine.NoteShadowEpochDrawn(payload, modeUsed, frameNumber, shadowStats);
		} else {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			notReady(ShadowNotReady::Epoch);
		}
		shadowStats.cpuMs += totalMs;
	}

	void ShadowEngine::NoteShadowViewModes(std::array<bool, kShadowModeCount>& a_modeUsed, std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates)
	{
		auto& modeUsed = a_modeUsed;
		// Inputs and pipelines cover every catalog state per mode (UpdateShadowCapability), so which views a frame draws (cascades
		// alternate depth-bias states, point-light views come and go) changes nothing the build reads. An unforeseen mode or state
		// is a defect, added to the catalog now.
		for (const auto& view : pendingViews) {
			modeUsed[view.modeIndex] = true;
			if (!readyModes[view.modeIndex]) {
				readyModes[view.modeIndex] = true;
				NoteCapability(shadowCatalogBuilt, fmt::format("a view of render mode {:#x}", view.renderMode));
			}
			if (const auto& states = readyStates[view.modeIndex].Of(view.casterClass != 0); !std::binary_search(states.begin(), states.end(), view.rasterState)) {
				readyStates[view.modeIndex].Add(view.rasterState, view.casterClass != 0);
				NoteCapability(shadowCatalogBuilt, fmt::format("view {} (mode {:#x}) draws with rasterizer state {}, not in the catalog", view.viewId, view.renderMode, view.rasterState));
			}
		}
		// The occlusion epoch draws later from this build, whether or not the engine draws the map this frame.
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (Extensions::OcclusionOn(ActiveToggles(), v) && readyModes[OcclusionModeOf(v)] && occlusion[v].dsvFormat != DXGI_FORMAT_UNKNOWN)
				modeUsed[OcclusionModeOf(v)] = true;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			if (modeUsed[m])
				a_modeRasterStates[m] = readyStates[m];
		// One pipeline set per shadow map format: every engine target is D16.
		const DXGI_FORMAT dsvFormat = pendingViews.front().dsvFormat;
		if (dsvFormat != shadowTargetFormat) {
			shadowTargetFormat = dsvFormat;
			NoteCapability(shadowCatalogBuilt, fmt::format("a view draws into format {}", static_cast<int>(dsvFormat)));
		}
	}

	void ShadowEngine::NoteShadowEpochDrawn(const ShadowPayload& a_payload, const std::array<bool, kShadowModeCount>& a_modeUsed, std::uint32_t a_frame,
		IndirectDraws::ShadowStats& a_stats)
	{
		const auto& payload = a_payload;
		const auto& modeUsed = a_modeUsed;
		auto& epochStats = a_stats;
		++epochStats.epochs;
		epochStats.viewsDrawn += static_cast<std::uint32_t>(pendingViews.size());
		// The occlusion maps' occluders are uploaded: each map can be DCLF's this frame.
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (!modeUsed[OcclusionModeOf(v)])
				continue;
			auto& state = occlusion[v];
			state.committedFrame = a_frame;
			state.inputs = static_cast<std::uint32_t>(payload.ModeInputs(OcclusionModeOf(v)));
		}
		ReadShadowCullCounters(a_frame, epochStats);
		for (std::uint32_t m = 0; m < kFirstOcclusionMode && m < epochStats.casters.size(); ++m)
			if (modeUsed[m])
				epochStats.casters[m] = static_cast<std::uint32_t>(payload.ModeInputs(m));
		epochStats.setWaiting += payload.setWaiting;
		if (epochStats.setWaitingFirst.empty())
			epochStats.setWaitingFirst = payload.setWaitingFirst;
		if (PassCapture::ShadowWithholdingEnabled()) {
			// The cascades' casters decide which sun entries the next frame's cascade culls may skip.
			// Handed back to the exclusion contribution (Features/Shadows/Local's slots).
			if (modeUsed[kSunShadowMode])
				Extensions::PublishShadowExclusion(Api::ShadowSource::Sun, payload.sunExclusion);
			// Likewise the paraboloids for point-light culls (none when no point light drew: a new light is the engine's), while the Point
			// source's culls are in to take it.
			if (Extensions::SourceCullsInstalled(Api::ShadowSource::Point))
				Extensions::PublishShadowExclusion(Api::ShadowSource::Point, modeUsed[kParabolicShadowMode] ? payload.parabolicExclusion : nullptr);
		}
		pendingViews.clear();
	}

	ShadowInputs ShadowEngine::PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const
	{
		ZoneScopedN("CS.DCLF.PrepareShadowInputs");
		ShadowInputs in;
		in.frameNumber = Scene::FrameView::Get().GetFrame();
		in.modeUsed = a_modeUsed;
		in.modeRasterStates = a_modeRasterStates;
		// The sun's full-frustum planes (kCullSunEntry), DCLF's as UpdateCamera set up each process (Api::SunVolumeContribution: Features/
		// Shadows/Sun's Sun board): this frame's at the shadow epoch, last frame's for the build ahead (posted before this frame's
		// UpdateCamera), as the engine held them there.
		Api::SunEntryPlanesSink entryPlanes(in.sunEntryProcesses);
		const bool entryPlanesTaken = Extensions::SampleSunEntryPlanes(entryPlanes, in.frameNumber);
		Diagnostics::Notify(Diagnostics::SunEntryPlanesTaken{ in.frameNumber, entryPlanesTaken });
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

	std::shared_ptr<ShadowPayload> ShadowEngine::AcquireShadowPayload()
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

	void ShadowEngine::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
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
					a_stats.cullBelowTexels = a_words[Draws::kCountBelowTexelsWord];
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

	void ShadowEngine::ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats)
	{
		if (const auto set = Scene::FrameView::Get().FrameClaims(); set && (set->drawn & (kSetCaster | kSetCasterPoint)) && PassCapture::ShadowWithholdingEnabled())
			++a_stats.notReadyWithheld;
	}
}
