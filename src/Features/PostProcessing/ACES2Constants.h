#pragma once
// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
//
// CPU-side ACES 2.0 Output Transform parameter pre-computation.
// Faithfully translated from aces-core CTL reference.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ACES2
{

	static constexpr int TABLE_SIZE = 360;
	static constexpr int BASE_INDEX = 1;
	static constexpr int TOTAL_TABLE_SIZE = TABLE_SIZE + 2;  // 362
	static constexpr double HUE_LIMIT = 360.0;
	static constexpr int CUSP_CORNER_COUNT = 6;
	static constexpr int TOTAL_CORNER_COUNT = CUSP_CORNER_COUNT + 2;
	static constexpr int MAX_SORTED_CORNERS = 2 * CUSP_CORNER_COUNT;

	// CAM parameters
	static constexpr double REF_LUMINANCE = 100.0;
	static constexpr double L_A = 100.0;
	static constexpr double Y_b = 20.0;
	static constexpr double J_SCALE = 100.0;
	static constexpr double CAM_NL_Y_REF = 100.0;
	static constexpr double CAM_NL_OFFSET = 0.2713 * CAM_NL_Y_REF;
	static constexpr double CAM_NL_SCALE = 4.0 * CAM_NL_Y_REF;

	// Surround: Dim
	static constexpr double SURROUND_Y = 0.59;
	static constexpr double SURROUND_C = 0.9;
	static constexpr double MODEL_GAMMA = SURROUND_Y * (1.48 + std::sqrt(Y_b / REF_LUMINANCE));

	// Chroma compression
	static constexpr double CHROMA_COMPRESS = 2.4;
	static constexpr double CHROMA_COMPRESS_FACT = 3.3;
	static constexpr double CHROMA_EXPAND = 1.3;
	static constexpr double CHROMA_EXPAND_FACT = 0.69;
	static constexpr double CHROMA_EXPAND_THR = 0.5;

	// Gamut compression
	static constexpr double SMOOTH_CUSPS = 0.12;
	static constexpr double SMOOTH_M = 0.27;
	static constexpr double CUSP_MID_BLEND = 1.3;
	static constexpr double FOCUS_GAIN_BLEND = 0.3;
	static constexpr double FOCUS_ADJUST_GAIN = 0.55;
	static constexpr double FOCUS_DISTANCE = 1.35;
	static constexpr double FOCUS_DISTANCE_SCALING = 1.75;
	static constexpr double COMPRESSION_THRESHOLD = 0.75;

	// Gamma search
	static constexpr double GAMMA_MINIMUM = 0.0;
	static constexpr double GAMMA_MAXIMUM = 5.0;
	static constexpr double GAMMA_SEARCH_STEP = 0.4;
	static constexpr double GAMMA_ACCURACY = 1e-5;

	static constexpr double REACH_CUSP_TOLERANCE = 1e-3;
	static constexpr double DISPLAY_CUSP_TOLERANCE = 1e-7;

	static constexpr double PI = 3.14159265358979323846;

	// ============================================================
	// Chromaticities
	// ============================================================
	struct Chromaticities
	{
		double red[2], green[2], blue[2], white[2];
	};

	static constexpr Chromaticities AP0 = {
		{ 0.73470, 0.26530 },
		{ 0.00000, 1.00000 },
		{ 0.00010, -0.07700 },
		{ 0.32168, 0.33767 }
	};

	static constexpr Chromaticities AP1 = {
		{ 0.713, 0.293 },
		{ 0.165, 0.830 },
		{ 0.128, 0.044 },
		{ 0.32168, 0.33767 }
	};

	static constexpr Chromaticities REC709 = {
		{ 0.6400, 0.3300 },
		{ 0.3000, 0.6000 },
		{ 0.1500, 0.0600 },
		{ 0.3127, 0.3290 }
	};

	static constexpr Chromaticities BT2020 = {
		{ 0.7080, 0.2920 },
		{ 0.1700, 0.7970 },
		{ 0.1310, 0.0460 },
		{ 0.3127, 0.3290 }
	};

}  // namespace ACES2
