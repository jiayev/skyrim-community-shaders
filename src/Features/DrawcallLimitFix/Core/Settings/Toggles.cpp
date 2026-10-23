// Toggles: the flag table (seed, menu entry, dependencies), packing the active set, and the per-frame apply (render thread).
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

namespace DCLF
{
	namespace
	{
		using S = ToggleSet;
		constexpr std::string_view kOn = "1";

		constexpr ToggleInfo kToggles[] = {
			// (Features/Skins' "skinned", which opens the "Object classes" heading, is listed here, before the projected's, with
			// Features/TreeWind's "trees" and Features/Decals' toggles after it.)
			{ &S::projectedUv, Switch::ProjectedUv, kOn, "projected", {}, "Projected UV", nullptr, true, {} },
			{ &S::plainSnowSparkle, Switch::PlainSnowSparkle, kOn, "plain snow sparkle", {}, "Sparkle on plain snow layers",
				"Glacier and ice snow layers carry a material without sparkle values, so the engine reads whatever memory follows it (in practice no "
				"sparkle). On: DCLF gives them the snow material's default sparkle. Off: no sparkle.", false, {} },
			// (Features/Landscape's "terrain", then Integrations/TerrainBlending's "terrain under TB", follow the projected's here; then
			// Features/Forward's "forward", Features/Effects' "effects" after it, and Features/Forward's "refraction" after that.)
			// (Features/Particles', Features/Billboards' and Features/SwitchNodes' toggles follow the refraction's here, then Features/Lod's
			// "object LOD" and "terrain LOD" and Features/TreeLod's "tree LOD"; then Features/Skins' "skin partitions" after the tree LOD's,
			// and Features/Actors' "actors" after it.)
			{ &S::shadows, Switch::Shadows, kOn, "shadows", "Shadow views", "Draw the shadow views", nullptr, false, {} },
			// (Features/Shadows/Sun's "skip sun accumulation", then its "exclude sun entries", follow the shadows' here.)
			// It needs the sun's entry exclusion by name (SeedRegistered resolves it): without Features/Shadows/Sun it never takes effect.
			{ &S::excludePrimaryEntries, Switch::PrimaryExclude, kOn, "exclude primary entries", {}, "Take DCLF's objects out of the engine's main camera cull",
				"The main camera's cull and registration skip every reference DCLF draws entirely; DCLF builds their main passes itself and runs their fade updates.", false,
				{}, false, {}, {}, {}, { "exclude sun entries" } },
			// (Features/OcclusionMaps' "sky occlusion" and "precipitation occlusion" follow here: the first feature toggles registered, their
			// bits right after these; then Features/Reflection's "reflections" and Features/FirstPerson's "first person".)
		};

		// The packed word: cullMode in the low two bits, then each flag in kToggles order, then the features' by their bits.
		constexpr std::uint32_t kFirstFlagBit = 2;
		constexpr std::uint32_t kFirstRegisteredBit = kFirstFlagBit + static_cast<std::uint32_t>(std::size(kToggles));
		static_assert(kFirstRegisteredBit < 32);

		// The features' toggles (RegisterToggle), in registration order: their strings (the table's entries view them), their anchors and
		// needs by name, and their entries; then the table with them placed in it (SeedRegistered), and their count. Written at install
		// alone (the main thread, before DCLF's threads start), read-only after; until placed, the table is kToggles.
		struct RegisteredToggle
		{
			std::string name, label, tooltip, section, onValue, after, before, feature;
			std::string needs[2];
			ToggleInfo info{ nullptr, Switch::Count, {}, {}, {}, nullptr, nullptr, false, { nullptr, nullptr } };  // spelled out: MSVC ICEs on {} here
		};
		std::deque<RegisteredToggle>& RegisteredToggles()
		{
			static std::deque<RegisteredToggle> registered;
			return registered;
		}
		std::vector<ToggleInfo>& Table()
		{
			static std::vector<ToggleInfo> table(std::begin(kToggles), std::end(kToggles));
			return table;
		}
		std::atomic<std::uint32_t> registeredCount{ 0 };
		std::uint32_t RegisteredMask()
		{
			return (1u << registeredCount.load(std::memory_order_relaxed)) - 1u;
		}

		bool NeedsMet(const ToggleInfo& a_info, const ToggleSet& a_set)
		{
			if (a_info.needsMissing)
				return false;
			for (const auto need : a_info.needs)
				if (need && !(a_set.*need))
					return false;
			for (const auto need : a_info.registeredNeeds)
				if (need && !a_set.On(need))
					return false;
			return true;
		}

		const std::string& SeedValue(const ToggleInfo& a_info)
		{
			return a_info.member ? SwitchValue(a_info.seed) : SwitchValue(a_info.registeredSeed);
		}

		ToggleSet FromSwitches()
		{
			ToggleSet set;
			const auto& cull = SwitchValue(Switch::Cull);
			set.cullMode = (cull.empty() || cull == "occlusion") ? 2 : cull == "frustum" ? 1 : 0;
			for (const auto& toggle : Table()) {
				const auto& value = SeedValue(toggle);
				SetToggleValue(set, toggle, value.empty() ? !toggle.offByDefault : value == toggle.onValue);
			}
			return set;
		}

		/** @brief The combinations that cannot hold: each flag off while one it needs is off (the table's order resolves chains). */
		ToggleSet Normalised(ToggleSet a_set)
		{
			for (const auto& toggle : Table())
				SetToggleValue(a_set, toggle, ToggleValue(a_set, toggle) && NeedsMet(toggle, a_set));
			return a_set;
		}

		bool EntersClassification(const ToggleSet& a, const ToggleSet& b)
		{
			for (const auto& toggle : Table())
				if (toggle.entersClassification && ToggleValue(a, toggle) != ToggleValue(b, toggle))
					return true;
			return false;
		}

		/** @brief The toggle named a_name in a_table, by its index (none: -1). */
		std::ptrdiff_t ToggleAt(const std::vector<ToggleInfo>& a_table, std::string_view a_name)
		{
			for (std::size_t i = 0; i < a_table.size(); ++i)
				if (a_table[i].name == a_name)
					return static_cast<std::ptrdiff_t>(i);
			return -1;
		}
		std::ptrdiff_t ToggleAt(std::string_view a_name) { return ToggleAt(Table(), a_name); }

		/**
		 * @brief The features' toggles whose a_anchor names a_name, in registration order, each with the toggles anchored at it around it
		 * (those before it first, those after it next), onto a_table. a_listed marks the toggles placed.
		 */
		void ListAnchoredAt(std::vector<ToggleInfo>& a_table, std::vector<std::uint8_t>& a_listed, std::string RegisteredToggle::*a_anchor, std::string_view a_name)
		{
			const auto& registered = RegisteredToggles();
			for (std::size_t i = 0; i < registered.size(); ++i)
				if (!a_listed[i] && registered[i].*a_anchor == a_name) {
					a_listed[i] = 1;
					const auto& entry = registered[i];
					ListAnchoredAt(a_table, a_listed, &RegisteredToggle::before, entry.name);
					a_table.push_back(entry.info);
					ListAnchoredAt(a_table, a_listed, &RegisteredToggle::after, entry.name);
				}
		}

		/**
		 * @brief Once every feature registered: the table in menu order (each Core flag, the features' toggles before it first and after it
		 * next, each with the toggles anchored at it around it; the unanchored last), then the features' needs resolved against it.
		 */
		void PlaceRegistered()
		{
			const auto& registered = RegisteredToggles();
			std::vector<ToggleInfo> table;
			table.reserve(std::size(kToggles) + registered.size());
			std::vector<std::uint8_t> listed(registered.size(), 0);
			for (const auto& toggle : kToggles) {
				ListAnchoredAt(table, listed, &RegisteredToggle::before, toggle.name);
				table.push_back(toggle);
				ListAnchoredAt(table, listed, &RegisteredToggle::after, toggle.name);
			}
			// A toggle no Core flag lists has an anchor that names no toggle, or anchors that loop back to it.
			for (std::size_t i = 0; i < registered.size(); ++i) {
				const auto& entry = registered[i];
				if (listed[i])
					continue;
				if (!entry.after.empty() || !entry.before.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s toggle {} is listed {} {}, which no toggle is (or its anchors loop)", entry.feature,
						entry.name, entry.after.empty() ? "before" : "after", entry.after.empty() ? entry.before : entry.after));
				table.push_back(entry.info);
			}
			// What a feature's toggle needs comes before it (Normalised resolves chains in table order): Core's, or a feature's (Features/
			// Decals: the layers need the decals; Features/FirstPerson: the first person needs the skinned and the actors).
			for (const auto& entry : registered) {
				const auto at = ToggleAt(table, entry.name);
				auto& info = table[static_cast<std::size_t>(at)];
				for (std::size_t n = 0; n < std::size(entry.needs); ++n) {
					if (entry.needs[n].empty())
						continue;
					const auto need = ToggleAt(table, entry.needs[n]);
					if (need < 0 || need >= at)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s toggle {} needs {}, which is not listed before it", entry.feature, entry.name, entry.needs[n]));
					const auto& needed = table[static_cast<std::size_t>(need)];
					if (needed.member)
						info.needs[n] = needed.member;
					else
						info.registeredNeeds[n] = needed.registered;
				}
			}
			Table() = std::move(table);
		}
	}

	std::span<const ToggleInfo> ToggleTable()
	{
		return Table();
	}

	Api::ToggleId RegisterToggle(const Api::FeatureToggle& a_toggle, std::string_view a_feature)
	{
		auto& registered = RegisteredToggles();
		const std::uint32_t bit = registeredCount.load(std::memory_order_relaxed);
		if (a_toggle.name.empty() || a_toggle.label.empty() || !a_toggle.seed)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete toggle", a_feature));
		const bool taken = std::ranges::any_of(kToggles, [&](const ToggleInfo& a_info) { return a_info.name == a_toggle.name; }) ||
		                   std::ranges::any_of(registered, [&](const RegisteredToggle& a_entry) { return a_entry.name == a_toggle.name; });
		if (taken)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered the toggle {}, which is taken", a_feature, a_toggle.name));
		if (kFirstRegisteredBit + bit >= 32)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s toggle {} is past the toggle word's 32 bits", a_feature, a_toggle.name));
		// Its anchor and needs may name a feature's toggle registered after it: SeedRegistered places it once every feature registered.
		if (!a_toggle.before.empty() && !a_toggle.after.empty())
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s toggle {} is listed before {} and after {}", a_feature, a_toggle.name, a_toggle.before,
				a_toggle.after));
		auto& entry = registered.emplace_back(RegisteredToggle{ std::string(a_toggle.name), std::string(a_toggle.label), std::string(a_toggle.tooltip),
			std::string(a_toggle.section), std::string(a_toggle.onValue), std::string(a_toggle.after), std::string(a_toggle.before), std::string(a_feature),
			{ std::string(a_toggle.needs[0]), std::string(a_toggle.needs[1]) } });
		entry.info = ToggleInfo{ nullptr, Switch::Count, entry.onValue, entry.name, entry.section, entry.label.c_str(),
			entry.tooltip.empty() ? nullptr : entry.tooltip.c_str(), a_toggle.entersClassification, {}, a_toggle.offByDefault, Api::ToggleId{ bit }, a_toggle.seed };
		registeredCount.store(bit + 1, std::memory_order_relaxed);
		return { bit };
	}

	Api::ToggleId FindToggle(std::string_view a_name)
	{
		for (const auto& toggle : Table())
			if (toggle.name == a_name)
				return toggle.registered;
		return {};
	}

	bool Toggles::Editable(const ToggleInfo& a_info) const
	{
		return NeedsMet(a_info, Normalised(requested));
	}

	Toggles& Toggles::Get()
	{
		static Toggles toggles;
		return toggles;
	}

	Toggles::Toggles() :
		requested(FromSwitches())
	{
		active.store(Pack(Normalised(requested)), std::memory_order_relaxed);
		frame.store(active.load(std::memory_order_relaxed), std::memory_order_relaxed);
	}

	void Toggles::SeedRegistered()
	{
		// The features' toggles by their anchors, which may name a toggle a later feature registered.
		PlaceRegistered();
		// The Core flags' needs by name, now every feature registered: before the flag (Normalised resolves chains in table order), or
		// missing (no feature registered it), which holds the flag off.
		auto& table = Table();
		for (std::size_t i = 0; i < table.size(); ++i) {
			auto& toggle = table[i];
			for (std::size_t n = 0; n < std::size(toggle.needNames); ++n) {
				if (toggle.needNames[n].empty())
					continue;
				const auto need = ToggleAt(toggle.needNames[n]);
				if (need < 0) {
					toggle.needsMissing = true;
					continue;
				}
				if (static_cast<std::size_t>(need) >= i)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the toggle {} needs {}, which is not listed before it", toggle.name, toggle.needNames[n]));
				const auto& needed = table[static_cast<std::size_t>(need)];
				if (needed.member)
					toggle.needs[n] = needed.member;
				else
					toggle.registeredNeeds[n] = needed.registered;
			}
		}
		requested.registered = FromSwitches().registered;
		active.store(Pack(Normalised(requested)), std::memory_order_relaxed);
		// The frames' until the first publication is installed.
		frame.store(active.load(std::memory_order_relaxed), std::memory_order_relaxed);
	}

	std::uint32_t Toggles::Pack(const ToggleSet& a_set)
	{
		std::uint32_t bits = a_set.cullMode & 3u;
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			bits |= (a_set.*kToggles[i].member ? 1u : 0u) << (kFirstFlagBit + i);
		bits |= (a_set.registered & RegisteredMask()) << kFirstRegisteredBit;
		return bits;
	}

	ToggleSet Toggles::Unpack(std::uint32_t a_bits)
	{
		ToggleSet set;
		set.cullMode = static_cast<std::uint8_t>(a_bits & 3u);
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			set.*kToggles[i].member = ((a_bits >> (kFirstFlagBit + i)) & 1u) != 0;
		set.registered = (a_bits >> kFirstRegisteredBit) & RegisteredMask();
		return set;
	}

	bool Toggles::BeginFrame()
	{
		const auto next = Normalised(requested);
		const auto current = Active();
		if (next == current)
			return false;
		active.store(Pack(next), std::memory_order_relaxed);
		generation.fetch_add(1, std::memory_order_relaxed);
		return EntersClassification(current, next);
	}
}
