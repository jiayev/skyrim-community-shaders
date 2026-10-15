// The first-person view's scene diagnostics, on Core's root view hook points (Diagnostics/Scene/RootViewEvents.h): the subtree's bits by
// events against the live tree (CS_DCLF_PERSISTENT_PARITY), its live chain and its hidden geometry (render thread, FrameGlobals' engine
// moments); the hidden verdicts of its entries (the classification); and under CS_DCLF_SET_PARITY the entries filed under another category
// node and the slots the set's decisions leave out of the view, the commit's and its parity's, reported after the set's line.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		using V = Diagnostics::SceneView;

		/** @brief Every node and geometry under a_root (not a_root) as it is now (the tree alive: the render thread, inside the frame). */
		void LiveSubtree(const RE::NiAVObject* a_root, std::vector<RootSubtreeNode>& a_out)
		{
			a_out.clear();
			if (!const_cast<RE::NiAVObject*>(a_root)->AsNode())
				return;
			Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t a_depth) {
				if (a_depth)
					a_out.push_back({ &a_object, a_object.AsGeometry() != nullptr, Engine::IsHidden(&a_object) });
			});
			std::ranges::sort(a_out, {}, &RootSubtreeNode::key);
		}

		/** @brief The node's bit at the last culls, else now. */
		bool CulledBit(const Diagnostics::RootSubtreeSynced& a_subtree, const RootSubtreeNode& a_node) { return a_subtree.culled && a_node.saved ? a_node.culled : a_node.hidden; }

		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY: the subtree's bits by events against a walk of the live tree now (render thread, at an engine
		 * moment, right after the subtree's sync): membership and each node's bit. A periodic line, and the first differences named.
		 */
		struct SubtreeParity
		{
			std::uint64_t checks = 0, compared = 0, membership = 0, bits = 0, walks = 0;
			std::string first;
			std::vector<RootSubtreeNode> live;
		} subtreeParity;
		bool subtreeParityOn = false;

		void CheckSubtree(const Diagnostics::RootSubtreeSynced& a_subtree)
		{
			auto& p = subtreeParity;
			++p.checks;
			LiveSubtree(a_subtree.root, p.live);
			std::size_t i = 0, j = 0;
			auto note = [&](const RE::NiAVObject* a_key, const char* a_what) {
				if (std::ranges::count(p.first, ';') < 6)
					p.first += fmt::format("{}{} '{}' {} ({})", p.first.empty() ? "" : "; ", a_what, a_key->name.c_str() ? a_key->name.c_str() : "", static_cast<const void*>(a_key), a_subtree.moment);
			};
			std::vector<RootSubtreeNode> kept;
			for (const auto& node : a_subtree.nodes)
				if (node.key)
					kept.push_back(node);
			while (i < p.live.size() || j < kept.size()) {
				if (j == kept.size() || (i < p.live.size() && p.live[i].key < kept[j].key)) {
					++p.membership;
					note(p.live[i].key, "not kept");
					++i;
				} else if (i == p.live.size() || kept[j].key < p.live[i].key) {
					++p.membership;
					++j;  // gone from the tree: not dereferenced
				} else {
					++p.compared;
					if (p.live[i].hidden != kept[j].hidden) {
						++p.bits;
						note(p.live[i].key, p.live[i].hidden ? "hidden, kept shown" : "shown, kept hidden");
					}
					++i;
					++j;
				}
			}
			if (p.checks % 600)
				return;
			logger::info("[DCLF] first-person bits by events (C3 parity): {} checks, {} nodes compared, {} membership and {} bit differences against the live walk; {} walks{}{}",
				p.checks, p.compared, p.membership, p.bits, a_subtree.walks - std::exchange(p.walks, a_subtree.walks), p.membership || p.bits ? " <- FLAG" : " <- OK",
				p.first.empty() ? "" : "; first: " + p.first);
			p.compared = p.membership = p.bits = 0;
			p.first.clear();
		}

		/** @brief The live chain of the first shown geometry, once (render thread, the frame's start: the tree alive): where the skeleton hangs. */
		void LogSubtreeChain(const Diagnostics::RootSubtreeSynced& a_subtree)
		{
			static bool chainLogged = false;
			if (chainLogged)
				return;
			for (const auto& node : a_subtree.nodes)
				if (node.key && node.geometry && !node.hidden) {
					chainLogged = true;
					std::string chain;
					std::uint32_t at = 0;
					for (const RE::NiAVObject* object = node.key; object && at < 24; object = object->parent, ++at)
						chain += fmt::format("{}'{}' {}", at ? " <- " : "", object->name.c_str() ? object->name.c_str() : "", static_cast<const void*>(object));
					logger::info("[DCLF] first-person subtree (T7e): the root {} (parent {}); a geometry's live chain: {}", static_cast<const void*>(a_subtree.root),
						static_cast<const void*>(a_subtree.root->parent), chain);
					break;
				}
		}

		/**
		 * @brief The first-person diagnostic (render thread, the frame's start): the geometry of the first-person subtree hidden now and at the
		 * last culls, logged when the two lists change (at most 24 times). Evaluated only when the bits or the membership changed.
		 */
		void NoteFirstPersonDifference(const Diagnostics::RootSubtreeSynced& a_subtree)
		{
			static std::string last;
			static std::uint32_t logged = 0;
			static std::uint64_t seen = ~0ull;
			if (logged >= 24 || std::exchange(seen, a_subtree.changes) == a_subtree.changes)
				return;
			auto names = [&](bool a_atCull) {
				std::string text;
				std::uint32_t geometry = 0, hidden = 0;
				for (const auto& node : a_subtree.nodes) {
					if (!node.key || !node.geometry)
						continue;
					++geometry;
					if (!(a_atCull ? CulledBit(a_subtree, node) : node.hidden))
						continue;
					++hidden;
					if (hidden <= 12)
						text += fmt::format("{}'{}'", text.empty() ? "" : ", ", node.key->name.c_str() ? node.key->name.c_str() : "");
				}
				return fmt::format("{} geometries, {} hidden{}{}", geometry, hidden, text.empty() ? "" : ": ", text);
			};
			std::string now = "at the frame's start " + names(false) + (a_subtree.culled ? "; at the last first-person culls " + names(true) : std::string("; no first-person cull seen"));
			if (now == last)
				return;
			last = now;
			++logged;
			logger::info("[DCLF] first-person subtree (T7e): {}", now);
		}

		void OnSubtreeSynced(const Diagnostics::RootSubtreeSynced& a_subtree)
		{
			// At both moments the parity; at the frame's start the chain (once) and the hidden geometry, in that order.
			if (subtreeParityOn)
				CheckSubtree(a_subtree);
			if (!a_subtree.frameStart)
				return;
			LogSubtreeChain(a_subtree);
			NoteFirstPersonDifference(a_subtree);
		}

		void OnHidden(const Diagnostics::RootViewHidden& a_hidden)
		{
			// The node that hid a first-person skeleton entry (ClassifyFrame), the first 16.
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged.fetch_add(1, std::memory_order_relaxed) >= 16)
				return;
			const auto& cullHidden = a_hidden.cullHidden;
			const auto* object = static_cast<const RE::NiAVObject*>(a_hidden.key);
			const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_object) { return a_entry.first < a_object; });
			const bool listed = it != cullHidden.end() && it->first == object;
			const auto& mirror = V::mirror(a_hidden.store);
			std::string chain;
			const void* at = a_hidden.geometry;
			for (std::uint32_t n = 0; at && n < 16; ++n) {
				const auto* node = mirror.Node(at);
				chain += fmt::format("{}{}", n ? " <- " : "", at);
				at = node ? node->parent : nullptr;
				if (!node)
					chain += " (no record)";
			}
			logger::info("[DCLF] first-person hidden verdict (T7e): geometry {} filed under {} (the first-person root {}{}); hidden at chain depth {}: node {}{}, {} (the mirror's bit {}); "
						 "{} nodes listed in the frame's capture; the mirror's chain: {}",
				a_hidden.geometry, a_hidden.category, a_hidden.root, a_hidden.sameRoot ? ", the same" : ", another", a_hidden.depth, a_hidden.key,
				a_hidden.key == a_hidden.category ? " (its category node)" : "",
				listed ? fmt::format("listed hidden {} in the frame's capture", it->second) : std::string("not in the frame's capture"), a_hidden.recordHidden,
				cullHidden.size(), chain);
		}

		/**
		 * @brief CS_DCLF_SET_PARITY: entries whose mirror chain reaches the first-person root while they are filed under another category
		 * node (the first-person view never takes them), counted from the evaluations' threads; and the first-person slots the set's
		 * decisions leave out (the coordinator's: the commit's and its parity's), with the first of each.
		 */
		struct FilingState
		{
			std::atomic<std::uint64_t> misfiled{ 0 };
			std::atomic<const void*> misfiledGeometry{ nullptr }, misfiledCategory{ nullptr };
			// First-person entries' slots the first-person view takes no part in (SetParticipation), the first.
			std::uint64_t excluded = 0;
			std::string firstExcluded;
			// First-person slots that take part but are held out (not resident, not main-ready, pipelines), the first.
			std::uint64_t held = 0;
			std::string firstHeld;
		} filing;

		void OnMisfiled(const Diagnostics::ViewMisfiled& a_event)
		{
			filing.misfiled.fetch_add(1, std::memory_order_relaxed);
			filing.misfiledGeometry.store(a_event.geometry, std::memory_order_relaxed);
			filing.misfiledCategory.store(a_event.category, std::memory_order_relaxed);
		}

		/** @brief The first-person slot a decision left out (a_waitWhy: the waiting reason it is shown with). */
		void NoteLeftOut(const Scene::Coordinator& a_store, std::uint32_t a_slot, const V::SlotDecision& a_decision, std::uint8_t a_waitWhy)
		{
			auto& fp = filing;
			const auto& tables = V::tables(a_store);
			if (a_decision.rootViewExcluded) {
				++fp.excluded;
				if (fp.firstExcluded.empty())
					if (const auto* geometry = tables.objectGeometry[a_slot]) {
						const auto p = tables.objects[a_slot].pipelineIndex;
						fp.firstExcluded = fmt::format("'{}' (slot {}, flags {:#x}, raster {:#x})", geometry->name.c_str() ? geometry->name.c_str() : "?",
							a_slot, tables.objects[a_slot].flags, p < tables.pipelines.size() ? tables.pipelines[p].rasterFlags : 0u);
					}
			}
			if (a_decision.rootViewHeld) {
				++fp.held;
				if (fp.firstHeld.empty())
					if (const auto* geometry = tables.objectGeometry[a_slot])
						fp.firstHeld = fmt::format("'{}' (slot {}, {}, waiting reason {}, first-person readiness cause {}, pipeline slot {} raster {:#x})",
							geometry->name.c_str() ? geometry->name.c_str() : "?", a_slot, V::IsResidentSlot(a_store, a_slot) ? "resident" : "not resident",
							a_waitWhy, a_decision.rootViewSlotWhy, tables.objects[a_slot].pipelineIndex,
							tables.objects[a_slot].pipelineIndex < tables.pipelines.size() ? tables.pipelines[tables.objects[a_slot].pipelineIndex].rasterFlags : 0u);
			}
		}

		void OnSlotDecided(const Diagnostics::SetSlotDecided& a_event) { NoteLeftOut(a_event.store, a_event.slot, a_event.decision, a_event.waitWhy); }
		void OnSlotRedecided(const Diagnostics::SetSlotRedecided& a_event) { NoteLeftOut(a_event.store, a_event.slot, a_event.decision, a_event.waitWhy); }
	}

	void SubscribeRootViewChecks()
	{
		const auto& o = Diagnostics::Observe();
		subtreeParityOn = o.scene;
		Diagnostics::Subscribe<Diagnostics::RootSubtreeSynced>(&OnSubtreeSynced);
		Diagnostics::Subscribe<Diagnostics::RootViewHidden>(&OnHidden);
		if (o.claims) {
			Diagnostics::Subscribe<Diagnostics::ViewMisfiled>(&OnMisfiled);
			Diagnostics::Subscribe<Diagnostics::SetSlotDecided>(&OnSlotDecided);
			Diagnostics::Subscribe<Diagnostics::SetSlotRedecided>(&OnSlotRedecided);
		}
	}

	void SetLines(std::uint32_t)
	{
		// The scene lane, the coordinator's report, right after the set's line: the filing, then the slots held out, then those excluded.
		if (const auto misfiled = filing.misfiled.exchange(0, std::memory_order_relaxed))
			logger::info("[DCLF] first-person entries filed under another category node (T7e, CS_DCLF_SET_PARITY): {} evaluations; last geometry {} under {} "
						 "(the first-person root {}) <- FIRST-PERSON FILING",
				misfiled, filing.misfiledGeometry.load(std::memory_order_relaxed), filing.misfiledCategory.load(std::memory_order_relaxed),
				static_cast<const void*>(V::firstViewRoot(Scene::Coordinator::Get())));
		const auto held = std::exchange(filing.held, 0);
		const auto firstHeld = std::exchange(filing.firstHeld, {});
		const auto excluded = std::exchange(filing.excluded, 0);
		const auto firstExcluded = std::exchange(filing.firstExcluded, {});
		if (held)
			logger::info("[DCLF] first-person slots held out of the first-person view (T7e, CS_DCLF_SET_PARITY): {} slot evaluations; first: {}", held, firstHeld);
		if (excluded)
			logger::info("[DCLF] first-person entries the first-person view takes no part in (T7e, CS_DCLF_SET_PARITY): {} slot evaluations; first: {}", excluded, firstExcluded);
	}
}
