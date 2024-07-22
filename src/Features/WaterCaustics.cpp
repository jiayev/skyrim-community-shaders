#include "WaterCaustics.h"

#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>

void WaterCaustics::DrawSettings()
{
}

void WaterCaustics::SetupResources()
{
	auto& device = State::GetSingleton()->device;
	auto& context = State::GetSingleton()->context;

	DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\WaterCaustics\\watercaustics.dds", nullptr, &causticsView);
}

void WaterCaustics::Prepass()
{
	auto& context = State::GetSingleton()->context;
	context->PSSetShaderResources(70, 1, &causticsView);
}

void WaterCaustics::Load(json& o_json)
{
	Feature::Load(o_json);
}

void WaterCaustics::Save(json&)
{
}

void WaterCaustics::RestoreDefaultSettings()
{
}

bool WaterCaustics::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}