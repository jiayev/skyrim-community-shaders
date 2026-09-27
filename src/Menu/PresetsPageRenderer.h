#pragma once

#include <string>

/**
 * @brief Renders the unified Presets browser for Effects 11, CS Post Processing, and Scene Manager exports.
 */
class PresetsPageRenderer
{
public:
	static void Render();

private:
	static void RenderToolbar();
	static void RenderList(float width);
	static void RenderDetail();
	static void DrawBackendBadges(bool hasE11, bool hasCSPP, bool hasSM, bool compact);
	static float MeasureBackendBadgesWidth(bool hasE11, bool hasCSPP, bool hasSM, bool compact);
	static bool FilterChip(const char* label, bool selected);

	/** Backend chips are independent toggles. All off = show all; each on = must have that backend. */
	static bool filterE11;
	static bool filterCSPP;
	static bool filterSM;
	static char searchBuffer[128];
	static std::string selectedPackId;
	static bool discovered;

	/** Featured screenshot index into screenshotSRVs; -1 = none (poster shows cover only). */
	static std::string heroPackId;
	static int heroImageIndex;
};
