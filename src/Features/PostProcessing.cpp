#include "PostProcessing.h"

#include "Util.h"

void PostProcessing::DrawSettings()
{
	// 0 for list of modules
	// 1 for module settings
	static int pageNum = 0;

	if (ImGui::BeginTable("Page Select", 2)) {
		ImGui::TableNextColumn();
		ImGui::RadioButton("Effect List", &pageNum, 0);
		ImGui::TableNextColumn();
		ImGui::RadioButton("Effect Settings", &pageNum, 1);

		ImGui::EndTable();
	}

	static int moduleIdx = 0;
	if (pageNum == 0) {
		ImGui::Separator();

		if (ImGui::BeginCombo("Add Module", "...Select")) {
			const auto& moduleConstructors = PostProcessModuleConstructor::GetModuleConstructors();

			for (auto& [id, modCon] : moduleConstructors) {
				if (ImGui::Selectable(modCon.name.c_str()))
					modules.push_back(std::unique_ptr<PostProcessModule>{ modCon.fn() });
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(modCon.desc.c_str());
			}

			ImGui::EndCombo();
		}

		if (ImGui::BeginListBox("##Modules", { -FLT_MIN, -FLT_MIN })) {
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));  // I hate this
			for (int i = 0; i < modules.size(); ++i) {
				auto& mod = modules[i];
				if (ImGui::Selectable(mod->GetName().c_str(), moduleIdx == i))
					moduleIdx = i;
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(mod->GetDesc().c_str());
			}
			ImGui::PopStyleColor();

			ImGui::EndListBox();
		}
	} else {
		if (moduleIdx < modules.size()) {
			auto& mod = modules[moduleIdx];
			ImGui::SeparatorText(mod->GetName().c_str());
			mod->DrawSettings();
		} else {
			ImGui::Separator();
			ImGui::TextDisabled("Please select an effect in the effect list to continue.");
		}
	}
}

void PostProcessing::LoadSettings(json& o_json)
{
	const auto& moduleConstructors = PostProcessModuleConstructor::GetModuleConstructors();

	for (auto& item : o_json) {
		auto itemType = item["type"].get<std::string>();
		if (moduleConstructors.contains(itemType)) {
			modules.push_back(std::unique_ptr<PostProcessModule>{ moduleConstructors.at(itemType).fn() });
			modules.back()->LoadSettings(item["settings"]);
		}
	}
}

void PostProcessing::SaveSettings(json&)
{
}

void PostProcessing::RestoreDefaultSettings()
{
	for (auto& module : modules)
		module->RestoreDefaultSettings();
}

void PostProcessing::ClearShaderCache()
{
	for (auto& module : modules)
		module->ClearShaderCache();
}