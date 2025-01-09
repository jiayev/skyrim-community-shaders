
#pragma once

class WidgetSharedData
{
private:
	int uniqueID = 0;

public:
	static WidgetSharedData* GetSingleton()
	{
		static WidgetSharedData sharedData;
		return &sharedData;
	}

	int GetNewID()
	{
		return -uniqueID++;
	}
};

class Widget
{
public:
	virtual ~Widget(){};

	virtual Widget* Clone() const = 0;

	std::string name = "";

	virtual std::string GetName()
	{
		return name;
	}

	virtual void SetName(std::string a_newName)
	{
		name = a_newName;
	}

	virtual int GetID() = 0;

	virtual std::string GetNameWithID()
	{
		return std::format("{}###{}", GetName(), GetID());
	}

	virtual std::string GetWindowTitle()
	{
		return std::format("{} ({:08X})", GetName(), GetID());

	}

	virtual std::string GetWindowTitleWithID()
	{
		return std::format("{}###{}", GetWindowTitle(), GetID());
	}

	virtual void DrawWidget() = 0;

	bool open = false;

	bool IsOpen() const
	{
		return open;
	}
};