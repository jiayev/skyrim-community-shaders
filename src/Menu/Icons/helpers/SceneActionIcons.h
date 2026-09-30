#pragma once

/**
 * @file SceneActionIcons.h
 * @brief Shared FA glyphs for Scene Manager list / toolbar actions.
 */

#include "IconsFontAwesome5.h"
#include "Menu/Icons/helpers/IconFonts.h"

namespace SceneActionIcons
{
	inline constexpr Icons::GlyphRef kDelete = Icons::FA(ICON_FA_TRASH_ALT);
	inline constexpr Icons::GlyphRef kAdd = Icons::FA(ICON_FA_PLUS);
	inline constexpr Icons::GlyphRef kAdded = Icons::FA(ICON_FA_CHECK);
	inline constexpr Icons::GlyphRef kSave = Icons::FA(ICON_FA_SAVE);
}
