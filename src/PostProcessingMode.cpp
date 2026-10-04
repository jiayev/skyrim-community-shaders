#include "PostProcessingMode.h"

#include "Features/Effects11.h"
#include "Features/Effects11/EffectManager.h"
#include "Features/LinearLighting.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

#include <array>
#include <optional>

namespace PostProcessingMode
{
	namespace
	{
		constexpr int kModeCount = 3;
		constexpr const char* kSettingKey = "Post Processing Mode";
		constexpr Mode kDefaultMode = Mode::Vanilla;

		/** @brief Mode read from the user settings, held until Effects 11 can take it. */
		std::optional<Mode> pendingMode;
		/** @brief The pending mode is only a placeholder, so ApplyPending() picks the default from what is installed. */
		bool detectPreset = false;

		/** @brief Effects 11 is initialized, so its UseEffect setting can be written. */
		bool IsEffects11Ready()
		{
			return globals::features::effects11.loaded && EffectManager::GetSingleton().IsInitialized();
		}

		/** @brief Effects 11 has a compiled preset to run, so it can claim the pipeline slot. */
		bool CanUseEffects11()
		{
			return IsEffects11Ready() && EffectManager::GetSingleton().IsPresetLoaded();
		}

		/** @brief Effects 11 with a loaded preset, else Post Processing if installed, else Vanilla. */
		Mode ResolveDefaultMode()
		{
			if (CanUseEffects11())
				return Mode::Effects11;
			return globals::features::postProcessing.loaded ? Mode::PostProcessing : Mode::Vanilla;
		}

		/** @brief Switches the pipeline flags without the user-facing side effects of Set(). */
		void Apply(Mode mode)
		{
			if (IsEffects11Ready())
				globals::features::effects11.SetUseEffect(mode == Mode::Effects11);
			globals::features::postProcessing.bypass = mode != Mode::PostProcessing;
		}
	}

	Mode Get()
	{
		if (globals::features::effects11.IsPresetEnabled())
			return Mode::Effects11;
		const auto& postProcessing = globals::features::postProcessing;
		return postProcessing.loaded && !postProcessing.bypass ? Mode::PostProcessing : Mode::Vanilla;
	}

	void Set(Mode mode)
	{
		if (mode == Mode::Effects11 && !CanUseEffects11())
			return;
		pendingMode.reset();
		Apply(mode);
		// Switching to Post Processing always starts linear; running without it is a manual opt-out.
		if (mode == Mode::PostProcessing)
			globals::features::linearLighting.settings.enableLinearLighting = true;
	}

	void Load(const json& a_general)
	{
		const auto saved = a_general.find(kSettingKey);
		const auto savedMode = saved != a_general.end() && saved->is_string() ?
		                           magic_enum::enum_cast<Mode>(saved->get<std::string>()) :
		                           std::nullopt;
		// Without a user settings file, the merged value is the SettingsDefault.json one, not a user choice.
		std::error_code ec;
		detectPreset = !savedMode || !std::filesystem::exists(Util::PathHelpers::GetSettingsUserPath(), ec);
		pendingMode = savedMode.value_or(kDefaultMode);
		// Effects 11 is not initialized this early, so only the bypass can apply now.
		globals::features::postProcessing.bypass = *pendingMode != Mode::PostProcessing;
	}

	void Save(json& a_general)
	{
		// Keep an unresolved default unsaved so preset detection still runs on the next boot.
		if (pendingMode && detectPreset)
			return;
		// Before Effects 11 initializes, Get() cannot see its UseEffect choice yet.
		const bool modeKnown = globals::features::postProcessing.loaded &&
		                       (!globals::features::effects11.loaded || EffectManager::GetSingleton().IsInitialized());
		a_general[kSettingKey] = magic_enum::enum_name(pendingMode.value_or(modeKnown ? Get() : kDefaultMode));
	}

	void ApplyPending()
	{
		if (!pendingMode)
			return;
		const Mode mode = detectPreset ? ResolveDefaultMode() : *pendingMode;
		Apply(mode == Mode::Effects11 && !CanUseEffects11() ? Mode::Vanilla : mode);
		pendingMode.reset();
	}

	void DrawSelector()
	{
		std::array<Mode, kModeCount> modes{};
		std::array<const char*, kModeCount> labels{};
		int count = 0;
		int selected = 0;
		const Mode current = Get();
		const auto addSegment = [&](Mode mode, const char* label) {
			if (mode == current)
				selected = count;
			modes[count] = mode;
			labels[count++] = label;
		};

		addSegment(Mode::Vanilla, T("ui.post_processing_mode.vanilla", "Vanilla"));
		if (globals::features::postProcessing.loaded)
			addSegment(Mode::PostProcessing, T("ui.post_processing_mode.post_processing", "Post Processing"));
		if (CanUseEffects11())
			addSegment(Mode::Effects11, T("ui.post_processing_mode.effects11", "Effects 11"));

		// Re-clicking the active mode must not undo a manual Linear Lighting opt-out.
		if (Util::SegmentedControl("PostProcessingMode", labels.data(), count, selected) && modes[selected] != current)
			Set(modes[selected]);

		Util::AddTooltip(T("ui.post_processing_mode.tooltip",
			"Only one pipeline runs at a time. Vanilla uses the game's original post processing."));
	}
}
