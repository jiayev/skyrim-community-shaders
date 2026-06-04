#include "PhysicalLighting.h"

#include "Features/InverseSquareLighting/Common.h"
#include "I18n/I18n.h"

#include <algorithm>
#include <numbers>

#define I18N_KEY_PREFIX "feature.physical_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalLighting::Settings,
	globalIntensityScale,
	enableAreaLights)

void PhysicalLighting::SetupResources()
{
	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DYNAMIC;
	sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	sbDesc.StructureByteStride = sizeof(PhysicalLightExt);
	sbDesc.ByteWidth = sizeof(PhysicalLightExt) * MAX_PHYSICAL_LIGHTS;
	physicalLightBuffer = eastl::make_unique<Buffer>(sbDesc, nullptr, "PhysicalLighting::Lights");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = MAX_PHYSICAL_LIGHTS;
	physicalLightBuffer->CreateSRV(srvDesc);
}

void PhysicalLighting::Prepass()
{
	if (!physicalLightBuffer)
		return;

	auto context = globals::d3d::context;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	DX::ThrowIfFailed(context->Map(physicalLightBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	memcpy_s(mapped.pData, physicalLightBuffer->desc.ByteWidth, physicalLightExtData.data(), physicalLightBuffer->desc.ByteWidth);
	context->Unmap(physicalLightBuffer->resource.get(), 0);

	ID3D11ShaderResourceView* view = physicalLightBuffer->srv.get();
	context->PSSetShaderResources(38, 1, &view);
}

void PhysicalLighting::DrawSettings()
{
	ImGui::SliderFloat(T(TKEY("global_intensity_scale"), "Global Intensity Scale"), &settings.globalIntensityScale, 0.0f, 100.0f, "%.2f");
	ImGui::Checkbox(T(TKEY("enable_area_lights"), "Enable Area Lights"), &settings.enableAreaLights);
}

void PhysicalLighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PhysicalLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PhysicalLighting::RestoreDefaultSettings()
{
	settings = {};
}

PhysicalLighting::PerFrame PhysicalLighting::GetCommonBufferData() const
{
	return {
		.globalIntensityScale = settings.globalIntensityScale,
		.enableAreaLights = static_cast<std::uint32_t>(settings.enableAreaLights),
	};
}

void PhysicalLighting::ProcessPhysicalLight(LightLimitFix::LightData& a_light, RE::NiLight* a_niLight, std::uint32_t a_lightIndex)
{
	if (a_lightIndex >= MAX_PHYSICAL_LIGHTS)
		return;

	const auto* config = FindConfig(a_niLight);
	if (!config)
		return;

	a_light.lightFlags.set(LightLimitFix::LightFlags::Physical);

	if (config->useColorTemp)
		a_light.lightFlags.set(LightLimitFix::LightFlags::UseColorTemp);

	const bool isAreaLight = settings.enableAreaLights && config->areaType != AreaType::Point;
	if (isAreaLight)
		a_light.lightFlags.set(LightLimitFix::LightFlags::AreaLight);

	auto& ext = physicalLightExtData[a_lightIndex];
	ext = {};
	ext.luminousPower = config->intensity;
	ext.unitType = static_cast<std::uint32_t>(config->unitType);
	ext.unitScale = config->unitScale;

	switch (config->unitType) {
	case UnitType::Lumen:
		ext.luminousIntensity = LumensToCandela(config->intensity);
		break;
	case UnitType::Candela:
		ext.luminousIntensity = config->intensity;
		break;
	case UnitType::Lux:
		ext.luminousIntensity = LuxToCandela(config->intensity, a_light.radius);
		break;
	}
	ext.luminousIntensity *= config->unitScale;

	ext.colorTemperature = config->colorTemperature;
	ext.tint = config->tint;
	if (config->useColorTemp)
		a_light.color = KelvinToLinearRGB(config->colorTemperature, config->tint);

	ext.areaType = static_cast<std::uint32_t>(isAreaLight ? config->areaType : AreaType::Point);
	ext.areaWidth = config->areaWidth;
	ext.areaHeight = config->areaHeight;
	ext.areaNormalize = ComputeAreaNormalization(config->areaType, config->areaWidth, config->areaHeight);
}

void PhysicalLighting::SetPhysicalLightData(RE::FormID a_formId, const PhysicalLightConfig& a_config)
{
	configByFormId.insert_or_assign(a_formId, a_config);
}

void PhysicalLighting::SetPhysicalLightData(RE::NiLight* a_niLight, const PhysicalLightConfig& a_config)
{
	if (a_niLight)
		configByPointer.insert_or_assign(a_niLight, a_config);
}

void PhysicalLighting::ClearPhysicalLightData(RE::FormID a_formId)
{
	configByFormId.erase(a_formId);
}

void PhysicalLighting::ClearPhysicalLightData(RE::NiLight* a_niLight)
{
	configByPointer.erase(a_niLight);
}

void PhysicalLighting::ClearAllPhysicalLightData()
{
	configByFormId.clear();
	configByPointer.clear();
}

float PhysicalLighting::LumensToCandela(float a_lumens)
{
	return a_lumens / (4.0f * std::numbers::pi_v<float>);
}

float PhysicalLighting::LuxToCandela(float a_lux, float a_distance)
{
	return a_lux * a_distance * a_distance;
}

float3 PhysicalLighting::KelvinToLinearRGB(float a_kelvin, float a_tint)
{
	const float t = std::clamp(a_kelvin, 1000.0f, 40000.0f);
	const float t2 = t * t;

	float x = 0.0f;
	if (t < 4000.0f)
		x = -0.2661239e9f / (t2 * t) - 0.2343589e6f / t2 + 0.8776956e3f / t + 0.179910f;
	else
		x = -3.0258469e9f / (t2 * t) + 2.1070379e6f / t2 + 0.2226347e3f / t + 0.240390f;

	const float x2 = x * x;
	float y = 0.0f;
	if (t < 2222.0f)
		y = -1.1063814f * x2 * x - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
	else if (t < 4000.0f)
		y = -0.9549476f * x2 * x - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
	else
		y = 3.0817580f * x2 * x - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;

	y = std::max(y + std::clamp(a_tint, -1.0f, 1.0f) * 0.05f, 1e-5f);

	const float Y = 1.0f;
	const float X = Y / y * x;
	const float Z = Y / y * (1.0f - x - y);

	return {
		std::max(3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z, 0.0f),
		std::max(-0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z, 0.0f),
		std::max(0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z, 0.0f),
	};
}

float PhysicalLighting::ComputeAreaNormalization(AreaType a_type, float a_width, float a_height)
{
	switch (a_type) {
	case AreaType::Sphere:
		{
			const float radius = std::max(a_width * 0.5f, 1e-4f);
			return 1.0f / (4.0f * std::numbers::pi_v<float> * radius * radius);
		}
	case AreaType::Disc:
		{
			const float radius = std::max(a_width * 0.5f, 1e-4f);
			return 1.0f / (std::numbers::pi_v<float> * radius * radius);
		}
	case AreaType::Tube:
		return 1.0f / std::max(a_width, 1e-4f);
	case AreaType::Rect:
		return 1.0f / std::max(a_width * a_height, 1e-4f);
	case AreaType::Point:
	default:
		return 1.0f;
	}
}

const PhysicalLighting::PhysicalLightConfig* PhysicalLighting::FindConfig(RE::NiLight* a_niLight) const
{
	if (!a_niLight)
		return nullptr;

	if (auto it = configByPointer.find(a_niLight); it != configByPointer.end())
		return &it->second;

	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(a_niLight);
	if (auto it = configByFormId.find(runtimeData->lighFormId); it != configByFormId.end())
		return &it->second;

	return nullptr;
}

#undef I18N_KEY_PREFIX
