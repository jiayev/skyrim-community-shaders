#pragma once
// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
//
// CPU-side ACES 2.0 pre-computation and GPU data structures.
// Translated from aces-core CTL reference.

#include "ACES2Constants.h"

namespace ACES2
{

	// ============================================================
	// 3x3 matrix math (double precision for CPU pre-computation)
	// ============================================================
	using Mat3 = std::array<std::array<double, 3>, 3>;
	using Vec3 = std::array<double, 3>;

	inline Mat3 identity3()
	{
		return { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
	}

	inline Vec3 mul_v3_m33(Vec3 v, Mat3 m)
	{
		return { v[0] * m[0][0] + v[1] * m[1][0] + v[2] * m[2][0],
			v[0] * m[0][1] + v[1] * m[1][1] + v[2] * m[2][1],
			v[0] * m[0][2] + v[1] * m[1][2] + v[2] * m[2][2] };
	}

	inline Mat3 mul_m33(Mat3 a, Mat3 b)
	{
		Mat3 r{};
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < 3; k++)
					r[i][j] += a[i][k] * b[k][j];
		return r;
	}

	inline Mat3 scale_diagonal(Mat3 m, Vec3 s)
	{
		Mat3 r = m;
		r[0][0] *= s[0];
		r[1][1] *= s[1];
		r[2][2] *= s[2];
		return r;
	}

	inline Mat3 scale_mat(double s, Mat3 m)
	{
		Mat3 r = m;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				r[i][j] *= s;
		return r;
	}

	Mat3 invert3(Mat3 m);
	Mat3 RGBtoXYZ(Chromaticities C, double Y);
	Mat3 XYZtoRGB(Chromaticities C, double Y);

	// ============================================================
	// Tonescale parameters
	// ============================================================
	struct TSParams
	{
		double n, n_r, g, t_1, c_t;
		double s_2, u_2, m_2;
		double forward_limit, inverse_limit, log_peak;
	};

	TSParams init_TSParams(double peakLuminance);

	// ============================================================
	// JMh parameters (CAM16-based appearance model)
	// ============================================================
	struct JMhParams
	{
		Mat3 MATRIX_RGB_to_CAM16_c;
		Mat3 MATRIX_CAM16_c_to_RGB;
		Mat3 MATRIX_cone_response_to_Aab;
		Mat3 MATRIX_Aab_to_cone_response;
		double F_L_n, cz, inv_cz, A_w_J, inv_A_w_J;
	};

	JMhParams init_JMhParams(Chromaticities prims);

	// ============================================================
	// ODT parameters (complete pre-computed data)
	// ============================================================
	struct ODTParams
	{
		double peakLuminance;
		JMhParams input_params;
		JMhParams reach_params;
		JMhParams limit_params;
		TSParams ts;

		double limit_J_max;
		double model_gamma_inv;
		std::array<double, TOTAL_TABLE_SIZE> TABLE_reach_M;

		double sat, sat_thr, compr, chroma_compress_scale;

		double mid_J, focus_dist, lower_hull_gamma_inv;
		std::array<double, TOTAL_TABLE_SIZE> TABLE_hues;
		std::array<std::array<double, 3>, TOTAL_TABLE_SIZE> TABLE_gamut_cusps;
		std::array<double, TOTAL_TABLE_SIZE> TABLE_upper_hull_gamma;
		int hue_linearity_search_range[2];
	};

	ODTParams init_ODTParams(double peakLuminance, Chromaticities limitingPri);

	// ============================================================
	// GPU data structures (must match HLSL layout exactly)
	// ============================================================

	struct alignas(16) GPU_TSParams
	{
		float n, n_r, g, t_1;
		float c_t, s_2, u_2, m_2;
		float forward_limit, inverse_limit, log_peak;
		float _pad;
	};

	// HLSL float3x3 is stored as 3 float4 rows (each row padded to 16 bytes)
	struct alignas(16) GPU_JMhParams
	{
		float MATRIX_RGB_to_CAM16_c[12];  // 3 rows x 4 floats (padded)
		float MATRIX_CAM16_c_to_RGB[12];
		float MATRIX_cone_response_to_Aab[12];
		float MATRIX_Aab_to_cone_response[12];
		float F_L_n, cz, inv_cz, A_w_J;
		float inv_A_w_J;
		float _pad0, _pad1, _pad2;
	};

	struct alignas(16) GPU_ODTParams
	{
		float peakLuminance;
		float limit_J_max;
		float model_gamma_inv;
		float mid_J;

		float focus_dist;
		float lower_hull_gamma_inv;
		float sat, sat_thr;

		float compr;
		float chroma_compress_scale;
		int hue_linearity_search_range0;
		int hue_linearity_search_range1;

		GPU_TSParams ts;
		GPU_JMhParams input_params;
		GPU_JMhParams reach_params;
		GPU_JMhParams limit_params;
	};

	// Total table data: 362 * 6 = 2172 floats
	static constexpr int TABLE_DATA_COUNT = TOTAL_TABLE_SIZE * 6;

	// Convert CPU ODTParams to GPU structs
	GPU_ODTParams buildGPUParams(const ODTParams& p);
	void buildGPUTableData(const ODTParams& p, std::array<float, TABLE_DATA_COUNT>& out);

}  // namespace ACES2
