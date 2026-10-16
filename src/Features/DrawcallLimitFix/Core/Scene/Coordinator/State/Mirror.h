#pragma once

// The scene mirror and its reads (CoordinatorMirror): the mirror itself, the mirror-read parity's counters and deferred
// rechecks, the batch's pins and the references made from them, the mirror's capture prepares. Applied in ApplyEvents.cpp, read
// by every lane phase (Classification.cpp, Walk.cpp, Residents.cpp, Components.cpp, Placements.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorMirror
	{
		/** @brief The lane's mirror reads, checked against live reads under CS_DCLF_MIRROR_PARITY; counted per kind. */
		enum class MirrorRead : std::uint8_t
		{
			CategoryNode,
			Subtree,
			SwitchEvent,
			NodeEvent,
			HiddenChain,
			MoveKey,
			LightEntry,
			SunEntry,
			Reference,  // a geometry the mirror's walk found with no pin (lagged: the live scene no longer has it there)
			kCount
		};
		static constexpr std::array<const char*, static_cast<std::size_t>(MirrorRead::kCount)> kMirrorReadNames{ "category node", "subtree", "switch event", "node event",
			"hidden chain", "move key", "light entry", "sun entry", "unpinned geometry" };
		struct MirrorReadStats
		{
			std::array<std::uint64_t, static_cast<std::size_t>(MirrorRead::kCount)> reads{}, checked{}, lagged{}, differ{}, unmirrored{}, refused{};
			std::array<std::string, static_cast<std::size_t>(MirrorRead::kCount)> first;
		};
		mutable MirrorReadStats mirrorReads;
		/** @brief Where this thread counts mirror reads: its chunk's counters under a ShardScope, else the coordinator's. */
		static inline thread_local MirrorReadStats* mirrorReadSink = nullptr;
		/**
		 * @brief A differing read, checked again after the next batch from the mirror alone: agreeing, the mirror lagged; still differing,
		 * it is counted.
		 */
		struct DeferredRead
		{
			MirrorRead read;
			std::string what;
			std::function<bool()> recheck;
		};
		mutable std::vector<DeferredRead> deferredReads;
		// ApplyMirrorEvents' prepares: one per capture it applies (not a fill), in apply order; the pool from kMirrorFanoutRecords records.
		std::vector<SceneMirror::Prepared> mirrorPreps;
		static constexpr std::size_t kMirrorFanoutRecords = 256;
		/**
		 * @brief The applied batch's pins by key, valid while it is applied; Pinned makes a reference from one (null: none). Counted:
		 * references from pins, from live pointers, refused.
		 */
		ankerl::unordered_dense::map<const void*, RE::NiRefObject*> batchPins;
		struct ReferenceStats
		{
			std::uint64_t pins = 0, pinned = 0, live = 0, refused = 0;
			std::uint64_t properties = 0, propertiesRefused = 0;  // a tracked geometry's properties held anew, and with no pin
			std::uint64_t fadeNodes = 0, fadeNodesRefused = 0;   // their fade nodes held anew, and with no pin
			// Of the refused: copied from a root owner, requested, and the answers (held or none); the first few named.
			std::uint64_t fadeNodesFromRoots = 0, fadeNodesRequested = 0, fadeNodesAnswered = 0, fadeNodesAnsweredNone = 0;
			std::uint32_t fadeNodeNamed = 0;
			std::string fadeNodeFirst;
			std::uint64_t fadeRootsUnowned = 0;                  // a fade root (or a component's node) left unlisted for want of its entry's reference
			std::string firstPropertyRefused;
		} referenceStats;
		/** @brief Records written without the mirror's leaf, and those evaluated again when a capture named it. */
		struct LeafStats
		{
			std::uint64_t missing = 0, rescheduled = 0;
		} leafStats;
		/** @brief The scene mirror: the hooks' records, applied in event order (ApplyMirrorEvents); checked by CS_DCLF_MIRROR_PARITY. */
		SceneMirror mirror;
	};
}
