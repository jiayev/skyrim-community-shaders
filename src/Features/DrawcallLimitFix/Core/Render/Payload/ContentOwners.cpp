// The scene's content owners (see the header).
#include "Features/DrawcallLimitFix/Core/Render/Payload/ContentOwners.h"

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include <algorithm>

namespace DCLF::Draws
{
	void ContentOwners::Set(const void* a_target, std::uint64_t a_offset, std::uint64_t a_bytes, std::span<const std::shared_ptr<const void>> a_owners)
	{
		if (!a_bytes || (ranges.empty() && a_owners.empty()))
			return;
		const std::uint64_t end = a_offset + a_bytes;
		// The same range naming the same owners: unchanged (a row written every frame with the same bindings).
		if (const auto same = ranges.find({ a_target, a_offset });
			same != ranges.end() && same->second.bytes == a_bytes && std::ranges::equal(same->second.owners, a_owners))
			return;
		// Every range a_target holds that overlaps: the one starting before a_offset, then those starting inside.
		auto it = ranges.lower_bound({ a_target, a_offset });
		if (it != ranges.begin()) {
			const auto before = std::prev(it);
			if (before->first.first == a_target && before->first.second + before->second.bytes > a_offset)
				it = before;
		}
		while (it != ranges.end() && it->first.first == a_target && it->first.second < end) {
			for (auto& owner : it->second.owners)
				replaced.push_back(std::move(owner));
			it = ranges.erase(it);
			changed = true;
		}
		if (!a_owners.empty()) {
			ranges.insert_or_assign({ a_target, a_offset }, Range{ a_bytes, { a_owners.begin(), a_owners.end() } });
			changed = true;
		}
	}

	std::shared_ptr<const void> ContentOwners::Snapshot()
	{
		if (!changed)
			return sealed;
		std::vector<std::shared_ptr<const void>> owners = std::exchange(replaced, {});
		for (const auto& [key, range] : ranges)
			owners.insert(owners.end(), range.owners.begin(), range.owners.end());
		sealed = GpuTextures::Get().Seal(std::move(owners));
		changed = false;
		++snapshotsMade;
		return sealed;
	}

	ContentOwners::Counts ContentOwners::TakeCounts()
	{
		Counts counts{ .ranges = ranges.size(), .snapshotsMade = std::exchange(snapshotsMade, 0) };
		for (const auto& [key, range] : ranges)
			counts.owners += range.owners.size();
		return counts;
	}
}
