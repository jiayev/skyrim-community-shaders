#include "ColorGrading.h"

#include "State.h"
#include "Util.h"

#include "ColorSpace.h"
#include "Features/HDRDisplay.h"
#include "Features/LinearLighting.h"
#include "Features/PostProcessing.h"
#include "Menu.h"
#include "OpenDRTIo.h"

#include <DDSTextureLoader.h>
#include <DirectXPackedVector.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cmath>

#include "IconsFontAwesome5.h"

namespace
{
	using ColorSettings = ColorGrading::Settings;
	using Vec3Keys = std::array<std::string_view, 3>;
	using Vec4Keys = std::array<std::string_view, 4>;

	constexpr Vec3Keys RGB = { "r", "g", "b" };
	constexpr Vec3Keys SaturationBrightnessContrast = { "Saturation", "Brightness", "Contrast" };
	constexpr Vec3Keys ExposureTemperatureTint = { "Exposure", "Temperature", "Tint" };
	constexpr Vec3Keys SaturationVibranceHueShift = { "Saturation", "Vibrance", "Hue Shift" };
	constexpr Vec3Keys HueShiftVibranceBrightness = { "Hue Shift", "Vibrance", "Brightness" };
	constexpr Vec4Keys XYZW = { "x", "y", "z", "w" };
	constexpr Vec4Keys AllRGB = { "All", "R", "G", "B" };
	constexpr Vec4Keys ShadowHighlightRange = { "Shadows Start", "Shadows End", "Highlights Start", "Highlights End" };
	constexpr std::array<std::string_view, 7> ColorMixerHues = { "Reds", "Oranges", "Greens", "Cyans", "Blues", "Purples", "Magentas" };
	constexpr std::array<std::array<float, 4>, ColorMixerHues.size()> ColorMixerHueColors = { {
		{ 1.f, 0.f, 0.f, 1.f },
		{ 0.714f, 0.486f, 0.004f, 1.f },
		{ 0.341f, 0.624f, 0.f, 1.f },
		{ 0.f, 0.631f, 0.569f, 1.f },
		{ 0.f, 0.584f, 0.851f, 1.f },
		{ 0.522f, 0.392f, 1.f, 1.f },
		{ 1.f, 0.137f, 0.741f, 1.f },
	} };
	constexpr std::array<std::string_view, 2> TonemapParamGroups = { "Primary", "Secondary" };
	constexpr int AllRGBChannelCount = 3;
	constexpr float AllRGBAllSliderWeight = 2.f;
	constexpr float AllRGBChannelSliderWeight = 1.f;
	constexpr float AllRGBSliderWeight = AllRGBAllSliderWeight + AllRGBChannelSliderWeight * AllRGBChannelCount;
	constexpr float AllRGBDragSpeed = 1e-3f;
	constexpr float ColorMixerSwatchHeight = 22.f;
	constexpr float ColorMixerSwatchRounding = 4.f;
	constexpr float ColorMixerSwatchSpacing = 8.f;
	constexpr float SelectedSwatchThickness = 2.f;
	constexpr float ZeroAllNeutral = 0.f;
	constexpr float UnitAllNeutral = 1.f;
	constexpr float PivotAllNeutral = 0.18f;
	constexpr float MinExposureMultiplier = 1e-6f;
	constexpr auto kSkipLDRColorGrading = "Skip LDR Color Grading";
	constexpr auto kSkipLUT = "Skip LUT (Direct Color Grading)";
	constexpr auto kConvertLinearToLog = "Convert Linear to Log Before HDR Color Grading";
	constexpr auto kConvertLogToLinear = "Convert Log to Linear After HDR Color Grading";
	constexpr auto kLogType = "Log Type";
	constexpr auto kInputGamma = "Input Gamma";
	constexpr auto kOutputGamma = "Output Gamma";
	constexpr auto kOKLCHColorMixer = "OKLCH Color Mixer";
	constexpr auto kExposureTemperatureTint = "Exposure/Temperature/Tint";
	constexpr auto kAscCdl = "ASC CDL";
	constexpr auto kShadowsMidtonesHighlights = "Shadows/Midtones/Highlights";
	constexpr auto kShadowsMidtonesHighlightsRange = "Shadows/Midtones/Highlights Range";
	constexpr auto kContrast = "Contrast";
	constexpr auto kLiftGammaGain = "Lift Gamma Gain";
	constexpr auto kUseOpenDRT = "Use OpenDRT";
	constexpr auto kTonemapper = "Tonemapper";
	constexpr auto kTonemapperSettings = "Tonemapper Settings";
	constexpr auto kCinematicBlend = "Cinematic Blend";
	constexpr auto kFadeBlend = "Fade Blend";
	constexpr auto kTintBlend = "Tint Blend";
	constexpr auto kEnableTonemapping = "Enable Tonemapping";
	constexpr auto kWorkingColorSpace = "Working Color Space";

	struct Vec3Field
	{
		std::string_view key;
		float4 ColorSettings::* value;
		const Vec3Keys* keys;
	};

	struct Vec4Field
	{
		std::string_view key;
		float4 ColorSettings::* value;
		const Vec4Keys* keys;
	};

	struct AllRGBControl
	{
		std::string_view label;
		float4 ColorSettings::* value;
		float min;
		float max;
		const char* format;
	};

	enum class JsonValueScale
	{
		Linear,
		ExposureEV
	};

	template <class T>
	void ReadField(const json& source, std::string_view key, T& value)
	{
		if (auto it = source.find(std::string(key)); it != source.end())
			it->get_to(value);
	}

	template <size_t N>
	json Floats(const float* values, const std::array<std::string_view, N>& keys)
	{
		json out = json::object();
		for (size_t i = 0; i < N; ++i)
			out[std::string(keys[i])] = values[i];
		return out;
	}

	template <size_t N>
	void ReadFloats(const json& source, float* values, const std::array<std::string_view, N>& keys)
	{
		if (!source.is_object())
			return;
		for (size_t i = 0; i < N; ++i)
			ReadField(source, keys[i], values[i]);
	}

	template <size_t N>
	void ReadFloatsField(const json& source, std::string_view key, float* values, const std::array<std::string_view, N>& keys)
	{
		if (auto it = source.find(std::string(key)); it != source.end())
			ReadFloats(*it, values, keys);
	}

	json Vec3(const float3& value, const Vec3Keys& keys)
	{
		return Floats(&value.x, keys);
	}

	json Vec4(const float4& value, const Vec4Keys& keys = XYZW)
	{
		return Floats(&value.x, keys);
	}

	void ReadVec3Field(const json& source, std::string_view key, float3& value, const Vec3Keys& keys)
	{
		ReadFloatsField(source, key, &value.x, keys);
	}

	void ReadVec4(const json& source, float4& value)
	{
		ReadFloats(source, &value.x, XYZW);
	}

	template <class Array, size_t N, class ToJson>
	json NamedObjects(const Array& values, const std::array<std::string_view, N>& keys, ToJson&& toJson)
	{
		json out = json::object();
		for (size_t i = 0; i < values.size() && i < N; ++i)
			out[std::string(keys[i])] = toJson(values[i]);
		return out;
	}

	template <class Array, size_t N, class ReadJson>
	void ReadNamedObjects(const json& source, Array& values, const std::array<std::string_view, N>& keys, ReadJson&& readJson)
	{
		if (!source.is_object())
			return;
		for (size_t i = 0; i < values.size() && i < N; ++i) {
			if (auto it = source.find(std::string(keys[i])); it != source.end())
				readJson(*it, values[i]);
		}
	}

	constexpr std::array kVec3Fields = {
		Vec3Field{ "OKLCH Saturation", &ColorSettings::oklchSaturation, &SaturationVibranceHueShift },
	};

	constexpr std::array kAscCdlFields = {
		Vec4Field{ "Slope", &ColorSettings::slope, &AllRGB },
		Vec4Field{ "Power", &ColorSettings::power, &AllRGB },
		Vec4Field{ "Offset", &ColorSettings::cdlOffset, &AllRGB },
	};

	constexpr std::array kShadowsMidtonesHighlightsFields = {
		Vec4Field{ "Shadows Gain", &ColorSettings::shadowsGain, &AllRGB },
		Vec4Field{ "Shadows Offset", &ColorSettings::shadowsOffset, &AllRGB },
		Vec4Field{ "Midtones Gain", &ColorSettings::midtonesGain, &AllRGB },
		Vec4Field{ "Midtones Offset", &ColorSettings::midtonesOffset, &AllRGB },
		Vec4Field{ "Highlights Gain", &ColorSettings::highlightsGain, &AllRGB },
		Vec4Field{ "Highlights Offset", &ColorSettings::highlightsOffset, &AllRGB },
		Vec4Field{ kShadowsMidtonesHighlightsRange, &ColorSettings::shadowsHighlightsRange, &ShadowHighlightRange },
	};

	constexpr std::array kContrastFields = {
		Vec4Field{ kContrast, &ColorSettings::contrast, &AllRGB },
		Vec4Field{ "Pivot", &ColorSettings::pivot, &AllRGB },
	};

	constexpr std::array kLiftGammaGainFields = {
		Vec4Field{ "Lift", &ColorSettings::lift, &AllRGB },
		Vec4Field{ "Gamma", &ColorSettings::gamma, &AllRGB },
		Vec4Field{ "Gain", &ColorSettings::gain, &AllRGB },
	};

	constexpr std::array kAscCdlControls = {
		AllRGBControl{ "Slope", &ColorSettings::slope, 0.f, 2.f, "%.2f" },
		AllRGBControl{ "Power", &ColorSettings::power, 0.f, 2.f, "%.2f" },
		AllRGBControl{ "Offset", &ColorSettings::cdlOffset, -1.f, 1.f, "%.2f" },
	};

	constexpr std::array kShadowsMidtonesHighlightsControls = {
		AllRGBControl{ "Shadows Gain", &ColorSettings::shadowsGain, 0.f, 2.f, "%.3f" },
		AllRGBControl{ "Shadows Offset", &ColorSettings::shadowsOffset, -0.5f, 0.5f, "%.3f" },
		AllRGBControl{ "Midtones Gain", &ColorSettings::midtonesGain, 0.f, 2.f, "%.3f" },
		AllRGBControl{ "Midtones Offset", &ColorSettings::midtonesOffset, -0.5f, 0.5f, "%.3f" },
		AllRGBControl{ "Highlights Gain", &ColorSettings::highlightsGain, 0.f, 2.f, "%.3f" },
		AllRGBControl{ "Highlights Offset", &ColorSettings::highlightsOffset, -0.5f, 0.5f, "%.3f" },
	};

	constexpr std::array kContrastControls = {
		AllRGBControl{ kContrast, &ColorSettings::contrast, 0.f, 2.f, "%.3f" },
		AllRGBControl{ "Pivot", &ColorSettings::pivot, 0.f, 1.f, "%.3f" },
	};

	constexpr std::array kLiftGammaGainControls = {
		AllRGBControl{ "Lift", &ColorSettings::lift, -1.f, 1.f, "%.3f" },
		AllRGBControl{ "Gamma", &ColorSettings::gamma, -1.5f, 1.5f, "%.3f" },
		AllRGBControl{ "Gain", &ColorSettings::gain, 0.f, 2.f, "%.3f" },
	};

	void WriteVec3Fields(json& j, const ColorSettings& settings)
	{
		for (const auto& field : kVec3Fields) {
			const auto& value = settings.*field.value;
			j[std::string(field.key)] = Floats(&value.x, *field.keys);
		}
	}

	void ReadVec3Fields(const json& j, ColorSettings& settings)
	{
		for (const auto& field : kVec3Fields) {
			auto& value = settings.*field.value;
			ReadFloatsField(j, field.key, &value.x, *field.keys);
		}
	}

	template <size_t N>
	void WriteVec4Fields(json& j, const ColorSettings& settings, const std::array<Vec4Field, N>& fields)
	{
		for (const auto& field : fields) {
			const auto& value = settings.*field.value;
			j[std::string(field.key)] = Floats(&value.x, *field.keys);
		}
	}

	template <size_t N>
	void ReadVec4Fields(const json& j, ColorSettings& settings, const std::array<Vec4Field, N>& fields)
	{
		for (const auto& field : fields) {
			auto& value = settings.*field.value;
			ReadFloatsField(j, field.key, &value.x, *field.keys);
		}
	}

	template <size_t N>
	void WriteVec4Group(json& j, std::string_view group, const ColorSettings& settings, const std::array<Vec4Field, N>& fields)
	{
		auto& node = j[std::string(group)] = json::object();
		WriteVec4Fields(node, settings, fields);
	}

	template <size_t N>
	void ReadVec4Group(const json& j, std::string_view group, ColorSettings& settings, const std::array<Vec4Field, N>& fields)
	{
		if (auto it = j.find(std::string(group)); it != j.end() && it->is_object())
			ReadVec4Fields(*it, settings, fields);
	}

	float ToJsonValue(JsonValueScale scale, float value)
	{
		switch (scale) {
		case JsonValueScale::ExposureEV:
			return std::log2(std::max(value, MinExposureMultiplier));
		default:
			return value;
		}
	}

	float FromJsonValue(JsonValueScale scale, float value)
	{
		switch (scale) {
		case JsonValueScale::ExposureEV:
			return std::exp2(value);
		default:
			return value;
		}
	}

	json ExposureTemperatureTintJson(const float4& value)
	{
		return {
			{ std::string(ExposureTemperatureTint[0]), ToJsonValue(JsonValueScale::ExposureEV, value.x) },
			{ std::string(ExposureTemperatureTint[1]), value.y },
			{ std::string(ExposureTemperatureTint[2]), value.z }
		};
	}

	void ReadExposureTemperatureTint(const json& source, float4& value)
	{
		if (auto it = source.find(kExposureTemperatureTint); it != source.end() && it->is_object()) {
			for (size_t i = 0; i < ExposureTemperatureTint.size(); ++i) {
				if (auto field = it->find(std::string(ExposureTemperatureTint[i])); field != it->end())
					(&value.x)[i] = FromJsonValue(i == 0 ? JsonValueScale::ExposureEV : JsonValueScale::Linear, field->get<float>());
			}
		}
	}

	float4 ApplyAllRGB(const float4& value, float neutral)
	{
		const float commonOffset = value.x - neutral;
		return { value.y + commonOffset, value.z + commonOffset, value.w + commonOffset, value.x };
	}

	float4 ApplyUnitAllRGB(const float4& value)
	{
		return ApplyAllRGB(value, UnitAllNeutral);
	}

	float4 ApplyZeroAllRGB(const float4& value)
	{
		return ApplyAllRGB(value, ZeroAllNeutral);
	}

	float4 PackLiftGammaGainAllRGB(const float4& value, float neutral)
	{
		const auto applied = ApplyAllRGB(value, neutral);
		return { applied.w, applied.x, applied.y, applied.z };
	}

	float GetAllSliderWidth()
	{
		const auto& style = ImGui::GetStyle();
		const float spacingWidth = style.ItemSpacing.x + style.ItemInnerSpacing.x * static_cast<float>(AllRGBChannelCount - 1);
		const float availableWidth = ImGui::CalcItemWidth() - spacingWidth;
		return std::max(availableWidth * AllRGBAllSliderWeight / AllRGBSliderWeight, ImGui::GetFrameHeight());
	}

	void DrawAllRGB(ColorSettings& settings, const AllRGBControl& control)
	{
		auto& value = settings.*control.value;
		ImGui::PushID(control.label.data());

		const auto& style = ImGui::GetStyle();
		const float allWidth = GetAllSliderWidth();
		const float rgbWidth = ImGui::CalcItemWidth() - allWidth - style.ItemSpacing.x;

		ImGui::SetNextItemWidth(allWidth);
		ImGui::SliderFloat("##All", &value.x, control.min, control.max, control.format);
		ImGui::SameLine(0.f, style.ItemSpacing.x);
		ImGui::SetNextItemWidth(rgbWidth);
		Util::RGBFloatDrag3("##RGB", &value.y, AllRGBDragSpeed, control.min, control.max, control.format);
		ImGui::SameLine();
		ImGui::TextUnformatted(control.label.data());

		ImGui::PopID();
	}

	template <size_t N>
	void DrawAllRGBControls(ColorSettings& settings, const std::array<AllRGBControl, N>& controls)
	{
		for (const auto& control : controls)
			DrawAllRGB(settings, control);
	}

	float Scaled(float value)
	{
		return value * Util::GetUIScale();
	}

	ImVec2 ColorMixerSwatchSize()
	{
		const float spacingWidth = Scaled(ColorMixerSwatchSpacing) * static_cast<float>(ColorMixerHues.size() - 1);
		const float width = (ImGui::CalcItemWidth() - spacingWidth) / static_cast<float>(ColorMixerHues.size());
		return { std::max(width, Scaled(ColorMixerSwatchHeight)), Scaled(ColorMixerSwatchHeight) };
	}

	void DrawColorMixerSwatch(size_t index, int& hueId, const ImVec2& swatchSize)
	{
		const auto& value = ColorMixerHueColors[index];
		const ImVec4 color{ value[0], value[1], value[2], value[3] };
		const float rounding = Scaled(ColorMixerSwatchRounding);

		ImGui::PushID(static_cast<int>(index));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
		if (ImGui::ColorButton("##Hue", color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, swatchSize))
			hueId = static_cast<int>(index);
		ImGui::PopStyleVar();

		if (hueId == static_cast<int>(index)) {
			auto* drawList = ImGui::GetWindowDrawList();
			drawList->AddRect(
				ImGui::GetItemRectMin(),
				ImGui::GetItemRectMax(),
				ImGui::GetColorU32(ImGuiCol_CheckMark),
				rounding,
				0,
				Scaled(SelectedSwatchThickness));
		}

		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(ColorMixerHues[index].data());
		ImGui::PopID();
	}

	void DrawColorMixerSelectors(int& hueId)
	{
		const ImVec2 swatchSize = ColorMixerSwatchSize();
		const float swatchSpacing = Scaled(ColorMixerSwatchSpacing);

		for (size_t i = 0; i < ColorMixerHues.size(); i++) {
			if (i != 0)
				ImGui::SameLine(0.f, swatchSpacing);
			DrawColorMixerSwatch(i, hueId, swatchSize);
		}
	}
}

void to_json(json& j, const ColorGrading::Settings& settings)
{
	j = {
		{ kSkipLDRColorGrading, settings.skipLDR },
		{ kSkipLUT, settings.skipLUT },
		{ kConvertLinearToLog, settings.useLog },
		{ kLogType, settings.logType },
		{ kConvertLogToLinear, settings.invertLog },
		{ kInputGamma, settings.inOutGamma.z },
		{ kOutputGamma, settings.inOutGamma.w },
		{ kOKLCHColorMixer, NamedObjects(settings.oklchColorMixer, ColorMixerHues, [](const float4& value) {
			 return Floats(&value.x, HueShiftVibranceBrightness);
		 }) },
		{ kUseOpenDRT, settings.useOpenDrt },
		{ kTonemapper, settings.currentTonemapper },
		{ kTonemapperSettings, NamedObjects(settings.tonemapParams, TonemapParamGroups, [](const float4& value) {
			 return Vec4(value);
		}) },
		{ kCinematicBlend, Vec3(settings.gameCinematicBlend, SaturationBrightnessContrast) },
		{ kFadeBlend, settings.gameFadeBlend },
		{ kTintBlend, settings.gameTintBlend },
		{ kEnableTonemapping, settings.enableTonemap },
		{ kWorkingColorSpace, settings.processColorSpace }
	};

	WriteVec3Fields(j, settings);
	j[kExposureTemperatureTint] = ExposureTemperatureTintJson(settings.exposureTemperatureTint);
	WriteVec4Group(j, kAscCdl, settings, kAscCdlFields);
	WriteVec4Group(j, kShadowsMidtonesHighlights, settings, kShadowsMidtonesHighlightsFields);
	WriteVec4Group(j, kContrast, settings, kContrastFields);
	WriteVec4Group(j, kLiftGammaGain, settings, kLiftGammaGainFields);
}

void from_json(const json& j, ColorGrading::Settings& settings)
{
	settings = {};
	ReadField(j, kSkipLDRColorGrading, settings.skipLDR);
	ReadField(j, kSkipLUT, settings.skipLUT);
	ReadField(j, kInputGamma, settings.inOutGamma.z);
	ReadField(j, kOutputGamma, settings.inOutGamma.w);
	ReadVec3Fields(j, settings);
	ReadExposureTemperatureTint(j, settings.exposureTemperatureTint);
	ReadVec4Group(j, kAscCdl, settings, kAscCdlFields);
	ReadVec4Group(j, kShadowsMidtonesHighlights, settings, kShadowsMidtonesHighlightsFields);
	ReadVec4Group(j, kContrast, settings, kContrastFields);
	ReadVec4Group(j, kLiftGammaGain, settings, kLiftGammaGainFields);
	if (auto it = j.find(kOKLCHColorMixer); it != j.end())
		ReadNamedObjects(*it, settings.oklchColorMixer, ColorMixerHues, [](const json& source, float4& value) {
			ReadFloats(source, &value.x, HueShiftVibranceBrightness);
		});
	ReadField(j, kUseOpenDRT, settings.useOpenDrt);
	ReadField(j, kTonemapper, settings.currentTonemapper);
	if (auto it = j.find(kTonemapperSettings); it != j.end())
		ReadNamedObjects(*it, settings.tonemapParams, TonemapParamGroups, [](const json& source, float4& value) {
			ReadVec4(source, value);
		});
	ReadVec3Field(j, kCinematicBlend, settings.gameCinematicBlend, SaturationBrightnessContrast);
	ReadField(j, kFadeBlend, settings.gameFadeBlend);
	ReadField(j, kTintBlend, settings.gameTintBlend);
	ReadField(j, kConvertLinearToLog, settings.useLog);
	ReadField(j, kLogType, settings.logType);
	ReadField(j, kConvertLogToLinear, settings.invertLog);
	ReadField(j, kEnableTonemapping, settings.enableTonemap);
	ReadField(j, kWorkingColorSpace, settings.processColorSpace);
}

template <int num = 1>
bool exposureSlider(float* val)
{
	float tempVal[num];
	for (int i = 0; i < num; i++)
		tempVal[i] = log2(val[i]);

	bool retval;
	if constexpr (num == 1)
		retval = ImGui::SliderFloat("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 2)
		retval = Util::ShiftSlider<2>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 3)
		retval = Util::ShiftSlider<3>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 4)
		retval = Util::ShiftSlider<4>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");

	for (int i = 0; i < num; i++)
		val[i] = exp2(tempVal[i]);

	return retval;
}

void drawHDRStatus()
{
	auto& hdr = globals::features::hdrDisplay;
	if (hdr.loaded && hdr.settings.enableHDR) {
		ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), ICON_FA_CHECK " HDR Output Active");
		ImGui::Text("Paper White: %.0f nits (from HDR settings)", static_cast<float>(hdr.settings.hdrPaperWhite));
		ImGui::Text("Peak Brightness: %.0f nits (from HDR settings)", static_cast<float>(hdr.settings.hdrPeakNits));
	} else {
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "SDR Output (HDR Display not enabled)");
	}
}

// Profjack Design
struct TonemapperInfo
{
	std::string_view name;
	std::string_view func_name;
	std::string_view desc;
	int nativeInputSpace;      // color space the tonemapper expects as input
	int nativeOutputSpace;     // color space the tonemapper produces as output
	bool supportsHDR;          // whether this tonemapper supports HDR output
	int nativeInputSpaceHDR;   // input color space index when HDR is active
	int nativeOutputSpaceHDR;  // output color space index when HDR is active

	using CTP = std::array<float4, 2>;
	std::function<void(CTP&)> draw_settings_func;
	CTP default_settings;

	CTP cached_settings;

	static auto& GetTonemappers()
	{
		using f4 = float4;
		constexpr auto shiftHint = []() { ImGui::TextWrapped("Press Shift to control all channels at the same time."); };

		static std::vector<TonemapperInfo> tonemappers = {
			{ "Reinhard"sv, "Reinhard"sv,
				"Mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002."sv, 0, 0, false, 0, 0,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Reinhard Extended"sv, "ReinhardExt"sv,
				"Extended mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002. "
				"An additional user parameter specifies the smallest luminance that is mapped to 1, which allows high luminances to burn out."sv,
				0, 0, false, 0, 0,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("White Point", &params[0].y, 0.f, 10.f, "%.2f"); },
				{ f4{ 1.f, 2.f, 0.f, 0.f } } },

			{ "Hejl Burgess-Dawson Filmic"sv, "HejlBurgessDawsonFilmic"sv,
				"Variation of the Hejl and Burgess-Dawson filmic curve done by Graham Aldridge. "
				"See his blog post about \"Approximating Film with Tonemapping\"."sv,
				0, 0, false, 0, 0,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Aldridge Filmic"sv, "AldridgeFilmic"sv,
				"Variation of the Hejl and Burgess-Dawson filmic curve done by Graham Aldridge. "
				"See his blog post about \"Approximating Film with Tonemapping\"."sv,
				0, 0, false, 0, 0,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Cutoff", &params[0].y, 0.f, .5f, "%.2f"); },
				{ f4{ 1.f, .19f, 0.f, 0.f } } },

			{ "Lottes Filmic/AMD Curve"sv, "LottesFilmic"sv,
				"Filmic curve by Timothy Lottes, described in his GDC talk \"Advanced Techniques and Optimization of HDR Color Pipelines\". "
				"Also known as the \"AMD curve\"."sv,
				0, 0, true, 0, 0,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Contrast", &params[0].y, 1.f, 2.f, "%.2f");
					ImGui::SliderFloat("Shoulder", &params[0].z, 0.01f, 2.f, "%.2f");
					ImGui::SliderFloat("Maximum HDR Value", &params[0].w, 1.f, 10.f, "%.2f");
					ImGui::SliderFloat("Input Mid-Level", &params[1].x, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Output Mid-Level", &params[1].y, 0.f, 1.f, "%.2f");
					drawHDRStatus(); },
				{ f4{ 1.f, 1.6f, 0.977f, 8.f }, f4{ 0.18f, 0.267f, 0.f, 0.f } } },

			{ "Day Filmic/Insomniac Curve"sv, "DayFilmic"sv,
				"Filmic curve by Mike Day, described in his document \"An efficient and user-friendly tone mapping operator\". "
				"Also known as the \"Insomniac curve\"."sv,
				0, 0, false, 0, 0,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Black Point", &params[0].y, 0.f, 5.f, "%.2f");
					ImGui::SliderFloat("White Point", &params[0].z, 0.f, 5.f, "%.2f");

					ImGui::SliderFloat("Cross-over Point", &params[0].w, 0.f, 5.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Point where the toe and shoulder are pieced together into a single curve.");
					ImGui::SliderFloat("Shoulder Strength", &params[1].x, 0.f, 1.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the shoulder.");
					ImGui::SliderFloat("Toe Strength", &params[1].y, 0.f, 1.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the toe."); },
				{ f4{ 1.f, 0.f, 2.f, 0.3f }, f4{ 0.8f, 0.7f, 0.f, 0.f } } },

			{ "Uchimura/Grand Turismo Curve"sv, "UchimuraFilmic"sv,
				"Filmic curve by Hajime Uchimura, described in his CEDEC talk \"HDR Theory and Practice\". Characterised by its middle linear section. "
				"Also known as the \"Gran Turismo curve\"."sv,
				0, 0, true, 0, 0,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Max Brightness", &params[0].y, 0.01f, 2.f, "%.2f");
					ImGui::SliderFloat("Contrast", &params[0].z, 0.f, 5.f, "%.2f");
					ImGui::SliderFloat("Linear Section Start", &params[0].w, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Linear Section Length", &params[1].x, .01f, .99f, "%.2f");
					ImGui::SliderFloat("Black Tightness Shape", &params[1].y, 1.f, 3.f, "%.2f");
					ImGui::SliderFloat("Black Tightness Offset", &params[1].z, 0.f, 1.f, "%.2f");
					drawHDRStatus(); },
				{ f4{ 1.f, 1.f, 1.f, .22f }, f4{ 0.4f, 1.33f, 0.f, 0.f } } },

			{ "AgX Minimal"sv, "AgxMinimal"sv,
				"Minimal version of Troy Sobotka's AgX using a 6th order polynomial approximation. "
				"Originally created by bwrensch, and improved by Troy Sobotka. Internally uses AgX input transform."sv,
				0, 0, false, 0, 0,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Slope", &params[0].y, 0.f, 2.f, "%.2f");
					ImGui::SliderFloat("Power", &params[0].z, 0.f, 2.f, "%.2f");
					ImGui::SliderFloat("Offset", &params[0].w, -1.f, 1.f, "%.2f");
					ImGui::SliderFloat("Saturation", &params[1].x, 0.f, 2.f, "%.2f"); },
				{ f4{ 1.f, 1.f, 1.f, 0.f }, f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Melon"sv, "MelonTonemap"sv,
				"Tonemapper designed by TripleMelon to fix the ACES issue of intense colour being shifted."sv, 0, 0, false, 0, 0,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Kajiya"sv, "KajiyaTonemap"sv,
				"Tonemapper designed by Tomasz Stachowiak/Embark for their real time ray tracing engine Kajiya."sv, 0, 0, false, 0, 0,
				[](CTP& params) { exposureSlider(&params[0].x); },
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "GT7"sv, "GT7ToneMapping"sv,
				"Tonemapper designed for Gran Turismo 7."sv, 2, 2, true, 2, 2,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					drawHDRStatus();
				},
				{ f4{ 1.f, 0.f, 1000.f, 0.f } } },

			{ "PsychoV"sv, "PsychoVTonemap"sv,
				"PsychoV 17 tonemapper by Carlos Lopez, from RenoDX."sv,
				0, 0, true, 0, 0,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					drawHDRStatus();
				},
				{ f4{ 1.f, 0.f, 0.f, 0.f } } },

			{ "Neutwo"sv, "NeutwoTonemap"sv,
				"Neutwo tonemapper by Carlos Lopez, from RenoDX."sv,
				0, 0, true, 2, 2,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Clip Point", &params[0].y, 1.f, 100.f, "%.2f");
					drawHDRStatus();
				},
				{ f4{ 1.f, 100.f, 0.f, 0.f } } },

			{ "ACES"sv, "ACESTonemap"sv,
				"ACES RRT+ODT tonemapper implementation from RenoDX."sv,
				0, 0, true, 2, 2,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Min Luminance", &params[0].y, 0.0001f, 1.f, "%.4f");
					drawHDRStatus();
				},
				{ f4{ 1.f, 0.0001f, 0.f, 0.f } } },

			{ "Frostbite"sv, "FrostbiteTonemap"sv,
				"Frostbite HDR display mapping implementation from RenoDX, based on EA's Frostbite color grading and display presentation work."sv,
				0, 0, true, 2, 2,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("Rolloff Start", &params[0].y, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Saturation Boost", &params[0].z, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Hue Correction", &params[0].w, 0.f, 1.f, "%.2f");
					drawHDRStatus();
				},
				{ f4{ 1.f, 0.25f, 0.3f, 0.6f } } },

			{ "Hermite Spline"sv, "HermiteSplineTonemap"sv,
				"Hermite spline tonemapper by Musa, from RenoDX."sv,
				0, 0, true, 2, 2,
				[](CTP& params) {
					exposureSlider(&params[0].x);
					ImGui::SliderFloat("White Clip", &params[0].y, 1.f, 500.f, "%.2f");
					drawHDRStatus();
				},
				{ f4{ 1.f, 100.f, 0.f, 0.f } } }
		};

		static std::once_flag flag;
		std::call_once(flag,
			[&]() {
				for (auto& t : tonemappers)
					t.cached_settings = t.default_settings;
			});

		return tonemappers;
	}

	static void GetDefaultParams(int& tonemapperType, CTP& params)
	{
		auto& tonemappers = GetTonemappers();
		if (auto it = std::ranges::find_if(tonemappers, [&](TonemapperInfo& x) { return "GT7"sv == x.name; });
			it != tonemappers.end()) {
			tonemapperType = (int)(it - tonemappers.begin());
			params = it->default_settings;
		} else
			logger::error("Somehow, the default settings are invalid. Please contact the author.");
	}
};

void ColorGrading::DrawSettings()
{
	ImGui::Checkbox("Skip LDR Color Grading", &settings.skipLDR);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Skip color grading after tonemapping. This includes Lift Gamma Gain. Will be automatically skipped with HDR on.");

	ImGui::Checkbox("Skip LUT (Direct Color Grading)", &settings.skipLUT);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Skip baking color grading into a LUT and apply it directly per-pixel. More accurate but slower.");

	ImGui::Checkbox("Convert Linear to Log Before HDR Color Grading", &settings.useLog);
	if (settings.useLog) {
		ImGui::Checkbox("Convert Log to Linear After HDR Color Grading", &settings.invertLog);
		ImGui::Combo("Log Type", (int*)&settings.logType, "ACEScct\0ARRILogC4\0SonySLog3\0");
	}

	ImGui::SeparatorText("Color Grading");
	{
		ImGui::SliderFloat("Input Gamma", &settings.inOutGamma.z, 0.f, 3.f, "%.3f");
		ImGui::SliderFloat("Output Gamma", &settings.inOutGamma.w, 0.f, 3.f, "%.3f");

		ImGui::Text("Pre-Tonemapping Settings");
		if (ImGui::TreeNode("Exposure/Temperature/Tint")) {
			exposureSlider(&settings.exposureTemperatureTint.x);
			ImGui::SliderFloat("Temperature", &settings.exposureTemperatureTint.y, 10.f, 150.f, "%1.f00K");
			ImGui::SliderFloat("Tint", &settings.exposureTemperatureTint.z, -1.f, 1.f, "%.3f");
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("ASC CDL")) {
			DrawAllRGBControls(settings, kAscCdlControls);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("OKLCH Saturation")) {
			ImGui::SliderFloat("Saturation", &settings.oklchSaturation.x, 0.f, 2.f, "%.3f");
			ImGui::SliderFloat("Vibrance", &settings.oklchSaturation.y, 0.f, 3.f, "%.3f");
			ImGui::SliderFloat("Hue Shift", &settings.oklchSaturation.z, -1.f, 1.f, "%.3f");
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("OKLCH Color Mixer")) {
			ImGui::Text("Adjust brightness, vibrance and hue shift of specific hues in the perceptually uniform OKLCH space.");
			static int hueId = 0;
			hueId = std::clamp(hueId, 0, static_cast<int>(ColorMixerHues.size()) - 1);
			DrawColorMixerSelectors(hueId);
			ImGui::SliderFloat("Hue Shift", &settings.oklchColorMixer[hueId].x, -1.f, 1.f, "%.3f");
			ImGui::SliderFloat("Vibrance", &settings.oklchColorMixer[hueId].y, 0.f, 3.f, "%.3f");
			ImGui::SliderFloat("Brightness", &settings.oklchColorMixer[hueId].z, -1.f, 1.f, "%.3f");
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Shadows/Midtones/Highlights")) {
			DrawAllRGBControls(settings, kShadowsMidtonesHighlightsControls);
			ImGui::InputFloat2("Shadows Start/End", &settings.shadowsHighlightsRange.x, "%.3f");
			ImGui::InputFloat2("Highlights Start/End", &settings.shadowsHighlightsRange.z, "%.3f");
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Contrast")) {
			DrawAllRGBControls(settings, kContrastControls);
			ImGui::TreePop();
		}

		ImGui::Text("Post-Tonemapping Settings");
		if (ImGui::TreeNode("Lift Gamma Gain")) {
			DrawAllRGBControls(settings, kLiftGammaGainControls);
			ImGui::TreePop();
		}
	}

	ImGui::SeparatorText("Tonemapping");
	ImGui::Checkbox("Enable Tonemapping", &settings.enableTonemap);
	if (settings.enableTonemap) {
		auto& hdrRef = globals::features::hdrDisplay;
		const bool hdrActive = hdrRef.loaded && hdrRef.settings.enableHDR;

		if (ImGui::Checkbox("Use OpenDRT", &settings.useOpenDrt))
			recompileFlag = true;

		if (settings.useOpenDrt) {
			ImGui::PushID("OpenDRT");
			OpenDRTDrawSettings(
				settings.odrtConfig,
				hdrActive,
				static_cast<float>(hdrRef.settings.hdrPaperWhite),
				static_cast<float>(hdrRef.settings.hdrPeakNits));
			ImGui::PopID();
		} else {
			auto& tonemappers = TonemapperInfo::GetTonemappers();

			if (ImGui::BeginCombo("Tonemapper", tonemappers[tonemapperType].name.data(), ImGuiComboFlags_HeightLargest)) {
				for (int i = 0; i < (int)tonemappers.size(); ++i) {
					// Hide non-HDR tonemappers when HDR is active
					if (hdrActive && !tonemappers[i].supportsHDR)
						continue;

					if (ImGui::Selectable(tonemappers[i].name.data(), i == tonemapperType)) {
						tonemappers[tonemapperType].cached_settings = settings.tonemapParams;
						settings.tonemapParams = tonemappers[i].cached_settings;
						tonemapperType = i;
						settings.currentTonemapper = tonemappers[tonemapperType].name.data();
						recompileFlag = true;
					}

					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text(tonemappers[i].desc.data());
				}
				ImGui::EndCombo();
			}
			ImGui::Spacing();
			ImGui::TextWrapped(tonemappers[tonemapperType].desc.data());
			ImGui::Spacing();
			if (ImGui::Button("Reset", { -1, 0 }))
				settings.tonemapParams = tonemappers[tonemapperType].default_settings;
			ImGui::Spacing();

			ImGui::PushID(tonemapperType);
			tonemappers[tonemapperType].draw_settings_func(settings.tonemapParams);
			ImGui::PopID();
		}

		// Tonemapping curve visualization (GPU-evaluated, RGB overlay)
		if (ImGui::TreeNode("Curve Preview")) {
			curveReadbackRequested = true;
			curveReadbackRequestFrame = ImGui::GetFrameCount();

			if (settings.skipLUT) {
				ImGui::TextDisabled("Enable LUT generation to see curve preview (uncheck 'Skip LUT')");
			} else {
				// Determine Y-axis max from data
				float yMax = 1.f;
				if (hdrActive) {
					for (int i = 0; i < CurveSamples; i++) {
						yMax = std::max({ yMax, curveR[i], curveG[i], curveB[i] });
					}
					yMax = std::ceil(yMax * 2.f) / 2.f;  // round up to nearest 0.5
					yMax = std::max(yMax, 1.f);
				}

				// Plot area
				const float uiScale = Util::GetUIScale();
				constexpr float kCurvePreviewHeight = 180.f;
				const float lineThickness = uiScale;
				const float curveThickness = 1.5f * uiScale;
				float plotW = ImGui::GetContentRegionAvail().x;
				float plotH = kCurvePreviewHeight * uiScale;
				ImVec2 canvasPos = ImGui::GetCursorScreenPos();
				ImVec2 canvasSize = { plotW, plotH };
				ImGui::InvisibleButton("##curve_canvas", canvasSize);
				bool hovered = ImGui::IsItemHovered();

				auto* dl = ImGui::GetWindowDrawList();

				// Background
				dl->AddRectFilled(canvasPos, { canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y }, IM_COL32(20, 20, 20, 255));
				dl->AddRect(canvasPos, { canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y }, IM_COL32(80, 80, 80, 255));

				// Grid lines
				auto gridColor = IM_COL32(50, 50, 50, 255);
				for (int g = 1; g <= 3; g++) {
					float gy = canvasPos.y + canvasSize.y * (1.f - (float)g / 4.f);
					dl->AddLine({ canvasPos.x, gy }, { canvasPos.x + canvasSize.x, gy }, gridColor, lineThickness);
				}
				// Vertical grid at input = 1.0
				{
					float gx = canvasPos.x + canvasSize.x * (1.f / CurveMaxInput);
					dl->AddLine({ gx, canvasPos.y }, { gx, canvasPos.y + canvasSize.y }, gridColor, lineThickness);
				}

				// Identity line (input = output, clamped to plot range)
				{
					float identityEndX = std::min(1.f, yMax) / CurveMaxInput;  // where identity line hits yMax
					float x0 = canvasPos.x;
					float y0 = canvasPos.y + canvasSize.y;
					float x1 = canvasPos.x + canvasSize.x * identityEndX;
					float y1 = canvasPos.y + canvasSize.y * (1.f - std::min(1.f, yMax) / yMax);
					dl->AddLine({ x0, y0 }, { x1, y1 }, IM_COL32(80, 80, 80, 128), lineThickness);
				}

				// Draw RGB curves
				auto drawCurve = [&](const std::array<float, CurveSamples>& data, ImU32 color) {
					for (int i = 0; i < CurveSamples - 1; i++) {
						float x0 = canvasPos.x + canvasSize.x * ((float)i / (CurveSamples - 1));
						float x1 = canvasPos.x + canvasSize.x * ((float)(i + 1) / (CurveSamples - 1));
						float y0 = canvasPos.y + canvasSize.y * (1.f - std::clamp(data[i] / yMax, 0.f, 1.f));
						float y1 = canvasPos.y + canvasSize.y * (1.f - std::clamp(data[i + 1] / yMax, 0.f, 1.f));
						dl->AddLine({ x0, y0 }, { x1, y1 }, color, curveThickness);
					}
				};

				drawCurve(curveR, IM_COL32(220, 60, 60, 255));
				drawCurve(curveG, IM_COL32(60, 200, 60, 255));
				drawCurve(curveB, IM_COL32(80, 80, 240, 255));

				// Hover tooltip with Pre/Post values
				if (hovered) {
					ImVec2 mousePos = ImGui::GetMousePos();
					float t = std::clamp((mousePos.x - canvasPos.x) / canvasSize.x, 0.f, 1.f);
					int idx = std::clamp((int)(t * (CurveSamples - 1)), 0, CurveSamples - 1);
					float preValue = t * CurveMaxInput;

					// Vertical cursor line
					dl->AddLine({ mousePos.x, canvasPos.y }, { mousePos.x, canvasPos.y + canvasSize.y }, IM_COL32(200, 200, 200, 100), lineThickness);

					ImGui::BeginTooltip();
					ImGui::Text("Pre:  %.3f", preValue);
					ImGui::TextColored(ImVec4(0.9f, 0.25f, 0.25f, 1), "Post R: %.3f", curveR[idx]);
					ImGui::TextColored(ImVec4(0.25f, 0.8f, 0.25f, 1), "Post G: %.3f", curveG[idx]);
					ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.95f, 1), "Post B: %.3f", curveB[idx]);
					ImGui::EndTooltip();
				}

				// Axis labels
				ImGui::TextDisabled("Pre: 0 - %.1f (HDR linear)  |  Post: 0 - %.1f%s", CurveMaxInput, yMax, hdrActive ? " (HDR)" : "");
			}
			ImGui::TreePop();
		} else {
			curveReadbackRequested = false;
		}
	}

	ImGui::SeparatorText("Game Color Grading");
	ImGui::SliderFloat3("Cinematic Blend", &settings.gameCinematicBlend.x, 0.f, 1.f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Saturation, Brightness and Contrast.");
	ImGui::SliderFloat("Fade Blend", &settings.gameFadeBlend, 0.f, 1.f, "%.3f");
	ImGui::SliderFloat("Tint Blend", &settings.gameTintBlend, 0.f, 1.f, "%.3f");
	ImGui::SeparatorText("Color Space Transform");
	{
		auto& spaces = getAvailableColorSpaces();
		auto& hdr = globals::features::hdrDisplay;
		const bool hdrEnabled = hdr.loaded && hdr.settings.enableHDR;

		constexpr int kHDRColorSpace = 2;  // BT2020
		constexpr int kSDRColorSpace = 0;  // sRGB / BT709 gamut
		const int outputColorSpace = hdrEnabled ? kHDRColorSpace : kSDRColorSpace;

		auto& llSettings = globals::features::linearLighting.settings;
		const bool wideGamutActive = llSettings.enableACEScg && llSettings.enableLinearLighting;
		const char* inputSpaceName = wideGamutActive ? spaces[5] : spaces[0];
		ImGui::TextDisabled("Input Color Space: %s (%s)", inputSpaceName, wideGamutActive ? "auto-detected from Linear Lighting ACEScg" : "fixed");
		ImGui::Combo("Working Color Space", &settings.processColorSpace, spaces.data(), (int)spaces.size());
		ImGui::TextDisabled("Output Color Space: %s (auto from HDR Display)", spaces[outputColorSpace]);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Output switches automatically: SDR -> sRGB, HDR -> BT2020.");

		UpdateColorSpaceTransforms(hdrEnabled);
	}

	if (ImGui::Button("Save LUT and Output Image")) {
		saveImagesFlag = true;
	}
	ImGui::SameLine();
	ImGui::Text("Output will be saved to: %s", outputPath.c_str());
}

void ColorGrading::RestoreDefaultSettings()
{
	settings = {};
	TonemapperInfo::GetDefaultParams(tonemapperType, settings.tonemapParams);
	settings.currentTonemapper = TonemapperInfo::GetTonemappers()[tonemapperType].name.data();
	recompileFlag = true;
}

void ColorGrading::ResolveTonemapperFromSettings()
{
	auto& tonemappers = TonemapperInfo::GetTonemappers();
	if (auto it = std::ranges::find_if(tonemappers, [&](TonemapperInfo& x) { return settings.currentTonemapper == x.name; });
		it != tonemappers.end()) {
		tonemapperType = static_cast<int>(it - tonemappers.begin());
		return;
	}

	TonemapperInfo::GetDefaultParams(tonemapperType, settings.tonemapParams);
	settings.currentTonemapper = tonemappers[tonemapperType].name.data();
}

void ColorGrading::LoadSettings(json& o_json)
{
	const bool oldUseOpenDrt = settings.useOpenDrt;
	const int oldTonemapperType = tonemapperType;

	try {
		settings = o_json;
		auto& spaces = getAvailableColorSpaces();
		settings.processColorSpace = std::clamp(settings.processColorSpace, 0, static_cast<int>(spaces.size()) - 1);
		ResolveTonemapperFromSettings();
	} catch (const json::exception& e) {
		logger::error("Failed to load Color Grading settings: {}", e.what());
		RestoreDefaultSettings();
		return;
	}

	try {
		auto part1 = o_json["ODRT1"].get<OpenDRTSettingsPart1>();
		auto part2 = o_json["ODRT2"].get<OpenDRTSettingsPart2>();
		std::memcpy(&settings.odrtConfig, &part1, sizeof(OpenDRTSettingsPart1));
		std::memcpy(
			reinterpret_cast<char*>(&settings.odrtConfig) + sizeof(OpenDRTSettingsPart1),
			&part2,
			sizeof(OpenDRTSettingsPart2));
	} catch (const json::exception&) {
		settings.odrtConfig = {};
	}

	recompileFlag = recompileFlag || settings.useOpenDrt != oldUseOpenDrt || tonemapperType != oldTonemapperType;
}

void ColorGrading::SaveSettings(json& o_json)
{
	auto& tonemappers = TonemapperInfo::GetTonemappers();
	settings.currentTonemapper = tonemappers[tonemapperType].name.data();
	o_json = settings;

	OpenDRTSettingsPart1 part1;
	OpenDRTSettingsPart2 part2;
	std::memcpy(&part1, &settings.odrtConfig, sizeof(part1));
	std::memcpy(
		&part2,
		reinterpret_cast<const char*>(&settings.odrtConfig) + sizeof(part1),
		sizeof(part2));
	o_json["ODRT1"] = part1;
	o_json["ODRT2"] = part2;
}

void ColorGrading::UpdateColorSpaceTransforms(bool hdrEnabled)
{
	auto& spaces = getAvailableColorSpaces();
	settings.processColorSpace = std::clamp(settings.processColorSpace, 0, static_cast<int>(spaces.size()) - 1);

	auto& tonemappers = TonemapperInfo::GetTonemappers();

	// Auto-detect input color space: ACEScg when wide gamut mode is active, otherwise sRGB
	auto& llSettings = globals::features::linearLighting.settings;
	const bool wideGamutActive = llSettings.enableACEScg && llSettings.enableLinearLighting;
	const int kInputColorSpace = wideGamutActive ? 5 : 0;  // 5 = ACEScg, 0 = sRGB
	constexpr int kHDRColorSpace = 2;                      // BT2020
	constexpr int kSDRColorSpace = 0;                      // sRGB / BT709 gamut
	const int outputColorSpace = hdrEnabled ? kHDRColorSpace : kSDRColorSpace;
	const int tonemapInputSpace =
		settings.useOpenDrt ? 4 :
							  ((hdrEnabled && tonemappers[tonemapperType].supportsHDR) ?
									  tonemappers[tonemapperType].nativeInputSpaceHDR :
									  tonemappers[tonemapperType].nativeInputSpace);
	const int tonemapOutputSpace =
		settings.useOpenDrt ? 2 :
							  ((hdrEnabled && tonemappers[tonemapperType].supportsHDR) ?
									  tonemappers[tonemapperType].nativeOutputSpaceHDR :
									  tonemappers[tonemapperType].nativeOutputSpace);

	auto storeMatrix = [](const DirectX::SimpleMath::Matrix& mat, std::array<float3, 3>& out) {
		out = {
			float3{ mat(0, 0), mat(0, 1), mat(0, 2) },
			float3{ mat(1, 0), mat(1, 1), mat(1, 2) },
			float3{ mat(2, 0), mat(2, 1), mat(2, 2) }
		};
	};

	storeMatrix(getRGBMatrix(spaces[kInputColorSpace], spaces[settings.processColorSpace]), inputToWorkingMatrix);
	storeMatrix(getRGBMatrix(spaces[settings.processColorSpace], spaces[tonemapInputSpace]), workingToTonemapMatrix);
	storeMatrix(getRGBMatrix(spaces[tonemapOutputSpace], spaces[outputColorSpace]), tonemapToOutputMatrix);
}

void ColorGrading::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	ResolveTonemapperFromSettings();

	logger::debug("Creating buffers...");
	{
		colorCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorCB>());
	}

	logger::debug("Creating 2D textures...");
	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texColor = std::make_unique<Texture2D>(texDesc);
		texColor->CreateSRV(srvDesc);
		texColor->CreateUAV(uavDesc);

		D3D11_TEXTURE3D_DESC lutTexDesc = {
			.Width = LUTDim,
			.Height = LUTDim,
			.Depth = LUTDim,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		D3D11_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {
			.Format = lutTexDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MostDetailedMip = 0, .MipLevels = lutTexDesc.MipLevels }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC lutUavDesc = {
			.Format = lutTexDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = lutTexDesc.Depth }
		};

		texLUT = std::make_unique<Texture3D>(lutTexDesc);
		texLUT->CreateSRV(lutSrvDesc);
		texLUT->CreateUAV(lutUavDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	}

	// Curve preview textures: 256x1 ramp for GPU-based curve evaluation
	{
		D3D11_TEXTURE2D_DESC curveTexDesc = {
			.Width = CurveSamples,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		};

		D3D11_SHADER_RESOURCE_VIEW_DESC curveSrvDesc = {
			.Format = curveTexDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC curveUavDesc = {
			.Format = curveTexDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texCurveInput = eastl::make_unique<Texture2D>(curveTexDesc);
		texCurveInput->CreateSRV(curveSrvDesc);

		texCurveOutput = eastl::make_unique<Texture2D>(curveTexDesc);
		texCurveOutput->CreateSRV(curveSrvDesc);
		texCurveOutput->CreateUAV(curveUavDesc);

		// Fill input with linear ramp [0, CurveMaxInput] in R=G=B
		std::array<DirectX::PackedVector::XMHALF4, CurveSamples> rampData;
		for (int i = 0; i < CurveSamples; i++) {
			float v = (float)i / (float)(CurveSamples - 1) * CurveMaxInput;
			rampData[i] = {
				DirectX::PackedVector::XMConvertFloatToHalf(v),
				DirectX::PackedVector::XMConvertFloatToHalf(v),
				DirectX::PackedVector::XMConvertFloatToHalf(v),
				DirectX::PackedVector::XMConvertFloatToHalf(1.f)
			};
		}
		context->UpdateSubresource(texCurveInput->resource.get(), 0, nullptr, rampData.data(), CurveSamples * sizeof(DirectX::PackedVector::XMHALF4), 0);

		// Staging texture for readback
		D3D11_TEXTURE2D_DESC stagingDesc = curveTexDesc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		device->CreateTexture2D(&stagingDesc, nullptr, curveStaging.put());
	}

	CompileComputeShaders();
}

void ColorGrading::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&colorgradingCS,
		&lutgenCS
	};

	Util::ResetComPtrs(shaderPtrs);

	CompileComputeShaders();
}

void ColorGrading::CompileComputeShaders()
{
	const auto& tonemappers = TonemapperInfo::GetTonemappers();

	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	auto tonemapFuncName = settings.useOpenDrt ? "OpenDRTTransform" : tonemappers[tonemapperType].func_name.data();

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &colorgradingCS, "colorgrading.cs.hlsl", { { "TONEMAP_FUNC", tonemapFuncName } }, "CSColorGrading" },
			{ &lutgenCS, "colorgrading.cs.hlsl", { { "TONEMAP_FUNC", tonemapFuncName } }, "CSLUTGen" }
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\ColorGrading") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
	curveNeedsUpdate = true;  // shader changed, curve must update
}

void ColorGrading::Draw(TextureInfo& inout_tex)
{
	auto context = globals::d3d::context;
	auto state = globals::state;

	// Auto-switch to an HDR-capable tonemapper if current one doesn't support HDR.
	// This runs every frame so the switch happens immediately when HDR is toggled,
	// regardless of which settings page the user is viewing.
	{
		auto& hdrRef = globals::features::hdrDisplay;
		const bool hdrActive = hdrRef.loaded && hdrRef.settings.enableHDR;
		auto& tonemappers = TonemapperInfo::GetTonemappers();

		if (hdrActive && !tonemappers[tonemapperType].supportsHDR) {
			for (int i = 0; i < (int)tonemappers.size(); ++i) {
				if (tonemappers[i].supportsHDR) {
					tonemappers[tonemapperType].cached_settings = settings.tonemapParams;
					settings.tonemapParams = tonemappers[i].cached_settings;
					tonemapperType = i;
					recompileFlag = true;
					break;
				}
			}
		}
	}

	if (recompileFlag)
		ClearShaderCache();

	state->BeginPerfEvent("Color Grading and Tonemapping");

	auto& pp = globals::features::postProcessing;

	RE::ImageSpaceData imageSpaceData = pp.imageSpaceManager->gameISData;
	auto& hdr = globals::features::hdrDisplay;
	const bool hdrEnabled = hdr.loaded && hdr.settings.enableHDR;
	UpdateColorSpaceTransforms(hdrEnabled);

	// Always compute XYZ matrices for white balance
	{
		auto& spaces = getAvailableColorSpaces();
		int wsIdx = std::clamp(settings.processColorSpace, 0, static_cast<int>(spaces.size()) - 1);
		auto storeMatrix = [](const DirectX::SimpleMath::Matrix& mat, std::array<float3, 3>& out) {
			out = {
				float3{ mat(0, 0), mat(0, 1), mat(0, 2) },
				float3{ mat(1, 0), mat(1, 1), mat(1, 2) },
				float3{ mat(2, 0), mat(2, 1), mat(2, 2) }
			};
		};
		storeMatrix(getRGBMatrix(spaces[wsIdx], "XYZ"), workingToXYZMatrix);
		storeMatrix(getRGBMatrix("XYZ", spaces[wsIdx]), xyzToWorkingMatrix);
	}

	ColorCB colorCBData = {
		.asccdl = { ApplyUnitAllRGB(settings.slope), ApplyUnitAllRGB(settings.power), ApplyZeroAllRGB(settings.cdlOffset) },
		.liftgammagain = {
			PackLiftGammaGainAllRGB(settings.lift, ZeroAllNeutral),
			PackLiftGammaGainAllRGB(settings.gamma, ZeroAllNeutral),
			PackLiftGammaGainAllRGB(settings.gain, UnitAllNeutral) },
		.inOutGamma = settings.inOutGamma,
		.oklchSaturation = settings.oklchSaturation,
		.oklchColorMixer = { settings.oklchColorMixer[0], settings.oklchColorMixer[1], settings.oklchColorMixer[2], settings.oklchColorMixer[3], settings.oklchColorMixer[4], settings.oklchColorMixer[5], settings.oklchColorMixer[6] },
		.contrast = ApplyUnitAllRGB(settings.contrast),
		.pivot = ApplyAllRGB(settings.pivot, PivotAllNeutral),
		.exposureTemperatureTint = settings.exposureTemperatureTint,
		.shadows = ApplyUnitAllRGB(settings.shadowsGain),
		.midtones = ApplyUnitAllRGB(settings.midtonesGain),
		.highlights = ApplyUnitAllRGB(settings.highlightsGain),
		.shadowsHighlightsRange = settings.shadowsHighlightsRange,
		.tonemapParams = { settings.tonemapParams[0], settings.tonemapParams[1] },
		.inputToWorking = { float4{ inputToWorkingMatrix[0].x, inputToWorkingMatrix[0].y, inputToWorkingMatrix[0].z, 0.f }, float4{ inputToWorkingMatrix[1].x, inputToWorkingMatrix[1].y, inputToWorkingMatrix[1].z, 0.f }, float4{ inputToWorkingMatrix[2].x, inputToWorkingMatrix[2].y, inputToWorkingMatrix[2].z, 0.f } },
		.workingToTonemap = { float4{ workingToTonemapMatrix[0].x, workingToTonemapMatrix[0].y, workingToTonemapMatrix[0].z, 0.f }, float4{ workingToTonemapMatrix[1].x, workingToTonemapMatrix[1].y, workingToTonemapMatrix[1].z, 0.f }, float4{ workingToTonemapMatrix[2].x, workingToTonemapMatrix[2].y, workingToTonemapMatrix[2].z, 0.f } },
		.tonemapToOutput = { float4{ tonemapToOutputMatrix[0].x, tonemapToOutputMatrix[0].y, tonemapToOutputMatrix[0].z, 0.f }, float4{ tonemapToOutputMatrix[1].x, tonemapToOutputMatrix[1].y, tonemapToOutputMatrix[1].z, 0.f }, float4{ tonemapToOutputMatrix[2].x, tonemapToOutputMatrix[2].y, tonemapToOutputMatrix[2].z, 0.f } },
		.workingToXYZ = { float4{ workingToXYZMatrix[0].x, workingToXYZMatrix[0].y, workingToXYZMatrix[0].z, 0.f }, float4{ workingToXYZMatrix[1].x, workingToXYZMatrix[1].y, workingToXYZMatrix[1].z, 0.f }, float4{ workingToXYZMatrix[2].x, workingToXYZMatrix[2].y, workingToXYZMatrix[2].z, 0.f } },
		.xyzToWorking = { float4{ xyzToWorkingMatrix[0].x, xyzToWorkingMatrix[0].y, xyzToWorkingMatrix[0].z, 0.f }, float4{ xyzToWorkingMatrix[1].x, xyzToWorkingMatrix[1].y, xyzToWorkingMatrix[1].z, 0.f }, float4{ xyzToWorkingMatrix[2].x, xyzToWorkingMatrix[2].y, xyzToWorkingMatrix[2].z, 0.f } },
		.workingWhitePoint = [&]() {
			auto& spaces = getAvailableColorSpaces();
			int wsIdx = std::clamp(settings.processColorSpace, 0, static_cast<int>(spaces.size()) - 1);
			auto wp = getWhitePoint(spaces[wsIdx]);
			return float4{ wp.x, wp.y, 0.f, 0.f }; }(),
		.shadowsOffset = ApplyZeroAllRGB(settings.shadowsOffset),
		.midtonesOffset = ApplyZeroAllRGB(settings.midtonesOffset),
		.highlightsOffset = ApplyZeroAllRGB(settings.highlightsOffset),
		.cinematic = float4{ std::lerp(1.f, imageSpaceData.baseData.cinematic.saturation, settings.gameCinematicBlend.x), std::lerp(1.f, imageSpaceData.baseData.cinematic.brightness, settings.gameCinematicBlend.y), std::lerp(1.f, imageSpaceData.baseData.cinematic.contrast, settings.gameCinematicBlend.z), imageSpaceData.baseAmount },
		.fade = float4{ imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeR], imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeG], imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeB], imageSpaceData.modData.data[RE::ImageSpaceModData::kFadeAmount] * settings.gameFadeBlend },
		.tint = float4{ imageSpaceData.baseData.tint.color.red, imageSpaceData.baseData.tint.color.green, imageSpaceData.baseData.tint.color.blue, imageSpaceData.baseData.tint.amount * settings.gameTintBlend },
		.logType = settings.useLog ? ((1u << settings.logType) | (settings.invertLog ? (1u << 3u) : 0u)) : 0u,
		.skipLDR = settings.skipLDR,
		.skipLUT = settings.skipLUT,
		.enableTonemap = settings.enableTonemap,
		.enableColorSpaceTransform = true,
		// Auto-populate HDR settings from HDR feature
		.enableHDR = [&]() -> uint {
			return hdrEnabled ? 1u : 0u;
		}(),
		.hdrPeakNits = [&]() -> float {
			return hdrEnabled ? static_cast<float>(hdr.settings.hdrPeakNits) : 1000.f;
		}(),
		.hdrPaperWhiteNits = [&]() -> float {
			return hdrEnabled ? static_cast<float>(hdr.settings.hdrPaperWhite) : 203.f;
		}(),
		.odrtConfig = settings.odrtConfig,
	};
	colorCB->Update(colorCBData);

	// Check if curve needs update (CB changed = settings changed)
	if (memcmp(&colorCBData, prevCurveCB.data(), sizeof(ColorCB)) != 0) {
		curveNeedsUpdate = true;
		memcpy(prevCurveCB.data(), &colorCBData, sizeof(ColorCB));
	}

	ID3D11Buffer* cb = colorCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);

	std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };
	context->CSSetSamplers(0, 1, samplers.data());
	ID3D11UnorderedAccessView* uav = nullptr;

	if (!settings.skipLUT) {
		// LUT Gen
		uav = texLUT->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(lutgenCS.get(), nullptr, 0);
		context->Dispatch(LUTDim >> 3, LUTDim >> 3, LUTDim >> 3);

		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// Apply Color Grading (via LUT or direct)
	std::array<ID3D11ShaderResourceView*, 2> srvs = { inout_tex.srv, texLUT->srv.get() };
	uav = texColor->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, (UINT)(settings.skipLUT ? 1 : 2), srvs.data());
	context->CSSetShader(colorgradingCS.get(), nullptr, 0);

	context->Dispatch((texColor->desc.Width + 7) >> 3, (texColor->desc.Height + 7) >> 3, 1);

	// clean up
	srvs.fill(nullptr);
	uav = nullptr;
	cb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 2, srvs.data());
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);

	if (saveImagesFlag) {
		saveImagesFlag = false;
		OutputTextures();
	}

	inout_tex = { texColor->resource.get(), texColor->srv.get() };

	const bool curveReadbackActive =
		Menu::GetSingleton()->IsEnabled &&
		curveReadbackRequested &&
		ImGui::GetCurrentContext() &&
		curveReadbackRequestFrame >= ImGui::GetFrameCount() - 1;
	if (!curveReadbackActive)
		curveReadbackRequested = false;

	// Debug: evaluate color grading pipeline on a neutral ramp for curve preview
	if (curveReadbackActive && curveNeedsUpdate && texCurveInput && texCurveOutput && colorgradingCS) {
		curveNeedsUpdate = false;
		// Re-bind CB and samplers for the curve dispatch
		ID3D11Buffer* curveCB = colorCB->CB();
		std::array<ID3D11SamplerState*, 1> curveSamplers = { linearSampler.get() };
		context->CSSetConstantBuffers(1, 1, &curveCB);
		context->CSSetSamplers(0, 1, curveSamplers.data());

		// Dispatch colorgradingCS on the 256x1 ramp input (same CB, same LUT)
		std::array<ID3D11ShaderResourceView*, 2> curveSRVs = { texCurveInput->srv.get(), texLUT ? texLUT->srv.get() : nullptr };
		ID3D11UnorderedAccessView* curveUAV = texCurveOutput->uav.get();

		context->CSSetShaderResources(0, (UINT)(settings.skipLUT ? 1 : 2), curveSRVs.data());
		context->CSSetUnorderedAccessViews(0, 1, &curveUAV, nullptr);
		context->CSSetShader(colorgradingCS.get(), nullptr, 0);

		context->Dispatch((CurveSamples + 7) >> 3, 1, 1);

		// Clean up
		curveUAV = nullptr;
		curveSRVs.fill(nullptr);
		curveCB = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &curveUAV, nullptr);
		context->CSSetShaderResources(0, 2, curveSRVs.data());
		context->CSSetConstantBuffers(1, 1, &curveCB);
		context->CSSetShader(nullptr, nullptr, 0);

		// Readback
		if (curveStaging) {
			context->CopyResource(curveStaging.get(), texCurveOutput->resource.get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(curveStaging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				auto* pixels = reinterpret_cast<const uint16_t*>(mapped.pData);
				for (int i = 0; i < CurveSamples; i++) {
					curveR[i] = DirectX::PackedVector::XMConvertHalfToFloat(pixels[i * 4 + 0]);
					curveG[i] = DirectX::PackedVector::XMConvertHalfToFloat(pixels[i * 4 + 1]);
					curveB[i] = DirectX::PackedVector::XMConvertHalfToFloat(pixels[i * 4 + 2]);
				}
				context->Unmap(curveStaging.get(), 0);
			}
		}
	}

	state->EndPerfEvent();
}

void ColorGrading::OutputTextures()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	DirectX::ScratchImage lutImage;
	DirectX::ScratchImage colorImage;

	if (texLUT->resource) {
		DirectX::CaptureTexture(device, context, texLUT->resource.get(), lutImage);
	}

	if (texColor->resource) {
		DirectX::CaptureTexture(device, context, texColor->resource.get(), colorImage);
	}

	if (std::filesystem::create_directories(outputPath)) {
		logger::info("Missing pp directory created: {}", outputPath);
	}

	std::filesystem::path savePath = outputPath;

	std::filesystem::path lutPath = savePath / "PP_ColorGrading_BakedLUT.dds";
	std::filesystem::path colorPath = savePath / "PP_ColorGrading_ColorOutput.dds";

	DX::ThrowIfFailed(SaveToDDSFile(lutImage.GetImages(), lutImage.GetImageCount(), lutImage.GetMetadata(), DirectX::DDS_FLAGS::DDS_FLAGS_NONE, lutPath.c_str()));
	DX::ThrowIfFailed(SaveToDDSFile(colorImage.GetImages(), colorImage.GetImageCount(), colorImage.GetMetadata(), DirectX::DDS_FLAGS::DDS_FLAGS_NONE, colorPath.c_str()));
}
