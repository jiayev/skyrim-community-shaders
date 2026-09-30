#include "PostProcessingMode.h"

#include "Features/Effects11.h"
#include "Features/Effects11/EffectManager.h"
#include "Features/LinearLighting.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Utils/UI.h"

#include <array>

namespace PostProcessingMode
{
	namespace
	{
		constexpr int kModeCount = 3;

		/** @brief Effects 11 feature is ready to claim the pipeline slot (preset optional). */
		bool CanUseEffects11()
		{
			const auto& effectManager = EffectManager::GetSingleton();
			return globals::features::effects11.loaded && effectManager.IsInitialized();
		}
	}

	Mode Get()
	{
		// UseEffect selects the Effects 11 slot even before a preset compiles; original PP is forced then.
		if (globals::features::effects11.IsUseEffectEnabled())
			return Mode::Effects11;
		const auto& postProcessing = globals::features::postProcessing;
		return postProcessing.loaded && !postProcessing.bypass ? Mode::PostProcessing : Mode::Vanilla;
	}

	void Set(Mode mode)
	{
		const bool effects11Available = CanUseEffects11();
		if (mode == Mode::Effects11 && !effects11Available)
			return;
		if (effects11Available)
			globals::features::effects11.SetUseEffect(mode == Mode::Effects11);
		globals::features::postProcessing.bypass = mode != Mode::PostProcessing;
		// Switching to Post Processing always starts linear; running without it is a manual opt-out.
		if (mode == Mode::PostProcessing)
			globals::features::linearLighting.settings.enableLinearLighting = true;
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

		const bool effects11WithoutPreset = globals::features::effects11.IsUseOriginalPostProcessingForced();
		if (effects11WithoutPreset) {
			Util::AddTooltip(T("ui.post_processing_mode.effects11_no_preset_tooltip",
				"Effects 11 is selected, but no usable preset is loaded.\n"
				"Use Original Post Processing stays on so the game's tonemap keeps running until you install a preset."));
			ImGui::Spacing();
			Util::Text::WrappedWarning("%s", T("ui.post_processing_mode.effects11_no_preset",
				"No Effects 11 preset loaded — Use Original Post Processing is forced on."));
		} else {
			Util::AddTooltip(T("ui.post_processing_mode.tooltip",
				"Only one pipeline runs at a time. Vanilla uses the game's original post processing."));
		}
	}
}
