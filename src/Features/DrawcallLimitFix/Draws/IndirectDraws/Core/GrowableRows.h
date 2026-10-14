#pragma once

// IndirectDraws' row tables (GrowableRows). Included by Internal.h.

namespace DCLF::Draws
{
	/**
	 * @brief A table of fixed-stride rows in one device buffer, read by address, that grows instead of capping: a buffer of the
	 * scene's set (SceneBufferSet), one row per unit. Rows are constant-buffer blocks (stride a multiple of kConstantAlignment); the
	 * caller owns the values. A new version holds nothing, or the rows its capture laid out (its contents stamp), so its writer sends
	 * every row again or those changed since the stamp (FilledContents). Its address and capacity are the named set's: a row past the
	 * capacity waits for the requirement that grows it; nothing is dropped, nothing falls back.
	 */
	struct GrowableRows
	{
		Versioned buffer;
		std::uint32_t stride = 0;
		std::string name;

		/** @brief The first version: a_rows rows of a_stride bytes, grown by a_policy, a new version holding what a_contents says. */
		bool Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name, org::services::GrowthPolicy a_policy = org::services::GrowthPolicy::Doubling(),
			org::services::GrowableContents a_contents = org::services::GrowableContents::Fresh, org::services::GrowableCapture a_capture = {});
		/** @brief The rows the version a_named names holds (its capacity: a row past it waits for the table to grow). */
		std::uint32_t RowsIn(const NamedVersions& a_named) const { return static_cast<std::uint32_t>(a_named.Of(buffer).units); }
	};
}
