#include "SceneTracker.h"

#include "PrimaryCull.h"
#include "Features/DrawcallLimitFix/Common/SceneWake.h"

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

		template <class T>
		void DetourNiNodeSlot(std::size_t a_slot)
		{
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_NiNode[0] };
			const auto target = reinterpret_cast<const std::uintptr_t*>(vtable.address())[a_slot];
			stl::detour_thunk<T>(target);
		}

		/**
		 * @brief A node class's own implementation of a slot (T6b1b): detoured when it is not NiNode's (BSParticleSystemManager's child
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
		// The node classes that override them (T6b1b: a particle system the manager moved was missed).
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
		// T6b3d: stamped for the latency histograms (the scene pump's), then the pump woken for what moves latency (the source decided
		// before the push: the event is the consumer's after it): an attach in the world, a detach, a hidden bit, a leaf swapped, a
		// property's or an alpha's values (WakeScenePass holds these during the frame's render). A node's or a geometry's values
		// (transforms, the cull's fade state, a switch's index, segments) wake nothing: the frame's next pass takes them.
		SceneWake source = SceneWake::Count;
		switch (a_event->type) {
		case EventType::Attached:
			// In the world alone (its capture: CaptureAttached captures only what is): a loader's subtree attached out of the world wakes
			// nothing (w130: 488 of 827 attach passes changed nothing); its world attach does. The event still goes to the mirror.
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
		// The mirror's records, on this thread after the engine's attach (step 6e F3): only what is in the world.
		event->captured = SceneCapture::CaptureAttached(*a_child);
		Push(event);
	}

	void SceneTracker::PushDetached(RE::NiAVObject* a_child)
	{
		if (!a_child || stopped.load(std::memory_order_acquire))
			return;
		auto* event = new Event{};
		event->type = EventType::Detached;
		CollectAncestors(a_child, event->ancestors);
		// In the world (before the engine's detach, which follows the hook): the mirror drops every record under it (step 6e F3).
		const bool inWorld = SceneCapture::InWorld(a_child);
		CollectGeometry(a_child, event->removed, inWorld ? &event->removedNodes : nullptr);
		if (inWorld)
			event->detachedRoot = a_child;
		if (event->removed.empty() && !inWorld) {
			delete event;
			return;
		}
		Push(event);
	}

	void SceneTracker::PushUpdate(SceneCapture::Update&& a_update, std::vector<RE::NiPointer<RE::NiRefObject>>&& a_pins)
	{
		if (!installed || stopped.load(std::memory_order_acquire))
			return;
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
		std::vector<RE::NiAVObject*> stack;
		stack.push_back(a_root);
		while (!stack.empty()) {
			RE::NiAVObject* object = stack.back();
			stack.pop_back();
			if (auto* geometry = object->AsGeometry()) {
				a_out.push_back(geometry);
				continue;
			}
			if (auto* node = object->AsNode()) {
				if (a_nodes)
					a_nodes->push_back(node);
				for (auto& child : node->GetChildren()) {
					if (child)
						stack.push_back(child.get());
				}
			}
		}
	}
}
