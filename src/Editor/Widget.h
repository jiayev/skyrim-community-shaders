
#pragma once

class Widget
{
public:
	virtual std::string GetName() = 0;

	virtual void DrawWidget() = 0;
};