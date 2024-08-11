#include "PostProcessing.h"

#include "IconsFontAwesome5.h"
#include "imgui_stdlib.h"

#include "State.h"
#include "Util.h"

constexpr auto def_settings = R"(
"effects": [
	{
    "name": "Bloom",
    "settings": {
     "BlendFactor": 0.05000000074505806,
     "MipBlendFactor": [
      1.0,
      1.0,
      1.0,
      1.0,
      1.0,
      1.0,
      1.0,
      1.0
     ],
     "Threshold": -6.0,
     "UpsampleRadius": 2.0
    },
    "type": "COD Bloom"
   },
   {
    "name": "Auto Exposure",
    "settings": {
     "AdaptArea": [
      0.6000000238418579,
      0.6000000238418579
     ],
     "AdaptSpeed": 1.0,
     "AdaptationRange": [
      0.0,
      2.0
     ],
     "ExposureCompensation": 0.0,
     "PurkinjeMaxEV": -5.0,
     "PurkinjeStartEV": -3.0,
     "PurkinjeStrength": 0.5
    },
    "type": "Histogram Auto Exposure"
   },
   {
    "name": "Vignette",
    "settings": {
     "FocalLength": 1.0,
     "Power": 3.0
    },
    "type": "Vignette"
   },
   {
    "name": "Tonemapper",
    "settings": {
     "Params": {
      "Params0": [
       3.160165548324585,
       0.0,
       0.0,
       0.0
      ],
      "Params1": [
       0.0,
       0.0,
       0.0,
       0.0
      ],
      "Params2": [
       0.0,
       0.0,
       0.0,
       0.0
      ],
      "Params3": [
       0.0,
       0.0,
       0.0,
       0.0
      ]
     },
     "TransformType": "Melon"
    },
    "type": "Colour Transforms"
   }
])";

void PostProcessing::DrawSettings()
{
	// 0 for list of feats
	// 1 for feat settings
	static int pageNum = 0;
	static int featIdx = 0;
	const float _iconButtonSize = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.x;
	const ImVec2 iconButtonSize{ _iconButtonSize, _iconButtonSize };

	if (ImGui::BeginTable("Page Select", 2)) {
		ImGui::TableNextColumn();
		ImGui::RadioButton("Effect List", &pageNum, 0);
		ImGui::TableNextColumn();
		ImGui::RadioButton("Effect Settings", &pageNum, 1);

		ImGui::EndTable();
	}

	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Spacing();
	ImGui::Spacing();

	if (pageNum == 0) {
		// Effect List

		if (ImGui::Button(ICON_FA_PLUS, iconButtonSize))
			ImGui::OpenPopup("New Feature");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Add a new effect.");

		if (ImGui::BeginPopup("New Feature", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
			bool doClose = false;
			if (ImGui::BeginListBox("##Feature List")) {
				const auto& featConstructors = PostProcessFeatureConstructor::GetFeatureConstructors();

				for (auto& [id, featCon] : featConstructors) {
					if (ImGui::Selectable(featCon.name.c_str())) {
						feats.push_back(std::unique_ptr<PostProcessFeature>{ featCon.fn() });
						feats.back()->name = feats.back()->GetType();

						auto bogey = json::object();
						feats.back()->LoadSettings(bogey);
						feats.back()->SetupResources();

						featIdx = (int)feats.size() - 1;

						doClose = true;
					}
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text(featCon.desc.c_str());
				}

				ImGui::EndListBox();
			}
			if (doClose)
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		ImGui::Dummy({ ImGui::GetTextLineHeightWithSpacing() * 2, 1 });
		ImGui::SameLine();

		ImGui::Checkbox("Bypass", &bypass);

		ImGui::Spacing();

		int markedFeat = -1;
		int actionType = -1;  // 0 - remove, 1 - move up, 2 - move down
		if (ImGui::BeginListBox("##Features", { -FLT_MIN, -FLT_MIN })) {
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));  // I hate this
			for (int i = 0; i < feats.size(); ++i) {
				ImGui::PushID(i);

				auto& feat = feats[i];

				bool nonVR = REL::Module::IsVR() && !feat->SupportsVR();

				ImGui::Checkbox("##Enabled", &feat->enabled);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Enabled/Bypassed");

				ImGui::SameLine();
				if (ImGui::Button(ICON_FA_TIMES, iconButtonSize)) {
					markedFeat = i;
					actionType = 0;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Remove the selected effect.");

				ImGui::SameLine();
				if (ImGui::Button(ICON_FA_ARROW_UP, iconButtonSize) && (i != 0)) {
					markedFeat = i;
					actionType = 1;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Move the selected effect up.");

				ImGui::SameLine();
				if (ImGui::Button(ICON_FA_ARROW_DOWN, iconButtonSize) && (i < feats.size() - 1)) {
					markedFeat = i;
					actionType = 2;
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Move the selected effect down.");

				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();

				if (nonVR)
					ImGui::BeginDisabled();
				if (ImGui::Selectable(std::format("{} ({})", feat->name, feat->GetType()).c_str(), featIdx == i))
					featIdx = i;
				if (nonVR)
					ImGui::EndDisabled();

				if (auto _tt = Util::HoverTooltipWrapper())
					if (nonVR)
						ImGui::Text("Bypassed due to no VR support.");
					else
						ImGui::Text(feat->GetDesc().c_str());

				ImGui::PopID();
			}
			ImGui::PopStyleColor();

			ImGui::EndListBox();
		}

		if (markedFeat >= 0 && actionType >= 0) {
			switch (actionType) {
			case 0:
				feats.erase(feats.begin() + markedFeat);
				break;
			case 1:
				std::iter_swap(feats.begin() + markedFeat, feats.begin() + markedFeat - 1);
				if (markedFeat == featIdx)
					featIdx--;
				else if (markedFeat - 1 == featIdx)
					featIdx++;
				break;
			case 2:
				std::iter_swap(feats.begin() + markedFeat, feats.begin() + markedFeat + 1);
				if (markedFeat == featIdx)
					featIdx++;
				else if (markedFeat + 1 == featIdx)
					featIdx--;
				break;
			default:
				break;
			}
		}

	} else if (pageNum == 1) {
		// Effect Settings

		if (featIdx < feats.size()) {
			auto& feat = feats[featIdx];
			ImGui::InputText("Name", &feat->name);

			ImGui::SeparatorText(std::format("{} ({})", feat->name, feat->GetType()).c_str());

			ImGui::TextWrapped(feat->GetDesc().c_str());

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			ImGui::PushID(featIdx);
			feat->DrawSettings();
			ImGui::PopID();
		} else {
			ImGui::TextDisabled("Please select an effect in the effect list to continue.");
		}
	}
}

void PostProcessing::LoadSettings(json& o_json)
{
	const auto& featConstructors = PostProcessFeatureConstructor::GetFeatureConstructors();

	logger::info("Loading post processing settings...");

	auto effects = o_json["effects"];
	if (!effects.is_array())
		effects = json::parse(def_settings);

	feats.clear();

	for (auto& item : o_json["effects"]) {
		auto currFeatCount = feats.size();
		try {
			auto itemType = item["type"].get<std::string>();
			if (featConstructors.contains(itemType)) {
				PostProcessFeature* feat = featConstructors.at(itemType).fn();
				feat->name = item["name"].get<std::string>();
				feat->LoadSettings(item["settings"]);
				if (loaded)
					feat->SetupResources();  // to prevent double setup after loaded

				feats.push_back(std::unique_ptr<PostProcessFeature>{ feat });

				logger::info("Loaded {}({}).", feat->name, feat->GetType());
			} else {
				logger::warn("Invalid post processing feature type \"{}\" detected in settings.", itemType);
			}
		} catch (json::exception& e) {
			logger::error("Error occured while parsing post processing settings: {}", e.what());
			if (feats.size() > currFeatCount)
				feats.pop_back();
		}
	}
}

void PostProcessing::SaveSettings(json& o_json)
{
	auto arr = json::array();

	for (auto& feat : feats) {
		json temp_json{};
		feat->SaveSettings(temp_json);
		arr.push_back({
			{ "type", feat->GetType() },
			{ "name", feat->name },
			{ "settings", temp_json },
		});
	}

	o_json["effects"] = arr;
}

void PostProcessing::RestoreDefaultSettings()
{
	auto bogus = json();
	LoadSettings(bogus);
}

void PostProcessing::ClearShaderCache()
{
	for (auto& feat : feats)
		if (!REL::Module::IsVR() || feat->SupportsVR())
			feat->ClearShaderCache();
}

void PostProcessing::SetupResources()
{
	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

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

		texCopy = eastl::make_unique<Texture2D>(texDesc);
		texCopy->CreateUAV(uavDesc);
	}

	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PostProcessing\\copy.cs.hlsl", {}, "cs_5_0")))
		copyCS.attach(rawPtr);

	for (auto& feat : feats)
		if (!REL::Module::IsVR() || feat->SupportsVR())
			feat->SetupResources();
}

void PostProcessing::Reset()
{
	for (auto& feat : feats)
		if (!REL::Module::IsVR() || feat->SupportsVR())
			feat->Reset();
}

void PostProcessing::PreProcess()
{
	if (bypass)
		return;

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = State::GetSingleton()->context;

	auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };

	// go through each fx
	for (auto& feat : feats)
		if (feat->enabled && (!REL::Module::IsVR() || feat->SupportsVR()))
			feat->Draw(lastTexColor);

	D3D11_TEXTURE2D_DESC desc;
	lastTexColor.tex->GetDesc(&desc);
	if (desc.Format == texCopy->desc.Format) {
		// either MAIN_COPY or MAIN is used as input for HDR pass
		// so we copy to both so whatever the game wants we're not failing it
		context->CopySubresourceRegion(gameTexMain.texture, 0, 0, 0, 0, lastTexColor.tex, 0, nullptr);
		context->CopySubresourceRegion(
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY].texture,
			0, 0, 0, 0, lastTexColor.tex, 0, nullptr);
	} else {
		ID3D11ShaderResourceView* srv = lastTexColor.srv;
		ID3D11UnorderedAccessView* uav = texCopy->uav.get();

		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetShader(copyCS.get(), nullptr, 0);
		context->Dispatch((texCopy->desc.Width + 7) >> 3, (texCopy->desc.Height + 7) >> 3, 1);

		srv = nullptr;
		uav = nullptr;

		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetShader(nullptr, nullptr, 0);

		context->CopySubresourceRegion(gameTexMain.texture, 0, 0, 0, 0, texCopy->resource.get(), 0, nullptr);
		context->CopySubresourceRegion(
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY].texture,
			0, 0, 0, 0, texCopy->resource.get(), 0, nullptr);
	}
}

void PostProcessing::PostPostLoad()
{
	logger::info("Hooking preprocess passes");
	stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematic_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematic[0]);
	stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematicFade_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematicFade[0]);
}