#pragma once

namespace SkinSources
{
	void Install();
	void Prune();
	RE::FormID Resolve(RE::BSShaderProperty* a_property, bool a_head);
}
