#include "PostProcessing.h"

#include "CSEditor/EditorWindow.h"
#include "CSEditor/SceneManager/SceneWidgetInterceptor.h"
#include "Features/CSEditor.h"
#include "IconsFontAwesome5.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/PresetsPageRenderer.h"
#include "imgui_stdlib.h"

#include "JiayeStatement.h"
#include "Menu.h"
#include "PostProcessingMode.h"
#include "Profiler.h"
#include "State.h"
#include "Util.h"

#include "PostProcessing/RasterPass.h"

#include "Features/HDRDisplay.h"
#include "Features/LinearLighting.h"
#include "Features/Upscaling.h"
#include "Utils/ColorSpace.h"

#include <format>

void PostProcessing::DrawSettings()
{
	static int pipelinePageNum = 0;
	static int pipelineFeatIdx = 0;

	// Presets live on the Presets page like every other pack; a scene replica of this page edits a
	// scene layer rather than the base settings a preset sets, so it leaves the row out.
	if (!SceneWidgetInterceptor::IsArmed()) {
		PresetsPageRenderer::DrawOpenButton("##PostProcessingOpenPresets",
			T("feature.post_processing.open_presets_tooltip",
				"Apply Post Processing presets on the Presets page. To save your current settings as one, use "
				"Export Preset in the CS Editor with Post Processing ticked."));

		// Inside the CS Editor (its Base Settings window draws this page too) the editor is already open.
		if (const auto* editor = EditorWindow::GetSingleton(); !editor || !editor->open) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const char* csEditorTitle = T("menu.presets.open_cs_editor", "CS Editor");
			const Icons::GlyphRef brush = Icons::FA(ICON_FA_PAINT_BRUSH);
			const float buttonWidth = Icons::CalcGlyphSize(brush).x + style.ItemInnerSpacing.x +
			                          ImGui::CalcTextSize(csEditorTitle).x + style.FramePadding.x * 2.0f;
			ImGui::SameLine();
			if (const float avail = ImGui::GetContentRegionAvail().x; avail > buttonWidth)
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - buttonWidth);
			if (Icons::LabeledButton("##PostProcessingOpenCSEditor", brush, csEditorTitle))
				CSEditor::OpenEditorWindow();
			Util::AddTooltip(T("menu.presets.open_cs_editor_tooltip", "Open the CS Editor for weather, lighting, and scene editing."));
		}

		ImGui::Separator();
	}

	PostProcessingMode::DrawSelector();

	ImGui::Separator();

	// Effects 11 discards the pipeline output, so its controls would do nothing.
	const Util::LockedSection effects11Lock(PostProcessingMode::Get() == PostProcessingMode::Mode::Effects11,
		T("common.settings_managed_by_enb", "This setting is managed by Effects 11."));

	{
		auto& cam = cinematicCamera;
		ImGui::Checkbox(T("feature.post_processing.cinematic_camera.name", "Cinematic Camera"), &cam.settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T("feature.post_processing.cinematic_camera.description",
								  "Controls lens, focus, exposure and FOV. Exposure processing runs automatically while the camera is active; other effects must be enabled separately."));
		ImGui::SameLine();
		if (Icons::LabeledButton("##CinematicCameraSettings", Icons::FA(ICON_FA_BARS),
				T("feature.post_processing.cinematic_camera.settings", "Settings")))
			pipelinePageNum = 2;

		if (const auto* state = cam.GetState()) {
			const auto isoText = state->Exposure == CinematicCamera::ExposureMode::AutoISO ?
			                         std::format("{} {:.0f}-{:.0f}", T("feature.post_processing.cinematic_camera.exposure_auto_iso", "Auto ISO"), state->MinISO, state->MaxISO) :
			                         std::format("ISO {:.0f}", state->ISO);
			ImGui::TextDisabled(T("feature.post_processing.cinematic_camera.summary_exposure",
									"%s - %.1f mm - f/%.1f - %.0f deg - %s - FOV %.1f deg - %s"),
				cam.GetFilmbackPresetName(),
				state->FocalLengthMM,
				state->FNumber,
				cam.settings.Exposure.ShutterAngleDeg,
				isoText.c_str(),
				state->HorizontalFOVDeg,
				cam.GetFovStateText());
		} else if (cam.settings.Enabled) {
			ImGui::TextDisabled("%s - %s", cam.GetFilmbackPresetName(), cam.GetFovStateText());
		}
	}

	ImGui::Separator();

	// Draws a read-only checkbox for a forced-on sub-feature and returns true. Otherwise callers bind
	// `&feat->enabled` themselves: the scene catalog generator only recognises that form as a scene toggle.
	auto drawForcedEnabled = [this](const char* label, PostProcessFeature& feature) {
		const bool automatic = feature.IsAutoEnabled() ||
		                       (&feature == GetPipelineFeature<HistogramAutoExposure>(FeaturePipelineIndex::AutoExposure) && GetActivePhysicalCameraState());
		if (!automatic)
			return false;
		bool active = feature.IsActive();
		ImGui::BeginDisabled();
		ImGui::Checkbox(label, &active);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.post_processing.cinematic_camera.exposure_required", "Exposure processing is required by Cinematic Camera. Disable Cinematic Camera to restore the saved enable state."));
		return true;
	};

	if (pipelinePageNum == 0) {
		for (int i = 0; i < pipeline.size(); ++i) {
			auto& feat = pipeline[i];
			if (feat && feat->IsVisible()) {
				auto displayName = feat->GetDisplayName();
				auto description = feat->GetDesc();
				ImGui::PushID(feat->GetType().c_str());
				if (!drawForcedEnabled("##Enabled", *feat))
					ImGui::Checkbox("##Enabled", &feat->enabled);
				ImGui::SameLine();
				if (Icons::Button("##Bars", Icons::FA(ICON_FA_BARS))) {
					pipelineFeatIdx = i;
					pipelinePageNum = 1;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T("feature.post_processing.edit_settings_for_this_feature", "Edit settings for this feature."));
				ImGui::SameLine();
				ImGui::Text("%s", displayName.c_str());
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", description.c_str());
				ImGui::PopID();
			}
		}
	} else if (pipelinePageNum == 1) {
		if (Icons::LabeledButton("##BackToPipeline", Icons::FA(ICON_FA_ARROW_LEFT),
				T("feature.post_processing.back_to_pipeline", "Back to Pipeline"))) {
			pipelinePageNum = 0;
		}
		ImGui::Separator();
		if (pipelineFeatIdx >= 0 && pipelineFeatIdx < pipeline.size()) {
			auto& feat = pipeline[pipelineFeatIdx];
			if (feat) {
				auto displayName = feat->GetDisplayName();
				auto description = feat->GetDesc();
				ImGui::PushID(feat->GetType().c_str());

				ImGui::SeparatorText(displayName.c_str());
				ImGui::TextWrapped("%s", description.c_str());

				ImGui::Spacing();
				if (Icons::LabeledButton("##RecompileShaders", Icons::FA(ICON_FA_SYNC),
						T("feature.post_processing.recompile_shaders", "Recompile Shaders"))) {
					feat->ClearShaderCache();
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("%s", T("feature.post_processing.recompile_shaders_for_this_sub_feature_only", "Recompile shaders for this sub-feature only."));
				ImGui::Separator();
				ImGui::Spacing();
				if (!drawForcedEnabled(T("feature.post_processing.enabled", "Enabled"), *feat))
					ImGui::Checkbox(T("feature.post_processing.enabled", "Enabled"), &feat->enabled);
				if (feat->IsActive()) {
					ImGui::Indent();
					feat->DrawSettings();
					ImGui::Unindent();
				} else {
					ImGui::TextDisabled("%s", T("feature.post_processing.enable_the_feature_to_see_its_settings", "Enable the feature to see its settings."));
				}

				ImGui::PopID();
			} else {
				ImGui::TextDisabled("%s", T("feature.post_processing.selected_feature_is_not_valid", "Selected feature is not valid."));
				pipelinePageNum = 0;
			}
		} else {
			ImGui::TextDisabled("%s", T("feature.post_processing.invalid_feature_selected_returning_to_list", "Invalid feature selected. Returning to list."));
			pipelinePageNum = 0;
		}
	} else if (pipelinePageNum == 2) {
		if (Icons::LabeledButton("##BackToPipeline", Icons::FA(ICON_FA_ARROW_LEFT),
				T("feature.post_processing.back_to_pipeline", "Back to Pipeline"))) {
			pipelinePageNum = 0;
		}
		ImGui::Separator();
		ImGui::SeparatorText(T("feature.post_processing.cinematic_camera.name", "Cinematic Camera"));

		ImGui::TextWrapped("%s", T("feature.post_processing.cinematic_camera.description",
									 "Controls lens, focus, exposure and FOV. Exposure processing runs automatically while the camera is active; other effects must be enabled separately."));
		ImGui::Spacing();

		ImGui::Checkbox(T("feature.post_processing.enabled", "Enabled"), &cinematicCamera.settings.Enabled);
		if (cinematicCamera.settings.Enabled) {
			ImGui::Indent();
			cinematicCamera.DrawSettings();
			if (auto* exposure = GetPipelineFeature<HistogramAutoExposure>(FeaturePipelineIndex::AutoExposure)) {
				ImGui::SeparatorText(T("feature.post_processing.cinematic_camera.exposure_meter", "Exposure Meter"));
				exposure->DrawCameraExposureReadout();
			}

			// Linked-effect status: which adapters are currently consuming overrides.
			ImGui::Separator();
			ImGui::Text("%s", T("feature.post_processing.cinematic_camera.linked_effects", "Linked Effects"));
			auto drawLinked = [&](FeaturePipelineIndex idx, const char* fallbackName) {
				auto& pipe = pipeline[static_cast<size_t>(idx)];
				if (!pipe)
					return;
				ImGui::Bullet();
				ImGui::Text("%s: ", pipe->GetDisplayName().empty() ? fallbackName : pipe->GetDisplayName().c_str());
				ImGui::SameLine();
				if (pipe->IsActive())
					ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", T("feature.post_processing.cinematic_camera.linked_active", "Active"));
				else
					ImGui::TextDisabled("%s", T("feature.post_processing.cinematic_camera.linked_disabled", "Disabled - no override"));
			};
			drawLinked(FeaturePipelineIndex::DoF, "Depth of Field");
			drawLinked(FeaturePipelineIndex::LensFlare, "Lens Flare");
			drawLinked(FeaturePipelineIndex::PhysicalGlare, "Physical Glare");
			drawLinked(FeaturePipelineIndex::Vignette, "Vignette");
			drawLinked(FeaturePipelineIndex::MotionBlur, "Motion Blur");
			drawLinked(FeaturePipelineIndex::AutoExposure, "Histogram Auto Exposure");
			drawLinked(FeaturePipelineIndex::Camera, "Camera");
			ImGui::Unindent();
		}
	}

	ImGui::Separator();

	if (ImGui::TreeNode(T("feature.post_processing.debug", "Debug"))) {
		if (ImGui::TreeNode(T("feature.post_processing.game_imagespace_values", "Game ImageSpace Values"))) {
			ImGui::Text(T("feature.post_processing.base_amount", "Base Amount: %.3f"), imageSpaceManager->gameISData.baseAmount);
			ImGui::Text("%s", T("feature.post_processing.base_data", "Base Data:"));
			ImGui::Text("%s", T("feature.post_processing.cinematic_values", "Cinematic Values:"));
			ImGui::Text(T("feature.post_processing.saturation_brightness_contrast_values", "Saturation: %.3f\nBrightness: %.3f\nContrast: %.3f"),
				imageSpaceManager->gameISData.baseData.cinematic.saturation,
				imageSpaceManager->gameISData.baseData.cinematic.brightness,
				imageSpaceManager->gameISData.baseData.cinematic.contrast);

			ImGui::Text("%s", T("feature.post_processing.hdr_values", "HDR Values:"));
			ImGui::Text(T("feature.post_processing.hdr_values_detail", "Eye Adapt Speed: %.3f\nBloom Blur Radius: %.3f\nBloom Threshold: %.3f\nBloom Scale: %.3f\nReceive Bloom Threshold: %.3f\nWhite: %.3f\nSunlight Scale: %.3f\nSky Scale: %.3f\nEye Adapt Strength: %.3f"),
				imageSpaceManager->gameISData.baseData.hdr.eyeAdaptSpeed,
				imageSpaceManager->gameISData.baseData.hdr.bloomBlurRadius,
				imageSpaceManager->gameISData.baseData.hdr.bloomThreshold,
				imageSpaceManager->gameISData.baseData.hdr.bloomScale,
				imageSpaceManager->gameISData.baseData.hdr.receiveBloomThreshold,
				imageSpaceManager->gameISData.baseData.hdr.white,
				imageSpaceManager->gameISData.baseData.hdr.sunlightScale,
				imageSpaceManager->gameISData.baseData.hdr.skyScale,
				imageSpaceManager->gameISData.baseData.hdr.eyeAdaptStrength);

			ImGui::Text("%s", T("feature.post_processing.tint_values", "Tint Values:"));
			ImGui::Text(T("feature.post_processing.tint_values_detail", "Tint Amount: %.3f\nTint Color: (%.3f, %.3f, %.3f)"),
				imageSpaceManager->gameISData.baseData.tint.amount,
				imageSpaceManager->gameISData.baseData.tint.color.red,
				imageSpaceManager->gameISData.baseData.tint.color.green,
				imageSpaceManager->gameISData.baseData.tint.color.blue);

			ImGui::Text("%s", T("feature.post_processing.depth_of_field_values", "Depth of Field Values:"));
			ImGui::Text(T("feature.post_processing.depth_of_field_values_detail", "DOF Strength: %.3f\nDOF Distance: %.3f\nDOF Range: %.3f\nDOF Flags: %d\nDOF Sky Blur Radius: %d"),
				imageSpaceManager->gameISData.baseData.depthOfField.strength,
				imageSpaceManager->gameISData.baseData.depthOfField.distance,
				imageSpaceManager->gameISData.baseData.depthOfField.range,
				imageSpaceManager->gameISData.baseData.depthOfField.flags,
				static_cast<int>(imageSpaceManager->gameISData.baseData.depthOfField.skyBlurRadius.get()));

			ImGui::Text(T("feature.post_processing.mod_amount", "Mod Amount: %.3f"), imageSpaceManager->gameISData.modAmount);
			ImGui::Text("%s", T("feature.post_processing.mod_data", "Mod Data:"));
			ImGui::Text(T("feature.post_processing.mod_fade_values_detail", "Fade Amount: %.3f\nFade Color: (%.3f, %.3f, %.3f)\nBlur Radius: %.3f\nDouble Vision Strength: %.3f\n"),
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeAmount],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeR],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeG],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kFadeB],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kBlurRadius],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDoubleVisionStrength]);
			ImGui::Text(T("feature.post_processing.radial_blur_values_detail", "Radial Blur Strength: %.3f\nRadial Blur Rampup: %.3f\nRadial Blur Start: %.3f\nRadial Blur Rampdown: %.3f\nRadial Blur Down Start: %.3f\nRadial Blur Center: (%.3f, %.3f)"),
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurStrength],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurRampup],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurStart],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurRampdown],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurDownStart],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurCenterX],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kRadialBlurCenterY]);
			ImGui::Text(T("feature.post_processing.mod_dof_values_detail", "DOF Strength: %.3f\nDOF Distance: %.3f\nDOF Range: %.3f\nDOF Mode: %d"),
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFStrength],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFDistance],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFRange],
				imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kDOFMode]);
			ImGui::Text(T("feature.post_processing.motion_blur_strength", "Motion Blur Strength: %.3f"), imageSpaceManager->gameISData.modData.data[RE::ImageSpaceModData::kMotionBlurStrength]);
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

void PostProcessing::LoadSettings(json& o_json)
{
	// Deferred to Prepass so a load lands at a fixed point in the frame instead of mid-pass
	// (Scene Manager and overrides call this from State::Draw); SaveSettings reports it until then.
	pendingSettings = o_json;
}

void PostProcessing::ProcessSettings(json& o_json)
{
	// Scene blends reload every frame, so no info-level log and no per-load SetupResources:
	// settings-dependent resources are reconciled in Draw.
	logger::debug("Loading post processing settings...");

	for (auto& feat : pipeline) {
		if (feat && o_json.contains(feat->GetType())) {
			if (!feat->IsAutoEnabled())
				feat->enabled = o_json.value(feat->GetType(), json::object()).value("enabled", true);
			json featSettings = o_json.value(feat->GetType(), json::object()).value("settings", json::object());
			feat->LoadSettings(featSettings);
		}
	}

	if (o_json.contains("cinematic_camera")) {
		json camJson = o_json["cinematic_camera"];
		cinematicCamera.LoadSettings(camJson);
	} else {
		cinematicCamera.RestoreDefaultSettings();
	}
}

void PostProcessing::SaveSettings(json& o_json)
{
	// A load not yet applied is the newest state, so report it rather than the live pipeline. This means
	// a Save -> Load -> Save round trip within one frame cannot observe clamping by the pipeline;
	// callers verifying retention (Scene Manager) only see it once Prepass has consumed the load.
	if (!pendingSettings.empty()) {
		o_json = pendingSettings;
		o_json.erase("ppsettings");
		return;
	}

	for (auto& pipe : pipeline) {
		if (pipe) {
			json featureSetting{};
			pipe->SaveSettings(featureSetting);
			o_json[pipe->GetType()] = {
				{ "enabled", pipe->enabled },
				{ "settings", featureSetting }
			};
		}
	}

	json camJson{};
	cinematicCamera.SaveSettings(camJson);
	o_json["cinematic_camera"] = camJson;
	o_json.erase("ppsettings");
}

void PostProcessing::ApplyDefaultEnabledStates()
{
	using enum FeaturePipelineIndex;
	constexpr std::array kOffByDefault = { LUT, MotionBlur, PhysicalGlare, Camera, Border };
	for (size_t index = 0; index < pipeline.size(); ++index) {
		if (auto& pipe = pipeline[index]; pipe && !pipe->IsAutoEnabled())
			pipe->enabled = std::ranges::find(kOffByDefault, static_cast<FeaturePipelineIndex>(index)) == kOffByDefault.end();
	}
}

void PostProcessing::RestoreDefaultSettings()
{
	// A restore supersedes any load still waiting for Prepass.
	pendingSettings = {};
	cinematicCamera.RestoreDefaultSettings();

	// Before SetupResources there is no pipeline yet; it is built from these same defaults.
	if (!pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)])
		return;

	ApplyDefaultEnabledStates();
	for (auto& pipe : pipeline) {
		if (pipe)
			pipe->RestoreDefaultSettings();
	}
}

void PostProcessing::ClearShaderCache()
{
	for (auto& pipe : pipeline) {
		if (pipe)
			pipe->ClearShaderCache();
	}
}

void PostProcessing::SetupResources()
{
	{
		auto renderer = globals::game::renderer;
		auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		D3D11_TEXTURE2D_DESC texMainDesc;
		D3D11_TEXTURE2D_DESC texMainCopyDesc;
		gameTexMain.texture->GetDesc(&texMainDesc);
		gameTexMainCopy.texture->GetDesc(&texMainCopyDesc);
		texDesc = texMainDesc;

		texDesc.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags = 0;

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texCopyMain = eastl::make_unique<Texture2D>(texDesc);
		texCopyMain->CreateRTV(rtvDesc);

		if (texMainCopyDesc.Format != texMainDesc.Format) {
			texDesc = texMainCopyDesc;
			rtvDesc.Format = texDesc.Format;
			texDesc.MipLevels = 1;
			texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
			texDesc.MiscFlags = 0;

			texCopyMainCopy = eastl::make_unique<Texture2D>(texDesc);
			texCopyMainCopy->CreateRTV(rtvDesc);
		} else {
			texCopyMainCopy = nullptr;
		}
	}

	{
		auto desc = texCopyMain->desc;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		texInput = std::make_unique<Texture2D>(desc);
		texInput->CreateSRV({ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
		texInput->CreateRTV({ .Format = desc.Format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
		copyCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CopyCB>());
	}

	if (auto rawPtr = reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PostProcessing\\fullscreen.hlsli", {}, "vs_5_0", "FullscreenTriangleVS")))
		fullscreenVS.attach(rawPtr);
	if (auto rawPtr = reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\PostProcessing\\copy.ps.hlsl", {}, "ps_5_0")))
		copyPS.attach(rawPtr);
	if (!fullscreenVS || !copyPS)
		stl::report_and_fail("Post Processing requires its input/output shaders."sv);

	pipeline[static_cast<size_t>(FeaturePipelineIndex::LocalExposure)] = std::make_shared<LocalExposure>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::AutoExposure)] = std::make_shared<HistogramAutoExposure>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)] = std::make_shared<ColorGrading>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::LUT)] = std::make_shared<LUT>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::MotionBlur)] = std::make_shared<MotionBlur>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::DoF)] = std::make_shared<DoF>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::PhysicalGlare)] = std::make_shared<PhysicalGlare>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::CODBloom)] = std::make_shared<CODBloom>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::LensFlare)] = std::make_shared<LensFlare>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Composite)] = std::make_shared<Composite>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Vignette)] = std::make_shared<Vignette>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Camera)] = std::make_shared<Camera>();
	pipeline[static_cast<size_t>(FeaturePipelineIndex::Border)] = std::make_shared<Border>();
	ApplyDefaultEnabledStates();

	for (auto& pipe : pipeline) {
		if (pipe) {
			pipe->owner = this;
			pipe->SetupResources();
		}
	}

	bokehResources.Setup();

	cinematicCamera.focusResolver.RequestTDM();

	ProcessSettings(pendingSettings);
	pendingSettings = {};
}

void PostProcessing::Reset()
{
	// Cleared per frame rather than only at the end of PreProcess: when Effects11 owns the
	// tonemap (or the pipeline is bypassed) PreProcess never runs, and a stale flag would
	// make the next frame we do run read from the wrong buffer.
	isrefraction = false;

	for (auto& pipe : pipeline) {
		if (pipe)
			pipe->Reset();
	}
}

void PostProcessing::CopyToRenderTarget(
	RE::BSGraphics::RenderTargetData& targetRT,
	Texture2D* convertTex,
	ID3D11Texture2D* srcTex,
	ID3D11ShaderResourceView* srcSRV, const CopyCB& conversion)
{
	const bool convert = conversion.gamma != 1.f || conversion.inputGamut != conversion.outputGamut;
	if (!targetRT.texture || !srcTex || (!convert && targetRT.texture == srcTex))
		return;

	auto context = globals::d3d::context;

	D3D11_TEXTURE2D_DESC srcDesc;
	srcTex->GetDesc(&srcDesc);

	D3D11_TEXTURE2D_DESC targetDesc;
	targetRT.texture->GetDesc(&targetDesc);

	if (!convert && srcDesc.Format == targetDesc.Format) {
		context->CopySubresourceRegion(targetRT.texture, 0, 0, 0, 0, srcTex, 0, nullptr);
		return;
	}

	if (!copyPS || !fullscreenVS || !convertTex || !convertTex->rtv || !convertTex->resource)
		return;

	{
		PostProcessingRaster::RasterPass pass(context);

		copyCB->Update(conversion);
		ID3D11Buffer* cb = copyCB->CB();
		context->PSSetConstantBuffers(1, 1, &cb);
		ID3D11ShaderResourceView* srv = srcSRV;
		context->PSSetShaderResources(0, 1, &srv);
		pass.SetTargets({ convertTex->rtv.get() }, (float)convertTex->desc.Width, (float)convertTex->desc.Height);
		pass.SetShaders(fullscreenVS.get(), copyPS.get());
		pass.Draw();

		srv = nullptr;
		context->PSSetShaderResources(0, 1, &srv);
		cb = nullptr;
		context->PSSetConstantBuffers(1, 1, &cb);
	}

	context->CopySubresourceRegion(targetRT.texture, 0, 0, 0, 0, convertTex->resource.get(), 0, nullptr);
}

void PostProcessing::BeginLinearProcessing(PostProcessFeature::TextureInfo& texture)
{
	auto& ll = globals::features::linearLighting;
	if (ll.IsLinearLightingActive())
		return;
	auto* context = globals::d3d::context;
	PostProcessingRaster::RasterPass pass(context);
	copyCB->Update(CopyCB{ .gamma = Util::ColorSpace::GAME_GAMMA });
	ID3D11Buffer* cb = copyCB->CB();
	context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	context->PSSetConstantBuffers(1, 1, &cb);
	context->PSSetShaderResources(0, 1, &texture.srv);
	pass.SetTargets({ texInput->rtv.get() }, (float)texInput->desc.Width, (float)texInput->desc.Height);
	pass.SetShaders(fullscreenVS.get(), copyPS.get());
	pass.Draw();
	ID3D11ShaderResourceView* srv = nullptr;
	context->PSSetShaderResources(0, 1, &srv);
	cb = nullptr;
	context->PSSetConstantBuffers(1, 1, &cb);
	texture = { texInput->resource.get(), texInput->srv.get() };
}

void PostProcessing::DrawFeature(PostProcessFeature& feature, PostProcessFeature::TextureInfo& lastTexColor)
{
	if (feature.WritesToMainTexture()) {
		feature.Draw(lastTexColor);
	} else {
		PostProcessFeature::TextureInfo inTex = lastTexColor;
		feature.Draw(inTex);
	}
}

void PostProcessing::DrawBeforeUpscaling()
{
	if (bypass || IsTonemapOwnedByEffects11())
		return;

	auto& upscaling = globals::features::upscaling;
	if (!upscaling.loaded)
		return;

	auto renderer = globals::game::renderer;
	auto state = globals::state;

	bool inMainLoadingMenu = state->IsMainOrLoadingMenuOpen();
	auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };
	bool processing = false;

	state->BeginPerfEvent("[Post Processing] Pre-Upscale");

	// update auto-enabled features
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsAutoEnabled())
			pipe->UpdateAutoEnabled();
	}

	// go through each fx
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsActive() && !pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && pipe->DrawBeforeUpscaling()) {
			if (!processing) {
				globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
				globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
				BeginLinearProcessing(lastTexColor);
				processing = true;
			}
			DrawFeature(*pipe, lastTexColor);
		}
	}

	if (processing)
		CopyToRenderTarget(gameTexMain, texCopyMain.get(), lastTexColor.tex, lastTexColor.srv,
			CopyCB{ .gamma = globals::features::linearLighting.IsLinearLightingActive() ? 1.f : Util::ColorSpace::GAME_GAMMA_INV });

	state->EndPerfEvent();
}

void PostProcessing::PreProcess(RE::RENDER_TARGET a_input)
{
	if (globals::features::linearLighting.IsLinearLightingActive() &&
		(globals::state->permutationData.RenderToUI || globals::state->IsMainOrLoadingMenuOpen()))
		return;
	auto& ll = globals::features::linearLighting;
	const bool linear = ll.IsLinearLightingActive();
	if (bypass && !linear)
		return;

	auto renderer = globals::game::renderer;

	auto& upscaling = globals::features::upscaling;

	// This runs before the HDR chain, so ISRefraction still has kMAIN_COPY bound as a render
	// target. D3D11 silently nulls any SRV of a resource that is also an output, which would
	// make the pipeline sample black instead of the scene.
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	bool inMainLoadingMenu = globals::state->IsMainOrLoadingMenuOpen();

	auto& gameTexMainRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& gameTexMainCopyRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

	// The tonemap hook hands us the pass input directly, so no need to probe the bound RTV.
	// Refraction still routes through kMAIN_COPY without that being reflected in a_input.
	bool useMainCopy = isrefraction || a_input == RE::RENDER_TARGETS::kMAIN_COPY;

	auto gameTexMain = useMainCopy ? gameTexMainCopyRT : gameTexMainRT;
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };
	BeginLinearProcessing(lastTexColor);
	auto gameTexMainAlt = useMainCopy ? gameTexMainRT : gameTexMainCopyRT;

	// update auto-enabled features
	for (auto& pipe : pipeline) {
		if (pipe && pipe->IsAutoEnabled())
			pipe->UpdateAutoEnabled();
	}

	// go through each fx
	bool colorGraded = false;
	for (auto& pipe : pipeline) {
		if (pipe && !bypass && pipe->IsActive() && !pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && (!pipe->DrawBeforeUpscaling() || !upscaling.loaded)) {
			auto* input = lastTexColor.tex;
			DrawFeature(*pipe, lastTexColor);
			if (pipe == pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)])
				colorGraded = lastTexColor.tex != input;
		}
	}

	for (auto& pipe : pipeline) {
		if (!bypass && pipe && pipe->IsActive() && pipe->DrawAfterColorGrading() && !(inMainLoadingMenu && pipe->DisableInMainLoadingMenu()) && (!pipe->DrawBeforeUpscaling() || !upscaling.loaded)) {
			DrawFeature(*pipe, lastTexColor);
		}
	}

	Texture2D* mainConvertTex = texCopyMain.get();
	Texture2D* mainCopyConvertTex = texCopyMainCopy ? texCopyMainCopy.get() : texCopyMain.get();

	const bool sceneOutput = globals::state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing;
	const auto sceneGamut = ll.IsACEScgActive() ? Gamut::ACEScg : Gamut::Rec709;
	const auto displayGamut = globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR ? Gamut::Rec2020 : Gamut::Rec709;
	const CopyCB conversion{
		.inputGamut = colorGraded ? displayGamut : sceneGamut,
		.outputGamut = sceneOutput ? sceneGamut : displayGamut,
		.gamma = sceneOutput && !linear ? Util::ColorSpace::GAME_GAMMA_INV : 1.f
	};
	CopyToRenderTarget(gameTexMain, useMainCopy ? mainCopyConvertTex : mainConvertTex, lastTexColor.tex, lastTexColor.srv, conversion);
	CopyToRenderTarget(gameTexMainAlt, useMainCopy ? mainConvertTex : mainCopyConvertTex, gameTexMain.texture, gameTexMain.SRV, CopyCB{});

	isrefraction = false;
}

void PostProcessing::ClearBorderMotionVectorsForFrameGen()
{
	// Effects11 owns the image, so no letterbox is drawn and zeroing its motion vectors
	// would hand frame generation a band of static pixels over live scene content.
	if (bypass || IsTonemapOwnedByEffects11())
		return;

	auto borderIdx = static_cast<size_t>(FeaturePipelineIndex::Border);
	auto& pipe = pipeline[borderIdx];
	if (pipe && pipe->enabled) {
		auto* border = static_cast<Border*>(pipe.get());
		border->ClearMotionVectorsForFrameGen();
	}
}

bool PostProcessing::WantsTonemapOwnership() const
{
	if (globals::features::linearLighting.IsLinearLightingActive())
		return !globals::state->IsMainOrLoadingMenuOpen();
	return !bypass;
}

bool PostProcessing::IsTonemapOwnedByEffects11() const
{
	return globals::state->GetTonemapOwner() == State::TonemapOwner::kEffects11;
}

bool PostProcessing::WantsAutoHDR() const
{
	if (globals::state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing)
		return false;

	auto* colorGrading = static_cast<const ColorGrading*>(pipeline[static_cast<size_t>(FeaturePipelineIndex::ColorGrading)].get());
	return colorGrading && colorGrading->IsActive() && colorGrading->WantsAutoHDR();
}

void PostProcessing::Prepass()
{
	if (!pendingSettings.empty()) {
		logger::debug("Processing pending post processing settings...");
		ProcessSettings(pendingSettings);
		pendingSettings = {};
	}

	{
		auto graphicsState = globals::game::graphicsState;
		const float aspect = graphicsState && graphicsState->screenHeight > 0 ?
		                         (float)graphicsState->screenWidth / (float)graphicsState->screenHeight :
		                         16.0f / 9.0f;
		const bool runnable = !bypass && !IsTonemapOwnedByEffects11() && !globals::state->IsMainOrLoadingMenuOpen();
		cinematicCamera.Update(runnable, aspect);
	}

	// Update gameISData
	const auto ImageSpace = RE::ImageSpaceManager::GetSingleton();
	const auto& iSRuntimeData = ImageSpace->GetRuntimeData();
	imageSpaceManager->gameISData = iSRuntimeData.data;
	if (const auto& overrideBaseData = iSRuntimeData.overrideBaseData) {
		imageSpaceManager->gameISData.baseData = *overrideBaseData;
	} else {
		imageSpaceManager->gameISData.baseData = *iSRuntimeData.currentBaseData;
	}
}

void PostProcessing::PostPostLoad()
{
	logger::info("Hooking preprocess passes");
	DoF::InstallHooks();
	stl::write_vfunc<0x2, BSImagespaceShaderRefraction_SetupTechnique>(RE::VTABLE_BSImagespaceShaderRefraction[0]);
}
