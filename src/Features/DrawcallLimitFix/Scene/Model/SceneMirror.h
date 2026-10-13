#pragma once

#include "Features/DrawcallLimitFix/Engine/Access/Walkers.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <span>
#include <string>

namespace DCLF
{
	namespace Diagnostics
	{
		struct SceneView;
	}

	/**
	 * @brief The coordinator's copy of the engine's scene: the records the hooks captured, applied in event order. Owned and
	 * written by the scene work (ApplyEvents); the scene pass's pool evaluations read it while it is unchanged.
	 *
	 * CS_DCLF_MIRROR_PARITY checks it (Diagnostics/Scene, the mirror's records): a slice of the tracked objects captured live at the
	 * frame's start against the mirror after the frame's events.
	 */
	class SceneMirror
	{
		// The mirror's report and its check (Diagnostics/Reports/SceneCoordinator.cpp, Diagnostics/Scene).
		friend struct Diagnostics::SceneView;

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
		 * @brief A record whose object an event names attached out of the world: it left the world by no detach a hook saw (or its
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
		/** @brief A key's parent as its record names it (null: none, or no record): a mirror chain's step. */
		const void* Parent(const void* a_key) const
		{
			const auto* record = Node(a_key);
			return record ? record->parent : nullptr;
		}
		const SceneCapture::GeometryRecord* Geometry(const void* a_key) const;
		const SceneCapture::PropertyRecord* Property(const void* a_key) const;
		const SceneCapture::AlphaRecord* Alpha(const void* a_key) const;
		/** @brief A geometry's records (its node, parent, properties, alpha and fade node), null where the mirror holds none. */
		SceneCapture::LeafView Leaf(const void* a_key) const;
		/**
		 * @brief The record writes so far (Apply, Detach, Evict, Update, Clear), never reset: the scene pass's evaluations read the
		 * mirror from the pool and assert it unchanged across them (it is written by ApplyEvents alone).
		 */
		std::uint64_t Writes() const { return writes; }

		/**
		 * @brief A LOD gate's incoming root (Coordinator::ApplyGateEvent; cleared at the gate's outcome or its root's detach). Its own
		 * kHidden is the gate's - the engine side hides the block before its attach and vetoes the engine's shows until the flip - so the
		 * scene's hidden test ignores that one bit (Coordinator::HiddenForWalk): the blocks are classified, written, bound and committed
		 * while hidden. Kept beside the records, not in them: a capture replaces a record whole, and the parity compares records alone.
		 */
		void SetGated(const void* a_key, bool a_gated)
		{
			++writes;
			if (a_gated)
				gated.insert(a_key);
			else
				gated.erase(a_key);
		}
		bool Gated(const void* a_key) const { return !gated.empty() && gated.contains(a_key); }
		std::size_t GatedCount() const { return gated.size(); }

		// The record types (node 0, geometry 1, property 2, alpha 3) by name, and a record's field bits by name.
		static constexpr std::array<const char*, 4> kTypeNames{ "node", "geometry", "property", "alpha" };
		template <class Record>
		static std::string Fields(std::uint32_t a_fields)
		{
			std::string text;
			for (std::uint32_t f = 0; f < Record::kFieldCount; ++f)
				if (a_fields & (1u << f))
					text += fmt::format("{}{}", text.empty() ? "" : "+", Record::kFieldNames[f]);
			return text;
		}
		static constexpr std::size_t kTypes = 4;
		static constexpr std::uint32_t kMaxFields = 32;  // a record's fields at most (their bits)

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
		KeySet gated;              // SetGated
		bool replaying = false;
		std::uint64_t replayed = 0, stale = 0, superseded = 0;
		std::pair<std::uint8_t, const void*> UpdateRecord(const SceneCapture::Update& a_update);
		void Replay(std::uintptr_t a_key, std::uint64_t a_after);
		std::uint64_t applied = 0, detached = 0, updates = 0, updatesUnheld = 0;
		std::uint64_t evictions = 0, evictedRecords = 0, lightEvictions = 0;  // lights: ShadowSceneNode's queued removal, no detach
		std::string firstEviction;
		// The updates applied, by record type and field.
		std::array<std::array<std::uint64_t, kMaxFields>, kTypes> updated{};
	};

	/**
	 * @brief The mirror's records under a_root depth first, a_root included (ForEachLive's order): a_visit(key, record, state, below) for
	 * each, where below is the state its children take (a copy of its own: a_state at a_root), at most a_limit keys. The visit answers as
	 * ForEachLive's does. True when a key on the way had no record (it is passed over).
	 */
	template <class State, class Visit>
	bool ForEachMirror(const SceneMirror& a_mirror, const void* a_root, State a_state, Visit&& a_visit, std::size_t a_limit = ~std::size_t{ 0 })
	{
		bool missing = false;
		std::vector<std::pair<const void*, State>> stack{ { a_root, std::move(a_state) } };
		for (std::size_t visited = 0; !stack.empty() && visited < a_limit; ++visited) {
			auto [key, state] = std::move(stack.back());
			stack.pop_back();
			const auto* record = a_mirror.Node(key);
			if (!record) {
				missing = true;
				continue;
			}
			State below = state;
			const Engine::Walk walk = Engine::Detail::Answer(a_visit, key, *record, std::as_const(state), below);
			if (walk == Engine::Walk::Stop)
				return missing;
			if (walk == Engine::Walk::Skip)
				continue;
			for (const void* child : record->children)
				if (child)
					stack.emplace_back(child, below);
		}
		return missing;
	}

	/** @brief ForEachMirror without a state: a_visit(key, record). */
	template <class Visit>
	bool ForEachMirror(const SceneMirror& a_mirror, const void* a_root, Visit&& a_visit, std::size_t a_limit = ~std::size_t{ 0 })
	{
		return ForEachMirror(
			a_mirror, a_root, std::uint8_t{ 0 },
			[&](const void* a_key, const SceneCapture::NodeRecord& a_record, std::uint8_t, std::uint8_t&) { return Engine::Detail::Answer(a_visit, a_key, a_record); },
			a_limit);
	}
}
