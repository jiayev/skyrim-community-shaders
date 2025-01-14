#include "WeatherWidget.h"

#include "../EditorWindow.h"

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

		auto editorWindow = EditorWindow::GetSingleton();
		auto& widgets = editorWindow->weatherWidgets;

		if (ImGui::BeginCombo("Parent", currentParentBuffer)) {
			// Option for "None"
			if (ImGui::Selectable("None", parent == nullptr)) {
				parent = nullptr;
				strncpy(currentParentBuffer, "None", sizeof(currentParentBuffer));
			}

			for (int i = 0; i < widgets.size(); i++) {
				auto& widget = widgets[i];

				// Skip self-selection
				if (widget == this)
					continue;

				// Option for each widget
				if (ImGui::Selectable(widget->GetEditorID().c_str(), parent == widget)) {
					parent = (WeatherWidget*)widget;
					strncpy(currentParentBuffer, widget->GetEditorID().c_str(), sizeof(currentParentBuffer) - 1);
					currentParentBuffer[sizeof(currentParentBuffer) - 1] = '\0';  // Ensure null-termination
				}

				// Set default focus to the current parent
				if (parent == widget) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		if (parent && !parent->IsOpen()) {
			ImGui::SameLine();
			if (ImGui::Button("Open"))
				parent->SetOpen(true);
		}
	}
	ImGui::End();
}
