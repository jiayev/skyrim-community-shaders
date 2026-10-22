#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace DCLF::TargetDump
{
	/**
	 * @brief CS_DCLF_TARGET_DUMP=f1,f2,... (a diagnostic): at those frames, counted as CS_DCLF_TEST_COMMANDS counts them (Presents without a loading screen), every render target and
	 * the depth target bound to the immediate context at each named point of the frame, and the renderer's kMAIN, main depth and kSHADOW_MASK (its PNG on every frame listed, and its raw texels), and at Present every slice of the two shadow map arrays, copied
	 * into staging textures. Render thread; with DCLF on or off (the points that only run with DCLF are labelled so). Nothing waits:
	 * the copies are read once they are done (Present, D3D11_MAP_FLAG_DO_NOT_WAIT) and written as half-size PNGs (NaN and Inf
	 * magenta, finite values x / (1 + x); every point on the first frame listed, the presented image alone on the others) with a statistics line each (finite min, max and mean per channel, NaN and Inf counts) into
	 * SKSE/dclf-dump/<count>/: "<order>-<point>-<slot>.png" and "stats.txt". A point reached twice in a frame keeps its first copy.
	 */
	void Point(const char* a_label, ID3D11Texture2D* a_extra = nullptr);
	/** @brief Every Present: the count advances; the copies whose GPU work is done are written. */
	void Present();
	/** @brief Whether this frame dumps (a point's caller may skip work to reach Point otherwise). */
	bool Armed();
}
