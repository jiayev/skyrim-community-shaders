#pragma once

// The owners of what GPU-visible bytes currently hold (dclf-architecture.md, "Descriptor ownership"): the descriptors an uploaded
// OwnedRow names, by the range its write replaced. Bytes written once and read by any later execution (a latched draw row both main
// epochs draw with, a table row read until it is rewritten) keep their descriptors through every execution submitted while they are
// current: each execution holds Snapshot (ExecutionOwners::Seal).

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief The scene's content owners (SceneBuffers::content): render thread. Ranges are keyed by their target as the upload classes
	 * match it (a plain buffer's address, a versioned buffer's Key(): every version of it, so a catch-up's or a growth's copy keeps them).
	 */
	class ContentOwners
	{
	public:
		/**
		 * @brief a_target's [a_offset, a_offset + a_bytes) now holds bytes naming a_owners' descriptors (none: plain bytes); every range it
		 * overlaps is replaced. A replaced range's owners go with the next snapshot, so the last of them is released by an execution.
		 */
		void Set(const void* a_target, std::uint64_t a_offset, std::uint64_t a_bytes, std::span<const std::shared_ptr<const void>> a_owners);

		/**
		 * @brief One owner of every current range's owners (and those replaced since the last snapshot), sealed on the cleanup queue; made
		 * again only after a change. Null when there are none.
		 */
		std::shared_ptr<const void> Snapshot();

		struct Counts
		{
			std::size_t ranges = 0, owners = 0;
			std::uint64_t snapshotsMade = 0;
		};
		Counts TakeCounts();

	private:
		struct Range
		{
			std::uint64_t bytes = 0;
			std::vector<std::shared_ptr<const void>> owners;
		};
		std::map<std::pair<const void*, std::uint64_t>, Range> ranges;
		std::vector<std::shared_ptr<const void>> replaced;  // owners of ranges replaced since the last snapshot
		std::shared_ptr<const void> sealed;
		bool changed = false;
		std::uint64_t snapshotsMade = 0;
	};
}
