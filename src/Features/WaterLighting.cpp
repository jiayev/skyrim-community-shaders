#include "WaterLighting.h"

#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>

void WaterLighting::SetupResources()
{
	auto& device = State::GetSingleton()->device;
	auto& context = State::GetSingleton()->context;

	DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\WaterLighting\\WaterLighting.dds", nullptr, &causticsView);
}

void WaterLighting::Prepass()
{
	auto& context = State::GetSingleton()->context;
	context->PSSetShaderResources(70, 1, &causticsView);
}

bool WaterLighting::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}
