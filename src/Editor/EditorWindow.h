#pragma once

#include "Buffer.h"
#include "Widget.h"
#include "Weather/WeatherWidget.h"

class EditorWindow
{
public:
	static EditorWindow* GetSingleton()
	{
		static EditorWindow singleton;
		return &singleton;
	}

	Texture2D* tempTexture;

	std::vector<Widget*> widgets;
	std::vector<Widget*> activeWidgets;

	void ShowObjectsWindow();

	void ShowViewportWindow();

	void ShowWidgetWindow();

	void RenderUI();

	void SetupResources();

	void Draw();
};