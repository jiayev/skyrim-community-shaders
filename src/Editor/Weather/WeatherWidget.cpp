#include "WeatherWidget.h"

WeatherWidget::~WeatherWidget()
{
}

void WeatherWidget::DrawWidget()
{
	if (ImGui::Begin(GetEditorID().c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) {
		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("Menu")) {
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}
	}
	ImGui::End();
}
