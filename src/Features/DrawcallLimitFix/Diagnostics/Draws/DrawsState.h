#pragma once

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#	include "DrawsView.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief The draws area's diagnostics state: what its observers keep between their hook points (the readbacks in flight, their
	 * counters, the histories), owned here and written only on the observers' thread (the render thread, but where a member says
	 * otherwise). Nothing of the main path reads it.
	 */
	struct DrawsState
	{
		static DrawsState& Get()
		{
			static DrawsState state;
			return state;
		}

		// CS_DCLF_GBUFFER_PROBE=<x>x<y>: the texel the in-epoch probe passes copy out, read back a few
		// frames later from the buffer they wrote.
		winrt::com_ptr<ID3D11Buffer> gbufferStaging;
		std::uint32_t gbufferFramesLeft = 0;
		std::uint32_t gbufferEpochs = 0;
		std::uint32_t gbufferCount = 0;
		std::uint32_t gbufferX = 0, gbufferY = 0;
		std::array<std::uint32_t, kColorTargets> gbufferBytes{};
		// The main pass's targets (its capture's: MainPassCaptured), so the probe's texels are printed in their formats.
		std::array<winrt::com_ptr<ID3D11Texture2D>, kColorTargets> probeTargets;
		std::uint32_t probeTargetCount = 0;

		// CS_DCLF_BUILD_PARITY: GPU output copied to staging after an epoch, compared a few frames later.
		struct ParityReadback
		{
			winrt::com_ptr<ID3D11Buffer> sequences, count;
			std::vector<DrawSequence> expected;
			// Per decal group, by slot: what the CPU expects there (a culled slot reads back with an index
			// count of zero and is otherwise identical).
			std::array<std::vector<DrawSequence>, kDecalGroups> expectedDecals;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // the buffer's ranges when it was copied
			std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};  // and its terrain groups' (MainSizing::terrainCapacity)
			std::uint32_t framesLeft = 0;
			// By object: the local shadow lights the CPU selects with the volumes the epoch uploaded (LocalShadowLights), which
			// BuildDraws put in the draw's object word (kObjectLocalShadowMask); none on the Z-prepass.
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> expectedLocalShadows;
		};
		std::optional<ParityReadback> parity;
		std::uint32_t parityEpochs = 0;
		std::uint32_t sequenceChecks = 0, sequenceMismatches = 0;  // the checks made, and those with a difference, since the start

		// The culling's own counters, read back from the draw-count buffer a few frames after the epoch
		// wrote them. Always on: the per-phase counts are what says whether the culling is doing anything,
		// and the buffer is two words.
		struct CullReadback
		{
			winrt::com_ptr<ID3D11Buffer> count;
			std::uint32_t framesLeft = 0;
			std::uint32_t sunCpuTested = 0, sunCpuMissed = 0;
			// CS_DCLF_SET_PARITY: the CPU's terrain near test over the same frame's depth inputs, eye and distance (<- TB NEAR).
			std::uint32_t terrainCpuTested = 0, terrainCpuNear = 0;
			bool terrainCpu = false;
			// CS_DCLF_SET_PARITY: the engine's blended passes of members registered for the copied frame (Registrations::
			// TakeForward), against its forward inputs in view (<- FORWARD).
			std::uint32_t forwardEngine = 0;
			bool forwardObserved = false;
			// CS_DCLF_SET_PARITY: the engine's refraction passes (hint 4) of members registered for the copied frame
			// (Registrations::TakeRefraction), against its refraction inputs in view (<- REFRACTION).
			std::uint32_t refractionEngine = 0;
			bool refractionObserved = false;
		};
		std::optional<CullReadback> cullReadback;
		std::uint32_t cullEpochs = 0;
		// The colour commit's last culling latch (CullLatchWritten: its sun test), and the depth commit's last terrain near test.
		BuildDrawsLatch sunUpload{};
		std::array<float, 4> terrainNearUpload{};
		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY: tree wind parity. Every 120th colour epoch the records of up to 64 tree members are
		 * read back (3 frames later) and checked: TreeParams.y and .w against the frame's wind magnitude and the node's leaf
		 * frequency, the amplitude's fade against its own distance, and the gust against the engine's formula at the record's
		 * timer; and the timers' drift from the engine's own clock on the node is reported.
		 */
		struct TreeReadback
		{
			winrt::com_ptr<ID3D11Buffer> records;
			std::uint32_t framesLeft = 0;
			struct Sample
			{
				std::uint32_t object = 0, tree = 0;
				const void* node = nullptr;
			};
			std::vector<Sample> samples;
			TreeWindFrame inputs{};
		};
		std::optional<TreeReadback> treeReadback;
		ankerl::unordered_dense::map<const void*, std::pair<float, float>> treeTimers;  // node -> engine's and GPU's timers at the last readback
		std::uint32_t treeEpochs = 0;

		/**
		 * @brief CS_DCLF_FADE_PARITY: FadeStateCS against the C++ port (Scene/FadeState.h). Every 30th frame the depth commit
		 * has the pass log kFadeLogEntries roots from a rotating cursor (NextFadeLog keeps their static rows and the frame's
		 * inputs, FadeLogWindow); the log is copied after the depth epoch and checked three frames later (ReadFadeLog): each logged update
		 * made again by the port from the state it started from, with the GPU's frustum verdict.
		 */
		struct FadeReadback
		{
			winrt::com_ptr<ID3D11Buffer> log;
			std::uint32_t frame = 0, base = 0, framesLeft = 0;
			std::vector<FadeRootStatic> roots;  // the static rows from base, as uploaded that frame
			FadeFrame inputs{};
			// An owned root with engine-drawn parts (not stood in): its node after the engine's own OnVisible that frame (the
			// list jobs have run), which FadeStateCS's update for its members must equal.
			std::vector<FadeNodeState> nodes;
			std::vector<std::uint8_t> engine;
			std::array<float, 4> worldCamera{};
			std::vector<std::array<float, 3>> nodeCentres;  // the nodes' world bound centres, read with them
			std::vector<std::string> nodeNames;
			std::vector<std::uint32_t> nodeFlags;
			// What FadeStateCS was given that frame: the portal programs, each root's program and the radius it read (the sun entry row).
			std::vector<std::uint32_t> programs;
			std::vector<std::uint32_t> rootPrograms;
			std::vector<float> radii, nodeRadii;  // the engine's world root camera when the nodes were read: position, lodAdjust
		};
		std::optional<FadeReadback> fadeReadback;
		std::uint32_t fadeLogCursor = 0;
		struct FadeParity
		{
			std::uint64_t logs = 0, updates = 0, inView = 0, serviced = 0, exact = 0, rounding = 0, differ = 0;
			std::uint64_t engineChecked = 0, engineExact = 0, engineRounding = 0, engineDiffer = 0;  // the GPU's state against the engine's node
			// The log entries the check skipped, by why: not the row asked for, another frame's update, another listing's (or none).
			std::uint64_t skippedRoot = 0, skippedFrame = 0, skippedGeneration = 0;
			// The rows' inputs against the nodes (fadeAmount, +0x109): a node whose value held since the last check while its row
			// differs has a writer DCLF has no event for.
			std::uint64_t inputChecks = 0, inputRoots = 0, inputMoving = 0, inputDiffer = 0;
			std::string first, engineFirst, skippedFirst, inputFirst;
		};
		FadeParity fadeParity;
		// The nodes' fadeAmount and +0x109 at the last input check, by node.
		ankerl::unordered_dense::map<const void*, std::pair<float, std::uint8_t>> fadeInputsSeen;
		// The log window every other time: a root the set parity last saw a member of rejected by the fade test with its live bound
		// in view (CS_DCLF_SET_PARITY), so that its FadeStateCS state is logged against the port and the engine's node.
		std::atomic<std::uint32_t> fadeLogFocus{ ~0u };
		std::uint32_t fadeLogTurn = 0;

		// CS_DCLF_SET_PARITY: one snapshot per frame in flight, read back a few frames later.
		struct SetParityFrame
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			std::uint32_t frame = 0;
			std::uint32_t framesLeft = 0;
			std::vector<std::uint8_t> depthState, colourState;  // MainPayload::objectState of the two builds
			// Per object: bit 0 in the frame's set (the main phase), bit 1 bound, bit 2 alpha tested, bit 3 its record's kObjectMember
			// disagrees with the set.
			std::vector<std::uint8_t> flags;
			std::vector<const RE::BSGeometry*> geometry;
			// The published tables the snapshot read (render thread): held, they keep every geometry they name alive (the retirement
			// chain), so the readback frames later reads them without asking the scene lane's live state.
			std::shared_ptr<const SceneTables> tables;
			// The frustum stamps (the fade test's drops), and per set member whether its live bound is inside the depth build's
			// frustum (the engine's verdict for it, occlusion and fading aside), with that bound: a rejection of one inside is not
			// the culling's to make.
			winrt::com_ptr<ID3D11Buffer> frustumStaging;
			std::vector<std::uint8_t> inView;
			std::vector<std::array<float, 4>> bound;
			// Per object, its geometry's phases in the claims the registration hooks withhold by (PassCapture::CurrentSet): the main
			// claim must be the frame's set exactly, or an object is drawn by nobody (claimed, not in the set) or twice.
			std::vector<std::uint8_t> claims;
		};
		std::deque<SetParityFrame> setParityFrames;
		std::vector<winrt::com_ptr<ID3D11Buffer>> setParityStaging;  // released stagings, reused
		struct SetParityCounts
		{
			std::uint32_t frames = 0, framesWithDamage = 0, skipped = 0;
			std::uint32_t depthOnly = 0, colourOnly = 0, colourUnpublished = 0, withheldUndrawn = 0, withheldCulled = 0;
			std::uint32_t colourDrawnTotal = 0, depthDrawnTotal = 0;
			std::uint32_t alphaDepthOnly = 0, alphaColourOnly = 0, alphaWithheldUndrawn = 0;
			// Drawn by DCLF outside the set (the engine draws it too: a double draw), and records whose kObjectMember is not the set.
			std::uint32_t outsideDrawn = 0, recordDisagrees = 0;
			std::uint32_t samples = 0;
			// Gaps: a resident object drawn (by DCLF, or outside the set), then withheld and GPU-culled for one to eight frames, then
			// drawn again. Counted, not flagged: an occlusion rejection is against this frame's depth, which only gets nearer, so it is
			// hidden in the final image too (two-phase oscillation behind an occluder that moves between the phases is most of them).
			// By the first gap frame's verdict (occluded retest, rejected), and how many were trees.
			std::uint32_t gaps = 0, gapsRetest = 0, gapsRejected = 0, gapsTree = 0, gapSamples = 0;
			// By length (one, two, longer), by the first gap frame's rejection (kGapClasses), with the live bound in view then,
			// within 60 frames of the geometry's first resident frame, and outside the set before and after.
			std::array<std::uint32_t, 3> gapsByLength{};
			std::array<std::uint32_t, 7> gapsByClass{};
			std::uint32_t gapsInView = 0, gapsNew = 0, gapsAfterNative = 0, gapsBeforeNative = 0;
			// Set members rejected while their live bound was in view: by the frustum (the bound the culling read was not the live
			// one), by the fade test (a fade root DCLF owns, faded out), and how many of those were skinned.
			std::uint32_t rejectedInView = 0, fadeHiddenInView = 0, skinnedInView = 0, inViewSamples = 0, fadeSamples = 0;
			// Fade-rejected in view while the engine, still culling the root (not stood in), has its node fully faded in.
			std::uint32_t fadeContradicted = 0, fadeContradictedLast = 0;
			std::string fadeContradictedFirst;
			// The registration hooks' main claims against the frame's set: claimed and not in it (withheld from the engine and not
			// drawn by DCLF), in it and not claimed (drawn by both).
			std::uint32_t claimedOutside = 0, unclaimedMembers = 0, claimSamples = 0;
		} setParity;
		// Per geometry, for the gap detector: its last frame's state (0 not resident, 1 resident and drawn by DCLF or outside the set,
		// 2 resident, withheld and GPU-culled), and its first resident frame.
		struct GapHistory
		{
			std::uint32_t frame = 0, first = 0;
			std::uint8_t last = 0;
			// The run of withheld, GPU-culled frames in progress: its length, its first frame's class (kGapClasses) and verdict, whether
			// its live bound was in view then, and who had it before the run (1 outside the set, 2 DCLF); drawnBy, the last frame's.
			std::uint8_t run = 0, runClass = 0, runVerdict = 0, runInView = 0, drawnBy = 0, runDrawnBy = 0;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, GapHistory> gapHistory;

		// CS_DCLF_PERSISTENT_PARITY: the frame values' rows and palettes against the engine's (CheckFrameValues), since the last report.
		struct FrameValuesParity
		{
			std::uint64_t checks = 0, differs = 0, fresh = 0, paletteChecks = 0, paletteDiffers = 0;
			std::string first, paletteFirst;
		} frameValues;

		// CS_DCLF_PERSISTENT_PARITY: the kept stores' checks (KeptChecks.cpp), each on its store's thread, since the last report (the
		// snapshot builder's StoresReport and ProducerReport take them).
		struct Kept
		{
			ParityCounter objects, geometry, drawBound, sunExclusion, parabolicExclusion, shadow;
			// The per-frame shadow build's inputs the kept build lacks, by why, and the first of each.
			std::map<std::string, std::uint64_t> shadowMissingBy;
			std::map<std::string, std::string> shadowMissingFirst;
			// The extras rows as each buffer holds them (the scene buffers', the payload ring entries'), the scene buffers' check and its
			// first differing row since the report.
			std::vector<float> sceneExtras;
			std::array<std::vector<float>, DrawsView::Impl::kPayloadRing> ringExtras;
			ParityCounter extras;
			std::string extrasFirst;
			/**
			 * @brief The payload ring entries' checks (their producer's, on the uploader's thread). Lock-free: the counts are atomic, and the
			 * first differing row's text is written by the one producer that claims it (firstState 0 -> 1) and taken by the report once
			 * written (2 -> 0).
			 */
			struct Ring
			{
				std::atomic<std::uint64_t> checks{ 0 }, mismatches{ 0 };
				std::atomic<std::uint32_t> firstState{ 0 };  // 0 none, 1 being written, 2 written
				std::string first;
			} ring;
		} kept;

		// CS_DCLF_PERSISTENT_PARITY: the view masks (CheckViewMasks, the builds ahead's), taken by the render thread's report.
		struct ViewMasks
		{
			std::atomic<std::uint64_t> checks{ 0 }, inputs{ 0 }, differ{ 0 };
			// The first that differed: (list << 56) | (object << 24) | expected mask; and its mask.
			std::atomic<std::uint64_t> first{ ~0ull }, firstMask{ 0 };
		} viewMasks;

		// CS_DCLF_BINDLESS_PARITY: a main build's templates by pipeline and the groups' scratch (CheckBindless; one build at a time).
		struct Bindless
		{
			ankerl::unordered_dense::map<std::uint32_t, GeometryTemplate> templates;
			std::vector<std::byte> vs, ps;
		} bindless;
		// The main builds' checks (PayloadChecks) summed at their commits, since the start (render thread).
		struct BuildChecks
		{
			std::uint64_t bindless = 0, bindlessDiffer = 0;
			std::uint64_t resident = 0, residentDiffer = 0, residentMissing = 0, pairs = 0, pairsStale = 0;
		} buildChecks;

		// The shadow epochs' checks (ShadowChecks.cpp, render thread): the index pool's updates (CS_DCLF_BUILD_PARITY), the predicted
		// targets, each occlusion map's view (since the last report), the sun views' culling against the engine's cascade culls.
		std::uint32_t indexPoolChecks = 0;
		struct ShadowTargets
		{
			std::uint64_t checked = 0, differ = 0;
			std::string first;
		} shadowTargets;
		struct OcclusionView
		{
			std::uint32_t maps = 0, differ = 0;
			float largest = 0.0f;
			std::string first;
		};
		std::array<OcclusionView, kOcclusionViews> occlusionViews;
		struct CascadeCulling
		{
			std::uint64_t checks = 0, registered = 0, rejected = 0, offscreen = 0;
			std::string first;
			std::uint32_t coversLogged = 0;
		} cascadeCulling;

		// CS_DCLF_STATS: the colour epoch's ViewProj against its other copies (MeasureJitter), over 300 epochs.
		struct Jitter
		{
			std::uint32_t epochs = 0;
			float vsUnjittered = 0, vsUnjitteredMax = 0, vsMain = 0, vsMainMax = 0, vsCached = 0, vsCachedMax = 0, vsPrevious = 0, vsPreviousMax = 0;
			std::array<float, 16> previous{};
			bool havePrevious = false;
		} jitter;
		// The reflection's face cameras (CheckReflectionFace) since the last report: faces compared, differing (block, slice).
		struct FaceCameras
		{
			std::uint32_t faces = 0, blocks = 0, slices = 0;
			std::uint64_t registers = 0;  // the registers (c0-c44) that differed, by bit
			float largest = 0.0f;
			std::string first;
		} faceCameras;

		// CS_DCLF_PERSISTENT_PARITY: the catalog's constant tables against Community Shaders' (CheckConstantTables), since the last report.
		struct ConstantTables
		{
			std::uint32_t checks = 0;       // parity frames
			std::uint32_t entries = 0;      // resolved pipeline entries compared
			std::uint32_t uncached = 0;     // of them, ShaderCache had no shader object for yet (not compared)
			std::uint32_t variables = 0;    // variables in both tables, compared
			std::uint32_t differ = 0;       // of them, at another offset
			std::uint32_t catalogOnly = 0;  // in DCLF's table alone (its stage reads one the game's does not declare)
			std::uint32_t cacheOnly = 0;    // in ShaderCache's alone (DCLF_BINDLESS's own blocks, or a block the stage never reads)
			std::string first, catalogOnlyFirst;
		} constantTables;
		// CS_DCLF_SET_PARITY: the first-person skeleton's slots and claimed phases as last logged (NoteFirstPersonSkeleton).
		std::string loggedSkeleton;

		// CS_DCLF_FOLIAGE_PARITY: the report (FoliageParityReport, on ORG's host thread: the mutex orders it with the next frame's): frames
		// read, frames with a defect, the counters' sums, the worst frame; terrain LOD's coverage.
		struct Foliage
		{
			std::uint64_t frames = 0, flagged = 0, inFrameFlagged = 0, whiteFlagged = 0;
			double whiteAverage = -1.0;  // the near-white foliage pixels' running average (kFoliageWhiteAlbedo + kFoliageWhiteDiffuse)
			std::uint32_t whiteLogged = 0, whiteMax = 0;
			double changeAverage = 0.0;  // the reprojected changes' running average (recoloured, brightened, whitened)
			std::uint32_t changeMax = 0;
			std::array<std::uint64_t, kFoliageCounters> totals{};
			std::uint32_t logged = 0, inFrameLogged = 0;
			std::uint32_t landFrames = 0, landMax = 0, landLogged = 0;
			std::mutex mutex;
		} foliage;

		// CS_DCLF_CAPTURE_POINT_PARITY (CheckCapturePoint), over the report window: the frames that differ, and how often each binding does.
		struct CapturePoint
		{
			Reports::Window window;
			std::uint32_t frames = 0;
			ankerl::unordered_dense::map<std::string, std::uint32_t> differ;
		} capturePoint;
	};
}
#endif
