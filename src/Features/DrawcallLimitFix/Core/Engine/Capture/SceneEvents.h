#pragma once

// The scene capture's events: pushed by the hooked engine writers (SceneEvents.cpp; the hidden stores' are a feature's:
// Features/HiddenNodes) on any thread, drained by the coordinator's scene passes (keys, or held references) and the mirror (SceneTracker's
// updates).

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"

namespace DCLF::Scene
{
	/** @brief Installs every scene event hook below (render thread, PostPostLoad). */
	void InstallSceneEvents();
	/** @brief The hidden events' source is installed (hiddenEventsInstalled: Features/HiddenNodes' patched stores). */
	bool HiddenStoresLive();

	/**
	 * @brief The mirror's updates: values a hooked writer changed, captured after the write on its thread and pushed to
	 * SceneTracker in order with the attaches and detaches (applied in that order, SceneMirror::Update).
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

	/**
	 * @brief A geometry whose property or alpha was swapped: its leaf, applied as a capture when the mirror holds it. In the world
	 * only, as the mirror's records: a subtree out of it is captured whole at its world attach, and its pins could hold an object no
	 * owner references yet (a clone's root under construction, or the projectile pool's master clone, which the pool keeps by a raw
	 * pointer at refcount 0), whose release at Present would free it under its owner (dclf-architecture.md, "Engine references").
	 */
	inline void PushLeafUpdate(const RE::BSGeometry& a_geometry)
	{
		if (!SceneCapture::InWorld(&a_geometry))
			return;
		using G = SceneCapture::GeometryRecord;
		// A capture too: numbered at its start.
		const auto sequence = SceneCapture::NextSequence();
		SceneCapture::Update update{ G::kProperty | G::kAlpha | G::kLayer, SceneCapture::CaptureGeometry(a_geometry) };
		auto leaf = std::make_shared<SceneCapture::Records>();
		// Its properties, held by the event.
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
	// Its per-frame state: currentFade, the LOD level, the screen-door byte.
	constexpr std::uint32_t kFadeState = SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeLevel | SceneCapture::NodeRecord::kFadeDoor;

	inline float CurrentFade(RE::BSFadeNode* a_node)
	{
		return a_node->GetRuntimeData().currentFade;
	}

	/**
	 * @brief SceneEvents: structural events (dclf-architecture.md, "Events, not polls"), pushed on any thread, drained by the scene pass
	 * (Coordinator::CollectEvents, ApplyEvents).
	 * - Property: BSShaderProperty::SetFlags (0x14147bee0) or SetMaterial (0x14147bff0) changed a shader property, or a
	 *   controller was added to one. A key only, never dereferenced.
	 * - Node: Havok wrote a node's transform from its rigid body (FUN_140ea55a0, called by every collision object class's
	 *   SetNodeTransformsFromWorldTransform and the island activation listener), or a controller was added to a node
	 *   (NiObjectNET::PrependController, FUN_140d268d0, called by NiTimeController::SetTarget). Holds a reference: the drain
	 *   walks the node's subtree and ancestors.
	 */
	inline EventQueue<const void*> propertyEvents{ &WakeScenePropertyEvent };
	// No wake: Havok writes movers' transforms every frame; the next pass takes them.
	inline EventQueue<RE::NiPointer<RE::NiAVObject>> nodeEvents;

	inline void PushProperty(const void* a_property) { propertyEvents.Push(a_property); }

	inline void PushNode(RE::NiAVObject* a_node)
	{
		if (a_node)
			nodeEvents.Push(RE::NiPointer<RE::NiAVObject>(a_node));
	}

	/**
	 * @brief LOD fade events: Lighting properties whose specularLODFade or envmapLODFade (+0x100, +0x104) changed.
	 * GetRenderPasses (BSLightingShaderProperty vtable slot 0x2A) writes them from the fade node's LOD metric whenever any view
	 * registers the object (skyrim-engine-notes.md, "LOD fades in GetRenderPasses"), possibly after the patch was sampled. The
	 * detour compares both floats around the call; the walk names dependents for the next shading sample (NameShadingEvents).
	 * Pushed from cull and accumulation jobs.
	 */
	// Per-frame: no wake, the next frame input's pass drains it.
	inline EventQueue<const void*> lodFadeEvents;
	inline bool lodFadeEventsInstalled = false;

	inline void DrainLodFadeEvents(std::vector<const void*>& a_out)
	{
		lodFadeEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
	}

	/**
	 * @brief Emittance events: a shared colour external emittance reads changed. A kExternalEmittance property's emissiveColor
	 * points (FUN_141480fc0, from the cell attach FUN_1402d2f60) at a light's colour (TESObjectLIGH +0x118, constant), a region's
	 * emittanceColor (TESRegion +0x40, blended from weather by the cell's emittance update FUN_1402b4390) or, for sky-lit movable
	 * statics, the sky's (Sky +0x9c, from FUN_14040ba70). Both updates write through Sky::SetColor (0x14040d970), whose detour
	 * pushes on change; the colour keys propertyDependents. Pushed from the main thread and cell update jobs.
	 */
	// Per-frame: no wake, the next frame input's pass drains it.
	inline EventQueue<const void*> emittanceEvents;

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
	 * @brief MoveEvents: keys of a reference whose 3D an update pass moved, or a category node whose subtree one did
	 * (dclf-architecture.md, "Movers by event"), or a particle system that took a new snapshot. Movers are placed every
	 * frame regardless (FrameValues samples every mover of the newest placement plan); what the keys gate is the scene pass's light
	 * path (a per-frame entry is evaluated only with an event naming one of its keys this frame or the last: Coordinator::MoverDue)
	 * and which still roots the plan lists (QueueRoots). Pushed after the write from the animation and cell jobs.
	 * - Actors: FUN_14066afa0 (start of every animation path: RunOneActorAnimationUpdateJob, Actor::UpdateAnimation,
	 *   PlayerCharacter::UpdateAnimation, Actor::FinishLoadGame); TESObjectREFR::Update3DPosition (0x1402d9800, called by
	 *   Actor::Update3DPosition); and Features/Actors' reserved writers (its early hooks): Actor::UpdateActor3DPosition
	 *   (0x14069eb80), the sky cell's skin job (FUN_140663430), the ragdoll job (FUN_140770dc0), per actor on its list.
	 * - Other references: a cell's animated references' UpdateSelectedDownwardPass (from FUN_1402b41a0, only in a visible room
	 *   or fading), the cell's dynamic node's (FUN_1402b3ae0, keyed by the node), graph-animated references' (FUN_1402f75a0,
	 *   from UpdateAnimationJob).
	 * - Node events (Havok writes, controller additions) name the reference above the node.
	 * - Particle systems: Features/Particles' reserved writer, ParticleSnapshots' NiParticleSystem update thunks (vtable 0x2C, 0x2D),
	 *   keyed by the system (the geometry), when a capture took a new snapshot of a followed system (the particles toggle on).
	 * A feature's writers are reserved (Extensions::ReserveMoveWriter) and push through Extensions::PushReservedMove.
	 */
	// Per-frame: no wake (thousands a frame); the next frame input's pass drains them.
	inline EventQueue<const void*> moveEvents;
	// Calls per writer (the order of moveWriterNames), for the report.
	inline std::array<std::atomic<std::uint64_t>, Extensions::kMoveWriterSlots> moveWriterCalls{};
	// Core's writers at their slots; the free ones (empty) are the reserved writers', named at their reservation (main thread, PostPostLoad).
	inline std::array<std::string_view, Extensions::kMoveWriterSlots> moveWriterNames{ "animation", "reference 3D position", {}, {}, {}, "graph animation",
		"update pass" };
	inline void CountMove(std::size_t a_writer) { moveWriterCalls[a_writer].fetch_add(1, std::memory_order_relaxed); }

	/**
	 * @brief HiddenEvents: a node whose kHidden bit (flags bit 0) a patched store changed, as a key, riding in its mirror update
	 * (Update::hiddenSite). An actor entry's verdict is retaken only when a node on its chain had one
	 * (Coordinator::hiddenDependents), and for all on witness frames (Policy::HiddenWitness).
	 *
	 * The channel is Core's; its source is a feature's (Features/HiddenNodes: every engine store that can change the bit, patched at its
	 * early hooks), which sets hiddenEventsInstalled and notes its sites (Extensions::NoteHiddenSourceSites: the reserved sites follow
	 * them) before DCLF's threads start. Without it the walk retakes every frame verdict (hiddenGating off), and neither the LOD gates
	 * nor the kept scene lists engage.
	 */
	struct HiddenEvent
	{
		const void* key = nullptr;
		std::uint32_t site = 0;  // the patched store's index (the source's: Features/HiddenNodes)
		bool hidden = false;     // the bit's value after the store
	};
	inline bool hiddenEventsInstalled = false;
	/**
	 * @brief A hidden store's event, on the storing thread after the store (a_site: the source's store, or a reserved site:
	 * Extensions::PushReservedHidden): the mirror's bit and the hidden event in one update, so both land in the same batch.
	 */
	void PushHiddenUpdate(const void* a_object, std::uint32_t a_site);

	inline void PushMove(const void* a_key)
	{
		if (a_key)
			moveEvents.Push(a_key);
	}

	/**
	 * @brief SwitchEvents: an NiSwitchNode's selection may have changed (dclf-architecture.md, "Switch nodes"), drained by the scene pass
	 * and applied by its walk (ApplySwitchEvents).
	 *
	 * NiSwitchNode::index (+0x12C) has no setter. Its only stores outside construction, cloning and loading (AE 1.6.1170, every
	 * `mov [reg+0x12c]` in .text):
	 * - BSTreeManager's LOD selection in Main::Update (FUN_140437e50, five stores; FUN_140438840, two) and the local map's
	 *   (FUN_140438580, one), on a tree's LOD switch (BSTreeNode +0x180); they leave the new child stale for
	 *   NiSwitchNode::OnVisible to update in the cull.
	 * - Harvesting (FUN_1401e8ef0, from TESObjectTREE::Activate and the flora's) and a harvestable's 3D setup (FUN_1401e9450),
	 *   on the switch under the reference's root; both update the switch right after.
	 * Each store is SceneWriterFramework's (SWF_EVENT_SWITCH_INDEX): SWF's stub stores, and on a change the event pushes the previous
	 * index (OnSwitchIndex). The tree manager can store twice in a pass (a level, then the far level), so events are judged by net
	 * change.
	 *
	 * AttachChild, DetachChild and SetAt reset revID to 1 (selected child stale without an index store), and DetachChild clears
	 * the index when the selected slot empties: those push a structural event.
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

	// The render (main) thread, recorded at every Present and frame start. Switch events are taken only there: loader threads build
	// subtrees not yet in the scene (their attach updates the switches, AddSubtree), and a reference taken to a node a loader is
	// still assembling is unsafe (a QueuedTree load crashed on a freed child under a tree's switch).
	inline std::atomic<std::uint32_t> switchEventThread{ 0 };

	inline void PushSwitch(RE::NiAVObject* a_switch, std::int32_t a_before, bool a_structural)
	{
		if (a_switch && ::GetCurrentThreadId() == switchEventThread.load(std::memory_order_relaxed))
			switchEvents.Push(SwitchEvent{ RE::NiPointer<RE::NiAVObject>(a_switch), a_before, a_structural });
	}

	/** @brief After one of NiSwitchNode's own child edits (its vtable's implementations). */
	inline void PushSwitchStructural(RE::NiNode* a_switch)
	{
		PushNodeUpdate(a_switch, SceneCapture::NodeRecord::kSwitch);
		PushSwitch(a_switch, SwitchIndexOf(a_switch), true);
	}

	inline bool switchEventsInstalled = false;
	// Core's move writers (InstallMoveEvents); the reserved writers not installed yet (Extensions::ReserveMoveWriter, MoveWriterInstalled).
	inline bool moveEventsInstalled = false;
	inline std::uint32_t moveWritersPending = 0;

	/** @brief The move writers are hooked (MoveEvents): Core's and every reserved one. */
	inline bool MoveEventsLive() { return moveEventsInstalled && moveWritersPending == 0; }
	/**
	 * @brief The switch events are installed (false when the engine's writers are not the expected code); a newly selected child
	 * is brought up to date when its event is applied (CatchUpSwitch).
	 */
	inline bool SwitchEventsLive() { return switchEventsInstalled; }
}
