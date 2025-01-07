#include "EditorWindow.h"

#include "State.h"

void EditorWindow::ShowObjectsWindow()
{
	ImGui::Begin("Object List", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);  // Center window

	// Create two columns: left for categories, right for objects
	ImGui::Columns(2, nullptr, true);

	// Static variable to track the selected category
	static std::string selectedCategory = "Weathers";

	// Left column: Categories
	{
		ImGui::BeginChild("Categories", ImVec2(0, 0), true);

		// List of categories
		const char* categories[] = { "Weathers", "Clouds" };
		for (int i = 0; i < IM_ARRAYSIZE(categories); ++i) {
			// Highlight the selected category
			if (ImGui::Selectable(categories[i], selectedCategory == categories[i])) {
				selectedCategory = categories[i];  // Update selected category
			}
		}

		ImGui::EndChild();
	}

	// Switch to the right column
	ImGui::NextColumn();

	// Right column: Objects
	{
		ImGui::BeginChild("Objects", ImVec2(0, 0), true);

		// Display objects based on the selected category
		if (selectedCategory == "Weathers") {
			for (int i = 0; i < widgets.size(); ++i) {
				if (ImGui::Selectable(widgets[i]->GetName().c_str())) {
					// Push to front
					for (auto it = activeWidgets.begin(); it != activeWidgets.end();) {
						if ((*it) == widgets[i]) {
							it = activeWidgets.erase(it);
						} else {
							++it;
						}
					}
					activeWidgets.push_back(widgets[i]);
				}
			}
		} else if (selectedCategory == "Clouds") {
			ImGui::Text("Not implemented");
		}

		ImGui::EndChild();
	}

	// End the window
	ImGui::End();
}

void EditorWindow::ShowViewportWindow()
{
	ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);  // Center window

	if (ImGui::BeginTabBar("ViewportTabBar")) {
		if (ImGui::BeginTabItem("Viewport")) {
			// Top bar
			if (ImGui::BeginChild("ViewportTopBar", ImVec2(0, 40), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove)) {
				auto calendar = RE::Calendar::GetSingleton();
				if (calendar) {
					ImGui::Text("Time :");
					ImGui::SameLine();
					ImGui::SliderFloat("##ViewportSlider", &calendar->gameHour->value, 0.0f, 23.99f, "Value: %.2f");
				}
			}
			ImGui::EndChild();

			// The size of the image in ImGui																														   // Get the available space in the current window
			ImVec2 availableSpace = ImGui::GetContentRegionAvail();

			// Calculate aspect ratio of the image
			float aspectRatio = ImGui::GetIO().DisplaySize.x / ImGui::GetIO().DisplaySize.y;

			// Determine the size to fit while preserving the aspect ratio
			ImVec2 imageSize;
			if (availableSpace.x / availableSpace.y < aspectRatio) {
				// Fit width
				imageSize.x = availableSpace.x;
				imageSize.y = availableSpace.x / aspectRatio;
			} else {
				// Fit height
				imageSize.y = availableSpace.y;
				imageSize.x = availableSpace.y * aspectRatio;
			}

			ImGui::Image((void*)tempTexture->srv.get(), imageSize);
			ImGui::EndTabItem();
			ImGui::EndTabBar();
		}
	}

	if (ImGui::BeginTabBar("ViewportTabBar2")) {
		if (ImGui::BeginTabItem("Other")) {
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	ImGui::End();
}

void EditorWindow::ShowWidgetWindow()
{
	ImGui::Begin("Tabs View", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);  // Center window

	if (ImGui::BeginTabBar("##tabs")) {
		std::unordered_set<Widget*> widgetsToRemove;

		for (int i = (int)activeWidgets.size() - 1; i > -1; i--) {
			auto widget = activeWidgets[i];
			if (ImGui::BeginTabItem(widget->GetName().c_str())) {
				if (ImGui::Button("Close Tab"))
					widgetsToRemove.insert(widget);
				widget->DrawWidget();
				ImGui::EndTabItem();
			}
		}

		// Use a manual loop to remove elements
		for (auto it = activeWidgets.begin(); it != activeWidgets.end();) {
			if (widgetsToRemove.count(*it)) {
				it = activeWidgets.erase(it);
			} else {
				++it;
			}
		}

		ImGui::EndTabBar();
	}

	ImGui::End();
}

void EditorWindow::RenderUI()
{
	// Ensure global alpha is fully opaque
	ImGui::GetStyle().Alpha = 1.0f;

	// Set an opaque background color
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;

	auto width = ImGui::GetIO().DisplaySize.x;
	auto viewportWidth = width * 0.5f;                // Make the viewport take up 50% of the width
	auto sideWidth = (width - viewportWidth) / 2.0f;  // Divide the remaining width equally between the side windows

	// Left window (Objects)
	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(sideWidth, ImGui::GetIO().DisplaySize.y));
	ShowObjectsWindow();

	// Middle window (Viewport, larger size)
	ImGui::SetNextWindowPos(ImVec2(sideWidth, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(viewportWidth, ImGui::GetIO().DisplaySize.y));
	ShowViewportWindow();

	// Right window (Widget)
	ImGui::SetNextWindowPos(ImVec2(sideWidth + viewportWidth, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(sideWidth, ImGui::GetIO().DisplaySize.y));
	ShowWidgetWindow();
}

void EditorWindow::SetupResources()
{
	auto dataHandler = RE::TESDataHandler::GetSingleton();
	auto& weatherArray = dataHandler->GetFormArray<RE::TESWeather>();

	for (auto weather : weatherArray) {
		std::string editorID = weather->GetFormEditorID();
		auto widget = new WeatherWidget(weather);
		widgets.push_back(widget);
	}
}

void EditorWindow::Draw()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& framebuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];

	ID3D11Resource* resource;
	framebuffer.SRV->GetResource(&resource);

	if (!tempTexture) {
		D3D11_TEXTURE2D_DESC texDesc{};
		((ID3D11Texture2D*)resource)->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		framebuffer.SRV->GetDesc(&srvDesc);

		tempTexture = new Texture2D(texDesc);
		tempTexture->CreateSRV(srvDesc);
	}

	auto& device = State::GetSingleton()->context;

	device->CopyResource(tempTexture->resource.get(), resource);

	RenderUI();
}