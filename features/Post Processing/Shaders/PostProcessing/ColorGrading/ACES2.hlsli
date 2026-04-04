// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
//
// ACES 2.0 Output Transform - HLSL port
// Faithfully translated from the official CTL reference implementation:
//   aces-core/lib/Lib.Academy.OutputTransform.ctl
//   aces-core/lib/Lib.Academy.Tonescale.ctl
//
// This implementation performs the complete ACES 2.0 Output Transform pipeline:
//   1. RGB -> JMh (CAM16-based color appearance model)
//   2. Tonescale (Michaelis-Menten + flare compensation)
//   3. Chroma compression (in-gamut saturation management)
//   4. Gamut compression (mapping to output display gamut)
//   5. JMh -> RGB (display-referred output)
//
// CPU-precomputed LUT data is passed via StructuredBuffer.

#ifndef ACES2_HLSLI
#define ACES2_HLSLI

// ============================================================================
// Constants matching the official ACES 2.0 reference
// ============================================================================

static const int ACES2_TABLE_SIZE = 360;
static const int ACES2_BASE_INDEX = 1;
static const int ACES2_TOTAL_TABLE_SIZE = 362;  // tableSize + 2
static const float ACES2_HUE_LIMIT = 360.0;

// CAM parameters
static const float ACES2_REF_LUMINANCE = 100.0;
static const float ACES2_J_SCALE = 100.0;
static const float ACES2_CAM_NL_Y_REF = 100.0;
static const float ACES2_CAM_NL_OFFSET = 0.2713 * ACES2_CAM_NL_Y_REF;
static const float ACES2_CAM_NL_SCALE = 4.0 * ACES2_CAM_NL_Y_REF;

// Surround: Dim
static const float ACES2_SURROUND_C = 0.9;  // surround[2]

// model_gamma = surround[1] * (1.48 + sqrt(Y_b / ref_luminance))
// = 0.59 * (1.48 + sqrt(20/100)) = 0.59 * (1.48 + 0.4472136) = 0.59 * 1.9272136
static const float ACES2_MODEL_GAMMA = 1.137056;

// Chroma compression constants
static const float ACES2_CHROMA_COMPRESS = 2.4;
static const float ACES2_CHROMA_COMPRESS_FACT = 3.3;
static const float ACES2_CHROMA_EXPAND = 1.3;
static const float ACES2_CHROMA_EXPAND_FACT = 0.69;
static const float ACES2_CHROMA_EXPAND_THR = 0.5;

// Gamut compression constants
static const float ACES2_SMOOTH_CUSPS = 0.12;
static const float ACES2_SMOOTH_M = 0.27;
static const float ACES2_CUSP_MID_BLEND = 1.3;
static const float ACES2_FOCUS_GAIN_BLEND = 0.3;
static const float ACES2_FOCUS_ADJUST_GAIN = 0.55;
static const float ACES2_FOCUS_DISTANCE = 1.35;
static const float ACES2_FOCUS_DISTANCE_SCALING = 1.75;
static const float ACES2_COMPRESSION_THRESHOLD = 0.75;

static const float ACES2_PI = 3.14159265358979323846;

// ============================================================================
// GPU-side ODT parameter struct (matches CPU layout)
// ============================================================================

struct ACES2_TSParams
{
	float n;
	float n_r;
	float g;
	float t_1;
	float c_t;
	float s_2;
	float u_2;
	float m_2;
	float forward_limit;
	float inverse_limit;
	float log_peak;
	float _pad;
};

struct ACES2_JMhParams
{
	float3x3 MATRIX_RGB_to_CAM16_c;
	float3x3 MATRIX_CAM16_c_to_RGB;
	float3x3 MATRIX_cone_response_to_Aab;
	float3x3 MATRIX_Aab_to_cone_response;
	float F_L_n;
	float cz;
	float inv_cz;
	float A_w_J;
	float inv_A_w_J;
	float _pad0, _pad1, _pad2;
};

// Per-entry of the gamut cusp table: J, M, hue
struct ACES2_CuspEntry
{
	float J;
	float M;
	float h;
	float _pad;
};

// Main ODT params buffer (single element)
struct ACES2_ODTParamsGPU
{
	float peakLuminance;
	float limit_J_max;
	float model_gamma_inv;
	float mid_J;

	float focus_dist;
	float lower_hull_gamma_inv;
	float sat;
	float sat_thr;

	float compr;
	float chroma_compress_scale;
	int hue_linearity_search_range0;
	int hue_linearity_search_range1;

	ACES2_TSParams ts;
	ACES2_JMhParams input_params;
	ACES2_JMhParams reach_params;
	ACES2_JMhParams limit_params;
};

// Table data buffer - single flat buffer with all arrays packed
// Layout: [reach_M: 362] [hues: 362] [upper_hull_gamma: 362] [cusps_J: 362] [cusps_M: 362] [cusps_h: 362]
// Total: 362 * 6 = 2172 floats
StructuredBuffer<float> ACES2_TableData : register(t2);

// Access helpers for the flat table buffer
static const int ACES2_TBL_REACH_M = 0;
static const int ACES2_TBL_HUES = 362;
static const int ACES2_TBL_UPPER_GAMMA = 724;
static const int ACES2_TBL_CUSPS_J = 1086;
static const int ACES2_TBL_CUSPS_M = 1448;
static const int ACES2_TBL_CUSPS_H = 1810;

// ============================================================================
// Utility functions
// ============================================================================

float aces2_wrap_to_360(float hue)
{
	float y = fmod(hue, 360.0);
	if (y < 0.0)
		y += 360.0;
	return y;
}

int aces2_hue_position_in_uniform_table(float hue, int table_size)
{
	float wrapped_hue = aces2_wrap_to_360(hue);
	return (int)(wrapped_hue / ACES2_HUE_LIMIT * table_size);
}

float aces2_base_hue_for_position(int i_lo, int table_size)
{
	return (float)i_lo * ACES2_HUE_LIMIT / (float)table_size;
}

// ============================================================================
// CAM functions (post-adaptation cone response compression)
// ============================================================================

float aces2_pacrc_fwd_core(float Rc)
{
	float F_L_Y = pow(Rc, 0.42);
	return F_L_Y / (ACES2_CAM_NL_OFFSET + F_L_Y);
}

float aces2_pacrc_inv_core(float Ra)
{
	float Ra_lim = min(Ra, 0.99);
	float F_L_Y = (ACES2_CAM_NL_OFFSET * Ra_lim) / (1.0 - Ra_lim);
	return pow(F_L_Y, 1.0 / 0.42);
}

float aces2_pacrc_fwd(float v)
{
	return sign(v) * aces2_pacrc_fwd_core(abs(v));
}

float aces2_pacrc_inv(float v)
{
	return sign(v) * aces2_pacrc_inv_core(abs(v));
}

// ============================================================================
// J <-> Y conversions
// ============================================================================

float aces2_Achromatic_n_to_J(float A, float cz)
{
	return ACES2_J_SCALE * pow(A, cz);
}

float aces2_J_to_Achromatic_n(float J, float inv_cz)
{
	return pow(J / ACES2_J_SCALE, inv_cz);
}

float aces2_A_to_Y(float A, ACES2_JMhParams p)
{
	float Ra = p.A_w_J * A;
	float Y = aces2_pacrc_inv_core(Ra) / p.F_L_n;
	return Y;
}

float aces2_J_to_Y(float J, ACES2_JMhParams p)
{
	float abs_J = abs(J);
	return aces2_A_to_Y(aces2_J_to_Achromatic_n(abs_J, p.inv_cz), p);
}

float aces2_Y_to_J(float Y, ACES2_JMhParams p)
{
	float abs_Y = abs(Y);
	float Ra = aces2_pacrc_fwd_core(abs_Y * p.F_L_n);
	float J = aces2_Achromatic_n_to_J(Ra * p.inv_A_w_J, p.cz);
	return sign(Y) * J;
}

// ============================================================================
// RGB <-> JMh conversions
// ============================================================================

float3 aces2_RGB_to_Aab(float3 RGB, ACES2_JMhParams p)
{
	float3 rgb_m = mul(p.MATRIX_RGB_to_CAM16_c, RGB);

	float3 rgb_a = float3(
		aces2_pacrc_fwd(rgb_m.x),
		aces2_pacrc_fwd(rgb_m.y),
		aces2_pacrc_fwd(rgb_m.z));

	return mul(p.MATRIX_cone_response_to_Aab, rgb_a);
}

float3 aces2_Aab_to_JMh(float3 Aab, ACES2_JMhParams p)
{
	if (Aab.x <= 0.0)
		return float3(0, 0, 0);

	float J = aces2_Achromatic_n_to_J(Aab.x, p.cz);
	float M = sqrt(Aab.y * Aab.y + Aab.z * Aab.z);
	float h_rad = atan2(Aab.z, Aab.y);
	float h = aces2_wrap_to_360(h_rad * 180.0 / ACES2_PI);

	return float3(J, M, h);
}

float3 aces2_RGB_to_JMh(float3 RGB, ACES2_JMhParams p)
{
	float3 Aab = aces2_RGB_to_Aab(RGB, p);
	return aces2_Aab_to_JMh(Aab, p);
}

float3 aces2_JMh_to_Aab(float3 JMh, ACES2_JMhParams p)
{
	float h_rad = JMh.z * ACES2_PI / 180.0;
	float A = aces2_J_to_Achromatic_n(JMh.x, p.inv_cz);
	float a = JMh.y * cos(h_rad);
	float b = JMh.y * sin(h_rad);
	return float3(A, a, b);
}

float3 aces2_Aab_to_RGB(float3 Aab, ACES2_JMhParams p)
{
	float3 rgb_a = mul(p.MATRIX_Aab_to_cone_response, Aab);

	float3 rgb_m = float3(
		aces2_pacrc_inv(rgb_a.x),
		aces2_pacrc_inv(rgb_a.y),
		aces2_pacrc_inv(rgb_a.z));

	return mul(p.MATRIX_CAM16_c_to_RGB, rgb_m);
}

float3 aces2_JMh_to_RGB(float3 JMh, ACES2_JMhParams p)
{
	float3 Aab = aces2_JMh_to_Aab(JMh, p);
	return aces2_Aab_to_RGB(Aab, p);
}

// ============================================================================
// Tonescale
// ============================================================================

float aces2_tonescale_fwd(float x, ACES2_TSParams ts)
{
	float f = ts.m_2 * pow(max(0.0, x) / (x + ts.s_2), ts.g);
	float h = max(0.0, f * f / (f + ts.t_1));
	return h * ts.n_r;
}

// ============================================================================
// Table lookups
// ============================================================================

float aces2_reach_M_from_table(float h)
{
	int base = aces2_hue_position_in_uniform_table(h, ACES2_TABLE_SIZE);
	float t = h - (float)base;
	int i_lo = base + ACES2_BASE_INDEX;
	int i_hi = i_lo + 1;
	float lo = ACES2_TableData[ACES2_TBL_REACH_M + i_lo];
	float hi = ACES2_TableData[ACES2_TBL_REACH_M + i_hi];
	return lerp(lo, hi, t);
}

// ============================================================================
// Chroma compression
// ============================================================================

float aces2_chroma_compress_norm(float h, float scale)
{
	float hr = h * ACES2_PI / 180.0;
	float a = cos(hr);
	float b = sin(hr);
	float cos_hr2 = a * a - b * b;
	float sin_hr2 = 2.0 * a * b;
	float cos_hr3 = 4.0 * a * a * a - 3.0 * a;
	float sin_hr3 = 3.0 * b - 4.0 * b * b * b;

	float M_val = 11.34072 * a +
	              16.46899 * cos_hr2 +
	              7.88380 * cos_hr3 +
	              14.66441 * b +
	              -6.37224 * sin_hr2 +
	              9.19364 * sin_hr3 +
	              77.12896;

	return M_val * scale;
}

float aces2_reinhard_remap(float sc, float nd, bool inv)
{
	if (inv) {
		if (nd >= 1.0)
			return sc;
		return sc * -(nd / (nd - 1.0));
	}
	return sc * nd / (1.0 + nd);
}

float aces2_toe(float x, float lim, float k1_in, float k2_in, bool inv)
{
	if (x > lim)
		return x;
	float k2 = max(k2_in, 0.001);
	float k1 = sqrt(k1_in * k1_in + k2 * k2);
	float k3 = (lim + k1) / (lim + k2);
	if (inv) {
		return (x * x + k1 * x) / (k3 * (x + k2));
	} else {
		float minus_b = k3 * x - k1;
		float minus_c = k2 * k3 * x;
		return 0.5 * (minus_b + sqrt(minus_b * minus_b + 4.0 * minus_c));
	}
}

float3 aces2_chroma_compress_fwd(
	float3 JMh,
	float tonemapped_J,
	ACES2_ODTParamsGPU p)
{
	float J = JMh.x;
	float M = JMh.y;
	float h = JMh.z;

	float M_compr = M;

	if (M != 0.0) {
		float nJ = tonemapped_J / p.limit_J_max;
		float snJ = max(0.0, 1.0 - nJ);
		float Mnorm = aces2_chroma_compress_norm(h, p.chroma_compress_scale);
		float lim = pow(nJ, p.model_gamma_inv) * aces2_reach_M_from_table(h) / Mnorm;

		float toe_lim = lim - 0.001;
		float toe_snJ_sat = snJ * p.sat;
		float toe_sqrt_nJ = sqrt(nJ * nJ + p.sat_thr);
		float toe_nJ_compr = nJ * p.compr;

		M_compr = M * pow(tonemapped_J / J, p.model_gamma_inv);
		M_compr = M_compr / Mnorm;
		M_compr = lim - aces2_toe(lim - M_compr, toe_lim, toe_snJ_sat, toe_sqrt_nJ, false);
		M_compr = aces2_toe(M_compr, lim, toe_nJ_compr, snJ, false);
		M_compr = M_compr * Mnorm;
	}

	return float3(tonemapped_J, M_compr, h);
}

// ============================================================================
// Gamut compression
// ============================================================================

float aces2_smin_scaled(float a, float b, float scale_ref)
{
	float s_scaled = ACES2_SMOOTH_CUSPS * scale_ref;
	float hv = max(s_scaled - abs(a - b), 0.0) / s_scaled;
	return min(a, b) - hv * hv * hv * s_scaled * (1.0 / 6.0);
}

float aces2_solve_J_intersect(float J, float M, float focusJ, float maxJ, float slope_gain)
{
	float M_scaled = M / slope_gain;
	float a = M_scaled / focusJ;

	if (J < focusJ) {
		float b = 1.0 - M_scaled;
		float c = -J;
		float det = b * b - 4.0 * a * c;
		return -2.0 * c / (b + sqrt(det));
	} else {
		float b = -(1.0 + M_scaled + maxJ * a);
		float c = maxJ * M_scaled + J;
		float det = b * b - 4.0 * a * c;
		return -2.0 * c / (b - sqrt(det));
	}
}

float aces2_compute_slope(float intersect_J, float focus_J, float limit_J_max, float slope_gain)
{
	float dir;
	if (intersect_J < focus_J)
		dir = intersect_J;
	else
		dir = limit_J_max - intersect_J;
	return dir * (intersect_J - focus_J) / (focus_J * slope_gain);
}

float aces2_estimate_boundary_M(
	float J_intersect, float slope, float inv_gamma,
	float J_max, float M_max, float J_ref)
{
	float nJ = J_intersect / J_ref;
	float shifted = J_ref * pow(nJ, inv_gamma);
	return shifted * M_max / (J_max - slope * M_max);
}

float aces2_get_focus_gain(float J, float analytical_thr, float limit_J_max, float focus_dist)
{
	float gain = limit_J_max * focus_dist;
	if (J > analytical_thr) {
		float adj = log10((limit_J_max - analytical_thr) / max(0.0001, limit_J_max - J));
		adj = adj * adj + 1.0;
		gain = gain * adj;
	}
	return gain;
}

float aces2_find_gamut_boundary(
	float2 JMcusp, float J_max,
	float gamma_top_inv, float gamma_bottom_inv,
	float J_src, float slope, float J_cusp)
{
	float M_lo = aces2_estimate_boundary_M(J_src, slope, gamma_bottom_inv, JMcusp.x, JMcusp.y, J_cusp);

	float f_J_cusp = J_max - J_cusp;
	float f_J_src = J_max - J_src;
	float f_JM_J = J_max - JMcusp.x;
	float M_hi = aces2_estimate_boundary_M(f_J_src, -slope, gamma_top_inv, f_JM_J, JMcusp.y, f_J_cusp);

	return aces2_smin_scaled(M_lo, M_hi, JMcusp.y);
}

float aces2_remap_M(float M, float gamut_M, float reach_M, bool inv)
{
	float ratio = gamut_M / reach_M;
	float proportion = max(ratio, ACES2_COMPRESSION_THRESHOLD);
	float thr = proportion * gamut_M;

	if (M <= thr || proportion >= 1.0)
		return M;

	float m_off = M - thr;
	float g_off = gamut_M - thr;
	float r_off = reach_M - thr;

	float sc = r_off / ((r_off / g_off) - 1.0);
	float nd = m_off / sc;

	return thr + aces2_reinhard_remap(sc, nd, inv);
}

// Cusp lookup from table
float2 aces2_cusp_from_table(float h)
{
	// Binary search in the hue-sorted cusp table
	int low_i = 0;
	int high_i = ACES2_BASE_INDEX + ACES2_TABLE_SIZE;
	int i = aces2_hue_position_in_uniform_table(h, ACES2_TABLE_SIZE) + ACES2_BASE_INDEX;

	[loop] while (low_i + 1 < high_i)
	{
		float tbl_h = ACES2_TableData[ACES2_TBL_CUSPS_H + i];
		if (h > tbl_h)
			low_i = i;
		else
			high_i = i;
		i = (low_i + high_i) / 2;
	}

	float lo_J = ACES2_TableData[ACES2_TBL_CUSPS_J + high_i - 1];
	float lo_M = ACES2_TableData[ACES2_TBL_CUSPS_M + high_i - 1];
	float lo_h = ACES2_TableData[ACES2_TBL_CUSPS_H + high_i - 1];
	float hi_J = ACES2_TableData[ACES2_TBL_CUSPS_J + high_i];
	float hi_M = ACES2_TableData[ACES2_TBL_CUSPS_M + high_i];
	float hi_h = ACES2_TableData[ACES2_TBL_CUSPS_H + high_i];

	float t = (h - lo_h) / (hi_h - lo_h);
	return float2(lerp(lo_J, hi_J, t), lerp(lo_M, hi_M, t));
}

// Hue interval lookup
int aces2_lookup_hue_interval(float h, int search0, int search1)
{
	uint i = (uint)(ACES2_BASE_INDEX + aces2_hue_position_in_uniform_table(h, ACES2_TOTAL_TABLE_SIZE));
	uint i_lo = (uint)max(ACES2_BASE_INDEX, (int)i + search0);
	uint i_hi = (uint)min(ACES2_BASE_INDEX + ACES2_TABLE_SIZE, (int)i + search1);

	[loop] while (i_lo + 1 < i_hi)
	{
		float tbl_h = ACES2_TableData[ACES2_TBL_HUES + i];
		if (h > tbl_h)
			i_lo = i;
		else
			i_hi = i;
		i = (i_lo + i_hi) / 2;
	}
	return max(1, (int)i_hi);
}

float aces2_upper_hull_gamma_at_hue(float h, int search0, int search1)
{
	int i_lo = aces2_hue_position_in_uniform_table(h, ACES2_TABLE_SIZE) + ACES2_BASE_INDEX;
	int i_hi = (i_lo + 1) % ACES2_TOTAL_TABLE_SIZE;

	float base_hue = aces2_base_hue_for_position(i_lo - ACES2_BASE_INDEX, ACES2_TABLE_SIZE);
	float t = aces2_wrap_to_360(h) - base_hue;

	float g_lo = ACES2_TableData[ACES2_TBL_UPPER_GAMMA + i_lo];
	float g_hi = ACES2_TableData[ACES2_TBL_UPPER_GAMMA + i_hi];
	return lerp(g_lo, g_hi, t);
}

float aces2_compute_focus_J(float cusp_J, float mid_J, float limit_J_max)
{
	return lerp(cusp_J, mid_J, min(1.0, ACES2_CUSP_MID_BLEND - (cusp_J / limit_J_max)));
}

float3 aces2_gamut_compress_fwd(float3 JMh, ACES2_ODTParamsGPU p)
{
	float J = JMh.x;
	float M = JMh.y;
	float h = JMh.z;

	if (J <= 0.0)
		return float3(0, 0, h);

	if (M < 0.0 || J > p.limit_J_max)
		return float3(J, 0, h);

	// Hue-dependent gamut params
	float2 JMcusp = aces2_cusp_from_table(h);
	float gamma_top_inv = aces2_upper_hull_gamma_at_hue(h, p.hue_linearity_search_range0, p.hue_linearity_search_range1);
	float focus_J = aces2_compute_focus_J(JMcusp.x, p.mid_J, p.limit_J_max);
	float analytical_thr = lerp(JMcusp.x, p.limit_J_max, ACES2_FOCUS_GAIN_BLEND);

	// Compress gamut
	float slope_gain = aces2_get_focus_gain(J, analytical_thr, p.limit_J_max, p.focus_dist);
	float J_src = aces2_solve_J_intersect(J, M, focus_J, p.limit_J_max, slope_gain);
	float gamut_slope = aces2_compute_slope(J_src, focus_J, p.limit_J_max, slope_gain);
	float J_cusp = aces2_solve_J_intersect(JMcusp.x, JMcusp.y, focus_J, p.limit_J_max, slope_gain);

	float gamut_M = aces2_find_gamut_boundary(JMcusp, p.limit_J_max, gamma_top_inv, p.lower_hull_gamma_inv, J_src, gamut_slope, J_cusp);

	if (gamut_M <= 0.0)
		return float3(J, 0, h);

	float reach_max_M = aces2_reach_M_from_table(h);
	float reach_M = aces2_estimate_boundary_M(J_src, gamut_slope, p.model_gamma_inv, p.limit_J_max, reach_max_M, p.limit_J_max);

	float remapped_M = aces2_remap_M(M, gamut_M, reach_M, false);

	return float3(J_src + remapped_M * gamut_slope, remapped_M, h);
}

// ============================================================================
// Complete Output Transform
// ============================================================================

// AP0 to AP1 matrix (ACES2065-1 to ACEScg)
static const float3x3 ACES2_AP0_TO_AP1 = float3x3(
	1.45143931614567, -0.23651074689374, -0.21492856925194,
	-0.07655377339602, 1.17622969983357, -0.09967592643754,
	0.00831614842569, -0.00603244979765, 0.99771630137196);

float3 aces2_outputTransform_fwd(float3 aces, ACES2_ODTParamsGPU p)
{
	// Clamp AP0 to AP1 range
	float3 AP1 = mul(ACES2_AP0_TO_AP1, aces);
	AP1 = clamp(AP1, 0.0, p.ts.forward_limit);
	// Convert back to AP0
	static const float3x3 AP1_TO_AP0 = float3x3(
		0.69545224933912, 0.14067870272055, 0.16386904794033,
		0.04479456365552, 0.85967111794498, 0.09553431839950,
		-0.00552588256522, 0.00402521030598, 1.00150067225924);
	float3 AP0_clamped = mul(AP1_TO_AP0, AP1);

	// RGB -> JMh (input/rendering space)
	float3 JMh = aces2_RGB_to_JMh(AP0_clamped, p.input_params);

	// Tonemap
	float linear_val = aces2_J_to_Y(JMh.x, p.input_params) / ACES2_REF_LUMINANCE;
	float tonemapped_Y = aces2_tonescale_fwd(linear_val, p.ts);
	float J_ts = aces2_Y_to_J(tonemapped_Y, p.input_params);

	// Chroma compress
	float3 JMh_tc = aces2_chroma_compress_fwd(JMh, J_ts, p);

	// Gamut compress
	float3 JMh_gc = aces2_gamut_compress_fwd(JMh_tc, p);

	// JMh -> RGB (limiting/display space)
	float3 RGBout = aces2_JMh_to_RGB(JMh_gc, p.limit_params);

	return RGBout;
}

// ============================================================================
// Entry point wrapper called by the tonemapper system
// ============================================================================

// sRGB -> AP0 matrix (pre-computed)
static const float3x3 ACES2_SRGB_TO_AP0 = float3x3(
	0.4397010, 0.3829780, 0.1773350,
	0.0897923, 0.8134230, 0.0967616,
	0.0175440, 0.1115440, 0.8707040);

float3 ACES2Tonemap(float3 color)
{
	color *= tonemapParams[0].x;  // exposure

	// Convert from sRGB/BT.709 linear to ACES AP0 (ACES2065-1)
	float3 aces = mul(ACES2_SRGB_TO_AP0, color);

	// Load ODT params from cbuffer slot b2
	// (The structured buffer at t2 contains the LUT tables)
	// ODT params are passed through the ACES2_Params cbuffer

	// Run the full ACES 2.0 Output Transform
	float3 result = aces2_outputTransform_fwd(aces, aces2Params);

	// Output is in limiting primaries (sRGB for SDR, BT2020 for HDR)
	// normalized to [0, peakLuminance/100]
	float peakScale = aces2Params.peakLuminance / ACES2_REF_LUMINANCE;
	result = clamp(result, 0.0, peakScale);

	// For SDR: normalize to [0,1]
	// For HDR: keep as-is (HDR pipeline handles the rest)
	if (!enableHDR)
		result = saturate(result);

	return result;
}

#endif  // ACES2_HLSLI
