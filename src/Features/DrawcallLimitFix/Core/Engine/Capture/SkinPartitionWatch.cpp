#include "SkinPartitionWatch.h"

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace DCLF::SkinPartitionWatch
{
	namespace
	{
		// How long a copied partition is watched. skee stores the copy at once on the main thread, or in an SKSE task (the next main
		// loop tick) from any other thread, so a few frames would do; the margin covers a load's long frames.
		constexpr auto kWatchFor = std::chrono::seconds(5);
		// skee copies one partition per shape and step: a slider drag makes many. Past this the oldest are dropped.
		constexpr std::size_t kMaxWatched = 512;

		struct Watched
		{
			const void* partition;
			std::chrono::steady_clock::time_point until;
		};
		std::mutex mutex;
		std::vector<Watched> watched;
		std::atomic<std::uint32_t> watchedCount{ 0 };
		std::atomic<std::uint64_t> copies{ 0 };

		void Note(const void* a_partition)
		{
			const auto until = std::chrono::steady_clock::now() + kWatchFor;
			std::lock_guard lock(mutex);
			copies.fetch_add(1, std::memory_order_relaxed);
			for (auto& entry : watched)
				if (entry.partition == a_partition) {
					entry.until = until;
					return;
				}
			if (watched.size() >= kMaxWatched)
				watched.erase(watched.begin());
			watched.push_back({ a_partition, until });
			watchedCount.store(static_cast<std::uint32_t>(watched.size()), std::memory_order_relaxed);
		}

		struct CreateDeepCopy
		{
			// NiObject::CreateDeepCopy(NiPointer<NiObject>& out): the source is this. Only a skin partition's copy is noted; the copy is
			// the caller's (skee stores it later), so only the source, the partition the plan holds, is known here.
			static void thunk(RE::NiObject* a_this, RE::NiPointer<RE::NiObject>& a_out)
			{
				func(a_this, a_out);
				static const REL::Relocation<const RE::NiRTTI*> partitionRtti{ RE::NiSkinPartition::Ni_RTTI };
				if (a_this && a_this->GetRTTI() == partitionRtti.get())
					Note(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		// It chains (another plugin's detour of the function keeps running), so no entry check.
		stl::detour_thunk<CreateDeepCopy>(REL::RelocationID(68839, 70191));
		HookSites::NoteDetour("NiObject::CreateDeepCopy (RaceMenu's skin partition copies)", CreateDeepCopy::func.address());
		if (CreateDeepCopy::func.address())
			logger::info("[DCLF] skin partition copies watched (NiObject::CreateDeepCopy): RaceMenu's morphs of still actors are drawn too");
	}

	bool Any() { return watchedCount.load(std::memory_order_relaxed) != 0; }

	void Collect(std::vector<const void*>& a_out)
	{
		a_out.clear();
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lock(mutex);
		std::erase_if(watched, [&](const Watched& a_entry) { return a_entry.until < now; });
		watchedCount.store(static_cast<std::uint32_t>(watched.size()), std::memory_order_relaxed);
		for (const auto& entry : watched)
			a_out.push_back(entry.partition);
	}

	Counts GetCounts()
	{
		return { copies.load(std::memory_order_relaxed), watchedCount.load(std::memory_order_relaxed) };
	}
}
