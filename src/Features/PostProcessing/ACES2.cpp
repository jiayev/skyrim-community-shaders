// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
//
// CPU-side ACES 2.0 pre-computation.
// Faithfully translated from aces-core CTL reference.

#include "ACES2.h"

namespace ACES2
{

	// ============================================================
	// Matrix math
	// ============================================================

	Mat3 invert3(Mat3 m)
	{
		double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
		             m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		double inv_det = 1.0 / det;
		Mat3 r;
		r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv_det;
		r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv_det;
		r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv_det;
		r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv_det;
		r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv_det;
		r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv_det;
		r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv_det;
		r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv_det;
		r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv_det;
		return r;
	}

	Mat3 RGBtoXYZ(Chromaticities C, double Y)
	{
		double X = C.white[0] * Y / C.white[1];
		double Z = (1.0 - C.white[0] - C.white[1]) * Y / C.white[1];

		double d = C.red[0] * (C.blue[1] - C.green[1]) +
		           C.blue[0] * (C.green[1] - C.red[1]) +
		           C.green[0] * (C.red[1] - C.blue[1]);

		double Sr = (X * (C.blue[1] - C.green[1]) -
						C.green[0] * (Y * (C.blue[1] - 1) + C.blue[1] * (X + Z)) +
						C.blue[0] * (Y * (C.green[1] - 1) + C.green[1] * (X + Z))) /
		            d;
		double Sg = (X * (C.red[1] - C.blue[1]) +
						C.red[0] * (Y * (C.blue[1] - 1) + C.blue[1] * (X + Z)) -
						C.blue[0] * (Y * (C.red[1] - 1) + C.red[1] * (X + Z))) /
		            d;
		double Sb = (X * (C.green[1] - C.red[1]) -
						C.red[0] * (Y * (C.green[1] - 1) + C.green[1] * (X + Z)) +
						C.green[0] * (Y * (C.red[1] - 1) + C.red[1] * (X + Z))) /
		            d;

		Mat3 M;
		M[0][0] = Sr * C.red[0];
		M[0][1] = Sr * C.red[1];
		M[0][2] = Sr * (1.0 - C.red[0] - C.red[1]);
		M[1][0] = Sg * C.green[0];
		M[1][1] = Sg * C.green[1];
		M[1][2] = Sg * (1.0 - C.green[0] - C.green[1]);
		M[2][0] = Sb * C.blue[0];
		M[2][1] = Sb * C.blue[1];
		M[2][2] = Sb * (1.0 - C.blue[0] - C.blue[1]);
		return M;
	}

	Mat3 XYZtoRGB(Chromaticities C, double Y)
	{
		return invert3(RGBtoXYZ(C, Y));
	}

	// ============================================================
	// Tonescale
	// ============================================================

	TSParams init_TSParams(double peakLuminance)
	{
		const double n = peakLuminance;
		const double n_r = 100.0;
		const double g = 1.15;
		const double c = 0.18;
		const double c_d = 10.013;
		const double w_g = 0.14;
		const double t_1 = 0.04;
		const double r_hit_min = 128.0;
		const double r_hit_max = 896.0;

		double r_hit = r_hit_min + (r_hit_max - r_hit_min) * (std::log(n / n_r) / std::log(10000.0 / 100.0));
		double m_0 = n / n_r;
		double m_1 = 0.5 * (m_0 + std::sqrt(m_0 * (m_0 + 4.0 * t_1)));
		double u = std::pow((r_hit / m_1) / ((r_hit / m_1) + 1.0), g);
		double m = m_1 / u;
		double w_i = std::log(n / 100.0) / std::log(2.0);
		double c_t = c_d / n_r * (1.0 + w_i * w_g);
		double g_ip = 0.5 * (c_t + std::sqrt(c_t * (c_t + 4.0 * t_1)));
		double g_ipp2 = -(m_1 * std::pow((g_ip / m), (1.0 / g))) / (std::pow(g_ip / m, 1.0 / g) - 1.0);
		double w_2 = c / g_ipp2;
		double s_2 = w_2 * m_1;
		double u_2 = std::pow((r_hit / m_1) / ((r_hit / m_1) + w_2), g);
		double m_2 = m_1 / u_2;

		TSParams p;
		p.n = n;
		p.n_r = n_r;
		p.g = g;
		p.t_1 = t_1;
		p.c_t = c_t;
		p.s_2 = s_2;
		p.u_2 = u_2;
		p.m_2 = m_2;
		p.forward_limit = 8.0 * r_hit;
		p.inverse_limit = n / (u_2 * n_r);
		p.log_peak = std::log10(n / n_r);
		return p;
	}

	// ============================================================
	// CAM functions
	// ============================================================

	static double pacrc_fwd(double v)
	{
		double av = std::abs(v);
		double F_L_Y = std::pow(av, 0.42);
		double Ra = F_L_Y / (CAM_NL_OFFSET + F_L_Y);
		return std::copysign(Ra, v);
	}

	static double Achromatic_n_to_J(double A, double cz)
	{
		return J_SCALE * std::pow(A, cz);
	}

	static double J_to_Achromatic_n(double J, double inv_cz)
	{
		return std::pow(J / J_SCALE, inv_cz);
	}

	static double pacrc_fwd_core(double Rc)
	{
		double F_L_Y = std::pow(Rc, 0.42);
		return F_L_Y / (CAM_NL_OFFSET + F_L_Y);
	}

	static double pacrc_inv_core(double Ra)
	{
		double Ra_lim = std::min(Ra, 0.99);
		double F_L_Y = (CAM_NL_OFFSET * Ra_lim) / (1.0 - Ra_lim);
		return std::pow(F_L_Y, 1.0 / 0.42);
	}

	static double pacrc_inv(double v)
	{
		double av = std::abs(v);
		double Rc = pacrc_inv_core(av);
		return std::copysign(Rc, v);
	}

	static double Y_to_J(double Y, const JMhParams& p)
	{
		double av = std::abs(Y);
		double Ra = pacrc_fwd_core(av * p.F_L_n);
		double J = Achromatic_n_to_J(Ra * p.inv_A_w_J, p.cz);
		return std::copysign(J, Y);
	}

	static Vec3 RGB_to_Aab(Vec3 RGB, const JMhParams& p)
	{
		Vec3 rgb_m = mul_v3_m33(RGB, p.MATRIX_RGB_to_CAM16_c);
		Vec3 rgb_a = { pacrc_fwd(rgb_m[0]), pacrc_fwd(rgb_m[1]), pacrc_fwd(rgb_m[2]) };
		return mul_v3_m33(rgb_a, p.MATRIX_cone_response_to_Aab);
	}

	static double wrap_to_360(double hue)
	{
		double y = std::fmod(hue, 360.0);
		if (y < 0.0)
			y += 360.0;
		return y;
	}

	static Vec3 Aab_to_JMh(Vec3 Aab, const JMhParams& p)
	{
		if (Aab[0] <= 0.0)
			return { 0, 0, 0 };
		double J = Achromatic_n_to_J(Aab[0], p.cz);
		double M = std::sqrt(Aab[1] * Aab[1] + Aab[2] * Aab[2]);
		double h_rad = std::atan2(Aab[2], Aab[1]);
		double h = wrap_to_360(h_rad * 180.0 / PI);
		return { J, M, h };
	}

	static Vec3 RGB_to_JMh(Vec3 RGB, const JMhParams& p)
	{
		Vec3 Aab = RGB_to_Aab(RGB, p);
		return Aab_to_JMh(Aab, p);
	}

	static Vec3 JMh_to_Aab(Vec3 JMh, const JMhParams& p)
	{
		double h_rad = JMh[2] * PI / 180.0;
		double A = J_to_Achromatic_n(JMh[0], p.inv_cz);
		double a = JMh[1] * std::cos(h_rad);
		double b = JMh[1] * std::sin(h_rad);
		return { A, a, b };
	}

	static Vec3 Aab_to_RGB(Vec3 Aab, const JMhParams& p)
	{
		Vec3 rgb_a = mul_v3_m33(Aab, p.MATRIX_Aab_to_cone_response);
		Vec3 rgb_m = { pacrc_inv(rgb_a[0]), pacrc_inv(rgb_a[1]), pacrc_inv(rgb_a[2]) };
		return mul_v3_m33(rgb_m, p.MATRIX_CAM16_c_to_RGB);
	}

	static Vec3 JMh_to_RGB(Vec3 JMh, const JMhParams& p)
	{
		Vec3 Aab = JMh_to_Aab(JMh, p);
		return Aab_to_RGB(Aab, p);
	}

	// ============================================================
	// init_JMhParams - from Lib.Academy.OutputTransform.ctl
	// ============================================================

	JMhParams init_JMhParams(Chromaticities prims)
	{
		Chromaticities CAM16_PRI = {
			{ 0.8336, 0.1735 },
			{ 2.3854, -1.4659 },
			{ 0.087, -0.125 },
			{ 0.333, 0.333 }
		};

		Mat3 MATRIX_16 = XYZtoRGB(CAM16_PRI, 1.0);

		Mat3 base_cr_to_Aab = { { { 2., 1., 1. / 9. },
			{ 1., -12. / 11., 1. / 9. },
			{ 1. / 20., 1. / 11., -2. / 9. } } };

		Mat3 RGB_TO_XYZ = RGBtoXYZ(prims, 1.0);
		Vec3 f3_ref = { REF_LUMINANCE, REF_LUMINANCE, REF_LUMINANCE };
		Vec3 XYZ_w = mul_v3_m33(f3_ref, RGB_TO_XYZ);
		double Y_w = XYZ_w[1];

		Vec3 RGB_w = mul_v3_m33(XYZ_w, MATRIX_16);

		double k = 1.0 / (5.0 * L_A + 1.0);
		double k4 = k * k * k * k;
		double F_L = 0.2 * k4 * (5.0 * L_A) +
		             0.1 * std::pow(1.0 - k4, 2.0) * std::pow(5.0 * L_A, 1.0 / 3.0);

		double F_L_n = F_L / REF_LUMINANCE;
		double cz = MODEL_GAMMA;

		Vec3 D_RGB = {
			F_L_n * Y_w / RGB_w[0],
			F_L_n * Y_w / RGB_w[1],
			F_L_n * Y_w / RGB_w[2]
		};

		Vec3 RGB_wc = {
			D_RGB[0] * RGB_w[0],
			D_RGB[1] * RGB_w[1],
			D_RGB[2] * RGB_w[2]
		};

		Vec3 RGB_Aw = {
			pacrc_fwd(RGB_wc[0]),
			pacrc_fwd(RGB_wc[1]),
			pacrc_fwd(RGB_wc[2])
		};

		Mat3 cr_to_Aab = scale_mat(CAM_NL_SCALE, base_cr_to_Aab);

		double A_w = cr_to_Aab[0][0] * RGB_Aw[0] +
		             cr_to_Aab[1][0] * RGB_Aw[1] +
		             cr_to_Aab[2][0] * RGB_Aw[2];

		double A_w_J = pacrc_fwd_core(F_L);

		Mat3 M1 = mul_m33(RGB_TO_XYZ, MATRIX_16);
		Mat3 M2 = scale_mat(REF_LUMINANCE, identity3());
		Mat3 MATRIX_RGB_to_CAM16 = mul_m33(M1, M2);
		Mat3 MATRIX_RGB_to_CAM16_c = mul_m33(
			MATRIX_RGB_to_CAM16,
			scale_diagonal(identity3(), D_RGB));

		Mat3 cone_response_to_Aab;
		for (int i = 0; i < 3; i++) {
			cone_response_to_Aab[i][0] = cr_to_Aab[i][0] / A_w;
			cone_response_to_Aab[i][1] = cr_to_Aab[i][1] * 43.0 * SURROUND_C;
			cone_response_to_Aab[i][2] = cr_to_Aab[i][2] * 43.0 * SURROUND_C;
		}

		JMhParams p;
		p.MATRIX_RGB_to_CAM16_c = MATRIX_RGB_to_CAM16_c;
		p.MATRIX_CAM16_c_to_RGB = invert3(MATRIX_RGB_to_CAM16_c);
		p.MATRIX_cone_response_to_Aab = cone_response_to_Aab;
		p.MATRIX_Aab_to_cone_response = invert3(cone_response_to_Aab);
		p.F_L_n = F_L_n;
		p.cz = cz;
		p.inv_cz = 1.0 / cz;
		p.A_w_J = A_w_J;
		p.inv_A_w_J = 1.0 / A_w_J;
		return p;
	}

	// ============================================================
	// Table-building helpers
	// ============================================================

	static int hue_position(double hue, int table_size)
	{
		double wh = wrap_to_360(hue);
		return (int)(wh / HUE_LIMIT * table_size);
	}

	static double base_hue_for_pos(int i, int table_size)
	{
		return (double)i * HUE_LIMIT / (double)table_size;
	}

	static Vec3 generate_unit_cube_corner(int corner)
	{
		Vec3 r;
		r[0] = (((corner + 1) % CUSP_CORNER_COUNT) < 3) ? 1.0 : 0.0;
		r[1] = (((corner + 5) % CUSP_CORNER_COUNT) < 3) ? 1.0 : 0.0;
		r[2] = (((corner + 3) % CUSP_CORNER_COUNT) < 3) ? 1.0 : 0.0;
		return r;
	}

	static bool any_below_zero(Vec3 v) { return v[0] < 0.0 || v[1] < 0.0 || v[2] < 0.0; }
	static bool outside_hull(Vec3 v, double mx) { return v[0] > mx || v[1] > mx || v[2] > mx; }

	// make_reach_m_table: find reach gamut M at limitJmax for each hue
	static std::array<double, TOTAL_TABLE_SIZE> make_reach_m_table(const JMhParams& params, double limitJmax)
	{
		std::array<double, TOTAL_TABLE_SIZE> tbl{};
		for (int i = 0; i < TABLE_SIZE; i++) {
			double hue = base_hue_for_pos(i, TABLE_SIZE);
			double low = 0, high = low + 50.0;
			bool outside = false;
			while (!outside && high < 1300.0) {
				Vec3 jmh = { limitJmax, high, hue };
				Vec3 rgb = JMh_to_RGB(jmh, params);
				outside = any_below_zero(rgb);
				if (!outside) {
					low = high;
					high += 50.0;
				}
			}
			while (high - low > 1e-2) {
				double mid = (high + low) / 2.0;
				Vec3 jmh = { limitJmax, mid, hue };
				Vec3 rgb = JMh_to_RGB(jmh, params);
				if (any_below_zero(rgb))
					high = mid;
				else
					low = mid;
			}
			tbl[i + BASE_INDEX] = high;
		}
		tbl[0] = tbl[TABLE_SIZE];
		tbl[BASE_INDEX + TABLE_SIZE] = tbl[BASE_INDEX];
		return tbl;
	}

	// Build cusp corners for limiting gamut
	struct CornerTables
	{
		std::array<Vec3, TOTAL_CORNER_COUNT> RGB;
		std::array<Vec3, TOTAL_CORNER_COUNT> JMh;
	};

	static CornerTables build_limiting_corners(const JMhParams& params, double peakLuminance)
	{
		std::array<Vec3, CUSP_CORNER_COUNT> tmpRGB, tmpJMh;
		int min_idx = 0;
		for (int i = 0; i < CUSP_CORNER_COUNT; i++) {
			Vec3 ucc = generate_unit_cube_corner(i);
			tmpRGB[i] = { ucc[0] * peakLuminance / REF_LUMINANCE, ucc[1] * peakLuminance / REF_LUMINANCE, ucc[2] * peakLuminance / REF_LUMINANCE };
			tmpJMh[i] = RGB_to_JMh(tmpRGB[i], params);
			if (tmpJMh[i][2] < tmpJMh[min_idx][2])
				min_idx = i;
		}
		CornerTables ct;
		for (int i = 0; i < CUSP_CORNER_COUNT; i++) {
			ct.RGB[i + 1] = tmpRGB[(i + min_idx) % CUSP_CORNER_COUNT];
			ct.JMh[i + 1] = tmpJMh[(i + min_idx) % CUSP_CORNER_COUNT];
		}
		ct.RGB[0] = ct.RGB[CUSP_CORNER_COUNT];
		ct.RGB[CUSP_CORNER_COUNT + 1] = ct.RGB[1];
		ct.JMh[0] = ct.JMh[CUSP_CORNER_COUNT];
		ct.JMh[CUSP_CORNER_COUNT + 1] = ct.JMh[1];
		ct.JMh[0][2] -= HUE_LIMIT;
		ct.JMh[CUSP_CORNER_COUNT + 1][2] += HUE_LIMIT;
		return ct;
	}

	static std::array<Vec3, TOTAL_CORNER_COUNT> find_reach_corners(const JMhParams& params, const ODTParams& p)
	{
		std::array<Vec3, CUSP_CORNER_COUNT> tmp;
		double limitA = J_to_Achromatic_n(p.limit_J_max, params.inv_cz);
		int min_idx = 0;
		for (int i = 0; i < CUSP_CORNER_COUNT; i++) {
			Vec3 rv = generate_unit_cube_corner(i);
			double lower = 0, upper = p.ts.forward_limit;
			while ((upper - lower) > REACH_CUSP_TOLERANCE) {
				double test = (lower + upper) / 2.0;
				Vec3 tc = { rv[0] * test, rv[1] * test, rv[2] * test };
				double A = RGB_to_Aab(tc, params)[0];
				if (A < limitA)
					lower = test;
				else
					upper = test;
			}
			Vec3 corner_rgb = { rv[0] * upper, rv[1] * upper, rv[2] * upper };
			tmp[i] = RGB_to_JMh(corner_rgb, params);
			if (tmp[i][2] < tmp[min_idx][2])
				min_idx = i;
		}
		std::array<Vec3, TOTAL_CORNER_COUNT> out;
		for (int i = 0; i < CUSP_CORNER_COUNT; i++)
			out[i + 1] = tmp[(i + min_idx) % CUSP_CORNER_COUNT];
		out[0] = out[CUSP_CORNER_COUNT];
		out[CUSP_CORNER_COUNT + 1] = out[1];
		out[0][2] -= HUE_LIMIT;
		out[CUSP_CORNER_COUNT + 1][2] += HUE_LIMIT;
		return out;
	}

	static std::array<double, MAX_SORTED_CORNERS> extract_sorted_hues(
		const std::array<Vec3, TOTAL_CORNER_COUNT>& reach,
		const std::array<Vec3, TOTAL_CORNER_COUNT>& limit)
	{
		std::array<double, MAX_SORTED_CORNERS> sorted{};
		int idx = 0, ri = 1, li = 1;
		while (ri < CUSP_CORNER_COUNT + 1 || li < CUSP_CORNER_COUNT + 1) {
			double rh = (ri < CUSP_CORNER_COUNT + 1) ? reach[ri][2] : 1e10;
			double lh = (li < CUSP_CORNER_COUNT + 1) ? limit[li][2] : 1e10;
			if (std::abs(rh - lh) < 1e-10) {
				sorted[idx++] = rh;
				ri++;
				li++;
			} else if (rh < lh) {
				sorted[idx++] = rh;
				ri++;
			} else {
				sorted[idx++] = lh;
				li++;
			}
		}
		return sorted;
	}

	// Build hue table with non-uniform spacing around cusp corners
	static std::array<double, TOTAL_TABLE_SIZE> build_hue_table(const std::array<double, MAX_SORTED_CORNERS>& sorted)
	{
		std::array<double, TOTAL_TABLE_SIZE> ht{};
		double ideal_spacing = (double)TABLE_SIZE / HUE_LIMIT;
		std::array<int, MAX_SORTED_CORNERS + 2> sc{};
		int min_index = (sorted[0] == 0.0) ? 0 : 1;

		for (int hi = 0; hi < MAX_SORTED_CORNERS; hi++) {
			int nom = std::clamp((int)std::round(sorted[hi] * ideal_spacing), min_index, TABLE_SIZE - 1);
			if (hi > 0 && sc[hi - 1] == nom) {
				if (hi > 1 && sc[hi - 2] != sc[hi - 1] - 1)
					sc[hi - 1]--;
				else
					nom = std::min(nom + 1, TABLE_SIZE - 1);
			}
			sc[hi] = nom;
			min_index = nom;
		}

		int total = 0;
		// First interval
		{
			int samples = sc[0];
			double lower = 0.0, upper = sorted[0];
			for (int j = 0; j < samples; j++)
				ht[total + 1 + j] = lower + j * (upper - lower) / samples;
			total += samples;
		}
		for (int i = 1; i < MAX_SORTED_CORNERS; i++) {
			int samples = sc[i] - sc[i - 1];
			double lower = sorted[i - 1], upper = sorted[i];
			if (samples > 0)
				for (int j = 0; j < samples; j++)
					ht[total + 1 + j] = lower + j * (upper - lower) / samples;
			total += samples;
		}
		// Last interval
		{
			int samples = TABLE_SIZE - total;
			double lower = sorted[MAX_SORTED_CORNERS - 1], upper = HUE_LIMIT;
			for (int j = 0; j < samples; j++)
				ht[total + 1 + j] = lower + j * (upper - lower) / samples;
		}

		ht[0] = ht[BASE_INDEX + TABLE_SIZE - 1] - HUE_LIMIT;
		ht[BASE_INDEX + TABLE_SIZE] = ht[BASE_INDEX] + HUE_LIMIT;
		return ht;
	}

	// Find display cusp for a given hue by binary search
	static std::array<double, 2> find_display_cusp(double hue,
		const std::array<Vec3, TOTAL_CORNER_COUNT>& RGB_corners,
		const std::array<Vec3, TOTAL_CORNER_COUNT>& JMh_corners,
		const JMhParams& params)
	{
		int upper = 1;
		for (int i = 1; i < TOTAL_CORNER_COUNT; i++)
			if (JMh_corners[i][2] > hue) {
				upper = i;
				break;
			}
		int lower = upper - 1;

		if (std::abs(JMh_corners[lower][2] - hue) < 1e-12)
			return { JMh_corners[lower][0], JMh_corners[lower][1] };

		Vec3 cl = RGB_corners[lower], cu = RGB_corners[upper];
		double lt = 0, ut = 1;
		Vec3 jmh;
		while ((ut - lt) > DISPLAY_CUSP_TOLERANCE) {
			double st = (lt + ut) / 2.0;
			Vec3 sample = { cl[0] + st * (cu[0] - cl[0]), cl[1] + st * (cu[1] - cl[1]), cl[2] + st * (cu[2] - cl[2]) };
			jmh = RGB_to_JMh(sample, params);
			if (jmh[2] < JMh_corners[lower][2])
				ut = st;
			else if (jmh[2] >= JMh_corners[upper][2])
				lt = st;
			else if (jmh[2] > hue)
				ut = st;
			else
				lt = st;
		}
		double st = (lt + ut) / 2.0;
		Vec3 sample = { cl[0] + st * (cu[0] - cl[0]), cl[1] + st * (cu[1] - cl[1]), cl[2] + st * (cu[2] - cl[2]) };
		jmh = RGB_to_JMh(sample, params);
		return { jmh[0], jmh[1] };
	}

	// Build cusp table
	static std::array<std::array<double, 3>, TOTAL_TABLE_SIZE> build_cusp_table(
		const std::array<double, TOTAL_TABLE_SIZE>& hue_table,
		const std::array<Vec3, TOTAL_CORNER_COUNT>& RGB_corners,
		const std::array<Vec3, TOTAL_CORNER_COUNT>& JMh_corners,
		const JMhParams& params)
	{
		std::array<std::array<double, 3>, TOTAL_TABLE_SIZE> out{};
		for (int i = BASE_INDEX; i < TOTAL_TABLE_SIZE; i++) {
			auto jm = find_display_cusp(hue_table[i], RGB_corners, JMh_corners, params);
			out[i] = { jm[0], jm[1] * (1.0 + SMOOTH_M * SMOOTH_CUSPS), hue_table[i] };
		}
		out[0] = { out[TABLE_SIZE][0], out[TABLE_SIZE][1], hue_table[0] };
		out[BASE_INDEX + TABLE_SIZE] = { out[BASE_INDEX][0], out[BASE_INDEX][1], hue_table[BASE_INDEX + TABLE_SIZE] };
		return out;
	}

	// Gamut compression helpers for gamma fitting
	static double solve_J_intersect(double J, double M, double focusJ, double maxJ, double slope_gain)
	{
		double M_scaled = M / slope_gain;
		double a = M_scaled / focusJ;
		if (J < focusJ) {
			double b = 1.0 - M_scaled, c = -J;
			return -2.0 * c / (b + std::sqrt(b * b - 4.0 * a * c));
		} else {
			double b = -(1.0 + M_scaled + maxJ * a), c = maxJ * M_scaled + J;
			return -2.0 * c / (b - std::sqrt(b * b - 4.0 * a * c));
		}
	}

	static double compute_slope(double iJ, double fJ, double lJm, double sg)
	{
		double dir = (iJ < fJ) ? iJ : (lJm - iJ);
		return dir * (iJ - fJ) / (fJ * sg);
	}

	static double compute_focus_J(double cusp_J, double mid_J, double limit_J_max)
	{
		return cusp_J + std::min(1.0, CUSP_MID_BLEND - cusp_J / limit_J_max) * (mid_J - cusp_J);
	}

	static double get_focus_gain(double J, double thr, double lJm, double fd)
	{
		double gain = lJm * fd;
		if (J > thr) {
			double adj = std::log10((lJm - thr) / std::max(0.0001, lJm - J));
			gain *= adj * adj + 1.0;
		}
		return gain;
	}

	static double estimate_boundary_M(double Ji, double slope, double igamma, double Jmax, double Mmax, double Jref)
	{
		double nJ = Ji / Jref;
		double shifted = Jref * std::pow(nJ, igamma);
		return shifted * Mmax / (Jmax - slope * Mmax);
	}

	static double smin_scaled(double a, double b, double sref)
	{
		double ss = SMOOTH_CUSPS * sref;
		double h = std::max(ss - std::abs(a - b), 0.0) / ss;
		return std::min(a, b) - h * h * h * ss / 6.0;
	}

	static double find_gamut_boundary(double cJ, double cM, double Jmax,
		double gti, double gbi, double Js, double slope, double Jc)
	{
		double Mlo = estimate_boundary_M(Js, slope, gbi, cJ, cM, Jc);
		double fJc = Jmax - Jc, fJs = Jmax - Js, fJM = Jmax - cJ;
		double Mhi = estimate_boundary_M(fJs, -slope, gti, fJM, cM, fJc);
		return smin_scaled(Mlo, Mhi, cM);
	}

	// Upper hull gamma fitting
	static constexpr int TEST_COUNT = 5;
	static constexpr double TEST_POS[TEST_COUNT] = { 0.01, 0.1, 0.5, 0.8, 0.99 };

	static bool evaluate_gamma_fit(double cJ, double cM, double hue,
		double top_gamma_inv, double peakLum, double lJm,
		double lhgi, const JMhParams& lparams, double mid_J, double focus_dist)
	{
		double focus_J = compute_focus_J(cJ, mid_J, lJm);
		double analytical_thr = cJ + FOCUS_GAIN_BLEND * (lJm - cJ);
		double luminance_limit = peakLum / REF_LUMINANCE;

		for (int t = 0; t < TEST_COUNT; t++) {
			double test_J = cJ + TEST_POS[t] * (lJm - cJ);
			double sg = get_focus_gain(test_J, analytical_thr, lJm, focus_dist);
			double Ji = solve_J_intersect(test_J, cM, focus_J, lJm, sg);
			double slope = compute_slope(Ji, focus_J, lJm, sg);
			double Jc = solve_J_intersect(cJ, cM, focus_J, lJm, sg);

			double aM = find_gamut_boundary(cJ, cM, lJm, top_gamma_inv, lhgi, Ji, slope, Jc);
			double aJ = Ji + slope * aM;
			Vec3 rgb = JMh_to_RGB({ aJ, aM, hue }, lparams);
			if (!outside_hull(rgb, luminance_limit))
				return false;
		}
		return true;
	}

	static std::array<double, TOTAL_TABLE_SIZE> make_upper_hull_gamma_table(
		const std::array<std::array<double, 3>, TOTAL_TABLE_SIZE>& cusps,
		const ODTParams& p)
	{
		std::array<double, TOTAL_TABLE_SIZE> uhg{};
		for (int i = BASE_INDEX; i < BASE_INDEX + TABLE_SIZE; i++) {
			double hue = cusps[i][2], cJ = cusps[i][0], cM = cusps[i][1];
			double low = GAMMA_MINIMUM, high = low + GAMMA_SEARCH_STEP;
			bool outside = false;
			while (!outside && high < GAMMA_MAXIMUM) {
				if (evaluate_gamma_fit(cJ, cM, hue, 1.0 / high, p.peakLuminance, p.limit_J_max, p.lower_hull_gamma_inv, p.limit_params, p.mid_J, p.focus_dist))
					outside = true;
				else {
					low = high;
					high += GAMMA_SEARCH_STEP;
				}
			}
			while ((high - low) > GAMMA_ACCURACY) {
				double mid = (high + low) / 2.0;
				if (evaluate_gamma_fit(cJ, cM, hue, 1.0 / mid, p.peakLuminance, p.limit_J_max, p.lower_hull_gamma_inv, p.limit_params, p.mid_J, p.focus_dist))
					high = mid;
				else
					low = mid;
			}
			uhg[i] = 1.0 / high;
		}
		uhg[0] = uhg[TABLE_SIZE];
		uhg[TABLE_SIZE + BASE_INDEX] = uhg[BASE_INDEX];
		return uhg;
	}

	static std::array<int, 2> determine_search_range(const std::array<double, TOTAL_TABLE_SIZE>& ht)
	{
		int lo_pad = 0, hi_pad = 1;
		for (int i = BASE_INDEX; i < BASE_INDEX + TABLE_SIZE; i++) {
			int pos = hue_position(ht[i], TOTAL_TABLE_SIZE);
			int delta = i - pos;
			lo_pad = std::min(lo_pad, delta);
			hi_pad = std::max(hi_pad, delta + 1);
		}
		return { lo_pad, hi_pad };
	}

	// ============================================================
	// init_ODTParams - Main entry point
	// ============================================================

	ODTParams init_ODTParams(double peakLuminance, Chromaticities limitingPri)
	{
		ODTParams p;
		p.peakLuminance = peakLuminance;

		p.input_params = init_JMhParams(AP0);
		p.reach_params = init_JMhParams(AP1);  // REACH_PRI == AP1
		p.limit_params = init_JMhParams(limitingPri);

		p.ts = init_TSParams(peakLuminance);

		p.limit_J_max = Y_to_J(peakLuminance, p.input_params);
		p.model_gamma_inv = 1.0 / MODEL_GAMMA;
		p.TABLE_reach_M = make_reach_m_table(p.reach_params, p.limit_J_max);

		// Chroma compression params
		p.sat = std::max(0.2, CHROMA_EXPAND - CHROMA_EXPAND * CHROMA_EXPAND_FACT * p.ts.log_peak);
		p.sat_thr = CHROMA_EXPAND_THR / peakLuminance;
		p.compr = CHROMA_COMPRESS + CHROMA_COMPRESS * CHROMA_COMPRESS_FACT * p.ts.log_peak;
		p.chroma_compress_scale = std::pow(0.03379 * peakLuminance, 0.30596) - 0.45135;

		// Gamut compression params
		p.mid_J = Y_to_J(p.ts.c_t * REF_LUMINANCE, p.input_params);
		p.focus_dist = FOCUS_DISTANCE + FOCUS_DISTANCE * FOCUS_DISTANCE_SCALING * p.ts.log_peak;
		double lower_hull_gamma = 1.14 + 0.07 * p.ts.log_peak;
		p.lower_hull_gamma_inv = 1.0 / lower_hull_gamma;

		// Build gamut tables
		auto reach_corners = find_reach_corners(p.reach_params, p);
		auto limit_corners = build_limiting_corners(p.limit_params, peakLuminance);
		auto sorted_hues = extract_sorted_hues(reach_corners, limit_corners.JMh);
		auto hue_table = build_hue_table(sorted_hues);
		auto cusp_table = build_cusp_table(hue_table, limit_corners.RGB, limit_corners.JMh, p.limit_params);

		p.TABLE_hues = hue_table;
		p.TABLE_gamut_cusps = cusp_table;
		p.TABLE_upper_hull_gamma = make_upper_hull_gamma_table(cusp_table, p);
		auto sr = determine_search_range(hue_table);
		p.hue_linearity_search_range[0] = sr[0];
		p.hue_linearity_search_range[1] = sr[1];

		return p;
	}

	// ============================================================
	// GPU data conversion
	// ============================================================

	static void storeMatrix(const Mat3& src, float* dst)
	{
		// HLSL float3x3 = 3 rows, each row stored as float4 (padded)
		for (int row = 0; row < 3; row++) {
			dst[row * 4 + 0] = (float)src[row][0];
			dst[row * 4 + 1] = (float)src[row][1];
			dst[row * 4 + 2] = (float)src[row][2];
			dst[row * 4 + 3] = 0.0f;
		}
	}

	static GPU_JMhParams buildGPUJMh(const JMhParams& p)
	{
		GPU_JMhParams g{};
		storeMatrix(p.MATRIX_RGB_to_CAM16_c, g.MATRIX_RGB_to_CAM16_c);
		storeMatrix(p.MATRIX_CAM16_c_to_RGB, g.MATRIX_CAM16_c_to_RGB);
		storeMatrix(p.MATRIX_cone_response_to_Aab, g.MATRIX_cone_response_to_Aab);
		storeMatrix(p.MATRIX_Aab_to_cone_response, g.MATRIX_Aab_to_cone_response);
		g.F_L_n = (float)p.F_L_n;
		g.cz = (float)p.cz;
		g.inv_cz = (float)p.inv_cz;
		g.A_w_J = (float)p.A_w_J;
		g.inv_A_w_J = (float)p.inv_A_w_J;
		return g;
	}

	GPU_ODTParams buildGPUParams(const ODTParams& p)
	{
		GPU_ODTParams g{};
		g.peakLuminance = (float)p.peakLuminance;
		g.limit_J_max = (float)p.limit_J_max;
		g.model_gamma_inv = (float)p.model_gamma_inv;
		g.mid_J = (float)p.mid_J;
		g.focus_dist = (float)p.focus_dist;
		g.lower_hull_gamma_inv = (float)p.lower_hull_gamma_inv;
		g.sat = (float)p.sat;
		g.sat_thr = (float)p.sat_thr;
		g.compr = (float)p.compr;
		g.chroma_compress_scale = (float)p.chroma_compress_scale;
		g.hue_linearity_search_range0 = p.hue_linearity_search_range[0];
		g.hue_linearity_search_range1 = p.hue_linearity_search_range[1];

		g.ts.n = (float)p.ts.n;
		g.ts.n_r = (float)p.ts.n_r;
		g.ts.g = (float)p.ts.g;
		g.ts.t_1 = (float)p.ts.t_1;
		g.ts.c_t = (float)p.ts.c_t;
		g.ts.s_2 = (float)p.ts.s_2;
		g.ts.u_2 = (float)p.ts.u_2;
		g.ts.m_2 = (float)p.ts.m_2;
		g.ts.forward_limit = (float)p.ts.forward_limit;
		g.ts.inverse_limit = (float)p.ts.inverse_limit;
		g.ts.log_peak = (float)p.ts.log_peak;

		g.input_params = buildGPUJMh(p.input_params);
		g.reach_params = buildGPUJMh(p.reach_params);
		g.limit_params = buildGPUJMh(p.limit_params);
		return g;
	}

	void buildGPUTableData(const ODTParams& p, std::array<float, TABLE_DATA_COUNT>& out)
	{
		// Layout: [reach_M: 362] [hues: 362] [upper_hull_gamma: 362] [cusps_J: 362] [cusps_M: 362] [cusps_h: 362]
		for (int i = 0; i < TOTAL_TABLE_SIZE; i++) {
			out[0 * TOTAL_TABLE_SIZE + i] = (float)p.TABLE_reach_M[i];
			out[1 * TOTAL_TABLE_SIZE + i] = (float)p.TABLE_hues[i];
			out[2 * TOTAL_TABLE_SIZE + i] = (float)p.TABLE_upper_hull_gamma[i];
			out[3 * TOTAL_TABLE_SIZE + i] = (float)p.TABLE_gamut_cusps[i][0];
			out[4 * TOTAL_TABLE_SIZE + i] = (float)p.TABLE_gamut_cusps[i][1];
			out[5 * TOTAL_TABLE_SIZE + i] = (float)p.TABLE_gamut_cusps[i][2];
		}
	}

}  // namespace ACES2
