#pragma once

// The scene capture's events: what the engine's hooked writers (SceneEvents.cpp, HiddenStores.cpp) push, on whichever thread runs them,
// for the coordinator's scene passes to drain (keys, or references the events hold) and for the mirror (SceneTracker's updates).

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Features/DrawcallLimitFix/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Runtime/SceneWake.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneTracker.h"

namespace DCLF::Scene
{
	/**
	 * @brief Installs the scene's event hooks (render thread, PostPostLoad): the fades, the property flags and materials, the material alpha,
	 * the LOD fades, the emittance, Havok's node transforms, the controllers, the switch stores and child edits, the moves, the hidden
	 * stores, object LOD's segments and tree LOD.
	 */
	void InstallSceneEvents();
	/** @brief The hidden-bit stores are patched (HiddenStores.cpp). */
	bool HiddenStoresLive();
	/** @brief LodGates' own app-cull stores (a flip, a forced release's unhide), pushed as a patched store's event. */
	void PushGateHidden(const RE::NiAVObject* a_object);

	/**
	 * @brief The mirror's updates (step 6e F3b): the values a hook's writer changed (SceneCapture::Update), captured after the
	 * write on the writer's thread and pushed onto SceneTracker's stack, in order with the attaches and detaches; the scene work
	 * applies them in that order (SceneMirror::Update).
	 */
	inline void PushNodeUpdate(const RE::NiAVObject* a_node, std::uint32_t a_fields)
	{
		if (a_node)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureNodeFields(*a_node, a_fields) });
	}

	inline void PushPropertyUpdate(const RE::BSShaderProperty* a_property, std::uint32_t a_fields)
	{
		if (a_property)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureProperty(*a_property) });
	}

	inline void PushAlphaUpdate(const RE::NiAlphaProperty* a_alpha, std::uint32_t a_fields)
	{
		if (a_alpha)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureAlpha(*a_alpha) });
	}

	inline void PushGeometryUpdate(const RE::BSGeometry* a_geometry, std::uint32_t a_fields)
	{
		if (a_geometry)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureGeometry(*a_geometry) });
	}

	/** @brief A geometry whose property or alpha a writer swapped: its leaf, applied as a capture when the mirror holds it. */
	inline void PushLeafUpdate(const RE::BSGeometry& a_geometry)
	{
		using G = SceneCapture::GeometryRecord;
		// A capture too: numbered at its start.
		const auto sequence = SceneCapture::NextSequence();
		SceneCapture::Update update{ G::kProperty | G::kAlpha | G::kLayer, SceneCapture::CaptureGeometry(a_geometry) };
		auto leaf = std::make_shared<SceneCapture::Records>();
		// Its properties held by the event (T6b1b: the scene work's references to them are copies).
		std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		SceneCapture::CaptureLeaf(a_geometry, *leaf, &pins);
		leaf->sequence = sequence;
		update.leaf = std::move(leaf);
		update.sequence = sequence;
		SceneTracker::Get().PushUpdate(std::move(update), std::move(pins));
	}

	// A fade node's statics (near and far, +0x109, the LOD type).
	constexpr std::uint32_t kFadeStatics = SceneCapture::NodeRecord::kFadeNear | SceneCapture::NodeRecord::kFadeFar |
	                                       SceneCapture::NodeRecord::kFade109 | SceneCapture::NodeRecord::kFadeType;
	// Its per-frame state (T6b1a): currentFade, the LOD level, the screen-door byte.
	constexpr std::uint32_t kFadeState = SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeLevel | SceneCapture::NodeRecord::kFadeDoor;

	inline float CurrentFade(RE::BSFadeNode* a_node)
	{
		return a_node->GetRuntimeData().currentFade;
	}

	/**
	 * @brief Placement snaps: FUN_14147aa20 (node, camera), from the cell attach (FUN_1402d1280, FUN_1402d5090), sets a fade
	 * node's fade, its faded-in flag, its snap radius (+0x134) and its last visible frame from the camera. A root listed
	 * before its snap (a dynamic reference's) would keep the state it was listed with on the GPU: its row is taken from
	 * the node again (Coordinator::ReseedFadeRoot). Identity only; the drain follows the attach and detach events.
	 */
	// T6b3d: no wake: a snap comes with its cell's attach, whose event wakes the pass.
	inline EventQueue<const void*> fadeSnapEvents;

	/**
	 * @brief fadeAmount events: Actor::SetAlpha (vfunc 0xE1, 0x1406c1a50) wrote an actor's 3D root's fadeAmount (+0x100), an input
	 * of its fade root's static row (FadeRootStatic::fadeAmount). Actor::UpdateAlpha calls it each frame of an actor's fade
	 * (FUN_1406eb630 steps ActorProcess's alpha from 0 to 1 after MoveToHigh, Enable or a resurrection), so an actor listed
	 * mid-fade would keep the alpha it was listed with: FadeStateCS fades it toward that and drops its members. Identity only
	 * (Coordinator::RefreshFadeAmount reads the listed node again).
	 */
	// T6b3d: no wake: an actor's fade writes it every frame; the frame's next pass takes them.
	inline EventQueue<const void*> fadeAmountEvents;

	/**
	 * @brief SceneEvents: the delta walk's structural events (dclf-event-driven-tables.md, "Phase 3"), pushed
	 * from the engine's writers on whichever thread runs them, drained and applied by the scene pass (T6b3d: CollectEvents, ApplyEvents).
	 *
	 * - A property event: BSShaderProperty::SetFlags (0x14147bee0) or SetMaterial (0x14147bff0) changed a shader
	 *   property, or a controller was added to a property. It carries the pointer as a key only: the drain looks it
	 *   up among the tracked entries' properties and never dereferences it.
	 * - A node event: Havok wrote a node's transform from its rigid body (FUN_140ea55a0, which every collision object
	 *   class's SetNodeTransformsFromWorldTransform and the island activation listener call), or a controller was
	 *   added to a node (NiObjectNET::PrependController, FUN_140d268d0, which NiTimeController::SetTarget calls). It
	 *   holds a reference, as SceneTracker's attach events do, because the drain walks the node's subtree and its
	 *   ancestors.
	 */
	inline EventQueue<const void*> propertyEvents{ &WakeScenePropertyEvent };
	// T6b3d: no wake: Havok writes the transforms of what moves every frame (the movers, once a frame); the frame's next pass takes them.
	inline EventQueue<RE::NiPointer<RE::NiAVObject>> nodeEvents;

	inline void PushProperty(const void* a_property) { propertyEvents.Push(a_property); }

	inline void PushNode(RE::NiAVObject* a_node)
	{
		if (a_node)
			nodeEvents.Push(RE::NiPointer<RE::NiAVObject>(a_node));
	}

	/**
	 * @brief LOD fade events: the Lighting properties whose specularLODFade or envmapLODFade (+0x100, +0x104) changed.
	 * GetRenderPasses (BSLightingShaderProperty vtable slot 0x2A) writes them from the fade node's LOD metric
	 * (skyrim-engine-notes.md, "LOD fades in GetRenderPasses") whenever any view registers the object, which can be
	 * after the accumulate phase sampled its patch or while it is a resident. The detour compares the two floats
	 * around the call and pushes the property when either moved; the walk names its dependents for the next frame's
	 * shading sample (NameShadingEvents). Cull and accumulation job threads push, the walk drains.
	 */
	// T6b3d: per-frame (the next frame's shading sample): no wake, the next frame input's pass drains it.
	inline EventQueue<const void*> lodFadeEvents;
	inline bool lodFadeEventsInstalled = false;

	inline void DrainLodFadeEvents(std::vector<const void*>& a_out)
	{
		lodFadeEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
	}

	/**
	 * @brief Emittance events: the shared colours external emittance reads, when they change. A Lighting property with
	 * kExternalEmittance has its emissiveColor pointed (FUN_141480fc0, from the cell attach FUN_1402d2f60) at its
	 * reference's emittance source: a light's colour (TESObjectLIGH +0x118, constant), a region's emittanceColor
	 * (TESRegion +0x40, which the cell's emittance update FUN_1402b4390 blends from the region's weather) or the sky's
	 * colour for sky-lit movable statics (Sky +0x9c of its colours, from Sky's colour update FUN_14040ba70). Both
	 * updates write through Sky::SetColor (0x14040d970), whose detour pushes the colour when it moved; the colour is a
	 * key in propertyDependents (ListDependents), and the walk names its dependents as it does for a LOD fade event.
	 * The main thread and the cell update jobs push, the walk drains.
	 */
	// T6b3d: per-frame (the next frame's shading sample): no wake, the next frame input's pass drains it.
	inline EventQueue<const void*> emittanceEvents;

	/**
	 * @brief Object LOD's segment events (dclf-lod.md, LodSegments): the BSSubIndexTriShapes a segment write touched, as keys
	 * (never dereferenced: the drain looks them up among the tracked shapes). Pushed after the write, by the patched calls
	 * of the terrain manager's segment updates (InstallLodSegmentHooks), from whichever thread runs them; drained by the
	 * scene pass (T6b3d: CollectEvents).
	 */
	// T6b3d: no wake: the terrain manager writes the segments as the camera moves, mostly to the ranges held (w130: 354 of 611 passes they
	// woke changed nothing); the frame's next pass takes them.
	inline EventQueue<const void*> lodSegmentEvents;
	inline bool lodSegmentEventsInstalled = false;
	bool InstallLodSegmentHooks();

	inline void DrainEmittanceEvents(std::vector<const void*>& a_out)
	{
		emittanceEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
	}

	inline void DrainPropertyEvents(std::vector<const void*>& a_out)
	{
		propertyEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
	}

	inline void DrainNodeEvents(std::vector<RE::NiPointer<RE::NiAVObject>>& a_out)
	{
		nodeEvents.Drain([&](RE::NiPointer<RE::NiAVObject>&& a_node) { a_out.push_back(std::move(a_node)); });
	}

	/**
	 * @brief MoveEvents: what the engine moved, as keys (never dereferenced): a reference whose 3D an update pass
	 * took, or a category node whose subtree one did (dclf-event-driven-tables.md, "Movers by event"). The light path
	 * places a mover only when its key had an event this frame or the last (Coordinator::MoveGated). Pushed after the
	 * engine's write, from the animation and cell jobs' threads; drained by the render thread at the delta walk.
	 * - Actors: FUN_14066afa0, which every animation path starts with (RunOneActorAnimationUpdateJob,
	 *   Actor::UpdateAnimation, PlayerCharacter::UpdateAnimation, Actor::FinishLoadGame); TESObjectREFR::Update3DPosition
	 *   (0x1402d9800, which Actor::Update3DPosition calls); Actor::UpdateActor3DPosition (0x14069eb80); the sky cell's
	 *   skin job (FUN_140663430); the ragdoll job (FUN_140770dc0), for every actor on its list.
	 * - Other references: a cell's animated references' update pass (UpdateSelectedDownwardPass on the reference's 3D,
	 *   called from FUN_1402b41a0 only for those in a visible room or fading), the cell's dynamic node's (FUN_1402b3ae0,
	 *   keyed by the node), and the graph-animated references' (FUN_1402f75a0, from UpdateAnimationJob).
	 * - The node events (Havok's node writes and controller additions) name the reference above the node.
	 */
	// T6b3d: per-frame (the movers the frame values place, MovedRecently: this frame or the last): no wake, the next frame input's
	// pass drains them (thousands a frame: a pass each would be a pass per animation update).
	inline EventQueue<const void*> moveEvents;
	// Calls per writer (the order of kMoveWriterNames), for the report.
	inline std::array<std::atomic<std::uint64_t>, 8> moveWriterCalls{};
	constexpr std::array<const char*, 8> kMoveWriterNames{ "animation", "reference 3D position", "actor 3D position", "sky cell skin", "ragdoll",
		"graph animation", "update pass", "" };
	inline void CountMove(std::size_t a_writer) { moveWriterCalls[a_writer].fetch_add(1, std::memory_order_relaxed); }

	/**
	 * @brief HiddenEvents: a node whose kHidden bit (NiAVObject::flags bit 0) a store changed, as a key (never
	 * dereferenced), from the patched stores (HiddenStores.cpp) on whichever thread runs them; drained by the render
	 * thread at the delta walk. An actor entry's frame verdict is taken again only on the frame a node on its chain
	 * had one (Coordinator::hiddenDependents), and on parity frames for all of them (the witness).
	 */
	struct HiddenEvent
	{
		const void* key = nullptr;
		std::uint32_t site = 0;  // the patched store's index (HiddenStoreSiteAt)
		bool hidden = false;     // the bit's value after the store
	};
	inline bool hiddenEventsInstalled = false;
	bool InstallHiddenStores();
	/** @brief The patched store at a_index (its address in the image), or 0. */
	std::uintptr_t HiddenStoreSiteAt(std::uint32_t a_index);
	/** @brief T6b0: the stores that changed the bit since the last call, by site: shown and hidden counts, most shows first. */
	std::string TakeHiddenSiteReport();
	/** @brief The patched store (its address in the image) whose stub holds a_address, or 0 (HiddenWatch's report). */
	std::uintptr_t HiddenStoreSiteOf(std::uintptr_t a_address);

	inline void PushMove(const void* a_key)
	{
		if (a_key)
			moveEvents.Push(a_key);
	}

	/**
	 * @brief SwitchEvents: an NiSwitchNode's selection may have changed (dclf-cull-job-elimination.md, "Phase 3"),
	 * pushed from the writer's thread, drained by the scene pass (T6b3d: CollectEvents) and applied by its walk (ApplySwitchEvents).
	 *
	 * NiSwitchNode::index (+0x12C) has no setter. Its only stores outside construction, cloning and loading (AE
	 * 1.6.1170, every `mov [reg+0x12c]` in .text) are:
	 * - BSTreeManager's LOD selection in Main::Update (FUN_140437e50, five stores; FUN_140438840, two) and the local
	 *   map's (FUN_140438580, one), on a tree's LOD switch (BSTreeNode +0x180). They leave the new child as it was:
	 *   NiSwitchNode::OnVisible brings it up to date in the cull.
	 * - Harvesting (FUN_1401e8ef0, from TESObjectTREE::Activate and the flora's) and a harvestable's 3D setup
	 *   (FUN_1401e9450), on the switch under the reference's root. Both update the switch right after.
	 * Each store is patched with a call to a stub (SwitchStoreStubs) that makes the store itself and pushes an event
	 * with the index before it when the value changed. The tree manager can store twice in one pass (a level, then
	 * the far level), so the event is judged by the net change when applied.
	 *
	 * NiSwitchNode's AttachChild, DetachChild and SetAt reset revID to 1, which leaves the selected child out of date
	 * without a store to the index, and DetachChild clears the index when the selected slot empties: those push a
	 * structural event.
	 */
	struct SwitchEvent
	{
		RE::NiPointer<RE::NiAVObject> node;
		std::int32_t before = 0;
		bool structural = false;
	};
	inline EventQueue<SwitchEvent> switchEvents{ &WakeSceneSwitchEvent };
	constexpr std::size_t kSwitchIndex = 0x12C;

	inline std::int32_t& SwitchIndexOf(RE::NiAVObject* a_switch)
	{
		return *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::byte*>(a_switch) + kSwitchIndex);
	}

	// The render thread (Skyrim's main thread), recorded at every Present and frame start (NoteLoadingScreen, BeginFrame). A switch event is taken only there:
	// a loader thread builds subtrees that are not in the scene yet (their attach brings the switches up to date,
	// AddSubtree), and a reference taken to a node a loader is still assembling, released later on another thread,
	// is not safe (a QueuedTree load crashed on a freed child under a tree's switch with these events taken there).
	inline std::atomic<std::uint32_t> switchEventThread{ 0 };

	inline void PushSwitch(RE::NiAVObject* a_switch, std::int32_t a_before, bool a_structural)
	{
		if (a_switch && ::GetCurrentThreadId() == switchEventThread.load(std::memory_order_relaxed))
			switchEvents.Push(SwitchEvent{ RE::NiPointer<RE::NiAVObject>(a_switch), a_before, a_structural });
	}

	/** @brief The patched stores' handler (SwitchStoreStubs): the store, and an event when it changed the index. */
	inline void SwitchIndexStore(RE::NiAVObject* a_switch, std::int32_t a_index)
	{
		auto& index = SwitchIndexOf(a_switch);
		const std::int32_t before = index;
		index = a_index;
		if (before != a_index) {
			// T6b1a: the mirror's value, on any thread (a value, no reference; a switch the mirror does not hold is captured whole
			// at its attach); the tracking's event on the render thread alone.
			PushNodeUpdate(a_switch, SceneCapture::NodeRecord::kSwitch);
			PushSwitch(a_switch, before, false);
		}
	}

	/** @brief After one of NiSwitchNode's own child edits (its vtable's implementations). */
	inline void PushSwitchStructural(RE::NiNode* a_switch)
	{
		PushNodeUpdate(a_switch, SceneCapture::NodeRecord::kSwitch);
		PushSwitch(a_switch, SwitchIndexOf(a_switch), true);
	}

	inline bool switchEventsInstalled = false;
	inline bool moveEventsInstalled = false;

	/** @brief Patches the stores (after checking every site's bytes; none is patched when one differs). */
	bool InstallSwitchStores();

	/** @brief The move writers are hooked: the light path places a mover only when its reference (or its category node) had a move event this frame or the last. */
	inline bool MoveEventsLive() { return moveEventsInstalled; }
	/**
	 * @brief The switch-selection events are installed (false only when the engine's writers are not the expected code): every writer of an
	 * NiSwitchNode's selected index and of its children is hooked, the newly selected child is brought up to date when the event is applied
	 * (CatchUpSwitch), and an entry under a switch is no longer evaluated every frame for its selection.
	 */
	inline bool SwitchEventsLive() { return switchEventsInstalled; }
}
