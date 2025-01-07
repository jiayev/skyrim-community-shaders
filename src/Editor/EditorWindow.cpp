#include "EditorWindow.h"

#include "State.h"

void EditorWindow::ShowObjectsWindow()
{
	ImGui::Begin("Object List");

	// Static variable to track the selected category
	static std::string selectedCategory = "Weathers";

	// Create a table with two columns
	if (ImGui::BeginTable("ObjectTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInner | ImGuiTableFlags_NoHostExtendX)) {
		// Set up column widths
		ImGui::TableSetupColumn("Categories", ImGuiTableColumnFlags_WidthStretch, 0.3f);  // 30% width
		ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthStretch, 0.7f);     // 70% width

		ImGui::TableNextRow();

		// Left column: Categories
		ImGui::TableSetColumnIndex(0);
		ImGui::BeginChild("Categories");

		// List of categories
		const char* categories[] = { "Weathers", "Clouds" };
		for (int i = 0; i < IM_ARRAYSIZE(categories); ++i) {
			// Highlight the selected category
			if (ImGui::Selectable(categories[i], selectedCategory == categories[i])) {
				selectedCategory = categories[i];  // Update selected category
			}
		}

		ImGui::EndChild();

		// Right column: Objects
		ImGui::TableSetColumnIndex(1);
		ImGui::BeginChild("Objects");

		// Display objects based on the selected category
		if (selectedCategory == "Weathers") {
			for (int i = 0; i < widgets.size(); ++i) {
				if (ImGui::Selectable(widgets[i]->GetName().c_str(), widgets[i]->open, ImGuiSelectableFlags_AllowDoubleClick)) {
					if (ImGui::IsMouseDoubleClicked(0))
						widgets[i]->open = true;
				}
			}
		} else if (selectedCategory == "Clouds") {
			ImGui::Text("Not implemented");
		}

		ImGui::EndChild();

		ImGui::EndTable();
	}

	// End the window
	ImGui::End();
}

void EditorWindow::ShowViewportWindow()
{
	ImGui::Begin("Viewport");

	// Top bar
	auto calendar = RE::Calendar::GetSingleton();
	if (calendar)
		ImGui::SliderFloat("##ViewportSlider", &calendar->gameHour->value, 0.0f, 23.99f, "Time: %.2f");
				
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

	ImGui::End();
}

void EditorWindow::ShowWidgetWindow()
{
	for (int i = 0; i < (int)widgets.size(); i++) {
		auto widget = widgets[i];
		if (widget->IsOpen()) {
			auto width = ImGui::GetIO().DisplaySize.x;
			auto viewportWidth = width * 0.5f;                // Make the viewport take up 50% of the width
			auto sideWidth = (width - viewportWidth) / 2.0f;  // Divide the remaining width equally between the side windows
			ImGui::SetNextWindowSize(ImVec2(sideWidth, ImGui::GetIO().DisplaySize.y));
			if (ImGui::Begin(widget->GetName().c_str(), &widget->open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) 
			{		
				if (ImGui::BeginMenuBar()) {
					if (ImGui::BeginMenu("Menu")) {
						ImGui::EndMenu();
					}
					ImGui::EndMenuBar();
				}
				widget->DrawWidget();
			}
			ImGui::End();
		}
	}
}

void EditorWindow::RenderUI()
{
	ImGui::GetStyle().Alpha = 1.0f;
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& framebuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
	auto& context = State::GetSingleton()->context;

	context->ClearRenderTargetView(framebuffer.RTV, (float*)&ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);

    if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Window")) {
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help")) {
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	auto width = ImGui::GetIO().DisplaySize.x;
	auto viewportWidth = width * 0.5f;                // Make the viewport take up 50% of the width
	auto sideWidth = (width - viewportWidth) / 2.0f;  // Divide the remaining width equally between the side windows

	ImGui::SetNextWindowSize(ImVec2(sideWidth, ImGui::GetIO().DisplaySize.y * 0.75f));
	ShowObjectsWindow();

	ImGui::SetNextWindowSize(ImVec2(viewportWidth, ImGui::GetIO().DisplaySize.y * 0.5f));
	ShowViewportWindow();

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

	auto& context = State::GetSingleton()->context;

	context->CopyResource(tempTexture->resource.get(), resource);

	RenderUI();
}