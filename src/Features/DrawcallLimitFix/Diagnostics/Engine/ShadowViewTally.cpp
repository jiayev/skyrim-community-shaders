// The shadow views' tallies (ShadowViewTally.h): render thread.

#include "ShadowViewTally.h"

#include <algorithm>
#include <array>

namespace DCLF::Diagnostics
{
	namespace
	{
		std::array<ViewTally, 3> tallies;
	}

	void ViewTally::Note(const char* a_what, float a_size, std::string a_detail)
	{
		largest = std::max(largest, a_size);
		// The first of each kind, up to four kinds.
		if (first.find(a_what) == std::string::npos && std::count(first.begin(), first.end(), '|') < 3)
			first += fmt::format("{}{}: {}", first.empty() ? "" : " | ", a_what, a_detail);
	}

	ViewTally& ShadowViewTally(ShadowView a_view)
	{
		return tallies[static_cast<std::size_t>(a_view)];
	}
}
