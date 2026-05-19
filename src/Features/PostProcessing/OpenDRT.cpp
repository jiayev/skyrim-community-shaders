#include "OpenDRT.h"
#include "Util.h"

namespace
{
	enum class LookPreset : int32_t
	{
		Standard = 0,
		Arriba,
		Sylvan,
		Colorful,
		Aery,
		Dystopic,
		Umbra,
		Base,
	};

	enum class TonescalePreset : int32_t
	{
		UseLookPreset = 0,
		LowContrast,
		MediumContrast,
		HighContrast,
		ArribaTonescale,
		SylvanTonescale,
		ColorfulTonescale,
		AeryTonescale,
		DystopicTonescale,
		UmbraTonescale,
		ACES1x,
		ACES2,
		MarvelousTonescape,
		DaGrinchiTonegroan,
	};

	struct OpenDRTPresetSelection
	{
		LookPreset lookPreset = LookPreset::Standard;
		TonescalePreset tonescalePreset = TonescalePreset::UseLookPreset;
	};

	const char* const kInputColorSpaceLabels[] = {
		"Linear Rec.709/sRGB",
		"ACEScg",
	};

	const char* const kOutputEncodingLabels[] = {
		"Linear Rec.709/sRGB",
		"Linear Rec.2020",
	};

	const char* const kLookPresetLabels[] = {
		"Standard",
		"Arriba",
		"Sylvan",
		"Colorful",
		"Aery",
		"Dystopic",
		"Umbra",
		"Base",
	};

	const char* const kTonescalePresetLabels[] = {
		"USE LOOK PRESET",
		"Low Contrast",
		"Medium Contrast",
		"High Contrast",
		"Arriba Tonescale",
		"Sylvan Tonescale",
		"Colorful Tonescale",
		"Aery Tonescale",
		"Dystopic Tonescale",
		"Umbra Tonescale",
		"ACES-1.x",
		"ACES-2.0",
		"Marvelous Tonescape",
		"DaGrinchi ToneGroan",
	};

	const char* const kCreativeWhitePresetLabels[] = {
		"USE LOOK PRESET",
		"D93",
		"D75",
		"D65",
		"D60",
		"D55",
		"D50",
	};

	const char* const kCreativeWhiteLabels[] = {
		"D93",
		"D75",
		"D65",
		"D60",
		"D55",
		"D50",
	};

	const char* const kSurroundLabels[] = { "Dark", "Dim", "Bright" };

	int ClampIndex(int value, int count)
	{
		if (value < 0) {
			return 0;
		}
		if (value >= count) {
			return count - 1;
		}
		return value;
	}

	bool ComboInt(const char* label, std::int32_t& value, const char* const* items, int count)
	{
		int current = ClampIndex(static_cast<int>(value), count);
		if (ImGui::Combo(label, &current, items, count)) {
			value = static_cast<std::int32_t>(current);
			return true;
		}
		return false;
	}

	bool CheckboxInt(const char* label, std::int32_t& value)
	{
		bool current = value != 0;
		if (ImGui::Checkbox(label, &current)) {
			value = current ? 1 : 0;
			return true;
		}
		return false;
	}

	template <typename Enum, int Count>
	bool ComboEnum(const char* label, Enum& value, const char* const (&items)[Count])
	{
		int current = ClampIndex(static_cast<int>(value), Count);
		if (ImGui::Combo(label, &current, items, Count)) {
			value = static_cast<Enum>(current);
			return true;
		}
		return false;
	}

}  // namespace

void ApplyLookPreset(OpenDRTSettings& s, LookPreset preset)
{
	switch (preset) {
	case LookPreset::Standard:
		s.tn_con = 1.66f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.005f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 0;
		s.tn_lcon = 0.0f;
		s.tn_lcon_w = 0.5f;
		s.cwp = 2;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.35f;
		s.rs_rw = 0.25f;
		s.rs_bw = 0.55f;
		s.pt_enable = 1;
		s.pt_lml = 0.25f;
		s.pt_lml_r = 0.5f;
		s.pt_lml_g = 0.0f;
		s.pt_lml_b = 0.1f;
		s.pt_lmh = 0.25f;
		s.pt_lmh_r = 0.5f;
		s.pt_lmh_b = 0.0f;
		s.ptl_enable = 1;
		s.ptl_c = 0.06f;
		s.ptl_m = 0.08f;
		s.ptl_y = 0.06f;
		s.ptm_enable = 1;
		s.ptm_low = 0.4f;
		s.ptm_low_rng = 0.25f;
		s.ptm_low_st = 0.5f;
		s.ptm_high = -0.8f;
		s.ptm_high_rng = 0.35f;
		s.ptm_high_st = 0.4f;
		s.brl_enable = 1;
		s.brl = 0.0f;
		s.brl_r = -2.5f;
		s.brl_g = -1.5f;
		s.brl_b = -1.5f;
		s.brl_rng = 0.5f;
		s.brl_st = 0.35f;
		s.brlp_enable = 1;
		s.brlp = -0.5f;
		s.brlp_r = -1.25f;
		s.brlp_g = -1.25f;
		s.brlp_b = -0.25f;
		s.hc_enable = 1;
		s.hc_r = 1.0f;
		s.hc_r_rng = 0.3f;
		s.hs_rgb_enable = 1;
		s.hs_r = 0.6f;
		s.hs_r_rng = 0.6f;
		s.hs_g = 0.35f;
		s.hs_g_rng = 1.0f;
		s.hs_b = 0.66f;
		s.hs_b_rng = 1.0f;
		s.hs_cmy_enable = 1;
		s.hs_c = 0.25f;
		s.hs_c_rng = 1.0f;
		s.hs_m = 0.0f;
		s.hs_m_rng = 1.0f;
		s.hs_y = 0.0f;
		s.hs_y_rng = 1.0f;
		break;
	case LookPreset::Arriba:
		s.tn_con = 1.05f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.1f;
		s.tn_off = 0.01f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.5f;
		s.tn_lcon_w = 0.2f;
		s.cwp = 2;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.35f;
		s.rs_rw = 0.25f;
		s.rs_bw = 0.55f;
		s.pt_enable = 1;
		s.pt_lml = 0.25f;
		s.pt_lml_r = 0.45f;
		s.pt_lml_g = 0.0f;
		s.pt_lml_b = 0.1f;
		s.pt_lmh = 0.25f;
		s.pt_lmh_r = 0.25f;
		s.pt_lmh_b = 0.0f;
		s.ptl_enable = 1;
		s.ptl_c = 0.06f;
		s.ptl_m = 0.08f;
		s.ptl_y = 0.06f;
		s.ptm_enable = 1;
		s.ptm_low = 1.0f;
		s.ptm_low_rng = 0.4f;
		s.ptm_low_st = 0.5f;
		s.ptm_high = -0.8f;
		s.ptm_high_rng = 0.66f;
		s.ptm_high_st = 0.6f;
		s.brl_enable = 1;
		s.brl = 0.0f;
		s.brl_r = -2.5f;
		s.brl_g = -1.5f;
		s.brl_b = -1.5f;
		s.brl_rng = 0.5f;
		s.brl_st = 0.35f;
		s.brlp_enable = 1;
		s.brlp = 0.0f;
		s.brlp_r = -1.7f;
		s.brlp_g = -2.0f;
		s.brlp_b = -0.5f;
		s.hc_enable = 1;
		s.hc_r = 1.0f;
		s.hc_r_rng = 0.3f;
		s.hs_rgb_enable = 1;
		s.hs_r = 0.6f;
		s.hs_r_rng = 0.8f;
		s.hs_g = 0.35f;
		s.hs_g_rng = 1.0f;
		s.hs_b = 0.66f;
		s.hs_b_rng = 1.0f;
		s.hs_cmy_enable = 1;
		s.hs_c = 0.15f;
		s.hs_c_rng = 1.0f;
		s.hs_m = 0.0f;
		s.hs_m_rng = 1.0f;
		s.hs_y = 0.0f;
		s.hs_y_rng = 1.0f;
		break;
	case LookPreset::Sylvan:
		s.tn_con = 1.6f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.01f;
		s.tn_off = 0.01f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 0.25f;
		s.tn_lcon_w = 0.75f;
		s.cwp = 2;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.25f;
		s.rs_rw = 0.25f;
		s.rs_bw = 0.55f;
		s.pt_enable = 1;
		s.pt_lml = 0.15f;
		s.pt_lml_r = 0.5f;
		s.pt_lml_g = 0.15f;
		s.pt_lml_b = 0.1f;
		s.pt_lmh = 0.25f;
		s.pt_lmh_r = 0.15f;
		s.pt_lmh_b = 0.15f;
		s.ptl_enable = 1;
		s.ptl_c = 0.05f;
		s.ptl_m = 0.08f;
		s.ptl_y = 0.05f;
		s.ptm_enable = 1;
		s.ptm_low = 0.5f;
		s.ptm_low_rng = 0.5f;
		s.ptm_low_st = 0.5f;
		s.ptm_high = -0.8f;
		s.ptm_high_rng = 0.5f;
		s.ptm_high_st = 0.5f;
		s.brl_enable = 1;
		s.brl = -1.0f;
		s.brl_r = -2.0f;
		s.brl_g = -2.0f;
		s.brl_b = 0.0f;
		s.brl_rng = 0.25f;
		s.brl_st = 0.25f;
		s.brlp_enable = 1;
		s.brlp = -1.0f;
		s.brlp_r = -0.5f;
		s.brlp_g = -0.25f;
		s.brlp_b = -0.25f;
		s.hc_enable = 1;
		s.hc_r = 1.0f;
		s.hc_r_rng = 0.4f;
		s.hs_rgb_enable = 1;
		s.hs_r = 0.6f;
		s.hs_r_rng = 1.15f;
		s.hs_g = 0.8f;
		s.hs_g_rng = 1.25f;
		s.hs_b = 0.6f;
		s.hs_b_rng = 1.0f;
		s.hs_cmy_enable = 1;
		s.hs_c = 0.25f;
		s.hs_c_rng = 0.25f;
		s.hs_m = 0.25f;
		s.hs_m_rng = 0.5f;
		s.hs_y = 0.35f;
		s.hs_y_rng = 0.5f;
		break;
	case LookPreset::Colorful:
		s.tn_con = 1.5f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.003f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 0.4f;
		s.tn_lcon_w = 0.5f;
		s.cwp = 2;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.35f;
		s.rs_rw = 0.25f;
		s.rs_bw = 0.55f;
		s.pt_enable = 1;
		s.pt_lml = 0.5f;
		s.pt_lml_r = 1.0f;
		s.pt_lml_g = 0.0f;
		s.pt_lml_b = 0.5f;
		s.pt_lmh = 0.15f;
		s.pt_lmh_r = 0.15f;
		s.pt_lmh_b = 0.15f;
		s.ptl_enable = 1;
		s.ptl_c = 0.05f;
		s.ptl_m = 0.06f;
		s.ptl_y = 0.05f;
		s.ptm_enable = 1;
		s.ptm_low = 0.8f;
		s.ptm_low_rng = 0.5f;
		s.ptm_low_st = 0.4f;
		s.ptm_high = -0.8f;
		s.ptm_high_rng = 0.4f;
		s.ptm_high_st = 0.4f;
		s.brl_enable = 1;
		s.brl = 0.0f;
		s.brl_r = -1.25f;
		s.brl_g = -1.25f;
		s.brl_b = -0.25f;
		s.brl_rng = 0.3f;
		s.brl_st = 0.5f;
		s.brlp_enable = 1;
		s.brlp = -0.5f;
		s.brlp_r = -1.25f;
		s.brlp_g = -1.25f;
		s.brlp_b = -0.5f;
		s.hc_enable = 1;
		s.hc_r = 1.0f;
		s.hc_r_rng = 0.4f;
		s.hs_rgb_enable = 1;
		s.hs_r = 0.5f;
		s.hs_r_rng = 0.8f;
		s.hs_g = 0.35f;
		s.hs_g_rng = 1.0f;
		s.hs_b = 0.5f;
		s.hs_b_rng = 1.0f;
		s.hs_cmy_enable = 1;
		s.hs_c = 0.25f;
		s.hs_c_rng = 1.0f;
		s.hs_m = 0.0f;
		s.hs_m_rng = 1.0f;
		s.hs_y = 0.25f;
		s.hs_y_rng = 1.0f;
		break;
	case LookPreset::Aery:
		s.tn_con = 1.15f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.04f;
		s.tn_off = 0.006f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 0.0f;
		s.tn_hcon_st = 0.5f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 0.5f;
		s.tn_lcon_w = 2.0f;
		s.cwp = 1;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.25f;
		s.rs_rw = 0.2f;
		s.rs_bw = 0.5f;
		s.pt_enable = 1;
		s.pt_lml = 0.0f;
		s.pt_lml_r = 0.5f;
		s.pt_lml_g = 0.15f;
		s.pt_lml_b = 0.1f;
		s.pt_lmh = 0.0f;
		s.pt_lmh_r = 0.1f;
		s.pt_lmh_b = 0.0f;
		s.ptl_enable = 1;
		s.ptl_c = 0.05f;
		s.ptl_m = 0.08f;
		s.ptl_y = 0.05f;
		s.ptm_enable = 1;
		s.ptm_low = 0.8f;
		s.ptm_low_rng = 0.35f;
		s.ptm_low_st = 0.5f;
		s.ptm_high = -0.9f;
		s.ptm_high_rng = 0.5f;
		s.ptm_high_st = 0.3f;
		s.brl_enable = 1;
		s.brl = -3.0f;
		s.brl_r = 0.0f;
		s.brl_g = 0.0f;
		s.brl_b = 1.0f;
		s.brl_rng = 0.8f;
		s.brl_st = 0.15f;
		s.brlp_enable = 1;
		s.brlp = -1.0f;
		s.brlp_r = -1.0f;
		s.brlp_g = -1.0f;
		s.brlp_b = 0.0f;
		s.hc_enable = 1;
		s.hc_r = 0.5f;
		s.hc_r_rng = 0.25f;
		s.hs_rgb_enable = 1;
		s.hs_r = 0.6f;
		s.hs_r_rng = 1.0f;
		s.hs_g = 0.35f;
		s.hs_g_rng = 2.0f;
		s.hs_b = 0.5f;
		s.hs_b_rng = 1.5f;
		s.hs_cmy_enable = 1;
		s.hs_c = 0.35f;
		s.hs_c_rng = 1.0f;
		s.hs_m = 0.25f;
		s.hs_m_rng = 1.0f;
		s.hs_y = 0.35f;
		s.hs_y_rng = 0.5f;
		break;
	case LookPreset::Dystopic:
		s.tn_con = 1.6f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.01f;
		s.tn_off = 0.008f;
		s.tn_hcon_enable = 1;
		s.tn_hcon = 0.25f;
		s.tn_hcon_pv = 0.0f;
		s.tn_hcon_st = 1.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.0f;
		s.tn_lcon_w = 0.75f;
		s.cwp = 3;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.2f;
		s.rs_rw = 0.25f;
		s.rs_bw = 0.55f;
		s.pt_enable = 1;
		s.pt_lml = 0.15f;
		s.pt_lml_r = 0.0f;
		s.pt_lml_g = 0.0f;
		s.pt_lml_b = 0.0f;
		s.pt_lmh = 0.0f;
		s.pt_lmh_r = 0.0f;
		s.pt_lmh_b = 0.0f;
		s.ptl_enable = 1;
		s.ptl_c = 0.05f;
		s.ptl_m = 0.08f;
		s.ptl_y = 0.05f;
		s.ptm_enable = 1;
		s.ptm_low = 0.25f;
		s.ptm_low_rng = 0.25f;
		s.ptm_low_st = 0.8f;
		s.ptm_high = -0.8f;
		s.ptm_high_rng = 0.6f;
		s.ptm_high_st = 0.25f;
		s.brl_enable = 1;
		s.brl = -2.0f;
		s.brl_r = -2.0f;
		s.brl_g = -2.0f;
		s.brl_b = 0.0f;
		s.brl_rng = 0.35f;
		s.brl_st = 0.35f;
		s.brlp_enable = 1;
		s.brlp = 0.0f;
		s.brlp_r = -1.0f;
		s.brlp_g = -1.0f;
		s.brlp_b = -1.0f;
		s.hc_enable = 1;
		s.hc_r = 1.0f;
		s.hc_r_rng = 0.25f;
		s.hs_rgb_enable = 1;
		s.hs_r = 0.7f;
		s.hs_r_rng = 1.33f;
		s.hs_g = 1.0f;
		s.hs_g_rng = 2.0f;
		s.hs_b = 0.75f;
		s.hs_b_rng = 2.0f;
		s.hs_cmy_enable = 1;
		s.hs_c = 1.0f;
		s.hs_c_rng = 0.5f;
		s.hs_m = 1.0f;
		s.hs_m_rng = 1.0f;
		s.hs_y = 1.0f;
		s.hs_y_rng = 0.765f;
		break;
	case LookPreset::Umbra:
		s.tn_con = 1.8f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.001f;
		s.tn_off = 0.015f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.0f;
		s.tn_lcon_w = 1.0f;
		s.cwp = 5;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.35f;
		s.rs_rw = 0.25f;
		s.rs_bw = 0.55f;
		s.pt_enable = 1;
		s.pt_lml = 0.0f;
		s.pt_lml_r = 0.5f;
		s.pt_lml_g = 0.0f;
		s.pt_lml_b = 0.15f;
		s.pt_lmh = 0.25f;
		s.pt_lmh_r = 0.25f;
		s.pt_lmh_b = 0.0f;
		s.ptl_enable = 1;
		s.ptl_c = 0.05f;
		s.ptl_m = 0.06f;
		s.ptl_y = 0.05f;
		s.ptm_enable = 1;
		s.ptm_low = 0.4f;
		s.ptm_low_rng = 0.35f;
		s.ptm_low_st = 0.66f;
		s.ptm_high = -0.6f;
		s.ptm_high_rng = 0.45f;
		s.ptm_high_st = 0.45f;
		s.brl_enable = 1;
		s.brl = -2.0f;
		s.brl_r = -4.5f;
		s.brl_g = -3.0f;
		s.brl_b = -4.0f;
		s.brl_rng = 0.35f;
		s.brl_st = 0.3f;
		s.brlp_enable = 1;
		s.brlp = 0.0f;
		s.brlp_r = -2.0f;
		s.brlp_g = -1.0f;
		s.brlp_b = -0.5f;
		s.hc_enable = 1;
		s.hc_r = 1.0f;
		s.hc_r_rng = 0.35f;
		s.hs_rgb_enable = 1;
		s.hs_r = 0.66f;
		s.hs_r_rng = 1.0f;
		s.hs_g = 0.5f;
		s.hs_g_rng = 2.0f;
		s.hs_b = 0.85f;
		s.hs_b_rng = 2.0f;
		s.hs_cmy_enable = 1;
		s.hs_c = 0.0f;
		s.hs_c_rng = 1.0f;
		s.hs_m = 0.25f;
		s.hs_m_rng = 1.0f;
		s.hs_y = 0.66f;
		s.hs_y_rng = 0.66f;
		break;
	case LookPreset::Base:
		s.tn_con = 1.66f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.005f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 0;
		s.tn_lcon = 0.0f;
		s.tn_lcon_w = 0.5f;
		s.cwp = 2;
		s.cwp_lm = 0.25f;
		s.rs_sa = 0.35f;
		s.rs_rw = 0.25f;
		s.rs_bw = 0.55f;
		s.pt_enable = 1;
		s.pt_lml = 0.5f;
		s.pt_lml_r = 0.5f;
		s.pt_lml_g = 0.15f;
		s.pt_lml_b = 0.15f;
		s.pt_lmh = 0.8f;
		s.pt_lmh_r = 0.5f;
		s.pt_lmh_b = 0.0f;
		s.ptl_enable = 1;
		s.ptl_c = 0.05f;
		s.ptl_m = 0.06f;
		s.ptl_y = 0.05f;
		s.ptm_enable = 0;
		s.ptm_low = 0.0f;
		s.ptm_low_rng = 0.5f;
		s.ptm_low_st = 0.5f;
		s.ptm_high = 0.0f;
		s.ptm_high_rng = 0.5f;
		s.ptm_high_st = 0.5f;
		s.brl_enable = 0;
		s.brl = 0.0f;
		s.brl_r = 0.0f;
		s.brl_g = 0.0f;
		s.brl_b = 0.0f;
		s.brl_rng = 0.5f;
		s.brl_st = 0.35f;
		s.brlp_enable = 1;
		s.brlp = -0.5f;
		s.brlp_r = -1.6f;
		s.brlp_g = -1.6f;
		s.brlp_b = -0.8f;
		s.hc_enable = 0;
		s.hc_r = 0.0f;
		s.hc_r_rng = 0.25f;
		s.hs_rgb_enable = 0;
		s.hs_r = 0.0f;
		s.hs_r_rng = 1.0f;
		s.hs_g = 0.0f;
		s.hs_g_rng = 1.0f;
		s.hs_b = 0.0f;
		s.hs_b_rng = 1.0f;
		s.hs_cmy_enable = 0;
		s.hs_c = 0.0f;
		s.hs_c_rng = 1.0f;
		s.hs_m = 0.0f;
		s.hs_m_rng = 1.0f;
		s.hs_y = 0.0f;
		s.hs_y_rng = 1.0f;
		break;
	default:
		break;
	}
}

void ApplyTonescalePreset(OpenDRTSettings& s, TonescalePreset preset)
{
	switch (preset) {
	case TonescalePreset::LowContrast:
		s.tn_con = 1.4f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.005f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 0;
		s.tn_lcon = 0.0f;
		s.tn_lcon_w = 0.5f;
		break;
	case TonescalePreset::MediumContrast:
		s.tn_con = 1.66f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.005f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 0;
		s.tn_lcon = 0.0f;
		s.tn_lcon_w = 0.5f;
		break;
	case TonescalePreset::HighContrast:
		s.tn_con = 1.4f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.005f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.0f;
		s.tn_lcon_w = 0.5f;
		break;
	case TonescalePreset::ArribaTonescale:
		s.tn_con = 1.05f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.1f;
		s.tn_off = 0.01f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.5f;
		s.tn_lcon_w = 0.2f;
		break;
	case TonescalePreset::SylvanTonescale:
		s.tn_con = 1.6f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.01f;
		s.tn_off = 0.01f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 0.25f;
		s.tn_lcon_w = 0.75f;
		break;
	case TonescalePreset::ColorfulTonescale:
		s.tn_con = 1.5f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.003f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 0.4f;
		s.tn_lcon_w = 0.5f;
		break;
	case TonescalePreset::AeryTonescale:
		s.tn_con = 1.15f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.04f;
		s.tn_off = 0.006f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 0.0f;
		s.tn_hcon_st = 0.5f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 0.5f;
		s.tn_lcon_w = 2.0f;
		break;
	case TonescalePreset::DystopicTonescale:
		s.tn_con = 1.6f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.01f;
		s.tn_off = 0.008f;
		s.tn_hcon_enable = 1;
		s.tn_hcon = 0.25f;
		s.tn_hcon_pv = 0.0f;
		s.tn_hcon_st = 1.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.0f;
		s.tn_lcon_w = 0.75f;
		break;
	case TonescalePreset::UmbraTonescale:
		s.tn_con = 1.8f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.001f;
		s.tn_off = 0.015f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.0f;
		s.tn_lcon_w = 1.0f;
		break;
	case TonescalePreset::ACES1x:
		s.tn_con = 1.0f;
		s.tn_sh = 0.35f;
		s.tn_toe = 0.02f;
		s.tn_off = 0.0f;
		s.tn_hcon_enable = 1;
		s.tn_hcon = 0.55f;
		s.tn_hcon_pv = 0.0f;
		s.tn_hcon_st = 2.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.13f;
		s.tn_lcon_w = 1.0f;
		break;
	case TonescalePreset::ACES2:
		s.tn_con = 1.15f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.04f;
		s.tn_off = 0.0f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 1.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 1.0f;
		s.tn_lcon_enable = 0;
		s.tn_lcon = 1.0f;
		s.tn_lcon_w = 0.6f;
		break;
	case TonescalePreset::MarvelousTonescape:
		s.tn_con = 1.5f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.003f;
		s.tn_off = 0.01f;
		s.tn_hcon_enable = 1;
		s.tn_hcon = 0.25f;
		s.tn_hcon_pv = 0.0f;
		s.tn_hcon_st = 4.0f;
		s.tn_lcon_enable = 1;
		s.tn_lcon = 1.0f;
		s.tn_lcon_w = 1.0f;
		break;
	case TonescalePreset::DaGrinchiTonegroan:
		s.tn_con = 1.2f;
		s.tn_sh = 0.5f;
		s.tn_toe = 0.02f;
		s.tn_off = 0.0f;
		s.tn_hcon_enable = 0;
		s.tn_hcon = 0.0f;
		s.tn_hcon_pv = 1.0f;
		s.tn_hcon_st = 1.0f;
		s.tn_lcon_enable = 0;
		s.tn_lcon = 0.0f;
		s.tn_lcon_w = 0.6f;
		break;
	default:
		break;
	}
}

void OpenDRTDrawSettings(OpenDRTSettings& s)
{
	static OpenDRTPresetSelection presets{};

	if (ImGui::CollapsingHeader("Built-in Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
		ComboEnum("Look Preset", presets.lookPreset, kLookPresetLabels);
		ComboEnum("Tonescale Preset", presets.tonescalePreset, kTonescalePresetLabels);

		if (ImGui::Button("Apply Preset", { -FLT_MIN, 0 })) {
			ApplyLookPreset(s, presets.lookPreset);
			ApplyTonescalePreset(s, presets.tonescalePreset);
			s.clamp = 1;
		}
	}

	// if (ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_DefaultOpen)) {
	// 	ComboInt("Input Color Space", s.input_color_space, kInputColorSpaceLabels, IM_ARRAYSIZE(kInputColorSpaceLabels));
	// 	ComboInt("Output Encoding", s.output_encoding, kOutputEncodingLabels, IM_ARRAYSIZE(kOutputEncodingLabels));
	// }

	if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Display Peak Luminance", &s.tn_Lp, 100.0f, 1000.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Peak display luminance in nits.\n"
				"In SDR, the max value stays pinned at 1.0. In HDR, the max value is adjusted to match peak luminance in the HDR container.");
		ImGui::SliderFloat("HDR Grey Boost", &s.tn_gb, 0.0f, 1.0f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Amount of stops to boost Grey Luminance, per stop of exposure increase of Peak Luminance.\n"
				"For example, if HDR Grey Boost is 0.1, middle grey will be boosted by 0.1 stops per stop of Peak Luminance increase.");
		ImGui::SliderFloat("HDR Purity", &s.pt_hdr, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How much to affect purity compression and hue shift as Peak Luminance increases.\n"
				"A value of 0.0 will keep the purity compression and hue shift behavior the same for SDR and HDR.\n"
				"A value of 1.0 will preserve more purity as peak luminance increases\n"
				"(at the risk of gradient disruptions in high purity high intensity light sources), and will reduce hue shift amount in highlights.");
		ImGui::SliderFloat("Display Grey Luminance", &s.tn_Lg, 3.0f, 25.0f, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Display luminance for middle grey (0.18) in nits.\n"
				"Sets the target value for middle grey within the available luminance range of the display device.");
		ComboInt("Surround", s.tn_su, kSurroundLabels, IM_ARRAYSIZE(kSurroundLabels));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"OpenDRT includes a simple surround compensation model.\n"
				"DCI cinema presets use dark surround. Rec.1886 uses dim surround. And sRGB Display uses bright surround.\n"
				"This functionality should provide a better perceptual match between viewing environments.");
		ComboInt("Creative White", s.cwp, kCreativeWhiteLabels, IM_ARRAYSIZE(kCreativeWhiteLabels));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Set the creative whitepoint of the display peak luminance.\n"
				"With D65 all channels are equal. With D50, the peak luminance value will match a D50 whitepoint.\n"
				"This can be creatively desireable. This adjustment is applied post-tonescale.");
		ImGui::SliderFloat("Creative White Limit", &s.cwp_lm, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the intensity range affected by the Creative Whitepoint.\n"
				"At 0.0, the entire intensity range is affected. As the limit is decreased, more of midtones and shadows are kept neutral.\n"
				"It can be creatively desireable to keep midtones more neutral while shifting highlights warmer for example.");
		CheckboxInt("Clamp", s.clamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Clamp the final image into the final range supported by the display device.");
	}

	if (ImGui::CollapsingHeader("Tonescale", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Contrast", &s.tn_con, 1.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Adjusts contrast or slope.\n"
				"A constrained power function applied in display linear.");
		ImGui::SliderFloat("Shoulder Clip", &s.tn_sh, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Unitless control for the scene-linear value at which the tonescale system crosses the peak display linear value (1.0) and clips.\n"
				"This is not an exact constraint in order to keep the system simple, but corresponds to roughly 16 at Shoulder Clip = 0 and 1024 at Shoulder Clip = 1");
		ImGui::SliderFloat("Toe", &s.tn_toe, 0.0f, 0.1f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Quadratic toe compression.\n"
				"Strongly compresses deep shadows. Helpful to have some amount to smooth the transition into display minimum.\n"
				"Higher values with a strong positive Offset also valid. Similar to common camera DRT tonescale strategies.");
		ImGui::SliderFloat("Offset", &s.tn_off, 0.0f, 0.02f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Pre-tonescale scene-linear offset.\n"
				"If 0.0, scene-linear 0.0 maps to display-linear 0.0 through the tonescale system.\n"
				"Many camera imaging pipelines apply a negative offset to set the average of shadow grain at 0.0.\n"
				"A positive Offset can be desireable to compensate for this and increase detail in shadows, in addition to being aesthetically desireable.\n"
				"Offset should NOT be a negative number (Looking at you ACES 1.x)");
		CheckboxInt("Enable Contrast High", s.tn_hcon_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Contrast High allows control of the upper section of the tonescale function.\n"
				"Off by default, but can be useful if a stronger highlight contrast, or a softer highlight rolloff behavior is desired.");
		ImGui::SliderFloat("Contrast High", &s.tn_hcon, -1.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Amount adjust highlights.\n"
				"Positive values increase highlight exposure, negative values decrease. 0 has no effect.");
		ImGui::SliderFloat("Contrast High Pivot", &s.tn_hcon_pv, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount of stops above middle grey (0.18) to start the adjustment.");
		ImGui::SliderFloat("Contrast High Strength", &s.tn_hcon_st, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How quickly above the Contrast High Pivot the effect begins.");
		CheckboxInt("Enable Contrast Low", s.tn_lcon_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Contrast Low adds contrast to the midtones and shadows.\n"
				"Middle grey (0.18) is un-changed through the adjustment.");
		ImGui::SliderFloat("Contrast Low", &s.tn_lcon, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Amount of contrast to add. 0.0 has no effect.\n"
				"1.0 will expose down by 1 stop at the origin (0,0)");
		ImGui::SliderFloat("Contrast Low Width", &s.tn_lcon_w, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The width of the adjustment.\n"
				"Width below 0.5 will mostly affect values between 0 and middle grey.\n"
				"Values above 0.5 will increasingly start to increase highlight contrast, which could be desired or not depending on what you are trying to do.");
	}

	if (ImGui::CollapsingHeader("Render Space")) {
		ImGui::SliderFloat("Render Space Strength", &s.rs_sa, 0.0f, 0.6f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Render space is the encoding in which the RGB Ratios are taken.\n"
				"Strength controls how much to desaturate from P3 gamut. Creatively, the more you desaturate, the more brilliance is increased in the resulting image.\n"
				"To be used with caution as this affects every other aspect of the image rendering.");
		ImGui::SliderFloat("Render Space Weight R", &s.rs_rw, 0.0f, 0.8f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The Red weight of the Render Space Strength.\n"
				"Modify with caution as this affects every other part of the image rendering.");
		ImGui::SliderFloat("Render Space Weight B", &s.rs_bw, 0.0f, 0.8f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The Blue weight of the Render Space Strength.\n"
				"Modify with caution as this affects every other part of the image rendering.");
	}

	if (ImGui::CollapsingHeader("Purity")) {
		CheckboxInt("Enable Purity Compress High", s.pt_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Compresses purity as intensity increases.\n"
				"Bare minimum functionality for a picture formation.");
		ImGui::SliderFloat("Purity Limit Low", &s.pt_lml, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the strength of purity compression as intensity decreases, for all hue angles.\n"
				"A higher value will compress purity less in midtones and shadows.");
		ImGui::SliderFloat("Purity Limit Low R", &s.pt_lml_r, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the strength of purity compression as intensity decreases, for reds.\n"
				"A higher value will compress purity less in midtones and shadows.");
		ImGui::SliderFloat("Purity Limit Low G", &s.pt_lml_g, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the strength of purity compression as intensity decreases, for all greens.\n"
				"A higher value will compress purity less in midtones and shadows.");
		ImGui::SliderFloat("Purity Limit Low B", &s.pt_lml_b, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the strength of purity compression as intensity decreases, for blues.\n"
				"A higher value will compress purity less in midtones and shadows.");
		ImGui::SliderFloat("Purity Limit High", &s.pt_lmh, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the strength of purity compression as intensity increases.\n"
				"Can be helpful to keep some color in high intensity high purity light sources.\n"
				"Use with caution as this can cause gradient disruptions.");
		ImGui::SliderFloat("Purity Limit High R", &s.pt_lmh_r, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the strength of red purity compression as intensity increases.\n"
				"Can be helpful to keep some color in high intensity fire and pure red light sources.\n"
				"Use with caution as this can cause gradient disruptions.");
		ImGui::SliderFloat("Purity Limit High B", &s.pt_lmh_b, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Limit the strength of blue purity compression as intensity increases.\n"
				"Can be helpful to keep some color in high intensity pure blue light sources.\n"
				"Use with caution as this can cause gradient disruptions.");
		CheckboxInt("Enable Purity Softclip", s.ptl_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Purity Softclip increases tonality and smoothness in extremely pure input values\n"
				"that can not be adequately compressed into the display-referred gamut volume.\n"
				"The algorithm is tuned for common camera observer colorimetry sources.");
		ImGui::SliderFloat("Purity Softclip C", &s.ptl_c, 0.0f, 0.25f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Purity Softclip strength for Cyan.");
		ImGui::SliderFloat("Purity Softclip M", &s.ptl_m, 0.0f, 0.25f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Purity Softclip strength for Magenta.");
		ImGui::SliderFloat("Purity Softclip Y", &s.ptl_y, 0.0f, 0.25f, "%.4f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Purity Softclip strength for Yellow.");
		CheckboxInt("Enable Mid Purity", s.ptm_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The Mid Purity module adjusts mid-range purity of midtones and highlights.\n"
				"Without this module enabled, it is likely that midtones will not appear colorful enough,\n"
				"and highlights will appear too colorful resulting in chaulky pasty looking images especially in yellows and cyans. ");
		ImGui::SliderFloat("Mid Purity Low", &s.ptm_low, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Amount to increase purity of midtones and shadows in mid-range purity areas.\n"
				"A value of 0.0 will have no effect. A value of 1.0 is the maximum possible value while preserving smoothness.");
		ImGui::SliderFloat("Mid Purity Low Range", &s.ptm_low_rng, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The strength of the Mid Purity Low adjustment.\n"
				"Higher values affect more of the luminance range.");
		ImGui::SliderFloat("Mid Purity Low Strength", &s.ptm_low_st, 0.1f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The strength of the Mid Purity Low adjustment.\n"
				"Higher values affect more of the purity range.");
		ImGui::SliderFloat("Mid Purity High", &s.ptm_high, -0.9f, 0.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Amount to decrease purity of upper midtones and highlights in mid-range purity areas.\n"
				"A value of 0.0 will have no effect. A value of 1.0 is the maximum possible value while preserving smoothness.");
		ImGui::SliderFloat("Mid Purity High Range", &s.ptm_high_rng, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The strength of the Mid Purity High adjustment.\n"
				"Higher values affect more of the luminance range.");
		ImGui::SliderFloat("Mid Purity High Strength", &s.ptm_high_st, 0.1f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The strength of the Mid Purity High adjustment.\n"
				"Higher values affect more of the purity range.");
	}

	if (ImGui::CollapsingHeader("Brilliance##Header")) {
		CheckboxInt("Enable Brilliance", s.brl_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Brilliance scales the intensity of more pure stimuli.\n"
				"The brilliance module is applied before the tonescale is taken for purity compression.\n"
				"This means that if you darken reds with this adjustment, the purity compression will also be reduced.\n"
				"This behavior is natural and smooth.");
		ImGui::SliderFloat("Brilliance", &s.brl, -6.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Global intensity scale of high-purity stimuli.");
		ImGui::SliderFloat("Brilliance R", &s.brl_r, -6.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Scale intensity of high-purity reds.");
		ImGui::SliderFloat("Brilliance G", &s.brl_g, -6.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Scale intensity of high-purity greens.");
		ImGui::SliderFloat("Brilliance B", &s.brl_b, -6.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Scale intensity of high-purity blues.");
		ImGui::SliderFloat("Brilliance Range", &s.brl_rng, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("As Brilliance Range is increased, the brilliance adjustments affect more the low intensity values of the image data.");
		ImGui::SliderFloat("Brilliance Strength", &s.brl_st, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("As Brilliance Strength is increased, the brilliance adjustments affect more the low purity values of the image data.");
		CheckboxInt("Enable Post Brilliance", s.brlp_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Post Brilliance scales the intensity of more pure stimuli after purity compression hue shifts have been applied.\n"
				"With the OpenDRT algorithm it is possible to get high intensity high purity values going out of the top of the display-referred gamut volume,\n"
				"which can cause discontinuities in gradients, especially on the RGB primaries.\n"
				"This module can help compensate for this.");
		ImGui::SliderFloat("Brilliance Post", &s.brlp, -1.0f, 0.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Global post purity compression brilliance adjustment ");
		ImGui::SliderFloat("Post Brilliance R", &s.brlp_r, -3.0f, 0.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Scale intensity of high-purity red, post purity compression.\n"
				"This can help reduce rings or halos around high purity light sources.");
		ImGui::SliderFloat("Post Brilliance G", &s.brlp_g, -3.0f, 0.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Scale intensity of high-purity green, post purity compression.\n"
				"This can help reduce rings or halos around high purity light sources.");
		ImGui::SliderFloat("Post Brilliance B", &s.brlp_b, -3.0f, 0.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Scale intensity of high-purity blue, post purity compression.\n"
				"This can help reduce rings or halos around high purity light sources.");
	}

	if (ImGui::CollapsingHeader("Hue")) {
		CheckboxInt("Enable Hue Contrast", s.hc_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Hue Contrast compresses hue angle towards the primary at the bottom end and expands the hue angle towards the secondary as intensity increases.\n"
				"It also increases purity as it compresses, and decreases purity as it expands.\n"
				"This leads to a nice creatively controllable simulation of this effect from per-channel tonescales.\n"
				"For OpenDRT we only keep the red hue angle control since it is the most useful.");
		ImGui::SliderFloat("Hue Contrast R", &s.hc_r, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount to increase Hue Contrast at the red hue angle.");
		ImGui::SliderFloat("Hue Contrast R Range", &s.hc_r_rng, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Hue contrast range control: determines where over the intensity range the hue contrast affects.\n"
				"Higher values place the crossover point higher in the intensity range.");
		CheckboxInt("Enable Hueshift RGB", s.hs_rgb_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Hue Shift RGB adds hue distortion to the red green and blue primary hue angles as intensity increases.\n"
				"By default OpenDRT will compress purity in a straight line in RGB/Chromaticity space.\n"
				"This can lead to perceived hue shifts due to the Abney Effect, for example a pure blue will perceptually shift towards purple as it desaturates.\n"
				"To compensate for this, and to use as a creative tool, this module allows creative control over the path that red green and blue hue angles take as their purity is compressed.");
		ImGui::SliderFloat("Hueshift R", &s.hs_r, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount to distort the red hue angle towards yellow as intensity increases.");
		ImGui::SliderFloat("Hueshift R Range", &s.hs_r_rng, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Range of the red hueshift: higher values affect more of the lower intensity range.");
		ImGui::SliderFloat("Hueshift G", &s.hs_g, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount to distort the green hue angle towards yellow as intensity increases.");
		ImGui::SliderFloat("Hueshift G Range", &s.hs_g_rng, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Range of the green hueshift: higher values affect more of the lower intensity range.");
		ImGui::SliderFloat("Hueshift B", &s.hs_b, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount to distort the blue hue angle towards cyan as intensity increases.");
		ImGui::SliderFloat("Hueshift B Range", &s.hs_b_rng, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Range of the blue hueshift: higher values affect more of the lower intensity range.");
		CheckboxInt("Enable Hueshift CMY", s.hs_cmy_enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Hue Shift CMY adds hue distortion to the cyan magenta and yellow secondary hue angles as intensity decreases.\n"
				"This module allows some very minimal adjustments of secondary hue angles as a creative tool.");
		ImGui::SliderFloat("Hueshift C", &s.hs_c, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount to distort the cyan hue angle towards blue as intensity decreases.");
		ImGui::SliderFloat("Hueshift C Range", &s.hs_c_rng, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Range of the cyan hueshift: higher values affect more of the upper intensity range.");
		ImGui::SliderFloat("Hueshift M", &s.hs_m, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount to distort the magenta hue angle towards blue as intensity decreases.");
		ImGui::SliderFloat("Hueshift M Range", &s.hs_m_rng, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Range of the magenta hueshift: higher values affect more of the upper intensity range.");
		ImGui::SliderFloat("Hueshift Y", &s.hs_y, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Amount to distort the yellow hue angle towards red as intensity decreases.");
		ImGui::SliderFloat("Hueshift Y Range", &s.hs_y_rng, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Range of the yellow hueshift: higher values affect more of the upper intensity range.");
	}
}