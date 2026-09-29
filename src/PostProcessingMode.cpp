#include "PostProcessingMode.h"

#include "Features/Effects11.h"
#include "Features/Effects11/EffectManager.h"
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

		/** @brief Without a compiled preset UseEffect has nothing to enable. */
		bool CanUseEffects11()
		{
			const auto& effectManager = EffectManager::GetSingleton();
			return globals::features::effects11.loaded && effectManager.IsInitialized() && effectManager.IsPresetLoaded();
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
		const bool effects11Available = CanUseEffects11();
		if (mode == Mode::Effects11 && !effects11Available)
			return;
		if (effects11Available)
			globals::features::effects11.SetUseEffect(mode == Mode::Effects11);
		globals::features::postProcessing.bypass = mode != Mode::PostProcessing;
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

		if (Util::SegmentedControl("PostProcessingMode", labels.data(), count, selected))
			Set(modes[selected]);
		Util::AddTooltip(T("ui.post_processing_mode.tooltip",
			"Only one pipeline runs at a time. Vanilla uses the game's original post processing."));
	}
}
