// Curated Tabler Icons macros - only glyphs referenced from src/.
// To add an icon: copy one #define (UTF-8 + U+ comment) from IconFontCppHeaders / webfont CSS,
// then add a singleton pair for that codepoint in ICON_TI_GLYPH_RANGES.

#pragma once

#define FONT_ICON_FILE_NAME_TI "tabler-icons.ttf"

#define ICON_MIN_TI 0xf548
#define ICON_MAX_16_TI 0xf548
#define ICON_MAX_TI 0xf548

#define ICON_TI_CLOCK_PAUSE "\xef\x95\x88"	// U+f548

// Inclusive [min,max] pairs for ImFontAtlas::AddFontFromFileTTF glyphRanges (0-terminated).
inline constexpr unsigned short ICON_TI_GLYPH_RANGES[] = {
	0xf548, 0xf548,
	0
};

