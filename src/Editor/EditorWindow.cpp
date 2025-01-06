#include "EditorWindow.h"

#include "State.h"

void EditorWindow::ShowObjectsWindow()
{
	ImGui::Begin("Object List");
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
	ImGui::Begin("Tabs View");

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
	auto width = ImGui::GetIO().DisplaySize.x;
	width /= 3.0f;

	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, ImGui::GetIO().DisplaySize.y));
	ShowObjectsWindow();

	ImGui::SetNextWindowPos(ImVec2(width, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, ImGui::GetIO().DisplaySize.y));
	ShowViewportWindow();

	ImGui::SetNextWindowPos(ImVec2(width + width, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, ImGui::GetIO().DisplaySize.y));
	ShowWidgetWindow();
}

void EditorWindow::SetupResources()
{
	auto dataHandler = RE::TESDataHandler::GetSingleton();
	auto& weatherArray = dataHandler->GetFormArray<RE::TESWeather>();

	for (auto weather : weatherArray)
	{
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