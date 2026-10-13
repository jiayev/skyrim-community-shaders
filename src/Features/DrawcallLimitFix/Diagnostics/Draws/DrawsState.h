#pragma once

// DrawsState: the draw observers' state between hook points. Render thread unless noted.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#	include "DrawsView.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief The draws area's diagnostics state (readbacks in flight, counters, histories). The main path never reads it.
	 */
	struct DrawsState
	{
		static DrawsState& Get()
		{
			static DrawsState state;
			return state;
		}


		// CS_DCLF_BUILD_PARITY: GPU output copied to staging after an epoch, compared a few frames later.
		struct ParityReadback
		{
			winrt::com_ptr<ID3D11Buffer> sequences, count;
			std::vector<DrawSequence> expected;
			// Per decal group, by slot (a culled slot reads back with index count zero, otherwise identical).
			std::array<std::vector<DrawSequence>, kDecalGroups> expectedDecals;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // the buffer's ranges when copied
			std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
			std::uint32_t framesLeft = 0;
			// By object: the CPU's local shadow light selection (kObjectLocalShadowMask); none on the Z-prepass.
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> expectedLocalShadows;
		};
		std::optional<ParityReadback> parity;
		std::uint32_t parityEpochs = 0;

		// The culling counters, read back from the draw-count buffer a few frames later. Always on: the per-phase counts show
		// whether culling does anything, and the buffer is two words.
		struct CullReadback
		{
			winrt::com_ptr<ID3D11Buffer> count;
			std::uint32_t framesLeft = 0;
			std::uint32_t sunCpuTested = 0, sunCpuMissed = 0;
			// CS_DCLF_SET_PARITY, against the same frame: the CPU's terrain near test (<- TB NEAR), the engine's blended passes of
			// registered members (<- FORWARD) and its refraction passes, hint 4 (<- REFRACTION).
			std::uint32_t terrainCpuTested = 0, terrainCpuNear = 0;
			bool terrainCpu = false;
			std::uint32_t forwardEngine = 0;
			bool forwardObserved = false;
			std::uint32_t refractionEngine = 0;
			bool refractionObserved = false;
		};
		std::optional<CullReadback> cullReadback;
		std::uint32_t cullEpochs = 0;
		// The colour commit's last culling latch (sun test), and the depth commit's last terrain near test.
		BuildDrawsLatch sunUpload{};
		std::array<float, 4> terrainNearUpload{};
		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY tree wind: every 120th colour epoch, up to 64 tree members' records are read back and
		 * checked against the engine's formulas; timer drift is reported.
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
		 * @brief CS_DCLF_FADE_PARITY: FadeStateCS against the C++ port (Scene/Ports/FadeState.h). Every 30th frame the depth commit
		 * logs kFadeLogEntries roots from a rotating cursor (NextFadeLog); ReadFadeLog checks three frames later that the port,
		 * from each update's starting state and the GPU's frustum verdict, reproduces it.
		 */
		struct FadeReadback
		{
			winrt::com_ptr<ID3D11Buffer> log;
			std::uint32_t frame = 0, base = 0, framesLeft = 0;
			std::vector<FadeRootStatic> roots;  // static rows from base, as uploaded that frame
			FadeFrame inputs{};
			// For a root with engine-drawn parts (not stood in): its node after the engine's OnVisible that frame, which
			// FadeStateCS's update must equal.
			std::vector<FadeNodeState> nodes;
			std::vector<std::uint8_t> engine;
			std::array<float, 4> worldCamera{};
			std::vector<std::array<float, 3>> nodeCentres;  // world bound centres, read with the nodes
			std::vector<std::string> nodeNames;
			std::vector<std::uint32_t> nodeFlags;
			// FadeStateCS's inputs that frame: portal programs, each root's program and radius (the sun entry row).
			std::vector<std::uint32_t> programs;
			std::vector<std::uint32_t> rootPrograms;
			std::vector<float> radii, nodeRadii;  // the engine's world root camera when the nodes were read: position, lodAdjust
		};
		std::optional<FadeReadback> fadeReadback;
		std::uint32_t fadeLogCursor = 0;
		struct FadeParity
		{
			std::uint64_t logs = 0, updates = 0, inView = 0, serviced = 0, exact = 0, rounding = 0, differ = 0;
			std::uint64_t engineChecked = 0, engineExact = 0, engineRounding = 0, engineDiffer = 0;  // GPU state against the engine's node
			// Skipped log entries: wrong row, another frame's update, another listing's (or none).
			std::uint64_t skippedRoot = 0, skippedFrame = 0, skippedGeneration = 0;
			// Row inputs against the nodes (fadeAmount, +0x109): a steady node whose row differs has a writer without an event.
			std::uint64_t inputChecks = 0, inputRoots = 0, inputMoving = 0, inputDiffer = 0;
			std::string first, engineFirst, skippedFirst, inputFirst;
		};
		FadeParity fadeParity;
		// Per node: fadeAmount and +0x109 at the last input check.
		ankerl::unordered_dense::map<const void*, std::pair<float, std::uint8_t>> fadeInputsSeen;
		// Every other log window focuses on a root the set parity saw fade-rejected with its live bound in view (CS_DCLF_SET_PARITY).
		std::atomic<std::uint32_t> fadeLogFocus{ ~0u };
		std::uint32_t fadeLogTurn = 0;

		// CS_DCLF_SET_PARITY: one snapshot per frame in flight, read back a few frames later.
		struct SetParityFrame
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			std::uint32_t frame = 0;
			std::uint32_t framesLeft = 0;
			std::vector<std::uint8_t> depthState, colourState;  // MainPayload::objectState of the two builds
			// Per object: bit 0 in the frame's main set, bit 1 bound, bit 2 alpha tested, bit 3 kObjectMember disagrees with the set.
			std::vector<std::uint8_t> flags;
			std::vector<const RE::BSGeometry*> geometry;
			// Held, the snapshot's tables keep every named geometry alive (retirement chain) until the readback.
			std::shared_ptr<const SceneTables> tables;
			// Frustum stamps (fade test drops), and per member whether its live bound (kept) is inside the depth build's frustum:
			// culling must not reject one inside.
			winrt::com_ptr<ID3D11Buffer> frustumStaging;
			std::vector<std::uint8_t> inView;
			std::vector<std::array<float, 4>> bound;
			// Per object, its phases in the registration hooks' claims (PassCapture::CurrentSet): the main claim must equal the set,
			// or an object is drawn by nobody or twice.
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
			// Drawn by DCLF outside the set (a double draw), and records whose kObjectMember disagrees with the set.
			std::uint32_t outsideDrawn = 0, recordDisagrees = 0;
			std::uint32_t samples = 0;
			// Gaps: a resident object drawn, withheld and GPU-culled for 1-8 frames, then drawn again. Counted, not flagged: an
			// occlusion rejection is against this frame's depth, so it is hidden in the final image too.
			std::uint32_t gaps = 0, gapsRetest = 0, gapsRejected = 0, gapsTree = 0, gapSamples = 0;
			std::array<std::uint32_t, 3> gapsByLength{};
			std::array<std::uint32_t, 7> gapsByClass{};
			std::uint32_t gapsInView = 0, gapsNew = 0, gapsAfterNative = 0, gapsBeforeNative = 0;
			// Members rejected with their live bound in view: by the frustum (culling read a stale bound), by the fade test, and
			// how many were skinned.
			std::uint32_t rejectedInView = 0, fadeHiddenInView = 0, skinnedInView = 0, inViewSamples = 0, fadeSamples = 0;
			// Fade-rejected in view while the engine (still culling the root) has its node fully faded in.
			std::uint32_t fadeContradicted = 0, fadeContradictedLast = 0;
			std::string fadeContradictedFirst;
			// Main claims against the set: claimed outside it (drawn by nobody), members unclaimed (drawn by both).
			std::uint32_t claimedOutside = 0, unclaimedMembers = 0, claimSamples = 0;
		} setParity;
		// Gap detector, per geometry: last frame's state (0 not resident, 1 drawn, 2 withheld and GPU-culled) and first resident frame.
		struct GapHistory
		{
			std::uint32_t frame = 0, first = 0;
			std::uint8_t last = 0;
			// The current withheld run; runDrawnBy/drawnBy: 1 outside the set, 2 DCLF.
			std::uint8_t run = 0, runClass = 0, runVerdict = 0, runInView = 0, drawnBy = 0, runDrawnBy = 0;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, GapHistory> gapHistory;

		// CS_DCLF_PERSISTENT_PARITY: frame values' rows and palettes against the engine's (CheckFrameValues).
		struct FrameValuesParity
		{
			std::uint64_t checks = 0, differs = 0, fresh = 0, paletteChecks = 0, paletteDiffers = 0;
			std::string first, paletteFirst;
		} frameValues;

		// CS_DCLF_PERSISTENT_PARITY: the kept stores' checks (KeptChecks.cpp), each on its store's thread; taken by the snapshot
		// builder's StoresReport and ProducerReport.
		struct Kept
		{
			ParityCounter objects, geometry, drawBound, sunExclusion, parabolicExclusion, shadow;
			// Inputs the kept shadow build lacks, by reason, with the first of each.
			std::map<std::string, std::uint64_t> shadowMissingBy;
			std::map<std::string, std::string> shadowMissingFirst;
			// Extras rows as each buffer holds them (scene buffers, payload ring entries), and the scene buffers' check.
			std::vector<float> sceneExtras;
			std::array<std::vector<float>, DrawsView::Impl::kPayloadRing> ringExtras;
			ParityCounter extras;
			std::string extrasFirst;
			/**
			 * @brief Payload ring checks (uploader thread). Lock-free: atomic counts; the first difference's text is written by the
			 * producer that claims it (firstState 0 -> 1) and taken by the report once written (2 -> 0).
			 */
			struct Ring
			{
				std::atomic<std::uint64_t> checks{ 0 }, mismatches{ 0 };
				std::atomic<std::uint32_t> firstState{ 0 };  // 0 none, 1 being written, 2 written
				std::string first;
			} ring;
		} kept;

		// CS_DCLF_PERSISTENT_PARITY: view masks (CheckViewMasks, in the builds), taken by the render thread's report.
		struct ViewMasks
		{
			std::atomic<std::uint64_t> checks{ 0 }, inputs{ 0 }, differ{ 0 };
			// First difference: (list << 56) | (object << 24) | expected mask; and its mask.
			std::atomic<std::uint64_t> first{ ~0ull }, firstMask{ 0 };
		} viewMasks;


		// Shadow epoch checks (ShadowChecks.cpp): index pool updates (CS_DCLF_BUILD_PARITY), predicted targets, occlusion views.
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

		// Reflection face cameras (CheckReflectionFace): faces compared, differing (block, slice).
		struct FaceCameras
		{
			std::uint32_t faces = 0, blocks = 0, slices = 0;
			std::uint64_t registers = 0;  // differing registers (c0-c44), by bit
			float largest = 0.0f;
			std::string first;
		} faceCameras;

		// CS_DCLF_PERSISTENT_PARITY: the catalog's constant tables against Community Shaders' (CheckConstantTables).
		struct ConstantTables
		{
			std::uint32_t checks = 0;       // parity frames
			std::uint32_t entries = 0;      // resolved pipeline entries compared
			std::uint32_t uncached = 0;     // of them, not yet in ShaderCache (not compared)
			std::uint32_t variables = 0;    // variables in both tables, compared
			std::uint32_t differ = 0;       // of them, at another offset
			std::uint32_t catalogOnly = 0;  // in DCLF's table alone (the game's stage does not declare it)
			std::uint32_t cacheOnly = 0;    // in ShaderCache's alone (DCLF_BINDLESS's blocks, or never read)
			std::string first, catalogOnlyFirst;
		} constantTables;
		// CS_DCLF_SET_PARITY: the first-person skeleton's slots and claimed phases as last logged.
		std::string loggedSkeleton;

		// CS_DCLF_FOLIAGE_PARITY report (FoliageParityReport, ORG's host thread; the mutex orders it with the next frame's).
		struct Foliage
		{
			std::uint64_t frames = 0, flagged = 0, inFrameFlagged = 0, whiteFlagged = 0;
			double whiteAverage = -1.0;  // running average of near-white foliage pixels (kFoliageWhiteAlbedo + kFoliageWhiteDiffuse)
			std::uint32_t whiteLogged = 0, whiteMax = 0;
			double changeAverage = 0.0;  // running average of reprojected changes (recoloured, brightened, whitened)
			std::uint32_t changeMax = 0;
			std::array<std::uint64_t, kFoliageCounters> totals{};
			std::uint32_t logged = 0, inFrameLogged = 0;
			std::uint32_t landFrames = 0, landMax = 0, landLogged = 0;
			std::mutex mutex;
		} foliage;

		// CS_DCLF_CAPTURE_POINT_PARITY (CheckCapturePoint), per report window: differing frames and per-binding counts.
		struct CapturePoint
		{
			Reports::Window window;
			std::uint32_t frames = 0;
			ankerl::unordered_dense::map<std::string, std::uint32_t> differ;
		} capturePoint;
	};
}
#endif
