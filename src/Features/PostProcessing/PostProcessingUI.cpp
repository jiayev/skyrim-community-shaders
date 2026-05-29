#include "PostProcessingUI.h"

#include <array>
#include <imgui.h>

namespace PostProcessingUI
{
	namespace
	{
		constexpr std::array<const char*, 4> FFTResolutionLabels = { "128", "256", "512", "1024" };
		constexpr std::array<int, FFTResolutionLabels.size()> FFTResolutionValues = { 128, 256, 512, 1024 };
		constexpr int FFTResolutionDefaultIndex = 1;
	}

	bool FFTResolutionCombo(const char* label, int& resolution)
	{
		int currentIndex = FFTResolutionDefaultIndex;
		bool matched = false;
		for (int i = 0; i < static_cast<int>(FFTResolutionValues.size()); i++) {
			if (FFTResolutionValues[i] == resolution) {
				currentIndex = i;
				matched = true;
				break;
			}
		}

		if (!matched)
			resolution = FFTResolutionValues[currentIndex];

		if (!ImGui::Combo(label, &currentIndex, FFTResolutionLabels.data(), static_cast<int>(FFTResolutionLabels.size())))
			return !matched;

		resolution = FFTResolutionValues[currentIndex];
		return true;
	}
}
