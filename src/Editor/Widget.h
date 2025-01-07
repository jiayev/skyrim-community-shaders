
#pragma once

class Widget
{
public:
	virtual std::string GetName() = 0;

	virtual void DrawWidget() = 0;

	bool open = false;

	bool IsOpen()
	{
		return open;
	}
};