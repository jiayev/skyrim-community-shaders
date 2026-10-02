#pragma once

/**
 * @file SceneActionIcons.h
 * @brief Shared FA glyphs for Scene Manager list / toolbar actions.
 */

#include "IconsFontAwesome5.h"
#include "IconsLucide.h"
#include "Menu/Icons/helpers/IconFonts.h"

namespace SceneActionIcons
{
	inline constexpr Icons::GlyphRef kDelete = Icons::FA(ICON_FA_TRASH_ALT);
	inline constexpr Icons::GlyphRef kAdd = Icons::FA(ICON_FA_PLUS);
	inline constexpr Icons::GlyphRef kAdded = Icons::FA(ICON_FA_CHECK);
	inline constexpr Icons::GlyphRef kSave = Icons::FA(ICON_FA_SAVE);
	inline constexpr Icons::GlyphRef kPause = Icons::FA(ICON_FA_PAUSE);
	inline constexpr Icons::GlyphRef kResume = Icons::FA(ICON_FA_PLAY);
	inline constexpr Icons::GlyphRef kCopy = Icons::FA(ICON_FA_COPY);
	/// Same share glyph the feature list and settings windows use for their export buttons.
	inline constexpr Icons::GlyphRef kExport = Icons::LC(ICON_LC_SHARE);
}
