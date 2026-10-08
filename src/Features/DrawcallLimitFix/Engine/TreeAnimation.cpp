#include "TreeAnimation.h"

#include "EngineAccess.h"
#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"

#include <ankerl/unordered_dense.h>

#include <atomic>
#include <cstring>

// The tree manager's animation list without DCLF's trees (drawcall-limit-fix.md, "Owned trees off the tree manager's
// animation list"). TreeAnimation.h describes it.

namespace DCLF::TreeAnimation
{
	namespace
	{
		using namespace Engine;

		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kManager = 0x20F6A18;  // BSTreeManager*
		constexpr std::uintptr_t kAdd = 0x4376f0;       // FUN_1404376f0(manager, node): a BSLeafAnimNode onto the animation list
		constexpr std::uintptr_t kRemove = 0x437840;    // FUN_140437840(manager, node): off the animation list (a BSTreeNode: off the tree list)
		constexpr std::uintptr_t kClear = 0x437ae0;     // FUN_140437ae0(manager): both lists emptied (TES, the manager's teardown)
		constexpr std::size_t kAnimationList = 0x50;    // BSTArray<NiAVObject*> (a reference each, taken by hand)
		constexpr std::size_t kLock = 0x18;             // BSSpinLock (owner, count), the animation list's

		using List = RE::BSTArray<RE::NiAVObject*>;

		std::byte* Manager() { return Global<std::byte*>(kManager); }
		List& AnimationList(std::byte* a_manager) { return *reinterpret_cast<List*>(a_manager + kAnimationList); }
		RE::BSSpinLock& Lock(std::byte* a_manager) { return *reinterpret_cast<RE::BSSpinLock*>(a_manager + kLock); }

		bool installed = false;
		bool enabled = false;
		// The roots the list should not hold (render thread).
		ankerl::unordered_dense::set<const RE::NiAVObject*> owned;
		// The nodes DCLF took off the list, each holding the list's reference: under the manager's lock, from any thread.
		// Never destroyed (nothing may release engine objects while the process is torn down).
		auto& detached = *new ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiAVObject*>();
		// Nodes the engine added (FUN_1404376f0), for the render thread to take off again if owned. Identity only.
		auto& added = *new EventQueue<const RE::NiAVObject*>();
		// detached.size(), for the report, which takes no lock.
		std::atomic<std::uint32_t> held{ 0 };

		struct Stats
		{
			std::atomic<std::uint64_t> engineAdds{ 0 }, engineRemoves{ 0 }, clears{ 0 };
			std::uint64_t syncs = 0, takenOff = 0, putBack = 0, takenOffAgain = 0, parityChecks = 0, parityOnList = 0;
		};
		Stats stats;

		// Under the lock: every node in a_take taken off the list (one pass), each keeping the list's reference. Returns how many.
		std::uint32_t TakeOff(List& a_list, const ankerl::unordered_dense::set<const RE::NiAVObject*>& a_take)
		{
			std::uint32_t kept = 0, taken = 0;
			const std::uint32_t size = a_list.size();
			for (std::uint32_t i = 0; i < size; ++i) {
				auto* node = a_list[i];
				if (node && a_take.contains(node) && detached.try_emplace(node, node).second) {
					++taken;
					continue;
				}
				a_list[kept++] = node;
			}
			if (kept != size)
				a_list.resize(kept);
			held.store(static_cast<std::uint32_t>(detached.size()), std::memory_order_relaxed);
			return taken;
		}

		struct Hooks
		{
			struct Add
			{
				static void thunk(std::byte* a_manager, RE::NiAVObject* a_node, std::uint64_t a_3, std::uint64_t a_4)
				{
					func(a_manager, a_node, a_3, a_4);
					if (a_node && enabled) {
						added.Push(a_node);
						stats.engineAdds.fetch_add(1, std::memory_order_relaxed);
					}
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct Remove
			{
				static std::uint64_t thunk(std::byte* a_manager, RE::NiAVObject* a_node)
				{
					// A node DCLF holds off the list is the engine's to drop: forgotten, with the list's reference.
					RE::NiAVObject* release = nullptr;
					if (a_node && a_manager) {
						auto& lock = Lock(a_manager);
						lock.Lock();
						if (const auto it = detached.find(a_node); it != detached.end()) {
							release = it->second;
							detached.erase(it);
							held.store(static_cast<std::uint32_t>(detached.size()), std::memory_order_relaxed);
						}
						lock.Unlock();
					}
					if (release) {
						stats.engineRemoves.fetch_add(1, std::memory_order_relaxed);
						release->DecRefCount();
						return 0;
					}
					return func(a_manager, a_node);
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};

			struct Clear
			{
				static std::uint64_t thunk(std::byte* a_manager, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
				{
					const auto result = func(a_manager, a_2, a_3, a_4);
					std::vector<RE::NiAVObject*> release;
					if (a_manager) {
						auto& lock = Lock(a_manager);
						lock.Lock();
						for (const auto& [node, reference] : detached)
							release.push_back(reference);
						detached.clear();
						held.store(0, std::memory_order_relaxed);
						lock.Unlock();
					}
					for (auto* node : release)
						node->DecRefCount();
					stats.clears.fetch_add(1, std::memory_order_relaxed);
					return result;
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};
		};

		// The engine's adds since the last call, taken off again where owned.
		void DrainAdds(std::byte* a_manager)
		{
			ankerl::unordered_dense::set<const RE::NiAVObject*> take;
			added.Drain([&](const RE::NiAVObject* a_node) {
				if (owned.contains(a_node))
					take.insert(a_node);
			});
			if (take.empty() || !a_manager)
				return;
			auto& lock = Lock(a_manager);
			lock.Lock();
			stats.takenOffAgain += TakeOff(AnimationList(a_manager), take);
			lock.Unlock();
		}
	}

	void Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		// The animation list's lock and array, as the add takes them: `lea rcx, [rdi + 0x18]` before the lock, the array at +0x50.
		stl::detour_thunk<Hooks::Add>(base + kAdd);
		stl::detour_thunk<Hooks::Remove>(base + kRemove);
		stl::detour_thunk<Hooks::Clear>(base + kClear);
		installed = enabled = true;
		logger::info("[DCLF] tree animation list: owned trees are taken off the tree manager's animation list");
	}

	bool Installed() { return installed; }

	void SetOwned(const std::vector<const RE::NiAVObject*>& a_roots)
	{
		if (!installed)
			return;
		auto* manager = Manager();
		DrainAdds(manager);
		ankerl::unordered_dense::set<const RE::NiAVObject*> next(a_roots.begin(), a_roots.end());
		++stats.syncs;
		if (!manager) {
			owned = std::move(next);
			return;
		}
		std::vector<RE::NiAVObject*> putBack;
		auto& lock = Lock(manager);
		lock.Lock();
		auto& list = AnimationList(manager);
		// Back on the list, with the reference DCLF held: the roots no longer owned.
		for (auto it = detached.begin(); it != detached.end();) {
			if (next.contains(it->first)) {
				++it;
				continue;
			}
			list.push_back(it->second);
			putBack.push_back(it->second);
			it = detached.erase(it);
		}
		held.store(static_cast<std::uint32_t>(detached.size()), std::memory_order_relaxed);
		stats.takenOff += TakeOff(list, next);
		lock.Unlock();
		stats.putBack += putBack.size();
		owned = std::move(next);
	}

	void CheckParity()
	{
		auto* manager = Manager();
		if (!installed || !manager)
			return;
		DrainAdds(manager);
		auto& lock = Lock(manager);
		lock.Lock();
		const auto& list = AnimationList(manager);
		for (std::uint32_t i = 0; i < list.size(); ++i)
			stats.parityOnList += list[i] && owned.contains(list[i]) ? 1 : 0;
		lock.Unlock();
		++stats.parityChecks;
	}

	std::string Report()
	{
		auto* manager = Manager();
		// Read without the lock: a count and a time, for the report.
		std::uint32_t listed = 0;
		float updateMs = 0.0f;
		if (manager) {
			updateMs = At<float>(manager, 0x7C);  // the manager's own measure of its last update (FUN_140437d40)
			listed = std::atomic_ref<std::uint32_t>(At<std::uint32_t>(manager, kAnimationList + 0x10)).load(std::memory_order_relaxed);
		}
		if (!installed)
			return fmt::format("[DCLF] tree animation list: off (CS_DCLF_TREE_LIST=0): {} on the list (the manager's last update {:.3f} ms)", listed, updateMs);
		auto& s = stats;
		const auto text = fmt::format("[DCLF] tree animation list: {} on the list, {} owned trees off it (the manager's last update {:.3f} ms); {} syncs: {} taken off, {} put back; engine: {} adds ({} taken off again), {} removes of trees off the list, {} clears; parity {} checks, {} owned trees on the list{}",
			listed, held.load(std::memory_order_relaxed), updateMs, s.syncs, s.takenOff, s.putBack, s.engineAdds.exchange(0), s.takenOffAgain, s.engineRemoves.exchange(0), s.clears.exchange(0), s.parityChecks,
			s.parityOnList, s.parityChecks ? (s.parityOnList ? " <- TREE LIST" : " <- OK") : "");
		s.syncs = s.takenOff = s.putBack = s.takenOffAgain = s.parityChecks = s.parityOnList = 0;
		return text;
	}
}
