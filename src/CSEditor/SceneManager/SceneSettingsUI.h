#pragma once

#include "RE/B/BSCoreTypes.h"  // RE::FormID
#include "SceneSettingsManager.h"

/// Scene Manager authoring UI. The panels are wired into the CS Editor; the entry
/// authoring controls are built on top of them.
namespace SceneSettingsUI
{
	/** @brief Draws the Scene Manager tab body inside a weather widget. */
	void DrawWeatherSceneTab(RE::FormID weatherId);

	/** @brief Draws the Scene Manager panel body in the CS Editor objects window. */
	void DrawSceneManagerPanel();

	/** @brief Draws the feature list under the Scene Manager category entry, since the panel body has no room for it.
	 *  Call only while that category is selected. */
	void DrawSceneManagerCategoryFeatures();

	/** @brief Draws the Locations category body: the live location chain to add from, and the user's list. */
	void DrawLocationBrowser();

	/** @brief Draws one editor window per location opened from the browser; call once per frame. */
	void DrawLocationWindows();

	/** @brief Opens the CS Editor on the page authoring a scene context, with a feature selected. */
	void OpenSceneContext(const SceneSettingsManager::SceneContextId& context, const std::string& featureShortName);

	/** @brief Pauses game time while a panel is on screen and restores it once none are.
	 *  Call every frame, even with the editor closed, so the pause is always released. */
	void SyncTimePause();
}
