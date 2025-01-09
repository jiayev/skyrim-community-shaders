#pragma once

#include "../Widget.h"

class WorldSpaceWidget : public Widget
{
public:
	RE::TESWorldSpace* worldSpace = nullptr;

	WorldSpaceWidget(RE::TESWorldSpace* a_worldspace)
	{
		worldSpace = a_worldspace;

		name = worldSpace->GetFormEditorID();
	}

	~WorldSpaceWidget();

	WorldSpaceWidget(const WorldSpaceWidget& other)
	{
		worldSpace = other.worldSpace;

		name = std::format("{} COPY", other.name);
	}

	virtual Widget* Clone() const override
	{
		return new WorldSpaceWidget(*this);  // Return a copy of this object
	}

	virtual int GetID()
	{
		return (int)worldSpace->GetFormID();
	}

	virtual void DrawWidget() override;
};