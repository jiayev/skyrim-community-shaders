#include "Features/PathTracing.h"

#include "Globals.h"
#include "Menu.h"
#include "Raytracing.h"
#include "Upscaling.h"
#include "Upscaling/Streamline.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.path_tracing."
#define RT_I18N_KEY_PREFIX "feature.raytracing."
#define RT_TKEY(suffix) RT_I18N_KEY_PREFIX suffix

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PathTracing::Settings,
	Enabled,
	RaytracingSettings,
	GeneralSettings,
	StablePlanes,
	NRDSettings,
	NRDReblurSettings,
	NRDRelaxSettings,
	SSSSettings,
	MaterialSettings,
	LightingSettings,
	WaterSettings,
	ExperimentalSettings)

static std::string StableLabel(const char* label, std::string_view id)
{
	return std::format("{}###{}", label, id);
}

static void DrawFloat2(const char* label, float2& v, float min = 0.0f, float max = 1.0f)
{
	float floats[2] = { v.x, v.y };
	if (ImGui::SliderFloat2(label, floats, min, max)) {
		v = { floats[0], floats[1] };
		v.Clamp({ min, min }, { max, max });
	}
}

template <class T>
static void ClampSetting(T& value, T min, T max)
{
	value = std::clamp(value, min, max);
}

void PathTracing::RestoreDefaultSettings()
{
	settings = {};
}

void PathTracing::LoadSettings(json& o_json)
{
	settings = o_json;
	UpdateSettings();
}

void PathTracing::SaveSettings(json& o_json)
{
	o_json = settings;
}

bool PathTracing::Available() const
{
	return loaded && settings.Enabled && globals::features::raytracing.Available();
}

void PathTracing::UpdateSettings()
{
	globals::features::raytracing.UpdateSettings();
}

void PathTracing::DrawSettings()
{
	auto& rt = globals::features::raytracing;

	if (!rt.loaded) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("requires_raytracing_feature"), "Raytracing feature is not loaded."));
		return;
	}

	if (!rt.Available(false)) {
		ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
			T(TKEY("requires_raytracing"), "Creation Engine Raytracing runtime is not available, check the Raytracing feature."));
		return;
	}

	auto settingsBefore = settings;

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);

	if (ImGui::BeginTabBar("##PathTracingTabs", ImGuiTabBarFlags_None)) {
		DrawGeneralSettings();
		DrawNRDSettings();
		DrawAdvancedSettings();
		DrawExperimentalSettings();

		ImGui::EndTabBar();
	}

	if (settingsBefore != settings) {
		UpdateSettings();
	}
}

void PathTracing::DrawGeneralSettings()
{
	if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
		ImGui::PushID("GeneralSettings");

		ImGui::SliderInt(T(TKEY("bounces"), "Bounces"), &settings.RaytracingSettings.Bounces, 1, 8);

		ImGui::SliderInt(T(TKEY("samples_per_pixel"), "Samples Per Pixel"), &settings.RaytracingSettings.SamplesPerPixel, 1, 16);

		const char* rrNames[] = { "Disabled", "Standard", "Enhanced" };
		int currentRR = static_cast<int>(settings.RaytracingSettings.RussianRoulette);
		if (ImGui::Combo(T(TKEY("russian_roulette"), "Russian Roulette"), &currentRR, rrNames, IM_ARRAYSIZE(rrNames))) {
			settings.RaytracingSettings.RussianRoulette = static_cast<CreationEngineRaytracing::RussianRoulette>(currentRR);
		}

		const char* denoiserNames[] = { "None", "NRD Reblur", "NRD Relax", "DLSS RR", "Accumulation" };
		int currentDenoiser = static_cast<int>(settings.GeneralSettings.Denoiser);
		if (ImGui::Combo(T(TKEY("denoiser"), "Denoiser"), &currentDenoiser, denoiserNames, IM_ARRAYSIZE(denoiserNames))) {
			settings.GeneralSettings.Denoiser = static_cast<CreationEngineRaytracing::Denoiser>(currentDenoiser);
		}

		if (settings.GeneralSettings.Denoiser == CreationEngineRaytracing::Denoiser::DLSS_RR) {
			auto* streamline = Streamline::GetSingleton();
			if (!streamline->IsDLSSRRSupported()) {
				ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Error, "%s",
					T(RT_TKEY("dlss_rr_not_available"), "DLSS Ray Reconstruction is not available on this system."));
			} else if (globals::features::upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS_RR) {
				ImGui::TextColored(globals::menu->GetTheme().StatusPalette.Warning, "%s",
					T(RT_TKEY("set_upscaling_to_dlss"), "Set Upscaling method to DLSS to enable Ray Reconstruction."));
			}
		}

		DrawMaterialSettings();

		DrawLightingSettings();

		ImGui::PopID();
		ImGui::EndTabItem();
	}
}

void PathTracing::DrawAdvancedSettings()
{
	if (ImGui::BeginTabItem(T(TKEY("tab_advanced"), "Advanced"))) {
		ImGui::PushID("AdvancedSettings");

		ImGui::Checkbox(T(RT_TKEY("stable_planes"), "Stable Planes"), &settings.StablePlanes);

		DrawSSSSettings();

		DrawWaterSettings();

		ImGui::PopID();
		ImGui::EndTabItem();
	}
}

void PathTracing::DrawNRDSettings()
{
	const auto& denoiser = settings.GeneralSettings.Denoiser;
	const bool reblur = (denoiser == CreationEngineRaytracing::Denoiser::NRD_Reblur);
	const bool relax = (denoiser == CreationEngineRaytracing::Denoiser::NRD_Relax);

	if (!reblur && !relax)
		return;

	if (ImGui::BeginTabItem(T(RT_TKEY("tab_nrd"), "NRD"))) {
		ImGui::PushID("NRDSettings");

		auto& nrdSettings = settings.NRDSettings;

		if (ImGui::InputScalar(T(RT_TKEY("history_fix_frames"), "History Fix Frames"), ImGuiDataType_U32, &nrdSettings.historyFixFrameNum))
			ClampSetting(nrdSettings.historyFixFrameNum, 0u, 3u);

		if (ImGui::InputScalar(T(RT_TKEY("history_fix_base_pixel_stride"), "History Fix Base Pixel Stride"), ImGuiDataType_U32, &nrdSettings.historyFixBasePixelStride))
			ClampSetting(nrdSettings.historyFixBasePixelStride, 1u, 64u);

		if (ImGui::InputScalar(T(RT_TKEY("history_fix_alternate_pixel_stride"), "History Fix Alternate Pixel Stride"), ImGuiDataType_U32, &nrdSettings.historyFixAlternatePixelStride))
			ClampSetting(nrdSettings.historyFixAlternatePixelStride, 1u, 64u);

		if (ImGui::SliderFloat(T(RT_TKEY("fast_history_clamping_sigma_scale"), "Fast History Clamping Sigma Scale"), &nrdSettings.fastHistoryClampingSigmaScale, 1.0f, 3.0f, "%.2f"))
			ClampSetting(nrdSettings.fastHistoryClampingSigmaScale, 1.0f, 3.0f);

		if (ImGui::SliderFloat(T(RT_TKEY("diffuse_prepass_blur_radius"), "Diffuse Prepass Blur Radius"), &nrdSettings.diffusePrepassBlurRadius, 0.0f, 100.0f, "%.1f"))
			ClampSetting(nrdSettings.diffusePrepassBlurRadius, 0.0f, 100.0f);

		if (ImGui::SliderFloat(T(RT_TKEY("specular_prepass_blur_radius"), "Specular Prepass Blur Radius"), &nrdSettings.specularPrepassBlurRadius, 0.0f, 100.0f, "%.1f"))
			ClampSetting(nrdSettings.specularPrepassBlurRadius, 0.0f, 100.0f);

		if (ImGui::SliderFloat(T(RT_TKEY("min_hit_distance_weight"), "Min Hit Distance Weight"), &nrdSettings.minHitDistanceWeight, 0.001f, 0.2f, "%.3f"))
			ClampSetting(nrdSettings.minHitDistanceWeight, 0.001f, 0.2f);

		if (ImGui::SliderFloat(T(RT_TKEY("lobe_angle_fraction"), "Lobe Angle Fraction"), &nrdSettings.lobeAngleFraction, 0.0f, 1.0f, "%.3f"))
			ClampSetting(nrdSettings.lobeAngleFraction, 0.0f, 1.0f);

		if (ImGui::SliderFloat(T(RT_TKEY("roughness_fraction"), "Roughness Fraction"), &nrdSettings.roughnessFraction, 0.0f, 1.0f, "%.3f"))
			ClampSetting(nrdSettings.roughnessFraction, 0.0f, 1.0f);

		ImGui::Checkbox(T(RT_TKEY("enable_anti_firefly"), "Enable Anti Firefly"), &nrdSettings.enableAntiFirefly);

		if (reblur)
			DrawReblurSettings();
		else if (relax)
			DrawRelaxSettings();

		ImGui::PopID();
		ImGui::EndTabItem();
	}
}

void PathTracing::DrawReblurSettings()
{
	if (!ImGui::TreeNodeEx(T(RT_TKEY("reblur"), "Reblur"), ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}

	auto& reblurSettings = settings.NRDReblurSettings;

	if (ImGui::InputScalar(T(RT_TKEY("max_accumulated_frames"), "Max Accumulated Frames"), ImGuiDataType_U32, &reblurSettings.maxAccumulatedFrameNum))
		ClampSetting(reblurSettings.maxAccumulatedFrameNum, 0u, 63u);

	if (ImGui::InputScalar(T(RT_TKEY("max_fast_accumulated_frames"), "Max Fast Accumulated Frames"), ImGuiDataType_U32, &reblurSettings.maxFastAccumulatedFrameNum))
		ClampSetting(reblurSettings.maxFastAccumulatedFrameNum, 0u, reblurSettings.maxAccumulatedFrameNum);

	if (ImGui::InputScalar(T(RT_TKEY("max_stabilized_frames"), "Max Stabilized Frames"), ImGuiDataType_U32, &reblurSettings.maxStabilizedFrameNum))
		ClampSetting(reblurSettings.maxStabilizedFrameNum, 0u, reblurSettings.maxAccumulatedFrameNum);

	if (ImGui::SliderFloat(T(RT_TKEY("min_blur_radius"), "Min Blur Radius"), &reblurSettings.minBlurRadius, 0.0f, 10.0f, "%.2f"))
		ClampSetting(reblurSettings.minBlurRadius, 0.0f, 10.0f);

	if (ImGui::SliderFloat(T(RT_TKEY("max_blur_radius"), "Max Blur Radius"), &reblurSettings.maxBlurRadius, 0.0f, 100.0f, "%.1f"))
		ClampSetting(reblurSettings.maxBlurRadius, 0.0f, 100.0f);

	if (ImGui::SliderFloat(T(RT_TKEY("plane_distance_sensitivity"), "Plane Distance Sensitivity"), &reblurSettings.planeDistanceSensitivity, 0.0f, 1.0f, "%.3f"))
		ClampSetting(reblurSettings.planeDistanceSensitivity, 0.0f, 1.0f);

	if (ImGui::SliderFloat(T(RT_TKEY("firefly_suppressor_min_relative_scale"), "Firefly Suppressor Min Relative Scale"), &reblurSettings.fireflySuppressorMinRelativeScale, 1.0f, 3.0f, "%.2f"))
		ClampSetting(reblurSettings.fireflySuppressorMinRelativeScale, 1.0f, 3.0f);

	ImGui::Checkbox(T(RT_TKEY("use_prepass_only_for_specular_motion_estimation"), "Use Prepass Only For Specular Motion Estimation"), &reblurSettings.usePrepassOnlyForSpecularMotionEstimation);
	ImGui::Checkbox(T(RT_TKEY("return_history_length_instead_of_occlusion"), "Return History Length Instead Of Occlusion"), &reblurSettings.returnHistoryLengthInsteadOfOcclusion);

	ImGui::TreePop();
}

void PathTracing::DrawRelaxSettings()
{
	if (!ImGui::TreeNodeEx(T(RT_TKEY("relax"), "Relax"), ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}

	auto& relaxSettings = settings.NRDRelaxSettings;

	if (ImGui::InputScalar(T(RT_TKEY("diffuse_max_accumulated_frames"), "Diffuse Max Accumulated Frames"), ImGuiDataType_U32, &relaxSettings.diffuseMaxAccumulatedFrameNum))
		ClampSetting(relaxSettings.diffuseMaxAccumulatedFrameNum, 0u, 63u);

	if (ImGui::InputScalar(T(RT_TKEY("specular_max_accumulated_frames"), "Specular Max Accumulated Frames"), ImGuiDataType_U32, &relaxSettings.specularMaxAccumulatedFrameNum))
		ClampSetting(relaxSettings.specularMaxAccumulatedFrameNum, 0u, 63u);

	if (ImGui::InputScalar(T(RT_TKEY("diffuse_max_fast_accumulated_frames"), "Diffuse Max Fast Accumulated Frames"), ImGuiDataType_U32, &relaxSettings.diffuseMaxFastAccumulatedFrameNum))
		ClampSetting(relaxSettings.diffuseMaxFastAccumulatedFrameNum, 0u, relaxSettings.diffuseMaxAccumulatedFrameNum);

	if (ImGui::InputScalar(T(RT_TKEY("specular_max_fast_accumulated_frames"), "Specular Max Fast Accumulated Frames"), ImGuiDataType_U32, &relaxSettings.specularMaxFastAccumulatedFrameNum))
		ClampSetting(relaxSettings.specularMaxFastAccumulatedFrameNum, 0u, relaxSettings.specularMaxFastAccumulatedFrameNum);

	if (ImGui::SliderFloat(T(RT_TKEY("diffuse_phi_luminance"), "Diffuse Phi Luminance"), &relaxSettings.diffusePhiLuminance, 0.0f, 10.0f, "%.2f"))
		ClampSetting(relaxSettings.diffusePhiLuminance, 0.0f, 10.0f);

	if (ImGui::SliderFloat(T(RT_TKEY("specular_phi_luminance"), "Specular Phi Luminance"), &relaxSettings.specularPhiLuminance, 0.0f, 10.0f, "%.2f"))
		ClampSetting(relaxSettings.specularPhiLuminance, 0.0f, 10.0f);

	if (ImGui::InputScalar(T(RT_TKEY("atrous_iteration_num"), "A-Trous Iteration Num"), ImGuiDataType_U32, &relaxSettings.atrousIterationNum))
		ClampSetting(relaxSettings.atrousIterationNum, 2u, 8u);

	if (ImGui::SliderFloat(T(RT_TKEY("specular_variance_boost"), "Specular Variance Boost"), &relaxSettings.specularVarianceBoost, 0.0f, 10.0f, "%.2f"))
		ClampSetting(relaxSettings.specularVarianceBoost, 0.0f, 10.0f);

	if (ImGui::SliderFloat(T(RT_TKEY("specular_lobe_angle_slack"), "Specular Lobe Angle Slack"), &relaxSettings.specularLobeAngleSlack, 0.0f, 1.0f, "%.3f"))
		ClampSetting(relaxSettings.specularLobeAngleSlack, 0.0f, 1.0f);

	if (ImGui::SliderFloat(T(RT_TKEY("depth_threshold"), "Depth Threshold"), &relaxSettings.depthThreshold, 0.0f, 0.1f, "%.4f"))
		ClampSetting(relaxSettings.depthThreshold, 0.0f, 0.1f);

	ImGui::Checkbox(T(RT_TKEY("enable_roughness_edge_stopping"), "Enable Roughness Edge Stopping"), &relaxSettings.enableRoughnessEdgeStopping);

	ImGui::TreePop();
}

void PathTracing::DrawSSSSettings()
{
	auto& sssSettings = settings.SSSSettings;

	if (ImGui::CollapsingHeader(T(RT_TKEY("subsurface_scattering"), "Subsurface Scattering"))) {
		ImGui::Checkbox(T(RT_TKEY("sss_enabled"), "Enable Subsurface Scattering"), &sssSettings.Enabled);

		if (sssSettings.Enabled) {
			ImGui::SliderInt(T(RT_TKEY("sss_sample_count"), "Sample Count"), &sssSettings.SampleCount, 1, 16);
			ImGui::SliderFloat(T(RT_TKEY("sss_max_sample_radius"), "Max Sample Radius"), &sssSettings.MaxSampleRadius, 0.01f, 64.0f, "%.2f");
			ImGui::Checkbox(T(RT_TKEY("sss_enable_transmission"), "Enable Transmission"), &sssSettings.EnableTransmission);
			ImGui::Checkbox(T(RT_TKEY("sss_material_override"), "Material Override"), &sssSettings.MaterialOverride);

			if (sssSettings.MaterialOverride) {
				if (ImGui::TreeNodeEx(T(RT_TKEY("sss_overrides"), "Overrides"), ImGuiTreeNodeFlags_DefaultOpen)) {
					const auto overrideTransmissionLabel = StableLabel(T(RT_TKEY("sss_override_transmission_color"), "Override Transmission Color"), "OverrideTransmissionColor");
					const auto overrideScatteringLabel = StableLabel(T(RT_TKEY("sss_override_scattering_color"), "Override Scattering Color"), "OverrideScatteringColor");
					ImGui::ColorEdit3(overrideTransmissionLabel.c_str(), reinterpret_cast<float*>(&sssSettings.OverrideTransmissionColor), ImGuiColorEditFlags_Float);
					ImGui::ColorEdit3(overrideScatteringLabel.c_str(), reinterpret_cast<float*>(&sssSettings.OverrideScatteringColor), ImGuiColorEditFlags_Float);
					ImGui::SliderFloat(T(RT_TKEY("sss_override_scale"), "Override Scale"), &sssSettings.OverrideScale, 0.01f, 1000.0f, "%.2f");
					ImGui::SliderFloat(T(RT_TKEY("sss_override_anisotropy"), "Override Anisotropy"), &sssSettings.OverrideAnisotropy, -0.99f, 0.99f);

					ImGui::TreePop();
				}
			}
		}
	}
}

void PathTracing::DrawMaterialSettings()
{
	if (ImGui::CollapsingHeader(T(RT_TKEY("material"), "Material"), ImGuiTreeNodeFlags_DefaultOpen)) {
		DrawFloat2(T(RT_TKEY("roughness"), "Roughness"), settings.MaterialSettings.Roughness);
		DrawFloat2(T(RT_TKEY("metalness"), "Metalness"), settings.MaterialSettings.Metalness);
	}
}

void PathTracing::DrawLightingSettings()
{
	if (ImGui::CollapsingHeader(T(RT_TKEY("lighting"), "Lighting"), ImGuiTreeNodeFlags_DefaultOpen)) {
		auto& lightingSettings = settings.LightingSettings;

		if (ImGui::DragFloat(T(RT_TKEY("directional_strength"), "Directional Strength"), &lightingSettings.Directional, 0.001f))
			lightingSettings.Directional = std::max(0.0f, lightingSettings.Directional);

		if (ImGui::DragFloat(T(RT_TKEY("point_strength"), "Point Strength"), &lightingSettings.Point, 0.001f))
			lightingSettings.Point = std::max(0.0f, lightingSettings.Point);

		ImGui::Checkbox(T(RT_TKEY("lod_dimmer"), "Lod Dimmer"), &lightingSettings.LodDimmer);

		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(RT_TKEY("lod_dimmer_desc"), "Vanilla behaviour of dimming lights that are far enough.\n"));

		if (ImGui::DragFloat(T(RT_TKEY("emissive_strength"), "Emissive Strength"), &lightingSettings.Emissive, 0.001f))
			lightingSettings.Emissive = std::max(0.0f, lightingSettings.Emissive);

		if (ImGui::DragFloat(T(RT_TKEY("effect_strength"), "Effect Strength"), &lightingSettings.Effect, 0.001f))
			lightingSettings.Effect = std::max(0.0f, lightingSettings.Effect);

		if (ImGui::DragFloat(T(RT_TKEY("sky_strength"), "Sky Strength"), &lightingSettings.Sky, 0.001f))
			lightingSettings.Sky = std::max(0.0f, lightingSettings.Sky);
	}
}

void PathTracing::DrawWaterSettings()
{
	if (ImGui::CollapsingHeader(T(RT_TKEY("water"), "Water"))) {
		auto& waterSettings = settings.WaterSettings;

		if (ImGui::DragFloat(T(RT_TKEY("absorption_scale"), "Absorption Scale"), &waterSettings.AbsorptionScale, 0.01f, 0.01f, 10.0f, "%.2f"))
			waterSettings.AbsorptionScale = std::clamp(waterSettings.AbsorptionScale, 0.01f, 10.0f);
	}
}

void PathTracing::DrawExperimentalSettings()
{
	if (ImGui::BeginTabItem(T(TKEY("tab_experimental"), "Experimental"))) {
		ImGui::PushID("ExperimentalSettings");

		const char* cullNames[] = { "Disabled", "Enabled", "Full" };
		int currentCull = static_cast<int>(settings.ExperimentalSettings.PathTracingCull);
		if (ImGui::Combo(T(TKEY("pathtracing_cull"), "Path Tracing Cull"), &currentCull, cullNames, IM_ARRAYSIZE(cullNames))) {
			settings.ExperimentalSettings.PathTracingCull = static_cast<CreationEngineRaytracing::PTCullMode>(currentCull);
		}

		ImGui::PopID();
		ImGui::EndTabItem();
	}
}



