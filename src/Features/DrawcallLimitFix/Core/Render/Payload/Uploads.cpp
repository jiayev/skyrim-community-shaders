// The main epochs' commit (render thread, inside the epoch): stream views, CommitMainPayload (the frame's values
// into the selected revision's shape) and the depth commit's scene uploads; DecideTreeLod; and the fade write-back (armed by the
// depth commit, judged on DCLF's executor by KickFadeWriteBack, its stores made on the main thread by WriteFadeStores).
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/GpuStages.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/LightLimitFix/ORGLightCulling.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Draws.h"

#include "Features/DrawcallLimitFix/Engine/Capture/SceneTracker.h"

namespace DCLF
{
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
			Extensions::WriteFrameBindings(bindings, frameRecord.textures);
			// The fade states and the roots' static rows every draw's alpha reads.
			if (const std::uint32_t roots = adopted.Of(sceneBuffers.fadeRoots).srvIndex) {
				frameRecord.textures[kFadeStatesRegister] = sceneBuffers.FadeStatesReadIndex(adopted, a_commit.frameNumber);
				frameRecord.textures[kFadeRootsRegister] = roots;
			}
			a_commit.latched(resources.frameConstants, &frameRecord, sizeof(frameRecord), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
		}

		// The frame slots, the shared light block and the frame-wide values in PS/VS b13 (lighting, LOD fades, fog, extras, effects,
		// foliage).
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
			// The camera planes soft effects read (PS b13 c17, DCLFEffectCamera).
			const auto effectCamera = SampleEffectCamera();
			latched(resources->frameConstants, effectCamera.data(), sizeof(effectCamera), kEffectCameraFrameOffset);
			// The sun's effect colour (PS b13 c18, DCLFEffectDLightColor).
			const auto effectLight = SampleEffectLight();
			latched(resources->frameConstants, effectLight.data(), sizeof(effectLight), kEffectLightFrameOffset);
			// Blood's AlphaTestRef.yz (PS b13 c19, DCLFEffectBlood): the weapon blood's two globals.
			const auto effectBlood = SampleEffectBlood();
			latched(resources->frameConstants, effectBlood.data(), sizeof(effectBlood), kEffectBloodFrameOffset);
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

		// Once a frame, FadeStateCS's inputs: the list jobs' camera (PrimaryCull::FadeEye), the engine's fade globals and the
		// animation job's batch (its roots' words sent), and the log window.
		void SampleFadeInputs(const MainCommit& a_commit, SceneBuffers& a_buffers, std::uint32_t a_rootSlots, FadeFrame& a_inputs, std::uint32_t& a_logBase)
		{
			const auto& adopted = a_commit.adopted;
			auto& uploads = a_commit.uploads;
			a_inputs = FadeState::SampleFrame(nullptr);
			const auto eye = PrimaryCull::Get().FadeEye();
			std::copy_n(eye.data(), 3, a_inputs.eye);
			a_inputs.lodAdjust = eye[3];
			// The animation job's updates since the last cull: roots stamped with this frame, inputs in the frame row.
			std::vector<PrimaryCull::AnimatedFadeRoot> roots;
			AnimatedFadeInputs anim{};
			PrimaryCull::Get().TakeAnimatedBatch(roots, anim);
			std::copy_n(anim.eye, 3, a_inputs.animEye);
			a_inputs.animLodAdjust = anim.lodAdjust;
			a_inputs.animCounter = anim.counter;
			a_inputs.animDeltaTime = anim.deltaTime;
			// Each root's updates counted in a dense array kept across frames (only the roots this batch names are touched, and
			// cleared again). The batch names root slots resolved by the animation job against the writer tables; the frame's tables
			// may be newer, so a slot counts only where they still list that node at that generation (no lookup, no hashing).
			const auto& batchTables = Scene::FrameView::Get().GetTables();
			auto& counts = a_buffers.fadeAnimatedCounts;
			if (counts.size() < a_rootSlots)
				counts.resize(a_rootSlots, 0u);
			std::vector<std::uint32_t> stamped;
			if (a_buffers.fadeAnimated && anim.valid) {
				for (const auto& root : roots) {
					if (root.slot >= a_rootSlots || root.slot >= batchTables.fadeRoots.size() || root.slot >= batchTables.fadeRootNode.size() ||
						batchTables.fadeRootNode[root.slot] != root.node || batchTables.fadeRoots[root.slot].generation != root.generation)
						continue;
					if (counts[root.slot]++ == 0)
						stamped.push_back(root.slot);
				}
			}
			auto& animatedWords = a_buffers.fadeAnimatedMirror;
			if (animatedWords.size() < a_rootSlots)
				animatedWords.resize(a_rootSlots, 0u);
			for (const std::uint32_t root : stamped) {
				animatedWords[root] = FadeAnimatedWord(Scene::FrameView::Get().GetFrame(), counts[root]);
				counts[root] = 0;
			}
			std::sort(stamped.begin(), stamped.end());
			// A version not filled is sent the whole mirror.
			if (const auto& animated = adopted.Of(a_buffers.fadeAnimated); !a_buffers.fadeAnimatedFilled.Holds(animated)) {
				stamped.resize(std::min<std::size_t>(animatedWords.size(), animated.units));
				std::iota(stamped.begin(), stamped.end(), 0u);
				a_buffers.fadeAnimatedFilled.Filled(animated);
			}
			SendWordRuns(stamped, [&](std::uint32_t a_first, std::uint32_t a_count) {
				uploads(a_buffers.fadeAnimated, animatedWords.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
			});
			if (anim.varied)
				Diagnostics::Notify(Diagnostics::FadeInputsVaried{ Scene::FrameView::Get().GetFrame() });
			// FadeStateCS's log window, when an observer asks.
			a_logBase = ~0u;
			Diagnostics::Notify(Diagnostics::FadeLogWindow{ Scene::FrameView::Get().GetFrame(), Scene::FrameView::Get().GetTables(), a_inputs, a_logBase });
		}

		// Each root's portal program (its room's), when the roots, a room's children or the portal graph changed. With the portals
		// unchanged and the version holding the mirror, only the roots written since the mirror's content epoch (the roots' journal)
		// and the new ones are judged again; a portal change, or a version the mirror is not of, judges every root.
		void UploadFadeRootPrograms(const MainCommit& a_commit, SceneBuffers& a_buffers)
		{
			const auto& adopted = a_commit.adopted;
			auto& uploads = a_commit.uploads;
			const auto& rootTables = Scene::FrameView::Get().GetTables();
			auto& portals = PortalViews::Get();
			const std::uint64_t rootsEpoch = rootTables.FadeRootsContentEpoch(), portalsVersion = portals.ProgramsVersion();
			const std::uint64_t programsKey = rootsEpoch * 0x9E3779B97F4A7C15ull ^ portalsVersion;
			const auto& programsVersion = adopted.Of(a_buffers.fadeRootPrograms);
			const auto programsFilledKey = a_buffers.fadeRootProgramsFilled.Since(programsVersion);
			const std::size_t rootCount = rootTables.fadeRootNode.size();
			if (programsFilledKey != programsKey && rootCount <= programsVersion.units) {
				auto& sent = a_buffers.fadeRootProgramsMirror;
				const auto judged = a_buffers.fadeRootProgramsMirrorOf;
				const bool incremental = programsFilledKey && *programsFilledKey == judged.key && judged.portals == portalsVersion && sent.size() <= rootCount;
				if (!programsFilledKey)
					sent.clear();
				// Only words that differ (all of them for a version not filled); the mirror takes the new words.
				std::vector<std::uint32_t> differing;
				const auto judge = [&](std::size_t a_root) {
					const auto* node = static_cast<const RE::NiAVObject*>(rootTables.fadeRootNode[a_root]);
					const std::uint32_t program = node ? portals.ProgramOf(node) : kPortalNoProgram;
					if (a_root < sent.size() && sent[a_root] == program)
						return;
					if (a_root >= sent.size())
						sent.resize(a_root + 1, ~program);
					sent[a_root] = program;
					differing.push_back(static_cast<std::uint32_t>(a_root));
				};
				if (incremental) {
					const std::size_t before = sent.size();
					rootTables.fadeRootsJournal.Take().ForEachRun(judged.rootsEpoch, before, [&](std::uint64_t a_first, std::uint64_t a_count) {
						for (std::uint64_t r = a_first; r < a_first + a_count && r < before; ++r)
							judge(static_cast<std::size_t>(r));
					});
					for (std::size_t r = before; r < rootCount; ++r)
						judge(r);
					std::sort(differing.begin(), differing.end());
					differing.erase(std::unique(differing.begin(), differing.end()), differing.end());
				} else {
					sent.resize(std::min(sent.size(), rootCount));
					for (std::size_t r = 0; r < rootCount; ++r)
						judge(r);
				}
				SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					uploads(a_buffers.fadeRootPrograms, sent.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				a_buffers.fadeRootProgramsFilled.Filled(programsVersion, programsKey);
				a_buffers.fadeRootProgramsMirrorOf = { programsKey, rootsEpoch, portalsVersion };
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
			a_latch.fadeSeedsIndex = FrameValues::Get().FadeSeedsIndex();
			// The depth segment: its stage word, for the feature stage before the depth build that reads one (Api::GpuStage::latchWord).
			a_latch.stageWord = depthOnly ? Extensions::StageLatchWord(Api::GpuStageSlot::DepthSegmentBeforeBuild) : 0u;
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
				// Terrain Blending's near test with its depth-pass eye, for the terrain group 1 twin buckets (kBucketTerrainPair).
				if (TerrainBlendingDefersTerrain()) {
					const auto& terrainEye = globals::features::terrainBlending.eyePosition;
					a_latch.terrainNear[0] = terrainEye.x;
					a_latch.terrainNear[1] = terrainEye.y;
					a_latch.terrainNear[2] = terrainEye.z;
					a_latch.terrainNear[3] = TerrainBlending::kTerrainDepthDistance;
				}
				Diagnostics::Notify(Diagnostics::CullLatchWritten{ a_latch, true });
			} else {
				a_latch.terrainCapacity[0] = shape.terrainCapacity[0];
				a_latch.terrainCapacity[1] = shape.terrainCapacity[1];
			}
			// Both segments read LOD skin levels from the same fade states.
			a_latch.fadeStatesIndex = resources->scene->FadeStatesReadIndex(a_commit.adopted, frameNumber);
		}
	}

	// The colour pass's cascades and local shadow volumes (the sun has accumulated, shadow maps are drawn), clamped to the latch
	// the producer sized (ReserveMainLatch).
	void IndirectDraws::Impl::CommitShadowInputs(bool a_depthOnly, const PassFrame& a_shape, std::uint32_t a_revisionEpoch)
	{
		if (!a_depthOnly) {
			SunAccumulation::Get().GpuCascades(sunCascades);
			// Frames with no cascades draw sun-lit passes unshadowed (right only when the sun did not accumulate).
			sunTestEmpty += sunCascades.empty() ? 1 : 0;
			if (sunTestWindow.Tick())
				Reports::ColourSunTestLine(std::exchange(sunTestEmpty, 0));
			// Local shadow lights accumulated this frame, a volume per shadowmap descriptor, for Light Limit Fix's shadow mask.
			localShadows = LocalShadowLights::Sample();
			shadowVolumes.clear();
			for (const auto& light : localShadows.lights) {
				for (const auto& volume : light.volumes) {
					auto& out = shadowVolumes.emplace_back();
					out.masks[0] = volume.masks[0];
					out.masks[1] = volume.masks[1];
					out.maskBit = light.maskBit;
					out.affectsLand = light.affectsLand ? 1u : 0u;
					out.sphere[0] = light.center[0];
					out.sphere[1] = light.center[1];
					out.sphere[2] = light.center[2];
					out.sphere[3] = light.radius;
					std::memcpy(out.planes, volume.planes.data(), sizeof(out.planes));
				}
			}
		}
		if (sunCascades.size() > a_shape.latchLayout.cascades || shadowVolumes.size() > a_shape.latchLayout.shadowVolumes) {
			++revisions.latchClamped[a_revisionEpoch];
			sunCascades.resize(std::min<std::size_t>(sunCascades.size(), a_shape.latchLayout.cascades));
			shadowVolumes.resize(std::min<std::size_t>(shadowVolumes.size(), a_shape.latchLayout.shadowVolumes));
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
		// Terrain Blending's mask (t55) is its terrain depth, bound by its replay after this epoch, so named here rather than
		// captured. By the colour epoch it holds the Z-prepass's main depth (TerrainBlending::BlendPrepassDepths).
		constexpr std::uint32_t kTerrainBlendingMaskRegister = 55;
		const auto& terrainBlending = globals::features::terrainBlending;
		auto* const terrainMask = TerrainBlendingDefersTerrain() ? terrainBlending.terrainDepth.depthSRV : nullptr;
		// Soft effects' depth: the kPOST_ZPREPASS_COPY SetupTechnique binds for a Soft pass, which CS fills from the main depth
		// after the deferred pass, before the forward epoch.
		auto* const effectSoftDepth = globals::game::renderer ?
		                                  reinterpret_cast<ID3D11ShaderResourceView*>(
											  globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
		                                  nullptr;
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t) {
			auto& held = frameTextureBindings[t];
			auto* const view = t == kCharacterLightRegister                         ? Scene::FrameView::Get().GetFrameCapture().characterLightView :
			                   (t == kTerrainBlendingMaskRegister && terrainMask) ? terrainMask :
			                   t == kEffectSoftDepthRegister                      ? effectSoftDepth :
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
		if (epochResources->lightLimitFix && !depthOnly)
			ORGLightCulling::Get().GetShaderResourceIndices(a_frameTextures[kLightsRegister], a_frameTextures[kLightsRegister + 1], a_frameTextures[kLightsRegister + 2]);
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

	// Uploads run ahead of every pass of this epoch: face streams, the features' commit hooks and the tree LOD.
	void IndirectDraws::Impl::UploadSceneStreams(const MainCommit& a_commit)
	{
		const auto& adopted = a_commit.adopted;
		const auto& epochResources = a_commit.resources;
		const bool depthOnly = a_commit.depthOnly;
		UploadFaceStreams(adopted, a_commit.payload.faceStreams, epochResources->scene->facePositions, epochResources->scene->faceUploaded, a_commit.uploads);
		Extensions::RunCommitHooks({ depthOnly ? Api::CommitKind::Depth : Api::CommitKind::Colour, adopted, Scene::FrameView::Get().GetTables(),
			Scene::FrameView::Get().GetFrame(), *epochResources->scene, a_commit.uploads, &a_commit.latched, a_commit.bindingOwners });
		// Tree LOD (dclf-lod.md): the mirror's changes, draw row and list arguments, for the depth cull and both passes' draws.
		if (depthOnly && epochResources->scene->treeLodCull) {
			auto& sceneBuffers = *epochResources->scene;
			UploadTreeLod(adopted, Scene::FrameView::Get().TreeLodMirror(), sceneBuffers, a_commit.latched, a_commit.bindingOwners, treeLodOwned);
			// The engine's passes were withheld: a commit that cannot draw leaves a hole.
			if (treeLodOwned && !sceneBuffers.treeLodReady.load(std::memory_order_acquire))
				Diagnostics::Notify(Diagnostics::TreeLodMissed{ a_commit.frameNumber, Scene::FrameView::Get().TreeLodMirror().ShapeSlots(), adopted.Of(sceneBuffers.treeLodShapes).units });
		}
	}

	// Fade roots and, once a frame, their inputs for FadeStateCS; the fade and visibility readbacks armed; the roots' portal programs.
	void IndirectDraws::Impl::CommitFadeRoots(const MainCommit& a_commit)
	{
		const auto& adopted = a_commit.adopted;
		auto& uploads = a_commit.uploads;
		auto& buffers = *a_commit.resources->scene;
		const auto rootSlots = static_cast<std::uint32_t>(adopted.Of(buffers.fadeRoots).units);
		std::uint32_t logBase = buffers.fadeLogBase;
		FadeFrame inputs = buffers.fadeFrame;
		if (buffers.fadeFrameNumber != Scene::FrameView::Get().GetFrame())
			SampleFadeInputs(a_commit, buffers, rootSlots, inputs, logBase);
		UploadFadeRoots(adopted, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetFrame(), inputs, logBase, buffers, uploads, PortalViews::Get().Encoded());
		Scene::FrameView::Get().FadeRootsSent(buffers.FadeRootsContentEpoch(adopted));
		// The fade events' header and readback slot (its last execution harvested first).
		ArmFadeReadback(buffers, uploads);
		ArmVisibilityReadback(buffers, uploads);
		UploadFadeRootPrograms(a_commit, buffers);
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
		// Likewise the HZB's group counter.
		if (depthOnly && epochResources->hzbCounter && !epochResources->hzbCounterZeroed) {
			static constexpr std::uint32_t kZero = 0;
			uploads(epochResources->hzbCounter, &kZero, sizeof(kZero), 0);
			epochResources->hzbCounterZeroed = true;
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
		const std::uint32_t volumeHeader[4] = { static_cast<std::uint32_t>(shadowVolumes.size()), 0, 0, 0 };
		LatchWrite(latchBlock, "main shadow volumes", a_latchSlot, layout.ShadowVolumeOffset(), std::as_bytes(std::span(volumeHeader)));
		if (!shadowVolumes.empty())
			LatchWrite(latchBlock, "main shadow volumes", a_latchSlot, layout.ShadowVolumeOffset() + kSunRegionHeader, std::as_bytes(std::span(shadowVolumes)));
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
		const auto revisionEpoch = static_cast<std::uint32_t>(latchedShape);
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
		UploadSceneStreams(commit);
		if (depthOnly && a_resources->scene->fadeState)
			CommitFadeRoots(commit);
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


namespace DCLF
{
	bool IndirectDraws::DecideTreeLod()
	{
		bool owned = false;
		auto* buffers = impl->scene.get();
		// A frame without claims is the engine's whole, tree LOD included; so is one whose family (its tree LOD class) is not taken or off.
		const auto& family = Extensions::DrawFamilyOf(Api::DrawFamily::TreeInstances);
		if (buffers && buffers->treeLodCull && Extensions::DrawFamilyOn(ActiveToggles(), Api::DrawFamily::TreeInstances) && Scene::FrameView::Get().TreeLodMirror().Size() &&
			!Scene::FrameView::Get().SetWithdrawn()) {
			// The pipeline lane builds its pipelines (the family's program source, the DistantTree shader, as input); until the frame's
			// catalog has them the engine draws tree LOD.
			auto& drawPipelines = DrawPipelines::Get();
			if (auto* shader = static_cast<RE::BSShader*>(family.contribution.frame.programSource())) {
				drawPipelines.SetTreeLodInputs(*shader);
				drawPipelines.RequestTreeLod();
			}
			const auto* catalog = FrameCatalog();
			const auto* texture = Engine::Global<RE::NiSourceTexture*>(Engine::Address::kTreeLodAtlas);  // its tree LOD atlas (UploadTreeLod)
			TreeLodPipelines pipelines;
			const bool built = TreeLodPipelinesOf(catalog, pipelines);
			if (built && texture && texture->rendererTexture && texture->rendererTexture->resourceView) {
				const auto published = buffers->treeLodPipelines.load(std::memory_order_acquire);
				if (!published || !SameHandle(published->depth, pipelines.depth) || !SameHandle(published->colour, pipelines.colour))
					buffers->treeLodPipelines.store(std::make_shared<const TreeLodPipelines>(pipelines), std::memory_order_release);
				owned = true;
			}
			drawPipelines.NoteFrameWaits(!built && !(catalog && catalog->treeLod.failed), false);
		}
		impl->treeLodOwned = owned;
		PassCapture::Get().SetTreeLodOwned(owned);
		return owned;
	}

	void IndirectDraws::WriteFadeStores(bool a_live, const std::function<bool(const void*)>& a_written)
	{
		if (!impl->scene || !impl->scene->fadeWriteBack)
			return;
		auto& writeBack = *impl->scene->fadeWriteBack;
		// The last task's stores, made on the main thread at Main::Update's entry (no list cull, cell or job animation runs; every
		// reader of the node sees them this frame), only on roots DCLF still writes (a_written; the engine steps the rest: one
		// writer). Their snapshot is released here.
		std::unique_ptr<FadeWriteBack::Stores> stores{ writeBack.ready.exchange(nullptr, std::memory_order_acq_rel) };
		if (!stores)
			return;
		// DCLF is not running: the engine culls every root this frame.
		if (!a_live) {
			writeBack.dropped.fetch_add(stores->stores.size(), std::memory_order_relaxed);
			return;
		}
		ZoneScopedN("CS.DCLF.FadeWriteBack.Stores");
		const auto start = std::chrono::steady_clock::now();
		std::uint64_t applied = 0, notWritten = 0;
		for (const auto& store : stores->stores) {
			if (!a_written(store.node)) {
				++notWritten;
				continue;
			}
			// The fade and the flags' fade bits (atomically: the engine owns the others). Stood-in roots' dependents read the fade
			// from FadeStateCS's state, so the fade watch is not told.
			auto* node = static_cast<std::byte*>(store.node);
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(node + 0xF4));
			flags.fetch_and(~kFadeFlagMask, std::memory_order_relaxed);
			flags.fetch_or(store.flags, std::memory_order_relaxed);
			*reinterpret_cast<float*>(node + 0x130) = store.fade;
			// A leaf or tree root's LOD level, transition and blend (written otherwise only by FUN_14147a430 and OnVisible, neither
			// running now).
			std::uint32_t fields = SceneCapture::NodeRecord::kFadeCurrent;
			if (store.lod) {
				*reinterpret_cast<float*>(node + 0x14C) = store.blend;
				*reinterpret_cast<std::uint8_t*>(node + 0x152) = static_cast<std::uint8_t>(store.levels);
				*reinterpret_cast<std::uint8_t*>(node + 0x153) = static_cast<std::uint8_t>(store.levels >> 8);
				fields |= SceneCapture::NodeRecord::kFadeLevel;
			}
			// The mirror's value: an update, not a fade event.
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ fields, SceneCapture::CaptureNodeFields(*static_cast<const RE::NiAVObject*>(store.node), fields) });
			++applied;
		}
		writeBack.applied.fetch_add(applied, std::memory_order_relaxed);
		writeBack.notWritten.fetch_add(notWritten, std::memory_order_relaxed);
		writeBack.storeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	void IndirectDraws::Impl::ArmFadeReadback(SceneBuffers& a_scene, CommitUploads& a_uploads)
	{
		if (!a_scene.fadeWriteBack || !a_scene.fadeEvents)
			return;
		auto& writeBack = *a_scene.fadeWriteBack;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		const std::uint32_t frameSlot = host->CurrentFrameSlot();
		if (frameSlot >= writeBack.armed.size())
			return;
		// Harvests the slot's last execution (the host waited for it) if its buffer holds the frame that armed it. If the epoch did
		// not run for that frame, FadeStateCS did not report either: the next execution reports those changes. Roots past the
		// list's end report again next frame while the list grows (RequireSceneTables).
		if (auto& armed = writeBack.armed[frameSlot]; armed.armed) {
			armed.armed = false;
			writeBack.Harvest(frameSlot, armed.frame, [&](const std::uint32_t* a_words, std::uint32_t a_count, std::uint32_t a_capacity) {
				if (const std::uint32_t events = std::min(a_count, a_capacity)) {
					auto* batch = new FadeWriteBack::Batch();
					batch->frame = armed.frame;
					const auto* list = reinterpret_cast<const FadeEvent*>(a_words + kFadeEventHeaderWords);
					batch->events.assign(list, list + events);
					writeBack.Push(batch);
					writeBack.harvestedWithEvents.fetch_add(1, std::memory_order_relaxed);
					writeBack.events.fetch_add(events, std::memory_order_relaxed);
				}
				writeBack.harvested.fetch_add(1, std::memory_order_relaxed);
			});
		}
		// The header FadeStateCS appends after: the count and the frame the harvest matches.
		const std::uint32_t header[kFadeEventHeaderWords]{ 0u, Scene::FrameView::Get().GetFrame(), 0u, 0u };
		a_uploads(a_scene.fadeEvents, header, sizeof(header), 0);
		auto& armed = writeBack.armed[frameSlot];
		armed.armed = true;
		armed.frame = Scene::FrameView::Get().GetFrame();
	}

	void IndirectDraws::KickFadeWriteBack()
	{
		if (!impl->scene || !impl->scene->fadeWriteBack)
			return;
		auto& writeBack = *impl->scene->fadeWriteBack;
		// One task at a time; a running one (or the next) takes the new batches.
		if (bool idle = false; !writeBack.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
			writeBack.busy.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		// The task judges events against the frame's immutable tables, which the stores hold (and with them the root nodes).
		std::shared_ptr<const SceneTables> tables = Scene::FrameView::Get().AcceptedTables();
		auto find = [scene = impl->scene, tables] {
			ZoneScopedN("CS.DCLF.FadeWriteBack");
			auto& state = *scene->fadeWriteBack;
			// The batches in frame order (the stack is newest first, and recordings may finish out of order).
			std::vector<FadeWriteBack::Batch*> batches;
			for (auto* batch = state.batches.exchange(nullptr, std::memory_order_acquire); batch; batch = batch->next)
				batches.push_back(batch);
			std::reverse(batches.begin(), batches.end());
			std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
			auto stores = std::make_unique<FadeWriteBack::Stores>();
			stores->tables = tables;
			for (auto* batch : batches) {
				for (const auto& event : batch->events) {
					if (!tables)
						break;
					if (event.root >= state.appliedFrame.size())
						state.appliedFrame.resize(std::size_t(event.root) + 1, 0u);
					// Only a root still DCLF's to write (same generation, kFadeRootWritten), and never an event older than the last taken.
					if (event.root >= tables->fadeRoots.size() || tables->fadeRoots[event.root].generation != event.generation ||
						!(tables->fadeRoots[event.root].bits & kFadeRootWritten) || !tables->fadeRootNode[event.root] || batch->frame < state.appliedFrame[event.root]) {
						state.stale.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					state.appliedFrame[event.root] = batch->frame;
					const std::uint32_t plan = tables->fadeRoots[event.root].bits & kFadeRootPlanMask;
					stores->stores.push_back({ const_cast<void*>(tables->fadeRootNode[event.root]), event.flags & kFadeFlagMask, event.currentFade,
						plan == kFadeRootLeaf || plan == kFadeRootTree, event.levels, event.blend });
				}
				delete batch;
			}
			// Stores an earlier task found that no Main::Update entry has made yet go first.
			if (std::unique_ptr<FadeWriteBack::Stores> earlier{ state.ready.exchange(nullptr, std::memory_order_acq_rel) }) {
				earlier->stores.insert(earlier->stores.end(), stores->stores.begin(), stores->stores.end());
				earlier->tables = std::move(stores->tables);
				stores = std::move(earlier);
			}
			if (!stores->stores.empty())
				state.ready.store(stores.release(), std::memory_order_release);
			state.running.store(false, std::memory_order_release);
			state.running.notify_all();
		};
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "fade write-back",
				[find = std::move(find)](const auto&) { find(); }))
			stl::report_and_fail("Drawcall Limit Fix: the fade write-back was refused by DCLF's executor");
	}

	void IndirectDraws::Impl::WaitFadeWriteBack()
	{
		if (!scene || !scene->fadeWriteBack)
			return;
		auto& running = scene->fadeWriteBack->running;
		while (running.load(std::memory_order_acquire))
			running.wait(true, std::memory_order_acquire);
		// Teardown, toggle or load: what it found is dropped (the roots may no longer stand in).
		delete scene->fadeWriteBack->ready.exchange(nullptr, std::memory_order_acq_rel);
	}
}
