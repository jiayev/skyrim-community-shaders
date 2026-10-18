// SceneTracker: the NiNode child-edit detours (on the editing thread) and the event stack they and the hooks push onto.

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"

#include <chrono>

namespace DCLF
{
	namespace
	{
		// NiNode vtable slots (SE/AE; VR shifts them, and the feature does not install there).
		constexpr std::size_t kAttachChild = 0x35;
		constexpr std::size_t kDetachChild1 = 0x37;
		constexpr std::size_t kDetachChild2 = 0x38;
		constexpr std::size_t kDetachChildAt1 = 0x39;
		constexpr std::size_t kDetachChildAt2 = 0x3A;
		constexpr std::size_t kSetAt1 = 0x3B;
		constexpr std::size_t kSetAt2 = 0x3C;

		RE::NiAVObject* ChildAt(RE::NiNode* a_node, std::uint32_t a_index)
		{
			auto& children = a_node->GetChildren();
			return a_index < children.free_idx() ? children[static_cast<std::uint16_t>(a_index)].get() : nullptr;
		}

		template <int N>
		struct AttachChildAt
		{
			static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child, bool a_firstAvail)
			{
				func(a_this, a_child, a_firstAvail);
				PrimaryCull::Get().NoteListStructure(a_this, a_child, true);
				SceneTracker::Get().PushAttached(a_child);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct DetachChild1At
		{
			static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child, RE::NiPointer<RE::NiAVObject>& a_out)
			{
				PrimaryCull::Get().NoteListStructure(a_this, a_child, false);
				SceneTracker::Get().PushDetached(a_child);
				func(a_this, a_child, a_out);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct DetachChild2At
		{
			static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child)
			{
				PrimaryCull::Get().NoteListStructure(a_this, a_child, false);
				SceneTracker::Get().PushDetached(a_child);
				func(a_this, a_child);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct DetachChildAt1At
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiPointer<RE::NiAVObject>& a_out)
			{
				PrimaryCull::Get().NoteListStructure(a_this, ChildAt(a_this, a_index), false);
				SceneTracker::Get().PushDetached(ChildAt(a_this, a_index));
				func(a_this, a_index, a_out);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct DetachChildAt2At
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index)
			{
				PrimaryCull::Get().NoteListStructure(a_this, ChildAt(a_this, a_index), false);
				SceneTracker::Get().PushDetached(ChildAt(a_this, a_index));
				func(a_this, a_index);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct SetAt1At
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiAVObject* a_child, RE::NiPointer<RE::NiAVObject>& a_out)
			{
				auto* previous = ChildAt(a_this, a_index);
				if (previous != a_child) {
					PrimaryCull::Get().NoteListStructure(a_this, previous, false);
					SceneTracker::Get().PushDetached(previous);
				}
				func(a_this, a_index, a_child, a_out);
				if (previous != a_child)
					PrimaryCull::Get().NoteListStructure(a_this, a_child, true);
				SceneTracker::Get().PushAttached(a_child);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <int N>
		struct SetAt2At
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiAVObject* a_child)
			{
				auto* previous = ChildAt(a_this, a_index);
				if (previous != a_child) {
					PrimaryCull::Get().NoteListStructure(a_this, previous, false);
					SceneTracker::Get().PushDetached(previous);
				}
				func(a_this, a_index, a_child);
				if (previous != a_child)
					PrimaryCull::Get().NoteListStructure(a_this, a_child, true);
				SceneTracker::Get().PushAttached(a_child);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FUN_140d1d630 (AE): every child of a node let go at once, not through the DetachChild slots (each child's parent and
		 * index cleared, its reference released, the count zeroed). Callers: Actor::DetachArrow (the nocked arrow at the release),
		 * Actor::RemoveWeapon, BSTaskPool's queued removal (task 0x3e) and others. Each child's detach before the engine's.
		 */
		struct RemoveAllChildren
		{
			static void thunk(RE::NiNode* a_this)
			{
				if (a_this) {
					const std::uint32_t count = a_this->GetChildren().free_idx();
					for (std::uint32_t i = 0; i < count; ++i)
						if (auto* child = ChildAt(a_this, i)) {
							PrimaryCull::Get().NoteListStructure(a_this, child, false);
							SceneTracker::Get().PushDetached(child);
						}
				}
				func(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		constexpr std::uintptr_t kRemoveAllChildren = 0xd1d630;

		template <class T>
		void DetourNiNodeSlot(std::size_t a_slot)
		{
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_NiNode[0] };
			const auto target = reinterpret_cast<const std::uintptr_t*>(vtable.address())[a_slot];
			stl::detour_thunk<T>(target);
		}

		/**
		 * @brief A node class's own implementation of a slot: detoured when it is not NiNode's (BSParticleSystemManager's child
		 * edits, which move world-space particle systems; BGSDecalNode's detaches and sets; BSFaceGenNiNode's attach), and not detoured
		 * already. One that calls NiNode's reports the edit twice: an attach tracked again, a detach whose records are gone.
		 */
		template <class T>
		void DetourOverride(const REL::VariantID& a_vtable, std::size_t a_slot, std::vector<std::uintptr_t>& a_done)
		{
			REL::Relocation<std::uintptr_t> base{ RE::VTABLE_NiNode[0] };
			REL::Relocation<std::uintptr_t> vtable{ a_vtable };
			const auto target = reinterpret_cast<const std::uintptr_t*>(vtable.address())[a_slot];
			if (target == reinterpret_cast<const std::uintptr_t*>(base.address())[a_slot] || std::ranges::find(a_done, target) != a_done.end())
				return;
			a_done.push_back(target);
			stl::detour_thunk<T>(target);
		}

		// The watched subtree's root (WatchSubtree): compared by the hooks, never dereferenced but for its descendants' parent chains.
		std::atomic<const RE::NiAVObject*> watchedRoot{ nullptr };

		/** @brief Whether a_object is a_root or under it (the hook's thread, the chain alive: the engine edits or stores under it). */
		bool Under(const RE::NiAVObject* a_object, const RE::NiAVObject* a_root)
		{
			for (std::uint32_t depth = 0; a_object && depth <= Engine::kMaxParentDepth; ++depth, a_object = a_object->parent)
				if (a_object == a_root)
					return true;
			return false;
		}

		// The watched category nodes (WatchCategoryNodes): an immutable sorted set the hooks read lock-free. A hook counts itself in
		// (categoryReaders) around its read; a replaced set is freed by the render thread once it sees no reader in (RetireCategoryNodes).
		std::atomic<const std::vector<const void*>*> categoryNodes{ nullptr };
		std::atomic<std::uint32_t> categoryReaders{ 0 };  // also counts the hidden stores' reads of subtreeMembers
		// The watched subtree's members (WatchSubtreeMembers): an immutable sorted set, read and retired like categoryNodes.
		std::atomic<const std::vector<const void*>*> subtreeMembers{ nullptr };
		std::unique_ptr<const std::vector<const void*>> subtreeCurrent;
		std::atomic<std::uint64_t> categoryEdits{ 0 };
		std::unique_ptr<const std::vector<const void*>> categoryCurrent;                // render thread: the published set's owner
		std::vector<std::unique_ptr<const std::vector<const void*>>> categoryRetired;  // render thread: replaced, maybe still read
		std::atomic<std::uint32_t> renderThread{ 0 };

		// The window premise's counters (TakeWindowEdits).
		std::atomic<std::uint64_t> windowEditsRender{ 0 }, windowEditsOther{ 0 };
		std::atomic<std::uint32_t> windowEditFirst{ 0 };

		template <int N>
		void DetourOverrides(const REL::VariantID& a_vtable, std::vector<std::uintptr_t>& a_done)
		{
			DetourOverride<AttachChildAt<N>>(a_vtable, kAttachChild, a_done);
			DetourOverride<DetachChild1At<N>>(a_vtable, kDetachChild1, a_done);
			DetourOverride<DetachChild2At<N>>(a_vtable, kDetachChild2, a_done);
			DetourOverride<DetachChildAt1At<N>>(a_vtable, kDetachChildAt1, a_done);
			DetourOverride<DetachChildAt2At<N>>(a_vtable, kDetachChildAt2, a_done);
			DetourOverride<SetAt1At<N>>(a_vtable, kSetAt1, a_done);
			DetourOverride<SetAt2At<N>>(a_vtable, kSetAt2, a_done);
		}
	}

	SceneTracker& SceneTracker::Get()
	{
		static SceneTracker tracker;
		return tracker;
	}

	void SceneTracker::Install()
	{
		if (installed)
			return;

		// Detour the NiNode implementations rather than the NiNode vtable, so every node class
		// that inherits them (BSFadeNode, BSMultiBoundNode, BSLeafAnimNode, ...) is covered.
		DetourNiNodeSlot<AttachChildAt<0>>(kAttachChild);
		DetourNiNodeSlot<DetachChild1At<0>>(kDetachChild1);
		DetourNiNodeSlot<DetachChild2At<0>>(kDetachChild2);
		DetourNiNodeSlot<DetachChildAt1At<0>>(kDetachChildAt1);
		DetourNiNodeSlot<DetachChildAt2At<0>>(kDetachChildAt2);
		DetourNiNodeSlot<SetAt1At<0>>(kSetAt1);
		DetourNiNodeSlot<SetAt2At<0>>(kSetAt2);
		// The engine's remove-all, which bypasses the slots.
		stl::detour_thunk<RemoveAllChildren>(REL::Offset(kRemoveAllChildren).address());
		// The node classes that override them (without these, a particle system the manager moves is missed).
		{
			std::vector<std::uintptr_t> done;
			DetourOverrides<1>(RE::VTABLE_BSParticleSystemManager[0], done);
			DetourOverrides<2>(RE::VTABLE_BGSDecalNode[0], done);
			DetourOverrides<3>(RE::VTABLE_BSFaceGenNiNode[0], done);
			logger::info("[DCLF] scene tracking: {} node classes' own child edits detoured", done.size());
		}

		installed = true;
		logger::info("[DCLF] Scene tracking hooks installed");
	}

	void SceneTracker::Push(Event* a_event)
	{
		// Stamped for the latency histograms (the scene pump's), then the pump woken for what moves latency (the source decided
		// before the push: the event is the consumer's after it): an attach in the world, a detach, a hidden bit, a leaf swapped, a
		// property's or an alpha's values (WakeScenePass holds these during the frame's render). A node's or a geometry's values
		// (transforms, the cull's fade state, a switch's index, segments) wake nothing: the frame's next pass takes them.
		SceneWake source = SceneWake::Count;
		switch (a_event->type) {
		case EventType::Attached:
			// In the world alone (its capture: CaptureAttached captures only what is): a loader's subtree attached out of the world wakes
			// nothing (most such passes would change nothing); its world attach does. The event still goes to the mirror.
			if (a_event->captured)
				source = SceneWake::Attach;
			break;
		case EventType::Detached:
			source = SceneWake::Detach;
			break;
		case EventType::Updated:
			if (a_event->update.hiddenSite != ~0u)
				source = SceneWake::Hidden;
			else if (a_event->update.leaf)
				source = SceneWake::Leaf;
			else if (std::holds_alternative<SceneCapture::PropertyRecord>(a_event->update.record))
				source = SceneWake::PropertyUpdate;
			else if (std::holds_alternative<SceneCapture::AlphaRecord>(a_event->update.record))
				source = SceneWake::AlphaUpdate;
			break;
		case EventType::Stacked:
			// By its channel (Api::WakeSource::Pass: the LOD gates', whose incoming blocks are withheld from the claims and hidden from the
			// engine until the coordinator flips the gate).
			if (a_event->stackedWakes)
				source = SceneWake::Channel;
			break;
		}
		// Only what wakes is stamped: the latencies measure what is meant to reach the frame at once, not what the frame's own pass takes.
		if (source != SceneWake::Count) {
			a_event->stampNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			a_event->stampFrame = SceneCapture::Frame();
		}
		a_event->next = head.load(std::memory_order_relaxed);
		while (!head.compare_exchange_weak(a_event->next, a_event, std::memory_order_release, std::memory_order_relaxed)) {
		}
		if (source != SceneWake::Count)
			WakeScenePass(source);
	}

	void SceneTracker::PushCaptured(Event* a_event, bool a_fillOnly)
	{
		if (!a_event)
			return;
		if (stopped.load(std::memory_order_acquire)) {
			delete a_event;
			return;
		}
		a_event->mirrorOnly = true;
		a_event->mirrorFill = a_fillOnly;
		Push(a_event);
	}

	void SceneTracker::Stop()
	{
		stopped.store(true, std::memory_order_release);
		FreeEvents(Drain());
	}

	void SceneTracker::CollectAncestors(const RE::NiAVObject* a_node, std::vector<const void*>& a_out)
	{
		// On the hook's thread, while the engine holds the chain (the attach or detach is in progress under it).
		for (const RE::NiAVObject* parent = a_node ? a_node->parent : nullptr; parent && a_out.size() < 32; parent = parent->parent)
			a_out.push_back(parent);
	}

	void SceneTracker::PushAttached(RE::NiAVObject* a_child)
	{
		if (!a_child || stopped.load(std::memory_order_acquire))
			return;
		auto* event = new Event{};
		event->type = EventType::Attached;
		event->node.reset(a_child);
		CollectAncestors(a_child, event->ancestors);
		// The mirror's records, on this thread after the engine's attach: only what is in the world.
		event->captured = SceneCapture::CaptureAttached(*a_child);
		// After the engine's attach: the child's parent is the node it went under.
		NoteEdit(a_child, event->captured != nullptr);
		if (const auto* root = watchedRoot.load(std::memory_order_acquire); root && a_child != root && Under(a_child, root))
			SubtreeEvents().Push(SubtreeEvent{ a_child, SubtreeEvent::Kind::Attached });
		Push(event);
	}

	void SceneTracker::PushDetached(RE::NiAVObject* a_child)
	{
		if (!a_child || stopped.load(std::memory_order_acquire))
			return;
		// Before the engine's detach (which follows the hook): the child's parent is still the node it leaves.
		const bool inWorld = SceneCapture::InWorld(a_child);
		NoteEdit(a_child, inWorld);
		// The watched subtree's keys it takes away, each named (pushed before the engine can free any of them).
		if (const auto* root = watchedRoot.load(std::memory_order_acquire); root && a_child != root && Under(a_child, root))
			Engine::ForEachLive(a_child, [](RE::NiAVObject& a_object, std::uint32_t) { SubtreeEvents().Push(SubtreeEvent{ &a_object, SubtreeEvent::Kind::Removed }); });
		auto* event = new Event{};
		event->type = EventType::Detached;
		CollectAncestors(a_child, event->ancestors);
		// In the world: the mirror drops every record under it.
		CollectGeometry(a_child, event->removed, inWorld ? &event->removedNodes : nullptr);
		if (inWorld)
			event->detachedRoot = a_child;
		if (event->removed.empty() && !inWorld) {
			delete event;
			return;
		}
		Push(event);
	}

	void SceneTracker::PushStacked(std::uint32_t a_channel, std::uint64_t a_token, std::span<RE::NiAVObject* const> a_pinned, std::span<RE::NiAVObject* const> a_named,
		bool a_wakes)
	{
		if (!a_token || stopped.load(std::memory_order_acquire))
			return;
		auto* event = new Event{};
		event->type = EventType::Stacked;
		event->stackedChannel = a_channel;
		event->stackedWakes = a_wakes;
		event->stackedToken = a_token;
		// The pinned roots pinned here, while the pusher holds them (released with the batch, on the render thread: never on the scene
		// work); the named are keys alone (the pusher holds them: a LOD gate's outgoing, until their detach at a LOD drain after the flip).
		event->stackedPinned.reserve(a_pinned.size());
		event->pins.reserve(a_pinned.size());
		for (auto* root : a_pinned)
			if (root) {
				event->stackedPinned.push_back(root);
				event->pins.emplace_back(root);
			}
		event->stackedNamed.reserve(a_named.size());
		for (auto* root : a_named)
			if (root)
				event->stackedNamed.push_back(root);
		Push(event);
	}

	void SceneTracker::PushUpdate(SceneCapture::Update&& a_update, std::vector<RE::NiPointer<RE::NiRefObject>>&& a_pins)
	{
		if (!installed || stopped.load(std::memory_order_acquire))
			return;
		// A hidden store under the watched subtree: its new bit, for the subtree's bits (FrameGlobals), in order with its edits.
		if (a_update.hiddenSite != ~0u)
			if (watchedRoot.load(std::memory_order_acquire))
				if (const auto* record = std::get_if<SceneCapture::NodeRecord>(&a_update.record)) {
					// A member by lookup: the stored object may be under construction, its parent not yet written.
					const auto* node = static_cast<const RE::NiAVObject*>(record->key);
					categoryReaders.fetch_add(1, std::memory_order_seq_cst);
					const auto* members = subtreeMembers.load(std::memory_order_seq_cst);
					const bool member = node && members && std::ranges::binary_search(*members, static_cast<const void*>(node));
					categoryReaders.fetch_sub(1, std::memory_order_seq_cst);
					if (member)
						SubtreeEvents().Push(SubtreeEvent{ node, (record->flags & 1u) ? SubtreeEvent::Kind::Hidden : SubtreeEvent::Kind::Shown });
				}
		auto* event = new Event{};
		event->type = EventType::Updated;
		event->update = std::move(a_update);
		event->pins = std::move(a_pins);
		if (!event->update.sequence)
			event->update.sequence = SceneCapture::NextSequence();
		Push(event);
	}

	SceneTracker::Event* SceneTracker::Drain()
	{
		// The stack holds the newest event first; reverse it to replay in order.
		Event* newestFirst = head.exchange(nullptr, std::memory_order_acquire);
		Event* oldestFirst = nullptr;
		while (newestFirst) {
			Event* next = newestFirst->next;
			newestFirst->next = oldestFirst;
			oldestFirst = newestFirst;
			newestFirst = next;
		}
		return oldestFirst;
	}

	void SceneTracker::FreeEvents(Event* a_head)
	{
		while (a_head) {
			Event* next = a_head->next;
			delete a_head;
			a_head = next;
		}
	}

	void SceneTracker::CollectGeometry(RE::NiAVObject* a_root, std::vector<RE::BSGeometry*>& a_out, std::vector<const void*>* a_nodes)
	{
		if (!a_root)
			return;

		// Iterative walk; scene subtrees can be deep enough that recursion is a risk on loader threads.
		Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t) {
			if (auto* geometry = a_object.AsGeometry())
				a_out.push_back(geometry);
			else if (auto* node = a_object.AsNode(); node && a_nodes)
				a_nodes->push_back(node);
		});
	}

	void SceneTracker::NoteEdit(const RE::NiAVObject* a_child, bool a_inWorld)
	{
		// A watched category node attached or detached, or a child of one (the hook's thread: the child and its parent alive).
		categoryReaders.fetch_add(1, std::memory_order_seq_cst);
		if (const auto* set = categoryNodes.load(std::memory_order_seq_cst))
			if (std::ranges::binary_search(*set, static_cast<const void*>(a_child)) ||
				(a_child->parent && std::ranges::binary_search(*set, static_cast<const void*>(a_child->parent))))
				categoryEdits.fetch_add(1, std::memory_order_release);
		categoryReaders.fetch_sub(1, std::memory_order_seq_cst);
		// The frame start's leased captures push in order with the hooks only while nothing in the world is attached or detached inside
		// the window: what breaks that premise is counted.
		if (a_inWorld && EngineReadWindow::IsOpen()) {
			const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
			(thread == renderThread.load(std::memory_order_relaxed) ? windowEditsRender : windowEditsOther).fetch_add(1, std::memory_order_relaxed);
			std::uint32_t none = 0;
			windowEditFirst.compare_exchange_strong(none, thread, std::memory_order_relaxed);
		}
	}

	void SceneTracker::WatchSubtree(const RE::NiAVObject* a_root)
	{
		watchedRoot.store(a_root, std::memory_order_release);
	}

	void SceneTracker::WatchSubtreeMembers(std::vector<const void*> a_members)
	{
		std::erase(a_members, nullptr);
		std::ranges::sort(a_members);
		if (subtreeCurrent && *subtreeCurrent == a_members)
			return;
		auto set = std::make_unique<const std::vector<const void*>>(std::move(a_members));
		subtreeMembers.store(set.get(), std::memory_order_seq_cst);
		if (subtreeCurrent)
			categoryRetired.push_back(std::move(subtreeCurrent));
		subtreeCurrent = std::move(set);
	}

	EventQueue<SceneTracker::SubtreeEvent>& SceneTracker::SubtreeEvents()
	{
		static EventQueue<SubtreeEvent> queue;
		return queue;
	}

	void SceneTracker::WatchCategoryNodes(std::vector<const void*> a_nodes)
	{
		renderThread.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
		std::erase(a_nodes, nullptr);
		std::ranges::sort(a_nodes);
		a_nodes.erase(std::unique(a_nodes.begin(), a_nodes.end()), a_nodes.end());
		if (categoryCurrent && *categoryCurrent == a_nodes)
			return;
		auto set = std::make_unique<const std::vector<const void*>>(std::move(a_nodes));
		categoryNodes.store(set.get(), std::memory_order_seq_cst);
		if (categoryCurrent)
			categoryRetired.push_back(std::move(categoryCurrent));
		categoryCurrent = std::move(set);
	}

	std::uint64_t SceneTracker::CategoryEdits()
	{
		return categoryEdits.load(std::memory_order_acquire);
	}

	void SceneTracker::RetireCategoryNodes()
	{
		// Every set replaced before this load is unreachable once no hook is in: a hook that loaded one counted itself in first.
		if (!categoryRetired.empty() && categoryReaders.load(std::memory_order_seq_cst) == 0)
			categoryRetired.clear();
	}

	SceneTracker::WindowEdits SceneTracker::TakeWindowEdits()
	{
		WindowEdits edits;
		edits.renderThread = windowEditsRender.exchange(0, std::memory_order_relaxed);
		edits.otherThreads = windowEditsOther.exchange(0, std::memory_order_relaxed);
		edits.firstThread = windowEditFirst.exchange(0, std::memory_order_relaxed);
		return edits;
	}
}
