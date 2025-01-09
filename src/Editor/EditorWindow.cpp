#include "EditorWindow.h"

#include "State.h"

void EditorWindow::ShowObjectsWindow()
{
	ImGui::Begin("Object List");

	// Static variable to track the selected category
	static std::string selectedCategory = "Weathers";

	// Static variable to track renaming
	static int renameIndex = -1;
	static char renameBuffer[256] = "";

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
		const char* categories[] = { "Weathers", "WorldSpace", "Clouds" };
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

		bool openContextMenu = false;

		// Create a table for the right column with "Name" and "ID" headers
		if (ImGui::BeginTable("DetailsTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			// Display objects based on the selected category
			auto& widgets = selectedCategory == "Weathers"   ? weatherWidgets :
			                selectedCategory == "WorldSpace" ? worldSpaceWidgets :
			                                                   cloudsWidgets;

			for (int i = 0; i < widgets.size(); ++i) {
				ImGui::TableNextRow();

				// Name column
				ImGui::TableSetColumnIndex(1);
				ImGui::Text(std::format("{:08X}", widgets[i]->GetID()).c_str());

				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(widgets[i]->GetID());

				if (renameIndex == i) {
					if (ImGui::InputText("##rename", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
						widgets[i]->SetName(renameBuffer);  // Apply new name
						renameIndex = -1;                   // Exit rename mode
					}
				} else {
					if (ImGui::Selectable(widgets[i]->GetName().c_str(), widgets[i]->open, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
						if (ImGui::IsMouseDoubleClicked(0)) {
							widgets[i]->open = true;
						}
					}
					// Open context menu for the item
					if (ImGui::BeginPopupContextItem(("ItemContextMenu" + std::to_string(i)).c_str())) {
						openContextMenu = true;
						if (ImGui::MenuItem("New")) {
							// Add a new widget based on the selected category
							if (selectedCategory == "Weathers") {
							} else if (selectedCategory == "WorldSpace") {
							} else if (selectedCategory == "Clouds") {
								cloudsWidgets.push_back(new CloudsWidget());
							}
						}
						if (ImGui::MenuItem("Duplicate")) {
							Widget* duplicateWidget = widgets[i]->Clone();
							widgets.push_back(duplicateWidget);
						}
						if (ImGui::MenuItem("Rename")) {
							renameIndex = i;  // Enter rename mode
							strncpy(renameBuffer, widgets[i]->GetEditableName().c_str(), sizeof(renameBuffer));
							renameBuffer[sizeof(renameBuffer) - 1] = '\0';  // Ensure null termination
						}
						if (ImGui::MenuItem("Delete")) {
							widgets.erase(widgets.begin() + i);
							--i;  // Adjust index after deletion
						}
						ImGui::EndPopup();
					}
				}

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		// Open context menu in the empty space
		if (!openContextMenu) {
			if (ImGui::BeginPopupContextWindow("BackgroundContextMenu", ImGuiPopupFlags_MouseButtonRight)) {
				if (ImGui::MenuItem("New")) {
					if (selectedCategory == "Weathers") {
					} else if (selectedCategory == "WorldSpace") {
					} else if (selectedCategory == "Clouds") {
						cloudsWidgets.push_back(new CloudsWidget());
					}
				}
				ImGui::EndPopup();
			}
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
	for (int i = 0; i < (int)weatherWidgets.size(); i++) {
		auto widget = weatherWidgets[i];
		if (widget->IsOpen()) {
			auto width = ImGui::GetIO().DisplaySize.x;
			auto viewportWidth = width * 0.5f;                // Make the viewport take up 50% of the width
			auto sideWidth = (width - viewportWidth) / 2.0f;  // Divide the remaining width equally between the side windows
			ImGui::SetNextWindowSize(ImVec2(sideWidth, ImGui::GetIO().DisplaySize.y));
			if (ImGui::Begin(widget->GetNameWithID().c_str(), &widget->open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) {
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

	for (int i = 0; i < (int)worldSpaceWidgets.size(); i++) {
		auto widget = worldSpaceWidgets[i];
		if (widget->IsOpen()) {
			auto width = ImGui::GetIO().DisplaySize.x;
			auto viewportWidth = width * 0.5f;                // Make the viewport take up 50% of the width
			auto sideWidth = (width - viewportWidth) / 2.0f;  // Divide the remaining width equally between the side windows
			ImGui::SetNextWindowSize(ImVec2(sideWidth, ImGui::GetIO().DisplaySize.y));
			if (ImGui::Begin(widget->GetNameWithID().c_str(), &widget->open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) {
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

	for (int i = 0; i < (int)cloudsWidgets.size(); i++) {
		auto widget = cloudsWidgets[i];
		if (widget->IsOpen()) {
			auto width = ImGui::GetIO().DisplaySize.x;
			auto viewportWidth = width * 0.5f;                // Make the viewport take up 50% of the width
			auto sideWidth = (width - viewportWidth) / 2.0f;  // Divide the remaining width equally between the side windows
			ImGui::SetNextWindowSize(ImVec2(sideWidth, ImGui::GetIO().DisplaySize.y));
			if (ImGui::Begin(widget->GetNameWithID().c_str(), &widget->open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) {
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
		auto widget = new WeatherWidget(weather);
		weatherWidgets.push_back(widget);
	}

	auto& worldSpaceArray = dataHandler->GetFormArray<RE::TESWorldSpace>();

	for (auto worldSpace : worldSpaceArray) {
		auto widget = new WorldSpaceWidget(worldSpace);
		worldSpaceWidgets.push_back(widget);
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