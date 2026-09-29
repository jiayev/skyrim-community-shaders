#pragma once

#include "Presets/UnifiedPresetCatalog.h"

#include <optional>
#include <string>

/** @brief Renders the unified Presets browser for Effects 11 and CS Presets packs. */
class PresetsPageRenderer
{
public:
	/** @brief Draws the page, discovering packs and picking an initial selection on first use. */
	static void Render();

	/** @brief Closes the screenshot lightbox if open. @return True if it consumed the request. */
	static bool CloseLightboxIfOpen();

	/** @brief One colored pill per backend the pack carries. */
	static void DrawBackendBadges(bool hasE11, bool hasCSPresets, bool hasBaseline, bool compact);

private:
	/** @brief Search box and backend filter chips. */
	static void RenderToolbar();
	/** @brief The filtered pack list. */
	static void RenderList(float width);
	/** @brief The selected pack's artwork, metadata and actions. */
	static void RenderDetail();
	/** @brief Full-viewport screenshot lightbox when a filmstrip thumb is selected. */
	static void RenderScreenshotLightbox();
	/** @brief Width DrawBackendBadges will take, for right-aligning it. */
	static float MeasureBackendBadgesWidth(bool hasE11, bool hasCSPresets, bool hasBaseline, bool compact);
	/** @brief Rounded toggle button, filled when selected. @return Whether it was clicked. */
	static bool FilterChip(const char* label, bool selected);

	/** @brief The one type shown; null shows all. */
	static std::optional<UnifiedPresetCatalog::PresetType> typeFilter;
	static char searchBuffer[128];
	static std::string selectedPackId;
	static bool discovered;

	/** @brief Pack lightboxImageIndex belongs to, so selecting another pack resets it. */
	static std::string lightboxPackId;
	/** @brief Open lightbox screenshot index into screenshotSRVs; -1 = closed. */
	static int lightboxImageIndex;
	/** @brief Ignores close clicks until the mouse is released after opening. */
	static bool lightboxSuppressClose;
};