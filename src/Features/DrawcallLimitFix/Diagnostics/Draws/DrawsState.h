#pragma once

// DrawsState: the draw observers' state between hook points. Render thread unless noted.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "DrawsView.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief CS_DCLF_PERSISTENT_PARITY: one backing pool's lease-leak watch, a report window at a time. The leases out at once are
	 * the frames in flight's (each holds its snapshot, and so its payload, until its GPU work retires) and two more (the installed
	 * version and the one being built); a lease that never returns stays out for good, so the leased count stays past that. Flagged
	 * once it has for kWindows windows in a row. The live count is no measure: a pool keeps free backings warm
	 * (PooledBackings::PerFrameSpares) up to its high-water mark.
	 */
	struct PoolLeakWatch
	{
		static constexpr std::uint64_t kFramesInFlight = RenderGraphRuntime::kGameFramesInFlight;
		static constexpr std::uint64_t kSlack = 2;
		static constexpr std::uint32_t kWindows = 3;
		std::uint32_t over = 0;  // consecutive windows past the bound

		/** @brief This window's leased count: the verdict's flag (empty: within the bound, or not yet for long enough). */
		const char* Window(std::uint64_t a_leased)
		{
			over = a_leased > kFramesInFlight + kSlack ? over + 1 : 0;
			return over >= kWindows ? " <- POOL LEAK" : "";
		}
	};

	/**
	 * @brief A D3D11 view of a versioned buffer's current version, as 32-bit words over the whole version, for immediate-context
	 * readbacks; made again when the version is not the one it wraps. It holds the version it wraps, so the view never outlives
	 * its backing.
	 */
	struct WrappedVersion
	{
		std::shared_ptr<const org::BufferVersion> wrapped;
		winrt::com_ptr<ID3D11Buffer> view;

		ID3D11Buffer* Of(const Draws::Versioned& a_buffer) { return Of(a_buffer ? a_buffer->Current() : nullptr); }
		/** @brief A given version (one a revision names, which need not be the buffer's current one). */
		ID3D11Buffer* Of(std::shared_ptr<const org::BufferVersion> a_version)
		{
			auto current = std::move(a_version);
			if (!current || !current->buffer)
				return nullptr;
			if (!view || wrapped != current) {
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = static_cast<UINT>(current->buffer->GetSize());
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				view = RenderGraphRuntime::Get().WrapBuffer(*current->buffer, desc);
				wrapped = std::move(current);
			}
			return view.get();
		}
	};

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
			winrt::com_ptr<ID3D11Buffer> sequences, count, classCounts;
			// The templates, their pipeline index their set index's index in its state class (as BuildDrawsCS stores it: classOf).
			std::vector<DrawSequence> expected;
			// Per decal group, by slot (a culled slot reads back with index count zero, otherwise identical).
			std::array<std::vector<DrawSequence>, kDecalGroups> expectedDecals;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // the buffer's ranges when copied
			// The shape's state class sub-ranges (PassFrame::classes) whose sequences are compared, by class.
			std::vector<SetClassRanges> classes;
			std::uint32_t framesLeft = 0;
			// The row tables of the payload the epoch committed with (PayloadVersions' PipelineRows and MaterialRows): the bases the
			// CPU's table-relative row offsets were placed at.
			std::uint64_t pipelineRowsBase = 0, materialRowsBase = 0;
			// By object: the CPU's local shadow light selection (kObjectLocalShadowMask); none on the Z-prepass.
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> expectedLocalShadows;
			// The mask bits of portal-strict lights' volumes (GpuShadowVolume::portalContext): BuildDraws gates them by the receiver's
			// root (StrictGateRejects), which this reference does not model; the strict light parity compares those receivers.
			std::uint32_t gatedLocalShadows = 0;
			// The sequences' version the adopted set names (what the epoch's recording wrote) and the buffer's current one, when copied.
			std::uint64_t sequencesNamed = 0, sequencesCurrent = 0;
			std::uint32_t armedEpoch = 0;  // parityEpochs when copied
		};
		std::optional<ParityReadback> parity;
		std::uint32_t parityEpochs = 0;
		WrappedVersion paritySequences, parityClassCounts;
		// The main epochs' payloads, newest last (every main epoch while the build parity is on; epoch: the colour epochs counted
		// then): where a sequence's row address outside its epoch's tables came from.
		struct ParityPayloadSeen
		{
			std::uint64_t serial = 0;
			std::uint32_t epoch = 0;  // parityEpochs when first seen
			std::uint64_t materialBase = 0, materialBytes = 0, pipelineBase = 0, pipelineBytes = 0;
		};
		std::array<ParityPayloadSeen, 32> parityPayloads{};
		std::uint32_t parityPayloadsSeen = 0;

		// CS_DCLF_SET_PARITY (SetClassParity.cpp): a colour epoch's state class sub-ranges, copied to staging after it and read a few colour
		// epochs later (never waiting: a copy still in flight is tried again the next epoch), against the shape it recorded with.
		struct SetClassReadback
		{
			winrt::com_ptr<ID3D11Buffer> sequences, count, classCounts;
			// The shape's sub-ranges (PassFrame::classes) and class plan (the latch's table and map: SetClassPlan), and its indirect state's
			// pipelines per class (each class's execution sets hold [0, pipelines)) and class words (IndirectState::classOf).
			std::vector<SetClassRanges> classes;
			std::shared_ptr<const SetClassPlan> plan;
			std::vector<std::uint32_t> pipelines, classOf;
			// By object: the set indices of the epoch's payload's inputs naming it (its resident region's and its list's; not refraction's,
			// a plain draw): what each of its draws' class and index must come from.
			ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> setsOf;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
			bool sorted = false;  // the colour draws went through DrawSort: each class's main sub-range is in pipeline order
			std::uint32_t framesLeft = 0, tries = 0;
			std::uint32_t armedEpoch = 0;  // setClassEpochs when copied
		};
		std::optional<SetClassReadback> setClass;
		std::uint32_t setClassEpochs = 0;
		WrappedVersion setClassSequences, setClassCounts;

		// The colour commit's last culling latch (sun test), and the depth commit's last terrain near test.
		BuildDrawsLatch sunUpload{};
		std::array<float, 4> terrainNearUpload{};
		// CS_DCLF_SET_PARITY: a root the set parity saw fade-rejected with its live bound in view, which every other window of the fade
		// log (Features/Fades/Diagnostics/FadeLog.cpp) focuses on.
		std::atomic<std::uint32_t> fadeLogFocus{ ~0u };

		// CS_DCLF_SET_PARITY: one snapshot per frame in flight, read back a few frames later.
		struct SetParityFrame
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			std::uint32_t frame = 0;
			std::uint32_t framesLeft = 0;
			std::vector<std::uint8_t> depthState, colourState;  // MainBody::objectState of the two builds
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
		WrappedVersion setParityVisibility, setParityFrustum;
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
			// Resident slots whose shading row no producer sampled (written 0) at two checks running.
			std::uint64_t shadingChecks = 0, shadingUnsampled = 0;
			std::string first, paletteFirst, shadingFirst;
		} frameValues;
		// The resident slots unsampled at the last check (kept across the report's reset of frameValues).
		std::vector<std::uint8_t> shadingUnsampledBefore;

		// CS_DCLF_PERSISTENT_PARITY: the kept stores' checks (KeptChecks.cpp), each on its store's thread; taken by the snapshot
		// builder's StoresReport and ProducerReport.
		struct Kept
		{
			ParityCounter objects, geometry, drawBound, sunExclusion, parabolicExclusion, shadow;
			// Inputs the kept shadow build lacks, by reason, with the first of each.
			std::map<std::string, std::uint64_t> shadowMissingBy;
			std::map<std::string, std::string> shadowMissingFirst;
			// The payload's extras backings' rows as caught up (MirrorExtras, CheckExtras: the snapshot builder's thread, as the report),
			// by Backing::generation; a released backing's mirror is dropped at the next catch-up.
			struct ExtrasMirror
			{
				std::weak_ptr<org::services::Backing> backing;
				std::vector<float> rows;
			};
			ankerl::unordered_dense::map<std::uint64_t, ExtrasMirror> extrasMirrors;
			ParityCounter extras;
		} kept;
		// The main commits' log (CommitLog.cpp): unfilled frame buffers by register, fade inputs varied, the culling setup logged, the epoch
		// lines' periods.
		std::array<std::uint32_t, kTextureRegisters> unfilledFrameBuffers{};
		std::uint32_t fadeInputsVaried = 0;
		bool cullingSetupLogged = false;
		std::array<std::uint32_t, 2> epochLines{};  // colour, Z-prepass

		// CS_DCLF_PERSISTENT_PARITY: the payload's pools' leak watch, by part (the snapshot builder's ProducerReport).
		std::array<PoolLeakWatch, Draws::kPayloadParts> payloadPools;
		// ... and FrameValues' pools' (the render thread's FrameValuesLine, which sizes it: the registered pools included).
		std::vector<PoolLeakWatch> frameValuesPools;

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
