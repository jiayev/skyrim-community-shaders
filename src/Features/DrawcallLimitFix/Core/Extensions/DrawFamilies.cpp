// The draw families' entries and their frame calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveDrawFamilies (InstallFeatures, before DCLF's threads exist), read-only after.
		DrawFamilySet& Entries()
		{
			static DrawFamilySet entries;
			return entries;
		}
	}

	void ResolveDrawFamilies(const std::array<Api::DrawFamilyContribution, kDrawFamilies>& a_registered)
	{
		auto& entries = Entries();
		for (std::size_t f = 0; f < kDrawFamilies; ++f) {
			const auto& registered = a_registered[f];
			auto& entry = entries[f];
			if (registered.family == Api::DrawFamily::Count)
				continue;
			entry.taken = true;
			entry.contribution = registered;
			entry.colourRange = registered.colourRange;
			entry.contribution.colourRange = entry.colourRange;
		}
	}

	const DrawFamilySet& DrawFamilies() { return Entries(); }

	bool DrawFamilyOn(const ToggleSet& a_toggles, Api::DrawFamily a_family)
	{
		const auto& entry = DrawFamilyOf(a_family);
		return entry.taken && ObjectClassOn(a_toggles, entry.contribution.objectClass);
	}

	void ServeDrawFamilies(std::uint32_t a_frame)
	{
		const Api::DrawFamilyFrame frame{ a_frame };
		for (const auto& entry : Entries())
			if (entry.taken && entry.contribution.frame.serve)
				entry.contribution.frame.serve(frame);
	}
}
