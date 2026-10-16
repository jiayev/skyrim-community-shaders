// The entry exclusions (see the header): their translations to later candidates, and their slots.

#include "Features/DrawcallLimitFix/Core/Render/Shadow/SunExclusion.h"

#include <algorithm>

namespace DCLF
{
	SunExclusionSlot& SunExclusionSlot::Get()
	{
		static SunExclusionSlot instance;
		return instance;
	}

	ParabolicExclusionSlot& ParabolicExclusionSlot::Get()
	{
		static ParabolicExclusionSlot instance;
		return instance;
	}

	void SunExclusionSlot::BeginFrame(const std::shared_ptr<const EntrySpace>& a_candidates)
	{
		// The builder translated each verdict it built to the candidates of every later build (ForwardExclusion): the frame takes the
		// one for its candidates, a swap. None (a snapshot the builder made no exclusion for): the pending one stays, a verdict on other
		// candidates, which the full-frustum cull does not apply (exclusionStale).
		if (!pending || !a_candidates || pending->candidates == a_candidates)
			return;
		if (auto forwarded = ForwardExclusion(pending, a_candidates)) {
			pending = std::move(forwarded);
			++counts.forwarded;
		} else {
			++counts.unforwarded;
		}
	}

	std::shared_ptr<SunExclusion> TranslateExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const EntrySpace>& a_to,
		bool a_newVersion, bool a_lightReach, std::vector<std::uint32_t>* a_changed, const SunExclusion* a_stamps)
	{
		if (!a_exclusion || !a_to || a_exclusion->candidates == a_to)
			return a_exclusion;
		std::vector<std::uint32_t> local;
		auto& changed = a_changed ? *a_changed : local;
		changed.clear();
		ChangedEntriesByLog(a_exclusion->candidates.get(), *a_to, changed);
		auto out = std::make_shared<SunExclusion>();
		out->candidates = a_to;
		const std::uint32_t capacity = a_to->Capacity();
		const std::uint32_t geometries = a_to->GeometryCapacity();
		out->excluded.assign(capacity, 0);
		const std::size_t kept = std::min<std::size_t>(capacity, a_exclusion->excluded.size());
		std::copy_n(a_exclusion->excluded.begin(), kept, out->excluded.begin());
		// Indices past the original's are new entries (changed), not excluded; ones the original had past a_to's capacity are gone.
		std::uint32_t count = a_exclusion->excludedCount;
		if (kept < a_exclusion->excluded.size())
			count -= static_cast<std::uint32_t>(std::count(a_exclusion->excluded.begin() + kept, a_exclusion->excluded.end(), std::uint8_t(1)));
		bool flipped = false;
		for (const std::uint32_t e : changed)
			if (e < capacity && out->excluded[e]) {
				out->excluded[e] = 0;
				--count;
				flipped = true;
			}
		out->excludedCount = count;
		// Versions of translated copies are apart from the builds' (ShadowBuild.cpp counts from 1).
		static std::atomic<std::uint64_t> translatedVersions{ 0 };
		out->version = a_newVersion && flipped ? (1ull << 63) | (translatedVersions.fetch_add(1, std::memory_order_relaxed) + 1) : a_exclusion->version;
		out->builtFrame = a_exclusion->builtFrame;
		out->reused = a_exclusion->reused;
		// The stamped words, shared where long enough (SunExclusion: no version but the frame's is stamped).
		const auto fits = [&](const SunExclusion* a_from) {
			return a_from && a_from->removed && a_from->cleared && (!a_lightReach || a_from->lightReach) && a_from->removedCount >= capacity &&
			       a_from->clearedCount >= geometries;
		};
		if (const SunExclusion* from = fits(a_stamps) ? a_stamps : fits(a_exclusion.get()) ? a_exclusion.get() : nullptr) {
			out->removed = from->removed;
			out->cleared = from->cleared;
			out->lightReach = a_lightReach ? from->lightReach : nullptr;
			out->removedCount = from->removedCount;
			out->clearedCount = from->clearedCount;
		} else {
			out->removed = std::make_shared<std::atomic<std::uint32_t>[]>(capacity);
			out->cleared = std::make_shared<std::atomic<std::uint32_t>[]>(geometries);
			if (a_lightReach)
				out->lightReach = std::make_shared<std::atomic<std::uint64_t>[]>(std::size_t(capacity) * 3);
			out->removedCount = capacity;
			out->clearedCount = geometries;
		}
		return out;
	}

	std::shared_ptr<SunExclusion> ForwardExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const EntrySpace>& a_to,
		std::vector<std::uint32_t>* a_changed)
	{
		if (!a_exclusion || !a_to)
			return nullptr;
		// Each link is held by the one before it (forwardOwned, written before forward is released and never again).
		const SunExclusion* node = a_exclusion.get();
		if (node->candidates == a_to)
			return a_exclusion;
		while (const SunExclusion* next = node->forward.load(std::memory_order_acquire)) {
			if (a_changed)
				a_changed->insert(a_changed->end(), next->forwardChanged.begin(), next->forwardChanged.end());
			if (next->candidates == a_to)
				return node->forwardOwned;
			node = next;
		}
		return nullptr;
	}
}
