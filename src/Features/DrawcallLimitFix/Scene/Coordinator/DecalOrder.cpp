// The kept decal draw order (Coordinator::OrderDecals) and its per-object ordinals, coordinator thread. Keys come from the scene
// mirror's parent chains, so the order needs no frame lists.

#include "Internal.h"
#include "DecalOrder.h"

#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"

namespace DCLF::Scene
{
	namespace
	{
		/** @brief VisitIndex from the parent's record: a decal node's decals (its record's, in array order), else its children by slot. */
		std::uint32_t VisitIndex(const SceneCapture::NodeRecord& a_parent, const void* a_child)
		{
			if (a_parent.kind & SceneCapture::kKindDecalNode) {
				const auto& decals = a_parent.decals;
				for (std::uint32_t j = 0; j < decals.size(); ++j)
					if (decals[j] && decals[j] == a_child)
						return static_cast<std::uint32_t>(decals.size()) - 1 - j;
				return ~0u;
			}
			for (std::uint32_t i = 0; i < a_parent.children.size(); ++i)
				if (a_parent.children[i] == a_child)
					return i;
			return ~0u;
		}

		/**
		 * @brief The order the scene gives a_geometry, without the frame's lists: whether a BSOrderedNode groups it, and its
		 * visit positions from the top of the scene graph down (NiNode slots do not move; a BGSDecalNode visits its decals
		 * last to first). Within a reference this is the registration's order exactly.
		 */
		bool SceneKeyOf(const SceneMirror& a_mirror, const void* a_geometry, OrderKey& a_out)
		{
			static const REL::Relocation<const RE::NiRTTI*> orderedNode{ RE::BSOrderedNode::Ni_RTTI };
			a_out = {};
			for (const void* key = a_geometry; key;) {
				const auto* record = a_mirror.Node(key);
				if (!record)
					return false;
				if (!record->parent)
					break;
				const auto* parent = a_mirror.Node(record->parent);
				if (!parent)
					return false;
				if (parent->rtti == orderedNode.get())
					a_out.ordered = true;
				const std::uint32_t at = VisitIndex(*parent, key);
				if (at == ~0u)
					return false;
				a_out.path.push_back(at);
				key = record->parent;
			}
			std::reverse(a_out.path.begin(), a_out.path.end());
			return true;
		}
	}

	namespace
	{
		struct OrderedDecal
		{
			std::uint64_t chain;
			OrderKey key;
			std::uint32_t object;
			// The BGSDecalNode on its chain, if any: its decals' places count from the end of the node's decal array
			// (VisitIndex), which an added or removed decal shifts for all of them, so they are keyed again together.
			const RE::NiAVObject* decalNode = nullptr;
		};

		std::string PathText(const std::vector<std::uint32_t>& a_path)
		{
			std::string text;
			for (const auto at : a_path)
				text += fmt::format("{}{}", text.empty() ? "" : " ", at);
			return text;
		}

		/** @brief The BGSDecalNode above a_object (its record's kind: the RTTI exactly). */
		const RE::NiAVObject* DecalNodeOf(const SceneMirror& a_mirror, const void* a_object)
		{
			const auto* record = a_object ? a_mirror.Node(a_object) : nullptr;
			for (const void* key = record ? record->parent : nullptr; key;) {
				const auto* node = a_mirror.Node(key);
				if (!node)
					return nullptr;
				if (node->kind & SceneCapture::kKindDecalNode)
					return static_cast<const RE::NiAVObject*>(key);
				key = node->parent;
			}
			return nullptr;
		}
		// Draw order within a chain: registered later is drawn earlier (RegisterPass prepends); the object last, for stability.
		bool DrawnBefore(const OrderedDecal& a_a, const OrderedDecal& a_b)
		{
			if (a_a.chain != a_b.chain)
				return a_a.chain < a_b.chain;
			if (Before(a_b.key, a_a.key))
				return true;
			if (Before(a_a.key, a_b.key))
				return false;
			return a_a.object < a_b.object;
		}
	}

	struct Coordinator::KeptDecalOrder
	{
		std::vector<OrderedDecal> entries;  // in draw order
	};

	namespace
	{
		/** @brief A decal's place in the order: its chain and key (none for an object with no geometry). */
		bool KeyedDecal(const Coordinator::Tables& a_tables, const SceneMirror& a_mirror, std::uint32_t a_object, std::uint64_t a_chain, OrderedDecal& a_out)
		{
			const auto* geometry = a_object < a_tables.objectGeometry.size() ? a_tables.objectGeometry[a_object] : nullptr;
			if (!geometry)
				return false;
			a_out = { a_chain, {}, a_object, DecalNodeOf(a_mirror, geometry) };
			SceneKeyOf(a_mirror, geometry, a_out.key);
			return true;
		}

		/** @brief The member decals' order made whole (every key taken now). */
		std::vector<OrderedDecal> WholeDecalOrder(const Coordinator::Tables& a_tables, const SceneMirror& a_mirror,
			const ankerl::unordered_dense::map<std::uint32_t, std::uint64_t>& a_members)
		{
			std::vector<OrderedDecal> ordered;
			ordered.reserve(a_members.size());
			for (const auto& [object, chain] : a_members)
				if (OrderedDecal entry; KeyedDecal(a_tables, a_mirror, object, chain, entry))
					ordered.push_back(std::move(entry));
			std::sort(ordered.begin(), ordered.end(), DrawnBefore);
			return ordered;
		}
	}

	Coordinator::DecalOrderComparison Coordinator::CompareDecalOrder() const
	{
		// The kept order against one made whole now (a key that went stale: a parent's children re-indexed under a kept decal).
		DecalOrderComparison out;
		static const std::vector<OrderedDecal> none;
		const auto& kept = keptDecals ? keptDecals->entries : none;
		const auto reference = WholeDecalOrder(tables, mirror, memberDecals);
		out.decals = reference.size();
		bool same = reference.size() == kept.size();
		for (std::size_t i = 0; same && i < kept.size(); ++i)
			same = reference[i].object == kept[i].object && reference[i].chain == kept[i].chain;
		// Why: a kept key no longer the decal's now (a decal node's places shift together, which keeps their order: counted, not a
		// difference), or an order the comparator does not hold (not a strict weak order).
		for (const auto& entry : kept) {
			OrderedDecal now;
			if (KeyedDecal(tables, mirror, entry.object, entry.chain, now) && (Before(now.key, entry.key) || Before(entry.key, now.key)) && out.staleKeys++ == 0) {
				const auto* geometry = tables.objectGeometry[entry.object];
				out.staleFirst = fmt::format("object {} '{}' (decal node {}, now {}): kept ordered {} path [{}], now ordered {} path [{}]", entry.object,
					geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", fmt::ptr(entry.decalNode), fmt::ptr(now.decalNode), entry.key.ordered,
					PathText(entry.key.path), now.key.ordered, PathText(now.key.path));
			}
		}
		for (std::size_t i = 1; i < reference.size(); ++i)
			out.unordered += DrawnBefore(reference[i], reference[i - 1]) ? 1u : 0u;
		if (!same) {
			out.differ = true;
			std::size_t at = 0;
			while (at < kept.size() && at < reference.size() && reference[at].object == kept[at].object)
				++at;
			out.first = fmt::format("{} kept, {} whole; first difference at {}: kept object {}, whole object {}", kept.size(), reference.size(), at,
				at < kept.size() ? kept[at].object : ~0u, at < reference.size() ? reference[at].object : ~0u);
		}
		return out;
	}

	void Coordinator::OrderDecals()
	{
		// The engine draws a decal chain (a group's technique bucket and sub-pass list) in reverse registration order, and
		// registers in the order of its scene lists (the OrderKey, which the decal order parity checks). Stable: the
		// scene's order, kept: a decal's key is taken when it joins (or is patched again), and a change re-keys only the
		// decals it names; the ordinals are counted again in one pass.
		if (!keptDecals)
			keptDecals = std::make_shared<KeptDecalOrder>();
		auto& kept = keptDecals->entries;
		// The observers' check of this pass (the kept order against one made whole) runs the order's upkeep even with no change.
		bool checked = false;
		Diagnostics::Notify(Diagnostics::DecalOrderDue{ *this, checked });
		if (!decalsRebuild && decalsChanged.empty() && !checked)
			return;
		if (tables.decalOrdinal.size() != tables.objects.size())
			tables.decalOrdinal.resize(tables.objects.size(), ~0u);

		auto keyed = [&](std::uint32_t a_object, std::uint64_t a_chain, OrderedDecal& a_out) { return KeyedDecal(tables, mirror, a_object, a_chain, a_out); };

		if (decalsRebuild) {
			kept = WholeDecalOrder(tables, mirror, memberDecals);
		} else if (!decalsChanged.empty()) {
			// The changed decals out, and those still members in again with their keys taken now; with them every kept decal under
			// a decal node one of them is (or was) under, whose places the node's decal array gives now.
			std::sort(decalsChanged.begin(), decalsChanged.end());
			decalsChanged.erase(std::unique(decalsChanged.begin(), decalsChanged.end()), decalsChanged.end());
			auto isChanged = [&](std::uint32_t a_object) { return std::binary_search(decalsChanged.begin(), decalsChanged.end(), a_object); };
			ankerl::unordered_dense::set<const RE::NiAVObject*> touched;
			for (const auto& entry : kept)
				if (entry.decalNode && isChanged(entry.object))
					touched.insert(entry.decalNode);
			std::vector<OrderedDecal> joined;
			for (const std::uint32_t object : decalsChanged)
				if (const auto it = memberDecals.find(object); it != memberDecals.end())
					if (OrderedDecal entry; keyed(object, it->second, entry)) {
						if (entry.decalNode)
							touched.insert(entry.decalNode);
						joined.push_back(std::move(entry));
					}
			std::vector<std::uint32_t> rekeyed;
			std::erase_if(kept, [&](const OrderedDecal& a_entry) {
				if (isChanged(a_entry.object))
					return true;
				if (!a_entry.decalNode || !touched.contains(a_entry.decalNode))
					return false;
				rekeyed.push_back(a_entry.object);
				return true;
			});
			for (const std::uint32_t object : rekeyed)
				if (const auto it = memberDecals.find(object); it != memberDecals.end())
					if (OrderedDecal entry; keyed(object, it->second, entry))
						joined.push_back(std::move(entry));
			std::sort(joined.begin(), joined.end(), DrawnBefore);
			const std::size_t middle = kept.size();
			kept.insert(kept.end(), std::make_move_iterator(joined.begin()), std::make_move_iterator(joined.end()));
			std::inplace_merge(kept.begin(), kept.begin() + middle, kept.end(), DrawnBefore);
		}
		const bool changed = decalsRebuild || !decalsChanged.empty();
		decalsChanged.clear();
		decalsRebuild = false;

		if (checked)
			Diagnostics::Notify<Diagnostics::ScenePoint::DecalsOrdered>();
		if (!changed)
			return;

		// The ordinals, counted again in draw order: a decal whose ordinal moved is a change (its draw input's), and so is one no
		// longer ordered.
		std::vector<std::pair<std::uint32_t, std::uint32_t>> before;
		before.reserve(decalOrdered.size());
		for (const std::uint32_t o : decalOrdered)
			if (o < tables.decalOrdinal.size()) {
				before.emplace_back(o, tables.decalOrdinal[o]);
				tables.decalOrdinal[o] = ~0u;
			}
		decalOrdered.clear();
		tables.decalCount = {};
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> previous(before.begin(), before.end());
		for (const auto& entry : kept) {
			const std::uint32_t group = static_cast<std::uint32_t>(entry.chain >> 60) - 1;
			// A skinned decal of several partitions takes a slot per partition (DecalSpanOf).
			const std::uint32_t ordinal = tables.decalCount[group];
			tables.decalCount[group] += DecalSpanOf(entry.object < tables.skinPartitions.size() ? tables.skinPartitions[entry.object] : 0u);
			tables.decalOrdinal[entry.object] = ordinal;
			decalOrdered.push_back(entry.object);
			const auto it = previous.find(entry.object);
			if (it == previous.end() || it->second != ordinal)
				tables.NoteChange(entry.object, kChangeBindings);
			if (it != previous.end())
				previous.erase(it);
		}
		// Decals no longer ordered (left the members, or no longer decals).
		for (const auto& [object, ordinal] : previous)
			if (ordinal != ~0u && object < tables.objects.size())
				tables.NoteChange(object, kChangeBindings);
	}
}
