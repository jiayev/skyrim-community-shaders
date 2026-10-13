#pragma once

#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"

#include <functional>
#include <vector>

#include <cstdint>
#include <span>

namespace DCLF::Diagnostics
{
	// The scene area's hook points (Hooks.h's mechanism): what Coordinator announces for the observers in Diagnostics/Scene. Each runs on
	// the thread of its Notify - the coordinator's pass (the scene lane, or the render thread while the passes run inline), or the render
	// thread where noted - and reads the store through SceneView.

	/** @brief Render thread, EarlyPrepass: the frame's registrations, drained from the pass capture. */
	struct CaptureDrained
	{
		Scene::FrameView& view;
		std::span<const PassCapture::Entry> entries;
	};

	/** @brief The coordinator, first in its pass: the frame inputs posted since its last pass taken. */
	struct AccumulateInputsTaken
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: the joins begin (the walk done, nothing bound yet). */
	struct JoinsBegin
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: the joins' residents kept (KeepResidentsAlive), before the slot check. */
	struct ResidentsKept
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: the slot check done (a_resolveBuffers: the pass resolves buffers). */
	struct SlotsChecked
	{
		Scene::Coordinator& store;
		bool resolveBuffers;
	};

	/** @brief The coordinator: the joins done (the verdicts and the residency this pass leaves). */
	struct JoinsEnd
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: a base record's membership pass gave none (a_why: PrimaryCull::LastSyntheticFail). */
	struct MembershipPassFailed
	{
		Scene::Coordinator& store;
		const RE::BSGeometry* geometry;
		std::uint8_t why;
		const SceneCapture::LeafView& leaf;
	};

	/** @brief The coordinator: a slot made resident (bound by its membership pass). */
	struct ResidentBound
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
	};

	/** @brief The coordinator: a tracked geometry's record taken anew at a slot (the walk's merge). */
	struct RecordWrittenAnew
	{
		Scene::Coordinator& store;
		const RE::BSGeometry* geometry;
	};

	/** @brief The coordinator: a slot joined the set (CommitSet's decision gave it phases it had none of). */
	struct SetJoined
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
	};

	/** @brief The coordinator: a hidden store's event applied (DrainHiddenEvents; a_key a node's key, never read). */
	struct HiddenStoreApplied
	{
		Scene::Coordinator& store;
		const void* key;
		std::uint32_t site;
		bool hidden;
	};

	/** @brief The coordinator: the hidden events drained. */
	struct HiddenEventsDrained
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: a material slot drained (its last reference gone), before its contents are dropped. */
	struct MaterialSlotDrained
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
	};

	/** @brief The coordinator: the scene phase begins (the change log trimmed), before anything of the pass writes. */
	struct ScenePhaseBegin
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: the walk done (the shading named, the placement plan published), before the joins. */
	struct WalkDone
	{
		Scene::Coordinator& store;
	};

	/** @brief Render thread, Prepass: the frame constants' refresh begins (the character light's view taken). */
	struct PrepassConstantsBegin
	{
		Scene::FrameView& view;
	};

	/** @brief Render thread, Prepass: the pipelines' blocks are about to be refreshed from the frame's sample (the engine shaders found). */
	struct PipelineConstantsBegin
	{
		Scene::FrameView& view;
	};

	/** @brief Render thread, Prepass: the frame constants refreshed. */
	struct PrepassConstantsEnd
	{
		Scene::FrameView& view;
	};

	/** @brief The coordinator: a candidate table brought up to date (a_sun: the sun's, else the point lights'), its snapshot published. */
	struct CandidatesUpdated
	{
		Scene::Coordinator& store;
		bool sun;
	};

	/** @brief The coordinator, the decal order's upkeep (after the joins): whether an observer checks the order this pass (a_checked), which runs the upkeep even with no change. */
	struct DecalOrderDue
	{
		Scene::Coordinator& store;
		bool& checked;
	};

	/** @brief The coordinator: the kept decal order brought up to date, on a pass an observer checks it (DecalOrderDue). */
	struct DecalsOrdered
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: the pass's object LOD segment events applied (the shapes' drawn ranges sampled again). */
	struct LodSegmentsApplied
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: the shadow sets updated by changes, about to be made whole again (Policy::RebuildShadowSets). */
	struct ShadowSetsKept
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: the shadow sets made whole again from the same tables. */
	struct ShadowSetsRebuilt
	{
		Scene::Coordinator& store;
	};

	/** @brief The coordinator: a publication's snapshot brought up to the tables (PublishTables; a_published: the publications before it). */
	struct TablesReplayed
	{
		Scene::Coordinator& store;
		const SceneTables& snapshot;
		std::uint64_t published;
		std::uint64_t position, geometryPosition;  // the snapshot's change and geometry logs' ends before the replay
		bool keepTrees, keepFadeRoots, keepConstants;  // the families the replay kept (their stamps stood)
	};

	/** @brief The coordinator, taking a publication's deltas: the gates its commits flipped (Coordinator's gateFlipsMade), against its claims. */
	struct GateFlipsPublished
	{
		Scene::Coordinator& store;
	};

	/** @brief Render thread, the frame's start (ReleaseGates): a LOD gate flipped by an installed publication, about to be released, and the claims the frame installed. */
	struct GateReleasing
	{
		const SetSnapshot& claims;
		std::uint64_t token;
		std::span<const std::pair<const RE::BSGeometry*, std::uint8_t>> incoming;  // the incoming base geometries claimed, with the flip's phases
		std::span<const RE::BSGeometry* const> outgoing;                          // the outgoing base geometries dropped
	};

	/** @brief The coordinator, a round fanned out on the pool: whether an observer compares it with a serial evaluation (a_checked). */
	struct FanoutDue
	{
		Scene::Coordinator& store;
		bool& checked;
	};

	/** @brief The coordinator: a walk round from a_first evaluated (FanoutDue checked); a_serial evaluates it again on this thread. */
	struct WalkRoundEvaluated
	{
		Scene::Coordinator& store;
		std::size_t first;
		std::span<const SceneView::EvalResult> results;
		const std::function<void(std::vector<SceneView::EvalResult>&)>& serial;
	};

	/** @brief The coordinator: the subtrees' prepares (FanoutDue checked); a_serial prepares them again on this thread. */
	struct SubtreesPrepared
	{
		Scene::Coordinator& store;
		std::span<const std::pair<RE::NiAVObject*, SceneView::SubtreeSource>> roots;
		std::span<const SceneView::SubtreePrep> results;
		const std::function<void(std::vector<SceneView::SubtreePrep>&)>& serial;
	};

	/** @brief The coordinator: a joins round a_what evaluated (FanoutDue checked); a_serial evaluates it again on this thread. */
	template <class Result>
	struct JoinsEvaluated
	{
		Scene::Coordinator& store;
		const char* what;
		std::span<const Result> results;
		const std::function<void(std::vector<Result>&)>& serial;
	};

	// The mirror's reads (the lane reads the scene from the mirror), announced for the observers that check them against the live scene:
	// the coordinator's, or an evaluation's (its thread; Policy::SerialFanout keeps them on the pass's while observed).

	/** @brief FindCategoryNode: a_object's category node by the mirror (a_found; a_reason the parent reasons on the way). */
	struct CategoryNodeRead
	{
		const Scene::Coordinator& store;
		RE::NiAVObject* object;
		RE::NiNode* found;
		Ineligible reason;
		bool unmirrored;  // a record missing on the way
	};

	/** @brief ResolveSunEntry: the mirror's sun entry (a_entry) and, on a live walk's path (a_liveKnown), the live chain's. */
	struct SunEntryRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const void* entry;
		const RE::NiAVObject* live;
		bool liveKnown;
	};

	/** @brief LightEntryOf: the mirror's root under a_category (an actor's: a_actorRoot) and, on a live walk's path (a_liveKnown), the live chain's. */
	struct LightEntryRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const RE::NiNode* category;
		const void* root;
		bool actorRoot;
		const RE::NiAVObject* live;
		bool liveActor;
		bool liveKnown;
	};

	/** @brief PrepareSubtree: the geometries under a_root by the mirror, with their parent reasons (a_lacking: a record missing on the way). */
	struct SubtreeRead
	{
		const Scene::Coordinator& store;
		RE::NiAVObject* root;
		Ineligible reasonAbove;
		std::span<const std::pair<RE::BSGeometry*, Ineligible>> found;
		bool lacking;
	};

	/** @brief A switch or node event's walk below a_root by the mirror (a_read: which). */
	struct TrackedBelowRead
	{
		const Scene::Coordinator& store;
		SceneView::MirrorRead read;
		RE::NiAVObject* root;
	};

	/** @brief ListHiddenChain: a_geometry's chain up to its category node by the mirror. */
	struct HiddenChainRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry* geometry;
		const RE::NiNode* category;
		std::span<const RE::NiAVObject* const> chain;
	};

	/** @brief MoveKeyOf: the one reference on a_geometry's chain up to its category node by the mirror (a_key; null: none, or several). */
	struct MoveKeyRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const RE::NiNode* category;
		const void* key;
	};

	// The scene mirror's writes and the probe's points (CS_DCLF_MIRROR_PARITY's records).

	/** @brief The coordinator: a hook's update applied to the mirror (a_type: the record type; a_key null when the mirror holds no record). */
	struct MirrorUpdateApplied
	{
		std::uint8_t type;
		const void* key;
		std::uint32_t fields;
	};

	/** @brief The coordinator, first in its drains (CollectEvents): a_loading, a load screen's pass (no batch is applied). */
	struct EventsCollecting
	{
		Scene::Coordinator& store;
		bool loading;
	};

	/** @brief The coordinator: a batch applied to the mirror; the objects its events without values named (the LOD segments' shapes, a load screen's named keys). */
	struct MirrorEventsApplied
	{
		Scene::Coordinator& store;
		std::span<const void* const> lodSegments;
		std::span<const void* const> named;
	};

	/** @brief The coordinator, at a pass's end (its events all drained). */
	struct MirrorCheckPoint
	{
		Scene::Coordinator& store;
	};

	/** @brief Render thread, the frame's start (ServeFrameRequests): the point the live scene is captured for the mirror's observers. */
	struct MirrorProbePoint
	{
		Scene::FrameView& view;
	};

	/** @brief LocalShadowLights::Sample: a local shadow descriptor's volume as LightViews has it, and its culling process after the cull. */
	struct ShadowVolumeSampled
	{
		const SunViews::Cascade& view;
		const RE::BSCullingProcess& process;
	};

	/** @brief An evaluation (any thread of the pass): an entry whose mirror chain reaches the first-person root while it is filed under another category node. */
	struct FirstPersonMisfiled
	{
		const void* geometry;
		const void* category;
	};

	/** @brief Render thread, EarlyPrepass: the frame's registrations drained (the registration jobs done). */
	struct RegistrationsDone
	{
		Scene::FrameView& view;
	};

	/** @brief The coordinator, a walk's merge: an actor's frame verdict changed with no hidden event; its chain up to its category node. */
	struct ActorVerdictUnannounced
	{
		std::span<const RE::NiAVObject* const> chain;
	};

	/** @brief The coordinator, CommitSet: a slot's decision that left a first-person slot out of its view (a_waitWhy: the waiting reason it shows). */
	struct SetSlotDecided
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
		const SceneView::SlotDecision& decision;
		std::uint8_t waitWhy;
	};

	/** @brief The coordinator: the set's commit applied its queue and flips (a_live: the scene built; a_drawn: the phases DCLF draws; a_gatesLive: gates open or held out). */
	struct SetCommitted
	{
		Scene::Coordinator& store;
		bool live;
		std::uint32_t drawn;
		bool gatesLive;
	};

	/** @brief Render thread, EarlyPrepass: the accumulate phase's frame inputs prepared (the material captures the joins asked for served). */
	struct AccumulatePrepared
	{
		Scene::FrameView& view;
	};
}
