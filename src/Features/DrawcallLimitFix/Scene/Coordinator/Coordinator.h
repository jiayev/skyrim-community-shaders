#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Engine/Access/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/MaterialBindings.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/SharedBindings.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPort.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialSources.h"
#include "Features/DrawcallLimitFix/Scene/Ports/GeometryPort.h"
#include "Features/DrawcallLimitFix/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Scene/Model/LodSegments.h"
#include "Features/DrawcallLimitFix/Scene/Ports/TreeLod.h"
#include "Features/DrawcallLimitFix/Scene/Model/ActorValueIndex.h"
#include "Features/DrawcallLimitFix/Engine/Capture/FaceSnapshots.h"
#include "Features/DrawcallLimitFix/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Scene/Model/SceneIdentity.h"
#include "Features/DrawcallLimitFix/Runtime/SlotTable.h"
#include "Features/DrawcallLimitFix/Runtime/Retirement.h"
#include "Features/DrawcallLimitFix/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Runtime/LatestSlot.h"
#include "Features/DrawcallLimitFix/Runtime/SceneWake.h"
#include "Features/DrawcallLimitFix/Runtime/PublicationLog.h"
#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LocalShadows.h"
#include "Features/DrawcallLimitFix/Scene/Ports/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/Model/FrameTables.h"
#include "Features/DrawcallLimitFix/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Scene/Model/Eligibility.h"
#include "Features/DrawcallLimitFix/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Scene/Channels.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/CandidateTable.h"

namespace DCLF
{
	namespace Scene
	{
		struct SwitchEvent;
	}
	namespace Diagnostics
	{
		struct SceneView;
		struct TrackedNotes;
		/** @brief Frees a tracked entry's notes (Diagnostics/Scene/State.cpp, where the type is complete). */
		struct TrackedNotesDelete
		{
			void operator()(TrackedNotes* a_notes) const;
		};
	}
	/** @brief Attributes the time since the last call to one BuildPart (Coordinator/Internal.h; CS_DCLF_PROFILE). */
	struct FrameGlobals;
	struct PartTimer;
	struct SunCandidates;
	struct PipelineCatalog;

	/**
	 * @brief The parts of Coordinator::BuildFrame, each measuring one thing.
	 *
	 * Timed only under CS_DCLF_PROFILE. One part per unrelated piece of work, so each can be optimised on its own.
	 */
	enum class BuildPart : std::uint32_t
	{
		Walk,            // the pass map, built before the loop
		PassLookup,      // FindAccumulatedPass per object
		ClassifyStatic,  // type, skin, property, material alpha, DeriveLightingDescriptors
		ClassifyFrame,   // the parent chain (hidden, actor) and the fade
		Diagnostics,     // the derivation counters, which only feed a log line
		Resolve,         // GpuResources::Resolve for the vertex and index buffers
		Dedup,           // the geometry, pipeline and material map probes
		PipelineEval,    // the new-pipeline body: its technique row and constants
		MaterialEval,    // the new-material body: the material port
		Record,          // scene writes and accumulator patch bookkeeping
		CapturePatch,    // accumulator's remaining engine-affine shading/light/tree sampling
		ApplyPatch,      // accumulator's value-only table propagation
		DedupHit,   // the three map probes on the HIT path (Dedup measures the misses)
		LoopTail,   // per TRACKED object: the continue path of a rejected one, and the iteration itself
		Skinning,   // skinned objects: the engine's palette update and the row copy
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(BuildPart::Count)> kBuildPartNames{
		"walk", "pass-lookup", "classify-static", "classify-frame", "diagnostics",
		"resolve", "dedup", "pipeline-eval", "material-eval", "record", "capture-patch", "apply-patch",
		"dedup-hit", "loop-tail", "skinning"
	};

	/**
	 * @brief The sub-zones of the scene tables (DrawcallLimitFix::BeginSceneFrame): the events applied (ApplyEvents), the scene phase
	 * block by block, and (T6b3c, one pass) the joins, the commit and the publication. Each is a Tracy zone and a sum in
	 * Stats::scenePartMs, reported under CS_DCLF_STATS. The first evaluation round's split by entry (EvaluateKind) is measured only
	 * under CS_DCLF_PROFILE.
	 */
	enum class ScenePart : std::uint32_t
	{
		CategoryNodes,     // the attach queue's drain and RefreshCategoryNodes
		AttachDetach,      // AddSubtree per attach, EraseTracked per detach
		Validate,          // ValidateSlice
		StructuralEvents,  // the fade, property, node and switch queues' drains
		Prologue,          // the used sets, the change log trim and check, LOD fade settings, GpuResources::BeginFrame
		SweepSlots,        // UpdateSlotReferences and the slot expiry
		CullHiddenBits,    // CaptureCullHiddenBits
		Schedule,          // BeginWalk and the order: the per-frame set and the events' dependents
		SwitchEvents,      // ApplySwitchEvents
		Evaluate,          // the first round: per-frame entries and those the events scheduled
		GeometryScan,      // FinishDeltaWalk's geometry slot touches and the stale-slot object scan
		LaterRounds,       // the rounds for stale and refreshed geometry slots
		ShadowSets,        // RefreshShadowSets and its parity
		ObjectSweep,       // SweepObjectSlots (a full walk only)
		SunCandidates,     // UpdateSunCandidates
		FaceWalk,          // EndFaceWalk
		WalkReference,     // the walk's observers' dense reference (DenseReference)
		Shading,           // NameShadingEvents: the shading events' slots named for FrameValues
		Placements,        // PublishPlacementPlan: the roots taken, and the walk's plan for FrameValues
		Joins,             // T6b3c, the pass's joins: BuildAccumulatePhase, the new pipelines' blocks, the bindings, the lookups resolved
		Commit,            // T6b3c: CommitSet, after the joins
		Publish,           // T6b3c: PublishScene (the set applied, the claims revoked, the tables and lookups published)
		// T6b3e, inside the parts above (ScenePartNested): the evaluation rounds' and the joins' two halves, every round's.
		EvaluateFanout,    // EvaluateEntry over the round (on the preparation pool from 128 entries, CS_DCLF_FANOUT=2)
		EvaluateMerge,     // MergeEntry over the round, in order, on the scene lane
		JoinsFanout,       // the joins' evaluations (EvaluateJoinPre over the bind queue, EvaluateJoin over the joins)
		JoinsMerge,        // the joins' merges, in order
		SubtreesFanout,    // T6b3e e5: the subtrees' prepares (PrepareSubtrees: on the preparation pool from 16 roots, CS_DCLF_FANOUT=2)
		SubtreesMerge,     // T6b3e e5: their merges (MergeSubtree), in event order
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(ScenePart::Count)> kScenePartNames{
		"category nodes", "attach/detach", "validate", "structural events", "prologue", "sweep slots", "cull hidden bits",
		"schedule", "switch events", "evaluate", "geometry scan", "later rounds", "shadow sets",
		"object sweep", "sun candidates", "face walk", "walk parity", "shading", "placement plan", "joins", "commit", "publish",
		"evaluate (fan-out)", "evaluate merge", "joins (fan-out)", "joins merge", "subtrees (fan-out)", "subtrees merge"
	};

	/** @brief T6b3e: a part measured inside another (the fan-outs' halves), left out of the parts' sum. */
	inline constexpr bool ScenePartNested(std::size_t a_part)
	{
		return a_part >= static_cast<std::size_t>(ScenePart::EvaluateFanout);
	}

	/** @brief What the first evaluation round did with an entry, for its per-entry timing (CS_DCLF_PROFILE). */
	enum class EvaluateKind : std::uint32_t
	{
		Light,        // the light path kept or moved it
		LightMissed,  // the light path tried and fell through to the full write
		Actor,        // written in full: an actor's (kTraitActor) whose verdict lets it have a record
		ActorNoRecord,  // written in full: an actor's whose verdict gives it none (hidden, fading, ...)
		Face,         // written in full: a face shape
		PerFrame,     // written in full: any other per-frame entry
		Event,        // written in full: scheduled by an event, a pending evaluation or a full walk
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(EvaluateKind::Count)> kEvaluateKindNames{
		"light path", "light path missed", "actor", "actor without a record", "face", "other per-frame", "event"
	};

	/**
	 * @brief The render thread's view of the static scene content Drawcall Limit Fix can draw.
	 *
	 * T6b3d: the coordinator's pump (the scene lane) drains the engine's events and applies them (CollectEvents, ApplyEvents:
	 * SceneTracker's events and the loaded-cell changes) in each scene pass, woken by its producers; the pass's walk refreshes the
	 * tracked geometries' records and its joins patch the half of each record the main camera's accumulator decides, then it commits
	 * the set and publishes. The render thread adopts the newest snapshot at the frame's start and serves the pass's requests.
	 *
	 * Tracked content is everything under the Static, Dynamic, MultiBound and Actor category nodes of attached cells
	 * (engine notes: cell 3D category nodes; an interior moves its actors into its rooms). Terrain, markers and water
	 * live under the other category nodes and are never tracked.
	 */
	namespace Scene
	{
		class Coordinator;
	}

	class Scene::Coordinator
	{
		// The scene area's observers and reports (Diagnostics/Scene, Diagnostics/Reports/Scene*.cpp) read the private state through it.
		friend struct Diagnostics::SceneView;

	public:
		using Tables = SceneTables;

		struct Stats
		{
			std::uint32_t tracked = 0;
			std::uint32_t objects = 0;
			std::uint32_t geometries = 0;
			std::uint32_t pipelines = 0;
			std::uint32_t materials = 0;
			std::uint32_t shadowMaskPipelines = 0;  // pipelines whose technique binds the shadow mask (not derived yet)
			std::uint32_t categoryNodes = 0;
			std::uint64_t attachedEvents = 0;
			std::uint64_t detachedEvents = 0;
			std::uint64_t detachMoves = 0;  // detached geometries attached again in the same batch (kept, not erased)
			std::uint64_t validationDrops = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			// Decal candidates this frame, by group (Records.h ObjectDecalGroup - 1).
			std::array<std::uint32_t, 3> decals{};
			// CS_DCLF_FADING: objects given bindings with the screen-door fade (AdditionalAlphaMask), summed over
			// frames until the report takes them (TakeFadingDrawn), since fades are brief.
			std::uint32_t fadingDrawn = 0;
			std::uint32_t fadingFrames = 0;  // frames in that sum with at least one
			std::uint32_t skinned = 0;  // skinned candidates this frame, and their palette rows
			std::uint32_t boneRows = 0;
			std::uint32_t projectedUV = 0;  // candidates with the ProjectedUV bit, and terrain ones
			std::uint32_t landBlend = 0;
			// CS_DCLF_DERIVED_CACHE: accumulated objects served from their cached derivation, and under
			// `probe` how many were recomputed and how many disagreed (the gate: 0).
			std::uint32_t derivedHits = 0;
			std::uint32_t derivedChecked = 0;
			std::uint32_t derivedDiffers = 0;
			std::uint32_t slotsSwept = 0;
			std::uint32_t geometriesRefreshed = 0;
			std::uint32_t slotViolations = 0;  // objects whose slots failed CheckObjectSlots (the gate: 0)
			std::uint32_t shadowCasters = 0;  // records the engine would draw into a shadow map
			std::array<std::uint32_t, 16> shadowRejects{};  // by ShadowReject, over the frame's records
			// Objects the engine accumulated that the scene phase had left out of the tables, so the frame
			// cannot draw them. One frame of staleness at most (the verdict is cleared for them); the gate
			// is 0 in steady state.
			std::uint32_t geometriesAlive = 0, pipelinesAlive = 0, materialsAlive = 0;
			// CS_DCLF_CLASSIFY_CACHE: objects served from the cached verdict, and - under `probe` - how
			// many were recomputed and how many disagreed. Zero disagreements is the gate.
			// Objects left native by Ineligible::Technique, by technique id (index 63 = refraction). This
			// is what says which technique to bring into coverage next, instead of guessing.
			std::array<std::uint32_t, 64> techniqueRejects{};
			// CS_DCLF_COVERAGE_PROBE=1: objects left native by NotLightingShader, by property type. The
			// class is the largest one outside coverage and "not a lighting property" says nothing about
			// which shader would have to be brought in.
			std::map<const RE::NiRTTI*, std::uint32_t> propertyRejects;
			// Of those, how many are alpha blended. This is the scoping question for a second shader:
			// blended geometry is not drawn in the pass DCLF owns at all, so covering it would mean a new
			// epoch after the deferred composite rather than another shader inside the existing one.
			std::uint32_t rejectedBlended = 0;
			std::uint32_t rejectedOpaque = 0;
			std::uint32_t rejectedOpaqueAlphaTest = 0;
			std::uint32_t classifyHits = 0;
			std::uint32_t classifyChecked = 0;
			std::uint32_t classifyDiffers = 0;
			// BuildFrame time by part (BuildPart), summed since the last ResetTimes (ms). Only filled under
			// CS_DCLF_PROFILE; with it off the loop makes no clock calls at all.
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};
			// The accumulator subset of partMs. It is measured at EarlyPrepass on
			// the render thread, distinct from the preceding scene walk's parts.
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> accumulatePartMs{};
			// The scene tables by sub-zone (ScenePart), summed since the last ResetTimes (ms).
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMs{};
			// The current frame's time by part, and the largest frame's since the last ResetTimes (EndSceneFrame).
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartFrameMs{};
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMaxMs{};
			// The light path since the last ResetTimes: entries it kept, by trait (an entry counts under each of its
			// lightTraits bits, Coordinator::kTrait*); the movers it listed for FrameValues (PlacementPlan::movers); the kept skins.
			std::array<std::uint64_t, 8> lightByTrait{};
			std::uint64_t lightPlaced = 0, lightSkins = 0;
			// The move events drained.
			std::uint64_t moveEvents = 0;
			// Walks whose schedule had to look the per-frame set up again (an entry was added or erased).
			std::uint64_t perFrameRelookups = 0;
			// Hidden events drained; actor frame verdicts taken again and left alone; on parity frames, verdicts that
			// changed with no event (the first named).
			std::uint64_t hiddenEvents = 0, verdictsChecked = 0, verdictsSkipped = 0, verdictsMissed = 0;
			std::uint64_t hiddenRetaken = 0;  // statics classified again for a hidden event on their chain (T6b4)
			std::uint64_t hiddenAheadAnnounced = 0;  // hidden events announced again at a capture newer than them (hiddenAhead)
			std::string firstVerdictMissed;
			// CS_DCLF_INPUT_WATCH: per input component (kInputComponentNames), the re-reads that found it changed; per kind
			// of re-read (classify, shading), the re-reads that found any change; the first such change named.
			std::array<std::uint64_t, 12> inputChanged{};
			std::array<std::uint64_t, 2> inputRereads{}, inputRereadsChanged{};
			std::string firstInputChange;
			// The first evaluation round by EvaluateKind: time (ms) and entries, under CS_DCLF_PROFILE.
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			std::array<std::uint64_t, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindCount{};
			// The property-derived descriptor against the accumulated one (Phase 5 readiness): objects
			// compared, and objects the derivation would have left native.
			//
			// The comparison is reported in two halves, because they mean different things. Outside
			// kRuntimePassBits the derivation is meant to be exact, and a difference is a defect. Inside
			// kRuntimePassBits it is guessing at values GetRenderPasses computes from per-frame light and
			// shadow assignment, so a difference there is expected until those bits are derived properly —
			// and how large it is decides whether GetRenderPasses can ever be skipped outright. The earlier
			// counter masked the runtime half out entirely, which made "0 differ" read as a much stronger
			// result than it was.
			std::uint32_t derivationChecked = 0;
			std::uint32_t derivationFadeBits = 0;  // differ only where the engine's LOD fades ran out (the draw fades them)
			// The draw's LOD fades against the engine's, on the registered objects with a fade node (CS_DCLF_DERIVE_PROBE).
			std::uint32_t lodFadeChecked = 0, lodMetricDiffers = 0, lodFadeDiffers = 0;
			std::string lodFadeFirst;
			std::uint32_t derivationDiffers = 0;        // differ outside kRuntimePassBits
			std::uint32_t derivationBits = 0;           // OR of those differing bits
			std::uint32_t derivationRuntimeDiffers = 0;  // differ inside kRuntimePassBits
			std::uint32_t derivationRuntimeBits = 0;     // OR of those differing bits
			// Objects differing per bit position, over the whole descriptor, so a single dominant bit can
			// be told apart from a smear across several.
			std::array<std::uint32_t, 32> derivationBitCounts{};
			std::uint32_t derivationNative = 0;
			// Material evaluations this frame.
			std::uint32_t materialsEvaluated = 0;
			std::uint32_t materialCacheEntries = 0;
			std::uint32_t materialCacheEvicted = 0;
			std::uint32_t materialEvictedMember = 0;  // of them, slots a membership resident had been bound to (materialMember)
			// T6b2c step 7, the scene work's last pass (RefreshMaterialRecords): the materials captured since the one before, the slots'
			// records they rewrote, the materials a rewrite could not evaluate yet (asked again), the signature samples.
			std::uint32_t materialWrites = 0;
			std::uint32_t materialsRewritten = 0;
			std::uint32_t materialsHeld = 0;
			std::uint32_t frameMaterialSamples = 0;
		};

		void ResetTimes()
		{
			stats.partMs = stats.accumulatePartMs = {};
			stats.scenePartMs = {};
			stats.scenePartMaxMs = {};
			stats.lightByTrait = {};
			stats.lightPlaced = stats.lightSkins = 0;
			stats.moveEvents = stats.perFrameRelookups = 0;
			stats.hiddenEvents = stats.verdictsChecked = stats.verdictsSkipped = stats.verdictsMissed = 0;
			stats.hiddenRetaken = stats.hiddenAheadAnnounced = 0;
			stats.firstVerdictMissed.clear();
			stats.inputChanged = {};
			stats.inputRereads = stats.inputRereadsChanged = {};
			stats.firstInputChange.clear();
			stats.evaluateKindMs = {};
			stats.evaluateKindCount = {};
		}

		static constexpr std::array<const char*, 12> kInputComponentNames{ "renderer data", "skin", "skin partition", "small bound", "shader property",
			"property flags", "material", "fade state", "material alpha", "alpha property", "alpha flags", "diffuse view" };
		/** @brief The end of the scene tables zone: folds this frame's part times into the maximums. */
		void EndSceneFrame()
		{
			for (std::size_t i = 0; i < stats.scenePartFrameMs.size(); ++i)
				stats.scenePartMaxMs[i] = std::max(stats.scenePartMaxMs[i], stats.scenePartFrameMs[i]);
			stats.scenePartFrameMs = {};
		}


		static Coordinator& Get();

		/**
		 * @brief T6b3d, render thread (PostPostLoad, once DCLF is installed): the coordinator's pump configured and its wakes allowed
		 * (WakeScenePass, by source). From here the scene passes (RunSceneWork) run on the scene lane whenever a producer wakes the pump: an
		 * engine event pushed, an answer, a catalog, a retirement returned, a frame input posted; never kicked or joined by the frame.
		 */
		void StartScenePump();
		/**
		 * @brief T6b3d, render thread (every frame's start, and the menu's toggle): a_full - DCLF runs (loaded and switched on): the passes
		 * do their whole work; else they apply the events alone, so the tracked set stays current (what Present's apply did). a_inline -
		 * the parities that observe the store from the render thread (CS_DCLF_PERSISTENT_PARITY, CS_DCLF_WALK_PARITY, the capture
		 * parity): the producers' wakes are dropped and the frame's start runs the pass itself (RunScenePassInline).
		 */
		void SetScenePassMode(bool a_full, bool a_inline);
		/**
		 * @brief T6b3d, render thread, the frame's start under the parities (SetScenePassMode's a_inline): one scene pass run here
		 * (SerializedTaskPump::TryRunInline), or, while a pass the lane started still runs (a wake from before the switch), after it ends
		 * (a wait, the parities' alone), so the render thread's observers (the mirror's leases, the validation slice, the extras and LOD
		 * parities) keep their placement. Then the notes it held for PrimaryCull, delivered.
		 */
		void RunScenePassInline();
		/**
		 * @brief T6b3d, render thread at Present under the parities, when no frame's start ran since the last Present (menus, load
		 * screens): one inline pass applying the events alone (Present's apply before T6b3d), so the queues do not wait for a frame.
		 */
		void ApplyEventsInline();
		/** @brief Whether a scene pass runs now (the pump not idle): a frame-side fallback that reads what the pass writes refuses then. */
		bool SceneTaskInFlight() const;

		// Scene members' records (drawcall-limit-fix.md, "Scene membership"): the accumulated half of a member's record is
		// patched once, from its membership pass (AccumulatedPass::resident, BindByMembership), and kept across frames: it is
		// not restored at the next walk, and only its slots are kept alive each frame (KeepResidentsAlive). A rewrite keeps it
		// while its binding stands; a release or a failed patch ends it.
		/** @brief Render thread: ends every membership now (the frame globals a membership pass reads changed). */
		void EndAllResidency();
		struct ResidentStats
		{
			std::uint64_t joined = 0, failed = 0, rewritten = 0, released = 0, frames = 0, resident = 0;
			std::uint64_t membershipQueued = 0;  // records BindByMembership handed a pass
			std::uint64_t materialWaits = 0, materialsServed = 0;  // joins that waited for a material capture, captures the render thread served
			std::uint64_t materialsStale = 0;  // T6b1b: requests whose property had another material at the serve (the swap's event not applied yet)
			std::uint64_t membershipKept = 0;    // members written again whose binding stands
			std::uint64_t layerUnpaired = 0;     // a base or a layer that joined without the other, and left again
			std::array<std::uint64_t, 4> failedBy{};  // (unused), no record, a frame verdict, material or extras
		};
		ResidentStats TakeResidentStats() { return std::exchange(residentStats, {}); }
		/**
		 * @brief The joins' frame inputs (T6b2c step 8), since the last call: LightLimitFix's room map and the pipeline frame
		 * the render thread posted (latest wins) and the scene lane took, and the PerGeometry blocks the coordinator made from the
		 * pipeline frame (MakeNewPipelineConstants) or could not make yet (no frame taken: the slot waited).
		 */
		struct AccumulateInputStats
		{
			std::uint64_t roomMapsPosted = 0, roomMapsTaken = 0, pipelineFramesPosted = 0, pipelineFramesTaken = 0;
			std::uint64_t pipelineBlocksMade = 0, pipelineBlocksWaited = 0;
		};
		AccumulateInputStats TakeAccumulateInputStats();


		/**
		 * @brief Which half of the frame's tables to build.
		 *
		 * The shadow views are drawn before the main camera's passes exist (engine notes: shadow maps),
		 * so the tables are built in two halves. Scene runs before the shadow maps and holds everything
		 * that does not depend on the accumulator - the object records a shadow epoch reads. Accumulate
		 * (the joins) patches those records with what the main pass draws them with: from the mirror and the
		 * membership passes, not the registrations, so since T6b3c it follows Scene in the same scene pass.
		 * Object indices are fixed from Scene onwards.
		 */
		enum class Phase : std::uint32_t
		{
			Scene,
			Accumulate
		};

		/** @brief Rebuilds one half of the CPU tables from the tracked set. */
		void BuildFrame(Phase a_phase);







		using RevisionRequest = DCLF::RevisionRequest;
		using ScenePublication = DCLF::ScenePublication;
		/** @brief The pipeline blocks the frame posted that the passes applied or found stale, and the technique rows' work, since the last call. */
		struct ConstantsPostStats
		{
			std::uint64_t pipelinesApplied = 0, stale = 0;
			// The coordinator's technique rows (T6b2c): evaluations (new rows, the frame inputs moved), the rows written, the inputs' moves.
			std::uint64_t techniquesEvaluated = 0, techniquesWritten = 0, techniqueInputsMoved = 0;
		};
		ConstantsPostStats TakeConstantsPostStats() { return std::exchange(constantsPostStats, ConstantsPostStats{}); }
		struct TablesPublication
		{
			std::uint64_t published = 0, reused = 0, made = 0;
			double ms = 0.0, maxMs = 0.0;
			// The replay: the material records written again (their version moved) against the slots, and the time the rest took
			// (copied whole still).
			std::uint64_t materialsReplayed = 0, materialSlots = 0;
			double restMs = 0.0;
			// The object slots the change log named (copied by slot) against the slots; publications copied whole (a snapshot just
			// made, or one the log no longer reaches).
			std::uint64_t objectsReplayed = 0, objectSlots = 0, wholeCopies = 0;
			std::uint64_t geometriesReplayed = 0, geometrySlots = 0, geometryWholeCopies = 0;
			std::uint64_t treesKept = 0, fadeRootsKept = 0;  // publications that kept the family (its stamp stood)
			std::size_t pool = 0;
			// T6b2c step 5, the lookups each publication carries (PublishLookups): copied (a new Snapshot), or the last one shared again
			// (unchanged since); the chunks written again since the last copy (SharedChunks, each a chunk's copy), and the copies' time.
			std::uint64_t lookupsCopied = 0, lookupsShared = 0, lookupsChunks = 0;
			double lookupsMs = 0.0, lookupsMaxMs = 0.0;
		};
		/** @brief The coordinator (T6b3d: its report): the retirement chain's counts since the last call, and what waits in it now. */
		std::string TakeRetirementReport()
		{
			const auto s = std::exchange(retirementStats, RetirementStats{});
			return fmt::format("{} retired, {} recycled in {} batches, {} dropped by a clear; {} object slots retiring", s.retired, s.recycled, s.batches, s.dropped,
				tables.objectsRetiring);
		}
		TablesPublication TakeTablesPublication() { return std::exchange(tablesPublication, TablesPublication{ .pool = tablesPool.size() }); }
		const Stats& GetStats() const { return stats; }
		/** @brief The screen-door fading objects given bindings since the last call, and in how many frames. */
		std::pair<std::uint32_t, std::uint32_t> TakeFadingDrawn()
		{
			const std::pair result{ stats.fadingDrawn, stats.fadingFrames };
			stats.fadingDrawn = stats.fadingFrames = 0;
			return result;
		}
		/**
		 * @brief The frame number: the render thread's (BeginFrame), or on a scene work thread the coordinator's (T6b3a: sceneFrame, the
		 * frame inputs' it last took; T6b3d: behind the render thread's by the pump's lag, and the same for every pass of a burst). Inline
		 * scene work runs with both equal.
		 */
		std::uint32_t GetFrame() const;
		/**
		 * @brief T6b3d: the pass as a 32-bit stamp for the per-entry "this pass" marks (Tracked::candidateFrame,
		 * Tables::geometryLastUsed): never 0 ("never") nor Tables::kSlotFree, unique over 2^32 - 2 passes.
		 */
		std::uint32_t PassStamp() const { return static_cast<std::uint32_t>(passSerial % 0xFFFFFFFEull) + 1u; }
		/** @brief The coordinator: the candidates as its work last made them (the builds ahead take them with the publication). */
		std::shared_ptr<const SunCandidates> CoordinatorSunCandidates() const { return sunCandidates; }
		std::shared_ptr<const SunCandidates> CoordinatorLightCandidates() const { return lightCandidates; }


		/**
		 * @brief Which partitions of the skin the engine draws with this LOD row (bit i = partition i):
		 * BSDismemberSkinInstance::Unk_25 (AE 140d31f40) first skips a partition whose flag (Data byte 0) is
		 * clear, then NiSkinPartition::Unk_25 applies the table. 0 when it draws none.
		 */
		static std::uint32_t SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow);
		/** @brief SkinPartitionMask from the geometry's record (T6b1b): its partitions' LOD bytes and a dismember skin's shown flags. */
		static std::uint32_t SkinPartitionMask(const SceneCapture::GeometryRecord& a_geometry, std::uint32_t a_lodRow);
		/**
		 * @brief Tables::skinPartitions for a geometry: 0 without a skin partition, kNoPartitions where its LOD row draws
		 * none, the mask for a skin of several partitions, 0 for one of a single drawn partition. The row is 3 (every level's
		 * partitions) whatever the node's level: a LOD skin's level is the draw's (SkinLodPartitionsOf).
		 */
		static std::uint16_t SkinPartitionsOf(const SceneCapture::GeometryRecord& a_geometry);
		/** @brief Tables::skinLodPartitions for a geometry: a kMeshLOD skin's masks per level (byte L: row L), 0 for any other. */
		static std::uint32_t SkinLodPartitionsOf(const SceneCapture::LeafView& a_leaf);

		/** @brief Index into GetTables().objects for this frame, or -1 when the geometry is not drawn by DCLF. */
		std::int32_t FindObject(const RE::BSGeometry* a_geometry) const;
		/** @brief The geometry's layer object (Tracked::layerSlot) in this walk's tables, or -1. */
		std::int32_t FindLayerObject(const RE::BSGeometry* a_geometry) const;
		/** @brief The layer's membership pass this frame (BindByMembership), or null. */
		const AccumulatedPass* FindAccumulatedLayerPass(const RE::BSGeometry* a_geometry) const;
		/** @brief Render thread (diagnostics): the shader property a slot draws with (its layer property for a layer's). */
		RE::BSShaderProperty* SlotPropertyOf(std::uint32_t a_slot) const { return SlotProperty(a_slot); }
		/**
		 * @brief The DCLF set (SceneSet.h): decided once a frame by CommitSet, at the scene phase, after the walk and before the
		 * main camera's cull. Every phase of the frame reads the same decision: the record's kObjectMember, which the builds select
		 * by, and the snapshot the engine's registration hooks withhold by. Nothing between two commits changes it.
		 *
		 * Readiness is part of it: an object joins only once everything its phases draw with is ready (its pipeline compiled, its
		 * material's textures imported), and readiness is taken by events (a lookups generation, the tables' change log), never by
		 * a scan of the scene. A member whose binding is taken again this frame (its record's inputs changed) leaves before the
		 * accumulate phase rebinds it, so no frame's builds draw a member from a binding that is not ready.
		 *
		 * The commit decides the next frame's set; ApplySet makes it the frame's (dclf-async-publication.md, "What each
		 * carries"): the engine's claims are installed at Main::Draw, before the frame's scene work can have run, so the frame
		 * draws exactly the set its claims are - the last commit's. The commit writes setPhasesNext and the snapshot; the
		 * records, Tables::setPhases and the lacking counts change only at ApplySet.
		 */
		void CommitSet();
		/**
		 * @brief The last commit's set made the frame's: its phases into the records (kObjectMember, Tables::setPhases) and the
		 * lacking counts, and its snapshot published as the engine's claims (PassCapture). Main::Draw (BeginSceneFrame), before the
		 * frame's events. A slot freed or given to another geometry since the commit (an event at Present) takes no phase.
		 *
		 * With scene snapshots (T6b3b) the claims applied are the adopted snapshot's publication's, whose revision was made from the same
		 * publication: the frame's claims are always within the revision's set. Until then the last applied claims stand,
		 * and the commits in between merge into one application (setApply, by slot, its geometry the latest commit's).
		 */
		void ApplySet();

		/**
		 * @brief The coordinator's pass (T6b3d: the pump's drain, ScenePass, on the scene lane; or inline under the parities): the frame
		 * inputs and the posts taken, the engine's queues drained up to where each stood at the pass's start (CollectEvents) and applied,
		 * the walk, the joins (membership bound, residents kept, decals ordered, the new pipelines' PerGeometry blocks and the bindings),
		 * the set's commit, then the publication (PublishScene: the set applied, the tables and lookups published, the snapshot builder
		 * handed the publication) - held back while the builder has not taken the last one (one publication per builder take, the
		 * passes between coalescing into it). Any number of passes a frame (bursts, or none in menus). While DCLF does not run (not
		 * loaded, switched off) a pass applies the events alone. What it drops of PrimaryCull's is held (primaryNotes). a_task: on the
		 * lane (its placements take read leases).
		 */
		void RunSceneWork(bool a_task);
		/** @brief Claims revoked mid-frame since the last call (RevokeUndrawnClaims): geometries, and the phases taken back. */
		std::pair<std::uint64_t, std::uint64_t> TakeRevokedClaims() { return { std::exchange(revokedGeometries, 0), std::exchange(revokedMain, 0) }; }
		/** @brief Of them, those taken back for a structural change after the selected revision's join (since the last call). */
		std::uint64_t TakeStructureRevocations() { return std::exchange(revokedStructureGeometries, 0); }
		struct SetStats
		{
			std::uint64_t commits = 0, evaluated = 0, joined = 0, left = 0, readinessEvents = 0, resyncs = 0;
			std::uint64_t members = 0, waiting = 0, rebinding = 0;  // summed over the commits
			// Summed over the commits: the objects out of the set although some of their phases are ready (U5: a claim is the whole
			// object), what per-phase claims would have drawn of them.
			std::uint64_t partial = 0;
			std::uint64_t publications = 0;
			std::uint64_t patchedMember = 0;  // must be 0: the accumulate phase patched a member's binding (CommitSet keeps rebinds out)
			std::uint64_t leftAfterCommit = 0;  // slots out of phases of the commit's decision after it (LeaveSet)
			// Why a bound object waits, summed over the commits: its pipeline, its material, its pipeline's shadow mask, the shared
			// lookups (samplers, null and projected textures), its geometry, its decal slot, its layer partner, its shadow pipelines
			// or occlusion pipelines (or an alpha-tested caster's diffuse).
			// The reflection phase (8): its forward pipeline.
			std::array<std::uint64_t, 14> waitingBy{};
			// T6b3d: why a member left (its commit's verdict): the scene not built (a load), no phase to take part in (its record freed or
			// ineligible now, its phases off), not bound (its membership dropped: the joins), waiting (by waitingBy's index: leftWaiting),
			// its layer partner or a phase of the whole object not ready.
			std::array<std::uint64_t, 5> leftBy{};
			std::array<std::uint64_t, 14> leftWaiting{};
			std::uint64_t waitingRequeued = 0;  // waiting slots taken again for a readiness source they wait on
			std::string firstWaiting;
		};
		SetStats TakeSetStats() { return std::exchange(setStats, {}); }
		void ApplyFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned);
		/**
		 * @brief Render thread: whether the fade node's state is the GPU's alone: every listed fade root is owned (FadeStateCS's state is
		 * its members') and DCLF's alone to write (kFadeRootWritten, T7f checklist items 4-5), whatever the cut stands in for; no engine
		 * OnVisible steps it, so its node holds the write-back's milestones, and a reader takes the GPU's state or the settled one instead.
		 * The cut's set only marks the stood-in roots (kFadeRootStoodIn: T7b2's kAccumulated class).
		 */
		bool FadeOnGpu(const RE::NiAVObject* a_fadeNode) const { return a_fadeNode && tables.fadeRootIndex.contains(a_fadeNode); }
		/**
		 * @brief Render thread, the scene work joined (a report): why a tracked geometry's sun entry rule allows it - its record, its
		 * ineligibility reason and the caster rule's verdict - or that it is not tracked. Looked up by address, never dereferenced
		 * unless tracked.
		 */
		std::string DescribeSunCandidate(const void* a_geometry) const;
		std::size_t StoodInFadeRoots() const
		{
			return static_cast<std::size_t>(std::count_if(fadeRootOwned.begin(), fadeRootOwned.end(), [](const auto& a_root) { return a_root.second; }));
		}
		/** @brief A listed fade root's row from its node again (a new generation: the GPU's state restarts from it), owned bits kept. */
		void ReseedFadeRoot(const void* a_node);
		/** @brief True when the geometry sits under a tracked category node: the coordinator's tracked map, read where the caller runs. */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;
		/**
		 * @brief IsTracked from the render thread (PrimaryCull's RefreshWalk and RunAdmission) while a pass may be writing the map: an
		 * unsynchronized read, the open defect "The render thread reads the coordinator's tracked map" (dclf-open-defects.md).
		 */
		bool IsTrackedFromRenderThread(const RE::BSGeometry* a_geometry) const { return IsTracked(a_geometry); }
		/** @brief Object LOD: the index ranges a tracked BSSubIndexTriShape draws (its visible segments' runs), or null. */
		const std::vector<LodSegments::Range>* LodRangesOf(const RE::BSGeometry* a_shape) const
		{
			const auto it = lodRanges.find(a_shape);
			return it != lodRanges.end() ? &it->second : nullptr;
		}

		/** @brief How a tracked geometry came to be tracked (diagnostics: the capture check's untracked draws). */
		enum class TrackSource : std::uint8_t
		{
			AttachEvent,       // an attach event's subtree walk
			CategoryAppeared,  // the walk of a category node RefreshCategoryNodes found new
			Rescan,            // the full rescan after a load
		};
		/** @brief The frame a tracked geometry was added, and how; false when it is not tracked. */
		bool GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const;
		/** @brief The category node an object hangs under, or null (diagnostics). */
		RE::NiNode* CategoryNodeOf(RE::NiAVObject* a_object) const { return FindCategoryNodeLive(a_object, nullptr); }
		/**
		 * @brief The frame a category node was found, and why the refresh that found it ran (0 signature change,
		 * 1 forced by a detach or rescan); false when unknown (diagnostics).
		 */
		bool GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const;


		/** @brief Full eligibility (static and per-frame) of a tracked geometry; NotTriShape if untracked. */
		Ineligible Classify(RE::BSGeometry* a_geometry) const;


		/** @brief The lighting pass the main-camera accumulator holds for a geometry this frame, or null. */
		const AccumulatedPass* FindAccumulatedPass(const RE::BSGeometry* a_geometry) const;


		using PlacementPlan = DCLF::PlacementPlan;
		using ShadingItem = DCLF::ShadingItem;
		using FadeSeedItem = DCLF::FadeSeedItem;
		using TreeSeedItem = DCLF::TreeSeedItem;
		using WetnessValue = DCLF::WetnessValue;
		/** @brief Render thread (diagnostics, the parities' inline scene work): the plan the last walk made, not published yet. */
		const PlacementPlan* PeekPlacementPlan() const { return placementPlanReady.get(); }

	private:
		std::vector<std::shared_ptr<const void>> retiredImports;  // the coordinator's, into the publication log (TakeRetiredImports)
		// The slots named for the next frame's shading sample (ShadingItem): by the walk and the accumulate phase, into the publication
		// log (TakeShadingItems). Every slot when the shading events are not installed.
		std::vector<ShadingItem> shadingNamed;
		/** @brief A slot's ShadingItem into shadingNamed (none for a free, unbound or propertyless slot). */
		void NameShading(std::uint32_t a_slot, bool a_member);
		/**
		 * @brief The walk's: the shading events (LOD fades, emittance, the controllers' MaterialSources writes) drained and their
		 * dependents named, with their extras' static rows written again; every slot the first time and while the events are not
		 * installed. a_slots: the slots named (the parity's), when given.
		 */
		void NameShadingEvents(std::vector<std::uint32_t>* a_slots = nullptr);
		/** @brief The dependents of shading event keys (properties, emittance colours) named; their slots into a_slots when given. */
		void NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots);
		// The walk's placement plan (PlacementPlan): filled by its rounds and PublishPlacementPlan, ready once the walk is done.
		std::shared_ptr<PlacementPlan> placementPlan, placementPlanReady;
		// T6b3d: plans no publication took yet, oldest first: the next publication carries them all (TakeDeltas), as the frame takes every
		// plan (a plan makes its pass publish: DeltasPending; kept for a pass that threw before its publication).
		std::vector<std::shared_ptr<PlacementPlan>> placementPlansHeld;
		// T6b3d: the per-frame entries are walked once a frame input (DeltaWalk): the frame the last such walk was for, whether this walk
		// took them, and the movers that walk listed (a later walk of the same frame's plan carries them: PublishPlacementPlan).
		std::uint32_t perFrameFrame = ~0u;
		bool perFrameWalked = true;
		// T6b3e: keys and slots only, never references: a reference kept across passes would be released by the pass that drops it (a
		// load, the next per-frame walk), on the pump's thread, where an engine destructor must not run. A carrying plan copies its
		// references again from the entries and the root owners (PublishPlacementPlan).
		struct CarriedItem
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t slot = kNoObjectSlot;
		};
		std::vector<CarriedItem> lastMovers;
		// T6b3e (e0): the roots that walk listed (QueueRoots: the per-frame work), carried likewise, with their dependents.
		struct CarriedRoot
		{
			const RE::NiAVObject* root = nullptr;
			std::vector<CarriedItem> dependents;
		};
		std::vector<CarriedRoot> lastRoots;
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			// T6b1b: its shader property and layer property, the mirror's (its geometry record's), held from the pins of the event that
			// named them (HoldProperties): what the scene work's references to them are copies of, never the live geometry's.
			RE::NiPointer<RE::BSShaderProperty> property, layerProperty;
			RE::BSShaderProperty* HeldProperty(bool a_layer) const { return (a_layer ? layerProperty : property).get(); }
			// T6b1b: written while the mirror held no record of it (WriteObject): evaluated again when a capture's batch names it.
			bool leafWaiting = false;
			std::uint64_t identity = 0;
			std::uint64_t groupIdentity = 0;
			const RE::TESObjectREFR* actorOwner = nullptr;  // lookup only, never published
			const RE::NiNode* roomNode = nullptr;  // lookup key, refreshed on structural attachment
			std::uint64_t roomMapGeneration = 0;
			int roomIndex = -1;
			RE::NiNode* categoryNode = nullptr;
			// Ineligible::UnsupportedParent or Billboard for what lies between the leaf and its category node
			// (ParentReason in Coordinator.cpp); Switch when a switch node lies there, which ClassifyFrame
			// decides per frame; else None.
			Ineligible parentReason = Ineligible::None;
			// When and how it was added (diagnostics, GetTrackInfo).
			std::uint32_t trackedFrame = 0;
			TrackSource trackedBy = TrackSource::AttachEvent;
			// What the observers note of it (Diagnostics/Scene/State.h: the timeline's stamps), made by the first that writes one.
			mutable std::unique_ptr<Diagnostics::TrackedNotes, Diagnostics::TrackedNotesDelete> notes;

			/**
			 * @brief A cached "this object cannot be drawn", and the witnesses that keep it honest.
			 *
			 * Only *negative* verdicts are cached. A positive one is not a property of the object: the
			 * descriptors it produces come from the engine's per-frame render pass, so it has to be
			 * derived again each frame. A negative is a property of the object - of its type, its skin
			 * instance, its shader property's flags and its material - and re-deriving it every frame is
			 * the single largest piece of waste in BuildFrame. In the Whiterun exterior 6097 of 9022
			 * tracked objects produce one, every frame, and it is thrown away.
			 *
			 * The witnesses are pointers that change when the thing behind them does, compared in full
			 * every frame. Cheap, and they close the one dangerous case: a stale *positive* over freed
			 * renderer data is a device loss, which is why positives are not cached at all and why
			 * rendererData is witnessed even so - a negative that became stale the other way would
			 * silently leave an object native forever.
			 *
			 * Entry lifetime covers attach, detach, cell teardown and the post-load rescan, because the
			 * whole Tracked is destroyed. ValidateSlice covers what has no cheap witness.
			 */
			struct StaticVerdict
			{
				bool cached = false;
				Ineligible reason = Ineligible::None;
				const RE::BSGraphics::TriShape* rendererData = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
			};
			StaticVerdict verdict;

			/**
			 * @brief The positive derivation, cached (CS_DCLF_DERIVED_CACHE): everything the loop derives
			 * for an accumulated object that is fixed until a witness changes - the descriptors, the static
			 * object flags, the pipeline key and the three table slots. The witnesses are the four pointers
			 * StaticVerdict uses, the fade state, the accumulated pass's technique / sub-pass / hint (the
			 * per-frame bits live in the technique), the interior flag, whether the material alpha is
			 * below one, and the decal bias modes of the frame; a slot is also checked against its key
			 * before it is served, so a swept and reused slot cannot be handed back.
			 */
			struct Derived
			{
				bool valid = false;
				std::uint32_t generation = 0;  // the slot tables' generation the slots belong to
				const RE::BSGraphics::TriShape* triShape = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
				bool interior = false;
				bool alphaBelowOne = false;
				std::uint32_t technique = 0;
				std::uint32_t subPass = 0;
				std::uint32_t hint = 0;
				std::uint32_t biasWitness = 0;
				LightingDescriptors descriptors;
				std::uint32_t staticFlags = 0;
				PipelineKey key{};
				std::uint32_t geometrySlot = ~0u;
				std::uint32_t pipelineSlot = ~0u;
				std::uint32_t materialSlot = ~0u;
			};
			Derived derived;

			/**
			 * @brief The cull-only verdict: whether the object is a culling candidate when the engine did not keep
			 * it, as the full classification last found it, with the frame it was found on. It stands until one of
			 * the delta walk's events takes it again (dclf-event-driven-tables.md, "Phase 3").
			 */
			std::uint32_t candidateFrame = 0;  // the pass that classified it (T6b3d: PassStamp, "classified in this walk"); 0: never classified
			Ineligible candidateReason = Ineligible::None;
			// An NPC face shape: a BSDynamicTriShape under a BSFaceGenNiNode, whose positions are FaceSnapshots'.
			// Resolved once, by the walk.
			bool faceShape = false;
			bool faceShapeResolved = false;
			// Its head (a key), while listed in faceHeads; and whether its last write found no snapshot of the head, so that the
			// head's next publication writes it again.
			const RE::BSFaceGenNiNode* faceHead = nullptr;
			bool faceWaiting = false;
			// T6b1b: a dynamic shape under a BSFaceGenNiNode (the mirror's parent): the head, held from the batch's pins at its attach (the
			// face snapshot's record's reference is a copy).
			RE::NiPointer<RE::BSFaceGenNiNode> faceHeadRef;
			// T6b3e: its property's fade node (the property record's fadeNode: the BSFadeNode or BSTreeNode above it), held from the pins of
			// the event that named the property (HoldProperties): what the listed tree and fade-root owners are copied from (ListTree,
			// ListFadeRoot), never a reference made from the mirror's key.
			RE::NiPointer<RE::NiAVObject> fadeNodeRef;
			const void* fadeNodeRequested = nullptr;  // T6b3e: the fade node key asked of the render thread (FadeNodeRequest), until answered
			// T6b5: the LOD gate whose roots it hangs under (its token; 0: none) and on which side (kGateIncoming, kGateOutgoing), until the
			// gate's outcome; an outgoing block's geometry held out of the set after its gate flipped, until its detach erases the entry; and
			// whether any gate ever tagged it (the residue's "shown with no gate").
			std::uint64_t gate = 0;
			std::uint8_t gateSide = 0;
			bool gateHeldOut = false;
			bool gateSeen = false;
			// Owned by an actor (its GetUserData is an ActorCharacter): Advanced Skin gives its draws the actor's
			// wetness (Tables::skinWetness). Resolved once, by the walk.
			bool actorOwned = false;
			bool actorOwnedResolved = false;
			// The accumulate phase's verdict when it left the object without bindings, and the frame it did so
			// (ReasonThisFrame).
			Ineligible accumulateReason = Ineligible::None;
			std::uint32_t accumulateReasonFrame = 0;  // the frame (T6b0's timeline compares it with writtenFrame)
			std::uint64_t accumulateReasonPass = 0;   // T6b3d: the pass (a join failed for a verdict of this pass)
			// Its index in this walk's tables, valid while objectStamp equals Coordinator::objectStamp. Kept here
			// rather than in a geometry -> index map rebuilt by every walk: the map's insert was ~0.12 us an object,
			// and every consumer already has the entry (the accumulate phase) or looks it up by the same key.
			std::uint32_t objectStamp = 0;
			std::uint32_t objectId = 0;
			// Its persistent object slot: the index objectId holds while the object keeps a record, walk after walk.
			// kNoObjectSlot while it has none.
			std::uint32_t slot = kNoObjectSlot;
			// The node whose bound decides whether the object is a sun caster candidate (ResolveSunEntry), resolved
			// once: the scene graph above a tracked geometry does not change while it is tracked.
			const RE::NiAVObject* sunEntryNode = nullptr;
			bool sunEntryResolved = false;
			// Its light entry (LightEntryOf), listed in lightDependents for as long as it is tracked; null: none.
			const RE::NiAVObject* lightRoot = nullptr;
			// Its main-pass layer's object slot (LayerPropertyOf: a multi-index shape's additional property, drawn from the
			// second index list in decal group 3), kNoObjectSlot while it has none. Written with the base record (WriteLayer),
			// bound and dropped with it; its positive derivation is layerDerived.
			std::uint32_t layerSlot = kNoObjectSlot;
			Derived layerDerived;
			const void* listedLayerProperty = nullptr;  // the layer's property in propertyDependents
			// T6b3e: its membership passes' derived descriptors, the base's and the layer's (PrimaryCull::MembershipPass, which returns the
			// entry BindByMembership stores here): PrimaryCull's per-geometry maps, which nothing erased, moved into the entry they belong to.
			MembershipDerived membershipDerived, membershipLayerDerived;
			// The delta walk's. perFrame: inputs that change from frame to frame (PerFrameTraits: owned by an actor,
			// skinned, a face shape, under a switch, or a controller or a non-fixed rigid body on its chain), so the
			// delta walk evaluates it every frame; set at an evaluation that classified it, and kept. scheduledWalk:
			// the walk that last scheduled it. bucket: the histogram bucket it is counted in (kNoBucket: none).
			bool perFrame = false;
			bool perFrameListed = false;
			// lightTraits: the entry is per frame only because it moves (kTraitMoves, kTraitRootMoves), follows a switch
			// (kTraitSwitch), has animated shading (kTraitAnimatedShading) or is skinned (kTraitSkin; a tree's wind moves
			// its bones). While its classification stands, the full walk would take nothing else again: a switch's
			// verdict is taken again and the shading's inputs compared (the record is written in full when either
			// changes), and a skin or a record that moves is listed for FrameValues (PlacementPlan::movers; KeepSkin checks a skin's
			// palette size first). 0 for every other entry. movedWalk: the walk that took this path.
			std::uint32_t lightTraits = 0;
			std::uint32_t movedWalk = 0;
			// The key its move events carry (MoveKeyOf): the one reference on its chain up to the category node, set at
			// classification. Null when there is none or more than one, and the light path places it every frame.
			const void* moveKey = nullptr;
			// An actor's light-path entry: the nodes from it up to its category node, listed in hiddenDependents, and whether one
			// of them had a hidden-bit event since the entry was last evaluated (DrainHiddenEvents sets it, MergeEntry clears it). Not
			// the pass that drained it: a pass a hidden store wakes outside the frame's render is an event walk, which does not evaluate
			// the per-frame entries, so the event must wait for the frame's next walk (the first-person switch's hide of the
			// third-person skeleton was lost that way).
			std::vector<const RE::NiAVObject*> hiddenChain;
			bool hiddenEventOwed = false;
			// kTraitAnimatedShading: what the record read from the animated shader and alpha properties when it was
			// last written in full (ShadingInputsOf); the light path writes it in full again when they differ.
			std::uint64_t shadingInputs = 0;
			// kTraitSwitch: the one switch node on its chain and its child on the leaf's path, found at classification,
			// so the light path reads the selection without walking the chain (null: none, or several).
			RE::NiSwitchNode* switchNode = nullptr;
			const RE::NiAVObject* switchChild = nullptr;
			// The keys it is listed under: its shader and alpha properties in propertyDependents (the last evaluation's),
			// its sun entry node in rootDependents (for as long as it is tracked).
			const void* listedProperty = nullptr;
			const void* listedAlpha = nullptr;
			const void* listedEmittance = nullptr;  // external emittance's shared colour (emittanceEvents)
			const RE::NiAVObject* listedRoot = nullptr;
			// A per-frame entry written in full (an actor's, a face's): what its classification read from the geometry,
			// its properties and its material (ClassifyInputsOf), re-read every frame. Its classification is taken again
			// when they differ; the hidden, actor and switch half is taken again every frame.
			std::uint64_t classifyInputs = 0;
			// CS_DCLF_INPUT_WATCH: the components the two hashes were taken from (InputComponentsOf), when they were stored.
			std::unique_ptr<std::array<std::uint64_t, 12>> inputComponents;
			std::uint32_t fullWalk = 0;  // the walk that must write it in full (an event, not the per-frame set)
			std::uint8_t bucket = kNoBucket;
			std::uint32_t scheduledWalk = 0;
			static constexpr std::uint8_t kNoBucket = 0xFF;
		};
		/**
		 * @brief The object's entry bound for the sun's cascade culls. The cascade cull (FUN_140e305c0) walks
		 * only the entries of the full-frustum culling processes' objectArray, which the full-frustum cull
		 * (FUN_141511f30) fills with the items passing their planes. Measured against the engine's culls (0 false
		 * rejects): a static reference's entry is its reference root (the topmost ancestor carrying the
		 * geometry's userData); an actor's is its cell's container, never tested; a geometry without a
		 * reference (a terrain block) has its nearest BSMultiBoundNode.
		 */
		void ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live = false);
		/** @brief ResolveSunEntry's from the mirror alone. */
		const void* MirrorSunEntry(const void* a_geometry) const;
		/** @brief ResolveSunEntry's from the live chain (a live walk's, the observers' check): the reference's root, or the multibound above. */
		static const RE::NiAVObject* LiveSunEntry(const RE::BSGeometry& a_geometry);
		/** @brief LightEntryOf's walk on the live chain: the category node's child a_geometry hangs from, and whether it is an actor's (a_actor). */
		static const RE::NiAVObject* LiveLightRoot(const RE::BSGeometry& a_geometry, const RE::NiNode* a_category, bool& a_actor);

		/**
		 * @brief The sun entries whose whole content DCLF can take out of the engine's cascade culls (SunCandidates,
		 * SunAccumulation), kept from the scene events: at the end of the delta walk, every entry a change touched (a
		 * dependent attached, detached, or gaining or losing its record or its verdict) is judged again.
		 *
		 * Any change bumps the generation at once; the snapshot is rebuilt only on a walk that changed nothing, since
		 * an exclusion built for an older generation is never applied (SunAccumulation::ExcludeEntries) and a cell
		 * load would otherwise rebuild it every frame.
		 */
		void UpdateSunCandidates(bool a_full);
		void DropSunCandidates();
		/** @brief A tracked entry's verdict as the eligibility rules read it (Eligibility::EntryVerdict). */
		/** @brief A tracked geometry's primary verdict (Eligibility::PrimaryVerdict); kPrimaryEngine when it is not tracked. */
		std::uint8_t PrimaryVerdictOf(RE::BSGeometry* a_geometry) const;
		static Eligibility::EntryVerdict VerdictOf(const Tracked& a_tracked) { return { a_tracked.slot != kNoObjectSlot, a_tracked.candidateFrame != 0, a_tracked.candidateReason }; }
		void MarkSunEntryDirty(const RE::NiAVObject* a_entry)
		{
			if (a_entry)
				sunEntriesDirty.push_back(a_entry);
		}
		void MarkLightEntryDirty(const RE::NiAVObject* a_entry)
		{
			if (a_entry)
				lightEntriesDirty.push_back(a_entry);
		}
		/**
		 * @brief A tracked geometry's light entry (UpdateLightCandidates): its actor's root when it hangs from one (its category
		 * node's child, whose reference is an actor: an actor has no sun entry), else its sun entry unless that is a category
		 * node (a cell's or a room's multibound node, which holds other entries).
		 */
		const RE::NiAVObject* LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const;
		void UpdateLightCandidates(bool a_full);
		/**
		 * @brief A light entry's tracked geometries (lightDependents), or null; null too for a node that has become a category
		 * node since (left out of a point light's cull, it would take every entry under it along).
		 */
		const std::vector<RE::BSGeometry*>* LightDependentsOf(const RE::NiAVObject* a_root) const;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> lightDependents;  // by light entry
		ankerl::unordered_dense::set<const RE::NiAVObject*> lightCandidateSet;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> lightSignature;  // its geometries, as a set
		std::vector<const RE::NiAVObject*> lightEntriesDirty;
		std::shared_ptr<const SunCandidates> lightCandidates;
		std::uint32_t lightCandidatesGeneration = 0;
		std::uint64_t lightEntriesAppeared = 0;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunCandidateSet;
		// Per sun candidate, a signature of its geometries' PrimaryEntryAllows verdicts: a change bumps the generation.
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> primarySignature;
		std::vector<const RE::NiAVObject*> sunEntriesDirty;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunEntriesForced;  // candidates whose subtree an attach or detach changed
		std::shared_ptr<const SunCandidates> sunCandidates;
		std::uint32_t sunCandidatesGeneration = 0;
		// The sun's and the point lights' candidate index spaces (CandidateTable.h), one version counter for both.
		CandidateTable sunTable, lightTable;
		std::uint32_t candidateVersions = 0;

		void RefreshCategoryNodes(bool a_force = false);
		/**
		 * @brief Render thread, the frame's start (not while a load screen is up): the newest CategoryCapture, made when a_force or the
		 * signature moved, and posted (categoryPosted); the portal roots it finds new or gone pushed onto the tracker's stack, mirror-only
		 * (SceneTracker::PushCaptured), before the post: a pass that takes the post drains them first.
		 */
		struct EventBatch;
		struct BatchUnwindGuard;  // T6b3e: a batch to the render thread on a throw (Internal.h)
		/** @brief T6b3d, the coordinator, first in its pass (before the drains): the newest category capture posted, taken. */
		void TakeCategoryCapture();
		std::shared_ptr<const CategoryCapture> categoryTaken;
		/** @brief T6b3e, the coordinator (ApplyBatch): the answers into their entries (Tracked::fadeNodeRef), each entry written again. */
		void ApplyFadeNodeAnswers();
		// The signature the category set was last rebuilt for.
		std::uint64_t categorySignature = 0;
		/**
		 * @brief The category node a_object hangs under (null for none), and the strongest parent reason on the way: the mirror's
		 * chain (T6b1b), the scene as the applied batch left it. The pointer is a key (never read through). CS_DCLF_MIRROR_PARITY
		 * checks it against the live chain (MirrorReads).
		 */
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored = nullptr) const;
		/** @brief The same from the live parent chain: the parities' and the diagnostics' (the object alive). */
		RE::NiNode* FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const;
		/**
		 * @brief T6b1b: the lane's reads ported to the mirror, each checked against the live read under CS_DCLF_MIRROR_PARITY (the
		 * object alive: the lane holds it). Counted per read since the last report: checked, differing, and the reads that found no
		 * record on the way (the mirror lacking an object the lane names).
		 */
		enum class MirrorRead : std::uint8_t
		{
			CategoryNode,
			Subtree,
			SwitchEvent,
			NodeEvent,
			HiddenChain,
			MoveKey,
			LightEntry,
			SunEntry,
			Reference,  // T6b1c: a geometry the mirror's walk found with no pin (lagged: the live scene no longer has it there)
			kCount
		};
		static constexpr std::array<const char*, static_cast<std::size_t>(MirrorRead::kCount)> kMirrorReadNames{ "category node", "subtree", "switch event", "node event",
			"hidden chain", "move key", "light entry", "sun entry", "unpinned geometry" };
		/** @brief A mirror read's check: counts it; a difference (a_describe() it) is checked again after the next batch (DeferredRead). */
		template <class Describe, class Recheck>
		void NoteMirrorRead(MirrorRead a_read, bool a_differ, Describe&& a_describe, Recheck&& a_recheck) const
		{
			++MirrorReads().checked[static_cast<std::size_t>(a_read)];
			if (a_differ)
				deferredReads.push_back({ a_read, a_describe(), std::function<bool()>(std::forward<Recheck>(a_recheck)) });
		}
		/** @brief The geometries under a_root as the mirror's children give them, each with the parent reasons below a_reason, in the walk's order. */
		void MirrorSubtree(const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored) const;
		/**
		 * @brief T6b1d: a live check's lease. The scene work is not joined at Present, so its observers' live reads each take one (the
		 * engine-read window); refused (the window closed: the engine's update may be running), the check is skipped and counted.
		 */
		EngineReadWindow::Lease LiveCheckLease(MirrorRead a_read) const
		{
			EngineReadWindow::Lease lease;
			if (!lease)
				++MirrorReads().refused[static_cast<std::size_t>(a_read)];
			return lease;
		}
		struct MirrorReadStats
		{
			std::array<std::uint64_t, static_cast<std::size_t>(MirrorRead::kCount)> reads{}, checked{}, lagged{}, differ{}, unmirrored{}, refused{};
			std::array<std::string, static_cast<std::size_t>(MirrorRead::kCount)> first;
		};
		mutable MirrorReadStats mirrorReads;
		/**
		 * @brief T6b3e: where this thread counts its mirror reads: an evaluation chunk's counters (EvalCounters::mirror, installed by its
		 * ShardScope, merged in chunk order), else the store's. Every count goes through MirrorReads().
		 */
		static inline thread_local MirrorReadStats* mirrorReadSink = nullptr;
		MirrorReadStats& MirrorReads() const { return mirrorReadSink ? *mirrorReadSink : mirrorReads; }
		/**
		 * @brief A read the check found differing: the mirror is the scene as the batch left it, the live read is later (what changed
		 * since is the next batch's). Checked again after the next batch (a_recheck: whether the mirror now gives what the live read
		 * gave, from the mirror and the values it captured alone): agreeing, it lagged; differing still, it is counted (TakeDeferredReads).
		 */
		struct DeferredRead
		{
			MirrorRead read;
			std::string what;
			std::function<bool()> recheck;
		};
		mutable std::vector<DeferredRead> deferredReads;
		void RecheckDeferredReads();
		/** @brief FindCategoryNode's chain, the mirror's alone (no counting, no check). */
		RE::NiNode* MirrorCategoryNode(const void* a_object, Ineligible& a_reason, bool& a_unmirrored) const;
		/**
		 * @brief Tracks the geometries under a_root (a category node above it). One the mirror holds no chain for (an attach no hook
		 * captured) is captured by the render thread at the next frame's start (mirrorCaptureRequests) and tried again after that batch
		 * (pendingSubtrees, once). An attach event's root the mirror has no chain for was out of the world at its attach (its world
		 * attach brings its own event): not tried again.
		 */
		enum class SubtreeSource : std::uint8_t
		{
			AttachEvent,
			CategoryWalk,
			Retry,
		};
		void AddSubtree(RE::NiAVObject* a_root, SubtreeSource a_source = SubtreeSource::AttachEvent);
		std::vector<RE::NiPointer<RE::NiAVObject>> pendingSubtrees;
		std::vector<RE::BSGeometry*> validationSuspects;  // ValidateSlice's: keys, checked again only while tracked
		std::uint64_t subtreesPended = 0, subtreesDropped = 0;  // since the last report: waiting for the mirror, and still without a chain after
		std::uint64_t categoryChildrenMissing = 0;  // a new category node the capture listed no children of (T6b1c: should not be)
		/**
		 * @brief Tracks a_geometry under a_categoryNode. Its reference is the batch's pin of it (T6b1c), or with a_live a live walk's
		 * pointer (the category walk's, until the render thread's category capture pins); without either it is left out (counted).
		 */
		struct GeometryPrep;
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live, const GeometryPrep& a_prep);
		// T6b3e e5: AddSubtree as a prepare (pure over the batch's frozen mirror, any thread under a ShardScope) and a merge in event order.
		/**
		 * @brief T6b3e e5: what AddGeometry reads of the mirror for one geometry, prepared: its record's actor owner, Light Limit Fix's room
		 * node, a face shape's head, whether the mirror holds its node, whether it is a sub-index shape, and (resolved: not under the
		 * mirror-read parity, whose live checks are the merge's) its sun entry (ResolveSunEntry's read) and LightEntryOf's actor root.
		 * Keys alone: no reference is made or dropped in a prepare.
		 */
		struct GeometryPrep
		{
			RE::BSGeometry* geometry = nullptr;
			Ineligible reason = Ineligible::None;  // what the nodes between the category node and it make of it (MirrorSubtree's)
			const RE::TESObjectREFR* actorOwner = nullptr;
			const RE::NiNode* roomNode = nullptr;
			const RE::BSFaceGenNiNode* faceHead = nullptr;
			bool mirrored = false;
			bool subIndex = false;
			bool resolved = false;
			const void* sunEntry = nullptr;
			const RE::NiAVObject* lightActorRoot = nullptr;
		};
		/** @brief T6b3e e5: one subtree's prepare (PrepareSubtree): its category node (none: nothing tracked), and its geometries in walk order. */
		struct SubtreePrep
		{
			RE::NiAVObject* root = nullptr;
			SubtreeSource source = SubtreeSource::AttachEvent;
			RE::NiNode* category = nullptr;
			bool unmirrored = false;  // FindCategoryNode found a record missing on the way up
			std::vector<GeometryPrep> geometries;
		};
		/** @brief T6b3e e5: a_root's prepare: FindCategoryNode, MirrorSubtree (and its mirror-read parity check), each geometry's. Const. */
		void PrepareSubtree(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const;
		/** @brief T6b3e e5: one geometry's prepare under a_category (PrepareSubtree's, and AddSubtree's). Const. */
		void PrepareGeometry(RE::BSGeometry* a_geometry, Ineligible a_reason, const RE::NiNode* a_category, GeometryPrep& a_out) const;
		/** @brief T6b3e e5: a prepared subtree's merge (the pending retry, capture requests, AddGeometry for each), in event order. */
		void MergeSubtree(SubtreePrep& a_prep);
		/**
		 * @brief T6b3e e5: a_roots' prepares into a_out (by CS_DCLF_FANOUT: 1 on this thread, 2 on the preparation pool in chunks of
		 * kSubtreeGrain from kSubtreeFanoutMin roots; an observer may prepare them again here: Diagnostics::SubtreesPrepared), timed as
		 * `subtrees (fan-out)`.
		 * The caller merges them in order (MergeSubtree) after it; nothing between may write the mirror, the category nodes or the portal
		 * roots (asserted by the caller's witnesses).
		 */
		void PrepareSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots, std::uint32_t a_mode, std::vector<SubtreePrep>& a_out);
		/**
		 * @brief T6b3e e5: a_roots tracked as AddSubtree tracks each, in order: by CS_DCLF_FANOUT, 0 each prepared and merged before the next
		 * (the reference), else all prepared (PrepareSubtrees) then merged in order. Under the mirror-read parity as 0.
		 */
		void AddSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots);
		// A subtree's prepare is a whole walk (one root to hundreds of geometries): chunks of a few roots, the pool from 16.
		static constexpr std::size_t kSubtreeGrain = 4;
		static constexpr std::size_t kSubtreeFanoutMin = 16;
		std::vector<SubtreePrep> subtreePreps;
		/** @brief T6b3e e5: a_ms into a nested scene part's sums (the pass's and the frame's), timed by hand like e4's halves. */
		void AddScenePartMs(ScenePart a_part, double a_ms)
		{
			stats.scenePartMs[static_cast<std::size_t>(a_part)] += a_ms;
			stats.scenePartFrameMs[static_cast<std::size_t>(a_part)] += a_ms;
		}
		/**
		 * @brief T6b1c: the applied batch's pins by key (its attach captures' and leaf updates' (T6b1b: with the properties), the category
		 * capture's), valid while the scene work applies it; and a reference made from one (null: no pin). Counted since the last report:
		 * references made from pins, from a live walk's pointers, and refused for want of a pin.
		 */
		ankerl::unordered_dense::map<const void*, RE::NiRefObject*> batchPins;
		void PinBatch(const std::vector<RE::NiPointer<RE::NiRefObject>>& a_pins)
		{
			for (const auto& pin : a_pins) {
				batchPins.insert_or_assign(pin.get(), pin.get());
				++referenceStats.pins;
			}
		}
		template <class T>
		RE::NiPointer<T> Pinned(const T* a_key)
		{
			if (const auto it = batchPins.find(a_key); it != batchPins.end()) {
				++referenceStats.pinned;
				return RE::NiPointer<T>(static_cast<T*>(it->second));
			}
			return nullptr;
		}
		struct ReferenceStats
		{
			std::uint64_t pins = 0, pinned = 0, live = 0, refused = 0;
			std::uint64_t properties = 0, propertiesRefused = 0;  // T6b1b: a tracked geometry's properties held anew, and with no pin
			std::uint64_t fadeNodes = 0, fadeNodesRefused = 0;   // T6b3e: their fade nodes held anew, and with no pin
			// T6b3e: of the refused, copied from a root owner (the node is another tracked reference's root), asked of the render thread, and
			// its answers: held, or none (the property names another node now, or the geometry left the world); the first few named.
			std::uint64_t fadeNodesFromRoots = 0, fadeNodesRequested = 0, fadeNodesAnswered = 0, fadeNodesAnsweredNone = 0;
			std::uint32_t fadeNodeNamed = 0;
			std::string fadeNodeFirst;
			std::uint64_t fadeRootsUnowned = 0;                  // T6b3e: a tree or fade root left unlisted for want of its entry's reference
			std::string firstPropertyRefused;
		} referenceStats;
		/** @brief T6b1b: a_entry's properties as the mirror's geometry record names them, each held anew from the batch's pins. */
		void HoldProperties(Tracked& a_entry);
		/** @brief T6b1b: Light Limit Fix's room node for an object (GetParentRoomNode), from the mirror's chain. */
		const RE::NiNode* RoomNodeOf(const void* a_object) const;
		/** @brief T6b1b: records written without the mirror's leaf (none yet), and evaluated again when a capture named it. */
		struct LeafStats
		{
			std::uint64_t missing = 0, rescheduled = 0;
		} leafStats;
		/**
		 * @brief Skylighting::OcclusionTechnique (its radius ignored: the view's BuildDraws test it), from the records (T6b1b): the nearest
		 * fade node above the leaf's BSX flags walked up the mirror.
		 */
		std::uint32_t OcclusionTechniqueOf(const SceneCapture::LeafView& a_leaf, bool a_skylighting) const;
		void ValidateSlice();
		/** @brief The scene pass, before the walk: the frame inputs posted since its last pass (room map, pipeline frame). */
		void TakeAccumulateInputs();
		/** @brief The scene pass, after the joins: a used pipeline without a current PerGeometry block made from the pipeline frame. */
		void MakeNewPipelineConstants();
		std::shared_ptr<const GeometryPort::PipelineFrame> scenePipelineFrame;  // the scene lane's: the newest taken

		Ineligible ClassifyFrame(const Tracked& a_tracked) const;
		/**
		 * @brief CS_DCLF_BILLBOARDS: the face mode (0-5) of the one NiBillboardNode between a Billboard-parented leaf and its category node,
		 * when DCLF draws the leaf turned on the GPU (Billboards.h): a BSEffectShaderProperty BSTriShape without a skin, one billboard node
		 * on the mirror's chain, a mode NiBillboardNode knows; -1 otherwise (the leaf stays Ineligible::Billboard).
		 */
		std::int32_t BillboardModeOf(const Tracked& a_tracked) const;
		/**
		 * @brief T7f item 7 (CS_DCLF_FIRST_PERSON_DECALS): a decal of a first-person entry (an effect's: a skinned blood decal on the arms; a
		 * Lighting one: a weapon's BloodLighting) made a
		 * forward object of the first-person forward part (the engine's first-person accumulator draws it in a decal group after its opaque
		 * passes; DCLF draws it after the first-person opaque buckets, sorted with the forward members): no Deferred (the first-person draws are
		 * forward draws after the composite),
		 * its group's blend mode (the alpha property applied in the blended group alone), depth tested, written by the opaque group alone (depth
		 * modes 3 and 1), and the group's depth bias (forwardDecalGroup). The main view never draws a first-person entry, so no decal group
		 * is left empty by it. Anything else is left as derived.
		 */
		void FirstPersonDecalForward(const Tracked& a_tracked, LightingDescriptors& a_descriptors) const;
		/**
		 * @brief Whether a node is hidden for the walk's views: the frame's capture for the nodes whose kHidden bit the engine
		 * flips while the asynchronous walk runs (FrameGlobals::cullHidden, taken on the render thread just before the walk is
		 * kicked, sorted by pointer), else the node's own bit.
		 *
		 * - ShadowSceneNode::OnVisible hides a portal graph's always-render children and its shared node for the
		 *   room traversal, then restores each bit. It runs in the main camera's cull jobs.
		 * - TESWaterReflections::Update hides the player's 3D while a cube-map reflection updates, then restores
		 *   it. Main::Draw calls it on the render thread while the cull jobs run.
		 * - Main::Draw hides the player's first-person skeleton just after the walk is kicked and keeps it hidden
		 *   for every world view: it is listed as hidden.
		 *
		 * Reading the nodes instead took the player's face shapes (classified every frame) out of the tables on
		 * the frames a reflection updated.
		 */
		bool HiddenForWalk(const RE::NiAVObject* a_object) const;
		/** @brief HiddenForWalk from a_object's record (T6b1b: the mirror's hidden bit where the frame's capture has none). */
		bool HiddenForWalk(const void* a_object, const SceneCapture::NodeRecord& a_record) const;
		/** @brief SwitchSelects from the switch's record (T6b1b): its selected child is a_child, and current. */
		static bool SwitchSelects(const SceneCapture::NodeRecord& a_switch, const void* a_child)
		{
			return (a_switch.kind & SceneCapture::kKindSwitch) && a_switch.switchChild && a_switch.switchChild == a_child && a_switch.switchCurrent;
		}

		ankerl::unordered_dense::map<RE::BSGeometry*, Tracked> tracked;
		SceneIdentity sceneIdentity;
		/**
		 * @brief BuildFrame's iteration order: the objects the engine kept, then the rest.
		 *
		 * Members rather than locals so their capacity survives the frame, and each entry carries the
		 * accumulated pass it was found under so the loop does not look it up a second time.
		 */
		struct OrderEntry
		{
			RE::BSGeometry* geometry;
			Tracked* tracked;  // mutable: BuildFrame updates the cached verdict through it
			const AccumulatedPass* accumulated;
			bool layer = false;  // the accumulate phase's: the entry's layer (Tracked::layerSlot) rather than its base
		};
		std::vector<OrderEntry> order;
		// Lookup keys are valid only for this walk; the captured recordId is durable.
		ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::shared_ptr<const FaceSnapshots::HeadView>> capturedFaceHeads;
		// The accumulate phase's iteration: the frame's membership joins. A member for its capacity, like `order`.
		std::vector<OrderEntry> accumulateOrder;
		// Member decals (DecalOrder.cpp): object -> its chain, the decal key's group, technique and sub-pass. Joined with the
		// membership patch, left with the membership (DropResidentSlot). Their draw order within a chain is the scene's
		// (CS_DCLF_DECAL_ORDER, default stable) or the engine's scene lists' of the frame (=engine).
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> memberDecals;
		// The decals whose membership or key changed since the last OrderDecals (joined, patched again, left), and whether the
		// order is to be made again whole (the members cleared).
		std::vector<std::uint32_t> decalsChanged;
		bool decalsRebuild = true;
		void NoteDecalChanged(std::uint32_t a_object) { decalsChanged.push_back(a_object); }
		void NoteDecalsCleared()
		{
			decalsChanged.clear();
			decalsRebuild = true;
		}
		// The member decals in draw order, each with its key (DecalOrder.cpp): kept, so a change re-keys only the decals it names.
		struct KeptDecalOrder;
		std::shared_ptr<KeptDecalOrder> keptDecals;
		/** @brief Tables::decalOrdinal and decalCount: the member decals, in the engine's draw order. */
		void OrderDecals();
		/** @brief The kept decal order against one made whole now (keys taken again): the observers' reference (DecalsOrdered). */
		struct DecalOrderComparison
		{
			std::uint64_t decals = 0, staleKeys = 0, unordered = 0;
			bool differ = false;
			std::string first, staleFirst;  // the first difference, the first stale key
		};
		DecalOrderComparison CompareDecalOrder() const;
		/**
		 * @brief T6b2a: the scene work's material captures (MaterialPort::captures: a material's writer, an attach's leaf, a request), the
		 * newest per material, each with the reference its capture took; one unused for kMaterialSnapshotFrames frames is let go (its
		 * reference to the render thread's releases). The join makes a record from one at once (MaterialPort::Evaluate, the frame's
		 * sources in FrameGlobals); a material with none is asked of the render thread (Channels::materialRequests: FrameView::ServeMaterialRequests
		 * captures it at EarlyPrepass).
		 */
		struct HeldMaterial
		{
			std::unique_ptr<MaterialPort::HeldSnapshot> held;
			std::uint32_t frame = 0;  // taken or used
		};
		static constexpr std::uint32_t kMaterialSnapshotFrames = 8;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, HeldMaterial> materialSnapshots;
		struct MaterialSnapshotStats
		{
			std::uint64_t captured = 0, replaced = 0, stale = 0, aged = 0, made = 0, uncovered = 0, requested = 0;
		} materialSnapshotStats;
		/** @brief The scene work (ApplyEvents): the captures pushed since the last call, the newest kept; the render thread's answers to requests. */
		void DrainMaterialCaptures();
		/** @brief A capture's reference, to the render thread's releases (materialsHandedBack). */
		void HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held);
		/**
		 * @brief T6b2c step 7: the material records kept by the scene work (FrameConstants.cpp), each scene pass after the captures are
		 * drained, from its frame's sources (FrameGlobals::Current().material), no engine read:
		 * - a material captured since the last pass (its writer's, its attach's, a request's): every slot of it evaluated again from the
		 *   capture (MaterialPort::Evaluate), its own values written where they differ (RewriteCapturedMaterials);
		 * - the frame-sourced components: per signature, one port evaluation of a kept capture (a probe, pure data) against the frame's
		 *   sources, written into every slot of the signature when it moves (RefreshMaterialSignatures);
		 * - TexcoordOffset: a material captured or keyed is watched while its two buffers differ (and two passes after), the frame's
		 *   buffer written into its slots (RefreshMaterialTransforms).
		 * The tables' versions and material log carry the writes to the bindings and the publication. Replaces the render thread's
		 * frame copies (FrameTables' materials), their posts and the coordinator's inbox.
		 */
		void RefreshMaterialRecords();
		void RewriteCapturedMaterials();
		void RefreshMaterialSignatures();
		void RefreshMaterialTransforms();
		/** @brief The scene work: a slot's record made (a join) or written from a_snapshot: listed under its signature, its material watched. */
		void NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot);
		void WatchMaterialTransforms(const RE::BSShaderMaterial* a_material, const MaterialPort::MaterialSnapshot& a_snapshot);
		/** @brief The tables reset (ResetSlotTables): the signatures' slot lists, the watch and the retries dropped. */
		void ResetMaterialRecords();
		// The materials whose capture DrainMaterialCaptures kept since the last RefreshMaterialRecords, and those a rewrite could not
		// evaluate yet (an Advanced Skin key not set up: asked again while their capture is held).
		std::vector<const RE::BSShaderMaterial*> materialsCaptured;
		std::vector<const RE::BSShaderMaterial*> materialRewritesPending;
		struct MaterialSignature
		{
			std::vector<std::uint32_t> slots;      // listed when keyed (materialSignatureListed), dropped as the list is walked
			MaterialPort::MaterialSnapshot probe;  // a capture of one of its materials (pure data: no pointer in it is followed)
			std::uint32_t probePass = 0;
			bool probeValid = false;
			bool probeFailed = false;  // its last evaluation failed (an Advanced Skin key not set up): the next slot noted replaces it
			MaterialRecord applied;  // the frame components last written into its slots
			bool appliedValid = false;
		};
		ankerl::unordered_dense::map<std::uint32_t, MaterialSignature> materialSignatures;
		std::vector<std::uint32_t> materialSignatureListed;  // per material slot: its signature + 1, 0 when unlisted
		struct TransformWatch
		{
			MaterialSources::TextureTransforms transforms;  // the newest capture's
			std::uint32_t frame = 0;                         // the frame it was captured or keyed (T6b3d: a frame, not a pass)
		};
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, TransformWatch> transformWatch;
		struct MaterialRecordStats
		{
			std::uint64_t passes = 0, captured = 0, rewritten = 0, retried = 0, uncovered = 0, samples = 0, applications = 0, slotsApplied = 0,
						  transformsWatched = 0, transformsWritten = 0;
		} materialRecordStats;
		// T6b2c: the material slots' texture bindings, the scene work's (MaterialBindings; Coordinator/MaterialBindings.cpp).
		MaterialBindings materialBindings;
		/**
		 * @brief The scene work, after the records' writers (RefreshMaterialRecords, the joins): the answers drained (the slots waiting on
		 * them written), then the slots whose record changed (the tables' material log) and the slots newly used, each resolved.
		 */
		void UpdateMaterialBindings();
		/** @brief The scene work: a used slot's entry written from its record, its views' bindings asked for where none is known. */
		void ResolveMaterialBinding(std::uint32_t a_slot);
		/** @brief The scene lane, before a commit or a publication: the entries written since the last call, each a new version, logged. */
		void VersionMaterialBindings();
		/** @brief A view's binding: answered in this drain, cached from an earlier answer, else asked for (pending; a_slot waits on it). */
		GpuTextures::Binding MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot);
		/** @brief The scene work: a material slot's retirement came back (no publication names it): its entry dropped. */
		void RetireMaterialBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped; an answer to an earlier request is stale. */
		void ResetMaterialBindings();
		// T6b2c: the shared, projected, shadow mask and shadow texture bindings, the scene work's (SharedBindings; Coordinator/SharedBindings.cpp).
		SharedBindings sharedBindings;
		/**
		 * @brief The scene work, after the technique rows' refresh: the answers drained, then the fixed bindings and the projected capture
		 * taken where they are new, each used pipeline's technique mask resolved, and the shadow textures the dependency index added or
		 * removed since the last pass (Tables::shadowTextureChanges) asked for or let go.
		 */
		void UpdateSharedBindings();
		/** @brief A view's binding: answered in this drain, cached from an earlier answer (here or the material bindings'), else asked for. */
		GpuTextures::Binding SharedViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, SharedBindings::Kind a_kind);
		/** @brief The scene work: the projected entries from the capture they are for, the pending ones asked for. */
		void ResolveProjectedBindings(std::uint32_t a_textures);
		/** @brief The scene work: a used pipeline's technique mask entry from its row. */
		void ResolveMaskBinding(std::uint32_t a_slot, ID3D11ShaderResourceView* a_frameMask);
		/** @brief The scene work: a shadow texture's entry from an answer or a known binding, asked for when there is none. */
		void ResolveShadowTextureBinding(ID3D11ShaderResourceView* a_view);
		/** @brief The scene work: a pipeline slot's lookups retirement came back (no publication names it): its mask entry dropped. */
		void RetireMaskBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped (but the render thread's posted capture). */
		void ResetSharedBindings();
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		// The portal graphs' parentless roots (alwaysRenderChildren), each filed under its graph's shared portal node, a
		// category node: FindCategoryNode, RefreshCategoryNodes. The root is held while it is listed here.
		struct AlwaysRenderRoot
		{
			RE::NiPointer<RE::NiAVObject> root;
			RE::NiNode* category = nullptr;
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, AlwaysRenderRoot> alwaysRenderRoots;
		// T7e: the coordinator's copy of the applied capture's first-person root (CategoryCapture::firstPersonRoot): the category node of
		// the first-person members. Its kHidden is view-scoped (Main::Draw shows it only to draw first person): ClassifyFrame stops at it
		// before the hidden test, and its hidden events announce nothing (DrainHiddenEvents).
		const RE::NiNode* firstPersonCategory = nullptr;
		/** @brief T7e's diagnostic: the node that hid an entry of the first-person skeleton (ClassifyFrame), the first 16. */
		void NoteFirstPersonHidden(const Tracked& a_tracked, const void* a_key, const SceneCapture::NodeRecord& a_record, std::uint32_t a_depth) const;
		/** @brief T7e: whether a tracked entry is a first-person one (its category node the first-person root). */
		bool FirstPersonEntry(const RE::NiNode* a_categoryNode) const { return a_categoryNode && a_categoryNode == firstPersonCategory; }
		// Diagnostics: the frame each category node was found and the refresh's cause (GetCategoryInfo), and
		// the source AddGeometry stamps on new entries.
		ankerl::unordered_dense::map<const RE::NiNode*, std::pair<std::uint32_t, std::uint8_t>> categoryFound;
		TrackSource addSource = TrackSource::AttachEvent;
		std::size_t validationCursor = 0;
		// Set while a load screen is up, so the first frame after it rebuilds the tracked set from
		// scratch instead of trusting anything discovered across the load (ProcessEvents).
		bool rescanPending = false;
		std::uint64_t publishedLodForced = 0; // T6b5, the coordinator's: the generation of its last publication (NotePublished)
		FrameInputs sceneInputs;              // the coordinator's: the newest taken
		std::uint32_t verdictsApplied = 0;    // the coordinator's: the requests it has applied
		/** @brief The coordinator, first in each scene pass: the newest frame inputs taken (sceneInputs, sceneFrame), the verdicts dropped when asked. */
		void TakeFrameInputs();
		// The coordinator's frame number (the frame inputs' it last took) and its passes: GetFrame on a scene work thread, PassSerial.
		std::uint32_t sceneFrame = 0;
		std::uint64_t passSerial = 0;
		std::shared_ptr<const ankerl::unordered_dense::map<const RE::NiNode*, int>> roomMap;
		std::uint64_t roomMapGeneration = ~0ull;
		/**
		 * @brief The scene mirror: the hooks' records, applied by the scene work in event order (ApplyMirrorEvents). CS_DCLF_MIRROR_PARITY
		 * checks it (Diagnostics/Scene).
		 */
		SceneMirror mirror;
		void ApplyMirrorEvents(const EventBatch& a_batch);
		// The coordinator's: a load screen's attach and detach events, for the mirror (the first pass after the load takes them).
		std::shared_ptr<EventBatch> loadingCarry;

		Tables tables;
		// Step 6: the tables as the scene work left them, published as an immutable snapshot (PublishTables, the coordinator), and the
		// one the frame accepted (AcceptTables). Snapshots are pooled: one no frame holds any more is written again.
		std::vector<std::shared_ptr<Tables>> tablesPool;
		// Parallel to tablesPool: whether the snapshot equals the tables as they stood at its change log's end (6d: it is brought up
		// to date by replay); a snapshot just made is copied whole once.
		std::vector<std::uint8_t> tablesPoolReplayable;
		std::vector<std::uint8_t> replaySlotMarks;  // scratch: the object slots a replay copied
		// The last published snapshot (the coordinator's), and the one the frame accepted (the render thread's: its installed
		// publication's, the newest taken while none is installed). The pool writes no snapshot anyone holds.
		std::shared_ptr<Tables> publishedTables;
	public:
		/** @brief A thread running scene work (the lane, or a parallel loop's chunk of it): GetFrame is the coordinator's there. */
		static inline thread_local bool sceneWorkThread = false;

	private:
		Channels& channels = Channels::Get();
		// The coordinator's last copy of its category nodes, with whether its set changed since (copied again at the next publication).
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> publishedCategoryNodes;
		bool nodeSetsDirty = true;  // the coordinator's: the category nodes changed
		using LightEntryChange = DCLF::LightEntryChange;
		std::vector<LightEntryChange> lightEntryChanges;               // the coordinator's, since the last publication
		/** @brief The coordinator: whether a node is one of its category nodes (its own set; the frame reads IsCategoryNode). */
		bool CategoryNodeOwn(const RE::NiAVObject* a_node) const
		{
			return categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}
		/** @brief The coordinator, first in its scene pass: what the render thread posted for it since (above), applied in that order. */
		void TakeCoordinatorInputs();
		// The material records are the scene work's own (T6b2c step 7: RefreshMaterialRecords): nothing posted.
		ConstantsPostStats constantsPostStats;
		void ApplyConstantsPosts();
		// T6b2c: the technique rows' witness, the coordinator's - the frame inputs (FrameGlobals: TechniqueInputs, SetupTechniqueDescriptor's
		// bytes) every row was last evaluated against. The fog is left out: a row keeps the fog it was made with (KeepTechniqueFog).
		struct TechniqueWitness
		{
			TechniqueInputs inputs{};
			std::uint8_t byte12 = 0, byte7 = 0;
			bool valid = false;
		} techniqueWitness;
		/**
		 * @brief The coordinator, at the start of each scene work pass (its frame's FrameGlobals bound): when the frame's technique inputs
		 * differ from the witness, every technique row evaluated again (EvaluateTechniqueRow). New rows are evaluated where they are made
		 * (TechniqueRowFor), after this, from the same sample.
		 */
		void RefreshTechniqueRows();
		/** @brief The coordinator: a technique row from its frame's sample, written and versioned where it differs (a new row in full). */
		void EvaluateTechniqueRow(std::uint32_t a_row);
		// The change log's position at the last commit (CommitSet): what the applied set's revocation checks from.
		LogCursor setCommitCursor;
		TablesPublication tablesPublication;
		/** @brief The coordinator, at the end of the scene work: the tables into a pooled snapshot no one holds, published. */
		void PublishTables();
		// The walk whose object indices the Tracked entries' objectStamp must match; a new walk or a teardown
		// takes a new value, which invalidates every entry's index at once.
		std::uint32_t objectStamp = 1;
		void InvalidateObjectIndices() { ++objectStamp; }
		// Set only for the dense rebuild CS_DCLF_WALK_PARITY compares against: the walk then lays the objects out
		// densely and leaves the Tracked entries' slots and indices alone.
		bool denseWalk = false;
		/** @brief The slot the walk writes this object at: its own, a free one, or a new one. */
		std::uint32_t AcquireObjectSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry);
		/** @brief After a walk: frees every slot it did not write, and sorts the per-frame index lists. */
		void SweepObjectSlots();
		/**
		 * @brief Frees an entry's slot as the entry leaves the tracked set (render thread, outside the walk), so a
		 * rescan's new entries reuse the slots of the ones it replaces instead of growing the arrays past them.
		 */
		void ReleaseObjectSlot(Tracked& a_entry);
		/** @brief The tracked entry's layer slot for its base slot a_base: its own, or a new one (Tracked::layerSlot). */
		std::uint32_t AcquireLayerSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry, std::uint32_t a_base);
		void ReleaseLayerSlot(Tracked& a_entry);
		/** @brief The property a slot's record draws: its entry's held one (T6b1b: the mirror's), its layer's for a layer slot. */
		RE::BSShaderProperty* SlotProperty(std::uint32_t a_slot) const;
		/** @brief Whether a member's binding still holds for its object as it is now (the derivation cache's witnesses). */
		bool MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const;
		void EraseTracked(RE::BSGeometry* a_geometry);
		/**
		 * @brief The walk's reference (CS_DCLF_WALK_PARITY's, Diagnostics/Scene), right after the walk: every tracked entry written from scratch
		 * into dense tables (a_dense) and classified without the verdict caches (a_reasons), the store left exactly as it was.
		 */
		void DenseReference(Tables& a_dense, ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible>& a_reasons);
		// The dense rebuild classifies every entry from scratch, without the verdict caches, and keeps its verdicts
		// here: the classification itself is what walk parity checks the kept one against.
		ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible> referenceReasons;
		// The per-frame dedup maps. Members, not locals, so their buckets survive the frame
		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::vector<std::uint32_t>> materialDependents;
		void ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		void UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		// The frame's membership joins' passes (BindByMembership), by geometry.
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedPasses;
		// The frame's membership joins of layers (BindByMembership: PrimaryCull::MembershipLayerPass), by their geometry.
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedLayerPasses;
		/**
		 * @brief A reference on a BSShaderMaterial, taken and dropped the way the engine does it (AE).
		 *
		 * Materials live in the engine's material database (the manager at 0x143187758), which shares one
		 * material among every property with the same contents. Its release (FUN_1414f7a40, what
		 * BSShaderProperty::SetMaterial calls for the old material) decrements the count under the database's
		 * lock and, at zero, takes the material out of the database before deleting it. A BSTSmartPointer
		 * deletes it directly instead.
		 */
		class MaterialReference
		{
		public:
			MaterialReference() = default;
			MaterialReference(const MaterialReference&) = delete;
			MaterialReference& operator=(const MaterialReference&) = delete;
			MaterialReference(MaterialReference&& a_other) noexcept :
				material(std::exchange(a_other.material, nullptr)) {}
			MaterialReference& operator=(MaterialReference&& a_other) noexcept
			{
				if (this != &a_other) {
					reset();
					material = std::exchange(a_other.material, nullptr);
				}
				return *this;
			}
			~MaterialReference() { reset(); }
			/** @brief Takes a reference on a_material (null: none), then drops the one held. */
			void reset(RE::BSShaderMaterial* a_material = nullptr);
			/** @brief Takes over a count already taken on a_material (a HeldSnapshot's); holds none before. */
			void Adopt(RE::BSShaderMaterial* a_material) { material = a_material; }
			explicit operator bool() const { return material != nullptr; }

		private:
			RE::BSShaderMaterial* material = nullptr;
		};
		// Parallel to Tables::materials: the slot itself is the only record cache.
		// Its engine reference prevents pointer reuse while the slot is live. Last-object-
		// reference events and table resets release it at the safe capture boundary.
		std::vector<MaterialReference> materialOwners;
		// T6b1a: the fade roots' seed requests (TakeFadeSeeds), and a row's coordinator half from the mirror (the seed brings the rest).
		std::vector<FadeSeedItem> fadeSeedRequests;
		std::vector<FadeSeedItem> treeSeedRequests;
		FadeRootStatic FadeRootFromMirror(const void* a_node);
		/** @brief A new generation for root a_root's row, its seed row toggled, and its seed requested. */
		void RequestFadeSeed(std::uint32_t a_root, FadeRootStatic& a_row);
		/** @brief The coordinator's residency of a slot (IsMember is the frame's). */
		bool ResidentObject(std::int32_t a_object) const { return a_object >= 0 && IsResidentSlot(static_cast<std::uint32_t>(a_object)); }
		std::uint32_t tablesGeneration = 0;
		std::uint32_t materialVersions = 0;  // the last Tables::materialVersion handed out

		// T6b2c step 5: the lookups (Lookups.h), the scene lane's: written by its bindings where they are made (MaterialBindings,
		// SharedBindings) and resolved against the pipeline lane's newest catalog before each commit and publication (ResolveLookups),
		// which the set's readiness reads and each publication carries. The render thread reads them only with the coordinator idle.
		Lookups lookups;
		LookupsResolveState lookupsResolve;
		std::shared_ptr<const PipelineCatalog> lookupsCatalog;  // the catalog `lookups` were last resolved against
		// The last publication's lookups (shared again while unchanged: Lookups::ChangeKey), with their catalog; the chunk copies counted.
		std::shared_ptr<const Lookups> publishedLookups;
		std::shared_ptr<const PipelineCatalog> publishedCatalog;
		std::array<std::uint64_t, 6> publishedLookupsKey{};
		std::uint64_t lookupsChunkCopies = 0;
		/**
		 * @brief The scene lane, before a commit (CommitSet) or a publication (PublishScene), with its bindings written: the material
		 * entries versioned (VersionMaterialBindings), then the pipeline entries and the shadow pipelines resolved against the pipeline
		 * lane's newest catalog (DrawPipelines::TakeCatalog; IndirectDraws::ResolveLookups), which becomes lookupsCatalog.
		 */
		void ResolveLookups();
		/** @brief The scene lane, publishing: the lookups as an immutable copy with their catalog (the last copy again if unchanged). */
		void PublishLookups();
		/** @brief The lookups made new (the tables reset): a new instance, its generations past the last, its versions continuing. */
		void ResetLookups();
		// Frame state the scene phase reads once and the accumulate phase reuses, so that both halves of
		// one frame see the same answer even though they run either side of the shadow maps.
		bool sceneBuilt = false;  // the scene phase ran and the records are this frame's
		bool frameResolveBuffers = false;
		bool frameInterior = false;
		std::array<std::uint32_t, 4> frameDecalBias{};
		bool graphWasActive = false;  // resolveBuffers of the previous BuildFrame, to log the flip
		/** @brief Fills one object's extras rows (Prepass: the main camera's state is current). */
		/**
		 * @brief An object's extras rows' static parts (Tables::extraRows; the draw completes them from ExtrasFrame and its
		 * placement, CompleteExtras), from its property and geometry now; true when they changed (the caller notes kChangeExtras).
		 */
		bool WriteObjectExtras(std::uint32_t a_object);
		void BuildScenePhase();
		void BuildAccumulatePhase();
		// Engine-affine sampling ends before this packet is applied. Its values
		// can be propagated by a publication coordinator without dereferencing a
		// geometry, material, shader property or render pass.
		struct AccumulatePatch
		{
			std::uint32_t object = 0, material = 0, pipeline = 0, flags = 0;
			float fadeDistance = 0.0f;
			ObjectLights lights{};
			ObjectTreeAnim tree{};
			bool projectedUV = false, landBlend = false;
			std::uint64_t decalKey = 0;
		};
		void ApplyAccumulatePatch(const AccumulatePatch& a_patch);
		/** @brief Walk parity's reference: every entry of `order` written from scratch into dense tables (denseWalk). */
		void DenseWalk();
		/** @brief Clears the per-frame tables and the walk's per-frame counters; a_keepIndices: a delta walk's, which keeps the objects' indices. */
		void BeginWalk(bool a_keepIndices = false);

		/**
		 * @brief The scene phase evaluates only the objects whose inputs can have changed, on the render thread at Main::Draw's early hook, and every other
		 * slot keeps its record. What it evaluates:
		 *
		 * - the per-frame objects (Tracked::perFrame), every frame;
		 * - new entries, and entries something marked (pendingEvaluation);
		 * - the dependents of a shader or alpha property whose flags, material or controllers changed
		 *   (propertyDependents), classified again;
		 * - the entries under a node Havok moved or gave a controller, and the dependents of every sun entry node
		 *   above it (rootDependents), classified again;
		 * - the dependents of a sun entry node something was attached under or detached from;
		 * - a slot whose geometry slot went stale, or was re-resolved in place for another object.
		 *
		 * A classification stands until one of these events (dclf-event-driven-tables.md, "Phase 3"), and
		 * CS_DCLF_WALK_PARITY checks it against a full walk that classifies from scratch.
		 */
		void DeltaWalk();
		/** @brief Lays out the whole tracked set in `order` (a full evaluation, the parity's dense walk). */
		void BuildFullOrder();
		// T6b3e: the walk's evaluation and merge halves (defined with EvaluateRound).
		struct EvalCounters;
		struct TrackedDelta;
		struct EvalResult;
		/**
		 * @brief One entry of the scene walk: writes its record at its slot; false when it gets none this frame. T6b3e: its evaluation
		 * (EvaluateWrite) then its merge (MergeWrite), one after the other (the dense walk's).
		 */
		bool WriteObject(RE::BSGeometry* a_geometry, Tracked& a_tracked, PartTimer& a_timer, Ineligible& a_bucket);
		/**
		 * @brief The entry's layer record, written after its base record (a_base): the base's placement, the second index list,
		 * no shadow, no bindings until it joins with the base (BindByMembership). a_member: the base is a main-pass record
		 * (verdict None); without it, or without a layer, the layer slot is released. a_keepMember: the base kept its binding.
		 */
		void WriteLayer(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, Tracked& a_tracked, std::uint32_t a_base, bool a_member, bool a_keepMember,
			PartTimer& a_timer);
		/** @brief Why an entry's inputs change from frame to frame (Tracked::perFrame): PerFrameTrait bits, 0 when they do not. */
		enum PerFrameTrait : std::uint32_t
		{
			kTraitFace = 1u << 0,
			kTraitActor = 1u << 1,
			kTraitSwitch = 1u << 2,
			kTraitSkin = 1u << 3,
			kTraitAnimatedShading = 1u << 4,  // a controller on the shader or alpha property
			kTraitMoves = 1u << 5,            // a controller or a non-fixed rigid body on its chain
			kTraitRootMoves = 1u << 6,        // its reference root's subtree moves (RootMoves)
		};
		std::uint32_t PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const;
		/** @brief T6b3e: PerFrameTraits from the entry's face shape, actor ownership and category node as its evaluation has them. */
		std::uint32_t PerFrameTraitsOf(bool a_faceShape, bool a_actorOwned, const RE::NiNode* a_categoryNode, const RE::BSGeometry& a_geometry) const;
		/**
		 * @brief Whether an entry with this verdict is evaluated every frame (Tracked::perFrame), and its light path's
		 * traits (Tracked::lightTraits); a_traits gets its traits. a_freshMotion: walk parity's, which takes the reference
		 * roots' motion from scratch (RootMovesNow) instead of from rootMotion.
		 */
		std::pair<bool, std::uint32_t> PerFrameOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, Ineligible a_reason, std::uint32_t& a_traits,
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool>* a_freshMotion = nullptr);
		/** @brief Adds an entry to this walk's `order` once; a_full: written in full even if it only moved. */
		void Schedule(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_full = true);
		/**
		 * @brief A hash of what a record reads from a geometry's shader and alpha properties and their material (flags,
		 * the alpha test and threshold, the material alpha, the material and its diffuse view): the inputs a property
		 * controller can animate.
		 */
		std::uint64_t ShadingInputsOf(const RE::BSGeometry& a_geometry) const;
		/**
		/**
		 * @brief Whether a kept record's skin stays as written: skinning on, its partitions drawn, its palette the
		 * record's size. Notes a partition mask that changed; the palette itself is the scene placement job's. T6b3e: the merge's half,
		 * the record's skin and rows (and the toggle) being the evaluation's (EvalResult::lightSkin): the slot's flags and palette rows.
		 */
		bool KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked, const EvalResult& a_result);
		/** @brief A kept record of a moving static: its transforms, its bound and its sun entry, nothing else. */
		/**
		 * @brief Whether an actor's record, written in an earlier frame, is what WriteObject would write again but for
		 * its placement and palette: its classification inputs (ClassifyInputsOf) and its verdict are unchanged.
		 */
		bool ActorRecordKept(const RE::BSGeometry& a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief A face shape's snapshot and region this walk (FaceSnapshots); false without one. */
		bool ResolveFace(RE::BSGeometry& a_geometry, const Tracked& a_tracked, FaceSnapshots::ShapeView& a_face, std::uint32_t& a_region);
		/** @brief Appends a face shape's entry to this walk's faceStreams and points its slot at it. */
		void PushFaceStream(RE::BSGeometry& a_geometry, const Tracked& a_tracked, std::uint32_t a_slot, const FaceSnapshots::ShapeView& a_face, std::uint32_t a_region);
		/** @brief A kept face shape's record: its stream for this walk (ResolveFace, PushFaceStream); false to write it in full. */
		/**
		 * @brief The heads' publications (FaceSnapshots::BeginWalk): a kept face stream takes its head's new snapshot in place;
		 * a shape waiting for one, or one its head's record no longer holds, is written again; every shape of a stale head is
		 * written again (which rebuilds its record).
		 */
		void ApplyFacePublications();
		/** @brief A tracked face shape joins or leaves its head's list (faceHeads); the head's last one releases its record. */
		void ListFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief Every face shape forgotten (the tracked entries were cleared). */
		void ClearFaceShapes();
		/**
		 * @brief The placements and palettes are FrameValues' (PlacementPlan): the walk lists what it read them from, and the
		 * frame's producer samples them on the pool. A moving reference root whose bound its dependents' sun entries are: an entry
		 * whose only motion is its root's bound (traits kTraitRootMoves alone) is not per frame: the root is listed instead (the
		 * plan's roots), when its reference or its category node had a move event this frame or the last (movingRoots, QueueRoots).
		 */
		struct MovingRoot
		{
			const void* key = nullptr;  // its reference
			const RE::NiNode* categoryNode = nullptr;
		};
		// Every root RootMoves found moving, until ScheduleRoot or its last dependent forgets it.
		ankerl::unordered_dense::map<const RE::NiAVObject*, MovingRoot> movingRoots;
		std::vector<const RE::NiAVObject*> rootPlacements;  // QueueRoots': the roots the plan lists
		void QueueRoots();
		struct PlacementStats
		{
			std::uint64_t roots = 0, stillRoots = 0, rootsGated = 0;
			std::uint64_t fadeRootsUnmirrored = 0;  // T6b1a: fade roots listed with no mirror record (FadeRootFromMirror)
		} placementStats;
		/** @brief The walk's plan item for an entry with a record (its slot and layer row, its resolved sun entry, its palette block). */
		PlacementPlan::Item PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief The walk's plan (PlacementPlan), opened by its first round and made ready by PublishPlacementPlan. */
		PlacementPlan& WalkPlan();
		/** @brief After the walk: the roots it takes into the plan (QueueRoots), and the plan ready for the next frame. */
		void PublishPlacementPlan();
		/**
		 * @brief The movers by event (MoveEvents): the frame each key last had one, drained at the delta walk. A moving root's
		 * bound is listed when its key or its category node had one this frame or the last (QueueRoots), and every one on the
		 * frame after a full evaluation (it drops the node events).
		 */
		ankerl::unordered_dense::map<const void*, std::uint32_t> movedFrame;
		// The keys with a move event this frame and the last (movedKeys[0] this frame's): QueueRoots takes the bound of a root
		// whose reference had one, whatever RootMoves says (the cells' update passes recompute a still root's bound).
		std::array<std::vector<const void*>, 2> movedKeys;
		// T6b3d: movedKeys turn over once a frame (the frame they were drained in, not a pass: a burst's passes add to this frame's), and
		// movedFrame is pruned once 256 frames went by (a frame a pass may not see).
		std::uint32_t movedKeysFrame = 0, movedPruneFrame = 0;
		bool moveGating = false;
		std::uint32_t moveUngatedThrough = 0;
		void DrainMoveEvents(bool a_full);
		// The actor entries by the nodes on their chain (ListHiddenChain), for the hidden events.
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> hiddenDependents;
		bool hiddenGating = false;
		bool hiddenWitness = false;
		void ListHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void DrainHiddenEvents();
		/**
		 * @brief T6b1b: the applied batches' hidden stores (the mirror updates carrying a site: their key, site and the bit's value), taken
		 * by DrainHiddenEvents.
		 */
		struct BatchHidden
		{
			const void* key = nullptr;
			std::uint32_t site = 0;
			bool hidden = false;
			std::uint64_t sequence = 0;  // the update's (SceneCapture::NextSequence after the store)
		};
		std::vector<BatchHidden> batchHidden;
		/**
		 * @brief The nodes FrameGlobals::cullHidden lists whose hidden event was newer than the capture the pass judged it by, and
		 * differed from it: their event's sequence. DrainHiddenEvents announces each again at the first pass whose capture is numbered
		 * after it (FrameGlobals::cullHiddenSequence). A key only, never read.
		 */
		ankerl::unordered_dense::map<const void*, std::uint64_t> hiddenAhead;
		bool MovedRecently(const void* a_key) const
		{
			const auto it = movedFrame.find(a_key);
			return it != movedFrame.end() && sceneFrame - it->second <= 1;
		}
		const void* MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const;
		/**
		 * @brief Whether a reference root's subtree holds anything that moves (a controller, a non-fixed rigid body, a
		 * skin): the root's bound is then not fixed, and it is the sun entry of every geometry under it. Walked once per
		 * root, and again after an event under it (ScheduleRoot).
		 */
		bool RootMoves(const RE::NiAVObject* a_root, const bool* a_now = nullptr);
		bool RootMovesNow(const RE::NiAVObject* a_root) const;
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;
		/** @brief Lists an evaluated entry under its properties; UnlistDependents takes it off every list. */
		void ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root);
		/** @brief Evaluates the entry this walk with its classification taken again. */
		void Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/**
		 * @brief Whether a placement event (a node moved, a sun entry node's bound changed) can change the entry's tables: it
		 * has a record, or a verdict of the frame's (None, Hidden, Switch, Actor). A static negative verdict cannot change
		 * with where the object is, and without a record nothing of it is placed.
		 */
		static bool PlacementMatters(const Tracked& a_tracked);
		/** @brief The dependents of a sun entry node, classified again, and its motion forgotten; a key, never dereferenced. */
		void ScheduleRoot(const RE::NiAVObject* a_root);
		/** @brief A node Havok moved or gave a controller: the entries under it and the dependents of every node above it. */
		void ApplyNodeEvent(RE::NiAVObject* a_node);
		/**
		 * @brief Before the walk's first round: the drained switch events whose selection changed (the index differs from
		 * the oldest event's value, or a child was attached, detached or replaced), for switches in the scene. Each is
		 * brought up to date (CatchUpSwitch) and, in a delta walk, the entries under it are classified again.
		 */
		void ApplySwitchEvents(bool a_full);
		// Scene members' records.
		struct ResidentPatch
		{
			AccumulatedPass pass;
			std::uint32_t pipeline = 0, material = 0;
		};
		static constexpr std::uint32_t kNotResident = ~0u;
		bool IsResidentSlot(std::uint32_t a_slot) const { return a_slot < residentPos.size() && residentPos[a_slot] != kNotResident; }
		void MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch);
		/** @brief Ends a slot's membership: a_restore resets its accumulated half now. */
		void DropResidentSlot(std::uint32_t a_slot, bool a_restore);
		/** @brief A slot's accumulated half back as the scene phase wrote it (its patch lapsed, or its residency ended). */
		void ResetAccumulatedHalf(std::uint32_t a_slot);
		/** @brief The accumulate phase: the residents' pipeline and material slots used this frame, and a template when none was. */
		void KeepResidentsAlive();
		/** @brief A member's tree slot (Tables::objectTree), taken for a tree member and given back otherwise or when it leaves. */
		void ListTree(std::uint32_t a_slot);
		void UnlistTree(std::uint32_t a_slot);
		/** @brief The member's fade root (Tables::fadeRoots): listed when it joins, released with its last member. */
		void ListFadeRoot(std::uint32_t a_slot);
		void UnlistFadeRoot(std::uint32_t a_slot);
		/**
		 * @brief T6b3e: the reference a newly listed tree or fade root's owner is copied from: the slot's entry's fade node (Tracked::
		 * fadeNodeRef, held from pins) when it is a_node; else null, and the root is not listed (counted: fadeRootsUnowned).
		 */
		RE::NiPointer<RE::NiAVObject> HeldFadeNode(std::uint32_t a_slot, const void* a_node);
		/**
		 * @brief T6b3e: a_entry's fade node reference for a_node (Tracked::fadeNodeRef): the batch's pin, else a root owner's copy (the node is
		 * a tracked reference's root), else asked of the render thread (FadeNodeRequest, once a key) and named in the report (a_path: where
		 * it was wanted). True when held now.
		 */
		bool AcquireFadeNode(Tracked& a_entry, const void* a_node, const char* a_path);
		/** @brief A switch event on a tree root's LOD switch: the root's kFadeRootTreeLod again (ApplySwitchEvents). */
		void RefreshFadeRootSwitch(const RE::NiAVObject* a_switch);
		/** @brief A fadeAmount event (Actor::SetAlpha): a listed root's fadeAmount again from its node, its state row kept. */
		void RefreshFadeAmount(const void* a_node);
		/** @brief What a classification reads from the geometry, its properties and its material, hashed. */
		std::uint64_t ClassifyInputsOf(const RE::BSGeometry& a_geometry) const;
		/** @brief CS_DCLF_INPUT_WATCH: what ClassifyInputsOf and ShadingInputsOf hash, by component (kInputComponentNames). */
		std::array<std::uint64_t, 12> InputComponentsOf(const RE::BSGeometry& a_geometry) const;
		/** @brief CS_DCLF_INPUT_WATCH: a re-read of kind a_kind (0 classify, 1 shading) that found the hash changed or not. */
		void NoteInputReread(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, std::uint32_t a_kind, bool a_changed, EvalCounters& a_counters,
			TrackedDelta& a_delta) const;
		void StoreInputComponents(Tracked& a_tracked, const RE::BSGeometry& a_geometry);
		void MoveBucket(Tracked& a_tracked, Ineligible a_bucket);
		/**
		 * @brief Scene membership: binds the bind queue's records from passes built from the objects (PrimaryCull::MembershipPass),
		 * as residents, which the engine's registrations and the lapse leave alone until the record is written again.
		 */
		void BindByMembership();
		/**
		 * @brief T6b3e: one bind queue element's evaluation (BindByMembership's JoinPre, pure over the tables as the joins found them, the
		 * mirror and the entry): skipped, its binding standing, or its membership pass (the derivation cache entry to store with it, why
		 * none, its fade distance and height test). The merge, in queue order, stores the cache entry, leaves the set, drops a failed
		 * member and queues the join.
		 */
		struct JoinPre
		{
			enum class Kind : std::uint8_t
			{
				Skip,    // not a record to bind (free, shadow only, another entry's slot, not this walk's record, not a verdict of None)
				Kept,    // a member whose binding stands (MemberBindingStands)
				Failed,  // no membership pass (PrimaryCull::LastSyntheticFail's reason in fail)
				Pass,    // a membership pass: joined by BuildAccumulatePhase
			};
			Kind kind = Kind::Skip;
			RE::BSGeometry* geometry = nullptr;
			bool layer = false;
			SceneCapture::LeafView leaf;
			MembershipDerived derived;  // the cache entry the pass leaves (Tracked::membershipDerived or membershipLayerDerived)
			AccumulatedPass pass;
			std::uint8_t fail = 0;
		};
		/** @brief T6b3e: bind queue element a_slot evaluated (pure). */
		void EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const;
		/**
		 * @brief T6b3e: one membership join's evaluation (BuildAccumulatePhase's, pure: the mirror, the entry, the tables as the joins
		 * found them, the room map): the held property's check, the derivation cache's witnesses (not its slots: the merge's), the
		 * classification and its descriptors, the pipeline key and static flags of a classified join, the room index. a_classify: classify
		 * even where the cache's witnesses hold (the merge's, when the cache's slots are gone).
		 */
		struct JoinResult
		{
			enum class Kind : std::uint8_t
			{
				Skip,      // the mirror's property is not the held one (a swap not applied yet)
				Rejected,  // a verdict of this frame (the classification or ClassifyFrame) leaves it without bindings
				Joined,    // bound: the merge patches it
			};
			Kind kind = Kind::Skip;
			SceneCapture::LeafView leaf;
			RE::BSShaderProperty* property = nullptr;        // the entry's held property: the template's, a material request's
			const RE::BSShaderMaterial* material = nullptr;  // the record's material (the witness, the material slot's key)
			std::uint8_t fadeState = 0;
			bool alphaBelowOne = false;
			std::uint32_t geometrySlot = 0;
			bool derivedWitness = false;  // the derivation cache's witnesses hold
			bool classified = false;      // ClassifyStatic or ClassifyLayer ran: descriptors, key and static flags are this join's
			Ineligible reason = Ineligible::None;
			LightingDescriptors descriptors;
			PipelineKey key{};
			std::uint32_t staticFlags = 0;
			bool roomSet = false;  // Light Limit Fix's room index taken again (the room map moved on since the entry's)
			int roomIndex = -1;
		};
		void EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const;
		/**
		 * @brief a_count evaluations a_evaluate(index, result) into a_results, by chunks of kFanoutGrain on the pool (a_pool: FanoutMode's 2)
		 * or on this thread; an observer may compare a second, serial evaluation (Diagnostics::JoinsEvaluated, a_what naming the round).
		 * Defined in AccumulatePhase.cpp, its only user.
		 */
		template <class Result, class Evaluate>
		void EvaluateJoins(const char* a_what, std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate);
		std::vector<JoinPre> joinPres;
		std::vector<JoinResult> joinResults;
		/** @brief The coordinator: the technique row of a pass descriptor's TechniqueKey (Tables::techniqueKeys), made and evaluated when new. */
		std::uint32_t TechniqueRowFor(std::uint32_t a_passDescriptor);
		bool shadingNamedAll = false;  // NameShadingEvents named every slot once
		// The shading events (NameShadingEvents): the slots named for the next frame's sample, the LOD fade and emittance events, and the
		// Prepass frames they are reported over (render thread's frames; the names are the walk's).
		struct ShadingStats
		{
			std::uint64_t resampled = 0, lodFadeEvents = 0, emittanceEvents = 0;
		} shadingStats;
		std::vector<const void*> lodFadeChanged;
		// Object LOD (dclf-lod.md): each tracked BSSubIndexTriShape's drawn index ranges (LodSegments::DrawnRanges), taken when it is
		// tracked and again at each of its segment events (lodSegmentEvents); CS_DCLF_PERSISTENT_PARITY compares them with the
		// shape's live state on parity frames.
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::vector<LodSegments::Range>> lodRanges;
		struct LodSegmentStats
		{
			std::uint64_t events = 0, changed = 0;
		} lodSegmentStats;
		void SampleLodRanges(const RE::BSGeometry& a_shape, bool a_event);
		void ApplyLodSegmentEvents(const std::vector<const void*>& a_shapes);
		std::array<std::uint64_t, kChangeCauseCount> reportedChangeCounts{};
		/** @brief After the delta walk's evaluations: the geometry slots of the slots it kept, and the shadow sets. */
		void FinishDeltaWalk(PartTimer& a_timer);
		/**
		 * @brief One round of the walk (T6b3e: the fan-out): every entry of `order` from a_first evaluated (EvaluateEntry: pure, into
		 * evalResults), then merged in index order (MergeEntry: the slots, the columns, the change log, the plan, the dependents). By
		 * CS_DCLF_FANOUT: 0 each entry evaluated and merged before the next (the walk as it was: the reference), 1 every entry
		 * evaluated then merged, on the scene lane, 2 (default) the evaluations on the preparation pool in chunks of kFanoutGrain from
		 * kFanoutMinEntries entries (fewer: as 1), each chunk merged as soon as it is ready while later ones are evaluated (the ordered
		 * streaming merge; under CS_DCLF_FANOUT_PARITY's passes every chunk first). Under the mirror-read parity the round runs as 0 (its
		 * live checks note on the lane).
		 */
		void EvaluateRound(PartTimer& a_timer, std::size_t a_first);

		// T6b3e: the fan-out's grain (one chunk's entries, one EvalCounters each) and the least entries a round fans out for.
		static constexpr std::size_t kFanoutGrain = 64;
		static constexpr std::size_t kFanoutMinEntries = 128;
		/**
		 * @brief T6b3e: an evaluation chunk's counters (a chunk: kFanoutGrain entries from its first; its index begin / kFanoutGrain, so
		 * they do not depend on the thread that ran it). Merged into the store's in chunk order before the round's merges
		 * (MergeCounters): a first string is taken only where the store has none yet, so the first named is the first in `order`, as the
		 * serial walk named it; the rate-limited warnings are logged there.
		 */
		struct EvalCounters
		{
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			std::array<std::uint32_t, 64> techniqueRejects{};
			std::map<const RE::NiRTTI*, std::uint32_t> propertyRejects;
			std::uint32_t rejectedBlended = 0, rejectedOpaque = 0, rejectedOpaqueAlphaTest = 0;
			std::uint32_t classifyHits = 0, classifyChecked = 0, classifyDiffers = 0;
			std::string classifyDiffersFirst;  // the classify cache's warning for its first difference here
			std::uint64_t verdictsChecked = 0, verdictsSkipped = 0, verdictsMissed = 0;
			std::string firstVerdictMissed;
			std::array<std::uint64_t, 12> inputChanged{};
			std::array<std::uint64_t, 2> inputRereads{}, inputRereadsChanged{};
			std::string firstInputChange;
			std::uint64_t leafMissing = 0;  // leafStats.missing
			std::uint64_t reread = 0;       // delta.reread
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};  // the chunk's PartTimer (CS_DCLF_PROFILE)
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			MirrorReadStats mirror;  // the mirror reads counted here (mirrorReadSink)
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;  // RootMovesNow's answers in this chunk
		};
		/** @brief T6b3e: what an evaluation decided for its entry's Tracked, applied by its merge (MergeWrite) as WriteObject wrote it. */
		struct TrackedDelta
		{
			bool faceShapeSet = false, faceShape = false;      // the face shape resolved (once)
			bool actorOwnedSet = false, actorOwned = false;    // owned by an actor, resolved (once)
			bool verdictSet = false;                           // the static verdict's cache
			Tracked::StaticVerdict verdict;
			bool candidateSet = false;                         // classified (or its frame verdict changed)
			std::uint32_t candidateFrame = 0;
			Ineligible candidateReason = Ineligible::None;
			bool classifyInputsSet = false;
			std::uint64_t classifyInputs = 0;
			bool inputComponentsSet = false;                   // CS_DCLF_INPUT_WATCH's components, the last stored
			std::array<std::uint64_t, 12> inputComponents{};
		};
		/**
		 * @brief T6b3e: one entry's evaluation (EvaluateEntry), for its merge. Everything in it was read from the frozen mirror, the entry's
		 * own Tracked, lodRanges, categoryNodes, the walk's stamps and the pass's snapshot (ShardScope): no table, slot, face, root motion
		 * or other entry. Compared field by field by the fan-out's observer (Diagnostics/Scene).
		 */
		struct EvalResult
		{
			enum class Write : std::uint8_t
			{
				None,      // the light path kept it (no write)
				NoLeaf,    // the mirror holds no leaf yet: leafWaiting
				Rejected,  // its verdict gives it no record
				Record,    // a record, its slot-free inputs below; the merge resolves the face, the geometry and the slots
			};
			EvaluateKind kind = EvaluateKind::Event;
			// The light path (the first round): kept, recorded, and KeepSkin's table half left to the merge (the record's rows and
			// partitions); whether ActorRecordKept found the frame verdict changed with no event.
			bool light = false;
			bool lightRecorded = false;
			bool lightSkin = false;
			std::uint32_t skinRows = 0;
			std::uint16_t skinPartitions = 0;
			std::uint32_t skinLodPartitions = 0;
			bool verdictUnannounced = false;  // ActorRecordKept: the frame verdict changed with no hidden event
			// The write (WriteObject's evaluation).
			Write write = Write::None;
			TrackedDelta delta;
			SceneCapture::LeafView leaf;
			Ineligible reason = Ineligible::None;  // its bucket
			bool shadowOnly = false;
			bool faceShape = false;        // a face shape with FaceSnapshots on: its snapshot is the merge's (ResolveFace)
			bool referenceReason = false;  // the dense walk's: reason into referenceReasons
			// The record's inputs with no slot in them.
			std::uint16_t partitionMask = 0;
			bool lodChain = false;
			bool skinCandidate = false;  // skinned with the toggle on: in this walk's skinnedObjects
			std::uint32_t boneRows = 0;  // its palette's rows, 0 without one (or past 240)
			std::uint32_t particleRows = 0;  // T7e: a particle member's block of the frame's palettes (ParticleSnapshots::RowsOf), 0 for anything else
			std::uint8_t shadowReject = 0;
			std::uint32_t shadowTechnique = 0;
			const RE::BSShaderMaterial* shadowMaterial = nullptr;  // the alpha-tested techniques' diffuse (the property record's)
			ID3D11ShaderResourceView* shadowDiffuse = nullptr;
			std::array<std::uint32_t, kOcclusionViews> occlusion{};
			std::uint32_t skinLodWord = 0;  // SkinLodPartitionsOf (0 for a LOD chain)
			// An entry classified in this walk: its per-frame traits without its root's motion (PerFrameFrom's input), whether its verdict
			// may give it a record, its sun entry as the merge's PerFrameOf will find it and that root's motion now, its move key.
			bool classified = false;
			std::uint32_t traits = 0;
			bool mayRecord = false;
			const RE::NiAVObject* rootCandidate = nullptr;
			bool rootMovesNow = false;
			const void* moveKey = nullptr;
			// ShadingInputsOf, taken when the merge may store it (the traits an animated shading or actor entry has).
			bool shadingInputsSet = false;
			std::uint64_t shadingInputs = 0;
		};
		/** @brief T6b3e: entry a_index of `order` evaluated (a_light: the first round, which tries the light path). Pure: reads only. */
		void EvaluateEntry(std::size_t a_index, bool a_light, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief T6b3e: WriteObject's evaluation half (the classification and the record's slot-free inputs), into a_out. */
		void EvaluateWrite(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/**
		 * @brief T6b3e: an entry classified in this walk (its candidateFrame as its merge leaves it is the pass's), its bookkeeping's
		 * evaluation: the per-frame traits without the root's motion, whether its verdict may give it a record, its sun entry and that
		 * root's motion now (the chunk's memo), its move key; and its shading inputs where the merge may store them.
		 */
		void EvaluateClassified(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief T6b3e: PerFrameOf at the merge, from the evaluation's traits and root motion (rootMotion holds the root first: RootMoves). */
		std::pair<bool, std::uint32_t> PerFrameMerge(const Tracked& a_tracked, const EvalResult& a_result);
		/** @brief T6b3e: PerFrameOf's verdict from the traits (its root's motion in them), the verdict and whether it may give a record. */
		static std::pair<bool, std::uint32_t> PerFrameFrom(std::uint32_t a_traits, Ineligible a_reason, bool a_mayRecord);
		/** @brief T6b3e: entry a_index's merge, after the evaluations of its round (a_profileKinds: its EvaluateKind timed). */
		void MergeEntry(std::size_t a_index, bool a_profileKinds, EvalResult& a_result, PartTimer& a_timer);
		/** @brief T6b3e: WriteObject's merge half: a_result's Tracked delta, then the face, the geometry slots and the record. */
		bool MergeWrite(RE::BSGeometry* a_geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& a_timer, Ineligible& a_bucket);
		/** @brief T6b3e: a chunk's counters into the store's (in chunk order), its warnings logged. */
		void MergeCounters(EvalCounters& a_counters);
		/** @brief T6b3e: a chunk's mirror-read counts into the store's (MergeCounters', and e5's prepares'). */
		void MergeMirrorReads(MirrorReadStats& a_counts);
		/**
		 * @brief T6b3e: the mode a round of a_count entries runs in (CS_DCLF_FANOUT, the parities, the pass's thread); a_joins: the joins';
		 * the pool from a_least (e5's subtrees: kSubtreeFanoutMin).
		 */
		std::uint32_t FanoutMode(std::size_t a_count, bool a_joins, std::size_t a_least = kFanoutMinEntries) const;
		/** @brief T6b3e: what a pass's evaluations read in place of the live toggles and Terrain Blending (BuildScenePhase takes it). */
		struct PassSnapshot
		{
			ToggleSet toggles;
			bool terrainDefers = false;
		};
		PassSnapshot passSnapshot;
		std::shared_ptr<const FrameGlobals> passGlobals;  // the frame globals the pass binds (ScenePass), bound on the pool too
		struct ShardScope;
		std::vector<EvalResult> evalResults;
		std::vector<EvalCounters> evalCounters;
		/** @brief The fan-out since the report (Reports::ScenePassEnd). */
		struct FanoutStats
		{
			std::uint64_t rounds = 0, parallelRounds = 0, entries = 0, parallelEntries = 0, chunks = 0;
			std::uint64_t joinRounds = 0, joinParallelRounds = 0, joinEntries = 0;
			std::uint64_t lightMissed = 0;  // light-path entries KeepSkin's table half wrote in full at the merge
			// e5: AddSubtrees' calls, roots and geometries (every mode), those prepared on the pool, and the prepares' and merges' times
			// (modes 1 and 2: mode 0 interleaves them).
			std::uint64_t subtreeRounds = 0, subtreeParallelRounds = 0, subtreeRoots = 0, subtreeParallelRoots = 0, subtreeGeometries = 0;
			double subtreePrepareMs = 0.0, subtreeMergeMs = 0.0;
		} fanoutStats;
		/** @brief A slot's inputs to the frame's shadow sets (the casters' textures and pipelines). */
		struct ShadowInputs
		{
			ID3D11ShaderResourceView* diffuse = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t technique = 0;
			std::uint32_t flags = 0;
			std::uint32_t reject = 0;
			std::array<std::uint32_t, kOcclusionViews> occlusionTechnique{};
			bool operator==(const ShadowInputs&) const = default;
		};
		ShadowInputs ShadowInputsOf(std::uint32_t a_slot) const;
		void RefreshShadowSets(bool a_forceRebuild);
		// Whether a slot's shadow inputs changed this walk: only the changed contributions are updated.
		bool shadowSetsDirty = true;
		bool shadowIndexNeedsRebuild = true;
		std::vector<std::uint32_t> shadowDirtySlots;
		std::vector<ShadowInputs> shadowIndexedInputs;
		std::vector<std::uint8_t> shadowIndexedLive;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::set<std::uint32_t>> shadowTextureMembers;
		ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash> shadowKeyMembers;
		std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash>, kOcclusionViews> occlusionKeyMembers;
		std::uint32_t keptShadowCasters = 0;
		std::array<std::uint32_t, 16> keptShadowRejects{};
		bool fullEvaluation = true;  // the next delta walk evaluates every entry (a reset, a load, a live toggle)
		std::uint32_t walkSerial = 0;
		/**
		 * @brief The entries evaluated every frame, with their entry's address. `tracked` keeps its values in one array,
		 * so an insert or an erase may move them: trackedLayout counts those, and the schedule trusts the addresses only
		 * while it equals perFrameLayout, the count they were taken at, and looks every entry up again otherwise.
		 */
		struct PerFrameItem
		{
			RE::BSGeometry* geometry = nullptr;
			Tracked* tracked = nullptr;
		};
		std::vector<PerFrameItem> perFrameSet;
		std::uint64_t trackedLayout = 0;
		std::uint64_t perFrameLayout = ~0ull;
		std::uint32_t validatedFrame = ~0u;  // T6b3d: the pass (PassStamp) that ran the validation slice: once a pass
		std::vector<RE::BSGeometry*> pendingEvaluation;
		// CS_DCLF_DERIVE_PROBE's LOD fade parity: the frame's inputs, sampled once a frame when first needed.
		LodFadeFrame lodFadeSample;
		bool lodFadeSampled = false;
		// Scene membership: the eligible records the scene phase wrote this frame, bound by the accumulate phase from a pass
		// built from the object (PrimaryCull::MembershipPass) and kept as residents until written again or released.
		std::vector<std::uint32_t> bindQueue;
		std::vector<std::uint8_t> materialMember;  // per material slot: a membership resident was bound to it (diagnostic)
		std::uint32_t membershipWitness = ~0u;  // PrimaryCull::MembershipWitness when the residents were bound
		std::vector<std::uint32_t> refreshedGeometry;  // geometry slots ResolveGeometrySlot re-resolved in place this walk
		// Slot liveness (Tables' SlotTables): the references, counted from the logs. Per object slot, the geometry, pipeline
		// and material it was counted as referencing (slot and generation); per geometry slot, the partition after it in a
		// skin's chain (a chain's slots live while its first does). What makes a geometry slot stale is an event: its buffer
		// references failing the staggered touch, buffers starting to resolve with the slot unresolved, or
		// ResolveGeometrySlot freeing it (freedGeometry); the objects drawing a stale slot are written again (FinishDeltaWalk).
		struct SlotReferences
		{
			static constexpr std::uint32_t kNone = ~0u;
			struct Reference
			{
				std::uint32_t slot = kNone, generation = 0;
				bool operator==(const Reference&) const = default;
			};
			struct Counted
			{
				Reference geometry, pipeline, material;
			};
			LogCursor objects, links;
			std::vector<Counted> object;  // per object slot
			std::vector<Reference> link;  // per geometry slot
		} slotReferences;
		/** @brief The slot tables' reference counts, from the change and geometry logs since the last call. */
		void UpdateSlotReferences();
		/** @brief Drops a geometry slot's contents: the reference it holds on the next partition, its map entry and its buffer leases. */
		void ClearGeometrySlot(std::uint32_t a_slot);
		/** @brief ClearGeometrySlot, then returns the slot to the free list. */
		void FreeGeometrySlot(std::uint32_t a_slot);
		/**
		 * @brief Drops a material slot's contents: its map entry, its dependents entry and its owner, and lists it retired
		 * (its record is dead to the kept stores). The slot table's own state is the caller's (freed, or drained).
		 */
		void ClearMaterialSlot(std::uint32_t a_slot);
		std::vector<std::uint32_t> freedGeometry;
		std::vector<std::uint32_t> staleGeometrySlots;
		bool geometryResolvedLastWalk = false;
		// Set when a slot of any kind was freed this frame; CheckObjectSlots runs only then (or under parity), since a
		// bound object can reference a slot that is not live only after one was.
		bool slotsFreedThisFrame = true;
		// The decals given an ordinal last frame, whose decalOrdinal entries are reset this frame.
		std::vector<std::uint32_t> decalOrdered;
		// SetFadeRootsOwned's set (kFadeRootOwned), by node: whether each is stood in (kFadeRootStoodIn).
		ankerl::unordered_dense::map<const void*, bool> fadeRootOwned;
		/**
		 * @brief The root's row owned or not; a root newly owned is seeded again from its node (a new generation). One whose
		 * stood-in state changes is reported to the fade watch: its dependents' shadow verdicts read its fade from elsewhere.
		 */
		void MarkFadeRootOwned(const void* a_node, bool a_owned, bool a_standIn);
		// The structural events (SceneEvents in Coordinator.cpp), applied at ApplyEvents: properties by key, nodes held.
		std::vector<const void*> propertyChanged;
		std::vector<RE::NiPointer<RE::NiAVObject>> nodeChanged;
		/**
		 * @brief T6b3d, the coordinator, in its pass after the posts are taken: the engine's queues (the tracker's stack, the fade snaps and
		 * amounts, the fades, the property, node and switch events, object LOD's segments) drained, each up to where it stood at the call,
		 * into the pass's batch (null while a load screen is up: into the carry, the references the discarded events hold to Present),
		 * with the render thread's mirror probe; the switch catch-ups the batch asks for posted to the render thread (switchCatchUps).
		 */
		std::shared_ptr<EventBatch> CollectEvents();
		/**
		 * @brief The coordinator: the load markers posted since applied (ApplyLoading), then a_batch (none: an empty one) - the category
		 * refresh, the attached subtrees walked and the detached entries erased, the validation slice, the structural events. What it lets
		 * go of is handed back (HandBack).
		 */
		void ApplyEvents(std::shared_ptr<EventBatch> a_batch);
		/** @brief The coordinator (ApplyEvents' start): what a load screen invalidated dropped (NoteLoadingScreen's marker, T6b3a). */
		void ApplyLoading();
		/** @brief The coordinator: one batch applied (ApplyEvents' body). */
		void ApplyBatch(std::shared_ptr<EventBatch> a_batch);
		/**
		 * @brief T6b3d: the scene pump. passesFull: SetScenePassMode's (render thread -> the passes). The rest is the
		 * coordinator's: the oldest stamp applied and not yet published, what the last publication was made from (PublishKey: a pass that
		 * changed none of it publishes nothing), and the report's samples (Reports::ScenePassEnd).
		 */
		std::atomic<bool> passesFull{ false };
		std::uint64_t unpublishedNs = 0;
		std::uint32_t unpublishedFrame = 0;
		using PublishKey = DCLF::PublishKey;
		PublishKey publishedKey;
		bool publishedKeyValid = false;
		bool publishForced = false;  // an input the coordinator took for the frame (TakeCoordinatorInputs) publishes
		/** @brief What a publication is made from, now (the tables' logs, stamps and sizes, the lookups, the catalog, the candidates). */
		PublishKey CurrentPublishKey() const;
		/** @brief The deltas the next publication carries (the log's node): any pending. */
		bool DeltasPending() const;
		/** @brief The coordinator, at a pass's publication point: whether anything changed since the last publication (else none). */
		bool PublicationNeeded();
		/** @brief The coordinator, after PublishScene: the key it was made from. */
		void NotePublished();
		struct PumpStats
		{
			std::vector<double> passMs;                // a pass's whole time, each pass
			std::vector<std::uint32_t> passEvents;     // the tracker events and queue entries a pass drained
			std::vector<double> publishMs;             // the oldest unpublished event to its publication (ms)
			std::vector<std::uint32_t> publishFrames;  // the same in the render thread's frames
			std::uint64_t passes = 0, eventsOnly = 0, loading = 0, unchanged = 0, published = 0, inlinePasses = 0, failed = 0;
			// The passes by what woke them (SceneWake, a pass counting under each source it took; the last: none - an inline pass, a burst's
			// continuation) and those of them that published nothing; the walks that took the per-frame entries (once a frame) and those
			// that did not.
			std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count) + 1> bySource{}, unchangedBySource{};
			std::uint64_t perFrameWalks = 0, eventWalks = 0;
			double fullMs = 0.0, fullMaxMs = 0.0;  // the whole passes' (not events alone): the scene pass CPU line's
			// The sceneFrames the passes saw since the report (the first, the last), the frames that had one, the most in one frame.
			std::uint32_t firstFrame = 0, lastFrame = 0, framesWithPass = 0, passesThisFrame = 0, maxPassesInFrame = 0;
			bool any = false;
		} pumpStats;
		/** @brief The pump's drain (the lane, or RunScenePassInline): the thread marked, the frame's globals bound, one RunSceneWork. */
		void ScenePass(bool a_inline);
		// The engine references the scene work let go of, its applied batches (their tracker events hold subtrees) and its material
		// references go to EngineReleases: released at Present, pushed where they are let go (RecycleRetired), never handed over.
		// The tree and fade-root nodes the tables list (Tables::treeNode, fadeRootNode), owned while listed: unlisted, they go
		// through the retirement chain (a kept publication still walks them).
		std::vector<RE::NiPointer<RE::NiAVObject>> treeOwners, fadeRootOwners;
		template <class T>
		void HandBack(RE::NiPointer<T>&& a_reference)
		{
			// Through the retirement chain: a published version of the tables may still name it (step 6e E3).
			if (a_reference)
				retirement.Open().references.emplace_back(std::move(a_reference));
		}
		template <class T>
		void HandBack(std::vector<RE::NiPointer<T>>& a_references)
		{
			for (auto& reference : a_references)
				HandBack(std::move(reference));
			a_references.clear();
		}
		// The switch events (SwitchEvents in Coordinator.cpp) drained so far, one per switch node: the index before its
		// oldest event, and whether a child changed. Applied, and cleared, by the next walk.
		struct SwitchPending
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::int32_t before = -1;
			bool structural = false;
		};
		std::vector<SwitchPending> switchPending;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchPendingIndex;
		// The switches the walks applied, for PrimaryCull (TakeSwitchChanges); switchResync when it must read them all.
		std::vector<const RE::NiAVObject*> switchesApplied;
		bool switchResync = true;
		// Resident records: the slots, each one's position in the list, and the patch it was given.
		std::vector<std::uint32_t> residents;
		std::vector<std::uint32_t> residentPos;
		std::vector<ResidentPatch> residentPatches;
		// What the residents keep (KeepResidentsAlive): the used pipeline and material sets, each pipeline's template member, the
		// trees' members and each fade root's centre member. Kept by changes: a join, a patch, a drop or a tree's listing names
		// its slot (NoteResidentTouched), and only those are counted again; made whole again only when every residency ended or
		// the tables were reset (residentMaintenanceDirty).
		bool residentMaintenanceDirty = true;
		void NoteResidentTouched(std::uint32_t a_slot) { residentsTouched.push_back(a_slot); }
		/** @brief Slots by a key (a pipeline, a fade root, the trees), each in one list at a time: swap-removed, the first kept. */
		struct SlotMembers
		{
			std::vector<std::vector<std::uint32_t>> lists;  // by key
			std::vector<std::uint32_t> position;            // by slot: its place in its key's list
			void Add(std::uint32_t a_key, std::uint32_t a_slot)
			{
				if (lists.size() <= a_key)
					lists.resize(std::size_t(a_key) + 1);
				if (position.size() <= a_slot)
					position.resize(std::size_t(a_slot) + 1, ~0u);
				position[a_slot] = static_cast<std::uint32_t>(lists[a_key].size());
				lists[a_key].push_back(a_slot);
			}
			void Remove(std::uint32_t a_key, std::uint32_t a_slot)
			{
				auto& list = lists[a_key];
				const std::uint32_t at = position[a_slot];
				const std::uint32_t moved = list.back();
				list[at] = moved;
				position[moved] = at;
				list.pop_back();
				position[a_slot] = ~0u;
			}
			std::uint32_t First(std::uint32_t a_key) const { return a_key < lists.size() && !lists[a_key].empty() ? lists[a_key].front() : ~0u; }
			std::size_t Count(std::uint32_t a_key) const { return a_key < lists.size() ? lists[a_key].size() : 0; }
			void Clear()
			{
				lists.clear();
				position.clear();
			}
			/** @brief Whether a_slot is listed under a_key (Remove's precondition). */
			bool Has(std::uint32_t a_key, std::uint32_t a_slot) const
			{
				return a_key < lists.size() && a_slot < position.size() && position[a_slot] < lists[a_key].size() && lists[a_key][position[a_slot]] == a_slot;
			}
		};
		// What each resident slot was counted under (kNone: not counted).
		struct ResidentCounted
		{
			static constexpr std::uint32_t kNone = ~0u;
			std::uint32_t pipeline = kNone, material = kNone;
			bool tree = false;
		};
		std::vector<ResidentCounted> residentCounted;  // by slot
		std::vector<std::uint32_t> residentsTouched;   // since the last KeepResidentsAlive
		SlotMembers pipelineMembers, fadeRootMembers, treeMembers;  // the trees' under key 0
		std::vector<std::uint32_t> materialMembers;    // by material slot: how many residents
		// The pipelines and materials the accumulate phase's joins marked used this frame: cleared again where no resident holds them.
		std::vector<std::uint32_t> joinMarkedPipelines, joinMarkedMaterials;
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentJoining;  // this frame's resident passes, until patched
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentLayerJoining;  // the layers' (accumulatedLayerPasses)
		ResidentStats residentStats;
		// The DCLF set (CommitSet): each object slot's committed phases, the bound slots waiting for readiness, and the
		// publication. The commit reads the change log from its own cursor, and the waiting slots again when a lookups
		// generation moved.
		std::vector<std::uint32_t> setWaiting;
		std::vector<std::uint8_t> setWaitingMark;  // parallel to objects: in setWaiting
		std::vector<std::uint32_t> setQueue;
		// Slots that joined the main phase at the last commit and take part in the reflection's: evaluated again at this one, when
		// their last frame's main membership gives them the phase.
		std::vector<std::uint8_t> setQueueMark;    // parallel to objects: in setQueue
		std::vector<std::uint8_t> setLacking;      // parallel to objects: the occluder and reflection phases it takes part in and is no member of
		std::array<std::uint32_t, 3> setLackingCount{};  // SetLacking's: per occlusion map, then the reflection
		std::vector<std::uint8_t> setPartialMark;  // per slot: out of the set with some of its phases ready (SetStats::partial)
		std::uint32_t setPartialCount = 0;
		// The last commit's decision, applied at the next ApplySet: each slot's phases and lacking phases, and the slots it changed
		// with the geometry each held when the commit decided (a slot that holds another one at ApplySet takes nothing).
		std::vector<std::uint8_t> setPhasesNext, setLackingNext;
		std::vector<std::pair<std::uint32_t, const RE::BSGeometry*>> setApply;
		// Parallel to objects: the geometry the last commit that decided a slot decided for (what a whole application after a
		// withdrawal applies its setPhasesNext to); and whether the claims are withdrawn (WithdrawSet) until the next ApplySet.
		std::vector<const RE::BSGeometry*> setGeometryNext;
		using GateFlip = DCLF::GateFlip;
		using PublicationDeltas = DCLF::PublicationDeltas;

		/**
		 * @brief T6b5: the LOD gates (Engine/LodGates.h; dclf-async-publication.md, "T6b5"), the coordinator's. A gate opens with its event
		 * (ApplyGateEvent): its incoming roots gated in the mirror (their own hide not the scene's), the tracked geometries under its roots
		 * tagged (a walk of the mirror per root; later arrivals by AddGeometry). The commit holds a gated incoming slot's phases out of the set
		 * (GateWithhold: stored per slot with its readiness), and flips the gate in the commit that finds every incoming geometry a whole
		 * member or ineligible and every root mirrored (UpdateGates): the held phases applied, the outgoing blocks' dropped and held out until
		 * their detach, the flip carried by the publication (PublicationDeltas::gateFlips) to the frame that installs it (ReleaseGates). The
		 * gate retires with its outcome (ApplyGateOutcomes: flipped, or forced by a safety path on the engine side).
		 */
		static constexpr std::uint8_t kGateIncoming = 1, kGateOutgoing = 2;
		struct LodGate
		{
			enum class State : std::uint8_t
			{
				Open,     // the incoming withheld, the outgoing members
				Flipped,  // the flip published: the incoming applied, the outgoing held out (Tracked::gateHeldOut); waiting for its outcome
			};
			State state = State::Open;
			std::vector<const void*> incoming, outgoing;  // the roots (keys: the blocks' nodes), less those detached since
			ankerl::unordered_dense::set<RE::BSGeometry*> incomingGeometries, outgoingGeometries;  // the tracked ones tagged (keys)
			std::uint32_t pending = 0;     // incoming geometries neither whole members nor ineligible (UpdateGates)
			std::uint32_t unmirrored = 0;  // incoming roots, or nodes under them, the mirror lacks a record of
			std::uint64_t stampNs = 0;     // its event's push (the request), steady clock
			std::uint32_t stampFrame = 0;
			std::uint64_t flipNs = 0;
			std::uint32_t flipFrame = 0;
		};
		struct GateSlot
		{
			const RE::BSGeometry* geometry = nullptr;  // the gated geometry the commit last evaluated the slot for
			std::uint8_t phases = 0;                   // its phases then (the held ones while its gate is open)
			bool ready = false;                        // whole: its phases all it takes part in, not waiting
		};
		struct GateOutcome
		{
			std::uint64_t token = 0;
			bool forced = false;
			std::array<const void*, 2> kept{};  // non-null: a held block of the gate the engine reused (LodGates::Outcome::kept), by its roots
		};
		struct GateStats
		{
			std::uint64_t opened = 0, flipped = 0, retiredFlipped = 0, retiredForced = 0, forcedOpen = 0, unknownOutcomes = 0, superseded = 0;
			std::uint64_t rootsDetached = 0, taggedIncoming = 0, taggedOutgoing = 0, heldOut = 0, emptyIncoming = 0;
			// Kept notices (ApplyGateKept): taken; geometries released from their hold-out (requeued) and outgoing tags dropped before a flip.
			std::uint64_t keptNotices = 0, keptCleared = 0, keptUntagged = 0;
			// Flips deferred for an incoming record the next publication claims first (UpdateGates).
			std::uint64_t structureDeferred = 0;
			std::vector<double> requestToFlipMs;
			std::vector<std::uint32_t> requestToFlipFrames;
		};
		ankerl::unordered_dense::map<std::uint64_t, LodGate> lodGates;     // by token
		struct GateRoot
		{
			std::uint64_t token = 0;
			std::uint8_t side = 0;  // kGateIncoming, kGateOutgoing
		};
		ankerl::unordered_dense::map<const void*, GateRoot> gateOfRoot;     // a root (incoming or outgoing) -> its gate
		std::vector<GateSlot> gateSlots;                                     // by object slot
		std::vector<GateFlip> gateFlipsMade;                                 // since the last publication (TakeDeltas)
		std::vector<GateOutcome> gateOutcomesHeld;                           // taken, not applied yet (TakeGateOutcomes)
		std::uint32_t gateHeldOutCount = 0;                                  // entries with Tracked::gateHeldOut
		// Slots a gate's tag or retirement wants evaluated again, queued by the next commit at its start (a QueueSet after a commit's queue
		// ran would be dropped with it: UpdateGates tags inside the commit).
		std::vector<std::uint32_t> gateQueued;
		GateStats gateStats;
		/**
		 * @brief The coordinator, ApplyMirrorEvents: a Gate event (SceneTracker::EventType::Gate, in order with the batch's other events) opens
		 * its gate: its token, its roots (keys), its push stamp.
		 */
		void ApplyGateEvent(std::uint64_t a_token, std::span<const void* const> a_incoming, std::span<const void* const> a_outgoing, std::uint64_t a_stampNs,
			std::uint32_t a_stampFrame);
		/** @brief The coordinator, ApplyMirrorEvents: a detach in the world; the gates' roots among a_root and a_nodes leave their gates. */
		void NoteGateRootsDetached(const void* a_root, std::span<const void* const> a_nodes);
		/** @brief The tracked geometries the mirror holds under a_root tagged on a_token's a_side; whether the mirror lacked a record there. */
		bool TagGateRoot(std::uint64_t a_token, const void* a_root, std::uint8_t a_side);
		/** @brief AddGeometry: a_geometry tagged when a gate's root is on its mirror chain (up to its category node). */
		void TagGateAbove(RE::BSGeometry* a_geometry, Tracked& a_entry);
		void TagGate(RE::BSGeometry* a_geometry, Tracked& a_entry, std::uint64_t a_token, std::uint8_t a_side);
		/** @brief Off its gate (EraseTracked, a retag, the outcome) and no longer held out. */
		void UntagGate(RE::BSGeometry* a_geometry, Tracked& a_entry);
		/** @brief The rescan (ApplyBatch): the gates' tags dropped with the entries (the rescan's adds tag again). */
		void ResetGateTags();
		/** @brief CommitSet: a_slot's phases as applied (0 for a gated incoming slot whose gate is open, or one held out), recorded per slot. */
		std::uint8_t GateWithhold(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live);
		/** @brief GateWithhold's verdict alone (the set parity's): whether the commit applies 0 to a_slot for a gate. */
		bool GateWithholds(std::uint32_t a_slot) const;
		/** @brief Whether a gated incoming geometry is ready: ineligible for a verdict of the gate's time, or every slot of it whole. */
		bool GateGeometryReady(const RE::BSGeometry* a_geometry, const Tracked& a_entry) const;
		/**
		 * @brief CommitSet, after its queue: the open gates' pending counts taken again, and those ready (every root mirrored, every incoming
		 * geometry ready, a live scene) flipped: the slots and phases to apply into a_apply, the flip onto gateFlipsMade.
		 */
		void UpdateGates(bool a_live, std::vector<std::pair<std::uint32_t, std::uint8_t>>& a_apply);
		/** @brief The coordinator, before CollectEvents: LodGates' outcomes posted so far, held (gateOutcomesHeld). */
		void TakeGateOutcomes();
		/**
		 * @brief The coordinator, after the batch: the held outcomes applied - each gate retired, its tags and gated roots cleared (a forced one's
		 * incoming classified again and committed with their own phases) - unless a_applied is false (a load screen's pass: its events carried).
		 */
		void ApplyGateOutcomes(bool a_applied);
		void RetireGate(std::uint64_t a_token, bool a_forced);
		/**
		 * @brief ApplyGateOutcomes: a_token's held block under a_roots reused by the engine (the node's current block again): the roots off
		 * the gate's outgoing, and the tracked geometries under them that the gate tags as outgoing or holds out untagged, the held-out ones'
		 * slots committed again (gateQueued). Geometries a later gate tags are that gate's.
		 */
		void ApplyGateKept(std::uint64_t a_token, const std::array<const void*, 2>& a_roots);
		/**
		 * @brief The coordinator: ApplySet, RevokeUndrawnClaims, PublishTables, then the publication's deltas onto the log and the publication
		 * to the snapshot builder (IndirectDraws::PostSnapshotWork; in that order: whoever adopts the publication finds its node).
		 */
		void PublishScene();
		/** @brief The coordinator: what its passes made for the frame since the last publication, moved into a_deltas (the log's node). */
		void TakeDeltas(PublicationDeltas& a_deltas);
		/** @brief The coordinator, at the start of its passes: the leases the render thread gave back, dropped. */
		void DropReturnedPublications();
		std::uint64_t publicationSequence = 0;
		std::uint32_t publicationCommitFrame = 0;    // the commit the next publication applies
		std::uint32_t publicationCommitToggles = 0;  // the toggles generation that commit was made under
		std::shared_ptr<const SetSnapshot> publicationClaims;
		std::vector<const RE::BSGeometry*> publicationJoined, publicationLeft;
		std::vector<std::uint32_t> setApplyMark;  // parallel to objects: its index in setApply plus one, 0 when not in it
		std::uint32_t setCommitFrame = 0;
		std::uint32_t setCommitToggles = 0;  // the toggles generation of the frame inputs the last commit was made under
		// The claims as applied (the frame's), kept apart from Tables::setPhases, which a freed slot clears: what RevokeUndrawnClaims
		// checks the records against, with the geometry each base slot is claimed under.
		std::vector<std::uint8_t> setPhasesApplied;
		std::vector<const RE::BSGeometry*> setGeometryApplied;
		LogCursor revokeCursor;
		std::uint64_t revokedGeometries = 0, revokedMain = 0;
		// R3b, structural stamps: the slots whose structure (kStructureCauses) changed since the selected revision's join, read
		// off revokeCursor before ApplySet's own notes and at the join; a claim among them is taken back at the join (its shapes
		// were made without the change), and is the engine's until a revision made after it is selected (the next ApplySet).
		std::vector<std::uint32_t> structureChanged;
		std::uint64_t revokedStructureGeometries = 0;
		/** @brief The structural changes revokeCursor has not read, into structureChanged (its position advanced). */
		void NoteStructureChanges();
		/**
		 * @brief After the frame's scene work: a claimed phase whose record the work stopped drawing (a slot freed, a main member
		 * whose binding went) is taken back from the snapshot the engine's hooks read, so the engine draws it this frame. By the
		 * tables' change log since ApplySet.
		 */
		void RevokeUndrawnClaims();
		bool holdLostMembers = false;  // RunSceneWork, from the joins on: the members it drops are held for PrimaryCull (primaryNotes)
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> materialRequested;  // the scene work's: asked, not answered yet
		/**
		 * @brief What the coordinator let go of that a published version of the tables may still name (step 6e E3): the slots
		 * (Tables::retiring, the SlotTables'), the geometry slots' buffer owners, the material slots' engine references and every
		 * engine reference handed back. Each publication holds the chain's node opened with it (Tables::retirementHold); a batch comes
		 * back once no publication up to it lives, and RecycleRetired puts its slots on their free lists and its references where
		 * they were released before (retiredImports; EngineReleases).
		 */
		struct RetiredBatch
		{
			std::vector<Tables::RetiredSlot> slots;
			std::vector<std::shared_ptr<const void>> imports;
			std::vector<MaterialReference> materials;
			std::vector<RE::NiPointer<RE::NiRefObject>> references;
			std::vector<std::shared_ptr<EventBatch>> events;  // applied event batches (their tracker events hold detached subtrees)
			bool Empty() const { return slots.empty() && imports.empty() && materials.empty() && references.empty() && events.empty(); }
		};
		RetirementChain<RetiredBatch> retirement;
		struct RetirementStats
		{
			std::uint64_t retired = 0, recycled = 0, dropped = 0, batches = 0;
		} retirementStats;
		/** @brief The coordinator: the tables' retiring slots into the chain's open node (before a publication). */
		void RetireIntoChain();
		/** @brief The coordinator: the batches no publication names any more, back on the free lists. */
		void RecycleRetired();
		std::vector<std::uint32_t> bindRetry;                // joins that waited for a material, bound again next phase

		// The scene pass (RunSceneWork): what it holds for the render thread while it runs.
		bool inSceneTask = false;       // RunSceneWork on the lane (not inline)
		bool holdPrimaryNotes = false;  // the scene work's passes: PrimaryCull's notes are held (primaryNotes)
		std::vector<const RE::BSGeometry*> setGeometry;  // parallel to objects: the geometry a base member is published under
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> setMemberSlot;  // published geometry -> its slot
		LogCursor setCursor;
		// The readiness sources a waiting slot may wait on (SetWaitCause bits by index), and their stamps as the waiting slots were last
		// checked against: the lookups' versions, the shadow lookups' generation, the constants' stamp, the scene buffers' fit serial.
		static constexpr std::uint32_t kWaitSources = 4;
		static constexpr std::uint8_t kWaitLookups = 1, kWaitShadow = 2, kWaitConstants = 4, kWaitFit = 8;
		static constexpr std::uint8_t kWaitOther = 16;  // any source moving takes it again (geometry, decal slot, reflection, layer partner)
		/** @brief The readiness source a waiting reason (SetStats::waitingBy's index) is released by. */
		static constexpr std::uint8_t WaitCauseOf(std::uint32_t a_why)
		{
			switch (a_why) {
			case 0:
			case 1:
			case 2:
			case 3:
				return kWaitLookups;
			case 7:
			case 13:
				return kWaitShadow;
			case 9:
				return kWaitConstants;
			case 10:
			case 12:
				return kWaitFit;
			default:
				return kWaitOther;
			}
		}
		std::array<std::uint64_t, kWaitSources> setReadiness{ ~0ull, ~0ull, ~0ull, ~0ull };
		// The forward views' pipelines ready in the lookups' catalog (the reflection's and T7e's first person's readiness, reason 8): no
		// readiness source above moves when the lane publishes one, so a change of their count takes the kWaitOther slots again.
		std::uint64_t setForwardReadiness = ~0ull;
		std::vector<std::uint8_t> setWaitCause;  // by slot: what it waits on (its last evaluation's), when waiting
		std::vector<std::uint8_t> setWaitWhy;    // by slot: the first reason (SetStats::waitingBy's index) of its last wait (T6b0)
		std::uint64_t setShadowModes = ~0ull;      // the shadow modes and states caster readiness was last taken under
		std::uint32_t setPhaseMask = ~0u;          // the phases DCLF draws (toggles) the last commit evaluated with
		std::shared_ptr<SetSnapshot> setBuilding;  // the next publication, kept up to date by each commit
		std::shared_ptr<const SetSnapshot> setSnapshot;
		bool setSnapshotDirty = true;
		SetStats setStats;
		/** @brief Queues a slot for the next commit. */
		void QueueSet(std::uint32_t a_slot);
		/**
		 * @brief A slot taken out of phases of the applied set after its commit (the joins binding it again before this pass's commit,
		 * a claim revoked after it): out of the commit's decision and the snapshot the claims are made from too, queued, so the next
		 * commit decides it again from what it has now, and marked for ApplySet (MarkSetApply), so a slot that commit leaves out still
		 * reaches the claims' notes. Without this the commit's copy keeps the phases: its next decision of the same phases is no change,
		 * the record stays out of them and the claims keep them, and nobody draws it. A base and its layer leave the main phases
		 * together, the applied tables too (CommitSet's rule: the claim is the base geometry's).
		 */
		void LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner = true);
		/**
		 * @brief A slot whose committed phases or lacking phases changed, for the next ApplySet, with the geometry it holds now (an entry
		 * not applied yet takes this one's geometry: the decision is now for it).
		 */
		void MarkSetApply(std::uint32_t a_slot);
		/** @brief The claims' snapshot of setBuilding, made again when a commit or LeaveSet changed it. */
		void RefreshSetSnapshot();
		/**
		 * @brief The phases DCLF can draw (the toggles, and what it has set up: IndirectDraws::ShadowCapability, ReflectionDrawable): an
		 * object's mask is its participation within them. A capability, not what a frame drew: it changes only on a toggle, a load,
		 * a failure or a setup completing, so which views the engine asks for never changes the set.
		 */
		static std::uint32_t SetCapability();
		/** @brief The phases an object takes part in among a_drawn, from its record (no readiness). */
		std::uint8_t SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const;
		/** @brief One slot's decision (DecideSlot): its phases, whether it waits and why, and what the observers note of a first-person slot. */
		struct SlotDecision
		{
			std::uint8_t phases = 0;  // every phase it takes part in when ready for all of them, else 0
			bool wait = false;        // it takes part in a phase it is not ready for
			bool partial = false;     // some of its phases were ready
			std::array<std::uint8_t, 8> whys{};  // the waiting reasons in order (SetStats::waitingBy's index)
			std::uint8_t whyCount = 0;
			bool firstPersonExcluded = false;    // a first-person entry's slot the first-person view takes no part in (observed runs)
			bool firstPersonHeld = false;        // a first-person slot held out of its view
			std::uint8_t heldWhyCount = 0;       // the reasons noted before it was held out
			std::uint32_t firstPersonSlotWhy = 0;  // IndirectDraws::FirstPersonSlotReady's cause
		};
		/**
		 * @brief What the commit decides for a_slot now, from its record and the readiness sources (a_live: the scene built; a_drawn: the
		 * phases DCLF draws): pure, the commit applies its side effects (CommitSet), and its observers decide every slot again with it.
		 */
		SlotDecision DecideSlot(std::uint32_t a_slot, bool a_live, std::uint32_t a_drawn) const;
		/** @brief Whether everything the object's main phase draws with is ready; a_why: SetStats::waitingBy's index when not. */
		bool MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const;
		/** @brief T7e: whether a slot's geometry is a first-person entry's (FirstPersonEntry; the coordinator's tracked map). */
		bool FirstPersonSlot(std::uint32_t a_slot) const;
		// Sun entry nodes something was attached under or detached from since the last walk (keys).
		std::vector<const RE::NiAVObject*> dirtyRoots;
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> propertyDependents;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> rootDependents;
		/**
		 * @brief A reference to every root listed in rootDependents or lightDependents (step 6e F1), taken when it is first listed (a
		 * live ancestor of the geometry listing it) and handed back when neither lists it any more. What the scene work hands out
		 * of a root - the placement plan's roots and entries, the candidates' snapshots - is a copy of it: no reference is ever
		 * made from a key, which may name a node already gone.
		 */
		ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> rootOwners;
		/** @brief Owns a_root while a dependents list names it: from the batch's pin (a_live: a live walk's pointer, the category walk's). */
		bool OwnRoot(const RE::NiAVObject* a_root, bool a_live);
		/** @brief Hands the root's reference back once neither dependents list names it. */
		void ReleaseRootOwner(const RE::NiAVObject* a_root);
		void ReleaseRootOwners();
		/** @brief The listed root's reference (a copy); null, and counted, for a root no list names. */
		RE::NiPointer<RE::NiAVObject> OwnedRoot(const RE::NiAVObject* a_root);
		std::uint64_t unownedRoots = 0;  // since the last report: roots handed out that no list names (a defect)
		// A listed reference root's reference, and back (the root is a key once its last dependent leaves: it may be gone).
		ankerl::unordered_dense::map<const RE::NiAVObject*, const void*> rootReference;
		ankerl::unordered_dense::map<const void*, const RE::NiAVObject*> referenceRoot;
		std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> buckets{};
		struct DeltaStats
		{
			std::uint32_t walks = 0, full = 0;
			std::uint64_t evaluated = 0, perFrame = 0, pending = 0, property = 0, node = 0, roots = 0, geometryDirty = 0, restored = 0, moved = 0, kept = 0;
			std::uint64_t propertyEvents = 0, nodeEvents = 0, reread = 0;
			std::uint64_t facePublished = 0, faceUpdated = 0, faceWritten = 0;
			std::uint64_t switchEvents = 0, switchChanges = 0, switchCatchUps = 0, switchReclassified = 0, attachCatchUps = 0;
			std::uint64_t live = 0;
			std::uint32_t evaluatedMax = 0;
		} delta;
		/**
		 * @brief A face shape's region of the positions buffer, kept while a stream of the shape holds it (the buffer grows to
		 * hold every region: IndirectDraws' ReserveSceneTables). EndFaceWalk frees the regions of the streams freed since
		 * (Tables::faceStreamsReleased). The walk's thread alone.
		 */
		std::uint32_t FaceRegionOf(const RE::BSGeometry* a_geometry, std::uint32_t a_vertexCount);
		void EndFaceWalk();
		void ClearFaceRegions();
		struct FaceRegion
		{
			std::uint32_t first = 0, count = 0;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, FaceRegion> faceRegions;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> faceRegionFree;  // (first, count), sorted, coalesced
		std::uint32_t faceRegionTop = 0;
		// The tracked face shapes by head (membership: ListFaceShape, UnlistFaceShape), and the heads' publications this walk.
		ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::vector<RE::BSGeometry*>> faceHeads;
		std::vector<const RE::BSFaceGenNiNode*> facePublished, faceStale;
		std::vector<RE::BSGeometry*> skinnedObjects;  // this walk's skinned objects, in object order
		std::uint32_t AllocateGeometrySlot();
		std::uint32_t AllocatePipelineSlot();
		std::uint32_t AllocateMaterialSlot();
		/**
		 * @brief The geometry slot for a TriShape: found, refreshed in place, or newly resolved.
		 * @return the slot, or Tables::kSlotFree when the buffers cannot be made stable for the graph.
		 */
		/** @brief What a geometry slot is resolved from: its key in geometryIndex, its buffers and its counts. */
		struct GeometrySource
		{
			const RE::BSGraphics::TriShape* key = nullptr;
			ID3D11Buffer* vertexBuffer = nullptr;
			ID3D11Buffer* indexBuffer = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t vertexCount = 0;
			std::uint32_t indexCount = 0;
			std::uint32_t firstIndex = 0;  // a range of the index list (object LOD's visible ranges); 0 for the whole
			bool layer = false;            // a key that is no TriShape (Tables::geometryLayerKey): a layer's second index list, T7e's particle quads
		};
		/** @brief The buffers order[a_first...]'s geometry slots will resolve, prefetched in one batch (GpuResources::Prefetch). */
		void PrefetchGeometryBuffers(std::size_t a_first);
		std::uint32_t ResolveGeometrySource(const GeometrySource& a_source, PartTimer& a_timer);
		/** @brief The geometry slot of a multi-index shape's second index list (its layer's draw), or Tables::kSlotFree. */
		std::uint32_t ResolveLayerGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, PartTimer& a_timer);
		/** @brief The geometry slot of a geometry's TriShape, or of its skin partition a_partition (T6b1b: from its record). */
		std::uint32_t ResolveGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, const SceneCapture::GeometryRecord::Partition* a_partition, PartTimer& a_timer);
		/**
		 * @brief Object LOD (dclf-lod.md): the geometry slots of a partly hidden shape's visible ranges, linked by nextPartition from
		 * the returned first (Tables::kSlotFree when one cannot be resolved), each keyed by its range's first segment record.
		 */
		std::uint32_t ResolveLodRangeSlots(const SceneCapture::GeometryRecord& a_geometry, const std::vector<LodSegments::Range>& a_ranges, PartTimer& a_timer);

		/** @brief Consume reference changes; retire unused materials and expire idle geometry/pipeline slots before capture. */
		void SweepSlots();
		/**
		 * @brief Drops the slot tables, their maps and every cached derivation (by generation): the
		 * teardown paths, where the tables are cleared outright, must not leave a map or a Derived
		 * pointing at slots that no longer exist.
		 */
		void ResetSlotTables();

	public:

	private:
		/**
		 * @brief The coordinator: drops every cached classification verdict and derivation: a live toggle that enters the
		 * classification (Toggles.h) changed, and the caches witness the object rather than the switches.
		 */
		void InvalidateVerdicts();

		/** @brief After the loop: every object with bindings names live slots of this frame, or is neutralised. */
		void CheckObjectSlots(bool a_resolveBuffers);
		Stats stats;
	};
}
