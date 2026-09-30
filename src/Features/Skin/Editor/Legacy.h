#pragma once

#include "NifDocument.h"

namespace SkinEditor
{
	std::vector<std::pair<std::string, SkinMaterials::Material>> ImportLegacy(const std::string& a_path);
}
