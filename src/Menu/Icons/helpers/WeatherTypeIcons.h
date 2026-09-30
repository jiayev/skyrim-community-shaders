#pragma once

/**
 * @file WeatherTypeIcons.h
 * @brief Weather-class glyphs for TESWeather records.
 *
 * Prefer editor ID / display-name tokens over DATA flags — vanilla flags are often
 * wrong or used as catch-alls (many event / DLC weathers are tagged Cloudy).
 *
 * Precipitation flags (rain / snow) are still trusted when labels don't match.
 * Aurora flags are used the same way. Unassigned / unknown weathers use the
 * custom CloudSun draw-list icon (Menu/Icons/helpers/Custom/imgui_cloud_sun_icon.h).
 *
 * Glyphs are family-tagged GlyphRefs from open-source icon fonts under Icons/glyphs.
 */

#include "IconsFontAwesome5.h"
#include "IconsLucide.h"
#include "Menu/Icons/helpers/Custom/imgui_cloud_sun_icon.h"
#include "Menu/Icons/helpers/IconFonts.h"

#include "RE/T/TESWeather.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <imgui.h>

namespace WeatherTypeIcons
{
	inline constexpr Icons::GlyphRef kRain = Icons::FA(ICON_FA_CLOUD_RAIN);
	inline constexpr Icons::GlyphRef kStorm = Icons::LC(ICON_LC_CLOUD_LIGHTNING);
	inline constexpr Icons::GlyphRef kSnow = Icons::LC(ICON_LC_SNOWFLAKE);
	inline constexpr Icons::GlyphRef kCloudy = Icons::FA(ICON_FA_CLOUD);
	inline constexpr Icons::GlyphRef kFog = Icons::LC(ICON_LC_CLOUD_FOG);
	inline constexpr Icons::GlyphRef kAsh = Icons::FA(ICON_FA_SMOG);
	inline constexpr Icons::GlyphRef kAurora = Icons::LC(ICON_LC_STARS);
	inline constexpr Icons::GlyphRef kEclipse = Icons::LC(ICON_LC_ECLIPSE);
	inline constexpr Icons::GlyphRef kPleasant = Icons::LC(ICON_LC_SUN_DIM);

	/**
	 * @brief Draw a resolved weather icon into an axis-aligned size×size box.
	 * Unknown uses CloudSun; font glyphs use Icons::DrawCenteredGlyph.
	 */
	inline void Draw(std::optional<Icons::GlyphRef> icon, ImDrawList* dl, ImVec2 pos, float size, ImU32 col)
	{
		if (!dl || size <= 0.0f || !icon.has_value())
			return;
		if (!icon->IsValid()) {
			Icons::CloudSun(dl, pos, size, col);
			return;
		}
		Icons::DrawCenteredGlyph(dl, pos, ImVec2(size, size), *icon, col);
	}

	/** @brief Infer a weather-class glyph from a descriptive editor ID / name. */
	[[nodiscard]] inline std::optional<Icons::GlyphRef> ResolveFromLabel(std::string_view label) noexcept
	{
		if (label.empty())
			return std::nullopt;

		std::string lower(label);
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		auto has = [&](std::string_view token) {
			return lower.find(token) != std::string::npos;
		};

		// Most specific tokens first — e.g. SkyrimOvercastRain is rain, not cloudy.
		if (has("storm") || has("thunder") || has("lightning"))
			return kStorm;
		if (has("blizzard") || has("snow") || has("ice"))
			return kSnow;
		if (has("rain"))
			return kRain;
		if (has("ash") || has("volcan") || has("erupt"))
			return kAsh;
		if (has("eclipse") || has("bloodmoon") || has("blood moon"))
			return kEclipse;
		if (has("aurora") || has("northernlight") || has("northern light"))
			return kAurora;
		if (has("fog") || has("mist") || has("haze"))
			return kFog;
		if (has("cloudy") || has("overcast") || has("clouded"))
			return kCloudy;
		if (has("clear") || has("sunny") || has("pleasant") || has("fair"))
			return kPleasant;
		return std::nullopt;
	}

	/**
	 * @brief Primary weather-class icon for a weather record.
	 * @return nullopt when weather is null; empty GlyphRef for unknown (CloudSun);
	 *         otherwise a family-tagged glyph.
	 */
	[[nodiscard]] inline std::optional<Icons::GlyphRef> Resolve(RE::TESWeather* weather) noexcept
	{
		if (!weather)
			return std::nullopt;

		if (const char* editorId = weather->GetFormEditorID()) {
			if (auto icon = ResolveFromLabel(editorId))
				return icon;
		}
		if (const char* name = weather->GetName()) {
			if (auto icon = ResolveFromLabel(name))
				return icon;
		}

		using Flag = RE::TESWeather::WeatherDataFlag;
		// Only trust precipitation / aurora flags — Cloudy and Pleasant are widely
		// mis-set on event, scripted, and DLC weathers, which made the cloud glyph
		// show up everywhere. Those fall through to CloudSun instead.
		if (weather->data.flags.any(Flag::kRainy))
			return kRain;
		if (weather->data.flags.any(Flag::kSnow))
			return kSnow;
		if (weather->data.flags.any(Flag::kPermAurora) || weather->data.flags.any(Flag::kAuroraFollowsSun))
			return kAurora;

		return Icons::GlyphRef{};  // unknown → CloudSun
	}
}
