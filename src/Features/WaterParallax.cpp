#include "WaterParallax.h"

void WaterParallax::DrawSettings()
{
}

void WaterParallax::Draw(const RE::BSShader*, const uint32_t)
{
}

void WaterParallax::SetupResources()
{
}

void WaterParallax::Load(json& o_json)
{
	Feature::Load(o_json);
}

void WaterParallax::Save(json&)
{
}

void WaterParallax::RestoreDefaultSettings()
{
}

bool WaterParallax::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}