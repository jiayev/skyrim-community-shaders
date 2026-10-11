#pragma once

#include "Features/DrawcallLimitFix/Engine/SceneCapture.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <span>
#include <string>

namespace DCLF
{
	/**
	 * @brief The coordinator's copy of the engine's scene (dclf-async-publication.md, "F3"): the records the hooks captured,
	 * applied in event order. Owned by the scene work.
	 *
	 * Step F3 builds it beside the live reads and checks it (CS_DCLF_MIRROR_PARITY): the render thread captures a slice of the
	 * tracked objects live at the frame's start (the probe), and the scene work compares it with the mirror after the frame's
	 * events. A field that differs is
	 * - evented: an event without values named the object since it was last captured (a hook that carries no value yet);
	 * - late: the next batch names it (an update of the field, or an event without values: pushed after the drain);
	 * - missed: nothing named it (a writer no hook sees: F3c).
	 * An update (step F3b) names only its fields: a field it did not carry that differs is late or missed.
	 */
	class SceneMirror
	{
	public:
		using KeySet = ankerl::unordered_dense::set<const void*>;
		// By object and record type (Key), the fields the batch's updates carried.
		using FieldMap = ankerl::unordered_dense::map<std::uintptr_t, std::uint32_t>;
		static std::uintptr_t Key(const void* a_object, std::uint8_t a_type) { return reinterpret_cast<std::uintptr_t>(a_object) | a_type; }
		/** @brief The record type (node 0, geometry 1, property 2, alpha 3) and key of an update. */
		static std::pair<std::uint8_t, const void*> TypeOf(const SceneCapture::Update& a_update);

		/** @brief A capture's records, newest wins. */
		void Apply(const SceneCapture::Records& a_records);
		/** @brief A detached subtree: its root (taken off its parent's children) and every object under it. */
		void Detach(const void* a_root, std::span<RE::BSGeometry* const> a_geometries, std::span<const void* const> a_nodes);
		/**
		 * @brief T6b1b: a record whose object an event names attached out of the world: it left the world by no detach a hook saw (or its
		 * memory is another object's now), so it and every record under it go. a_what names the new object for the report.
		 */
		void Evict(const void* a_key, std::string_view a_what);
		/**
		 * @brief A hook's values into the record, when the mirror holds one (an object out of the world has none). Returns the
		 * record's type and key (a geometry named by its skin resolved), the key null when the mirror holds none.
		 */
		std::pair<std::uint8_t, const void*> Update(const SceneCapture::Update& a_update);
		/** @brief A new batch: the recent updates older than two batches go. */
		void BeginBatch();
		void Clear();

		const SceneCapture::NodeRecord* Node(const void* a_key) const;
		const SceneCapture::GeometryRecord* Geometry(const void* a_key) const;
		const SceneCapture::PropertyRecord* Property(const void* a_key) const;
		const SceneCapture::AlphaRecord* Alpha(const void* a_key) const;
		/** @brief T6b1b: a geometry's records (its node, parent, properties, alpha and fade node), null where the mirror holds none. */
		SceneCapture::LeafView Leaf(const void* a_key) const;
		/**
		 * @brief T6b3e: the record writes so far (Apply, Detach, Evict, Update, Clear), never reset: the scene pass's evaluations read the
		 * mirror from the pool and assert it unchanged across them (it is written by ApplyEvents alone).
		 */
		std::uint64_t Writes() const { return writes; }

		/**
		 * @brief The parity: the last probe's pending differences resolved by this batch's event keys (late or missed), then this
		 * probe against the mirror (a difference whose object an event of the probe's batch named is evented, else pending).
		 */
		void Check(const SceneCapture::Records& a_probe, const KeySet& a_eventKeys, const FieldMap& a_eventFields);
		/** @brief Since the last call: the records held and what the parity found. Empty when nothing to say. */
		std::string Report();
		/** @brief The last missed property's key (its flags differed with no event), taken: CS_DCLF_MIRROR_WATCH=parity arms on it. */
		const void* TakeMissedProperty() { return std::exchange(missedProperty, nullptr); }
		const void* TakeMissedAlpha() { return std::exchange(missedAlpha, nullptr); }
		const void* TakeMissedNode() { return std::exchange(missedNode, nullptr); }

	private:
		template <class T>
		struct Counted
		{
			T record;
			std::uint32_t uses = 0;
		};
		void Use(const void* a_property, const void* a_layer, const void* a_alpha, int a_delta);
		void ApplyCapture(const SceneCapture::Records& a_records);

		ankerl::unordered_dense::map<const void*, SceneCapture::NodeRecord> nodes;
		ankerl::unordered_dense::map<const void*, SceneCapture::GeometryRecord> geometries;
		ankerl::unordered_dense::map<const void*, Counted<SceneCapture::PropertyRecord>> properties;
		ankerl::unordered_dense::map<const void*, Counted<SceneCapture::AlphaRecord>> alphas;
		ankerl::unordered_dense::map<const void*, const void*> geometryBySkin;  // a skinned geometry record's skin instance -> its key
		// The last two batches' updates by object and record type (Key), for the captures numbered before them (Apply).
		struct Recent
		{
			std::uint64_t batch = 0;
			SceneCapture::Update update;
		};
		ankerl::unordered_dense::map<std::uintptr_t, std::vector<Recent>> recent;
		std::uint64_t batch = 0;
		std::uint64_t writes = 0;  // Writes()
		bool replaying = false;
		std::uint64_t replayed = 0, stale = 0, superseded = 0;
		const void* missedProperty = nullptr;
		const void* missedAlpha = nullptr;
		const void* missedNode = nullptr;  // a fade node stale in its currentFade (missed or evented), for CS_DCLF_MIRROR_WATCH=parity:current
		std::pair<std::uint8_t, const void*> UpdateRecord(const SceneCapture::Update& a_update);
		void Replay(std::uintptr_t a_key, std::uint64_t a_after);

		// The parity, by record type (node, geometry, property, alpha) and field.
		static constexpr std::size_t kTypes = 4;
		static constexpr std::uint32_t kMaxFields = 32;  // a record's fields at most (their bits)
		struct Pending
		{
			std::uint8_t type = 0;
			std::uint32_t fields = 0;
			std::string what;
			std::uint32_t flagBits = 0;  // a node's: the flag bits that differ
		};
		// By object and record type (a geometry's node and geometry records share its key).
		ankerl::unordered_dense::map<std::uintptr_t, Pending> pending;
		// The objects an event named since their last capture (Apply takes them off): a field that differs on one has a hook.
		KeySet named;
		struct Tally
		{
			std::uint64_t probes = 0, checked = 0, absent = 0;
			std::array<std::array<std::uint64_t, kMaxFields>, kTypes> evented{}, late{}, missed{};
			std::string firstAbsent, firstMissed, firstEvented;
			// Per type and field, the first missed object; per node flag bit, how often it differed (missed, evented).
			std::array<std::array<std::string, kMaxFields>, kTypes> firstMissedBy;
			std::array<std::uint64_t, 32> flagBitsMissed{}, flagBitsEvented{};
		} tally;
		std::uint64_t applied = 0, detached = 0, updates = 0, updatesUnheld = 0;
		std::uint64_t evictions = 0, evictedRecords = 0, lightEvictions = 0;  // lights: ShadowSceneNode's queued removal, no detach
		std::string firstEviction;
		// The updates applied, by record type and field.
		std::array<std::array<std::uint64_t, kMaxFields>, kTypes> updated{};
	};
}
