#pragma once

#include "Features/DrawcallLimitFix/Engine/SceneCapture.h"

#include <atomic>
#include <memory>
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
			Updated,  // step 6e F3b: a hook's values for the mirror (update), in the same order as the attaches and detaches
		};

		struct Event
		{
			Event* next = nullptr;
			EventType type = EventType::Attached;
			RE::NiPointer<RE::NiAVObject> node;    // Attached: the subtree root
			std::vector<RE::BSGeometry*> removed;  // Detached: geometry leaves (identity only, never dereferenced)
			// Step 6e F3, the mirror's: an attach in the world, its subtree's and ancestors' records (SceneCapture::CaptureAttached);
			// a detach in the world, its root and its other nodes (keys).
			std::unique_ptr<SceneCapture::Records> captured;
			const void* detachedRoot = nullptr;
			std::vector<const void*> removedNodes;
			// The attached or detached node's ancestors at the hook (identities only, never dereferenced): the sun candidates among them
			// are planned again (their subtrees changed: SceneStore::sunEntriesForced).
			std::vector<const void*> ancestors;
			SceneCapture::Update update;
			// T6b1b: an update's own references (a leaf update's new properties: what the scene work holds its geometry's by), taken on
			// the writer's thread. The event's, not the update's: the mirror keeps copies of updates, whose last release would be the
			// scene work's. Released with the batch, on the render thread.
			std::vector<RE::NiPointer<RE::NiRefObject>> pins;
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
		static void CollectAncestors(const RE::NiAVObject* a_node, std::vector<const void*>& a_out);
		void PushDetached(RE::NiAVObject* a_child);
		/** @brief A hook's values (after its write, on the writer's thread): one stack with the attaches and detaches, so an address a
		 * detach let go and an attach took again is never given an earlier object's values. */
		void PushUpdate(SceneCapture::Update&& a_update, std::vector<RE::NiPointer<RE::NiRefObject>>&& a_pins = {});

		static void CollectGeometry(RE::NiAVObject* a_root, std::vector<RE::BSGeometry*>& a_out, std::vector<const void*>* a_nodes = nullptr);

	private:
		void Push(Event* a_event);

		std::atomic<Event*> head{ nullptr };
		std::atomic<bool> stopped{ false };
		bool installed = false;
	};
}
