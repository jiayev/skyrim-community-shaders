#pragma once

#include "Buffer.h"
#include "Weather/WeatherWidget.h"
#include "Widget.h"

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

	void ShowObjectsWindow();

	void ShowViewportWindow();

	void ShowWidgetWindow();

	void RenderUI();

	void SetupResources();

	void Draw();
};