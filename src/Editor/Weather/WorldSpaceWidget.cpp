#include "WorldSpaceWidget.h"

WorldSpaceWidget::~WorldSpaceWidget()
{
}

std::string WorldSpaceWidget::GetName()
{
	return std::format("{} ({:08X})", name, worldSpace->GetFormID());
}

void WorldSpaceWidget::DrawWidget()
{
}
