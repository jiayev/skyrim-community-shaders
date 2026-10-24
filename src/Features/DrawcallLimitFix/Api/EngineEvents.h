#pragma once

// The engine event contracts: what DCLF's engine side tells a feature that follows the engine state DCLF takes over, as it changes
// (Api::InstallContext::RegisterStandInObserver: the primary cull's stood-in roots, Core/Handoff/PrimaryCull), and the engine event
// channels a feature's own engine hooks feed, which the coordinator drains into its passes (Api::InstallContext::RegisterEngineChannel).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/EngineEvents.h), which the features inside DCLF include for them.
	class StandInRootsChange;
	class KeySink;

	/**
	 * @brief An observer of the primary cull's stood-in roots: the fade roots DCLF stands in for whole (state on the GPU alone; no engine
	 * cull visits them). Called per change, never per root; the roots are alive during the call (the cut's snapshot holds them).
	 *
	 * - changed: render thread (the main thread), when the admitted entries' ownership moved (PrimaryCull::SyncRootOwnership): the roots
	 *   that left and joined, left first (a root in both stays). Optional.
	 * - cleared: render thread, when the stand-in is dropped whole (PrimaryCull::ClearRootOwnership): no root is stood in. Optional.
	 * - updateEntry: the main thread at Main::Update's entry (PrimaryCull::AtUpdateEntry), after the stood-in roots' kAccumulated is
	 *   written, in registration order (the fade writers' tables and stores are the last's, Fades'): whether DCLF runs this frame. Optional.
	 * - afterWrites: the main thread at Main::Update's entry too, after every updateEntry and the UpdateEntry diagnostics event, last
	 *   there: whether DCLF runs this frame. Optional.
	 * - structure: any thread (the attaching or detaching one), per attach/detach the primary cull's structure events follow
	 *   (PrimaryCull::NoteListStructure), first there, before the main cull input's: a_parent, the node attached to or detached from (an
	 *   engine NiNode; null for none). Lock-free and cheap. Optional.
	 * - listJobsDone: render thread, once a frame the list jobs ran, at their Finish (PrimaryCull's ListJobsFinish, where the stand-in's
	 *   jobs are done and before the registration jobs read their output): what the feature's hooks gathered on the jobs' threads is
	 *   whole (the animated fades' batch). Optional.
	 */
	struct StandInObserver
	{
		void (*changed)(const StandInRootsChange& a_change) = nullptr;
		void (*cleared)() = nullptr;
		void (*updateEntry)(bool a_running) = nullptr;
		void (*afterWrites)(bool a_running) = nullptr;
		void (*structure)(const void* a_parent) = nullptr;
		void (*listJobsDone)() = nullptr;
	};

	/** @brief What an engine channel carries. */
	enum class ChannelKind : std::uint8_t
	{
		// Keys: the objects the hooked writers wrote, as keys pushed after the write (never dereferenced by the drain), from any thread.
		Keys,
		// SceneStack: events on the scene's event stack itself (Extensions::PushStacked), ordered with the attaches and detaches pushed
		// around them on the same thread; applied in stack order (CoordinatorComponent::Events::stacked). No queue, no drain.
		SceneStack,
	};

	/**
	 * @brief Where a pass applies a channel's keys (Core/Scene/Coordinator/ApplyEvents.cpp, ApplyBatch's structural events), and so where
	 * CollectEvents drains its queue among Core's own (each queue drained up to where it stands at its turn: what is pushed after is the
	 * next pass's).
	 *
	 * - BeforeStructural: drained after Core's fade snaps and fade amounts, before the property events; applied after Core's fade snaps and
	 *   fade amounts, before the property and node events are taken (an actor's fadeAmount, Actor::SetAlpha, would apply here).
	 * - AfterSwitches: drained after Core's switch events, last; applied after the switch events are pending, before the leaf holds (object
	 *   LOD's segment writes: Features/Lod).
	 * - AfterBatch: a SceneStack channel's: after the batch's events are applied (ApplyEvents), only in a pass that applies a batch; the
	 *   keys hook is called with no keys (what it applies is what the component took itself, CoordinatorComponent::Commit::take).
	 */
	enum class ChannelApply : std::uint8_t
	{
		BeforeStructural,
		AfterSwitches,
		AfterBatch,
	};

	/** @brief What wakes the scene pass for a channel (Extensions::WakeChannel): None, nothing; Pass, a scene pass (WakeScenePass). */
	enum class WakeSource : std::uint8_t
	{
		None,
		Pass,
	};

	/** @brief A registered engine channel (RegisterEngineChannel's result, FindEngineChannel's): the keys hook names its channel by it. */
	struct EngineChannelId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
		bool operator==(const EngineChannelId&) const = default;
	};

	/**
	 * @brief An engine event channel: a feature's own queue its engine hooks push into (an EventQueue of keys, Core/Runtime/EventQueue.h:
	 * its capacity and its wake, WakeScenePass or none, are the feature's), which every scene pass drains into its batch and applies at
	 * its point, handing the keys to the coordinator components' keys hooks (Api::CoordinatorComponent::Events, every pass that applies a
	 * batch, the keys possibly none).
	 *
	 * - name: the channel's (FindEngineChannel; the install's failures).
	 * - kind, apply, wake: above.
	 * - namesKeys: the keys a pass applies are objects whose writers' events carry no value of their own, which the mirror's parity counts
	 *   named (a field its probe finds different on one has a hook).
	 * - drain: the scene lane, the queue's one consumer (CollectEvents, once a pass): every key pushed before the call into a_sink, oldest
	 *   first (EventQueue::Drain(a_sink)); returns how many. None for a SceneStack channel.
	 * - push (optional): any thread, lock-free, after the store it reports: a_key into the channel's queue, for another feature's hooks
	 *   (InstallContext::NeedEngineChannel, Extensions::PushChannelKey: Features/Actors' Actor::SetAlpha into the fade amounts). Whatever
	 *   else the channel's own hooks do with the store (the mirror's value: Extensions::PushNodeUpdate) it does too. None: the channel
	 *   takes no pushes but its own.
	 */
	struct EngineChannel
	{
		std::string_view name;
		ChannelKind kind = ChannelKind::Keys;
		ChannelApply apply = ChannelApply::AfterSwitches;
		WakeSource wake = WakeSource::None;
		bool namesKeys = false;
		std::size_t (*drain)(KeySink& a_sink) = nullptr;
		void (*push)(const void* a_key) = nullptr;
	};

	/**
	 * @brief A SceneStack channel's event as a pass applies it (CoordinatorComponent::Events::stacked), in stack order: its token, the
	 * roots the event pinned (alive while the batch holds its pins) and the roots it only names (keys, never dereferenced), and its push
	 * (steady clock ns, the render thread's frame then).
	 */
	struct Stacked
	{
		std::uint64_t token = 0;
		std::span<const void* const> pinned, named;
		std::uint64_t stampNs = 0;
		std::uint32_t stampFrame = 0;
	};

	/**
	 * @brief The show veto (InstallContext::RegisterShowVeto; one): the hidden-node source's stubs on the engine's vetoable show stores
	 * test open (one dword compare) and, while it is not 0, call vetoes; a vetoed store returns unrun, without a hidden event. Core keeps
	 * the cells the stubs load (open defaults to a zero word, vetoes to a function answering false), so the stubs' shape does not depend
	 * on whether a veto is registered.
	 *
	 * - open: a word not 0 while anything may be vetoed (any thread; read in the stub).
	 * - vetoes: any thread, lock-free: whether a_object's show is skipped.
	 * - engineHid: any thread, after one of hideSites' engine stores hid a_object (a_siteOffset: the store's offset). Optional (parity).
	 * - hideSites: the engine hide stores' offsets engineHid is told of (copied).
	 */
	struct ShowVeto
	{
		const std::atomic<std::uint32_t>* open = nullptr;
		bool (*vetoes)(const void* a_object) = nullptr;
		void (*engineHid)(const void* a_object, std::uint32_t a_siteOffset) = nullptr;
		std::span<const std::uint32_t> hideSites;
	};

	/**
	 * @brief A move writer of a feature's own (Extensions::ReserveMoveWriter): an engine writer whose hook pushes move events (Core's
	 * MoveEvents, Core/Engine/Capture/SceneEvents.h) through Extensions::PushReservedMove, counted under its name in the report's move writers'
	 * calls.
	 */
	struct MoveWriterId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
		bool operator==(const MoveWriterId&) const = default;
	};

	/**
	 * @brief Whom a reserved writer's move event reaches: ScenePass, the scene pass's light path alone (MoveEvents); WithPlacements, also
	 * FrameValues' placement producer (FrameValueMoves), as Core's own writers push; Placements, the producer alone (a key only its
	 * index knows, such as a BSOrderedNode whose bound a leaf's forward sort reads).
	 */
	enum class MoveReach : std::uint8_t
	{
		ScenePass,
		WithPlacements,
		Placements,
	};
}

// Node capture contributions and the features' node updates (Core/Extensions/NodeCaptures.h).

#include <vector>

namespace DCLF::Api
{
	/**
	 * @brief A node capture contribution (InstallContext::RegisterNodeCapture): an engine node class (its RTTI exactly) whose OnVisible
	 * visits an ordered list of its own in its children's place (Features/Decals: a BGSDecalNode's decals, last to first). The scene
	 * capture records the list with the node's record (kMirroredKindCaptured; CoordinatorComponentContext::MirroredNodeOf's captured),
	 * and again on each update of kNodeFieldCaptured the feature's own hooks push (Extensions::PushNodeUpdate). One contribution a class.
	 *
	 * - name: the contribution's (the install's failures).
	 * - rtti: the node class's RTTI, called once at registration.
	 * - capture: the capturing thread (a hook's writer, a loader attaching, or the render thread), inside the engine-read window or under
	 *   the engine call that holds the node: a_node's list into a_out (empty on the call), in its order. Per captured node of the class,
	 *   never per frame.
	 */
	struct NodeCaptureContribution
	{
		std::string_view name;
		const void* (*rtti)() = nullptr;
		void (*capture)(const void* a_node, std::vector<const void*>& a_out) = nullptr;
	};

	/**
	 * @brief The node record fields a feature's hook may push (Extensions::PushNodeUpdate's a_fields; the capture's NodeRecord fields as
	 * they are, Core checks they match). Captured: a node capture contribution's list (its writers' edits). FadeAmount: a fade node's
	 * fadeAmount (+0x100: Actor::SetAlpha's, through the fade amounts channel's push).
	 */
	inline constexpr std::uint32_t kNodeFieldCaptured = 1u << 21;
	inline constexpr std::uint32_t kNodeFieldFadeAmount = 1u << 20;
}
