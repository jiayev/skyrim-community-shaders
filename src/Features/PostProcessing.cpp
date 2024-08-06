#include "PostProcessing.h"

#include "IconsFontAwesome5.h"
#include "imgui_stdlib.h"

#include "State.h"
#include "Util.h"

void PostProcessing::DrawSettings()
{
	// 0 for list of feats
	// 1 for feat settings
	static int pageNum = 0;
	static int featIdx = 0;
	const float _iconButtonSize = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.x;
	const ImVec2 iconButtonSize{ _iconButtonSize, _iconButtonSize };

	if (ImGui::BeginTable("Page Select", 3)) {
		ImGui::TableNextColumn();
		ImGui::RadioButton("Effect List", &pageNum, 0);
		ImGui::TableNextColumn();
		ImGui::RadioButton("Effect Settings", &pageNum, 1);
		ImGui::TableNextColumn();
		ImGui::RadioButton("LUT Baker", &pageNum, 2);

		ImGui::EndTable();
	}

	ImGui::SeparatorText("");

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

		if (ImGui::Button(ICON_FA_MINUS, iconButtonSize) && (featIdx < feats.size()))
			feats.erase(feats.begin() + featIdx);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Remove the selected effect.");

		ImGui::SameLine();
		ImGui::Dummy({ ImGui::GetTextLineHeightWithSpacing() * 2, 1 });
		ImGui::SameLine();

		if (ImGui::Button(ICON_FA_ARROW_UP, iconButtonSize) && (featIdx < feats.size()) && (featIdx != 0)) {
			std::iter_swap(feats.begin() + featIdx, feats.begin() + featIdx - 1);
			featIdx--;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Move the selected effect up.");

		ImGui::SameLine();

		if (ImGui::Button(ICON_FA_ARROW_DOWN, iconButtonSize) && (featIdx < feats.size() - 1)) {
			std::iter_swap(feats.begin() + featIdx, feats.begin() + featIdx + 1);
			featIdx++;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Move the selected effect down.");

		ImGui::Spacing();

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
				ImGui::AlignTextToFramePadding();

				if (nonVR)
					ImGui::BeginDisabled();
				if (ImGui::Selectable(feat->name.c_str(), featIdx == i))
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

			feat->DrawSettings();
		} else {
			ImGui::TextDisabled("Please select an effect in the effect list to continue.");
		}
	} else {
		// LUT Baker

		ImGui::TextDisabled("UNDER CONSTRUCTION");
	}
}

void PostProcessing::LoadSettings(json& o_json)
{
	const auto& featConstructors = PostProcessFeatureConstructor::GetFeatureConstructors();

	logger::info("Loading post processing settings...");

	auto effects = o_json["effects"];
	if (!effects.is_array())
		effects = json::parse(R"(
		[
            {
                "name": "COD Bloom",
                "settings": {},
                "type": "COD Bloom"
            },
            {
                "name": "Histogram Auto Exporsure",
                "settings": {},
                "type": "Histogram Auto Exporsure"
            },
            {
                "name": "Math Tonemapper",
                "settings": {},
                "type": "Math Tonemapper"
            }
        ])");

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
					feat->SetupResources();

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
	for (auto& feat : feats)
		if (!REL::Module::IsVR() || feat->SupportsVR())
			feat->RestoreDefaultSettings();
}

void PostProcessing::ClearShaderCache()
{
	for (auto& feat : feats)
		if (!REL::Module::IsVR() || feat->SupportsVR())
			feat->ClearShaderCache();
}

void PostProcessing::SetupResources()
{
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
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = State::GetSingleton()->context;

	auto gameTexMain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	PostProcessFeature::TextureInfo lastTexColor = { gameTexMain.texture, gameTexMain.SRV };

	// go through each fx
	for (auto& feat : feats)
		if (feat->enabled && (!REL::Module::IsVR() || feat->SupportsVR()))
			feat->Draw(lastTexColor);

	// either MAIN_COPY or MAIN is used as input for HDR pass
	// so we copy to both so whatever the game wants we're not failing it
	context->CopySubresourceRegion(gameTexMain.texture, 0, 0, 0, 0, lastTexColor.tex, 0, nullptr);
	context->CopySubresourceRegion(
		renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY].texture,
		0, 0, 0, 0, lastTexColor.tex, 0, nullptr);
}

void PostProcessing::PostPostLoad()
{
	logger::info("Hooking preprocess passes");
	stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematic_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematic[0]);
	stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematicFade_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematicFade[0]);
}