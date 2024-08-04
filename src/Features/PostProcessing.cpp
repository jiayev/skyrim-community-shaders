#include "PostProcessing.h"

#include "IconsFontAwesome5.h"
#include "imgui_stdlib.h"

#include "Util.h"

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

	if (pageNum == 0) {
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
	} else {
		if (featIdx < feats.size()) {
			auto& feat = feats[featIdx];
			ImGui::InputText("Name", &feat->name);

			ImGui::SeparatorText(feat->name.c_str());

			feat->DrawSettings();
		} else {
			ImGui::Separator();
			ImGui::TextDisabled("Please select an effect in the effect list to continue.");
		}
	}
}

void PostProcessing::LoadSettings(json& o_json)
{
	const auto& featConstructors = PostProcessFeatureConstructor::GetFeatureConstructors();

	logger::info("Loading post processing settings...");

	for (auto& item : o_json) {
		auto currFeatCount = feats.size();
		try {
			auto itemType = item["type"].get<std::string>();
			if (featConstructors.contains(itemType)) {
				PostProcessFeature* feat = featConstructors.at(itemType).fn();
				feat->name = item["name"].get<std::string>();
				feat->LoadSettings(item["settings"]);

				feats.push_back(std::unique_ptr<PostProcessFeature>{ feat });
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
	o_json = json::array();

	for (auto& feat : feats) {
		json temp_json{};
		feat->SaveSettings(temp_json);
		o_json.push_back({
			{ "type", feat->GetType() },
			{ "name", feat->name },
			{ "settings", temp_json },
		});
	}
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
