#pragma once

// The scene coordinator: the scene-work half of DCLF's persistent scene. It owns the tracked set, the scene mirror, the slot tables
// (SceneTables) and their change log, the membership joins, the DCLF set and the publications the render thread installs. Scene lane only:
// passes run one at a time, woken by producers (SceneWake), fanning pure reads out to the preparation pool; under the render-thread
// parities a pass runs inline. The render thread reaches it only through Scene::Channels and the publications.

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
	}
	struct FrameGlobals;
	/** @brief Attributes the time since the last call to one BuildPart (CS_DCLF_PROFILE). */
	struct PartTimer;
	struct SunCandidates;
	struct PipelineCatalog;

	/** @brief The parts of Coordinator::BuildFrame, timed separately under CS_DCLF_PROFILE. */
	enum class BuildPart : std::uint32_t
	{
		Walk,            // the setup before the loop (the walk's prologue, the joins' BindByMembership)
		PassLookup,      // the loop's order built (the joins' passes looked up)
		ClassifyStatic,  // type, skin, property, material alpha, DeriveLightingDescriptors
		ClassifyFrame,   // the parent chain (hidden, actor) and the fade
		Resolve,         // GpuResources::Resolve for the vertex and index buffers
		Dedup,           // the geometry, pipeline and material map probes
		PipelineEval,    // the new-pipeline body: its technique row and constants
		MaterialEval,    // the new-material body: the material port
		Record,          // the record's writes, and the joins' patch bookkeeping
		CapturePatch,    // the joins' room index and tree wind
		ApplyPatch,      // the joins' patch into the tables (ApplyAccumulatePatch)
		DedupHit,   // the three map probes on the HIT path (Dedup measures the misses)
		LoopTail,   // per tracked object: a rejected one's continue path, and the iteration itself
		Skinning,   // skinned objects: their palette rows counted
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(BuildPart::Count)> kBuildPartNames{
		"walk", "pass-lookup", "classify-static", "classify-frame",
		"resolve", "dedup", "pipeline-eval", "material-eval", "record", "capture-patch", "apply-patch",
		"dedup-hit", "loop-tail", "skinning"
	};

	/** @brief The sub-zones of a scene pass, each a Tracy zone and a Stats::scenePartMs sum (CS_DCLF_STATS). */
	enum class ScenePart : std::uint32_t
	{
		CategoryNodes,     // the attach queue's drain and RefreshCategoryNodes
		AttachDetach,      // AddSubtree per attach, EraseTracked per detach
		Validate,          // ValidateSlice
		StructuralEvents,  // the fade, property, node and switch queues' drains
		Prologue,          // used sets, change log trim, LOD fade settings, GpuResources::BeginFrame
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
		Joins,             // BuildAccumulatePhase, new pipelines' blocks, bindings, lookups
		Commit,            // CommitSet, after the joins
		Publish,           // PublishScene: set applied, claims revoked, tables and lookups published
		// Nested in the parts above (ScenePartNested): the two halves of each fan-out.
		EvaluateFanout,    // EvaluateEntry over the round (the pool from 128 entries)
		EvaluateMerge,     // MergeEntry over the round, in order
		JoinsFanout,       // EvaluateJoinPre and EvaluateJoin
		JoinsMerge,        // their merges, in order
		SubtreesFanout,    // PrepareSubtrees (the pool from 16 roots)
		SubtreesMerge,     // MergeSubtree, in event order
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(ScenePart::Count)> kScenePartNames{
		"category nodes", "attach/detach", "validate", "structural events", "prologue", "sweep slots", "cull hidden bits",
		"schedule", "switch events", "evaluate", "geometry scan", "later rounds", "shadow sets",
		"object sweep", "sun candidates", "face walk", "walk parity", "shading", "placement plan", "joins", "commit", "publish",
		"evaluate (fan-out)", "evaluate merge", "joins (fan-out)", "joins merge", "subtrees (fan-out)", "subtrees merge"
	};

	/** @brief A part measured inside another (the fan-outs' halves), left out of the parts' sum. */
	inline constexpr bool ScenePartNested(std::size_t a_part)
	{
		return a_part >= static_cast<std::size_t>(ScenePart::EvaluateFanout);
	}

	/** @brief What the first evaluation round did with an entry, for its timing (CS_DCLF_PROFILE). */
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

	namespace Scene
	{
		class Coordinator;
	}

	/**
	 * @brief The scene work: the tracked geometries' records, kept current from engine events and published for the frame.
	 *
	 * A pass (RunSceneWork) applies the engine's queued events (CollectEvents, ApplyEvents), rewrites the entries whose inputs changed
	 * (Phase::Scene), joins records to their membership passes (Phase::Accumulate), commits the set (CommitSet) and publishes on change
	 * (PublishScene); with DCLF off it applies events only. Engine references it lets go of are released at Present through the retirement
	 * chain, never on its threads. Tracked content: everything under attached cells' Static, Dynamic, MultiBound and Actor category nodes
	 * (terrain, markers and water are not).
	 */
	class Scene::Coordinator
	{
		// Diagnostics/Scene and the scene reports read the private state through it.
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
			// Decal candidates this frame, by group (ObjectDecalGroup - 1).
			std::array<std::uint32_t, 3> decals{};
			// Objects bound with the screen-door fade (AdditionalAlphaMask), summed until TakeFadingDrawn (fades are brief).
			std::uint32_t fadingDrawn = 0;
			std::uint32_t fadingFrames = 0;  // frames in that sum with at least one
			std::uint32_t skinned = 0;  // skinned candidates this frame, and their palette rows
			std::uint32_t boneRows = 0;
			std::uint32_t projectedUV = 0;  // candidates with the ProjectedUV bit, and terrain ones
			std::uint32_t landBlend = 0;
			// Joins served from Tracked::Derived; under Policy::RecomputeCachedVerdicts, recomputed and disagreeing (the gate: 0).
			std::uint32_t derivedHits = 0;
			std::uint32_t derivedChecked = 0;
			std::uint32_t derivedDiffers = 0;
			std::uint32_t slotsSwept = 0;
			std::uint32_t geometriesRefreshed = 0;
			std::uint32_t slotViolations = 0;  // objects whose slots failed CheckObjectSlots (the gate: 0)
			std::uint32_t shadowCasters = 0;  // records the engine would draw into a shadow map
			std::array<std::uint32_t, 16> shadowRejects{};  // by ShadowReject, over the frame's records
			// Live slots by kind.
			std::uint32_t geometriesAlive = 0, pipelinesAlive = 0, materialsAlive = 0;
			// Objects left native by Ineligible::Technique, by technique id (63 = refraction).
			std::array<std::uint32_t, 64> techniqueRejects{};
			// Served from the cached static verdict (Tracked::StaticVerdict); recomputed and disagreeing as for derived* above.
			std::uint32_t classifyHits = 0;
			std::uint32_t classifyChecked = 0;
			std::uint32_t classifyDiffers = 0;
			// BuildFrame time by BuildPart since ResetTimes (ms). CS_DCLF_PROFILE only; off, the loop makes no clock calls.
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};
			// The joins' share of partMs (BuildAccumulatePhase).
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> accumulatePartMs{};
			// Scene pass time by ScenePart since ResetTimes (ms).
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMs{};
			// This pass's time by part, and the largest pass's since ResetTimes (EndSceneFrame).
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartFrameMs{};
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMaxMs{};
			// The light path since ResetTimes: entries kept by lightTraits bit, movers listed, kept skins.
			std::array<std::uint64_t, 8> lightByTrait{};
			std::uint64_t lightPlaced = 0, lightSkins = 0;
			std::uint64_t moveEvents = 0;
			// Walks that had to look the per-frame set up again (an entry was added or erased).
			std::uint64_t perFrameRelookups = 0;
			// Hidden events drained; actor verdicts retaken and skipped; on parity frames, verdicts changed with no event (first named).
			std::uint64_t hiddenEvents = 0, verdictsChecked = 0, verdictsSkipped = 0, verdictsMissed = 0;
			std::uint64_t hiddenRetaken = 0;  // statics classified again for a hidden event on their chain
			std::uint64_t hiddenAheadAnnounced = 0;  // hidden events announced again at a capture newer than them (hiddenAhead)
			std::string firstVerdictMissed;
			// The first round by EvaluateKind: time (ms) and entries (CS_DCLF_PROFILE).
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			std::array<std::uint64_t, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindCount{};
			// Material evaluations this frame.
			std::uint32_t materialsEvaluated = 0;
			std::uint32_t materialCacheEntries = 0;
			std::uint32_t materialCacheEvicted = 0;
			std::uint32_t materialEvictedMember = 0;  // of them, slots a membership resident had been bound to (materialMember)
			// The last RefreshMaterialRecords: captures, slots rewritten, materials held, signature samples.
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
			stats.evaluateKindMs = {};
			stats.evaluateKindCount = {};
		}

		/** @brief The end of a full scene pass: folds its part times into the maximums. */
		void EndSceneFrame()
		{
			for (std::size_t i = 0; i < stats.scenePartFrameMs.size(); ++i)
				stats.scenePartMaxMs[i] = std::max(stats.scenePartMaxMs[i], stats.scenePartFrameMs[i]);
			stats.scenePartFrameMs = {};
		}


		static Coordinator& Get();

		/** @brief Render thread, once DCLF is installed: allows the pump's wakes. From then producers wake the passes, never the frame. */
		void StartScenePump();
		/**
		 * @brief Render thread, each frame start and menu toggle. a_full: DCLF runs (else passes apply events only). a_inline: a parity
		 * observes from the render thread, so the frame start runs the pass (RunScenePassInline).
		 */
		void SetScenePassMode(bool a_full, bool a_inline);
		/**
		 * @brief Render thread, frame start under the parities: one pass run inline once any lane pass ends, then its held PrimaryCull
		 * notes delivered.
		 */
		void RunScenePassInline();
		/**
		 * @brief Render thread at Present under the parities, when no frame start ran since the last (menus, load screens): an events-only
		 * pass.
		 */
		void ApplyEventsInline();
		/** @brief Whether a scene pass runs now: frame-side fallbacks that read what a pass writes refuse then. */
		bool SceneTaskInFlight() const;

		// Scene members (drawcall-limit-fix.md): a member's accumulated half is patched once from its membership pass and kept across
		// frames until a rewrite breaks its binding, a release or a failed patch.
		/** @brief Ends every membership (the frame globals a membership pass reads changed). */
		void EndAllResidency();
		struct ResidentStats
		{
			std::uint64_t joined = 0, failed = 0, rewritten = 0, released = 0, frames = 0, resident = 0;
			std::uint64_t membershipQueued = 0;  // records BindByMembership handed a pass
			std::uint64_t materialWaits = 0, materialsServed = 0;  // joins that waited for a material capture, captures the render thread served
			std::uint64_t materialsStale = 0;  // requests whose property had another material at the serve (the swap's event not applied yet)
			std::uint64_t membershipKept = 0;    // members written again whose binding stands
			std::uint64_t layerUnpaired = 0;     // a base or a layer that joined without the other, and left again
			std::array<std::uint64_t, 4> failedBy{};  // (unused), no record, a frame verdict, material or extras
		};
		ResidentStats TakeResidentStats() { return std::exchange(residentStats, {}); }
		/**
		 * @brief The joins' frame inputs since the last call: room maps and pipeline frames posted and taken, PerGeometry blocks made or
		 * waited.
		 */
		struct AccumulateInputStats
		{
			std::uint64_t roomMapsPosted = 0, roomMapsTaken = 0, pipelineFramesPosted = 0, pipelineFramesTaken = 0;
			std::uint64_t pipelineBlocksMade = 0, pipelineBlocksWaited = 0;
		};
		AccumulateInputStats TakeAccumulateInputStats();


		/**
		 * @brief Which half of the tables to build: Scene (the walk) writes what does not depend on the main pass; Accumulate (the joins)
		 * patches in what the main pass draws with, from membership passes. Object indices are fixed from Scene on.
		 */
		enum class Phase : std::uint32_t
		{
			Scene,
			Accumulate
		};

		/** @brief Builds one half of the tables from the tracked set. */
		void BuildFrame(Phase a_phase);

		using RevisionRequest = DCLF::RevisionRequest;
		using ScenePublication = DCLF::ScenePublication;
		/**
		 * @brief Pipeline blocks the frame posted that the passes applied or found stale, and the technique rows' work, since the last
		 * call.
		 */
		struct ConstantsPostStats
		{
			std::uint64_t pipelinesApplied = 0, stale = 0;
			// Technique rows: evaluated (new, or inputs moved), written, input moves.
			std::uint64_t techniquesEvaluated = 0, techniquesWritten = 0, techniqueInputsMoved = 0;
		};
		ConstantsPostStats TakeConstantsPostStats() { return std::exchange(constantsPostStats, ConstantsPostStats{}); }
		struct TablesPublication
		{
			std::uint64_t published = 0, reused = 0, made = 0;
			double ms = 0.0, maxMs = 0.0;
			// The replay: material records rewritten (version moved) against slots, and the rest's time (copied whole).
			std::uint64_t materialsReplayed = 0, materialSlots = 0;
			double restMs = 0.0;
			// Object slots replayed from the change log against slots; whole copies (a new snapshot, or one the log no longer reaches).
			std::uint64_t objectsReplayed = 0, objectSlots = 0, wholeCopies = 0;
			std::uint64_t geometriesReplayed = 0, geometrySlots = 0, geometryWholeCopies = 0;
			std::uint64_t treesKept = 0, fadeRootsKept = 0;  // publications that kept the family (its stamp stood)
			std::size_t pool = 0;
			// Lookups per publication (PublishLookups): copied, or the last shared again unchanged; chunks rewritten since the last copy;
			// copy time.
			std::uint64_t lookupsCopied = 0, lookupsShared = 0, lookupsChunks = 0;
			double lookupsMs = 0.0, lookupsMaxMs = 0.0;
		};
		/** @brief The retirement chain's counts since the last call, and what waits in it now. */
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
		 * @brief The frame: the render thread's, or on a scene work thread sceneFrame (from the last frame inputs taken; lags the render
		 * thread).
		 */
		std::uint32_t GetFrame() const;
		/**
		 * @brief The pass as a 32-bit stamp for the per-entry "this pass" marks (Tracked::candidateFrame, Tables::geometryLastUsed):
		 * never 0 ("never") nor Tables::kSlotFree, unique over 2^32 - 2 passes.
		 */
		std::uint32_t PassStamp() const { return static_cast<std::uint32_t>(passSerial % 0xFFFFFFFEull) + 1u; }
		/** @brief The candidates as the coordinator last made them (the frame takes the publication's copy). */
		std::shared_ptr<const SunCandidates> CoordinatorSunCandidates() const { return sunCandidates; }
		std::shared_ptr<const SunCandidates> CoordinatorLightCandidates() const { return lightCandidates; }


		/**
		 * @brief Which skin partitions the engine draws for this LOD row (bit i = partition i): BSDismemberSkinInstance::Unk_25 (AE
		 * 140d31f40) skips a partition whose flag (Data byte 0) is clear, then NiSkinPartition::Unk_25 applies the table. 0 when it draws
		 * none.
		 */
		static std::uint32_t SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow);
		/** @brief SkinPartitionMask from the geometry's record: its partitions' LOD bytes and a dismember skin's shown flags. */
		static std::uint32_t SkinPartitionMask(const SceneCapture::GeometryRecord& a_geometry, std::uint32_t a_lodRow);
		/** @brief Tables::skinPartitions: 0 for none or a single drawn partition, kNoPartitions when row 3 draws none, else the mask. */
		static std::uint16_t SkinPartitionsOf(const SceneCapture::GeometryRecord& a_geometry);
		/** @brief Tables::skinLodPartitions for a geometry: a kMeshLOD skin's masks per level (byte L: row L), 0 for any other. */
		static std::uint32_t SkinLodPartitionsOf(const SceneCapture::LeafView& a_leaf);

		/** @brief The geometry's object slot in this walk's tables, or -1 when DCLF does not draw it. */
		std::int32_t FindObject(const RE::BSGeometry* a_geometry) const;
		/** @brief The geometry's layer object (Tracked::layerSlot) in this walk's tables, or -1. */
		std::int32_t FindLayerObject(const RE::BSGeometry* a_geometry) const;
		/** @brief The layer's membership pass this frame (BindByMembership), or null. */
		const AccumulatedPass* FindAccumulatedLayerPass(const RE::BSGeometry* a_geometry) const;
		/** @brief Diagnostics: the shader property a slot draws with (its layer property for a layer's). */
		RE::BSShaderProperty* SlotPropertyOf(std::uint32_t a_slot) const { return SlotProperty(a_slot); }
		/**
		 * @brief Decides the DCLF set (SceneSet.h) after the joins: one decision (kObjectMember, the claims snapshot) for every phase of a
		 * frame. An object joins once everything its phases draw with is ready, judged by events, never a scan; a member being rebound
		 * leaves first. Records and Tables::setPhases change only at ApplySet (dclf-async-publication.md).
		 */
		void CommitSet();
		/**
		 * @brief PublishScene: applies the commits since the last publication by slot (setApply) into the records and lacking counts; a
		 * slot freed or reassigned since takes no phase. Claims are installed with the revision from the same publication.
		 */
		void ApplySet();

		/**
		 * @brief One scene pass (steps in the class comment); a_task: on the lane. Publishes only when something changed
		 * (PublicationNeeded).
		 */
		void RunSceneWork(bool a_task);
		/** @brief Claims revoked mid-frame since the last call (RevokeUndrawnClaims): geometries, and main phases taken back. */
		std::pair<std::uint64_t, std::uint64_t> TakeRevokedClaims() { return { std::exchange(revokedGeometries, 0), std::exchange(revokedMain, 0) }; }
		/** @brief Of them, those revoked for a structural change after the selected revision's join. */
		std::uint64_t TakeStructureRevocations() { return std::exchange(revokedStructureGeometries, 0); }
		struct SetStats
		{
			std::uint64_t commits = 0, evaluated = 0, joined = 0, left = 0, readinessEvents = 0, resyncs = 0;
			std::uint64_t members = 0, waiting = 0, rebinding = 0;  // summed over the commits
			// Summed over commits: objects out of the set although some phases are ready (a claim is the whole object).
			std::uint64_t partial = 0;
			std::uint64_t publications = 0;
			std::uint64_t patchedMember = 0;  // must be 0: the joins patched a member's binding (CommitSet keeps rebinds out)
			std::uint64_t leftAfterCommit = 0;  // slots taken out of the commit's phases after it (LeaveSet)
			// Why a bound object waits, summed over commits: pipeline, material, pipeline's shadow mask, shared lookups (samplers, null and
			// projected textures), geometry, decal slot, layer partner, shadow or occlusion pipelines (or an alpha-tested caster's
			// diffuse); 8: the reflection's forward pipeline.
			std::array<std::uint64_t, 14> waitingBy{};
			// Why a member left (its commit's verdict): scene not built (a load), no phase (freed, ineligible, phases off), not bound
			// (membership dropped), waiting (by waitingBy index: leftWaiting), layer partner or a whole-object phase not ready.
			std::array<std::uint64_t, 5> leftBy{};
			std::array<std::uint64_t, 14> leftWaiting{};
			std::uint64_t waitingRequeued = 0;  // waiting slots taken again for a readiness source they wait on
			std::string firstWaiting;
		};
		SetStats TakeSetStats() { return std::exchange(setStats, {}); }
		/** @brief TakeCoordinatorInputs: PrimaryCull's fade root ownership, a whole set (MarkFadeRootOwned each). */
		void ApplyFadeRootsOwned(const std::vector<OwnedFadeRoot>& a_owned);
		/**
		 * @brief Diagnostics: whether the fade node's state is the GPU's alone. Every listed fade root is owned and DCLF's to write
		 * (kFadeRootWritten); no engine OnVisible steps it, so readers take the GPU's or the settled state.
		 */
		bool FadeOnGpu(const RE::NiAVObject* a_fadeNode) const { return a_fadeNode && tables.fadeRootIndex.contains(a_fadeNode); }
		/** @brief A report: why a tracked geometry's sun entry rule allows it, or that it is not tracked (looked up by address). */
		std::string DescribeSunCandidate(const void* a_geometry) const;
		std::size_t StoodInFadeRoots() const
		{
			return static_cast<std::size_t>(std::count_if(fadeRootOwned.begin(), fadeRootOwned.end(), [](const auto& a_root) { return a_root.second; }));
		}
		/** @brief A listed fade root's row from its node again (a new generation: the GPU's state restarts from it), owned bits kept. */
		void ReseedFadeRoot(const void* a_node);
		/** @brief Whether the geometry sits under a tracked category node (the tracked map, read on the caller's thread). */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;
		/**
		 * @brief IsTracked from the render thread (PrimaryCull's RefreshWalk, RunAdmission) while a pass may write the map: an
		 * unsynchronized read, the open defect "The render thread reads the coordinator's tracked map" (dclf-open-defects.md).
		 */
		bool IsTrackedFromRenderThread(const RE::BSGeometry* a_geometry) const { return IsTracked(a_geometry); }
		/** @brief Object LOD: the index ranges a tracked BSSubIndexTriShape draws (its visible segments' runs), or null. */
		const std::vector<LodSegments::Range>* LodRangesOf(const RE::BSGeometry* a_shape) const
		{
			const auto it = lodRanges.find(a_shape);
			return it != lodRanges.end() ? &it->second : nullptr;
		}

		/** @brief How a tracked geometry came to be tracked (diagnostics). */
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
		 * @brief The frame a category node was found and the refresh's cause (0 signature change, 1 forced by a detach or rescan); false
		 * when unknown (diagnostics).
		 */
		bool GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const;


		/** @brief Full eligibility (static and per-frame) of a tracked geometry; NotTriShape if untracked. */
		Ineligible Classify(RE::BSGeometry* a_geometry) const;


		/** @brief The geometry's membership pass this pass (BindByMembership), or null. */
		const AccumulatedPass* FindAccumulatedPass(const RE::BSGeometry* a_geometry) const;


		using PlacementPlan = DCLF::PlacementPlan;
		using ShadingItem = DCLF::ShadingItem;
		using FadeSeedItem = DCLF::FadeSeedItem;
		using TreeSeedItem = DCLF::TreeSeedItem;
		using WetnessValue = DCLF::WetnessValue;
		/** @brief Diagnostics (inline passes): the plan the last walk made, not yet published. */
		const PlacementPlan* PeekPlacementPlan() const { return placementPlanReady.get(); }

	private:
		std::vector<std::shared_ptr<const void>> retiredImports;  // into the next publication's deltas (TakeDeltas)
		// Slots named for the next frame's shading sample, into the next deltas.
		std::vector<ShadingItem> shadingNamed;
		/** @brief A slot's ShadingItem into shadingNamed (none for a free, unbound or propertyless slot). */
		void NameShading(std::uint32_t a_slot, bool a_member);
		/**
		 * @brief The walk: drains the shading events and names their dependents (their extras' static rows rewritten); every slot while the
		 * events are not installed.
		 */
		void NameShadingEvents(std::vector<std::uint32_t>* a_slots = nullptr);
		/** @brief The dependents of shading event keys (properties, emittance colours) named; their slots into a_slots when given. */
		void NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots);
		// The walk's placement plan: filled by its rounds and PublishPlacementPlan, ready once the walk is done.
		std::shared_ptr<PlacementPlan> placementPlan, placementPlanReady;
		// Plans no publication took yet, oldest first; the next publication carries them all.
		std::vector<std::shared_ptr<PlacementPlan>> placementPlansHeld;
		// Per-frame entries are walked once per frame input: the last such frame, whether this walk took them, and its movers and roots
		// (carried by later walks of that frame).
		std::uint32_t perFrameFrame = ~0u;
		bool perFrameWalked = true;
		// Keys and slots only: a held reference dropped by a later pass would be released on the lane, where engine destructors must not
		// run.
		struct CarriedItem
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t slot = kNoObjectSlot;
		};
		std::vector<CarriedItem> lastMovers;
		// The roots that walk listed (QueueRoots), carried likewise with their dependents.
		struct CarriedRoot
		{
			const RE::NiAVObject* root = nullptr;
			std::vector<CarriedItem> dependents;
		};
		std::vector<CarriedRoot> lastRoots;
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			// Its shader and layer properties, held from the naming event's pins (HoldProperties): the scene work's references are copies
			// of these.
			RE::NiPointer<RE::BSShaderProperty> property, layerProperty;
			RE::BSShaderProperty* HeldProperty(bool a_layer) const { return (a_layer ? layerProperty : property).get(); }
			// Written while the mirror held no record of it (WriteObject): evaluated again when a capture names it.
			bool leafWaiting = false;
			std::uint64_t identity = 0;
			std::uint64_t groupIdentity = 0;
			const RE::TESObjectREFR* actorOwner = nullptr;  // lookup only, never published
			const RE::NiNode* roomNode = nullptr;  // lookup key, refreshed on structural attachment
			std::uint64_t roomMapGeneration = 0;
			int roomIndex = -1;
			RE::NiNode* categoryNode = nullptr;
			// UnsupportedParent, Billboard or Switch (decided per frame) for what lies between the leaf and its category node; else None.
			Ineligible parentReason = Ineligible::None;
			// When and how it was added (GetTrackInfo).
			std::uint32_t trackedFrame = 0;
			TrackSource trackedBy = TrackSource::AttachEvent;

			/**
			 * @brief A cached negative verdict (positives are Derived's), checked in full against its witnesses each evaluation
			 * (rendererData too: a stale negative would leave an object native forever); ValidateSlice covers what has no cheap witness.
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
			 * @brief The cached positive derivation (descriptors, static flags, pipeline key, slots), valid while its witnesses hold; slots
			 * are checked against their keys before use, so a reused slot is never handed back.
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
			 * @brief The cull-only verdict (a culling candidate when the engine did not keep it) and the pass that took it; stands until a
			 * delta walk event retakes it.
			 */
			std::uint32_t candidateFrame = 0;  // the pass that classified it (PassStamp); 0: never classified
			Ineligible candidateReason = Ineligible::None;
			// An NPC face shape (a BSDynamicTriShape under a BSFaceGenNiNode, positions from FaceSnapshots); resolved once by the walk.
			bool faceShape = false;
			bool faceShapeResolved = false;
			// Its head (a key) while listed in faceHeads, and whether its last write found no head snapshot (the head's next publication
			// rewrites it).
			const RE::BSFaceGenNiNode* faceHead = nullptr;
			bool faceWaiting = false;
			// A face shape's head, held from the batch's pins at its attach (the face snapshot record's reference is a copy).
			RE::NiPointer<RE::BSFaceGenNiNode> faceHeadRef;
			// Its property's fade node, held from the naming event's pins: tree and fade-root owners are copied from it, never made from a
			// key.
			RE::NiPointer<RE::NiAVObject> fadeNodeRef;
			const void* fadeNodeRequested = nullptr;  // the fade node key asked of the render thread (FadeNodeRequest), until answered
			// Its LOD gate (token; 0: none) and side; held out after its gate flipped (until its detach); whether any gate ever tagged it.
			std::uint64_t gate = 0;
			std::uint8_t gateSide = 0;
			bool gateHeldOut = false;
			bool gateSeen = false;
			// Owned by an actor (GetUserData is an ActorCharacter): Advanced Skin gives its draws the actor's wetness
			// (Tables::skinWetness). Resolved once by the walk.
			bool actorOwned = false;
			bool actorOwnedResolved = false;
			// The joins' verdict when they left it unbound (Diagnostics::CensusVerdictOf reads this frame's).
			Ineligible accumulateReason = Ineligible::None;
			std::uint32_t accumulateReasonFrame = 0;  // the frame it was taken
			std::uint64_t accumulateReasonPass = 0;   // the pass (a join failed for a verdict of this pass)
			// Its index in this walk's tables, valid while objectStamp equals the coordinator's.
			std::uint32_t objectStamp = 0;
			std::uint32_t objectId = 0;
			// Its persistent object slot (objectId's value while it keeps a record), kNoObjectSlot while it has none.
			std::uint32_t slot = kNoObjectSlot;
			// The node whose bound decides sun caster candidacy, resolved once (the graph above a tracked geometry is fixed).
			const RE::NiAVObject* sunEntryNode = nullptr;
			bool sunEntryResolved = false;
			// Its light entry (LightEntryOf), listed in lightDependents while tracked; null: none.
			const RE::NiAVObject* lightRoot = nullptr;
			// Its main-pass layer's slot (LayerPropertyOf: a multi-index shape's additional property, second index list, decal group 3), or
			// kNoObjectSlot; it lives and dies with the base (WriteLayer).
			std::uint32_t layerSlot = kNoObjectSlot;
			Derived layerDerived;
			const void* listedLayerProperty = nullptr;  // the layer's property in propertyDependents
			// Its membership passes' derived descriptors, base and layer (BindByMembership stores them): they go with the entry.
			MembershipDerived membershipDerived, membershipLayerDerived;
			// The delta walk's. perFrame: inputs change every frame (PerFrameTraits), set by a classifying evaluation and kept.
			// scheduledWalk: the walk that last scheduled it. bucket: its histogram bucket (kNoBucket: none).
			bool perFrame = false;
			bool perFrameListed = false;
			// lightTraits: per frame only because it moves, follows a switch, has animated shading or is skinned: while its classification
			// stands the light path retakes only the switch verdict and shading inputs and lists it for FrameValues. 0 otherwise.
			// movedWalk: the walk that took this path.
			std::uint32_t lightTraits = 0;
			std::uint32_t movedWalk = 0;
			// Its move events' key (MoveKeyOf): the one reference on its chain, or null (the light path then places it every frame).
			const void* moveKey = nullptr;
			// An actor's light-path entry: the nodes up to its category node (in hiddenDependents), and whether one had a hidden event
			// since its last evaluation. Owed to the next per-frame walk, not the draining event walk, which skips per-frame entries (else
			// the first-person switch's hide of the third-person skeleton is lost).
			std::vector<const RE::NiAVObject*> hiddenChain;
			bool hiddenEventOwed = false;
			// kTraitSwitch: the one switch node on its chain and its child on the leaf's path, found at classification (null: none or
			// several).
			RE::NiSwitchNode* switchNode = nullptr;
			const RE::NiAVObject* switchChild = nullptr;
			// The keys it is listed under: its shader and alpha properties in propertyDependents (the last evaluation's), its sun entry
			// node in rootDependents (while tracked).
			const void* listedProperty = nullptr;
			const void* listedAlpha = nullptr;
			const void* listedEmittance = nullptr;  // external emittance's shared colour (emittanceEvents)
			const RE::NiAVObject* listedRoot = nullptr;
			std::uint32_t fullWalk = 0;  // the walk that must write it in full (an event, not the per-frame set)
			std::uint8_t bucket = kNoBucket;
			std::uint32_t scheduledWalk = 0;
			static constexpr std::uint8_t kNoBucket = 0xFF;
		};
		/**
		 * @brief The object's entry bound for the sun's cascade culls: the cascade cull (FUN_140e305c0) walks only the objectArray entries
		 * the full-frustum cull (FUN_141511f30) passed. A static reference's is its reference root (the topmost ancestor with the
		 * geometry's userData); an actor's is its cell's container (never tested); a terrain block's is its nearest BSMultiBoundNode.
		 */
		void ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live = false);
		/** @brief ResolveSunEntry's from the mirror alone. */
		const void* MirrorSunEntry(const void* a_geometry) const;
		/** @brief ResolveSunEntry from the live chain (live walks, the observers' check). */
		static const RE::NiAVObject* LiveSunEntry(const RE::BSGeometry& a_geometry);
		/** @brief LightEntryOf on the live chain: the category node's child a_geometry hangs from; a_actor: it is an actor's. */
		static const RE::NiAVObject* LiveLightRoot(const RE::BSGeometry& a_geometry, const RE::NiNode* a_category, bool& a_actor);

		/**
		 * @brief The sun entries DCLF can take wholly out of the engine's cascade culls, judged again from scene events after the delta
		 * walk. A change bumps the generation at once; the snapshot is rebuilt only on a walk that changed nothing (an exclusion for an
		 * older generation is never applied, and a cell load would rebuild it every frame).
		 */
		void UpdateSunCandidates(bool a_full);
		void DropSunCandidates();
		/** @brief A tracked geometry's primary verdict (Eligibility::PrimaryVerdict); kPrimaryEngine when it is not tracked. */
		std::uint8_t PrimaryVerdictOf(RE::BSGeometry* a_geometry) const;
		/** @brief A tracked entry's verdict as the eligibility rules read it (Eligibility::EntryVerdict). */
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
		 * @brief A tracked geometry's light entry: its actor's root (actors have no sun entry), else its sun entry unless that is a
		 * category node.
		 */
		const RE::NiAVObject* LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const;
		void UpdateLightCandidates(bool a_full);
		/**
		 * @brief A light entry's tracked geometries, or null (also for a node that became a category node, which would take every entry
		 * under it).
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

		/** @brief Diffs the category nodes and portal roots against the taken capture when it is new or a_force (the post-load rescan). */
		void RefreshCategoryNodes(bool a_force = false);
		struct EventBatch;
		struct BatchUnwindGuard;  // a batch back to the render thread on a throw (Internal.h)
		/** @brief First in the pass, before the drains: takes the newest category capture posted. */
		void TakeCategoryCapture();
		std::shared_ptr<const CategoryCapture> categoryTaken;
		/** @brief ApplyBatch: the fade node answers into their entries (Tracked::fadeNodeRef), each entry rewritten. */
		void ApplyFadeNodeAnswers();
		// The signature the category set was last rebuilt for.
		std::uint64_t categorySignature = 0;
		/**
		 * @brief The category node a_object hangs under (null: none) and the strongest parent reason, from the mirror's chain; the pointer
		 * is a key. CS_DCLF_MIRROR_PARITY checks it against the live chain.
		 */
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored = nullptr) const;
		/** @brief The same from the live chain (the parities and diagnostics; the object alive). */
		RE::NiNode* FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const;
		/** @brief The lane's mirror reads, checked against live reads under CS_DCLF_MIRROR_PARITY; counted per kind. */
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
			Reference,  // a geometry the mirror's walk found with no pin (lagged: the live scene no longer has it there)
			kCount
		};
		static constexpr std::array<const char*, static_cast<std::size_t>(MirrorRead::kCount)> kMirrorReadNames{ "category node", "subtree", "switch event", "node event",
			"hidden chain", "move key", "light entry", "sun entry", "unpinned geometry" };
		/** @brief Counts a mirror read's check; a difference (a_describe()) is checked again after the next batch (DeferredRead). */
		template <class Describe, class Recheck>
		void NoteMirrorRead(MirrorRead a_read, bool a_differ, Describe&& a_describe, Recheck&& a_recheck) const
		{
			++MirrorReads().checked[static_cast<std::size_t>(a_read)];
			if (a_differ)
				deferredReads.push_back({ a_read, a_describe(), std::function<bool()>(std::forward<Recheck>(a_recheck)) });
		}
		/** @brief The geometries under a_root by the mirror's children, each with its parent reasons below a_reason, in walk order. */
		void MirrorSubtree(const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored) const;
		/** @brief A live check's lease (EngineReadWindow): refused while the engine's update may run, the check is skipped and counted. */
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
		/** @brief Where this thread counts mirror reads: its chunk's counters under a ShardScope, else the coordinator's. */
		static inline thread_local MirrorReadStats* mirrorReadSink = nullptr;
		MirrorReadStats& MirrorReads() const { return mirrorReadSink ? *mirrorReadSink : mirrorReads; }
		/**
		 * @brief A differing read, checked again after the next batch from the mirror alone: agreeing, the mirror lagged; still differing,
		 * it is counted.
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
		 * @brief Tracks the geometries under a_root. A root with no mirror chain is captured by the render thread at the next frame start
		 * and retried once (pendingSubtrees); an attach event's root without one was out of the world (its world attach brings its own
		 * event).
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
		std::uint64_t categoryChildrenMissing = 0;  // a new category node the capture listed no children of (should not happen)
		/**
		 * @brief Tracks a_geometry under a_categoryNode from the batch's pin or (a_live) a live pointer; with neither it is left out
		 * (counted).
		 */
		struct GeometryPrep;
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live, const GeometryPrep& a_prep);
		// AddSubtree as a prepare (pure over the batch's frozen mirror, any thread under a ShardScope) and a merge in event order.
		/**
		 * @brief What AddGeometry reads of the mirror for one geometry (keys only). The sun entry and actor root are left to the merge
		 * under the mirror-read parity.
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
		/** @brief One subtree's prepare (PrepareSubtree): its category node (none: nothing tracked), and its geometries in walk order. */
		struct SubtreePrep
		{
			RE::NiAVObject* root = nullptr;
			SubtreeSource source = SubtreeSource::AttachEvent;
			RE::NiNode* category = nullptr;
			bool unmirrored = false;  // FindCategoryNode found a record missing on the way up
			std::vector<GeometryPrep> geometries;
		};
		/** @brief a_root's prepare: FindCategoryNode, MirrorSubtree (and its mirror-read parity check), each geometry's. Const. */
		void PrepareSubtree(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const;
		/** @brief One geometry's prepare under a_category (PrepareSubtree's, and AddSubtree's). Const. */
		void PrepareGeometry(RE::BSGeometry* a_geometry, Ineligible a_reason, const RE::NiNode* a_category, GeometryPrep& a_out) const;
		/** @brief A prepared subtree's merge (the pending retry, capture requests, AddGeometry for each), in event order. */
		void MergeSubtree(SubtreePrep& a_prep);
		/**
		 * @brief a_roots' prepares (CS_DCLF_FANOUT 1: this thread, 2: the pool); the caller merges them in order, and nothing in between
		 * may write the mirror, category nodes or portal roots.
		 */
		void PrepareSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots, std::uint32_t a_mode, std::vector<SubtreePrep>& a_out);
		/**
		 * @brief Tracks a_roots in order: prepared and merged each in turn (CS_DCLF_FANOUT 0, the mirror-read parity), else all prepared,
		 * then merged.
		 */
		void AddSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots);
		// A subtree's prepare is a whole walk (one root to hundreds of geometries): chunks of a few roots, the pool from 16.
		static constexpr std::size_t kSubtreeGrain = 4;
		static constexpr std::size_t kSubtreeFanoutMin = 16;
		std::vector<SubtreePrep> subtreePreps;
		/** @brief Adds hand-timed ms to a nested scene part's sums (since ResetTimes, and this pass's). */
		void AddScenePartMs(ScenePart a_part, double a_ms)
		{
			stats.scenePartMs[static_cast<std::size_t>(a_part)] += a_ms;
			stats.scenePartFrameMs[static_cast<std::size_t>(a_part)] += a_ms;
		}
		/**
		 * @brief The applied batch's pins by key, valid while it is applied; Pinned makes a reference from one (null: none). Counted:
		 * references from pins, from live pointers, refused.
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
			std::uint64_t properties = 0, propertiesRefused = 0;  // a tracked geometry's properties held anew, and with no pin
			std::uint64_t fadeNodes = 0, fadeNodesRefused = 0;   // their fade nodes held anew, and with no pin
			// Of the refused: copied from a root owner, requested, and the answers (held or none); the first few named.
			std::uint64_t fadeNodesFromRoots = 0, fadeNodesRequested = 0, fadeNodesAnswered = 0, fadeNodesAnsweredNone = 0;
			std::uint32_t fadeNodeNamed = 0;
			std::string fadeNodeFirst;
			std::uint64_t fadeRootsUnowned = 0;                  // a tree or fade root left unlisted for want of its entry's reference
			std::string firstPropertyRefused;
		} referenceStats;
		/** @brief Holds a_entry's properties anew from the batch's pins, as the mirror's geometry record names them. */
		void HoldProperties(Tracked& a_entry);
		/** @brief Light Limit Fix's room node for an object (GetParentRoomNode), from the mirror's chain. */
		const RE::NiNode* RoomNodeOf(const void* a_object) const;
		/** @brief Records written without the mirror's leaf, and those evaluated again when a capture named it. */
		struct LeafStats
		{
			std::uint64_t missing = 0, rescheduled = 0;
		} leafStats;
		/** @brief Skylighting::OcclusionTechnique from the records: the nearest fade node above the leaf's BSX flags. */
		std::uint32_t OcclusionTechniqueOf(const SceneCapture::LeafView& a_leaf, bool a_skylighting) const;
		void ValidateSlice();
		/** @brief The scene pass, before the walk: the frame inputs posted since its last pass (room map, pipeline frame). */
		void TakeAccumulateInputs();
		/** @brief The scene pass, after the joins: a used pipeline without a current PerGeometry block made from the pipeline frame. */
		void MakeNewPipelineConstants();
		std::shared_ptr<const GeometryPort::PipelineFrame> scenePipelineFrame;  // the scene lane's: the newest taken

		Ineligible ClassifyFrame(const Tracked& a_tracked) const;
		/**
		 * @brief CS_DCLF_BILLBOARDS: the face mode (0-5) of the one NiBillboardNode above a Billboard-parented leaf when DCLF turns it on
		 * the GPU (Billboards.h: a skinless BSEffectShaderProperty BSTriShape, a known mode); else -1 (it stays Ineligible::Billboard).
		 */
		std::int32_t BillboardModeOf(const Tracked& a_tracked) const;
		/**
		 * @brief CS_DCLF_FIRST_PERSON_DECALS: a first-person entry's decal (a blood decal, a weapon's BloodLighting) as a forward object
		 * drawn after the first-person opaque buckets, as the engine's accumulator does: no Deferred, its group's blend mode, depth tested,
		 * depth written by the opaque group only (modes 3 and 1), the group's depth bias.
		 */
		void FirstPersonDecalForward(const Tracked& a_tracked, LightingDescriptors& a_descriptors) const;
		/**
		 * @brief Whether a node is hidden for the walk's views: from the frame's capture (FrameGlobals::cullHidden, taken at frame start)
		 * for nodes whose kHidden the engine flips around its views, else the node's own bit. ShadowSceneNode::OnVisible hides a portal
		 * graph's always-render children and shared node during the room traversal (main camera cull jobs); TESWaterReflections::Update
		 * hides the player's 3D while a cube-map reflection updates (Main::Draw, render thread, during the cull jobs); Main::Draw keeps the
		 * first-person skeleton hidden for every world view. Live bits would drop the player's face shapes on reflection-update frames.
		 */
		bool HiddenForWalk(const RE::NiAVObject* a_object) const;
		/** @brief HiddenForWalk from a_object's record (the mirror's hidden bit where the frame's capture has none). */
		bool HiddenForWalk(const void* a_object, const SceneCapture::NodeRecord& a_record) const;
		/** @brief Whether the switch's record selects a_child, and is current. */
		static bool SwitchSelects(const SceneCapture::NodeRecord& a_switch, const void* a_child)
		{
			return (a_switch.kind & SceneCapture::kKindSwitch) && a_switch.switchChild && a_switch.switchChild == a_child && a_switch.switchCurrent;
		}

		ankerl::unordered_dense::map<RE::BSGeometry*, Tracked> tracked;
		SceneIdentity sceneIdentity;
		/** @brief The walk's iteration order, a member to keep its capacity; a join's entry carries its membership pass. */
		struct OrderEntry
		{
			RE::BSGeometry* geometry;
			Tracked* tracked;  // mutable: BuildFrame updates the cached verdict through it
			const AccumulatedPass* accumulated;
			bool layer = false;  // the joins': the entry's layer (Tracked::layerSlot) rather than its base
		};
		std::vector<OrderEntry> order;
		// Keys valid for this walk only; the captured recordId is durable.
		ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::shared_ptr<const FaceSnapshots::HeadView>> capturedFaceHeads;
		// The joins' iteration (the pass's membership joins); a member for its capacity.
		std::vector<OrderEntry> accumulateOrder;
		// Member decals (DecalOrder.cpp): object -> its chain, group, technique and sub-pass; joined and left with the membership.
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> memberDecals;
		// Decals changed since the last OrderDecals, and whether to rebuild the order whole.
		std::vector<std::uint32_t> decalsChanged;
		bool decalsRebuild = true;
		void NoteDecalChanged(std::uint32_t a_object) { decalsChanged.push_back(a_object); }
		void NoteDecalsCleared()
		{
			decalsChanged.clear();
			decalsRebuild = true;
		}
		// The member decals in draw order with their keys, kept so a change re-keys only the decals it names.
		struct KeptDecalOrder;
		std::shared_ptr<KeptDecalOrder> keptDecals;
		/** @brief Tables::decalOrdinal and decalCount: the member decals, in the engine's draw order. */
		void OrderDecals();
		/** @brief The kept decal order against one rebuilt whole (keys retaken): the observers' reference (DecalsOrdered). */
		struct DecalOrderComparison
		{
			std::uint64_t decals = 0, staleKeys = 0, unordered = 0;
			bool differ = false;
			std::string first, staleFirst;  // the first difference, the first stale key
		};
		DecalOrderComparison CompareDecalOrder() const;
		/**
		 * @brief The scene work's material captures, newest per material, let go after kMaterialSnapshotFrames unused frames. A join makes
		 * a record from one (MaterialPort::Evaluate); a material with none is requested (Channels::materialRequests).
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
		/** @brief ApplyEvents: the captures pushed since the last call (the newest kept), and the render thread's answers to requests. */
		void DrainMaterialCaptures();
		/** @brief A capture's reference, to EngineReleases (released at Present). */
		void HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held);
		/**
		 * @brief Keeps the material records from the frame's sources each pass, with no engine read: newly captured materials' slots
		 * evaluated again (RewriteCapturedMaterials), frame-sourced components per signature (RefreshMaterialSignatures), TexcoordOffset of
		 * watched materials (RefreshMaterialTransforms).
		 */
		void RefreshMaterialRecords();
		void RewriteCapturedMaterials();
		void RefreshMaterialSignatures();
		void RefreshMaterialTransforms();
		/** @brief A slot's record made (a join) or written from a_snapshot: listed under its signature, its material watched. */
		void NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot);
		void WatchMaterialTransforms(const RE::BSShaderMaterial* a_material, const MaterialPort::MaterialSnapshot& a_snapshot);
		/** @brief The tables reset (ResetSlotTables): the signatures' slot lists, the watch and the retries dropped. */
		void ResetMaterialRecords();
		// Materials DrainMaterialCaptures kept since the last RefreshMaterialRecords, and those a rewrite could not evaluate yet (an
		// Advanced Skin key not set up: retried while the capture is held).
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
			std::uint32_t frame = 0;                         // the frame it was captured or keyed (a frame, not a pass)
		};
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, TransformWatch> transformWatch;
		struct MaterialRecordStats
		{
			std::uint64_t passes = 0, captured = 0, rewritten = 0, retried = 0, uncovered = 0, samples = 0, applications = 0, slotsApplied = 0,
						  transformsWatched = 0, transformsWritten = 0;
		} materialRecordStats;
		// The material slots' texture bindings (MaterialBindings; Coordinator/MaterialBindings.cpp).
		MaterialBindings materialBindings;
		/** @brief After the records' writers: drains the answers, then resolves the changed (material log) and newly used slots. */
		void UpdateMaterialBindings();
		/** @brief A used slot's entry written from its record; bindings requested for views with none known. */
		void ResolveMaterialBinding(std::uint32_t a_slot);
		/** @brief The scene lane, before a commit or publication: entries written since the last call, each versioned and logged. */
		void VersionMaterialBindings();
		/** @brief A view's binding: answered in this drain, cached from an earlier answer, else asked for (pending; a_slot waits on it). */
		GpuTextures::Binding MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot);
		/** @brief A material slot's retirement came back (no publication names it): its entry dropped. */
		void RetireMaterialBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped; an answer to an earlier request is stale. */
		void ResetMaterialBindings();
		// The shared, projected, shadow mask and shadow texture bindings (SharedBindings; Coordinator/SharedBindings.cpp).
		SharedBindings sharedBindings;
		/**
		 * @brief After the technique rows' refresh: answers drained, fixed and projected bindings taken, technique masks resolved, shadow
		 * textures requested or released.
		 */
		void UpdateSharedBindings();
		/** @brief A view's binding: answered in this drain, cached (here or in the material bindings), else requested. */
		GpuTextures::Binding SharedViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, SharedBindings::Kind a_kind);
		/** @brief The projected entries from their capture; pending ones requested. */
		void ResolveProjectedBindings(std::uint32_t a_textures);
		/** @brief A used pipeline's technique mask entry from its row. */
		void ResolveMaskBinding(std::uint32_t a_slot, ID3D11ShaderResourceView* a_frameMask);
		/** @brief A shadow texture's entry from an answer or a known binding, else requested. */
		void ResolveShadowTextureBinding(ID3D11ShaderResourceView* a_view);
		/** @brief A pipeline slot's lookups retirement came back (no publication names it): its mask entry dropped. */
		void RetireMaskBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped, except the render thread's posted capture. */
		void ResetSharedBindings();
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		// The portal graphs' parentless roots (alwaysRenderChildren), each filed under its graph's shared portal node, a category node
		// (FindCategoryNode, RefreshCategoryNodes); held while listed.
		struct AlwaysRenderRoot
		{
			RE::NiPointer<RE::NiAVObject> root;
			RE::NiNode* category = nullptr;
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, AlwaysRenderRoot> alwaysRenderRoots;
		// The first-person members' category node (CategoryCapture::firstPersonRoot). Its kHidden is view-scoped (Main::Draw shows it only
		// for first person): ClassifyFrame stops at it before the hidden test, and its hidden events announce nothing.
		const RE::NiNode* firstPersonCategory = nullptr;
		/** @brief Diagnostics: the node that hid a first-person skeleton entry (ClassifyFrame), the first 16. */
		void NoteFirstPersonHidden(const Tracked& a_tracked, const void* a_key, const SceneCapture::NodeRecord& a_record, std::uint32_t a_depth) const;
		/** @brief Whether a tracked entry is a first-person one (its category node the first-person root). */
		bool FirstPersonEntry(const RE::NiNode* a_categoryNode) const { return a_categoryNode && a_categoryNode == firstPersonCategory; }
		// Diagnostics: the frame each category node was found and why (GetCategoryInfo), and the source AddGeometry stamps on new entries.
		ankerl::unordered_dense::map<const RE::NiNode*, std::pair<std::uint32_t, std::uint8_t>> categoryFound;
		TrackSource addSource = TrackSource::AttachEvent;
		std::size_t validationCursor = 0;
		// Set by a load screen's marker: the next batch rebuilds the tracked set from scratch.
		bool rescanPending = false;
		std::uint64_t publishedLodForced = 0; // the LOD gates' forced generation of the last publication (NotePublished)
		FrameInputs sceneInputs;              // the newest taken
		std::uint32_t verdictsApplied = 0;    // the verdicts generation last applied (InvalidateVerdicts)
		/** @brief First in each pass: the newest frame inputs taken (sceneInputs, sceneFrame); the verdicts dropped when asked. */
		void TakeFrameInputs();
		// The coordinator's frame (from its last frame inputs; GetFrame on a scene work thread) and pass count (PassStamp).
		std::uint32_t sceneFrame = 0;
		std::uint64_t passSerial = 0;
		std::shared_ptr<const ankerl::unordered_dense::map<const RE::NiNode*, int>> roomMap;
		std::uint64_t roomMapGeneration = ~0ull;
		/** @brief The scene mirror: the hooks' records, applied in event order (ApplyMirrorEvents); checked by CS_DCLF_MIRROR_PARITY. */
		SceneMirror mirror;
		void ApplyMirrorEvents(const EventBatch& a_batch);
		// A load screen's attach and detach events for the mirror (taken by the first pass after the load).
		std::shared_ptr<EventBatch> loadingCarry;

		Tables tables;
		// The published table snapshots (PublishTables), pooled: one no publication holds any more is written again.
		std::vector<std::shared_ptr<Tables>> tablesPool;
		// Per pool entry: whether it equals the tables at its change log's end (brought up to date by replay); a new snapshot is copied
		// whole once.
		std::vector<std::uint8_t> tablesPoolReplayable;
		std::vector<std::uint8_t> replaySlotMarks;  // scratch: the object slots a replay copied
		// The last published snapshot.
		std::shared_ptr<Tables> publishedTables;
	public:
		/** @brief A thread running scene work (the lane or a fan-out chunk): GetFrame is the coordinator's there. */
		static inline thread_local bool sceneWorkThread = false;

	private:
		Channels& channels = Channels::Get();
		// The last copy of the category nodes, and whether the set changed since (copied again at the next publication).
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> publishedCategoryNodes;
		bool nodeSetsDirty = true;  // the category nodes changed
		using LightEntryChange = DCLF::LightEntryChange;
		std::vector<LightEntryChange> lightEntryChanges;               // since the last publication
		/** @brief Whether a node is one of the coordinator's category nodes (the frame reads IsCategoryNode). */
		bool CategoryNodeOwn(const RE::NiAVObject* a_node) const
		{
			return categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}
		/** @brief Early in a full pass: applies the fade root posts (Channels::fadeRootsSentPosted, fadeOwnedPosted). */
		void TakeCoordinatorInputs();
		// Pipeline blocks the render thread posted (Channels::pipelineConstantsPosted), applied where the slot still holds the key.
		ConstantsPostStats constantsPostStats;
		void ApplyConstantsPosts();
		// The frame inputs the technique rows were last evaluated against; fog left out (a row keeps its fog: KeepTechniqueFog).
		struct TechniqueWitness
		{
			TechniqueInputs inputs{};
			std::uint8_t byte12 = 0, byte7 = 0;
			bool valid = false;
		} techniqueWitness;
		/** @brief Each full pass before the walk: every technique row evaluated again when the inputs differ from the witness. */
		void RefreshTechniqueRows();
		/** @brief A technique row from its frame's sample, written and versioned where it differs. */
		void EvaluateTechniqueRow(std::uint32_t a_row);
		// The change log position at the last commit, which the applied set's revocation checks from.
		LogCursor setCommitCursor;
		TablesPublication tablesPublication;
		void PublishTables();
		// The walk the entries' objectStamp must match; a new walk or a teardown bumps it, invalidating every index at once.
		std::uint32_t objectStamp = 1;
		void InvalidateObjectIndices() { ++objectStamp; }
		// Only for CS_DCLF_WALK_PARITY's dense rebuild: objects laid out densely, the entries' slots and indices left alone.
		bool denseWalk = false;
		/** @brief The slot the walk writes this object at: its own, a free one, or a new one. */
		std::uint32_t AcquireObjectSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry);
		/** @brief After a walk: frees every slot it did not write and sorts the per-frame index lists. */
		void SweepObjectSlots();
		/**
		 * @brief Frees an entry's slot as it leaves the tracked set (outside the walk), so a rescan's entries reuse slots instead of
		 * growing the arrays.
		 */
		void ReleaseObjectSlot(Tracked& a_entry);
		/** @brief The entry's layer slot for base slot a_base: its own or a new one. */
		std::uint32_t AcquireLayerSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry, std::uint32_t a_base);
		void ReleaseLayerSlot(Tracked& a_entry);
		/** @brief The property a slot's record draws: its entry's held one, or its layer's for a layer slot. */
		RE::BSShaderProperty* SlotProperty(std::uint32_t a_slot) const;
		/** @brief Whether a member's binding still holds for its object (the derivation cache's witnesses). */
		bool MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const;
		void EraseTracked(RE::BSGeometry* a_geometry);
		/**
		 * @brief CS_DCLF_WALK_PARITY's reference: every entry rewritten from scratch into dense tables and classified without caches, the
		 * coordinator left unchanged.
		 */
		void DenseReference(Tables& a_dense, ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible>& a_reasons);
		// The dense rebuild's verdicts, classified without the caches: what walk parity checks the kept ones against.
		ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible> referenceReasons;
		// The slots by key, and each material's slots.
		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::vector<std::uint32_t>> materialDependents;
		void ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		void UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		// The pass's membership passes (BindByMembership) by geometry, and its layers' (PrimaryCull::MembershipLayerPass).
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedPasses;
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedLayerPasses;
		/**
		 * @brief A BSShaderMaterial reference taken and dropped the engine's way (AE): the material database (manager 0x143187758) shares
		 * one material among equal properties, and its release (FUN_1414f7a40, used by BSShaderProperty::SetMaterial) decrements under the
		 * database's lock and at zero removes it from the database before deleting it. A BSTSmartPointer would delete it directly.
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
			/** @brief Takes over a count already taken (a HeldSnapshot's). */
			void Adopt(RE::BSShaderMaterial* a_material) { material = a_material; }
			explicit operator bool() const { return material != nullptr; }

		private:
			RE::BSShaderMaterial* material = nullptr;
		};
		// Per material slot: its engine reference on the material, so the pointer is not reused while live; retired via
		// RetiredBatch::materials.
		std::vector<MaterialReference> materialOwners;
		// Fade and tree root seed requests (into the deltas); the mirror gives a row's coordinator half, the seed the rest.
		std::vector<FadeSeedItem> fadeSeedRequests;
		std::vector<FadeSeedItem> treeSeedRequests;
		FadeRootStatic FadeRootFromMirror(const void* a_node);
		/** @brief A new generation for a root's row, its seed row toggled and requested. */
		void RequestFadeSeed(std::uint32_t a_root, FadeRootStatic& a_row);
		/** @brief The coordinator's residency of a slot (IsMember is the frame's). */
		bool ResidentObject(std::int32_t a_object) const { return a_object >= 0 && IsResidentSlot(static_cast<std::uint32_t>(a_object)); }
		std::uint32_t tablesGeneration = 0;
		std::uint32_t materialVersions = 0;  // the last Tables::materialVersion handed out

		// The lookups (Lookups.h): written by the bindings, resolved against the newest catalog before each commit and publication; each
		// publication carries an immutable copy, the render thread's only view.
		Lookups lookups;
		LookupsResolveState lookupsResolve;
		std::shared_ptr<const PipelineCatalog> lookupsCatalog;  // the catalog they were last resolved against
		// The last publication's lookups (shared again while unchanged: Lookups::ChangeKey) and catalog; chunk copies counted.
		std::shared_ptr<const Lookups> publishedLookups;
		std::shared_ptr<const PipelineCatalog> publishedCatalog;
		std::array<std::uint64_t, 6> publishedLookupsKey{};
		std::uint64_t lookupsChunkCopies = 0;
		/**
		 * @brief Before a commit or publication: material entries versioned, then pipeline and shadow-pipeline entries resolved against the
		 * pipeline lane's newest catalog.
		 */
		void ResolveLookups();
		/** @brief The lookups as an immutable copy with their catalog (the last copy again if unchanged). */
		void PublishLookups();
		/** @brief New lookups (tables reset): generations past the last, versions continuing. */
		void ResetLookups();
		// Frame state the walk reads once and the joins reuse, so both halves agree.
		bool sceneBuilt = false;  // the walk ran: the records are this frame's
		bool frameResolveBuffers = false;
		bool frameInterior = false;
		std::array<std::uint32_t, 4> frameDecalBias{};
		bool graphWasActive = false;  // the previous BuildFrame's resolveBuffers, to log the flip
		/** @brief An object's extras rows' static parts from its property and geometry; true when changed. */
		bool WriteObjectExtras(std::uint32_t a_object);
		void BuildScenePhase();
		void BuildAccumulatePhase();
		// A join's result: values only, applied without dereferencing a geometry, material, property or render pass.
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
		/** @brief Walk parity's reference: every entry of `order` written from scratch into dense tables. */
		void DenseWalk();
		/** @brief Clears the per-frame tables and the walk's counters; a_keepIndices: a delta walk's, keeping object indices. */
		void BeginWalk(bool a_keepIndices = false);

		/**
		 * @brief The walk: evaluates only entries whose inputs can have changed (per-frame entries, new or marked ones, dependents of
		 * property, node and attach events, slots whose geometry slot went stale); every other slot keeps its record. A classification
		 * stands until such an event (dclf-event-driven-tables.md); CS_DCLF_WALK_PARITY checks it against a from-scratch walk.
		 */
		void DeltaWalk();
		/** @brief The whole tracked set into `order` (a full evaluation, the dense walk). */
		void BuildFullOrder();
		struct EvalCounters;
		struct TrackedDelta;
		struct EvalResult;
		/**
		 * @brief One walk entry: writes its record at its slot; false when it gets none. EvaluateWrite then MergeWrite in turn (the dense
		 * walk).
		 */
		bool WriteObject(RE::BSGeometry* a_geometry, Tracked& a_tracked, PartTimer& a_timer, Ineligible& a_bucket);
		/**
		 * @brief The entry's layer record after its base: the base's placement, the second index list, no shadow, bound with the base.
		 * Without a_member (the base a main-pass record) or a layer, the slot is released.
		 */
		void WriteLayer(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, Tracked& a_tracked, std::uint32_t a_base, bool a_member, bool a_keepMember,
			PartTimer& a_timer);
		/** @brief Why an entry's inputs change every frame (Tracked::perFrame); 0 when they do not. */
		enum PerFrameTrait : std::uint32_t
		{
			kTraitFace = 1u << 0,
			kTraitActor = 1u << 1,
			kTraitSwitch = 1u << 2,
			kTraitSkin = 1u << 3,
			kTraitAnimatedShading = 1u << 4,  // its reference root's subtree moves
			kTraitMoves = 1u << 5,            // a controller or a non-fixed rigid body on its chain
			kTraitRootMoves = 1u << 6,        // its reference root's subtree moves (RootMoves)
		};
		std::uint32_t PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const;
		/** @brief PerFrameTraits from what the entry's evaluation has. */
		std::uint32_t PerFrameTraitsOf(bool a_faceShape, bool a_actorOwned, const RE::NiNode* a_categoryNode, const RE::BSGeometry& a_geometry) const;
		/**
		 * @brief Whether an entry with this verdict is per frame, and its light-path traits (a_traits: all traits). a_freshMotion: walk
		 * parity's root motion from scratch.
		 */
		std::pair<bool, std::uint32_t> PerFrameOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, Ineligible a_reason, std::uint32_t& a_traits,
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool>* a_freshMotion = nullptr);
		/** @brief Adds an entry to this walk's `order` once; a_full: written in full even if it only moved. */
		void Schedule(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_full = true);
		/**
		 * @brief Whether a kept record's skin stays as written (skinning on, partitions drawn, palette size unchanged), checked at the
		 * merge against the slot; notes a changed partition mask.
		 */
		bool KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked, const EvalResult& a_result);
		/**
		 * @brief Whether an actor's earlier record is what WriteObject would write again but for placement and palette (its verdict
		 * stands).
		 */
		bool ActorRecordKept(const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief A face shape's snapshot and region this walk; false without one. */
		bool ResolveFace(RE::BSGeometry& a_geometry, const Tracked& a_tracked, FaceSnapshots::ShapeView& a_face, std::uint32_t& a_region);
		/** @brief Appends a face shape's stream to this walk's faceStreams and points its slot at it. */
		void PushFaceStream(RE::BSGeometry& a_geometry, const Tracked& a_tracked, std::uint32_t a_slot, const FaceSnapshots::ShapeView& a_face, std::uint32_t a_region);
		/**
		 * @brief The heads' publications: kept face streams take the new snapshot in place; waiting, missing or stale-head shapes are
		 * rewritten.
		 */
		void ApplyFacePublications();
		/** @brief A tracked face shape joins or leaves its head's list; the head's last one releases its record. */
		void ListFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void ClearFaceShapes();
		/**
		 * @brief A root whose bound is an entry's only motion (kTraitRootMoves alone) is listed in the placement plan instead of the entry,
		 * when it had a move event this frame or the last (QueueRoots).
		 */
		struct MovingRoot
		{
			const void* key = nullptr;  // its reference
			const RE::NiNode* categoryNode = nullptr;
		};
		// Every root RootMoves found moving, until ScheduleRoot or its last dependent forgets it.
		ankerl::unordered_dense::map<const RE::NiAVObject*, MovingRoot> movingRoots;
		std::vector<const RE::NiAVObject*> rootPlacements;  // the roots the plan lists
		void QueueRoots();
		struct PlacementStats
		{
			std::uint64_t roots = 0, stillRoots = 0, rootsGated = 0;
			std::uint64_t fadeRootsUnmirrored = 0;  // listed with no mirror record (FadeRootFromMirror)
		} placementStats;
		/** @brief The walk's plan item for a recorded entry (slot, layer row, sun entry, palette block). */
		PlacementPlan::Item PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief The walk's plan, opened by its first round. */
		PlacementPlan& WalkPlan();
		/** @brief After the walk: the roots it takes into the plan (QueueRoots), and the plan made ready. */
		void PublishPlacementPlan();
		/** @brief The frame each key last had a move event (drained at the delta walk). */
		ankerl::unordered_dense::map<const void*, std::uint32_t> movedFrame;
		// Keys with a move event this frame and the last: QueueRoots takes those roots' bounds whatever RootMoves says (cell updates
		// recompute a still root's bound).
		std::array<std::vector<const void*>, 2> movedKeys;
		// movedKeys turn over once a frame (a burst's passes add to this frame's); movedFrame is pruned every 256 frames.
		std::uint32_t movedKeysFrame = 0, movedPruneFrame = 0;
		bool moveGating = false;
		std::uint32_t moveUngatedThrough = 0;
		void DrainMoveEvents(bool a_full);
		// Actor entries by the nodes on their chain, for the hidden events.
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> hiddenDependents;
		bool hiddenGating = false;
		bool hiddenWitness = false;
		void ListHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void DrainHiddenEvents();
		/** @brief The applied batches' hidden stores (mirror updates with a site), taken by DrainHiddenEvents. */
		struct BatchHidden
		{
			const void* key = nullptr;
			std::uint32_t site = 0;
			bool hidden = false;
			std::uint64_t sequence = 0;  // SceneCapture::NextSequence after the store
		};
		std::vector<BatchHidden> batchHidden;
		/**
		 * @brief Hidden events newer than the capture the pass judged by (cullHidden) that differed: announced again at the first pass with
		 * a later capture (cullHiddenSequence). Keys only.
		 */
		ankerl::unordered_dense::map<const void*, std::uint64_t> hiddenAhead;
		bool MovedRecently(const void* a_key) const
		{
			const auto it = movedFrame.find(a_key);
			return it != movedFrame.end() && sceneFrame - it->second <= 1;
		}
		const void* MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const;
		/**
		 * @brief Whether a reference root's subtree holds anything that moves (controller, non-fixed rigid body, skin), so its bound, the
		 * sun entry under it, is not fixed. Walked once per root, again after an event under it.
		 */
		bool RootMoves(const RE::NiAVObject* a_root, const bool* a_now = nullptr);
		bool RootMovesNow(const RE::NiAVObject* a_root) const;
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;
		/** @brief Lists an evaluated entry under its properties; UnlistDependents takes it off every list. */
		void ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root);
		/** @brief Evaluates the entry this walk with its classification retaken. */
		void Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/**
		 * @brief Whether a placement event can change the entry's tables: it has a record or a frame verdict (None, Hidden, Switch, Actor).
		 */
		static bool PlacementMatters(const Tracked& a_tracked);
		/** @brief Reclassifies a sun entry node's dependents and forgets its motion; a key, never dereferenced. */
		void ScheduleRoot(const RE::NiAVObject* a_root);
		/** @brief A node Havok moved or gave a controller: the entries under it and the dependents of every node above it. */
		void ApplyNodeEvent(RE::NiAVObject* a_node);
		/**
		 * @brief Before the first round: drained switch events whose selection changed, each caught up (CatchUpSwitch) and, in a delta
		 * walk, its entries reclassified.
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
		/** @brief Ends a slot's membership; a_restore resets its accumulated half now. */
		void DropResidentSlot(std::uint32_t a_slot, bool a_restore);
		/** @brief A slot's accumulated half back as the walk wrote it. */
		void ResetAccumulatedHalf(std::uint32_t a_slot);
		/** @brief The residents' pipeline and material slots used this frame, and a template when none was. */
		void KeepResidentsAlive();
		/** @brief A member's tree slot (Tables::objectTree): taken for a tree member, given back otherwise or when it leaves. */
		void ListTree(std::uint32_t a_slot);
		void UnlistTree(std::uint32_t a_slot);
		/** @brief A member's fade root (Tables::fadeRoots): listed when it joins, released with its last member. */
		void ListFadeRoot(std::uint32_t a_slot);
		void UnlistFadeRoot(std::uint32_t a_slot);
		/**
		 * @brief The reference a newly listed tree or fade root owner is copied from: the slot's entry's fade node if it is a_node, else
		 * null (not listed: fadeRootsUnowned).
		 */
		RE::NiPointer<RE::NiAVObject> HeldFadeNode(std::uint32_t a_slot, const void* a_node);
		/**
		 * @brief a_entry's fade node reference for a_node: the batch's pin, a root owner's copy, or requested from the render thread (once
		 * per key). True when held now.
		 */
		bool AcquireFadeNode(Tracked& a_entry, const void* a_node, const char* a_path);
		/** @brief A switch event on a tree root's LOD switch: its kFadeRootTreeLod retaken. */
		void RefreshFadeRootSwitch(const RE::NiAVObject* a_switch);
		/** @brief A fadeAmount event (Actor::SetAlpha): a listed root's fadeAmount retaken from its node, its state row kept. */
		void RefreshFadeAmount(const void* a_node);
		void MoveBucket(Tracked& a_tracked, Ineligible a_bucket);
		/**
		 * @brief Binds the bind queue's records as residents from membership passes (PrimaryCull::MembershipPass), kept until rewritten.
		 */
		void BindByMembership();
		/**
		 * @brief One bind queue element's evaluation (pure): skip, binding stands, or its membership pass. The merge, in queue order,
		 * stores the cache entry, leaves the set, drops a failed member and queues the join.
		 */
		struct JoinPre
		{
			enum class Kind : std::uint8_t
			{
				Skip,    // not a record to bind (free, shadow only, another entry's slot, not this walk's, verdict not None)
				Kept,    // a member whose binding stands (MemberBindingStands)
				Failed,  // no membership pass (PrimaryCull::LastSyntheticFail's reason in fail)
				Pass,    // a membership pass: joined by BuildAccumulatePhase
			};
			Kind kind = Kind::Skip;
			RE::BSGeometry* geometry = nullptr;
			bool layer = false;
			SceneCapture::LeafView leaf;
			MembershipDerived derived;  // the cache entry the pass leaves
			AccumulatedPass pass;
			std::uint8_t fail = 0;
		};
		void EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const;
		/**
		 * @brief One membership join's evaluation (pure): the held property check, the derivation cache's witnesses (its slots are the
		 * merge's), classification, pipeline key, static flags, room index. a_classify: classify even where the witnesses hold.
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
			RE::BSShaderProperty* property = nullptr;        // the entry's held property
			const RE::BSShaderMaterial* material = nullptr;  // the witness, the material slot's key
			std::uint8_t fadeState = 0;
			bool alphaBelowOne = false;
			std::uint32_t geometrySlot = 0;
			bool derivedWitness = false;  // the derivation cache's witnesses hold
			bool classified = false;      // descriptors, key and static flags are this join's
			Ineligible reason = Ineligible::None;
			LightingDescriptors descriptors;
			PipelineKey key{};
			std::uint32_t staticFlags = 0;
			bool roomSet = false;  // Light Limit Fix's room index retaken (the room map moved on)
			int roomIndex = -1;
		};
		void EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const;
		/**
		 * @brief a_count evaluations into a_results, in chunks of kFanoutGrain on the pool (a_pool) or on this thread. In
		 * AccumulatePhase.cpp.
		 */
		template <class Result, class Evaluate>
		void EvaluateJoins(std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate);
		std::vector<JoinPre> joinPres;
		std::vector<JoinResult> joinResults;
		/** @brief The technique row of a pass descriptor's TechniqueKey, made and evaluated when new. */
		std::uint32_t TechniqueRowFor(std::uint32_t a_passDescriptor);
		bool shadingNamedAll = false;  // NameShadingEvents named every slot once
		// The shading events: slots resampled, LOD fade and emittance events.
		struct ShadingStats
		{
			std::uint64_t resampled = 0, lodFadeEvents = 0, emittanceEvents = 0;
		} shadingStats;
		std::vector<const void*> lodFadeChanged;
		// Object LOD (dclf-lod.md): each tracked BSSubIndexTriShape's drawn index ranges, taken again at its segment events.
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::vector<LodSegments::Range>> lodRanges;
		struct LodSegmentStats
		{
			std::uint64_t events = 0, changed = 0;
		} lodSegmentStats;
		void SampleLodRanges(const RE::BSGeometry& a_shape, bool a_event);
		void ApplyLodSegmentEvents(const std::vector<const void*>& a_shapes);
		std::array<std::uint64_t, kChangeCauseCount> reportedChangeCounts{};
		/** @brief After the delta walk's evaluations: the kept slots' geometry slots, and the shadow sets. */
		void FinishDeltaWalk(PartTimer& a_timer);
		/**
		 * @brief One walk round from a_first: evaluations (EvaluateEntry, pure), then merges in index order (MergeEntry). CS_DCLF_FANOUT 0:
		 * each in turn (also under the mirror-read parity); 1: all evaluated, then merged; 2 (default): pool chunks, each merged once
		 * ready.
		 */
		void EvaluateRound(PartTimer& a_timer, std::size_t a_first);

		// Entries per fan-out chunk (one EvalCounters each), and the fewest a round fans out for.
		static constexpr std::size_t kFanoutGrain = 64;
		static constexpr std::size_t kFanoutMinEntries = 128;
		/**
		 * @brief An evaluation chunk's counters, merged in chunk order before the round's merges, so first-named strings match the serial
		 * walk.
		 */
		struct EvalCounters
		{
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			std::array<std::uint32_t, 64> techniqueRejects{};
			std::uint32_t classifyHits = 0, classifyChecked = 0, classifyDiffers = 0;
			std::string classifyDiffersFirst;
			std::uint64_t verdictsChecked = 0, verdictsSkipped = 0, verdictsMissed = 0;
			std::string firstVerdictMissed;
			std::uint64_t leafMissing = 0;  // leafStats.missing
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};  // the chunk's PartTimer (CS_DCLF_PROFILE)
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			MirrorReadStats mirror;  // mirrorReadSink
			ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;  // RootMovesNow's answers
		};
		/** @brief What an evaluation decided for its entry's Tracked, applied by its merge. */
		struct TrackedDelta
		{
			bool faceShapeSet = false, faceShape = false;      // resolved once
			bool actorOwnedSet = false, actorOwned = false;    // resolved once
			bool verdictSet = false;
			Tracked::StaticVerdict verdict;
			bool candidateSet = false;                         // classified, or its frame verdict changed
			std::uint32_t candidateFrame = 0;
			Ineligible candidateReason = Ineligible::None;
		};
		/**
		 * @brief One entry's evaluation for its merge: reads only the frozen mirror, the entry's own Tracked and the pass snapshot, never a
		 * table, slot or other entry.
		 */
		struct EvalResult
		{
			enum class Write : std::uint8_t
			{
				None,      // kept by the light path
				NoLeaf,    // leafWaiting
				Rejected,  // its verdict gives it no record
				Record,    // the merge resolves the face, the geometry and the slots
			};
			EvaluateKind kind = EvaluateKind::Event;
			// The light path (first round): kept, recorded, and KeepSkin's table half left to the merge.
			bool light = false;
			bool lightRecorded = false;
			bool lightSkin = false;
			std::uint32_t skinRows = 0;
			std::uint16_t skinPartitions = 0;
			std::uint32_t skinLodPartitions = 0;
			bool verdictUnannounced = false;  // the frame verdict changed with no hidden event
			Write write = Write::None;
			TrackedDelta delta;
			SceneCapture::LeafView leaf;
			Ineligible reason = Ineligible::None;  // its bucket
			bool shadowOnly = false;
			bool faceShape = false;        // its snapshot is the merge's (ResolveFace)
			bool referenceReason = false;  // the dense walk's: reason into referenceReasons
			// Slot-free record inputs.
			std::uint16_t partitionMask = 0;
			bool lodChain = false;
			bool skinCandidate = false;  // in this walk's skinnedObjects
			std::uint32_t boneRows = 0;  // 0 without a palette (or past 240)
			std::uint32_t particleRows = 0;  // a particle member's block of the frame's palettes (ParticleSnapshots::RowsOf)
			std::uint8_t shadowReject = 0;
			std::uint32_t shadowTechnique = 0;
			const RE::BSShaderMaterial* shadowMaterial = nullptr;  // the alpha-tested techniques' diffuse
			ID3D11ShaderResourceView* shadowDiffuse = nullptr;
			std::array<std::uint32_t, kOcclusionViews> occlusion{};
			std::uint32_t skinLodWord = 0;  // SkinLodPartitionsOf (0 for a LOD chain)
			// A classified entry: traits without root motion (PerFrameFrom's input), whether it may get a record, its sun entry and that
			// root's motion now, its move key.
			bool classified = false;
			std::uint32_t traits = 0;
			bool mayRecord = false;
			const RE::NiAVObject* rootCandidate = nullptr;
			bool rootMovesNow = false;
			const void* moveKey = nullptr;
		};
		/** @brief Entry a_index evaluated (a_light: the first round, trying the light path). Pure. */
		void EvaluateEntry(std::size_t a_index, bool a_light, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief WriteObject's evaluation half: the classification and the slot-free record inputs. */
		void EvaluateWrite(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief A classified entry's bookkeeping, evaluated (see EvalResult), plus its shading inputs where the merge may store them. */
		void EvaluateClassified(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief PerFrameOf at the merge, from the evaluation's traits and root motion. */
		std::pair<bool, std::uint32_t> PerFrameMerge(const Tracked& a_tracked, const EvalResult& a_result);
		/** @brief PerFrameOf's verdict from the traits (root motion included), the verdict and whether it may get a record. */
		static std::pair<bool, std::uint32_t> PerFrameFrom(std::uint32_t a_traits, Ineligible a_reason, bool a_mayRecord);
		/** @brief Entry a_index's merge (a_profileKinds: time its EvaluateKind). */
		void MergeEntry(std::size_t a_index, bool a_profileKinds, EvalResult& a_result, PartTimer& a_timer);
		/** @brief WriteObject's merge half: the Tracked delta, then the face, geometry slots and record. */
		bool MergeWrite(RE::BSGeometry* a_geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& a_timer, Ineligible& a_bucket);
		/** @brief A chunk's counters into the coordinator's, in chunk order; its warnings logged. */
		void MergeCounters(EvalCounters& a_counters);
		/** @brief A chunk's mirror-read counts into the coordinator's. */
		void MergeMirrorReads(MirrorReadStats& a_counts);
		/** @brief The mode a round of a_count entries runs in (CS_DCLF_FANOUT, the parities, the thread); the pool from a_least entries. */
		std::uint32_t FanoutMode(std::size_t a_count, std::size_t a_least = kFanoutMinEntries) const;
		/** @brief What a pass's evaluations read in place of the live toggles and Terrain Blending. */
		struct PassSnapshot
		{
			ToggleSet toggles;
			bool terrainDefers = false;
		};
		PassSnapshot passSnapshot;
		std::shared_ptr<const FrameGlobals> passGlobals;  // the frame globals the pass binds, on the pool too
		struct ShardScope;
		std::vector<EvalResult> evalResults;
		std::vector<EvalCounters> evalCounters;
		/** @brief The fan-out since the report. */
		struct FanoutStats
		{
			std::uint64_t rounds = 0, parallelRounds = 0, entries = 0, parallelEntries = 0, chunks = 0;
			std::uint64_t joinRounds = 0, joinParallelRounds = 0, joinEntries = 0;
			std::uint64_t lightMissed = 0;  // light-path entries rewritten in full at the merge
			// AddSubtrees' calls, roots and geometries, those prepared on the pool, and prepare / merge times (modes 1 and 2).
			std::uint64_t subtreeRounds = 0, subtreeParallelRounds = 0, subtreeRoots = 0, subtreeParallelRoots = 0, subtreeGeometries = 0;
			double subtreePrepareMs = 0.0, subtreeMergeMs = 0.0;
		} fanoutStats;
		/** @brief A slot's inputs to the frame's shadow sets. */
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
		// Only slots whose shadow inputs changed this walk update their contributions.
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
		bool fullEvaluation = true;  // a reset, a load or a live toggle: the next delta walk evaluates everything
		std::uint32_t walkSerial = 0;
		/**
		 * @brief The per-frame entries with their addresses, trusted only while trackedLayout (bumped when `tracked` may move its values)
		 * equals perFrameLayout.
		 */
		struct PerFrameItem
		{
			RE::BSGeometry* geometry = nullptr;
			Tracked* tracked = nullptr;
		};
		std::vector<PerFrameItem> perFrameSet;
		std::uint64_t trackedLayout = 0;
		std::uint64_t perFrameLayout = ~0ull;
		std::uint32_t validatedFrame = ~0u;  // the pass that ran the validation slice: once a pass
		std::vector<RE::BSGeometry*> pendingEvaluation;
		// Eligible records the walk wrote this pass, bound by the joins and kept as residents until rewritten or released.
		std::vector<std::uint32_t> bindQueue;
		std::vector<std::uint8_t> materialMember;  // per material slot: a resident was bound to it (diagnostic)
		std::uint32_t membershipWitness = ~0u;  // PrimaryCull::MembershipWitness at the residents' binding
		std::vector<std::uint32_t> refreshedGeometry;  // geometry slots re-resolved in place this walk
		// Slot liveness, counted from the logs: each object slot's references (slot, generation) and each geometry slot's next skin
		// partition. A geometry slot goes stale by event; objects drawing it are rewritten (FinishDeltaWalk).
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
		/** @brief The slot reference counts, from the change and geometry logs since the last call. */
		void UpdateSlotReferences();
		/** @brief Drops a geometry slot's contents: its reference on the next partition, map entry and buffer leases. */
		void ClearGeometrySlot(std::uint32_t a_slot);
		/** @brief ClearGeometrySlot, then back to the free list. */
		void FreeGeometrySlot(std::uint32_t a_slot);
		/**
		 * @brief Drops a material slot's contents (map entry, dependents, owner) and lists it retired. The slot table's state is the
		 * caller's.
		 */
		void ClearMaterialSlot(std::uint32_t a_slot);
		std::vector<std::uint32_t> freedGeometry;
		std::vector<std::uint32_t> staleGeometrySlots;
		bool geometryResolvedLastWalk = false;
		// CheckObjectSlots runs only after a slot was freed (or under parity): only then can a bound object reference a dead slot.
		bool slotsFreedThisFrame = true;
		// The decals the last OrderDecals ordered, whose ordinals the next one resets.
		std::vector<std::uint32_t> decalOrdered;
		// ApplyFadeRootsOwned's set (kFadeRootOwned), by node: whether each is stood in (kFadeRootStoodIn).
		ankerl::unordered_dense::map<const void*, bool> fadeRootOwned;
		/**
		 * @brief Marks a root's row owned or not; a newly owned root is reseeded from its node. A change of stood-in state goes to the fade
		 * watch: its dependents' shadow verdicts read its fade from elsewhere.
		 */
		void MarkFadeRootOwned(const void* a_node, bool a_owned, bool a_standIn);
		// The structural events (SceneEvents) ApplyEvents applies: properties by key, nodes held.
		std::vector<const void*> propertyChanged;
		std::vector<RE::NiPointer<RE::NiAVObject>> nodeChanged;
		/**
		 * @brief Drains each engine queue up to where it stood (tracker stack, fades, property, node and switch events, LOD segments) into
		 * the pass's batch (under a load screen: the carry), with the mirror probe; posts the switch catch-ups.
		 */
		std::shared_ptr<EventBatch> CollectEvents();
		/**
		 * @brief Applies the load markers (ApplyLoading), then a_batch: category refresh, attaches, detaches, validation slice, structural
		 * events.
		 */
		void ApplyEvents(std::shared_ptr<EventBatch> a_batch);
		/** @brief Drops what a load screen invalidated (NoteLoadingScreen's marker). */
		void ApplyLoading();
		void ApplyBatch(std::shared_ptr<EventBatch> a_batch);
		/**
		 * @brief The scene pump: passesFull (SetScenePassMode, render thread); the oldest unpublished stamp and the last publication's key
		 * (unchanged passes publish nothing); report samples.
		 */
		std::atomic<bool> passesFull{ false };
		std::uint64_t unpublishedNs = 0;
		std::uint32_t unpublishedFrame = 0;
		using PublishKey = DCLF::PublishKey;
		PublishKey publishedKey;
		bool publishedKeyValid = false;
		bool publishForced = false;  // an input TakeCoordinatorInputs took publishes
		/** @brief What a publication would be made from now. */
		PublishKey CurrentPublishKey() const;
		/** @brief Whether the next publication would carry deltas. */
		bool DeltasPending() const;
		/** @brief Whether anything changed since the last publication (else none is made). */
		bool PublicationNeeded();
		void NotePublished();
		struct PumpStats
		{
			std::vector<double> passMs;                // each pass's whole time
			std::vector<std::uint32_t> passEvents;     // tracker events and queue entries drained
			std::vector<double> publishMs;             // oldest unpublished event to its publication
			std::vector<std::uint32_t> publishFrames;  // the same in render-thread frames
			std::uint64_t passes = 0, eventsOnly = 0, loading = 0, unchanged = 0, published = 0, inlinePasses = 0, failed = 0;
			// Passes by what woke them (each source it took; last: none, an inline pass or a burst's continuation) and those that published
			// nothing; walks that took the per-frame entries (once a frame) and those that did not.
			std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count) + 1> bySource{}, unchangedBySource{};
			std::uint64_t perFrameWalks = 0, eventWalks = 0;
			double fullMs = 0.0, fullMaxMs = 0.0;  // whole passes (not events alone)
			// The sceneFrames seen since the report (first, last), frames with a pass, the most passes in one frame.
			std::uint32_t firstFrame = 0, lastFrame = 0, framesWithPass = 0, passesThisFrame = 0, maxPassesInFrame = 0;
			bool any = false;
		} pumpStats;
		/** @brief The pump's drain (the lane or RunScenePassInline): marks the thread, binds the frame's globals, runs RunSceneWork. */
		void ScenePass(bool a_inline);
		// Engine references the scene work lets go of go through the retirement chain to EngineReleases (released at Present), since a kept
		// publication may still name them.
		std::vector<RE::NiPointer<RE::NiAVObject>> treeOwners, fadeRootOwners;
		template <class T>
		void HandBack(RE::NiPointer<T>&& a_reference)
		{
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
		// Drained switch events, one per node: the index before its oldest event and whether a child changed; applied by the next walk.
		struct SwitchPending
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::int32_t before = -1;
			bool structural = false;
		};
		std::vector<SwitchPending> switchPending;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchPendingIndex;
		// Switches the walks applied, for PrimaryCull via the deltas (FrameView::TakeSwitchChanges); switchResync: it must read them all.
		std::vector<const RE::NiAVObject*> switchesApplied;
		bool switchResync = true;
		// Resident slots, each one's list position, and its patch.
		std::vector<std::uint32_t> residents;
		std::vector<std::uint32_t> residentPos;
		std::vector<ResidentPatch> residentPatches;
		// What the residents keep (KeepResidentsAlive), kept by changes: only slots NoteResidentTouched names are recounted; rebuilt whole
		// when every residency ended or the tables reset.
		bool residentMaintenanceDirty = true;
		void NoteResidentTouched(std::uint32_t a_slot) { residentsTouched.push_back(a_slot); }
		/** @brief Slots by key (a pipeline, a fade root, the trees), each in one list at a time: swap-removed, the first kept. */
		struct SlotMembers
		{
			std::vector<std::vector<std::uint32_t>> lists;
			std::vector<std::uint32_t> position;
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
		std::vector<ResidentCounted> residentCounted;
		std::vector<std::uint32_t> residentsTouched;   // since the last KeepResidentsAlive
		SlotMembers pipelineMembers, fadeRootMembers, treeMembers;  // trees under key 0
		std::vector<std::uint32_t> materialMembers;    // by material slot: resident count
		// Pipelines and materials the joins marked used this pass, cleared again where no resident holds them.
		std::vector<std::uint32_t> joinMarkedPipelines, joinMarkedMaterials;
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentJoining;  // this pass's resident passes, until patched
		ankerl::unordered_dense::set<const RE::BSGeometry*> residentLayerJoining;  // the layers'
		ResidentStats residentStats;
		// The DCLF set (CommitSet): bound slots waiting for readiness and the commit queue. The commit reads the change log from its own
		// cursor, and the waiting slots again when a readiness source moved.
		std::vector<std::uint32_t> setWaiting;
		std::vector<std::uint8_t> setWaitingMark;  // per slot: in setWaiting
		std::vector<std::uint32_t> setQueue;
		// Joined the main phase at the last commit and take part in the reflection's: re-evaluated at this one.
		std::vector<std::uint8_t> setQueueMark;    // per slot: in setQueue
		std::vector<std::uint8_t> setLacking;      // per slot: occluder and reflection phases it takes part in but is not a member of
		std::array<std::uint32_t, 3> setLackingCount{};  // per occlusion map, then the reflection
		std::vector<std::uint8_t> setPartialMark;  // per slot: out of the set with some phases ready
		std::uint32_t setPartialCount = 0;
		// The last commit's decision for the next ApplySet: phases, lacking phases, and the changed slots with their geometry (a slot
		// holding another geometry by then takes nothing).
		std::vector<std::uint8_t> setPhasesNext, setLackingNext;
		std::vector<std::pair<std::uint32_t, const RE::BSGeometry*>> setApply;
		// Per slot: the geometry the last deciding commit decided for.
		std::vector<const RE::BSGeometry*> setGeometryNext;
		using GateFlip = DCLF::GateFlip;
		using PublicationDeltas = DCLF::PublicationDeltas;

		/**
		 * @brief The LOD gates, coordinator side (Engine/Cull/LodGates.h; dclf-async-publication.md). A gate tags the geometries under its
		 * roots (ApplyGateEvent); the commit withholds its incoming slots (GateWithhold) until all are whole members or ineligible and
		 * mirrored, then flips it (UpdateGates): held phases applied, outgoing blocks held out until their detach, the flip published. It
		 * retires with its outcome (ApplyGateOutcomes).
		 */
		static constexpr std::uint8_t kGateIncoming = 1, kGateOutgoing = 2;
		struct LodGate
		{
			enum class State : std::uint8_t
			{
				Open,     // the incoming withheld, the outgoing members
				Flipped,  // the incoming applied, the outgoing held out (Tracked::gateHeldOut), awaiting the outcome
			};
			State state = State::Open;
			std::vector<const void*> incoming, outgoing;  // the roots (keys), less those detached since
			ankerl::unordered_dense::set<RE::BSGeometry*> incomingGeometries, outgoingGeometries;  // the tracked ones tagged
			std::uint32_t pending = 0;     // incoming geometries neither whole members nor ineligible
			std::uint32_t unmirrored = 0;  // incoming roots or nodes under them without a mirror record
			std::uint64_t stampNs = 0;     // its event's push, steady clock
			std::uint32_t stampFrame = 0;
			std::uint64_t flipNs = 0;
			std::uint32_t flipFrame = 0;
		};
		struct GateSlot
		{
			const RE::BSGeometry* geometry = nullptr;  // the gated geometry the commit last evaluated
			std::uint8_t phases = 0;                   // its phases then (held while the gate is open)
			bool ready = false;                        // all its phases, not waiting
		};
		struct GateOutcome
		{
			std::uint64_t token = 0;
			bool forced = false;
			std::array<const void*, 2> kept{};  // a held block the engine reused (LodGates::Outcome::kept), by its roots
		};
		struct GateStats
		{
			std::uint64_t opened = 0, flipped = 0, retiredFlipped = 0, retiredForced = 0, forcedOpen = 0, unknownOutcomes = 0, superseded = 0;
			std::uint64_t rootsDetached = 0, taggedIncoming = 0, taggedOutgoing = 0, heldOut = 0, emptyIncoming = 0;
			// Kept notices (ApplyGateKept): taken, geometries released from hold-out, outgoing tags dropped before a flip.
			std::uint64_t keptNotices = 0, keptCleared = 0, keptUntagged = 0;
			// Flips deferred for an incoming record the next publication claims first.
			std::uint64_t structureDeferred = 0;
			std::vector<double> requestToFlipMs;
			std::vector<std::uint32_t> requestToFlipFrames;
		};
		ankerl::unordered_dense::map<std::uint64_t, LodGate> lodGates;
		struct GateRoot
		{
			std::uint64_t token = 0;
			std::uint8_t side = 0;  // kGateIncoming, kGateOutgoing
		};
		ankerl::unordered_dense::map<const void*, GateRoot> gateOfRoot;
		std::vector<GateSlot> gateSlots;                                     // by object slot
		std::vector<GateFlip> gateFlipsMade;                                 // since the last publication
		std::vector<GateOutcome> gateOutcomesHeld;                           // taken, not applied yet
		std::uint32_t gateHeldOutCount = 0;                                  // entries with Tracked::gateHeldOut
		// Slots a gate's tag or retirement wants evaluated again, queued at the next commit's start.
		std::vector<std::uint32_t> gateQueued;
		GateStats gateStats;
		/** @brief A Gate event (ApplyMirrorEvents, in batch order) opens its gate: token, roots (keys), push stamp. */
		void ApplyGateEvent(std::uint64_t a_token, std::span<const void* const> a_incoming, std::span<const void* const> a_outgoing, std::uint64_t a_stampNs,
			std::uint32_t a_stampFrame);
		/** @brief A detach in the world (ApplyMirrorEvents): gate roots among a_root and a_nodes leave their gates. */
		void NoteGateRootsDetached(const void* a_root, std::span<const void* const> a_nodes);
		/** @brief Tags the tracked geometries under a_root on a_token's a_side; true when the mirror lacked a record there. */
		bool TagGateRoot(std::uint64_t a_token, const void* a_root, std::uint8_t a_side);
		/** @brief AddGeometry: tags a_geometry when a gate's root is on its mirror chain. */
		void TagGateAbove(RE::BSGeometry* a_geometry, Tracked& a_entry);
		void TagGate(RE::BSGeometry* a_geometry, Tracked& a_entry, std::uint64_t a_token, std::uint8_t a_side);
		/** @brief Off its gate (EraseTracked, a retag, the outcome) and no longer held out. */
		void UntagGate(RE::BSGeometry* a_geometry, Tracked& a_entry);
		/** @brief The rescan: gate tags dropped with the entries (its adds tag again). */
		void ResetGateTags();
		/** @brief CommitSet: a_slot's phases as applied (0 for an open gate's incoming slot or one held out), recorded per slot. */
		std::uint8_t GateWithhold(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live);
		/** @brief GateWithhold's verdict alone (the set parity's). */
		bool GateWithholds(std::uint32_t a_slot) const;
		/** @brief Whether a gated incoming geometry is ready: ineligible by a verdict of the gate's time, or every slot of it whole. */
		bool GateGeometryReady(const RE::BSGeometry* a_geometry, const Tracked& a_entry) const;
		/**
		 * @brief CommitSet after its queue: flips the open gates that are ready (roots mirrored, incoming ready, a live scene) into a_apply
		 * and gateFlipsMade.
		 */
		void UpdateGates(bool a_live, std::vector<std::pair<std::uint32_t, std::uint8_t>>& a_apply);
		/** @brief Before CollectEvents: holds LodGates' outcomes posted so far. */
		void TakeGateOutcomes();
		/**
		 * @brief After the batch, unless a_applied is false (a load screen's pass): retires each held outcome's gate (a forced gate's
		 * incoming reclassified).
		 */
		void ApplyGateOutcomes(bool a_applied);
		void RetireGate(std::uint64_t a_token, bool a_forced);
		/**
		 * @brief The engine reused a_token's held block under a_roots: those roots and their geometries leave the gate's outgoing side and
		 * hold-out (recommitted via gateQueued).
		 */
		void ApplyGateKept(std::uint64_t a_token, const std::array<const void*, 2>& a_roots);
		/**
		 * @brief The pass's publication: ApplySet, RevokeUndrawnClaims, PublishTables, then the deltas onto the log before the publication
		 * goes to the snapshot builder, so whoever adopts it finds its node.
		 */
		void PublishScene();
		/** @brief The frame's products since the last publication, moved into a_deltas. */
		void TakeDeltas(PublicationDeltas& a_deltas);
		/** @brief Drops the publications the render thread gave back (Channels::retiredPublications). */
		void DropReturnedPublications();
		std::uint64_t publicationSequence = 0;
		std::uint32_t publicationCommitFrame = 0;    // the commit the next publication applies
		std::uint32_t publicationCommitToggles = 0;  // the toggles generation that commit was made under
		std::shared_ptr<const SetSnapshot> publicationClaims;
		std::vector<const RE::BSGeometry*> publicationJoined, publicationLeft;
		std::vector<std::uint32_t> setApplyMark;  // per slot: its index in setApply + 1, 0 when absent
		std::uint32_t setCommitFrame = 0;
		std::uint32_t setCommitToggles = 0;  // the toggles generation the last commit was made under
		// The claims as applied (a freed slot clears Tables::setPhases): what RevokeUndrawnClaims checks against, with each base slot's
		// geometry.
		std::vector<std::uint8_t> setPhasesApplied;
		std::vector<const RE::BSGeometry*> setGeometryApplied;
		LogCursor revokeCursor;
		std::uint64_t revokedGeometries = 0, revokedMain = 0;
		// Slots whose structure (kStructureCauses) changed since the selected revision's join: a claim among them is revoked at the join
		// (its shapes predate the change) until a later revision is selected.
		std::vector<std::uint32_t> structureChanged;
		std::uint64_t revokedStructureGeometries = 0;
		/** @brief The structural changes revokeCursor has not read, into structureChanged. */
		void NoteStructureChanges();
		/**
		 * @brief After ApplySet: revokes claimed phases whose record the work stopped drawing (from the change log), so the engine draws
		 * them.
		 */
		void RevokeUndrawnClaims();
		bool holdLostMembers = false;  // from the joins on: members RunSceneWork drops are held for PrimaryCull
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> materialRequested;  // asked, not answered yet
		/**
		 * @brief What the coordinator let go of that a published table version may still name. Each publication holds the chain node opened
		 * with it; once none up to it lives, RecycleRetired frees the slots and sends engine references to EngineReleases (released at
		 * Present).
		 */
		struct RetiredBatch
		{
			std::vector<Tables::RetiredSlot> slots;
			std::vector<std::shared_ptr<const void>> imports;
			std::vector<MaterialReference> materials;
			std::vector<RE::NiPointer<RE::NiRefObject>> references;
			std::vector<std::shared_ptr<EventBatch>> events;  // tracker events hold detached subtrees
			bool Empty() const { return slots.empty() && imports.empty() && materials.empty() && references.empty() && events.empty(); }
		};
		RetirementChain<RetiredBatch> retirement;
		struct RetirementStats
		{
			std::uint64_t retired = 0, recycled = 0, dropped = 0, batches = 0;
		} retirementStats;
		/** @brief The tables' retiring slots into the chain's open node (before a publication). */
		void RetireIntoChain();
		/** @brief Each pass's start: batches no publication names any more, back on the free lists. */
		void RecycleRetired();
		std::vector<std::uint32_t> bindRetry;                // joins that waited for a material, bound by the next joins

		// What a scene pass holds for the render thread while it runs.
		bool inSceneTask = false;       // on the lane, not inline
		bool holdPrimaryNotes = false;  // PrimaryCull's notes held (primaryNotes)
		std::vector<const RE::BSGeometry*> setGeometry;  // per slot: the geometry a base member is published under
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> setMemberSlot;  // published geometry -> its slot
		LogCursor setCursor;
		// Readiness sources a waiting slot may wait on (SetWaitCause bits) and their stamps at the last check.
		static constexpr std::uint32_t kWaitSources = 4;
		static constexpr std::uint8_t kWaitLookups = 1, kWaitShadow = 2, kWaitConstants = 4, kWaitFit = 8;
		static constexpr std::uint8_t kWaitOther = 16;  // any source moving retakes it (geometry, decal slot, reflection, layer partner)
		/** @brief The readiness source that releases a waiting reason (SetStats::waitingBy's index). */
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
		// Forward views' pipelines ready in the catalog (reason 8): no source above moves with them, so a count change retakes the
		// kWaitOther slots.
		std::uint64_t setForwardReadiness = ~0ull;
		std::vector<std::uint8_t> setWaitCause;  // per slot: what it waits on
		std::vector<std::uint8_t> setWaitWhy;    // per slot: the first reason of its last wait
		std::uint64_t setShadowModes = ~0ull;      // the shadow modes and states caster readiness was last taken under
		std::uint32_t setPhaseMask = ~0u;          // the phases DCLF draws at the last commit
		std::shared_ptr<SetSnapshot> setBuilding;  // the next publication, kept current by each commit
		std::shared_ptr<const SetSnapshot> setSnapshot;
		bool setSnapshotDirty = true;
		SetStats setStats;
		void QueueSet(std::uint32_t a_slot);
		/**
		 * @brief Takes a slot out of the applied set's phases after its commit (rebound by the joins, a claim revoked): out of the commit's
		 * decision and the claims snapshot, queued for the next commit and marked for ApplySet. Otherwise the commit's copy keeps the
		 * phases and nobody draws the record. A base and its layer leave the main phases together.
		 */
		void LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner = true);
		/** @brief Marks a slot whose phases changed for the next ApplySet, with its current geometry. */
		void MarkSetApply(std::uint32_t a_slot);
		/** @brief Remakes the claims snapshot when a commit or LeaveSet changed setBuilding. */
		void RefreshSetSnapshot();
		/**
		 * @brief The phases DCLF can draw (toggles, ShadowCapability, ReflectionDrawable): a capability that changes only on a toggle,
		 * load, failure or setup, so the views the engine asks for never change the set.
		 */
		static std::uint32_t SetCapability();
		/** @brief The phases an object takes part in among a_drawn, from its record (no readiness). */
		std::uint8_t SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const;
		/** @brief One slot's decision: phases, whether and why it waits, and the observers' first-person notes. */
		struct SlotDecision
		{
			std::uint8_t phases = 0;  // every phase it takes part in if ready for all, else 0
			bool wait = false;        // takes part in a phase it is not ready for
			bool partial = false;
			std::array<std::uint8_t, 8> whys{};  // waiting reasons in order (waitingBy's index)
			std::uint8_t whyCount = 0;
			bool firstPersonExcluded = false;    // observed runs: a first-person slot its view takes no part in
			bool firstPersonHeld = false;        // a first-person slot held out of its view
			std::uint8_t heldWhyCount = 0;       // the reasons noted before it was held out
			std::uint32_t firstPersonSlotWhy = 0;  // IndirectDraws::FirstPersonSlotReady's cause
		};
		/** @brief What the commit decides for a_slot now. Pure: CommitSet applies the side effects; its observers re-decide every slot. */
		SlotDecision DecideSlot(std::uint32_t a_slot, bool a_live, std::uint32_t a_drawn) const;
		/** @brief Whether everything the main phase draws with is ready; a_why: waitingBy's index when not. */
		bool MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const;
		/** @brief Whether a slot's geometry is a first-person entry's. */
		bool FirstPersonSlot(std::uint32_t a_slot) const;
		// Sun entry nodes something was attached under or detached from since the last walk (keys).
		std::vector<const RE::NiAVObject*> dirtyRoots;
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> propertyDependents;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> rootDependents;
		/**
		 * @brief A reference to every root a dependents list names, taken when first listed, handed back when none does. What the scene
		 * work hands out of a root is a copy: no reference is made from a key, which may name a dead node.
		 */
		ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> rootOwners;
		/** @brief Owns a_root from the batch's pin (a_live: a live walk's pointer) while a dependents list names it. */
		bool OwnRoot(const RE::NiAVObject* a_root, bool a_live);
		void ReleaseRootOwner(const RE::NiAVObject* a_root);
		void ReleaseRootOwners();
		/** @brief The listed root's reference (a copy); null and counted for an unlisted root. */
		RE::NiPointer<RE::NiAVObject> OwnedRoot(const RE::NiAVObject* a_root);
		std::uint64_t unownedRoots = 0;  // since the last report: roots handed out unlisted (a defect)
		// A listed root's reference, and back (once its last dependent leaves, the root is a key that may be gone).
		ankerl::unordered_dense::map<const RE::NiAVObject*, const void*> rootReference;
		ankerl::unordered_dense::map<const void*, const RE::NiAVObject*> referenceRoot;
		std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> buckets{};
		struct DeltaStats
		{
			std::uint32_t walks = 0, full = 0;
			std::uint64_t evaluated = 0, perFrame = 0, pending = 0, property = 0, node = 0, roots = 0, geometryDirty = 0, restored = 0, moved = 0, kept = 0;
			std::uint64_t propertyEvents = 0, nodeEvents = 0;
			std::uint64_t facePublished = 0, faceUpdated = 0, faceWritten = 0;
			std::uint64_t switchEvents = 0, switchChanges = 0, switchCatchUps = 0, switchReclassified = 0, attachCatchUps = 0;
			std::uint64_t live = 0;
			std::uint32_t evaluatedMax = 0;
		} delta;
		/**
		 * @brief A face shape's region of the positions buffer, held while a stream uses it; EndFaceWalk frees released streams' regions.
		 * Walk thread only.
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
		// Tracked face shapes by head, and the heads' publications this walk.
		ankerl::unordered_dense::map<const RE::BSFaceGenNiNode*, std::vector<RE::BSGeometry*>> faceHeads;
		std::vector<const RE::BSFaceGenNiNode*> facePublished, faceStale;
		std::vector<RE::BSGeometry*> skinnedObjects;  // this walk's, in object order
		std::uint32_t AllocateGeometrySlot();
		std::uint32_t AllocatePipelineSlot();
		std::uint32_t AllocateMaterialSlot();
		/** @brief What a geometry slot is resolved from. */
		struct GeometrySource
		{
			const RE::BSGraphics::TriShape* key = nullptr;
			ID3D11Buffer* vertexBuffer = nullptr;
			ID3D11Buffer* indexBuffer = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t vertexCount = 0;
			std::uint32_t indexCount = 0;
			std::uint32_t firstIndex = 0;  // an index range (object LOD's visible ranges); 0: the whole
			bool layer = false;            // a non-TriShape key (Tables::geometryLayerKey): a layer's second index list, particle quads
		};
		/** @brief Prefetches the buffers order[a_first...]'s geometry slots will resolve (GpuResources::Prefetch). */
		void PrefetchGeometryBuffers(std::size_t a_first);
		/**
		 * @brief The geometry slot for a source: found, refreshed in place, or newly resolved.
		 * @return the slot, or Tables::kSlotFree when the buffers cannot be made stable for the graph.
		 */
		std::uint32_t ResolveGeometrySource(const GeometrySource& a_source, PartTimer& a_timer);
		/** @brief The geometry slot of a multi-index shape's second index list, or Tables::kSlotFree. */
		std::uint32_t ResolveLayerGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, PartTimer& a_timer);
		/** @brief The geometry slot of a geometry's TriShape, or of skin partition a_partition. */
		std::uint32_t ResolveGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, const SceneCapture::GeometryRecord::Partition* a_partition, PartTimer& a_timer);
		/**
		 * @brief Object LOD: geometry slots for a partly hidden shape's visible ranges, linked by nextPartition from the returned first
		 * (Tables::kSlotFree if one fails), each keyed by its range's first segment record.
		 */
		std::uint32_t ResolveLodRangeSlots(const SceneCapture::GeometryRecord& a_geometry, const std::vector<LodSegments::Range>& a_ranges, PartTimer& a_timer);

		/** @brief The walk's prologue: reference changes counted, unused materials retired, idle geometry and pipeline slots expired. */
		void SweepSlots();
		/**
		 * @brief Drops the slot tables, their maps and every cached derivation (by generation), so teardown leaves nothing pointing at dead
		 * slots.
		 */
		void ResetSlotTables();

	public:

	private:
		/**
		 * @brief Drops every cached verdict and derivation when a live toggle the classification reads changed (Toggles.h): the caches
		 * witness the object, not the switches.
		 */
		void InvalidateVerdicts();

		/** @brief After the joins: every bound object names live slots of this frame, or is neutralised. */
		void CheckObjectSlots(bool a_resolveBuffers);
		Stats stats;
	};
}
