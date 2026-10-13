#include "SceneObservers.h"

#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/DecalOrder.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

// CS_DCLF_DECAL_ORDER_PROBE: the main pass's decal registrations against the scene lists' order (the engine registers in the order of its
// scene lists; DecalOrder.cpp's key is that order), reported every 300 observed frames.

namespace DCLF::Diagnostics
{
	namespace
	{
		using Engine::Global;
		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

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

		std::string Named(const RE::NiAVObject* a_object)
		{
			const char* name = a_object ? a_object->name.c_str() : nullptr;
			return name && *name ? name : "-";
		}
	}

	void ProbeDecalOrder(std::span<const PassCapture::Entry> a_entries, const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>& a_main)
	{
		auto& p = decalOrderProbe;
		++p.frames;
		// This frame's scene lists: every root's list and position, and whether a root kept both since last frame.
		p.positions.clear();
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		if (auto* lists = Global<SceneList*>(Engine::Address::kSceneLists))
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
			if (!a_main.contains(entry.batch) || (entry.hint != 2 && entry.hint != 3) || !entry.geometry)
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
		if (p.frames % Reports::kInterval == 0) {
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
}
