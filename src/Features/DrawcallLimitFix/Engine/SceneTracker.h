#pragma once

#include <atomic>
#include <vector>

namespace DCLF
{
	/**
	 * @brief Reports scene graph attachments and detachments from any thread.
	 *
	 * The NiNode child-management functions are detoured once. Every attach becomes an
	 * event holding a reference to the attached subtree; every detach becomes an event
	 * holding the BSGeometry leaves the subtree contained, collected on the detaching thread
	 * while the subtree is still intact. Events go onto a lock-free stack and are drained,
	 * in order, by the render thread.
	 */
	class SceneTracker
	{
	public:
		enum class EventType : std::uint8_t
		{
			Attached,
			Detached,
		};

		struct Event
		{
			Event* next = nullptr;
			EventType type = EventType::Attached;
			RE::NiPointer<RE::NiAVObject> node;    // Attached: the subtree root
			std::vector<RE::BSGeometry*> removed;  // Detached: geometry leaves (identity only, never dereferenced)
		};

		static SceneTracker& Get();

		void Install();
		bool IsInstalled() const { return installed; }

		/**
		 * @brief Stops queueing events and frees the ones pending, for good. The detours stay installed and
		 * pass straight through: used when DCLF is forced off after install, with nothing left to drain the queue.
		 */
		void Stop();

		/** @brief Takes every pending event, oldest first. The caller owns the list (see FreeEvents). */
		Event* Drain();
		static void FreeEvents(Event* a_head);

		void PushAttached(RE::NiAVObject* a_child);
		void PushDetached(RE::NiAVObject* a_child);

		static void CollectGeometry(RE::NiAVObject* a_root, std::vector<RE::BSGeometry*>& a_out);

	private:
		void Push(Event* a_event);

		std::atomic<Event*> head{ nullptr };
		std::atomic<bool> stopped{ false };
		bool installed = false;
	};
}
