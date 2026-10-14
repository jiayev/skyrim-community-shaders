#pragma once

#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Runtime/EventQueue.h"

#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	/**
	 * @brief The scene's event stack: scene graph attaches and detaches, the hooks' value updates and LOD gates, from any thread.
	 *
	 * The NiNode child-management functions are detoured once. An attach becomes an event holding a reference to the attached subtree
	 * and, in the world, its records (SceneCapture::CaptureAttached); a detach becomes an event holding the subtree's BSGeometry leaves
	 * and nodes, collected on the detaching thread while the subtree is still intact. Every event goes onto one lock-free stack, so
	 * they apply in push order; the coordinator drains it at each scene pass (Coordinator::CollectEvents), and the drained events'
	 * references are released with their batch on the render thread.
	 */
	class SceneTracker
	{
	public:
		enum class EventType : std::uint8_t
		{
			Attached,
			Detached,
			Updated,  // a hook's values for the mirror (update), in the same order as the attaches and detaches
			Gate,     // a LOD swap's gate (LodGates::CaptureGate), after the incoming blocks' attaches on the swapping thread
		};

		struct Event
		{
			Event* next = nullptr;
			EventType type = EventType::Attached;
			RE::NiPointer<RE::NiAVObject> node;    // Attached: the subtree root
			std::vector<RE::BSGeometry*> removed;  // Detached: geometry leaves (identity only, never dereferenced)
			// For the mirror: an attach in the world, its subtree's and ancestors' records (SceneCapture::CaptureAttached); a detach in the
			// world, its root and its other nodes (keys).
			std::unique_ptr<SceneCapture::Records> captured;
			const void* detachedRoot = nullptr;
			std::vector<const void*> removedNodes;
			// The attached or detached node's ancestors at the hook (identities only, never dereferenced): the sun candidates among them
			// are planned again (their subtrees changed: Coordinator::sunEntriesForced).
			std::vector<const void*> ancestors;
			SceneCapture::Update update;
			// An update's own references (a leaf update's new properties: what the scene work holds its geometry's by), taken on the
			// writer's thread, or a Gate's incoming roots. The event's, not the update's: the mirror keeps copies of updates, whose last
			// release would be the scene work's. Released with the batch, on the render thread.
			std::vector<RE::NiPointer<RE::NiRefObject>> pins;
			// The render thread's captures at the frame's start (the category capture's portal roots, the mirror's requests:
			// PushCaptured), for the mirror alone - in order with the hooks' events, so a capture is never applied over a newer write -
			// and never a tracking attach or detach.
			bool mirrorOnly = false;
			// A mirror request's capture (FrameView::CaptureMirrorRequests): only the records the mirror lacks are taken from it (a record
			// the mirror holds is the events' to keep: the frame's start can catch an ancestor in a state the engine sets and undoes within
			// the frame, with no event).
			bool mirrorFill = false;
			// When an event that wakes the scene pass was pushed (steady clock, ns) and the render thread's frame then
			// (SceneCapture::Frame): the event-to-publication and event-to-adoption latencies.
			std::uint64_t stampNs = 0;
			std::uint32_t stampFrame = 0;
			// A Gate's: its token (LodGates', from 1), the incoming roots (the blocks' nodes: keys, each held by a pin in `pins` until
			// the batch is released on the render thread) and the outgoing roots (keys: attached and shown until the flip, held by the engine
			// side's stolen handles).
			std::uint64_t gateToken = 0;
			std::vector<const void*> gateIncoming, gateOutgoing;
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
		/**
		 * @brief Render thread: a capture of its own (the frame's start: the category capture's portal roots, the mirror's
		 * requests) onto the same stack, marked mirror-only (Event::mirrorOnly). The stack owns a_event from here.
		 */
		void PushCaptured(Event* a_event, bool a_fillOnly = false);
		/**
		 * @brief LodGates::CaptureGate: a LOD swap's gate, on the swapping thread under the terrain manager's lock, after the incoming
		 * blocks' attaches (one stack with them: the gate is applied after their captures). The incoming roots are pinned by the event.
		 */
		void PushGate(std::uint64_t a_token, std::span<RE::NiAVObject* const> a_incoming, std::span<RE::NiAVObject* const> a_outgoing);

		static void CollectGeometry(RE::NiAVObject* a_root, std::vector<RE::BSGeometry*>& a_out, std::vector<const void*>* a_nodes = nullptr);

		/**
		 * @brief An edit under the watched subtree (WatchSubtree), pushed by the hooks in order with each other: a hidden store's new
		 * bit, a detach's nodes and geometry (their keys leave the subtree: an address the engine frees and reuses is a new node) and an
		 * attach (the subtree's membership changed). The node is an identity, never dereferenced by the consumer.
		 */
		struct SubtreeEvent
		{
			enum class Kind : std::uint8_t
			{
				Hidden,
				Shown,
				Removed,
				Attached,
			};
			const RE::NiAVObject* node = nullptr;
			Kind kind = Kind::Hidden;
		};
		/**
		 * @brief Render thread: the root whose subtree's edits the hooks push onto SubtreeEvents from now on (FrameGlobals' first-person
		 * cull bits; null: none). The root itself is not of its subtree: its own hidden stores are not pushed.
		 */
		static void WatchSubtree(const RE::NiAVObject* a_root);
		/**
		 * @brief Render thread: the watched subtree's members as its consumer last walked them (keys, never dereferenced). A hidden store
		 * is pushed onto SubtreeEvents only for a member: the store's object may be under construction (its parent not yet written), so
		 * the hook looks it up instead of walking its parents. A node attached since the walk has no event; the walk the attach triggers
		 * reads its bit. Replaced sets are freed by RetireCategoryNodes.
		 */
		static void WatchSubtreeMembers(std::vector<const void*> a_members);
		/** @brief The watched subtree's edits, for their one consumer (FrameGlobals, render thread). */
		static EventQueue<SubtreeEvent>& SubtreeEvents();

		/**
		 * @brief Render thread: the nodes the category capture finds its category nodes through (FrameView::CategorySignature: TES's object
		 * root, the cells' 3D and their parents, the portal graphs' shared nodes and parentless roots). An attach or detach of one of them,
		 * or of a child of one, counts an edit (CategoryEdits), so the frame's start re-reads the signature only after one.
		 */
		static void WatchCategoryNodes(std::vector<const void*> a_nodes);
		/** @brief Any thread: the edits of watched category nodes since the start. */
		static std::uint64_t CategoryEdits();
		/** @brief Render thread, once at the frame's start (before any read of the watched set it replaces is retired): frees replaced sets no reader holds. */
		static void RetireCategoryNodes();

		/**
		 * @brief The ordering premise of the frame start's leased captures (FrameView::ServeFrameRequests): no attach or detach in the world
		 * while EngineReadWindow is open. What the hooks pushed against it since the last call: by the render thread and by others, and
		 * the first one's thread id.
		 */
		struct WindowEdits
		{
			std::uint64_t renderThread = 0, otherThreads = 0;
			std::uint32_t firstThread = 0;
		};
		static WindowEdits TakeWindowEdits();

	private:
		void Push(Event* a_event);
		static void NoteEdit(const RE::NiAVObject* a_child, bool a_inWorld);

		std::atomic<Event*> head{ nullptr };
		std::atomic<bool> stopped{ false };
		bool installed = false;
	};
}
