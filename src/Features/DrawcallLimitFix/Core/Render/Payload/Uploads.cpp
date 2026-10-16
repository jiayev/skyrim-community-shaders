// The main epochs' commit (render thread, inside the epoch): stream views, CommitMainPayload (the frame's values
// into the selected revision's shape) and the depth commit's scene uploads.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Draws.h"

namespace DCLF
{
	// Core's feature rows start right after the extras' pixel row (FrameSlots.h).
	static_assert(Draws::FrameConstantRowOffset(Extensions::kFrameConstantRowFirst) == Draws::kExtrasPixelFrameOffset + sizeof(ExtrasFrame::projectedGlobals),
		"Core's first frame constant row is not right after the extras' pixel row");

	void Draws::CommitUploads::operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
	{
		Diagnostics::Notify(Diagnostics::CommitTargetStaged{ adopted, a_target });
		(*this)(TargetOf(a_target), a_data, a_bytes, a_offset);
	}

	/** @brief Brings the stores up to the tables from what the buffers hold, as StreamViews. */
	std::shared_ptr<const StreamViews> Draws::MakeStreamViews(ObjectRecordStore& a_objects, ExtrasStore& a_extras, GeometryStore& a_geometries, const TablesContentEpochs& a_oldest,
			std::shared_ptr<const SceneTables> a_hold, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame)
		{
			ZoneScopedN("CS.DCLF.MakeStreamViews");
			auto views = std::make_shared<StreamViews>();
			views->tables = std::move(a_hold);
			views->generation = a_generation;
			// The three stores are independent (each its own journal, all reading the immutable tables): pool tasks, unless the parity
			// keeps the fan-outs on the builder's thread (Policy::SerialFanout).
			GeometryDrawsOut geometries;
			const auto store = [&](std::size_t a_store) {
				switch (a_store) {
				case 0:
					UpdateObjectRecords(&a_objects, a_oldest.objects, a_tables, a_generation, a_frame, views->objects);
					break;
				case 1:
					UpdateExtras(&a_extras, a_oldest.extras, a_tables, a_generation, views->extras);
					break;
				default:
					UpdateGeometryDraws(&a_geometries, a_oldest.geometries, a_tables, a_generation, a_frame, geometries);
					break;
				}
			};
			if (Policy::SerialFanout()) {
				for (std::size_t s = 0; s < 3; ++s)
					store(s);
			} else {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.MakeStreamViews.Stores", 3, 1, [&](std::size_t a_begin, std::size_t a_end) {
					ZoneScopedN("CS.DCLF.MakeStreamViews.StoresShards");
					for (std::size_t s = a_begin; s < a_end; ++s)
						store(s);
				});
			}
			views->geometries = std::move(geometries.slots);
			return views;
		}

	namespace
	{
		// The main latch's regions the depth commits wrote the snapshot's bucket map and tables into (render thread).
		Draws::LatchRegionsHeld mainBucketRegions;
	}

	// CommitMainPayload's locals, shared by its stages (render thread, one commit).
	struct Draws::MainCommit
	{
		const Capture& capture;
		const MainInputs& in;
		MainPayload& payload;
		const std::shared_ptr<Resources>& resources;
		IndirectDraws::Stats& stats;
		std::vector<std::shared_ptr<const void>>& bindingOwners;
		const SceneTables& tables;
		const PayloadVersions& parts;
		const NamedVersions& adopted;
		CommitUploads& uploads;
		LatchedUploads& latched;
		GpuTextures& textures;
		ConstantMirror& mirror;
		const PassFrame& shape;
		const std::shared_ptr<const PassFrame>& revisionShape;
		const ZBucketPlan& writePlan;
		const org::LatchBlock& latchBlock;
		const MainLatchLayout& latchLayout;
		std::uint32_t frameNumber;
		bool depthOnly;
		bool replayVertexInputs;
	};

	namespace
	{
		// Frame textures the drawn pipelines read but the commit could not resolve read zero, as an unbound view does; counted,
		// since the build could not skip their draws. A natively bound view that cannot be resolved (not a CPU-written structured
		// buffer) lands here too.
		void CountMissingFrameTextures(const MainCommit& a_commit, const std::array<std::uint32_t, kTextureRegisters>& a_frameTextures)
		{
			std::uint32_t frameTexturesMissing = 0;
			std::array<std::uint64_t, 2> missingRegisters{};
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				if (!((a_commit.payload.frameRegisters[t / 64] >> (t % 64)) & 1) || a_frameTextures[t] != kInvalidIndex)
					continue;
				++frameTexturesMissing;
				missingRegisters[t / 64] |= 1ull << (t % 64);
				Diagnostics::Notify(Diagnostics::FrameTextureUnresolved{ t, a_commit.capture.psViews[t] });
			}
			a_commit.stats.frameTexturesMissing = frameTexturesMissing;
			a_commit.stats.frameTexturesMissingRegisters = missingRegisters;
		}

		// The frame record: every register a draw's rows do not give.
		void WriteFrameRecord(const MainCommit& a_commit, const std::array<std::uint32_t, kTextureRegisters>& a_frameTextures)
		{
			const auto& in = a_commit.in;
			const auto& resources = *a_commit.resources;
			DrawBindings frameRecord{};
			const std::uint64_t frameConstants = resources.frameConstantsAddress;
			for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
				frameRecord.vertexConstants[r] = ((in.vsFrameMask >> r) & 1) ? frameConstants + FrameSlotOffset(false, r) : 0;
				frameRecord.pixelConstants[r] = ((in.psFrameMask >> r) & 1) ? frameConstants + FrameSlotOffset(true, r) : 0;
			}
			frameRecord.pixelConstants[3] = frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
			frameRecord.pixelConstants[kFrameLightingRegister] = frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
			const std::uint32_t nullIndex = a_commit.textures.NullIndex() != kInvalidIndex ? a_commit.textures.NullIndex() : 0u;
			for (std::uint32_t t = 0; t < kTextureRegisters; ++t)
				frameRecord.textures[t] = a_frameTextures[t] == kInvalidIndex ? nullIndex : a_frameTextures[t];
			const auto& sceneBuffers = *resources.scene;
			const auto& parts = a_commit.parts;
			const auto& adopted = a_commit.adopted;
			frameRecord.textures[kObjectBufferRegister] = parts[PayloadPart::Objects].srvIndex;
			frameRecord.textures[kExtrasBufferRegister] = parts[PayloadPart::Extras].srvIndex;
			frameRecord.textures[kPlacementBufferRegister] = in.addresses.placementsIndex;
			frameRecord.textures[kPaletteBufferRegister] = in.addresses.palettesIndex;
			frameRecord.textures[kShadingBufferRegister] = in.addresses.shadingIndex;
			Extensions::FrameBindingValues bindings;
			Extensions::FrameBindingValuesOf(adopted, sceneBuffers, a_commit.frameNumber, bindings);
			// The root states and rows every draw's alpha reads are among them (t120/t119: Features/Fades').
			Extensions::WriteFrameBindings(bindings, frameRecord.textures);
			a_commit.latched(resources.frameConstants, &frameRecord, sizeof(frameRecord), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
		}

		// The frame slots, the shared light block and the frame-wide values in PS/VS b13 (lighting, LOD fades, fog, extras, the frame
		// constant rows, foliage).
		void LatchFrameSlots(const MainCommit& a_commit, const FrameBlocks& a_blocks)
		{
			const auto& resources = a_commit.resources;
			auto& latched = a_commit.latched;
			auto& uploads = a_commit.uploads;
			// The frame slots, and the zeroed light block every draw's b3 reads.
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if (!a_blocks.vs[slot].empty())
					latched(resources->frameConstants, a_blocks.vs[slot].data(), a_blocks.vs[slot].size(), FrameSlotOffset(false, slot));
				if (!a_blocks.ps[slot].empty())
					latched(resources->frameConstants, a_blocks.ps[slot].data(), a_blocks.ps[slot].size(), FrameSlotOffset(true, slot));
			}
			if (!resources->sharedLightZeroed) {
				static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
				uploads(resources->frameConstants, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
				resources->sharedLightZeroed = true;
			}
			// The frame lighting, every commit: latching it only on change would change the frame's shape.
			const auto& frameCapture = Scene::FrameView::Get().GetFrameCapture();
			latched(resources->frameConstants, frameCapture.lighting.data(), sizeof(frameCapture.lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
			// The LOD fades' frame inputs after the lighting (PS b13, c6): the draws fade specular and envmap by distance.
			const LodFadeFrame lodFadeFrame = SampleLodFadeFrame();
			latched(resources->frameConstants, &lodFadeFrame, sizeof(lodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
			// The frame's fog into VS b13 (DCLFFrameFog); the technique rows keep theirs.
			latched(resources->frameConstants, frameCapture.fog.data(), sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
			// The extras' frame inputs: vertex part after the fog (c3-c7), pixel part at PS b13 c16.
			const ExtrasFrame extrasFrame = SampleExtrasFrame();
			latched(resources->frameConstants, &extrasFrame, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
			latched(resources->frameConstants, extrasFrame.projectedGlobals, sizeof(extrasFrame.projectedGlobals), kExtrasPixelFrameOffset);
			// The frame constant rows (PS b13 c17 and up, Api::FrameConstantRow: the Effect port's camera planes, light colour and blood),
			// in ascending row order, as MainLatchedLayout lays them out.
			for (const auto& row : Extensions::RegisteredFrameConstantRows()) {
				float value[4]{};
				row.sample(value);
				latched(resources->frameConstants, value, sizeof(value), FrameConstantRowOffset(row.row));
			}
			// The foliage record: buffers (by parity) and tag at PS b13 c14, counters zeroed. The Z-prepass reads the owners' index
			// and size as the previous colour commit left them.
			if (const auto& foliage = resources->foliage; foliage && !a_commit.depthOnly) {
				const std::uint32_t epoch = ++foliage->epoch, h = epoch & 1;
				const std::uint32_t words[8] = { static_cast<std::uint32_t>(foliage->idsAddress[h]), static_cast<std::uint32_t>(foliage->idsAddress[h] >> 32),
					static_cast<std::uint32_t>(foliage->coloursAddress[h]), static_cast<std::uint32_t>(foliage->coloursAddress[h] >> 32), foliage->width, foliage->height,
					epoch & 0xFFu, foliage->ownersIndex };
				latched(resources->frameConstants, words, sizeof(words),
					std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame));
				static const std::array<std::uint32_t, kFoliageCounters> zeros{};
				uploads(foliage->results, zeros.data(), sizeof(zeros), 0);
			}
		}

		// The resident region's counters.
		void CopyResidentStats(const MainCommit& a_commit)
		{
			const auto& payload = a_commit.payload;
			auto& stats = a_commit.stats;
			const bool depthOnly = a_commit.depthOnly;
			stats.residentInputs = static_cast<std::uint32_t>(payload.resident.Count());
			if (!depthOnly) {
				stats.residentDraws = payload.residentDraws;
				stats.residentPairs = payload.residentPairs;
				stats.residentUndrawable = payload.residentUndrawable;
			}
			stats.residentEpochs += payload.resident.ContentEpoch() != stats.residentLastEpoch[depthOnly ? 0 : 1] ? 1 : 0;
			stats.residentLastEpoch[depthOnly ? 0 : 1] = payload.resident.ContentEpoch();
			stats.residentResyncs += payload.residentResyncs;
			for (std::size_t k = 0; k < stats.residentResyncBy.size(); ++k)
				stats.residentResyncBy[k] += (payload.residentResyncReasons >> k) & 1u;
			stats.residentDecalRetakes += payload.residentDecalRetakes;
		}

		// The build's counters and its writes.
		void CopyBuildStats(const MainCommit& a_commit)
		{
			const auto& payload = a_commit.payload;
			auto& stats = a_commit.stats;
			stats.skipped = payload.skipped;
			stats.missingTextures = payload.missingTextures;
			stats.missingVertexConstants = payload.missingVertexConstants;
			stats.missingPixelConstants = payload.missingPixelConstants;
			stats.deferredTextures = payload.deferredTextures;
			stats.rowTableConflicts += payload.rowTableConflicts;
			for (std::size_t i = 0; i < payload.partMs.size(); ++i)
				stats.partMs[i] = payload.partMs[i];
			if (!a_commit.depthOnly)
				stats.decalsDrawn = payload.decalsDrawn;
			if (payload.shortBuffers) {
				const bool first = stats.shortBuffers == 0;
				stats.shortBuffers += payload.shortBuffers;
				if (first)
					Diagnostics::Notify(Diagnostics::ShortBuffersDrawn{ a_commit.tables, payload });
			}
			// The build's writes (what the payload's catch-up sent is the snapshot builder's: PayloadVersions::bytes).
			stats.uploadBytes = (payload.materialRowsWritten * sizeof(MaterialRow)) + (payload.pipelineRowsWritten * sizeof(PipelineRow)) +
			                      payload.inputList.size() * sizeof(DrawInput);
			stats.records = static_cast<std::uint32_t>(payload.materialRows.Count());
		}

		// What the culling latch is made from: the counts and the draws' ViewProj.
		struct MainCullInputs
		{
			std::uint32_t inputCount = 0;
			std::array<float, 16> viewProj{};
			bool hasViewProj = false;
		};

		// The segment's draw and input counts (checked against the shape's bound) and its ViewProj.
		void CountMainDraws(const MainCommit& a_commit, const std::vector<std::byte>& a_prepassPerFrame, MainCullInputs& a_cull)
		{
			const auto& payload = a_commit.payload;
			const auto& shape = a_commit.shape;
			const bool depthOnly = a_commit.depthOnly;
			// The resident region's draws and inputs lead the frame's own.
			const std::uint32_t drawCount = static_cast<std::uint32_t>(payload.sequences.size() + payload.residentDraws);
			const auto decalCount = depthOnly ? std::array<std::uint32_t, kDecalGroups>{} : payload.decalCount;
			// Inputs the culling covers; in the depth segment more than drawCount (cull-only candidates).
			a_cull.inputCount = static_cast<std::uint32_t>(payload.inputList.size() + (payload.resident.Count()));
			// The shape's ranges hold the scene's draw bound, and the sequences' named version holds the ranges (the revision required
			// them): a count past them is a defect of that bound.
			if (drawCount > shape.sequenceDraws)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} draws past the sequence buffer's {} (the scene's draw bound missed them)", drawCount, shape.sequenceDraws));
			for (std::uint32_t group = 0; group < kDecalGroups; ++group)
				if (decalCount[group] > shape.sequenceDecals)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} decals past the sequence buffer's {} per group", decalCount[group], shape.sequenceDecals));
			// The culling must use the draws' ViewProj (VS_PerFrame c8).
			if (auto* buffer = a_commit.capture.vsBuffers[kPerFrameVertexRegister]) {
				a_commit.mirror.Watch(buffer);
				const auto contents = a_commit.mirror.Contents(buffer);
				if (contents.size() >= 48 * sizeof(float)) {
					std::memcpy(a_cull.viewProj.data(), reinterpret_cast<const float*>(contents.data()) + 32, sizeof(float) * 16);
					a_cull.hasViewProj = true;
				}
			}
			// The culling setup (logged once) and, periodically, the epoch's viewport and ViewProj z row: both epochs must rasterise
			// identically for the colour EQUAL test.
			Diagnostics::Notify(Diagnostics::MainCullingSetup{ shape, a_commit.capture, depthOnly, a_cull.hasViewProj, a_commit.replayVertexInputs, a_commit.frameNumber,
				a_commit.tables.liveObjects, a_commit.replayVertexInputs ? std::span<const std::byte>(a_prepassPerFrame) : std::span<const std::byte>() });
			a_commit.stats.drawn = drawCount;
			++a_commit.stats.commitEpochs;
		}

		// The segment's BuildDraws latch: payload, dispatch, culling (its ViewProj, HZB scale, view bits) and the depth segment's
		// fade, tree and terrain tests.
		void FillCullLatch(const MainCommit& a_commit, const MainCullInputs& a_cull, BuildDrawsLatch& a_latch)
		{
			const auto& resources = a_commit.resources;
			const auto& shape = a_commit.shape;
			const bool depthOnly = a_commit.depthOnly;
			const std::uint32_t frameNumber = a_commit.frameNumber;
			a_latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			// The root states' seeds (the cull binding RootSeeds: a pooled buffer's index).
			a_latch.fadeSeedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, *resources->scene, frameNumber, a_commit.adopted);
			// The depth segment: its stage word, for the feature pass before the depth build that reads one (Api::PassDef::latchWord).
			a_latch.stageWord = depthOnly ? Draws::PassLatchWord() : 0u;
			PayloadLatch(a_commit.parts, a_latch);
			a_latch.dispatch[0] = (a_cull.inputCount + 63) / 64;
			a_latch.dispatch[1] = 1;
			a_latch.dispatch[2] = 1;
			a_latch.drawCount = a_cull.inputCount;
			a_latch.cullFlags = a_cull.hasViewProj ? shape.cullMode : 0u;
			// The depth segment culls every candidate; the colour segment draws the members and writes every decal slot.
			a_latch.viewBits = depthOnly ? kDepthViewBits : kColourViewBits;
			// The frame number: the depth segment publishes and the colour segment reads within one frame.
			a_latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
			if (resources->hzb && shape.width && shape.height) {
				// Mip 0 covers twice its size in source pixels; image UVs scale by the rendered share to reach the HZB.
				const auto scale = [](std::uint32_t a_rendered, std::uint32_t a_covered) {
					const double ratio = a_covered ? std::min(1.0, double(a_rendered) / double(a_covered)) : 0.0;
					return static_cast<std::uint32_t>(std::lround(ratio * 65535.0)) & 0xFFFFu;
				};
				a_latch.hzbUvScalePacked = scale(shape.width, resources->hzbWidth * 2) | (scale(shape.height, resources->hzbHeight * 2) << 16);
			}
			FoldEyeIntoViewProj(a_cull.viewProj, a_commit.capture.eye, a_latch.viewProj);
			std::copy(std::begin(a_latch.viewProj), std::end(a_latch.viewProj), a_commit.payload.cullViewProj.begin());
			a_commit.payload.culled = a_latch.cullFlags != 0;
			Diagnostics::Notify(Diagnostics::MainCullViewProj{ a_latch, shape.width, shape.height, frameNumber, depthOnly, a_commit.tables });
			// The depth segment: the fade test's camera (kObjectFadeTest).
			if (depthOnly) {
				const auto eye = PrimaryCull::Get().FadeEye();
				std::copy(eye.begin(), eye.end(), a_latch.fadeEye);
				const auto treeHeight = PrimaryCull::Get().TreeHeightTest();
				a_latch.treeHeight[0] = treeHeight[0];
				a_latch.treeHeight[1] = treeHeight[1];
				// The near twin's test (Api::NearTwinContribution: Integrations/TerrainBlending's, with its depth-pass eye), for the terrain
				// group 1 twin buckets (kBucketTerrainPair). No test (false) leaves the latch's zeros: w 0.
				if (const auto nearTest = Extensions::NearTwin().nearTest)
					nearTest(a_latch.terrainNear);
				Diagnostics::Notify(Diagnostics::CullLatchWritten{ a_latch, true });
			} else {
				a_latch.terrainCapacity[0] = shape.terrainCapacity[0];
				a_latch.terrainCapacity[1] = shape.terrainCapacity[1];
			}
			// Both segments read LOD skin levels from the same fade states (the cull binding RootStates: the frame's read parity).
			a_latch.fadeStatesIndex = CullBindingIndex(Api::CullBindingSlot::RootStates, *resources->scene, frameNumber, a_commit.adopted);
		}
	}

	namespace
	{
		/** @brief This frame's cascades from DCLF's views (the Sun board). Render thread, after the sun's UpdateCamera; empty without views. */
		void GpuCascades(std::vector<GpuCascade>& a_out)
		{
			// This frame's cascades, whether or not the entry exclusion ran: the colour pass's sun test needs them every frame (without them
			// every pass with the sun's bits would miss, and draw unshadowed). From DCLF's own views (the Sun board, from the cameras UpdateCamera
			// set up for the sun this frame), so the test does not depend on the engine's Accumulate running. None when UpdateCamera did not
			// run for the sun this frame (the sun does not draw: an interior), when the engine gives no pass the sun's bits either. Each
			// cascade's planes are those Accumulate's hook copies from the same views (the bits' cascades), in descriptor order.
			a_out.clear();
			const auto& views = ShadowCameras::Sun();
			const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
			if (!views.valid || views.sceneFrame != frame)
				return;
			for (std::uint32_t d = 0; d < views.count && d < views.cascades.size(); ++d) {
				const auto* cascade = ShadowCameras::Sun().CascadeOf(d, frame);
				if (!cascade)
					continue;
				auto& out = a_out.emplace_back();
				for (std::uint32_t p = 0; p < 6; ++p) {
					std::memcpy(out.planes[p], cascade->cull.plane[p].data(), sizeof(float) * 4);
					std::memcpy(out.planes[6 + p], cascade->caster.plane[p].data(), sizeof(float) * 4);
				}
				out.masks[0] = cascade->cull.mask;
				out.masks[1] = cascade->caster.mask;
			}
		}
	}

	// The colour pass's cascades and local shadow volumes (the sun has accumulated, shadow maps are drawn), clamped to the latch
	// the producer sized (ReserveMainLatch).
	void IndirectDraws::Impl::CommitShadowInputs(bool a_depthOnly, const PassFrame& a_shape, std::uint32_t a_revisionEpoch)
	{
		if (!a_depthOnly) {
			GpuCascades(sunCascades);
			// Frames with no cascades draw sun-lit passes unshadowed (right only when the sun did not accumulate).
			sunTestEmpty += sunCascades.empty() ? 1 : 0;
			if (sunTestWindow.Tick())
				Reports::ColourSunTestLine(std::exchange(sunTestEmpty, 0));
			// The local shadow lights accumulated this frame, a volume per shadowmap descriptor (Api::LightVolumesContribution: Features/
			// LocalLights'), for the objects' shadow mask (Light Limit Fix's ShadowBitMask). The list stays whole (the build parity's
			// reference); the latch takes the clamped count.
			shadowVolumes.clear();
			Api::LightVolumeSink sink(shadowVolumes);
			Extensions::SampleLightVolumes(sink);
			shadowVolumesLatched = static_cast<std::uint32_t>(shadowVolumes.size());
		}
		if (sunCascades.size() > a_shape.latchLayout.cascades || shadowVolumesLatched > a_shape.latchLayout.shadowVolumes) {
			++revisions.latchClamped[a_revisionEpoch];
			sunCascades.resize(std::min<std::size_t>(sunCascades.size(), a_shape.latchLayout.cascades));
			shadowVolumesLatched = std::min(shadowVolumesLatched, a_shape.latchLayout.shadowVolumes);
		}
	}

	// The frame's textures (t16 and up), resolved into the frame record, and the frame buffers' copies.
	void IndirectDraws::Impl::ResolveFrameTextures(const MainCommit& a_commit, std::array<std::uint32_t, kTextureRegisters>& a_frameTextures)
	{
		const auto& capture = a_commit.capture;
		const auto& epochResources = a_commit.resources;
		auto& textures = a_commit.textures;
		auto& mirror = a_commit.mirror;
		const bool depthOnly = a_commit.depthOnly;
		a_frameTextures.fill(kInvalidIndex);
		if (frameTextureGeneration != textures.Generation()) {
			frameTextureBindings = {};
			frameTextureGeneration = textures.Generation();
		}
		// The frame textures (Api::FrameTexture: the soft effects' depth, the kPOST_ZPREPASS_COPY a Soft pass reads; Integrations/
		// TerrainBlending's mask, t55, bound by its replay after this epoch, so named rather than captured), each view once a colour
		// commit; their registers read them. A null view with capturedWhenNull leaves the captured one (the mask while not deferred).
		const auto frameTextures = Extensions::RegisteredFrameTextures();
		const auto frameTextureAt = Extensions::FrameTextureIndices();
		std::array<ID3D11ShaderResourceView*, Extensions::kFrameTextureSlots> frameTextureViews{};
		for (std::size_t i = 0; i < frameTextures.size() && !depthOnly; ++i)
			frameTextureViews[i] = static_cast<ID3D11ShaderResourceView*>(frameTextures[i].view());
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t) {
			auto& held = frameTextureBindings[t];
			const auto at = frameTextureAt[t];
			const bool registered = at != Extensions::kNoFrameTexture && (frameTextureViews[at] || !frameTextures[at].capturedWhenNull);
			auto* const view = t == kCharacterLightRegister ? Scene::FrameView::Get().GetFrameCapture().characterLightView :
			                   registered                   ? frameTextureViews[at] :
			                                                  capture.psViews[t];
			if (!view) {
				if (held.view)
					held = {};
				a_frameTextures[t] = textures.NullIndex();
				continue;  // the shared lookup root already owns the null descriptor
			}
			if (held.view != view || !held.binding.owner || held.binding.index == GpuTextures::kInvalid) {
				held.view = view;
				held.binding = textures.ResolveBinding(held.view, 64 + t);
			}
			a_frameTextures[t] = held.binding.index;
			if (held.binding.owner)
				a_commit.bindingOwners.push_back(held.binding.owner);
		}
		// The frame graph buffers on the graph (Api::FrameGraphBuffers: Integrations/LightLimitFix's, t35-t37), their indices at their registers.
		const auto graphBuffers = Extensions::RegisteredFrameGraphBuffers();
		for (std::size_t b = 0; b < graphBuffers.size() && !depthOnly; ++b)
			if ((epochResources->graphBuffers >> b) & 1)
				graphBuffers[b].indices(std::span(a_frameTextures).subspan(graphBuffers[b].firstRegister, graphBuffers[b].count));
		for (const auto& frameBuffer : depthOnly ? decltype(epochResources->frameBuffers){} : epochResources->frameBuffers) {
			auto* buffer = BufferOf(capture.psViews[frameBuffer.textureRegister]);
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			const std::size_t offset = std::size_t(frameBuffer.firstElement) * frameBuffer.stride;
			const std::size_t bytes = std::size_t(frameBuffer.elements) * frameBuffer.stride;
			if (contents.size() < offset + bytes) {
				// Unfillable: its copy reads zero (still written, as the shape's latched copies name it).
				Diagnostics::Notify(Diagnostics::FrameBufferUnfilled{ frameBuffer.textureRegister, frameBuffer.elements, frameBuffer.stride, contents.size() });
				static std::vector<std::byte> zeros;
				if (zeros.size() < bytes)
					zeros.resize(bytes);
				a_commit.latched(frameBuffer.copy, zeros.data(), bytes, 0);
				continue;  // not written since it is watched
			}
			a_commit.latched(frameBuffer.copy, contents.data() + offset, bytes, 0);
			a_frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
		}
	}

	// Uploads run ahead of every pass of this epoch: the vertex stream's rows and the features' commit hooks (Features/TreeLod's tables
	// among them).
	void IndirectDraws::Impl::UploadSceneStreams(const MainCommit& a_commit)
	{
		const auto& adopted = a_commit.adopted;
		const auto& epochResources = a_commit.resources;
		const bool depthOnly = a_commit.depthOnly;
		Extensions::UploadVertexStream(adopted, a_commit.payload.streamRows, *epochResources->scene, a_commit.uploads);
		Extensions::RunCommitHooks({ depthOnly ? Api::CommitKind::Depth : Api::CommitKind::Colour, adopted, Scene::FrameView::Get().GetTables(),
			Scene::FrameView::Get().GetFrame(), *epochResources->scene, a_commit.uploads, &a_commit.latched, a_commit.bindingOwners });
	}

	// The segment's counters zeroed: the count words, the sort's and the HZB's once, and the colour segment's decal, terrain,
	// forward and refraction words.
	void IndirectDraws::Impl::ZeroCommitCounts(const MainCommit& a_commit)
	{
		const auto& epochResources = a_commit.resources;
		const auto& payload = a_commit.payload;
		auto& latched = a_commit.latched;
		auto& uploads = a_commit.uploads;
		const bool depthOnly = a_commit.depthOnly;
		// The depth segment clears every counter; the colour segment only its own draw word, or it would erase the depth phases'
		// counters before anything read them.
		const std::size_t zeroBytes = depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t);
		latched(epochResources->count, kZeroCounts, zeroBytes, 0);
		// The sort's counts start at zero; afterwards the scan clears them.
		if (epochResources->sort)
			epochResources->sort->ZeroCountsOnce(uploads);
		// Likewise the depth pyramid's scratch (Api::DepthPyramid::scratch: the HZB's group counter).
		if (depthOnly && epochResources->hzbScratch && !epochResources->hzbScratchZeroed) {
			static constexpr std::uint32_t kZero = 0;
			uploads(epochResources->hzbScratch, &kZero, sizeof(kZero), 0);
			epochResources->hzbScratchZeroed = true;
		}
		// Colour segment only: each decal group's slot count, and the tallies zeroed.
		if (!depthOnly) {
			decalWords = { payload.decalCount[0], payload.decalCount[1], 0u, 0u, payload.decalCount[2] };
			latched(epochResources->count, decalWords.data(), 4 * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
			latched(epochResources->count, &decalWords[4], sizeof(std::uint32_t), kCountDecalLayerWord * sizeof(std::uint32_t));
			// The terrain groups' counts.
			latched(epochResources->count, kZeroCounts, kTerrainGroups * sizeof(std::uint32_t), kCountTerrainGroupWord * sizeof(std::uint32_t));
			// The forward range's count and culling tallies.
			latched(epochResources->count, kZeroCounts, 4 * sizeof(std::uint32_t), kCountForwardWord * sizeof(std::uint32_t));
			// The refraction range's count and culling tallies.
			latched(epochResources->count, kZeroCounts, 4 * sizeof(std::uint32_t), kCountRefractionWord * sizeof(std::uint32_t));
		}
	}

	// The depth segment's plain draws: the index pool brought up to the tables, the bucket map and each phase's table in the
	// latch, their count words zeroed. A slot without a published pipeline maps to none: BuildDraws drops its draws.
	void IndirectDraws::Impl::WriteDepthBuckets(const MainCommit& a_commit, std::uint32_t a_latchSlot, BuildDrawsLatch& a_latch)
	{
		const auto& epochResources = a_commit.resources;
		const auto& writePlan = a_commit.writePlan;
		const auto& latchBlock = a_commit.latchBlock;
		const auto& layout = a_commit.latchLayout;
		UpdateIndexPool(a_commit.adopted, *epochResources->pool, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetTablesGeneration(), latchBlock, a_latchSlot,
			layout.PoolOffset(), a_commit.uploads);
		// The plan's slots, and none for any other slot the installed members can use (new since the plan).
		const auto planned = static_cast<std::uint32_t>(writePlan.map.size());
		auto slots = std::max(planned, InstalledPipelineSlots());
		if (slots > layout.buckets)
			slots = layout.buckets;
		const std::uint32_t buckets = writePlan.Buckets();
		// The map and the tables are the snapshot's (its revision's plan and latch, the installed publication's pipeline slots):
		// written into this frame slot's region only when it holds another snapshot's.
		auto& held = mainBucketRegions.Of(a_latchSlot, a_commit.shape.latch, a_commit.revisionShape, slots);
		if (!Draws::LatchRegionsHeld::Hold(held, 0)) {
			WriteBucketMap(latchBlock, "main bucket map", a_latchSlot, layout.BucketMapOffset(), writePlan.map, slots);
			if (buckets) {
				const std::size_t words = std::size_t(buckets) * 2;
				LatchWrite(latchBlock, "main bucket tables", a_latchSlot, layout.BucketTableOffset(), std::as_bytes(std::span(writePlan.table.data(), words)));
				LatchWrite(latchBlock, "main bucket tables", a_latchSlot, layout.PhaseTwoBucketTableOffset(), std::as_bytes(std::span(writePlan.table.data() + words, words)));
			}
		}
		if (buckets) {
			zBucketZeros.assign(buckets, 0u);
			for (const auto& counts : epochResources->zBucketCounts)
				a_commit.latched(counts, zBucketZeros.data(), std::size_t(buckets) * sizeof(std::uint32_t), 0);
		}
		const auto region = static_cast<std::uint32_t>(latchBlock.Offset(a_latchSlot));
		a_latch.bucketTableOffset = region + layout.BucketTableOffset();
		a_latch.phaseTwoBucketTableOffset = region + layout.PhaseTwoBucketTableOffset();
		a_latch.bucketMapOffset = region + layout.BucketMapOffset();
	}

	// The colour segment's sun cascades and local shadow volumes in the latch, and the latch's sun test.
	void IndirectDraws::Impl::WriteShadowLatch(const MainCommit& a_commit, std::uint32_t a_latchSlot, BuildDrawsLatch& a_latch)
	{
		const auto& latchBlock = a_commit.latchBlock;
		const auto& layout = a_commit.latchLayout;
		const std::uint32_t header[4] = { static_cast<std::uint32_t>(sunCascades.size()), 0, 0, 0 };
		LatchWrite(latchBlock, "main sun cascades", a_latchSlot, MainLatchLayout::CascadeOffset(), std::as_bytes(std::span(header)));
		if (!sunCascades.empty())
			LatchWrite(latchBlock, "main sun cascades", a_latchSlot, MainLatchLayout::CascadeOffset() + kSunRegionHeader, std::as_bytes(std::span(sunCascades)));
		const std::uint32_t volumeHeader[4] = { shadowVolumesLatched, 0, 0, 0 };
		LatchWrite(latchBlock, "main shadow volumes", a_latchSlot, layout.ShadowVolumeOffset(), std::as_bytes(std::span(volumeHeader)));
		if (shadowVolumesLatched)
			LatchWrite(latchBlock, "main shadow volumes", a_latchSlot, layout.ShadowVolumeOffset() + kSunRegionHeader,
				std::as_bytes(std::span(shadowVolumes).first(shadowVolumesLatched)));
		a_latch.sunState = kSunTestOn;
		a_latch.sunCascadeOffset = static_cast<std::uint32_t>(latchBlock.Offset(a_latchSlot)) + MainLatchLayout::CascadeOffset();
		a_latch.localShadowOffset = static_cast<std::uint32_t>(latchBlock.Offset(a_latchSlot)) + layout.ShadowVolumeOffset();
		Diagnostics::Notify(Diagnostics::CullLatchWritten{ a_latch, false });
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats,
		std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision)
	{
		ZoneScopedN("CS.DCLF.CommitMainPayload");
		// Cleared geometry slots' buffers, held until this execution retires.
		for (auto& owner : Scene::FrameView::Get().TakeRetiredImports())
			a_bindingOwners.push_back(std::move(owner));
		// Per-frame values (number, frame slots, frame record buffers) come from a_frame: a payload ahead is a frame old.
		const auto& in = a_frame;
		const auto& tables = Scene::FrameView::Get().GetTables();
		const bool depthOnly = in.depthOnly;
		// The committed payload is the installed publication's: its buffers are the installed PayloadVersions (a covered frame has a
		// ready one), which the snapshot builder filled. Nothing of it is uploaded here.
		const PayloadVersions& parts = *InstalledPayload();
		const std::uint32_t frameNumber = in.frameNumber;
		auto& textures = GpuTextures::Get();
		auto& mirror = ConstantMirror::Get();
		const bool replayVertexInputs = !depthOnly && prepassInputs;
		// The adopted set: every versioned buffer's address, SRV index and capacity the commit writes, and the version it writes into.
		const auto& adopted = Adopted();
		CommitUploads uploads(commitStagedPool, adopted);
		const std::size_t latchedShape = depthOnly ? kDepthShape : kColourShape;
		// The selected revision's shape is trusted: the commit writes the frame's values into its latch, bucket plan and latched
		// copies, and submits its recording. Nothing is checked against it.
		const auto revisionEpoch = EpochOfShape(ShapeKind::Main, static_cast<std::uint32_t>(latchedShape));
		const auto revisionShape = a_revision.shape->Value<PassFrame>();
		const PassFrame& shape = *revisionShape;
		CommitShadowInputs(depthOnly, shape, revisionEpoch);
		static const ZBucketPlan kNoPlan;
		const ZBucketPlan& writePlan = shape.zPlan ? *shape.zPlan : kNoPlan;
		const org::LatchBlock& latchBlock = *shape.latch;
		const MainLatchLayout& latchLayout = shape.latchLayout;
		// Per-frame values go through the latch; the epoch's first pass copies them.
		auto latched = LatchedUploads(shape.latched, uploads, RenderGraphRuntime::Get().Host()->CurrentFrameSlot());
		auto lap = [&, last = std::chrono::steady_clock::now()](std::size_t a_part) mutable {
			const auto now = std::chrono::steady_clock::now();
			a_stats.commitUs[a_part] += std::chrono::duration<double, std::micro>(now - last).count();
			last = now;
		};
		const std::size_t shapeIndex = latchedShape;
		const MainCommit commit{ a_capture, in, a_payload, a_resources, a_stats, a_bindingOwners, tables, parts, adopted, uploads, latched, textures, mirror, shape,
			revisionShape, writePlan, latchBlock, latchLayout, frameNumber, depthOnly, replayVertexInputs };

		std::array<std::uint32_t, kTextureRegisters> frameTextures;
		ResolveFrameTextures(commit, frameTextures);
		CountMissingFrameTextures(commit, frameTextures);
		WriteFrameRecord(commit, frameTextures);
		lap(2);

		LatchFrameSlots(commit, a_blocks);

		lap(3);
		// The commit hooks ran there (the root states' producer's among them: Features/Fades', the roots and their write-back armed).
		// The visibility readback reads the roots' states the depth epoch steps: armed after them.
		UploadSceneStreams(commit);
		if (depthOnly)
			ArmVisibilityReadback(*a_resources->scene, uploads);
		ZeroCommitCounts(commit);
		CopyResidentStats(commit);

		lap(4);
		CopyBuildStats(commit);
		lap(6);
		lap(5);

		MainCullInputs cull;
		CountMainDraws(commit, prepassVS[kPerFrameVertexRegister], cull);

		// The segment's BuildDraws latch, in its frame slot (the host waited for the slot).
		BuildDrawsLatch latch{};
		FillCullLatch(commit, cull, latch);
		const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		if (depthOnly && a_resources->pool)
			WriteDepthBuckets(commit, latchSlot, latch);
		if (!depthOnly)
			WriteShadowLatch(commit, latchSlot, latch);
		LatchWriteValue(latchBlock, "main culling latch", latchSlot, 0, latch);
		// What the reflection faces draw from early next frame: this commit's inputs and buffer backings.
		a_resources->committed[shapeIndex] = { frameNumber, cull.inputCount, adopted.Serial() };

		(void)latched.Finish();
		revisions.latchedMisses += latched.Misses();
		SubmitRevisionRecording(revisionEpoch, *a_revision.recordings, 0);
		lap(6);
		return true;
	}

}

