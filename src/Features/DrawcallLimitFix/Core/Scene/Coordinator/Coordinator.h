#pragma once

// The scene coordinator: the scene-work half of DCLF's persistent scene. It owns the tracked set, the scene mirror, the slot tables
// (SceneTables) and their change log, the membership joins, the DCLF set and the publications the render thread installs. Scene lane only:
// passes run one at a time, woken by producers (SceneWake), fanning pure reads out to the preparation pool; under the render-thread
// parities a pass runs inline. The render thread reaches it only through Scene::Channels and the publications.
// Its state is split by concern into private bases, one per header of State/ (each says which phases use it); the functions
// over it are declared here, grouped the same way.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Api/Reports.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/MaterialBindings.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/SharedBindings.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialSources.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Api/EntryRanges.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneIdentity.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SlotTable.h"
#include "Features/DrawcallLimitFix/Core/Runtime/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Runtime/PublicationLog.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/FrameTables.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Eligibility.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Channels.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/EntrySpaceTable.h"

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
	struct EntrySpace;
	struct PipelineCatalog;
}

// The scene passes' timed parts, and the coordinator's state: one private base per concern (State/), dependencies first.
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Timing.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Tracked.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Mirror.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Dependents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Categories.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Attach.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Candidates.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Placements.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Hidden.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Events.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Shading.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Frame.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Slots.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Materials.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Bindings.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Walk.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Joins.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Residents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Components.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/ShadowSets.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Set.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Pump.h"

namespace DCLF
{
	namespace Scene
	{
		class Coordinator;

		/** @brief Residents.cpp: a root listed with no mirror record (the coordinator's placement statistics count it; FadeRoots.cpp calls it). */
		void NoteFadeRootUnmirrored();
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
	class Scene::Coordinator :
		private CoordinatorTracked,
		private CoordinatorMirror,
		private CoordinatorDependents,
		private CoordinatorCategories,
		private CoordinatorAttach,
		private CoordinatorCandidates,
		private CoordinatorPlacements,
		private CoordinatorHidden,
		private CoordinatorEvents,
		private CoordinatorShading,
		private CoordinatorFrame,
		private CoordinatorSlots,
		private CoordinatorMaterials,
		private CoordinatorBindings,
		private CoordinatorWalk,
		private CoordinatorJoins,
		private CoordinatorResidents,
		private CoordinatorComponents,
		private CoordinatorShadowSets,
		private CoordinatorSet,
		private CoordinatorPublication,
		private CoordinatorPump
	{
		// Diagnostics/Scene and the scene reports read the private state through it.
		friend struct Diagnostics::SceneView;
		// The features' coordinator components reach its services through their context (Components.cpp).
		friend class Api::CoordinatorComponentContext;

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

		// Scene members (dclf-architecture.md, "Eligibility and classification"): a member's accumulated half is patched once from its membership pass and kept across
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
		 * @brief The joins' frame inputs since the last call: ancestor maps and pipeline frames posted and taken, PerGeometry blocks made or
		 * waited.
		 */
		struct AccumulateInputStats
		{
			std::uint64_t ancestorMapsPosted = 0, ancestorMapsTaken = 0, pipelineFramesPosted = 0, pipelineFramesTaken = 0;
			std::uint64_t pipelineBlocksMade = 0, pipelineBlocksWaited = 0;
			std::uint64_t ancestorValuesMoved = 0;
			std::uint64_t ancestorFound = 0, ancestorNoKey = 0, ancestorNotMapped = 0;
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
			// Publications that kept the family (its stamp stood): the table families' by registration (the fade roots among them).
			std::array<std::uint64_t, Extensions::kTableFamilySlots> familiesKept{};
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
		std::shared_ptr<const EntrySpace> CoordinatorSunCandidates() const { return sunCandidates; }
		std::shared_ptr<const EntrySpace> CoordinatorLightCandidates() const { return lightCandidates; }


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
		 * leaves first. Records and Tables::setPhases change only at ApplySet (dclf-architecture.md, "The DCLF set").
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
			std::uint64_t fitRequeued = 0;      // members taken again because the scene buffers' fit moved below them
			std::string firstWaiting;
		};
		SetStats TakeSetStats() { return std::exchange(setStats, {}); }
		/** @brief A report: why a tracked geometry's sun entry rule allows it, or that it is not tracked (looked up by address). */
		std::string DescribeSunCandidate(const void* a_geometry) const;
		/** @brief The fade roots' component (scene lane): a root listed with no mirror record (the placement statistics). */
		void NoteFadeRootUnmirrored() { ++placementStats.fadeRootsUnmirrored; }
		/** @brief Whether the geometry sits under a tracked category node (the tracked map, read on the caller's thread). */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;
		/** @brief The index ranges a tracked entry of a ranged geometry type draws (object LOD: its visible segments' runs), or null. */
		const std::vector<Api::EntryRange>* EntryRangesOf(const RE::BSGeometry* a_shape) const
		{
			const auto it = entryRanges.find(a_shape);
			return it != entryRanges.end() ? &it->second : nullptr;
		}

		/** @brief How a tracked geometry came to be tracked (diagnostics; State/Tracked.h). */
		using TrackSource = CoordinatorTracked::TrackSource;
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
		/** @brief Diagnostics (inline passes): the plan the last walk made, not yet published. */
		const PlacementPlan* PeekPlacementPlan() const { return placementPlanReady.get(); }


		/** @brief A thread running scene work (the lane or a fan-out chunk): GetFrame is the coordinator's there. */
		static inline thread_local bool sceneWorkThread = false;

	private:
		// Declared here, defined in Internal.h and the phases' files (their definitions name them as Coordinator's).
		struct EventBatch;
		struct BatchUnwindGuard;  // a batch back to the render thread on a throw (Internal.h)
		/**
		 * @brief The batch walked once (ApplyMirrorEvents' apply loop) for the later steps: the tracker's attach and detach events (not
		 * mirror-only) in order, whether one was a detach, and the records of the captures and leaf updates in order (both heads).
		 */
		struct BatchIndex;
		struct ShardScope;

		// State whose types are the coordinator's own (public, or declared here): the report counters, the
		// load screen's carry, the material owners and the retirement chain. The rest of the state is in the bases (State/).
		// Pipeline blocks the render thread posted (Channels::pipelineConstantsPosted), applied where the slot still holds the key.
		ConstantsPostStats constantsPostStats;
		TablesPublication tablesPublication;
		ResidentStats residentStats;
		SetStats setStats;
		Stats stats;
		// A load screen's attach and detach events for the mirror (taken by the first pass after the load).
		std::shared_ptr<EventBatch> loadingCarry;
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

		// The mirror's reads and the batch's pins: State/Mirror.h (CoordinatorMirror).
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
		MirrorReadStats& MirrorReads() const { return mirrorReadSink ? *mirrorReadSink : mirrorReads; }
		void RecheckDeferredReads();
		/** @brief FindCategoryNode's chain, the mirror's alone (no counting, no check). */
		RE::NiNode* MirrorCategoryNode(const void* a_object, Ineligible& a_reason, bool& a_unmirrored) const;
		/** @brief The batch's captures copied into mirrorPreps (the pool, or here), for ApplyMirrorEvents' serial apply. */
		void PrepareMirrorCaptures(const std::vector<const SceneCapture::Records*>& a_captures, std::size_t a_records);
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
		/** @brief Pinned's lookup alone (null: no pin), pure: a prepare's read of the batch's frozen pins; FromPin makes the reference. */
		RE::NiRefObject* PinOf(const void* a_key) const
		{
			const auto it = a_key ? batchPins.find(a_key) : batchPins.end();
			return it != batchPins.end() ? it->second : nullptr;
		}
		template <class T>
		RE::NiPointer<T> FromPin(RE::NiRefObject* a_pin)
		{
			if (!a_pin)
				return nullptr;
			++referenceStats.pinned;
			return RE::NiPointer<T>(static_cast<T*>(a_pin));
		}
		/**
		 * @brief The ancestor value's key for an object: its nearest ancestor of the contribution's classes, from the mirror's chain
		 * (Api::AncestorValueContribution::ancestorRtti: Light Limit Fix's GetParentRoomNode). Null without one, or without the contribution.
		 */
		const void* AncestorKeyOf(const void* a_object) const;
		void ApplyMirrorEvents(const EventBatch& a_batch, BatchIndex& a_index);
		// ApplyMirrorEvents' stages: the batch's pins and the captures to prepare (one walk), the apply's work counters, the uses parity.
		void PinMirrorBatch(const EventBatch& a_batch, std::vector<const SceneCapture::Records*>& a_captures, std::size_t& a_records);
		void CountMirrorWork();
		void CheckMirrorRecordUses();

		// The dependents lists and the root owners: State/Dependents.h (CoordinatorDependents).
		/** @brief Lists an evaluated entry under its properties; UnlistDependents takes it off every list. */
		void ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root);
		/** @brief Off its category node's members (EraseTracked; AddGeometry before it lists the entry anew). */
		void UnlistCategoryMember(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief Parity (CheckCategoryMembers): the index against the tracked entries, every member each way; logs the first difference. */
		void CheckCategoryMembers(const char* a_where);
		/** @brief Owns a_root from the batch's pin (a_live: a live walk's pointer) while a dependents list names it. */
		bool OwnRoot(const RE::NiAVObject* a_root, bool a_live);
		void ReleaseRootOwner(const RE::NiAVObject* a_root);
		void ReleaseRootOwners();
		/** @brief The listed root's reference (a copy); null and counted for an unlisted root. */
		RE::NiPointer<RE::NiAVObject> OwnedRoot(const RE::NiAVObject* a_root);

		// The category nodes and the CategoryRoot views' roots: State/Categories.h (CoordinatorCategories).
		/** @brief Diffs the category nodes and portal roots against the taken capture when it is new or a_force (the post-load rescan). */
		void RefreshCategoryNodes(bool a_force = false);
		// RefreshCategoryNodes' stages: the entries of the cells and portal roots that went away erased; the new ones' subtrees tracked.
		void DropRemovedCategories(const CategoryCapture& a_capture, bool a_removedRoot);
		void TrackAppearedCategories(const CategoryCapture& a_capture, const std::vector<RE::NiNode*>& a_added,
			const std::vector<RE::NiAVObject*>& a_addedRoots, std::uint8_t a_cause);
		/** @brief First in the pass, before the drains: takes the newest category capture posted. */
		void TakeCategoryCapture();
		/**
		 * @brief The category node a_object hangs under (null: none) and the strongest parent reason, from the mirror's chain; the pointer
		 * is a key. CS_DCLF_MIRROR_PARITY checks it against the live chain.
		 */
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored = nullptr) const;
		/** @brief The same from the live chain (the parities and diagnostics; the object alive). */
		RE::NiNode* FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const;
		/** @brief The CategoryRoot view whose root a_node is (EngineView::Count: none, or a null a_node). */
		Api::EngineView ViewRootOf(const void* a_node) const
		{
			for (std::size_t v = 0; v < viewRoots.size(); ++v)
				if (a_node && a_node == viewRoots[v])
					return static_cast<Api::EngineView>(v);
			return Api::EngineView::Count;
		}
		/** @brief Whether a node is a CategoryRoot view's root (the first-person root). */
		bool ViewRoot(const void* a_node) const { return ViewRootOf(a_node) != Api::EngineView::Count; }
		/** @brief Whether a tracked entry is a CategoryRoot view's (its category node the view's root: a first-person entry). */
		bool ViewRootEntry(const RE::NiNode* a_categoryNode) const { return ViewRoot(a_categoryNode); }
		/** @brief The first CategoryRoot view's root this pass has (null: none): the diagnostics' "the first-person root". */
		const RE::NiNode* FirstViewRoot() const
		{
			for (const auto* root : viewRoots)
				if (root)
					return root;
			return nullptr;
		}
		/** @brief Whether a node is one of the coordinator's category nodes (the frame reads IsCategoryNode). */
		bool CategoryNodeOwn(const RE::NiAVObject* a_node) const
		{
			return categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}

		// Attach and detach: State/Attach.h (CoordinatorAttach).
		/**
		 * @brief Tracks the geometries under a_root. A root with no mirror chain is captured by the render thread at the next frame start
		 * and retried once (pendingSubtrees); an attach event's root without one was out of the world (its world attach brings its own
		 * event).
		 */
		void AddSubtree(RE::NiAVObject* a_root, SubtreeSource a_source = SubtreeSource::AttachEvent);
		/**
		 * @brief Tracks a_geometry under a_categoryNode from the batch's pin or (a_live) a live pointer; with neither it is left out
		 * (counted).
		 */
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live, GeometryPrep& a_prep);
		HeldKeys HeldKeysOf(const void* a_geometry) const;
		/** @brief a_root's prepare: FindCategoryNode, MirrorSubtree (and its mirror-read parity check), each geometry's. Const. */
		void PrepareSubtree(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const;
		/** @brief PrepareSubtree's walk alone: the category node and the geometries found (geometry and reason), not their prepares. Const. */
		void PrepareSubtreeWalk(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const;
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
		/** @brief The attach burst's report line (AttachStats), the counters reset; empty when nothing happened. */
		std::string TakeAttachLine();
		/** @brief Holds a_entry's properties anew from the batch's pins, as the mirror's geometry record names them. */
		void HoldProperties(Tracked& a_entry) { HoldProperties(a_entry, HeldKeysOf(a_entry.geometry.get())); }
		/** @brief HoldProperties from a_keys (HeldKeysOf's, maybe a prepare's): stores only. */
		void HoldProperties(Tracked& a_entry, const HeldKeys& a_keys);
		void ValidateSlice();
		void EraseTracked(RE::BSGeometry* a_geometry);

		// The sun and light candidates: State/Candidates.h (CoordinatorCandidates).
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
		void MarkListedRootDirty(const RE::NiAVObject* a_entry)
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

		// Placements, moves and root motion: State/Placements.h (CoordinatorPlacements).
		/** @brief A listed root's dependent may have changed (its root planned): compared at the next plan. */
		void TouchPlacedDependent(const RE::NiAVObject* a_root, const void* a_geometry)
		{
			if (a_root && plannedRoots.contains(a_root))
				placementDependentsTouched.emplace_back(a_root, a_geometry);
		}
		/** @brief A mover's planned key now (none: not in the set); a_prune drops a stale member from moverSet (never the parity's). */
		std::optional<PlannedKey> PlannedMoverNow(RE::BSGeometry* a_geometry, bool a_prune);
		/** @brief Whether a root is listed now (moving or still, with dependents and an owner); a_prune forgets a still root that is not. */
		bool PlannedRootNow(const RE::NiAVObject* a_root, bool a_prune);
		/** @brief A listed root's dependent's planned key now (none: not listed under it, or no record). */
		std::optional<PlannedKey> PlannedDependentNow(const RE::NiAVObject* a_root, const void* a_geometry) const;
		void AddPlannedSlots(const PlannedKey& a_key, const PlannedOwner& a_owner);
		void RemovePlannedSlots(const PlannedKey& a_key, const PlannedOwner& a_owner);
		/** @brief Into a_plan: what the touched keys changed against the planned set (a_reset: the whole set, the planned one cleared). */
		void DiffPlacementSet(PlacementPlan& a_plan, bool a_reset);
		/** @brief Parity: the set rebuilt whole from the coordinator's state, sorted (PlacementPlan::Check). */
		PlacementPlan::Check RebuildPlacementSet();
		/** @brief The placement deltas' report line, once its window turned (PublishPlacementPlan; CS_DCLF_STATS or a parity finding). */
		void ReportPlacementDeltas();
		/** @brief A node whose controllers or rigid body changed: every entry under it reclassified, every root above it scheduled. A key. */
		void ApplyMotionUpdate(const void* a_node);
		/** @brief Whether an entry is a recorded per-frame mover (moverSet's: every plan lists it, FrameValues samples it every frame). */
		bool IsPlacedMover(const Tracked& a_tracked) const;
		/**
		 * @brief After an entry's merge: it joins or leaves moverSet as IsPlacedMover says; true when that changed the set. a_written: a
		 * write that may have changed its plan key (slot, layer, palette block), so a member is compared at the next plan.
		 */
		bool NoteMover(RE::BSGeometry* a_geometry, const Tracked& a_tracked, bool a_written = true);
		/** @brief Before the plan: the change log's entries naming planned slots, and the tables' sizes against the last plan's. */
		void SyncPlacementSet();
		/**
		 * @brief Whether a per-frame walk takes a per-frame entry (the light path's gate): an event names it (one of its move keys or its
		 * geometry moved this frame or the last; a hidden event is owed), or it has none to wait on (no light path; a first-person actor's).
		 * a_why: 1 moved, 2 hidden event owed, 3 first person, 4 no light path.
		 */
		bool MoverDue(const Tracked& a_tracked, const RE::BSGeometry* a_geometry, std::uint32_t& a_why) const;
		/** @brief The mover gate's report line, once its window turned (ScenePass). */
		void ReportMoverGate();
		/** @brief The still roots this frame's move keys name (movedKeys[0] from movedKeysQueued on), into stillRoots. */
		void QueueRoots();
		/** @brief The walk's plan item for a recorded entry (slot, layer row, sun entry, palette block). */
		PlacementPlan::Item PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief PlanItemOf's key, no reference taken. */
		PlannedKey PlacedKeyOf(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief The walk's plan, opened by its first write (or by PublishPlacementPlan when the placement set changed). */
		PlacementPlan& WalkPlan();
		/** @brief After the walk: the still roots it lists (QueueRoots), and a plan made ready when the placement set changed or a slot was written. */
		void PublishPlacementPlan();
		void DrainMoveEvents(bool a_full);
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
		/**
		 * @brief Whether a placement event can change the entry's tables: it has a record or a frame verdict (None, Hidden, Switch, Actor).
		 */
		static bool PlacementMatters(const Tracked& a_tracked);
		/**
		 * @brief Reclassifies a sun entry node's dependents and forgets its motion; a key, never dereferenced. a_whole false (a dirty root:
		 * a dependent attached or detached, the attached one already pending): its motion is read again, and only a changed verdict
		 * reclassifies the dependents (the parity's witness passes schedule the rest and check them, rootWitness).
		 */
		void ScheduleRoot(const RE::NiAVObject* a_root, bool a_whole = true);
		/** @brief A node Havok moved or gave a controller: the entries under it and the dependents of every node above it. */
		void ApplyNodeEvent(RE::NiAVObject* a_node);
		/** @brief MoveKeyOf, else its fallbacks (Tracked::moveKey, moveKeyOuter): the innermost and outermost references, or the category node. */
		std::pair<const void*, const void*> MoveKeysOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const;

		// The hidden events: State/Hidden.h (CoordinatorHidden).
		void ListHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void DrainHiddenEvents();

		// The structural events, switches and the entries' drawn ranges: State/Events.h (CoordinatorEvents).
		/** @brief Whether the switch's record selects a_child, and is current. */
		static bool SwitchSelects(const SceneCapture::NodeRecord& a_switch, const void* a_child)
		{
			return (a_switch.kind & SceneCapture::kKindSwitch) && a_switch.switchChild && a_switch.switchChild == a_child && a_switch.switchCurrent;
		}
		/**
		 * @brief Before the first round: drained switch events whose selection changed, each caught up (CatchUpSwitch) and, in a delta
		 * walk, its entries reclassified.
		 */
		void ApplySwitchEvents(bool a_full);
		/**
		 * @brief Drains each engine queue up to where it stood (tracker stack, fades, property, node and switch events, the features' engine
		 * channels) into the pass's batch (under a load screen: the carry), with the mirror probe; posts the switch catch-ups.
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
		// ApplyBatch's stages: the post-load rescan's clear, the category refresh and retries, the attach/detach merge (its detaches
		// erased last), the leaf updates' property holds.
		void ClearForRescan();
		void RefreshBatchCategories(const BatchIndex& a_index, bool a_rescanned);
		void ApplyAttachDetach(const BatchIndex& a_index);
		void ApplyDetaches(const std::vector<RE::BSGeometry*>& a_detached);
		void HoldLeafProperties(const BatchIndex& a_index);

		// The shading events: State/Shading.h (CoordinatorShading).
		/** @brief A slot's ShadingItem into shadingNamed (none for a free, unbound or propertyless slot). */
		void NameShading(std::uint32_t a_slot, bool a_member);
		/**
		 * @brief The walk: drains the shading events and names their dependents (their extras' static rows rewritten); every slot while the
		 * events are not installed.
		 */
		void NameShadingEvents(std::vector<std::uint32_t>* a_slots = nullptr);
		/** @brief The dependents of shading event keys (properties, emittance colours) named; their slots into a_slots when given. */
		void NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots);

		// The frame inputs and the technique rows: State/Frame.h (CoordinatorFrame).
		/** @brief The scene pass, before the walk: the frame inputs posted since its last pass (ancestor map, pipeline frame). */
		void TakeAccumulateInputs();
		/**
		 * @brief A new ancestor map taken: every tracked object's ancestor value looked up again and written where it moved. The joins look
		 * it up only when they evaluate an object, so one bound before the map named its key (Light Limit Fix's rooms fill as lights
		 * reach them) would keep the none value.
		 */
		void RefreshAncestorValues();
		/** @brief The scene pass, after the joins: a used pipeline without a current PerGeometry block made from the pipeline frame. */
		void MakeNewPipelineConstants();
		/** @brief First in each pass: the newest frame inputs taken (sceneInputs, sceneFrame); the verdicts dropped when asked. */
		void TakeFrameInputs();
		void ApplyConstantsPosts();
		/** @brief Each full pass before the walk: every technique row evaluated again when the inputs differ from the witness. */
		void RefreshTechniqueRows();
		/** @brief A technique row from its frame's sample, written and versioned where it differs. */
		void EvaluateTechniqueRow(std::uint32_t a_row);
		/** @brief The technique row of a pass descriptor's TechniqueKey, made and evaluated when new. */
		std::uint32_t TechniqueRowFor(std::uint32_t a_passDescriptor);

		// The slot tables: State/Slots.h (CoordinatorSlots).
		void InvalidateObjectIndices() { ++objectStamp; }
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
		void ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		void UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
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
		void IndexObjectHead(std::uint32_t a_object, std::uint32_t a_head);
		void IndexLink(std::uint32_t a_from, std::uint32_t a_to);
		/** @brief The object slots the index lists under a_slots (a_chains: and under every head whose chain reaches one), sorted, unchecked. */
		void ObjectsUnder(const std::vector<std::uint32_t>& a_slots, bool a_chains, std::vector<std::uint32_t>& a_out) const;
		std::uint32_t AllocateGeometrySlot();
		std::uint32_t AllocatePipelineSlot();
		std::uint32_t AllocateMaterialSlot();
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
		 * @brief A ranged entry (object LOD: a partly hidden shape): geometry slots for its drawn ranges, linked by nextPartition from the
		 * returned first (Tables::kSlotFree if one fails), each keyed by its range's key (object LOD: its first segment record).
		 */
		std::uint32_t ResolveEntryRangeSlots(const SceneCapture::GeometryRecord& a_geometry, const std::vector<Api::EntryRange>& a_ranges, PartTimer& a_timer);

		/** @brief The walk's prologue: reference changes counted, unused materials retired, idle geometry and pipeline slots expired. */
		void SweepSlots();
		/**
		 * @brief Drops the slot tables, their maps and every cached derivation (by generation), so teardown leaves nothing pointing at dead
		 * slots.
		 */
		void ResetSlotTables();
		/** @brief After the joins: every bound object names live slots of this frame, or is neutralised. */
		void CheckObjectSlots(bool a_resolveBuffers);

		// The material records: State/Materials.h (CoordinatorMaterials).
		/** @brief ApplyEvents: the captures pushed since the last call (the newest kept), and the render thread's answers to requests. */
		void DrainMaterialCaptures();
		/** @brief A capture's reference, to EngineReleases (released at Present). */
		void HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held);
		/**
		 * @brief Keeps the material records from the frame's sources each pass, with no engine read: newly captured materials' slots
		 * evaluated again (RewriteCapturedMaterials), frame-sourced components per signature (RefreshMaterialSignatures). TexcoordOffset is
		 * no record's: a record keeps its capture's, and the frame values patch every material row with the frame's (TextureTransformList).
		 */
		void RefreshMaterialRecords();
		void RewriteCapturedMaterials();
		void RefreshMaterialSignatures();
		/** @brief A slot's record made (a join) or written from a_snapshot: listed under its signature. */
		void NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot);
		/**
		 * @brief Pure (any thread; the pass's sources passed in): a_snapshot's record for a_pass as records hold it (no frame view). For a
		 * rewrite (a_slot != kNoMaterialSlot) the slot's frame parts and unread floats are kept, and `same` tells an unchanged record.
		 */
		void EvaluateMaterial(const MaterialPort::MaterialSnapshot& a_snapshot, std::uint32_t a_pass, const MaterialPort::MaterialFrame& a_sources, std::uint32_t a_slot,
			MaterialEvaluation& a_out) const;
		/**
		 * @brief a_count prepare items by chunks of kFanoutGrain (FanoutMode's minimum count before the pool), each chunk under a ShardScope;
		 * a_body(begin, end) is pure. Whether the pool ran.
		 */
		bool PrepareMaterials(std::string_view a_zone, std::size_t a_count, const std::function<void(std::size_t, std::size_t)>& a_body);
		/** @brief The joins' material evaluations, after their EvaluateJoin and before their merges. */
		void PrepareJoinMaterials(std::size_t a_count);
		/** @brief The tables reset (ResetSlotTables): the signatures' slot lists and the retries dropped. */
		void ResetMaterialRecords();

		// The bindings and the lookups: State/Bindings.h (CoordinatorBindings).
		/** @brief After the records' writers: drains the answers, then resolves the changed (material log) and newly used slots. */
		void UpdateMaterialBindings();
		/** @brief A used slot's entry written from its record; bindings requested for views with none known. */
		void ResolveMaterialBinding(std::uint32_t a_slot);
		/** @brief ResolveMaterialBinding's per-texture binder: one slot's textures bound or cleared, its owner changes collected (MaterialBindings.cpp). */
		struct MaterialTextureBinder;
		/**
		 * @brief An entry's new owner block: a_next's moved owners (bits of a_texturesMoved / a_featuresMoved) with a_held's others, sealed,
		 * or the block another slot of a_material sealed for the same owners; null when none is held.
		 */
		std::shared_ptr<const Lookups::MaterialOwners> SealMaterialOwners(const RE::BSShaderMaterial* a_material, const Lookups::MaterialOwners* a_held,
			Lookups::MaterialOwners& a_next, std::uint32_t a_texturesMoved, std::uint32_t a_featuresMoved);
		/** @brief The scene lane, before a commit or publication: entries written since the last call, each versioned and logged. */
		void VersionMaterialBindings();
		/** @brief A view's binding: answered in this drain, cached from an earlier answer, else asked for (pending; a_slot waits on it). */
		GpuTextures::Binding MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot);
		/** @brief A material slot's retirement came back (no publication names it): its entry dropped. */
		void RetireMaterialBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped; an answer to an earlier request is stale. */
		void ResetMaterialBindings();
		/**
		 * @brief After the technique rows' refresh: answers drained, fixed and projected bindings taken, technique masks resolved, shadow
		 * textures requested or released.
		 */
		void UpdateSharedBindings();
		/** @brief UpdateSharedBindings' answers drained into arrived and the cache; returns the kinds that waited on them. */
		std::uint32_t DrainSharedReplies();
		/** @brief UpdateSharedBindings' fixed bindings (null texture, samplers) taken from GpuTextures::Fixed where they moved. */
		void TakeFixedBindings();
		/** @brief UpdateSharedBindings' technique masks: the change set (or every used one on a sweep) resolved, then the parity. */
		void ResolveMaskBindings();
		/** @brief UpdateSharedBindings' shadow textures the dependency index added (requested) or removed (released). */
		void ApplyShadowTextureChanges(const std::vector<std::pair<ID3D11ShaderResourceView*, bool>>& a_changes);
		/** @brief UpdateSharedBindings' shared entries' binding block, sealed again when one changed. */
		void SealSharedBindingBlock();
		/** @brief A view's binding: answered in this drain, cached (here or in the material bindings), else requested. */
		GpuTextures::Binding SharedViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, SharedBindings::Kind a_kind);
		/** @brief The projected entries from their capture; pending ones requested. */
		void ResolveProjectedBindings(std::uint32_t a_textures);
		/** @brief A used pipeline's technique mask entry from its row. False: it stood (nothing looked up). */
		bool ResolveMaskBinding(std::uint32_t a_slot, ID3D11ShaderResourceView* a_frameMask);
		/** @brief A shadow texture's entry from an answer or a known binding, else requested. */
		void ResolveShadowTextureBinding(ID3D11ShaderResourceView* a_view);
		/** @brief A pipeline slot's lookups retirement came back (no publication names it): its mask entry dropped. */
		void RetireMaskBinding(std::uint32_t a_slot);
		/** @brief The tables reset (ResetSlotTables): every entry and request dropped, except the render thread's posted capture. */
		void ResetSharedBindings();
		/**
		 * @brief Before a commit or publication: material entries versioned, then pipeline and shadow-pipeline entries resolved against the
		 * pipeline lane's newest catalog.
		 */
		void ResolveLookups();
		/** @brief The lookups as an immutable copy with their catalog (the last copy again if unchanged). */
		void PublishLookups();
		/** @brief New lookups (tables reset): generations past the last, versions continuing. */
		void ResetLookups();

		// The walk: State/Walk.h (CoordinatorWalk).
		/**
		 * @brief The BSX flags of the nearest fade node above a_leaf, by the mirror's parents (-1: none, or none read): what an occlusion
		 * map's occluder rule asks (Api::OccluderLeaf::NearestFadeNodeBsx).
		 */
		std::int32_t NearestFadeNodeBsx(const SceneCapture::LeafView& a_leaf) const;
		Ineligible ClassifyFrame(const Tracked& a_tracked) const;
		/**
		 * @brief The face mode (0-5) of the one NiBillboardNode above a Billboard-parented leaf when DCLF turns it on the GPU (its parent
		 * contribution's toggle on: Api::ParentLeaves::TurnedEffectShapes; Billboards.h: a skinless BSEffectShaderProperty BSTriShape, a
		 * known mode); else -1 (it stays Ineligible::Billboard).
		 */
		std::int32_t BillboardModeOf(const Tracked& a_tracked) const;
		/**
		 * @brief A CategoryRoot view's entry's decal (a first-person blood decal, a weapon's BloodLighting) as a forward object drawn
		 * after the view's opaque buckets, while the view's contribution says so (Api::ViewContribution::forwardDecals: the first person's
		 * CS_DCLF_FIRST_PERSON_DECALS), as the engine's accumulator does: no Deferred, its group's blend mode, depth tested, depth written by
		 * the opaque group only (modes 3 and 1), the group's depth bias.
		 */
		void ViewDecalForward(const Tracked& a_tracked, LightingDescriptors& a_descriptors) const;
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
		/**
		 * @brief CS_DCLF_WALK_PARITY's reference: every entry rewritten from scratch into dense tables and classified without caches, the
		 * coordinator left unchanged.
		 */
		void DenseReference(Tables& a_dense, ankerl::unordered_dense::map<const RE::BSGeometry*, Ineligible>& a_reasons);
		/** @brief An object's extras rows' static parts from its property and geometry; true when changed. */
		bool WriteObjectExtras(std::uint32_t a_object);
		void BuildScenePhase();
		/** @brief Walk parity's reference: every entry of `order` written from scratch into dense tables. */
		void DenseWalk();
		/** @brief Clears the per-frame tables and the walk's counters; a_keepIndices: a delta walk's, keeping object indices. */
		void BeginWalk(bool a_keepIndices = false);

		/**
		 * @brief The walk: evaluates only entries whose inputs can have changed (per-frame entries, new or marked ones, dependents of
		 * property, node and attach events, slots whose geometry slot went stale); every other slot keeps its record. A classification
		 * stands until such an event (dclf-architecture.md, "Eligibility and classification"); CS_DCLF_WALK_PARITY checks it against a from-scratch walk.
		 */
		void DeltaWalk();
		/** @brief DeltaWalk's scheduling of a full evaluation: everything kept dropped, the whole tracked set into `order`. */
		void ScheduleFullEvaluation();
		/** @brief DeltaWalk's scheduling: the per-frame entries the light path's gate (MoverDue) takes, into a cleared `order`. */
		void SchedulePerFrameEntries();
		/** @brief DeltaWalk's scheduling after the per-frame entries: pending, component, property, mirror, node and root events. */
		void ScheduleEventEntries();
		/** @brief After the walk: the published classes' kept slots still theirs back into Tables::classMembers. */
		void RestoreKeptMembers(std::array<std::vector<std::uint32_t>, Extensions::kPublishedEntryClasses>& a_kept);
		/** @brief The whole tracked set into `order` (a full evaluation, the dense walk). */
		void BuildFullOrder();
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
		std::uint32_t PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const;
		/** @brief PerFrameTraits from what the entry's evaluation has. */
		std::uint32_t PerFrameTraitsOf(bool a_entryClassTrait, bool a_ownerClassTrait, const RE::NiNode* a_categoryNode, const RE::BSGeometry& a_geometry) const;
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
		 * @brief Whether a per-frame owner class's (an actor's) earlier record is what WriteObject would write again but for placement and
		 * palette (its verdict stands).
		 */
		bool FrameVerdictKept(const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief Evaluates the entry this walk with its classification retaken. */
		void Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void MoveBucket(Tracked& a_tracked, Ineligible a_bucket);
		/** @brief After the delta walk's evaluations: the kept slots' geometry slots, and the shadow sets. */
		void FinishDeltaWalk(PartTimer& a_timer);
		/** @brief FinishDeltaWalk's geometry scan: the kept objects drawing a stale or freed geometry slot scheduled (or retired). */
		void ScheduleStaleGeometry();
		/** @brief FinishDeltaWalk's third round: the kept objects drawing a geometry slot re-resolved in place, scheduled and evaluated. */
		void ScheduleRefreshedGeometry(PartTimer& a_timer);
		/**
		 * @brief One walk round from a_first: evaluations (EvaluateEntry, pure), then merges in index order (MergeEntry). CS_DCLF_FANOUT 0:
		 * each in turn (also under the mirror-read parity); 1: all evaluated, then merged; 2 (default): pool chunks, each merged once
		 * ready.
		 */
		void EvaluateRound(PartTimer& a_timer, std::size_t a_first);
		/** @brief EvaluateRound's mode 2: pool chunks of a_count entries, each merged in order once ready (a_mirrorWrites, a_layout: the witnesses). */
		void EvaluateStreamed(PartTimer& a_timer, std::size_t a_first, std::size_t a_count, std::size_t a_chunks, bool a_light, bool a_profileKinds,
			std::uint64_t a_mirrorWrites, std::uint64_t a_layout);
		/** @brief Entry a_index evaluated (a_light: the first round, trying the light path). Pure. */
		void EvaluateEntry(std::size_t a_index, bool a_light, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief WriteObject's evaluation half: the classification and the slot-free record inputs. */
		void EvaluateWrite(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief EvaluateWrite's cached classification (the frame verdict retaken where it may change); false: rejected, no record. */
		bool EvaluateKeptVerdict(const Tracked& a_tracked, const SceneCapture::LeafView& a_leaf, bool a_rereads, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief EvaluateWrite's classification from scratch (the static verdict's cache, then the frame's); false: rejected, no record. */
		bool EvaluateStaticVerdict(RE::BSGeometry* a_geometry, const Tracked& a_tracked, const SceneCapture::LeafView& a_leaf, bool a_classifyProbe,
			EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief EvaluateWrite's slot-free record inputs: partitions or ranges, palette rows. */
		void EvaluateRecordInputs(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, EvalResult& a_out) const;
		/** @brief EvaluateWrite's shadow caster verdict and technique, and the occlusion maps' techniques. */
		void EvaluateShadowInputs(const SceneCapture::LeafView& a_leaf, EvalResult& a_out) const;
		/** @brief A classified entry's bookkeeping, evaluated (see EvalResult), plus its shading inputs where the merge may store them. */
		void EvaluateClassified(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief PerFrameOf at the merge, from the evaluation's traits and root motion. */
		std::pair<bool, std::uint32_t> PerFrameMerge(const Tracked& a_tracked, const EvalResult& a_result);
		/** @brief PerFrameOf's verdict from the traits (root motion included), the verdict and whether it may get a record. */
		static std::pair<bool, std::uint32_t> PerFrameFrom(std::uint32_t a_traits, Ineligible a_reason, bool a_mayRecord);
		/** @brief Entry a_index's merge (a_profileKinds: time its EvaluateKind). */
		void MergeEntry(std::size_t a_index, bool a_profileKinds, EvalResult& a_result, PartTimer& a_timer);
		/**
		 * @brief MergeEntry's light path: true when the kept record stands; else the entry is evaluated in full for the write (a_missed:
		 * the gate witness's finding).
		 */
		bool MergeLight(RE::BSGeometry* a_geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& a_timer, bool a_witnessed, const char*& a_missed);
		/** @brief ScheduleRoot's parity at the merge: a dependent only its witness scheduled came out as it went in. */
		void CheckRootWitness(const Tracked& a_tracked, bool a_written, bool a_hadSlot, std::uint32_t a_slotBefore, const Tables::Columns& a_columnsBefore,
			bool a_perFrameBefore, std::uint32_t a_traitsBefore, Ineligible a_reasonBefore);
		/** @brief WriteObject's merge half: the Tracked delta, then the face, geometry slots and record. */
		bool MergeWrite(RE::BSGeometry* a_geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& a_timer, Ineligible& a_bucket);
		/** @brief MergeWrite's geometry slots (a skin's partitions linked, or a LOD chain's ranges); Tables::kSlotFree: unstable, rebucketed. */
		std::uint32_t ResolveWriteGeometry(RE::BSGeometry* a_geometry, const SceneCapture::GeometryRecord& a_record, const EvalResult& a_result, PartTimer& a_timer,
			Ineligible& a_bucket);
		/** @brief MergeWrite's object record before its slot: flags, skinning and palette rows (a_boneRows). */
		ObjectRecord RecordOfWrite(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, const EvalResult& a_result, std::uint32_t a_geometrySlot,
			PartTimer& a_timer, std::uint32_t& a_boneRows);
		/** @brief MergeWrite's shadow and occlusion columns at a_slot, and the keys they use. */
		void MergeShadowColumns(std::uint32_t a_slot, std::uint32_t a_geometrySlot, const SceneCapture::LeafView& a_leaf, const EvalResult& a_result,
			ObjectRecord& a_object);
		/** @brief MergeWrite's row at a_slot: the remaining columns, the record (a_keepHalf: its accumulate half kept), the draw and the layer. */
		void MergeRecordRow(RE::BSGeometry* a_geometry, Tracked& a_tracked, const EvalResult& a_result, Api::EntryWrite& a_face, std::uint32_t a_slot,
			std::uint32_t a_geometrySlot, bool a_keepHalf, ObjectRecord& a_object, Ineligible a_bucket, PartTimer& a_timer);
		/** @brief A chunk's counters into the coordinator's, in chunk order; its warnings logged. */
		void MergeCounters(EvalCounters& a_counters);
		/** @brief A chunk's mirror-read counts into the coordinator's. */
		void MergeMirrorReads(MirrorReadStats& a_counts);
		/** @brief The mode a round of a_count entries runs in (CS_DCLF_FANOUT, the parities, the thread); the pool from a_least entries. */
		std::uint32_t FanoutMode(std::size_t a_count, std::size_t a_least = kFanoutMinEntries) const;
		/**
		 * @brief Drops every cached verdict and derivation when a live toggle the classification reads changed (Toggles.h): the caches
		 * witness the object, not the switches.
		 */
		void InvalidateVerdicts();

		// The membership joins: State/Joins.h (CoordinatorJoins).
		void BuildAccumulatePhase();
		// BuildAccumulatePhase's stages (AccumulatePhase.cpp): the joins run, one join's merge (its pipeline, material, derivation
		// cache and patch), and the residency the unpatched joins leave.
		struct JoinFrame;
		struct JoinBinding;
		void RunJoins(JoinFrame& a_frame);
		void MergeJoin(const OrderEntry& a_entry, JoinResult& a_result, JoinFrame& a_frame);
		std::uint32_t BindJoinPipeline(const JoinResult& a_result, const LightingDescriptors& a_descriptors, bool a_layer, PartTimer& a_timer);
		std::uint32_t MakeJoinPipeline(const JoinResult& a_result, const LightingDescriptors& a_descriptors, bool a_layer);
		bool BindJoinMaterial(std::uint32_t a_objectId, const JoinResult& a_result, const LightingDescriptors& a_descriptors, Tracked::Derived& a_derived,
			PartTimer& a_timer, std::uint32_t& a_materialSlot);
		void RecordJoinDerivation(const OrderEntry& a_entry, const JoinResult& a_result, const JoinFrame& a_frame, bool a_derivedHit,
			const LightingDescriptors& a_descriptors, const JoinBinding& a_binding, Tracked::Derived& a_derived);
		void PatchJoin(const OrderEntry& a_entry, const JoinResult& a_result, JoinFrame& a_frame, ObjectRecord& a_object, std::uint32_t a_objectId,
			const LightingDescriptors& a_descriptors, const JoinBinding& a_binding);
		void SettleJoinResidency();
		void ApplyAccumulatePatch(const AccumulatePatch& a_patch);
		/**
		 * @brief Binds the bind queue's records as residents from membership passes (PrimaryCull::MembershipPass), kept until rewritten.
		 */
		void BindByMembership();
		void EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const;
		void EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const;
		/** @brief A classified join's pipeline key and static flags (EvaluateJoin's stage; pure). */
		void KeyJoin(const Tracked& a_entry, bool a_layer, const SceneCapture::PropertyRecord& a_property, bool a_interior, JoinResult& a_out) const;
		/**
		 * @brief a_count evaluations into a_results, in chunks of kFanoutGrain on the pool (a_pool) or on this thread. In
		 * AccumulatePhase.cpp.
		 */
		template <class Result, class Evaluate>
		void EvaluateJoins(std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate);

		// The residents: State/Residents.h (CoordinatorResidents).
		/** @brief Whether a member's binding still holds for its object (the derivation cache's witnesses). */
		bool MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const;
		/** @brief The coordinator's residency of a slot (IsMember is the frame's). */
		bool ResidentObject(std::int32_t a_object) const { return a_object >= 0 && IsResidentSlot(static_cast<std::uint32_t>(a_object)); }
		bool IsResidentSlot(std::uint32_t a_slot) const { return a_slot < residentPos.size() && residentPos[a_slot] != kNotResident; }
		void MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch);
		/** @brief Ends a slot's membership; a_restore resets its accumulated half now. */
		void DropResidentSlot(std::uint32_t a_slot, bool a_restore);
		/** @brief A slot's accumulated half back as the walk wrote it. */
		void ResetAccumulatedHalf(std::uint32_t a_slot);
		/** @brief The residents' pipeline and material slots used this frame, and a template when none was. */
		void KeepResidentsAlive();
		void NoteResidentTouched(std::uint32_t a_slot) { residentsTouched.push_back(a_slot); }

		// The fade nodes the slots' listings hold (the fade roots' component, FadeRoots.cpp, and the features': Features/TreeWind's trees).
		/** @brief ApplyBatch: the fade node answers into their entries (Tracked::fadeNodeRef), each entry rewritten. */
		void ApplyFadeNodeAnswers();
		/** @brief Early in a full pass: applies the render thread's posts (the table families' sent epochs, the admitted roots: standInRootsPosted). */
		void TakeCoordinatorInputs();
		/**
		 * @brief The reference a newly listed fade root owner (or a component's: Features/TreeWind's trees) is copied from: the slot's
		 * entry's fade node if it is a_node, else null (not listed: fadeRootsUnowned).
		 */
		RE::NiPointer<RE::NiAVObject> HeldFadeNode(std::uint32_t a_slot, const void* a_node);
		/**
		 * @brief a_entry's fade node reference for a_node: the batch's pin, a root owner's copy, or requested from the render thread (once
		 * per key). True when held now.
		 */
		bool AcquireFadeNode(Tracked& a_entry, const void* a_node, const char* a_path);

		// The features' components: State/Components.h (CoordinatorComponents). Each call runs every registered component's hook at
		// the coordinator's residency point, walk phase or entry write (Api::CoordinatorComponent's hook sets: residency, accumulate, walk,
		// dense, retirement, entryClass), in registration order.
		/** @brief a_slot is resident (MarkResidentSlot; a fade node answer for a resident: ApplyFadeNodeAnswers). */
		void ComponentsResident(std::uint32_t a_slot);
		/** @brief a_slot's residency ended (DropResidentSlot, EndAllResidency). */
		void ComponentsLeft(std::uint32_t a_slot);
		/**
		 * @brief a_slot's record or layer written (WriteObject, WriteLayer: before its identity and ComponentsRewritten), or its entry's fade
		 * node answered (ApplyFadeNodeAnswers, before ComponentsResident): every component's slots.written.
		 */
		void ComponentsSlotWritten(std::uint32_t a_slot);
		/** @brief a_slot freed (the slot and layer releases, the sweep, the scan's untracked hit), right before its object is reset. */
		void ComponentsSlotReleased(std::uint32_t a_slot);
		/** @brief ApplySwitchEvents: a switch event applied (after switchesApplied, before the entries under it are scheduled). */
		void ComponentsSwitched(const void* a_switch);
		/** @brief TakeCoordinatorInputs: the primary cull's admitted roots, whole (after the table families' sent epochs). */
		void ComponentsAdmitted(std::span<const Api::StandInRoot> a_roots);
		/** @brief KeepResidentsAlive's recount: a_rebuild, every resident counted again; otherwise the touched slots (sorted, unique). */
		void ComponentsRecount(bool a_rebuild, std::span<const std::uint32_t> a_touched);
		/** @brief ResetSlotTables, before the tables are cleared. */
		void ComponentsReset();
		/** @brief EndAllResidency, before the residents' ComponentsLeft. */
		void ComponentsAllLeft();
		/** @brief ApplyAccumulatePatch wrote a_slot's accumulated half (a_chain: the patch's decalKey). */
		void ComponentsAccumulated(std::uint32_t a_slot, std::uint64_t a_chain);
		/** @brief The walk rewrote a_slot (a_written: its scene half; its skin partitions before and now). */
		void ComponentsRewritten(std::uint32_t a_slot, bool a_written, std::uint32_t a_partitionsBefore, std::uint32_t a_partitionsNow);
		/** @brief The accumulate phase's joins done (its statistics). */
		void ComponentsJoinsDone();
		/** @brief BeginWalk, its frame tables cleared and its serial moved on. */
		void ComponentsWalkBegun();
		/** @brief DeltaWalk's scheduling: a_full, a full evaluation; else after the pending entries (the counts into delta). */
		void ComponentsScheduling(bool a_full);
		/** @brief The walk's end (DeltaWalk after the candidates, DenseWalk after its sweep). */
		void ComponentsWalkEnded();
		/** @brief DenseReference: a_begun, before the dense walk; else right after it. */
		void ComponentsDense(bool a_begun);
		/** @brief EraseTracked, after the entry's slot is released. */
		void ComponentsForgot(const RE::BSGeometry* a_geometry);
		/** @brief The rescan, after the tracked set is cleared. */
		void ComponentsForgotAll();
		/** @brief RecycleRetired: a range component a_component retired (Tables::kRetiredComponentRange) is free. */
		void ComponentRecycled(std::uint32_t a_component, std::uint32_t a_first, std::uint32_t a_count);
		/**
		 * @brief MergeWrite's writes of an entry of a_class (Api::CoordinatorComponent::EntryClassHooks::follows): every write of one
		 * (listed); a taken record's stream before its slot (resolve: false, it waits); the slot it took (streamed).
		 */
		void ComponentsListed(RE::BSGeometry& a_geometry, const Tracked& a_tracked, Api::EntryClass a_class);
		bool ComponentsResolve(Api::EntryWrite& a_write, Api::EntryClass a_class);
		void ComponentsStreamed(std::uint32_t a_slot, Api::EntryWrite& a_write, Api::EntryClass a_class);
		/**
		 * @brief ApplyBatch's structural events at a_apply: each engine channel applied there (registration order), its keys in a_batch
		 * handed to every component's keys hook (Api::CoordinatorComponent::Events).
		 */
		void ComponentsChannelKeys(Api::ChannelApply a_apply, const EventBatch& a_batch);
		/** @brief A point with no batch's keys (AfterBatch, after ApplyEvents in a pass that applied a batch): its channels' keys hooks, no keys. */
		void ComponentsChannelKeys(Api::ChannelApply a_apply);
		// The commit's vetoes (State/Components.h; CoordinatorComponentContext's services).
		/** @brief a_entry's veto set (Api::kVetoHold, kVetoObserve or 0; none when untracked); in the settle, what it drops or holds applies. */
		void SetEntryVeto(const void* a_entry, std::uint8_t a_veto);
		/** @brief What the commit recorded of a_slot while Observe withheld it (none past the slots). */
		Api::ObservedState ObservedSlotOf(std::uint32_t a_slot) const;
		/** @brief a_entry's tracked state (tracked false: not tracked). */
		Api::TrackedState TrackedStateOf(const void* a_entry) const;
		/** @brief Whether the commit runs live: a component asked (SetCommitLive) or an entry is vetoed. */
		bool CommitLive() const;
		/**
		 * @brief The coordinator's report pass at a text point (Api::ReportPoint::SceneCoordinatorSegments, ...Materials, ...Delta): the features' text lines and the
		 * components' (Api::ReportLines::componentText, with the component's context) there, in registration order.
		 */
		void ComponentsReportText(Api::ReportPoint a_point, std::string& a_text);
		/**
		 * @brief The coordinator's report pass at a logged point (Api::ReportPoint::SceneCoordinatorGates, ...LastPass): the features' lines (handed
		 * a_frame) and the components' text (with the component's context), each logged as one line when not empty, in registration order.
		 */
		void ComponentsReportLines(Api::ReportPoint a_point, std::uint32_t a_frame);
		/** @brief Component a_component's state, the components made first when they are not (scene lane). */
		const std::shared_ptr<void>& ComponentState(std::uint32_t a_component);
		/** @brief A SceneStack channel's event applied in stack order: every component's stacked hook (Api::CoordinatorComponent::Events). */
		void ComponentsStacked(Api::EngineChannelId a_channel, const Api::Stacked& a_event);
		/** @brief A marked root (or a node under a detached root) detached, its mark erased: the marking component's detached hook. */
		void ComponentsDetached(const void* a_key, const Api::Mark& a_mark);
		/**
		 * @brief A detach in the world (ApplyMirrorEvents, before the mirror's records go): the marks on a_root and a_nodes erased (their
		 * hides ungated), each marking component told (ComponentsDetached).
		 */
		void UnmarkDetached(const void* a_root, std::span<const void* const> a_nodes);
		/** @brief The rescan: every component's rescan hook. */
		void ComponentsRescan();
		/** @brief A newly tracked entry under a marked root (the nearest on its chain): the marking component's underMark hook. */
		void ComponentsUnderMark(const RE::BSGeometry* a_geometry, const Api::Mark& a_mark);
		/** @brief The commit's settle (a_live: the scene built), the pass's take before CollectEvents, the deltas taken (published). */
		void ComponentsSettle(bool a_live);
		void ComponentsTake();
		void ComponentsPublished();

		// The shadow sets: State/ShadowSets.h (CoordinatorShadowSets).
		ShadowInputs ShadowInputsOf(std::uint32_t a_slot) const;
		void RefreshShadowSets(bool a_forceRebuild);
		// RefreshShadowSets' stages: the index cleared for a rebuild, one slot's inputs in or out of the indices, the lists emitted.
		void ResetShadowIndex(std::vector<std::uint32_t>& a_changed);
		void ApplyShadowInputs(std::uint32_t a_slot, const ShadowInputs& a_inputs, bool a_live, bool a_add, std::uint32_t& a_moved);
		void EmitShadowSets(bool a_rebuild, std::uint32_t a_moved);

		// The DCLF set and the claims: State/Set.h (CoordinatorSet).
		/** @brief The structural changes revokeCursor has not read, into structureChanged. */
		void NoteStructureChanges();
		/**
		 * @brief After ApplySet: revokes claimed phases whose record the work stopped drawing (from the change log), so the engine draws
		 * them.
		 */
		void RevokeUndrawnClaims();
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
		/** @brief A write of setBuilding's phases at a_geometry, for the claims' replay. */
		void NoteSetPhases(const RE::BSGeometry* a_geometry) { setPhaseLog.Push(a_geometry); }
		/** @brief A pool entry nothing else holds (a new one when none). */
		std::size_t TakeClaimsEntry();
		/** @brief PublishScene, after ApplySet: publicationClaims brought up to setBuilding (or the last shared when it did not change). */
		void PublishClaims();
		/** @brief The end of RevokeUndrawnClaims: claimsRevoked out of publicationClaims (a copy first when they were shared). */
		void RevokeFromClaims();
		void QueueSet(std::uint32_t a_slot);
		/**
		 * @brief Takes a slot out of the applied set's phases after its commit (rebound by the joins, a claim revoked): out of the commit's
		 * decision and the claims snapshot, queued for the next commit and marked for ApplySet. Otherwise the commit's copy keeps the
		 * phases and nobody draws the record. A base and its layer leave the main phases together.
		 */
		void LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner = true);
		/** @brief Marks a slot whose phases changed for the next ApplySet, with its current geometry. */
		void MarkSetApply(std::uint32_t a_slot);
		/**
		 * @brief The phases DCLF can draw (toggles, ShadowCapability, IndirectDraws::ViewDrawable): a capability that changes only on a toggle,
		 * load, failure or setup, so the views the engine asks for never change the set.
		 */
		static std::uint32_t SetCapability();
		/** @brief The phases an object takes part in among a_drawn, from its record (no readiness). */
		std::uint8_t SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const;
		/** @brief What the commit decides for a_slot now. Pure: CommitSet applies the side effects; its observers re-decide every slot. */
		SlotDecision DecideSlot(std::uint32_t a_slot, bool a_live, std::uint32_t a_drawn) const;
		// CommitSet's stages (SceneSet.cpp), over one commit's inputs (SetCommit).
		struct SetCommit;
		void CommitMembershipEvents(const SetCommit& a_commit);
		void CommitReadinessSources();
		void CommitShadowModeRequeues(std::size_t a_objects);
		/** @brief DecideSlot plus the commit's side effects (wait causes and reasons, counts, observers). */
		std::uint8_t CommitDecideSlot(std::uint32_t a_slot, const SetCommit& a_commit, bool& a_wait, bool* a_partial = nullptr);
		/** @brief a_slot's decided phases into the commit's copy, the lacking and waiting marks and setBuilding. */
		void CommitApplySlot(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, SetCommit& a_commit);
		void CommitQueuedSlots(SetCommit& a_commit);
		void CommitComponentsSettle(SetCommit& a_commit);
		/**
		 * @brief CommitSet: a_slot's phases as applied by its entry's veto (Hold: 0; Observe: 0, what it would have applied recorded for
		 * ObservedSlot; none: a_phases).
		 */
		std::uint8_t VetoedPhases(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live);
		/** @brief Whether a_slot's entry is vetoed (VetoedPhases' withholding alone: the set parity's). */
		bool EntryVetoed(std::uint32_t a_slot) const;
		/** @brief Whether a_slot's structure changed since the last publication's revocation (revokeCursor's unread changes), once a commit. */
		bool RestructuredSincePublication(std::uint32_t a_slot);
		/** @brief Whether everything the main phase draws with is ready; a_why: waitingBy's index when not. */
		bool MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const;
		/** @brief The CategoryRoot view whose entry a slot's geometry is (a first-person entry's), EngineView::Count for none. */
		Api::EngineView ViewRootOfSlot(std::uint32_t a_slot) const;

		// The publication and the retirement chain: State/Publication.h (CoordinatorPublication).
		void PublishTables();
		/** @brief PublishTables' replay jobs: a_snapshot's object columns (a_replay) and material records by the collected slots. */
		void ReplayTableColumns(Tables& a_snapshot, bool a_replay);
		// Engine references the scene work lets go of go through the retirement chain to EngineReleases (released at Present), since a kept
		// publication may still name them.
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
		/**
		 * @brief The pass's publication: ApplySet, PublishClaims, RevokeUndrawnClaims, PublishTables, then the deltas onto the log before the publication
		 * goes to the snapshot builder, so whoever adopts it finds its node.
		 */
		void PublishScene();
		/** @brief The frame's products since the last publication, moved into a_deltas. */
		void TakeDeltas(PublicationDeltas& a_deltas);
		/** @brief Drops the publications the render thread gave back (Channels::retiredPublications). */
		void DropReturnedPublications();
		/** @brief The tables' retiring slots into the chain's open node (before a publication). */
		void RetireIntoChain();
		/** @brief Each pass's start: batches no publication names any more, back on the free lists. */
		void RecycleRetired();

		// The scene pump and the publication pacing: State/Pump.h (CoordinatorPump).
		/** @brief Adds hand-timed ms to a nested scene part's sums (since ResetTimes, and this pass's). */
		void AddScenePartMs(ScenePart a_part, double a_ms)
		{
			stats.scenePartMs[static_cast<std::size_t>(a_part)] += a_ms;
			stats.scenePartFrameMs[static_cast<std::size_t>(a_part)] += a_ms;
		}
		/** @brief What a publication would be made from now. */
		PublishKey CurrentPublishKey() const;
		/** @brief Whether the next publication would carry deltas. */
		bool DeltasPending() const;
		/** @brief Whether anything changed since the last publication (else none is made). */
		bool PublicationNeeded();
		void NotePublished();
		/** @brief Whether this pass publishes now (PublicationNeeded holds), and why; false: it defers. */
		bool PublicationDue(bool a_task, PublishReason& a_why) const;
		/** @brief RunSceneWork's publish stage: the pacing counts, then PublishScene when needed and due, else a deferral. */
		void PublishIfDue(bool a_task);
		/** @brief Once a report window: the pacing line (passes and publications a frame, deferrals, the claims' replay, the paced parity). */
		void ReportPublicationPacing();
		/** @brief CS_DCLF_SET_PARITY (Policy::CheckPacedPublication): the publication just made against the coordinator's whole state. */
		void CheckPacedPublication(const Tables& a_published, const SetSnapshot& a_claims);
		/** @brief The pump's drain (the lane or RunScenePassInline): marks the thread, binds the frame's globals, runs RunSceneWork. */
		void ScenePass(bool a_inline);
	};
}
