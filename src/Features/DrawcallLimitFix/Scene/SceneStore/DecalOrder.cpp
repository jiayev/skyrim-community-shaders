#include "Internal.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"

namespace DCLF
{
	namespace
	{
		using Engine::Global;
		constexpr std::uintptr_t kSceneLists = 0x338c870;      // BSTArray<NiPointer<NiAVObject>>*
		constexpr std::uintptr_t kSceneListCount = 0x338c868;  // std::uint32_t
		constexpr std::uintptr_t kExtraList = 0x338c888;       // BSTArray<NiPointer<NiAVObject>>, culled by the first job only
		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

		/**
		 * @brief Where the main registration reaches a geometry (skyrim-engine-notes.md, "The main registration's order"):
		 * its scene list, whether a BSOrderedNode groups it (registered after the list's plain appends), its root's
		 * position in the list, and the positions the cull visits on the way down.
		 */
		struct OrderKey
		{
			std::uint32_t list = 0;
			bool ordered = false;
			std::uint32_t position = 0;
			std::vector<std::uint32_t> path;
		};

		bool Before(const OrderKey& a_a, const OrderKey& a_b)
		{
			if (a_a.list != a_b.list)
				return a_a.list < a_b.list;
			if (a_a.ordered != a_b.ordered)
				return !a_a.ordered;
			if (a_a.position != a_b.position)
				return a_a.position < a_b.position;
			return std::lexicographical_compare(a_a.path.begin(), a_a.path.end(), a_b.path.begin(), a_b.path.end());
		}

		enum class Miss : std::uint8_t
		{
			None,
			NoRoot,     // no list entry on its parent chain
			DecalNode,  // under a BGSDecalNode, but none of its decals' 3D is on the chain
			NotChild,   // not among its parent's children
		};

		/** @brief a_child's place in the order a_parent's OnVisible visits: ~0u when it does not. */
		std::uint32_t VisitIndex(const RE::NiNode& a_parent, const RE::NiAVObject* a_child, Miss& a_miss)
		{
			static const REL::Relocation<const RE::NiRTTI*> decalNode{ RE::BGSDecalNode::Ni_RTTI };
			if (a_parent.GetRTTI() == decalNode.get()) {
				// BGSDecalNode::OnVisible (AE 1401fdf10) culls its decals' 3D (BSTempEffect::Get3D), last to first, and not its children.
				const auto& decals = static_cast<const RE::BGSDecalNode&>(a_parent).GetRuntimeData().decals;
				for (std::uint32_t j = 0; j < decals.size(); ++j)
					if (decals[j] && decals[j]->Get3D() == a_child)
						return decals.size() - 1 - j;
				a_miss = Miss::DecalNode;
				return ~0u;
			}
			const auto& children = a_parent.GetChildren();
			for (std::uint16_t i = 0; i < children.free_idx(); ++i)
				if (children[i].get() == a_child)
					return i;
			a_miss = Miss::NotChild;
			return ~0u;
		}

		struct DecalOrderProbe
		{
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::pair<std::uint32_t, std::uint32_t>> positions, lastPositions;
			std::uint64_t frames = 0, registrations = 0, chains = 0, pairs = 0, inversions = 0, ordered = 0, rootsMoved = 0;
			std::uint64_t decalRootFrames = 0, decalRootsReordered = 0;  // frames whose decal roots' relative order changed
			std::array<std::uint64_t, 4> misses{};
			std::string firstInversion;
			std::vector<const RE::NiAVObject*> decalRoots, lastDecalRoots;  // this frame's decal roots, in registration order
			std::map<std::string, std::uint32_t> unresolvedTops;
		};
		DecalOrderProbe decalOrderProbe;
	}

	namespace
	{

		Miss KeyOf(const RE::BSGeometry* a_geometry, const DecalOrderProbe& a_probe, OrderKey& a_out, const RE::NiAVObject*& a_root)
		{
			static const REL::Relocation<const RE::NiRTTI*> orderedNode{ RE::BSOrderedNode::Ni_RTTI };
			a_out = {};
			Miss miss = Miss::None;
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent) {
				if (const auto it = a_probe.positions.find(object); it != a_probe.positions.end()) {
					a_out.list = it->second.first;
					a_out.position = it->second.second;
					std::reverse(a_out.path.begin(), a_out.path.end());
					a_root = object;
					return Miss::None;
				}
				const auto* parent = object->parent;
				if (!parent) {
					a_root = object;
					break;
				}
				if (parent->GetRTTI() == orderedNode.get())
					a_out.ordered = true;
				const std::uint32_t at = VisitIndex(*parent, object, miss);
				if (at == ~0u)
					return miss;
				a_out.path.push_back(at);
			}
			return Miss::NoRoot;
		}

		/** @brief VisitIndex from the parent's record (T6b1b): a decal node's decals (its record's, in array order), else its children by slot. */
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
		 * last to first). Within a reference this is the registration's order exactly. The mirror's chain (T6b1b).
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

		std::string Named(const RE::NiAVObject* a_object)
		{
			const char* name = a_object ? a_object->name.c_str() : nullptr;
			return name && *name ? name : "-";
		}
	}

	void Scene::ProbeDecalOrder(std::span<const PassCapture::Entry> a_entries, const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>& a_main)
	{
		auto& p = decalOrderProbe;
		++p.frames;
		// This frame's scene lists: every root's list and position, and whether a root kept both since last frame.
		p.positions.clear();
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		if (auto* lists = Global<SceneList*>(kSceneLists))
			for (std::uint32_t l = 0; l < count; ++l)
				for (std::uint32_t i = 0; i < lists[l].size(); ++i)
					if (const auto* root = lists[l][i].get())
						p.positions.emplace(root, std::pair{ l, i });
		for (const auto& [root, at] : p.positions)
			if (const auto it = p.lastPositions.find(root); it != p.lastPositions.end() && it->second != at)
				++p.rootsMoved;

		// The main pass's decal chains: a batch renderer's technique and sub-pass list, in registration order.
		struct Registered
		{
			OrderKey key;
			const RE::BSGeometry* geometry;
		};
		std::map<std::tuple<const RE::BSBatchRenderer*, std::uint32_t, std::uint32_t>, std::vector<Registered>> chains;
		for (const auto& entry : a_entries) {
			if (!a_main.contains(entry.batch) || (entry.hint != 2 && entry.hint != 3) || entry.fading || !entry.geometry)
				continue;
			++p.registrations;
			Registered registered{ {}, entry.geometry };
			const RE::NiAVObject* root = nullptr;
			const Miss miss = KeyOf(entry.geometry, p, registered.key, root);
			if (miss != Miss::None) {
				++p.misses[static_cast<std::size_t>(miss)];
				if (miss == Miss::NoRoot && p.unresolvedTops.size() < 24) {
					// The first object below the top of its chain, which is what a list would have held.
					const RE::NiAVObject* below = entry.geometry;
					while (below->parent && below->parent->parent)
						below = below->parent;
					++p.unresolvedTops[fmt::format("{} under {}", Named(below), Named(below->parent))];
				}
				continue;
			}
			p.decalRoots.push_back(root);
			p.ordered += registered.key.ordered ? 1 : 0;
			chains[{ entry.batch, entry.technique, entry.subPass }].push_back(std::move(registered));
		}
		// Whether the decal roots kept their relative order since last frame (the lists move whenever a root before them is
		// shown or hidden, BuildSceneLists dealing the roots out round robin).
		{
			std::vector<const RE::NiAVObject*> unique;
			ankerl::unordered_dense::set<const RE::NiAVObject*> seen;
			for (const auto* root : p.decalRoots)
				if (seen.insert(root).second)
					unique.push_back(root);
			std::vector<const RE::NiAVObject*> common, lastCommon;
			ankerl::unordered_dense::set<const RE::NiAVObject*> lastSet(p.lastDecalRoots.begin(), p.lastDecalRoots.end());
			for (const auto* root : unique)
				if (lastSet.contains(root))
					common.push_back(root);
			for (const auto* root : p.lastDecalRoots)
				if (seen.contains(root))
					lastCommon.push_back(root);
			++p.decalRootFrames;
			if (common != lastCommon)
				++p.decalRootsReordered;
			p.lastDecalRoots = std::move(unique);
			p.decalRoots.clear();
		}
		for (const auto& [chain, registered] : chains) {
			++p.chains;
			for (std::size_t i = 0; i < registered.size(); ++i)
				for (std::size_t j = i + 1; j < registered.size(); ++j) {
					++p.pairs;
					if (!Before(registered[j].key, registered[i].key))
						continue;
					++p.inversions;
					if (p.firstInversion.empty())
						p.firstInversion = fmt::format("'{}' (list {} at {}, depth {}) registered before '{}' (list {} at {}, depth {})",
							Named(registered[i].geometry), registered[i].key.list, registered[i].key.position, registered[i].key.path.size(),
							Named(registered[j].geometry), registered[j].key.list, registered[j].key.position, registered[j].key.path.size());
				}
		}
		if (p.frames % 300 == 0) {
			logger::info("[DCLF] decal order probe: {} frames, {} decal registrations, {} chains, {} pairs, {} out of the scene's order, {} under a "
			             "BSOrderedNode; unresolved: {} no list root, {} decal node, {} not a child; {} list roots moved; decal roots reordered on {} of {} "
			             "frames{}{}",
				p.frames, p.registrations, p.chains, p.pairs, p.inversions, p.ordered, p.misses[1], p.misses[2], p.misses[3], p.rootsMoved,
				p.decalRootsReordered, p.decalRootFrames, p.firstInversion.empty() ? "" : "; first: ", p.firstInversion);
			std::string tops;
			for (const auto& [name, n] : p.unresolvedTops)
				tops += fmt::format("{}{} x{}", tops.empty() ? "" : ", ", name, n);
			if (!tops.empty())
				logger::info("[DCLF] decal order probe, decals under no list root: {}", tops);
			p.unresolvedTops.clear();
			p.decalRootFrames = p.decalRootsReordered = 0;
			p.frames = p.registrations = p.chains = p.pairs = p.inversions = p.ordered = p.rootsMoved = 0;
			p.misses = {};
			p.firstInversion.clear();
		}
		std::swap(p.positions, p.lastPositions);
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

		/** @brief The BGSDecalNode above a_object (its record's kind: the RTTI exactly), the mirror's chain (T6b1b). */
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

	struct SceneStore::KeptDecalOrder
	{
		std::vector<OrderedDecal> entries;  // in draw order
	};

	void SceneStore::OrderDecals()
	{
		// The engine draws a decal chain (a group's technique bucket and sub-pass list) in reverse registration order, and
		// registers in the order of its scene lists (DecalOrder.cpp's key, which the order probe measures). Stable: the
		// scene's order, kept: a decal's key is taken when it joins (or is patched again), and a change re-keys only the
		// decals it names; the ordinals are counted again in one pass.
		if (!keptDecals)
			keptDecals = std::make_shared<KeptDecalOrder>();
		auto& kept = keptDecals->entries;
		const bool parity = SwitchEnabled(Switch::PersistentParity) && ParityDue(sceneFrame);
		if (!decalsRebuild && decalsChanged.empty() && !parity)
			return;
		if (tables.decalOrdinal.size() != tables.objects.size())
			tables.decalOrdinal.resize(tables.objects.size(), ~0u);

		// A decal's place in the order: its chain and key (none for an object with no geometry).
		auto keyed = [&](std::uint32_t a_object, std::uint64_t a_chain, OrderedDecal& a_out) {
			const auto* geometry = a_object < tables.objectGeometry.size() ? tables.objectGeometry[a_object] : nullptr;
			if (!geometry)
				return false;
			a_out = { a_chain, {}, a_object, DecalNodeOf(mirror, geometry) };
			SceneKeyOf(mirror, geometry, a_out.key);
			return true;
		};
		auto whole = [&] {
			std::vector<OrderedDecal> ordered;
			ordered.reserve(memberDecals.size());
			for (const auto& [object, chain] : memberDecals)
				if (OrderedDecal entry; keyed(object, chain, entry))
					ordered.push_back(std::move(entry));
			std::sort(ordered.begin(), ordered.end(), DrawnBefore);
			return ordered;
		};

		if (decalsRebuild) {
			kept = whole();
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

		// CS_DCLF_PERSISTENT_PARITY: the kept order against one made whole now (a key that went stale: a parent's children
		// re-indexed under a kept decal).
		if (parity) {
			auto& dp = decalOrderParity;
			++dp.checks;
			const auto reference = whole();
			dp.decals += reference.size();
			bool same = reference.size() == kept.size();
			for (std::size_t i = 0; same && i < kept.size(); ++i)
				same = reference[i].object == kept[i].object && reference[i].chain == kept[i].chain;
			// Why: a kept key no longer the decal's now (a decal node's places shift together, which keeps their order: counted, not a
			// difference), or an order the comparator does not hold (not a strict weak order).
			for (const auto& entry : kept) {
				OrderedDecal now;
				if (keyed(entry.object, entry.chain, now) && (Before(now.key, entry.key) || Before(entry.key, now.key)) && dp.staleKeys++ == 0) {
					const auto* geometry = tables.objectGeometry[entry.object];
					dp.staleFirst = fmt::format("object {} '{}' (decal node {}, now {}): kept ordered {} path [{}], now ordered {} path [{}]", entry.object,
						geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", fmt::ptr(entry.decalNode), fmt::ptr(now.decalNode), entry.key.ordered,
						PathText(entry.key.path), now.key.ordered, PathText(now.key.path));
				}
			}
			for (std::size_t i = 1; i < reference.size(); ++i)
				dp.unordered += DrawnBefore(reference[i], reference[i - 1]) ? 1u : 0u;
			if (!same && dp.differ++ == 0) {
				std::size_t at = 0;
				while (at < kept.size() && at < reference.size() && reference[at].object == kept[at].object)
					++at;
				dp.first = fmt::format("{} kept, {} whole; first difference at {}: kept object {}, whole object {}", kept.size(), reference.size(), at,
					at < kept.size() ? kept[at].object : ~0u, at < reference.size() ? reference[at].object : ~0u);
			}
		}
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
			const std::uint32_t ordinal = tables.decalCount[group]++;
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
