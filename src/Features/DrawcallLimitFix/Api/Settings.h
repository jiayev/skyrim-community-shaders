#pragma once

// The settings contracts (Api::InstallContext::RegisterSwitch, RegisterToggle): a feature's own CS_* switches, beside Core's registry
// (Core/Settings/Switches.h), and its live toggles, beside Core's (Core/Settings/Toggles.h).

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/** @brief What a switch is for: the startup summary groups by it. */
	enum class SwitchKind : std::uint8_t
	{
		Feature,
		Parity,
		Diagnostic,
		Test,
		Other,
	};

	/** @brief Which values of a feature switch narrow DCLF's featureset (the startup log names a reduced run). */
	enum class SwitchReduced : std::uint8_t
	{
		Never,      // not a feature
		UnlessOne,  // on when unset or "1"
		Zero,       // off only with "0"
	};

	/** @brief A registered switch (RegisterSwitch's result): SwitchValue and SwitchEnabled read it by this. */
	struct SwitchId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A switch: read once, from the environment or the switches file (Switches.h), when it is registered. Listed in the startup
	 * summary and the reduced-featureset line right after the Core switch named by after, or right before the one named by before (after
	 * the features' listed after the Core switch before it); empty both: last. Features' switches at one place are listed in their
	 * registration order.
	 */
	struct FeatureSwitch
	{
		std::string_view name;  // CS_DCLF_...
		SwitchKind kind = SwitchKind::Feature;
		SwitchReduced reduced = SwitchReduced::Never;
		std::string_view description;
		std::string_view after;
		std::string_view before;  // or empty (one of after and before)
	};

	/** @brief A registered toggle (RegisterToggle's result): ToggleSet::On reads it by this. */
	struct ToggleId
	{
		std::uint32_t bit = ~0u;
		explicit operator bool() const { return bit != ~0u; }
	};

	/**
	 * @brief A live toggle (Toggles.h): seeded from its switch, edited from the menu, in force from the next frame's start. Shown, and
	 * named on the menu's "This frame" line, right after the Core toggle named by after (empty: last).
	 */
	struct FeatureToggle
	{
		std::string_view name;     // the "This frame" line's, and the test harness's (dclf:<name>=<0|1>)
		std::string_view label;    // the menu's
		std::string_view tooltip;  // or empty
		std::string_view section;  // the menu heading it opens, or empty
		SwitchId seed;             // a registered switch
		std::string_view onValue = "1";  // the seed's value besides unset that turns it on
		bool entersClassification = false;  // a change drops the classification caches (Coordinator::InvalidateVerdicts)
		bool offByDefault = false;          // unset seeds it off: only onValue turns it on
		std::string_view after;
		std::string_view needs[2];  // the toggles it needs on (their names: Core's, or a feature's registered before it), or empty
	};
}
