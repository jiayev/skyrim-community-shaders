// Toggles: the flag table (seed, menu entry, dependencies), packing the active set, and the per-frame apply (render thread).
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"

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

		// The packed word: cullMode in the low two bits, then each flag in kToggles order, then the features' by their bits (the low 32).
		constexpr std::uint32_t kFirstFlagBit = 2;
		constexpr std::uint32_t kFirstRegisteredBit = kFirstFlagBit + static_cast<std::uint32_t>(std::size(kToggles));
		static_assert(kFirstRegisteredBit < 32);
		// The high word: the pipeline's state (pipelineMode, interiorCpu, exteriorCpu, cpuRecord, then cpuCadence's bits by Direct::ViewType).
		constexpr std::uint32_t kPipelineModeBit = 32, kInteriorCpuBit = 33, kExteriorCpuBit = 34, kCpuRecordBit = 35, kFirstCadenceBit = 36;
		static_assert(kFirstCadenceBit + Direct::kViewTypes <= 64 && Direct::kViewTypes <= 16, "the cadences fit the word and ToggleSet::cpuCadence");
		// Main's cadence is always per frame.
		constexpr std::uint16_t kCadenceMask = static_cast<std::uint16_t>(((1u << Direct::kViewTypes) - 1u) & ~(1u << static_cast<std::uint32_t>(Direct::ViewType::Main)));

		constexpr std::string_view kCpuViewNames[] = { "main", "decal", "forward", "refraction", "sun", "spot", "point", "sky", "precipitation", "reflection",
			"first-person" };
		static_assert(std::size(kCpuViewNames) == Direct::kViewTypes);

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
			if (const auto& pipeline = SwitchValue(Switch::CpuPipeline); !pipeline.empty() && !ApplyPipeline(set, pipeline))
				logger::warn("[DCLF] CS_DCLF_PIPELINE={}: not a pipeline (auto, manual, gpu, cpu); auto", pipeline);
			set.cpuRecord = SwitchValue(Switch::CpuRecord) == "parallel" ? static_cast<std::uint8_t>(Direct::RecordMode::Parallel) : static_cast<std::uint8_t>(Direct::RecordMode::Serial);
			if (const auto& cadence = SwitchValue(Switch::CpuCadence); !cadence.empty() && !ApplyCpuCadence(set, cadence))
				logger::warn("[DCLF] CS_DCLF_CPU_CADENCE={}: a view or value it names is unknown (views: main, decal, forward, refraction, sun, spot, point, sky, "
							 "precipitation, reflection, first-person; values: frame, snapshot)",
					cadence);
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
			a_set.cpuRecord &= 1u;
			a_set.cpuCadence &= kCadenceMask;
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

	Direct::Mode ModeOf(const ToggleSet& a_set)
	{
		Direct::Mode mode;
		mode.record = a_set.cpuRecord ? Direct::RecordMode::Parallel : Direct::RecordMode::Serial;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v)
			mode.cadence[v] = ((a_set.cpuCadence & kCadenceMask) >> v) & 1u ? Direct::Cadence::PerSnapshot : Direct::Cadence::PerFrame;
		return mode;
	}

	bool ApplyPipeline(ToggleSet& a_set, std::string_view a_value)
	{
		if (a_value == "auto" || a_value == "manual") {
			a_set.pipelineMode = a_value == "auto" ? PipelineMode::Auto : PipelineMode::Manual;
			return true;
		}
		if (a_value != "gpu" && a_value != "cpu")
			return false;
		a_set.pipelineMode = PipelineMode::Manual;
		a_set.interiorCpu = a_set.exteriorCpu = a_value == "cpu";
		return true;
	}

	std::string_view CpuViewName(std::size_t a_view)
	{
		return a_view < std::size(kCpuViewNames) ? kCpuViewNames[a_view] : std::string_view{};
	}

	std::size_t CpuViewByName(std::string_view a_name)
	{
		for (std::size_t v = 0; v < std::size(kCpuViewNames); ++v)
			if (kCpuViewNames[v] == a_name)
				return v;
		return Direct::kViewTypes;
	}

	bool ApplyCpuCadence(ToggleSet& a_set, std::string_view a_spec)
	{
		const auto bitOf = [](std::string_view a_value, bool& a_known) {
			a_known = a_value == "frame" || a_value == "snapshot";
			return a_value == "snapshot";
		};
		bool ok = true;
		if (a_spec.find('=') == std::string_view::npos) {
			bool known = false;
			const bool snapshot = bitOf(a_spec, known);
			if (known)
				a_set.cpuCadence = snapshot ? kCadenceMask : 0;
			return known;
		}
		while (!a_spec.empty()) {
			const auto comma = a_spec.find(',');
			const auto item = a_spec.substr(0, comma);
			a_spec = comma == std::string_view::npos ? std::string_view{} : a_spec.substr(comma + 1);
			const auto equals = item.find('=');
			const auto view = CpuViewByName(item.substr(0, equals));
			bool known = false;
			const bool snapshot = equals != std::string_view::npos && bitOf(item.substr(equals + 1), known);
			if (view >= Direct::kViewTypes || !known) {
				ok = false;
				continue;
			}
			const auto bit = static_cast<std::uint16_t>(1u << view);
			a_set.cpuCadence = static_cast<std::uint16_t>((snapshot ? a_set.cpuCadence | bit : a_set.cpuCadence & ~bit) & kCadenceMask);
		}
		return ok;
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

	std::uint64_t Toggles::Pack(const ToggleSet& a_set)
	{
		std::uint32_t bits = a_set.cullMode & 3u;
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			bits |= (a_set.*kToggles[i].member ? 1u : 0u) << (kFirstFlagBit + i);
		bits |= (a_set.registered & RegisteredMask()) << kFirstRegisteredBit;
		std::uint64_t word = bits;
		word |= static_cast<std::uint64_t>(a_set.pipelineMode == PipelineMode::Manual ? 1u : 0u) << kPipelineModeBit;
		word |= static_cast<std::uint64_t>(a_set.interiorCpu ? 1u : 0u) << kInteriorCpuBit;
		word |= static_cast<std::uint64_t>(a_set.exteriorCpu ? 1u : 0u) << kExteriorCpuBit;
		word |= static_cast<std::uint64_t>(a_set.cpuRecord & 1u) << kCpuRecordBit;
		word |= static_cast<std::uint64_t>(a_set.cpuCadence & kCadenceMask) << kFirstCadenceBit;
		return word;
	}

	ToggleSet Toggles::Unpack(std::uint64_t a_word)
	{
		const auto bits = static_cast<std::uint32_t>(a_word);
		ToggleSet set;
		set.cullMode = static_cast<std::uint8_t>(bits & 3u);
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			set.*kToggles[i].member = ((bits >> (kFirstFlagBit + i)) & 1u) != 0;
		set.registered = (bits >> kFirstRegisteredBit) & RegisteredMask();
		set.pipelineMode = ((a_word >> kPipelineModeBit) & 1u) != 0 ? PipelineMode::Manual : PipelineMode::Auto;
		set.interiorCpu = ((a_word >> kInteriorCpuBit) & 1u) != 0;
		set.exteriorCpu = ((a_word >> kExteriorCpuBit) & 1u) != 0;
		set.cpuRecord = static_cast<std::uint8_t>((a_word >> kCpuRecordBit) & 1u);
		set.cpuCadence = static_cast<std::uint16_t>((a_word >> kFirstCadenceBit) & kCadenceMask);
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
