#pragma once

/// CS Editor windows that edit feature settings directly, mirroring the main menu's feature pages.
namespace FeatureSettingsWindow
{
	/**
	 * @brief Draws the open Features and Post Processing windows and their shared constraint warning.
	 * @param featuresOpen Features window visibility; cleared by its close button.
	 * @param postProcessingOpen Post Processing window visibility; cleared by its close button.
	 */
	void Draw(bool& featuresOpen, bool& postProcessingOpen);
}
