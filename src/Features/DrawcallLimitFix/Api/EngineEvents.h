#pragma once

// The engine event contracts: what DCLF's engine side tells a feature that follows the engine state DCLF takes over, as it changes
// (Api::InstallContext::RegisterStandInObserver: the primary cull's stood-in roots, Engine/Cull/PrimaryCull), and the engine event
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
	 * cull visits them), and the BSTreeNodes among them. Called per change, never per root; the roots are alive during the call (the
	 * cut's snapshot holds them).
	 *
	 * - changed: render thread (the main thread), when the admitted entries' ownership moved (PrimaryCull::SyncFadeOwnership): the roots
	 *   that left and joined, left first (a root in both stays). Optional.
	 * - cleared: render thread, when the stand-in is dropped whole (PrimaryCull::ClearFadeOwnership): no root is stood in. Optional.
	 * - updateEntry: the main thread at Main::Update's entry (PrimaryCull::AtUpdateEntry), after the stood-in roots' kAccumulated is
	 *   written: whether DCLF runs this frame. Optional.
	 */
	struct StandInObserver
	{
		void (*changed)(const StandInRootsChange& a_change) = nullptr;
		void (*cleared)() = nullptr;
		void (*updateEntry)(bool a_running) = nullptr;
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
	 * @brief Where a pass applies a channel's keys (Scene/Coordinator/ApplyEvents.cpp, ApplyBatch's structural events), and so where
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

	/**
	 * @brief A load screen's passes apply no batch (CollectEvents, loading): every channel is drained there, in registration order, after
	 * Core's fade amounts and before its switch events, and its keys:
	 *
	 * - KeepKeys: are kept for the mirror's parity (EventBatch::mirrorNamed, carried to the first pass after the load: the writers have
	 *   hooks), never applied;
	 * - Discard: are dropped.
	 */
	enum class ChannelLoading : std::uint8_t
	{
		KeepKeys,
		Discard,
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
	 * - kind, apply, loading, wake: above.
	 * - namesKeys: the keys a pass applies are objects whose writers' events carry no value of their own, which the mirror's parity counts
	 *   named (a field its probe finds different on one has a hook).
	 * - drain: the scene lane, the queue's one consumer (CollectEvents, once a pass): every key pushed before the call into a_sink, oldest
	 *   first (EventQueue::Drain(a_sink)); returns how many. None for a SceneStack channel.
	 */
	struct EngineChannel
	{
		std::string_view name;
		ChannelKind kind = ChannelKind::Keys;
		ChannelApply apply = ChannelApply::AfterSwitches;
		ChannelLoading loading = ChannelLoading::KeepKeys;
		WakeSource wake = WakeSource::None;
		bool namesKeys = false;
		std::size_t (*drain)(KeySink& a_sink) = nullptr;
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
	 * @brief An adoption guard (InstallContext::RegisterAdoptionGuard): a generation the frame samples at its start and every publication
	 * carries (the frame inputs' it was made under); a frame whose installed publication carries an older one than the frame's withdraws
	 * its claims (the engine draws) until one made after it is installed. At most Extensions::kAdoptionGuardSlots.
	 *
	 * - name: the guard's (the install's failures).
	 * - generation: any thread (the render thread at the frame's start): the current generation, never decreasing.
	 */
	struct AdoptionGuard
	{
		std::string_view name;
		std::uint64_t (*generation)() = nullptr;
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
}
