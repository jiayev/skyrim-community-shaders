#pragma once

#include "Buffer.h"

#include "Widget.h"
#include "Weather/WeatherWidget.h"
#include "Weather/WorldSpaceWidget.h"
#include "Weather/CloudsWidget.h"

class EditorWindow
{
public:
	static EditorWindow* GetSingleton()
	{
		static EditorWindow singleton;
		return &singleton;
	}

	Texture2D* tempTexture;

	std::vector<Widget*> weatherWidgets;
	std::vector<Widget*> worldSpaceWidgets;
	std::vector<Widget*> cloudsWidgets;

	void ShowObjectsWindow();

	void ShowViewportWindow();

	void ShowWidgetWindow();

	void RenderUI();

	void SetupResources();

	void Draw();
};