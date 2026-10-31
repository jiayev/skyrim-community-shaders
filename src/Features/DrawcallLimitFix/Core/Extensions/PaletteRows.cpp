// The palette rows classes' entries and the walk's and frame start's calls into them (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolvePaletteRows (InstallFeatures, before DCLF's threads exist), read-only after. By class index - 1.
		struct Resolved
		{
			std::array<PaletteRowsEntry, Api::kPaletteRowsClasses> entries;
			std::array<bool, Api::kPaletteRowsClasses> registered{};
		};

		Resolved& State()
		{
			static Resolved resolved;
			return resolved;
		}
	}

	void ResolvePaletteRows(const std::array<Api::PaletteRowsContribution, Api::kPaletteRowsClasses>& a_registered)
	{
		auto& resolved = State();
		for (std::size_t c = 0; c < a_registered.size(); ++c) {
			if (a_registered[c].itemClass == Api::FrameValueItemClass::Count)
				continue;
			auto& entry = resolved.entries[c];
			entry.name = std::string(a_registered[c].name);
			entry.contribution = a_registered[c];
			entry.contribution.name = entry.name;
			resolved.registered[c] = true;
		}
	}

	const PaletteRowsEntry* PaletteRows(Api::FrameValueItemClass a_class)
	{
		const auto index = static_cast<std::size_t>(a_class);
		if (index == 0 || index > Api::kPaletteRowsClasses)
			return nullptr;
		auto& resolved = State();
		return resolved.registered[index - 1] ? &resolved.entries[index - 1] : nullptr;
	}

	std::uint32_t PaletteRowsOf(Api::FrameValueItemClass a_class, std::uint32_t a_units)
	{
		const auto* entry = PaletteRows(a_class);
		return entry ? entry->contribution.rule.Rows(a_units) : 0u;
	}

	bool PaletteGeometryOf(Api::FrameValueItemClass a_class, std::uint32_t a_units, Api::PaletteGeometrySource& a_source)
	{
		const auto* entry = PaletteRows(a_class);
		if (!entry)
			return false;
		a_source.units = entry->contribution.rule.Units(a_units);
		return entry->contribution.geometrySource(a_source);
	}

	void PaletteRowsFrameStart()
	{
		const auto& resolved = State();
		for (std::size_t c = 0; c < resolved.entries.size(); ++c)
			if (resolved.registered[c] && resolved.entries[c].contribution.frameStart)
				resolved.entries[c].contribution.frameStart();
	}
}
