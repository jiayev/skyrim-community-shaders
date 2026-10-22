#pragma once

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/RootViewEvents.h"

#include <vector>

#include <cstdint>
#include <span>

namespace DCLF::Diagnostics
{
	/**
	 * @brief The scene area's points that carry nothing (On<ScenePoint::...>, Hooks.h): the coordinator's (its pass's thread; the observers
	 * read it through SceneView on Scene::Coordinator::Get()) and the render thread's (Scene::FrameView::Get()).
	 */
	enum class ScenePoint : std::uint8_t
	{
		AccumulateInputsTaken,   // the coordinator, first in its pass: the frame inputs posted since its last pass taken
		ScenePhaseBegin,         // the scene phase begins (the change log trimmed), before anything of the pass writes
		ShadowSetsKept,          // the shadow sets updated by changes, about to be made whole again (Policy::RebuildShadowSets)
		ShadowSetsRebuilt,       // ... made whole again from the same tables
		WalkDone,                // the walk done (the shading named, the placement plan published), before the joins
		JoinsBegin,              // the joins begin (the walk done, nothing bound yet)
		ResidentsKept,           // the joins' residents kept (KeepResidentsAlive), before the slot check
		JoinsEnd,                // the joins done (the verdicts and the residency this pass leaves)
		MirrorCheckPoint,        // at a pass's end (its events all drained)
		MirrorProbePoint,        // render thread, the frame's start (ServeFrameRequests): the live scene is captured for the mirror's observers
		AccumulatePrepared,      // render thread, EarlyPrepass: the accumulate phase's frame inputs prepared (the material captures served)
		PrepassConstantsBegin,   // render thread, Prepass: the frame constants' refresh begins (the character light's view taken)
		PipelineConstantsBegin,  // ... the pipelines' blocks are about to be refreshed from the frame's sample (the engine shaders found)
		PrepassConstantsEnd,     // ... the frame constants refreshed
	};

	// The scene area's hook points (Hooks.h's mechanism): what Coordinator announces for the observers in Diagnostics/Scene. Each runs on
	// the thread of its Notify - the coordinator's pass (the scene lane, or the render thread while the passes run inline), or the render
	// thread where noted - and reads the coordinator's state through SceneView.

	/** @brief Render thread, EarlyPrepass: the frame's registrations, drained from the pass capture. */
	struct CaptureDrained
	{
		Scene::FrameView& view;
		std::span<const PassCapture::Entry> entries;
	};

	/** @brief The coordinator: a material slot drained (its last reference gone), before its contents are dropped. */
	struct MaterialSlotDrained
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
	};

	/** @brief The coordinator: a candidate table brought up to date (`sun`: the sun's, else the point lights'), its snapshot published. */
	struct CandidatesUpdated
	{
		Scene::Coordinator& store;
		bool sun;
	};

	/** @brief The coordinator: a publication's snapshot brought up to the tables (PublishTables; `published`: the publications before it). */
	struct TablesReplayed
	{
		Scene::Coordinator& store;
		const SceneTables& snapshot;
		std::uint64_t published;
		std::uint64_t position, geometryPosition;  // the snapshot's change and geometry logs' ends before the replay
		std::uint32_t keptFamilies;  // the table families the replay kept (their stamps stood), a bit by registration (the fade roots among them)
		bool keepConstants;  // the other family the replay kept
	};

	// The mirror's reads (the lane reads the scene from the mirror), announced for the observers that check them against the live scene:
	// the coordinator's, or an evaluation's (its thread; Policy::SerialFanout keeps them on the pass's while observed).

	/** @brief FindCategoryNode: `object`'s category node by the mirror (`found`; `reason` the parent reasons on the way). */
	struct CategoryNodeRead
	{
		const Scene::Coordinator& store;
		RE::NiAVObject* object;
		RE::NiNode* found;
		Ineligible reason;
		bool unmirrored;  // a record missing on the way
	};

	/** @brief ResolveSunEntry: the mirror's sun entry (`entry`) and, on a live walk's path (`liveKnown`), the live chain's. */
	struct SunEntryRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const void* entry;
		const RE::NiAVObject* live;
		bool liveKnown;
	};

	/** @brief LightEntryOf: the mirror's root under `category` (an actor's: `actorRoot`) and, on a live walk's path (`liveKnown`), the live chain's. */
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

	/** @brief PrepareSubtree: the geometries under `root` by the mirror, with their parent reasons (`lacking`: a record missing on the way). */
	struct SubtreeRead
	{
		const Scene::Coordinator& store;
		RE::NiAVObject* root;
		Ineligible reasonAbove;
		std::span<const std::pair<RE::BSGeometry*, Ineligible>> found;
		bool lacking;
	};

	/** @brief A switch or node event's walk below `root` by the mirror (`read`: which). */
	struct TrackedBelowRead
	{
		const Scene::Coordinator& store;
		SceneView::MirrorRead read;
		RE::NiAVObject* root;
	};

	/** @brief ListHiddenChain: `geometry`'s chain up to its category node by the mirror. */
	struct HiddenChainRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry* geometry;
		const RE::NiNode* category;
		std::span<const RE::NiAVObject* const> chain;
	};

	/** @brief MoveKeyOf: the one reference on `geometry`'s chain up to its category node by the mirror (`key`; null: none, or several). */
	struct MoveKeyRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const RE::NiNode* category;
		const void* key;
	};

	// The scene mirror's writes and the probe's points (CS_DCLF_MIRROR_PARITY's records).

	/** @brief The coordinator: a hook's update applied to the mirror (`type`: the record type; `key` null when the mirror holds no record). */
	struct MirrorUpdateApplied
	{
		std::uint8_t type;
		const void* key;
		std::uint32_t fields;
	};

	/** @brief The coordinator, first in its drains (CollectEvents). */
	struct EventsCollecting
	{
		Scene::Coordinator& store;
	};

	/**
	 * @brief The coordinator: a batch applied to the mirror; the objects its events without values named (the engine channels' keys, by
	 * channel: those whose channel names its keys, Api::EngineChannel::namesKeys).
	 */
	struct MirrorEventsApplied
	{
		Scene::Coordinator& store;
		std::span<const std::vector<const void*>> channelKeys;
	};

	/** @brief LocalShadowLights::Sample: a local shadow descriptor's volume as the Lights board has it, and its culling process after the cull. */
	struct ShadowVolumeSampled
	{
		const ShadowCameras::Camera& view;
		const RE::BSCullingProcess& process;
	};

	/**
	 * @brief An evaluation (any thread of the pass): an entry whose mirror chain reaches a CategoryRoot view's root (the first-person
	 * root) while it is filed under another category node.
	 */
	struct ViewMisfiled
	{
		Api::EngineView view;
		const void* geometry;
		const void* category;
	};

	/** @brief The coordinator, a walk's merge: an actor's frame verdict changed with no hidden event; its chain up to its category node. */
	struct ActorVerdictUnannounced
	{
		std::span<const RE::NiAVObject* const> chain;
	};

	/**
	 * @brief The coordinator, CommitSet: a slot's decision that left a CategoryRoot view's slot (a first-person one) out of its view
	 * (`waitWhy`: the waiting reason it shows).
	 */
	struct SetSlotDecided
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
		const SceneView::SlotDecision& decision;
		std::uint8_t waitWhy;
	};

	/** @brief The coordinator: the set's commit applied its queue and flips (`live`: the scene built; `drawn`: the phases DCLF draws; `gatesLive`: gates open or held out). */
	struct SetCommitted
	{
		Scene::Coordinator& store;
		bool live;
		std::uint32_t drawn;
		bool gatesLive;
	};

}
