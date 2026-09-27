#include "SceneTracker.h"

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
			return a_index < children.size() ? children[static_cast<std::uint16_t>(a_index)].get() : nullptr;
		}

		struct AttachChild
		{
			static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child, bool a_firstAvail)
			{
				func(a_this, a_child, a_firstAvail);
				SceneTracker::Get().PushAttached(a_child);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DetachChild1
		{
			static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child, RE::NiPointer<RE::NiAVObject>& a_out)
			{
				SceneTracker::Get().PushDetached(a_child);
				func(a_this, a_child, a_out);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DetachChild2
		{
			static void thunk(RE::NiNode* a_this, RE::NiAVObject* a_child)
			{
				SceneTracker::Get().PushDetached(a_child);
				func(a_this, a_child);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DetachChildAt1
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiPointer<RE::NiAVObject>& a_out)
			{
				SceneTracker::Get().PushDetached(ChildAt(a_this, a_index));
				func(a_this, a_index, a_out);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DetachChildAt2
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index)
			{
				SceneTracker::Get().PushDetached(ChildAt(a_this, a_index));
				func(a_this, a_index);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SetAt1
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiAVObject* a_child, RE::NiPointer<RE::NiAVObject>& a_out)
			{
				auto* previous = ChildAt(a_this, a_index);
				if (previous != a_child)
					SceneTracker::Get().PushDetached(previous);
				func(a_this, a_index, a_child, a_out);
				SceneTracker::Get().PushAttached(a_child);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SetAt2
		{
			static void thunk(RE::NiNode* a_this, std::uint32_t a_index, RE::NiAVObject* a_child)
			{
				auto* previous = ChildAt(a_this, a_index);
				if (previous != a_child)
					SceneTracker::Get().PushDetached(previous);
				func(a_this, a_index, a_child);
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
		DetourNiNodeSlot<AttachChild>(kAttachChild);
		DetourNiNodeSlot<DetachChild1>(kDetachChild1);
		DetourNiNodeSlot<DetachChild2>(kDetachChild2);
		DetourNiNodeSlot<DetachChildAt1>(kDetachChildAt1);
		DetourNiNodeSlot<DetachChildAt2>(kDetachChildAt2);
		DetourNiNodeSlot<SetAt1>(kSetAt1);
		DetourNiNodeSlot<SetAt2>(kSetAt2);

		installed = true;
		logger::info("[DCLF] Scene tracking hooks installed");
	}

	void SceneTracker::Push(Event* a_event)
	{
		a_event->next = head.load(std::memory_order_relaxed);
		while (!head.compare_exchange_weak(a_event->next, a_event, std::memory_order_release, std::memory_order_relaxed)) {
		}
	}

	void SceneTracker::Stop()
	{
		stopped.store(true, std::memory_order_release);
		FreeEvents(Drain());
	}

	void SceneTracker::PushAttached(RE::NiAVObject* a_child)
	{
		if (!a_child || stopped.load(std::memory_order_acquire))
			return;
		auto* event = new Event{};
		event->type = EventType::Attached;
		event->node.reset(a_child);
		Push(event);
	}

	void SceneTracker::PushDetached(RE::NiAVObject* a_child)
	{
		if (!a_child || stopped.load(std::memory_order_acquire))
			return;
		auto* event = new Event{};
		event->type = EventType::Detached;
		CollectGeometry(a_child, event->removed);
		if (event->removed.empty()) {
			delete event;
			return;
		}
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

	void SceneTracker::CollectGeometry(RE::NiAVObject* a_root, std::vector<RE::BSGeometry*>& a_out)
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
				for (auto& child : node->GetChildren()) {
					if (child)
						stack.push_back(child.get());
				}
			}
		}
	}
}
