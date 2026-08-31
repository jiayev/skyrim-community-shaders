#include "LinearLighting.h"

#include "RE/N/NiPointLight.h"

#include "../I18n/I18n.h"
#include "State.h"
#include "Util.h"

#include "Effects11.h"
#include "Effects11/SettingManager.h"
#include "Globals.h"
#include "Hooks.h"
#include "InverseSquareLighting/Common.h"
#include "ShaderCache.h"
#include "Utils/Game.h"

#include "JiayeStatement.h"

#define I18N_KEY_PREFIX "feature.linear_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LinearLighting::Settings,
	enableLinearLighting,
	enableACEScg,
	colorEncoding,
	vanillaTextureEncoding,
	vanillaDiffuseColorMult,
	directionalLightMult,
	pointLightMult,
	ambientMult,
	emitColorMult,
	glowmapMult,
	effectLightingMult,
	membraneEffectMult,
	bloodEffectMult,
	projectedEffectMult,
	deferredEffectMult,
	otherEffectMult)

namespace
{
	constexpr float GAME_GAMMA = 1.6f;
	constexpr std::uint8_t RGB_MASK = 0b0111;
	constexpr std::array CONSTANT_GROUP_NAMES{ std::string_view{ "PerTechnique" }, std::string_view{ "PerMaterial" }, std::string_view{ "PerGeometry" } };

	enum class ShaderStage : std::uint8_t
	{
		Vertex,
		Pixel
	};

	enum class ColorTransform : std::uint8_t
	{
		Standard,
		SRGBComposition,
		Emissive,
		PointLights
	};

	struct GammaToLinearLUT
	{
		static constexpr std::size_t Size = 256;

		std::array<float, Size> srgb{};
		std::array<float, Size> gameGamma{};

		GammaToLinearLUT()
		{
			for (std::size_t i = 0; i < Size; ++i) {
				const float encoded = static_cast<float>(i) / static_cast<float>(Size - 1);
				srgb[i] = encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
				gameGamma[i] = std::pow(encoded, GAME_GAMMA);
			}
		}
	};

	struct ColorField
	{
		RE::BSShader::Type shaderType;
		ShaderStage stage;
		RE::BSGraphics::ConstantGroupLevel group;
		std::uint8_t variableIndex;
		std::string_view variableName;
		std::uint8_t elementCount;
		std::uint8_t componentMask;
		std::uint32_t requiredDescriptor;
		std::uint32_t forbiddenDescriptor;
		std::uint32_t excludedTechniques = 0;
		ColorTransform transform = ColorTransform::Standard;
		std::uint32_t includedTechniques = 0;
	};

	struct MappedColorBuffer
	{
		ID3D11Resource* resource;
		void* data;
		std::size_t byteWidth;
		RE::BSShader::Type shaderType;
		ShaderStage stage;
		RE::BSGraphics::ConstantGroupLevel group;
		std::uint32_t descriptor;
		void* shaderObject;
		const std::int8_t* constantTable;
		std::size_t constantTableSize;
		float emissiveMult;
		std::array<ColorManagement::ColorSpace, 8> lightColorSpaces;
	};

	struct LightColorBackup
	{
		RE::NiLight* light;
		RE::NiColor color;
	};

	constexpr std::array COLOR_FIELDS{
		ColorField{ RE::BSShader::Type::Lighting, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerTechnique, 14, "FogNearColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Lighting, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerTechnique, 15, "FogFarColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Lighting, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 23, "TintColor", 1, RGB_MASK, 0, 0, 0, ColorTransform::Standard, 1u << static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderTechniques::Hair) },
		ColorField{ RE::BSShader::Type::Lighting, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 25, "SpecularColor", 1, RGB_MASK, static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Specular), 0 },
		ColorField{ RE::BSShader::Type::Lighting, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 13, "ProjectedUVParams2", 1, RGB_MASK, static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV), static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr), 1u << static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderTechniques::MultiIndexSparkle) },
		ColorField{ RE::BSShader::Type::Lighting, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 8, "EmitColor", 1, RGB_MASK, 0, 0, 0, ColorTransform::Emissive },
		ColorField{ RE::BSShader::Type::Lighting, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 2, "PointLightColor", 7, RGB_MASK, 0, 0, 0, ColorTransform::PointLights },

		ColorField{ RE::BSShader::Type::DistantTree, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerTechnique, 1, "AmbientColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Sky, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 3, "BlendColor", 3, RGB_MASK, 0, 0, 0, ColorTransform::SRGBComposition },
		ColorField{ RE::BSShader::Type::Particle, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 8, "Color1", 1, RGB_MASK, 0, 0, (1u << 1) | (1u << 3) },
		ColorField{ RE::BSShader::Type::Particle, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 9, "Color2", 1, RGB_MASK, 0, 0, (1u << 1) | (1u << 3) },
		ColorField{ RE::BSShader::Type::Particle, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 10, "Color3", 1, RGB_MASK, 0, 0, (1u << 1) | (1u << 3) },

		ColorField{ RE::BSShader::Type::Effect, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerTechnique, 5, "FogNearColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Effect, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerTechnique, 6, "FogFarColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Effect, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 15, "BaseColor", 1, RGB_MASK, 0, static_cast<std::uint32_t>(SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor) },
		ColorField{ RE::BSShader::Type::Effect, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 0, "PropertyColor", 1, RGB_MASK, 0, static_cast<std::uint32_t>(SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor) },
		ColorField{ RE::BSShader::Type::Effect, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerGeometry, 2, "MembraneRimColor", 1, RGB_MASK, 0, 0 },

		ColorField{ RE::BSShader::Type::Water, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 9, "VSFogNearColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Water, ShaderStage::Vertex, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 10, "VSFogFarColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Water, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 1, "ShallowColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Water, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 2, "DeepColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Water, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 3, "ReflectionColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Water, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 12, "FogNearColor", 1, RGB_MASK, 0, 0 },
		ColorField{ RE::BSShader::Type::Water, ShaderStage::Pixel, RE::BSGraphics::ConstantGroupLevel::PerMaterial, 13, "FogFarColor", 1, RGB_MASK, 0, 0 },
	};

	thread_local std::vector<MappedColorBuffer> mappedColorBuffers;
	thread_local std::vector<std::vector<LightColorBackup>> passLightColorBackups;

	float DecodeLUT(float value, const std::array<float, GammaToLinearLUT::Size>& lut)
	{
		if (!std::isfinite(value) || value == 0.0f)
			return value;

		const float sign = std::signbit(value) ? -1.0f : 1.0f;
		const float magnitude = std::abs(value);
		if (magnitude > 1.0f)
			return sign * magnitude;

		const float position = magnitude * static_cast<float>(GammaToLinearLUT::Size - 1);
		const auto lower = static_cast<std::size_t>(position);
		const auto upper = std::min(lower + 1, GammaToLinearLUT::Size - 1);
		return sign * std::lerp(lut[lower], lut[upper], position - static_cast<float>(lower));
	}

	void DecodeToLinearSRGB(float* color, ColorManagement::Encoding encoding)
	{
		static const GammaToLinearLUT lut;
		switch (encoding) {
		case ColorManagement::Encoding::SRGB:
			for (std::size_t i = 0; i < 3; ++i) {
				const float value = color[i];
				if (std::abs(value) <= 1.0f) {
					color[i] = DecodeLUT(value, lut.srgb);
				} else {
					const float magnitude = std::abs(value);
					const float linear = magnitude <= 0.04045f ? magnitude / 12.92f : std::pow((magnitude + 0.055f) / 1.055f, 2.4f);
					color[i] = std::copysign(linear, value);
				}
			}
			break;
		case ColorManagement::Encoding::GameGamma:
			for (std::size_t i = 0; i < 3; ++i) {
				const float value = color[i];
				color[i] = std::abs(value) <= 1.0f ? DecodeLUT(value, lut.gameGamma) : std::copysign(std::pow(std::abs(value), GAME_GAMMA), value);
			}
			break;
		case ColorManagement::Encoding::Linear:
			break;
		}
	}

	void EncodeLinearSRGB(float* color)
	{
		for (std::size_t i = 0; i < 3; ++i) {
			const float value = color[i];
			if (!std::isfinite(value) || value == 0.0f)
				continue;

			const float magnitude = std::abs(value);
			const float encoded = magnitude <= 0.0031308f ? magnitude * 12.92f : 1.055f * std::pow(magnitude, 1.0f / 2.4f) - 0.055f;
			color[i] = std::copysign(encoded, value);
		}
	}

	void ConvertToSRGBComposition(float* color, ColorManagement::Encoding sourceEncoding)
	{
		if (sourceEncoding == ColorManagement::Encoding::SRGB)
			return;

		DecodeToLinearSRGB(color, sourceEncoding);
		EncodeLinearSRGB(color);
	}

	std::uint32_t GetShaderTechnique(RE::BSShader::Type shaderType, std::uint32_t descriptor)
	{
		switch (shaderType) {
		case RE::BSShader::Type::Lighting:
			return (descriptor >> 24) & 0x3F;
		case RE::BSShader::Type::Water:
			return (descriptor >> 11) & 0xF;
		case RE::BSShader::Type::Sky:
			return descriptor & 0xFF;
		default:
			return descriptor;
		}
	}

	void ConvertSRGBToAP1(float* color)
	{
		const float x = 0.4123907993f * color[0] + 0.3575843394f * color[1] + 0.1804807884f * color[2];
		const float y = 0.2126390059f * color[0] + 0.7151686788f * color[1] + 0.0721923154f * color[2];
		const float z = 0.0193308187f * color[0] + 0.1191947798f * color[1] + 0.9505321522f * color[2];

		color[0] = 1.6410233797f * x - 0.3248032942f * y - 0.2364246952f * z;
		color[1] = -0.6636628587f * x + 1.6153315917f * y + 0.0167563477f * z;
		color[2] = 0.0117218943f * x - 0.0082844420f * y + 0.9883948585f * z;
	}
}

void LinearLighting::DrawSettings()
{
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Warning, "%s", T("common.settings_managed_by_enb", "Settings are currently managed by ENB."));
			return;
		}
	}

	ImGui::Checkbox(T(TKEY("enable"), "Enable Linear Lighting"), (bool*)&settings.enableLinearLighting);
	ImGui::Checkbox(T(TKEY("enable_acescg"), "Enable ACEScg Wide Gamut"), (bool*)&settings.enableACEScg);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enable_acescg_tooltip"),
							  "Render in ACEScg color space for wider gamut and more accurate lighting.\n"
							  "Requires Linear Lighting and Post Processing enabled.\n"
							  "All sRGB-gamut textures and colors will be converted to ACEScg during shading."));

	const char* colorEncodings[] = { "sRGB", "Linear", "Game Gamma" };
	settings.colorEncoding = std::min(settings.colorEncoding, static_cast<uint>(ColorEncoding::GameGamma));
	int colorEncoding = static_cast<int>(settings.colorEncoding);
	if (ImGui::Combo(T(TKEY("color_encoding"), "Color Encoding"), &colorEncoding, colorEncodings, IM_ARRAYSIZE(colorEncodings)))
		settings.colorEncoding = static_cast<uint>(colorEncoding);
	settings.vanillaTextureEncoding = std::min(settings.vanillaTextureEncoding, static_cast<uint>(ColorEncoding::GameGamma));
	int textureEncoding = static_cast<int>(settings.vanillaTextureEncoding);
	if (ImGui::Combo(T(TKEY("vanilla_texture_encoding"), "Vanilla Texture Encoding"), &textureEncoding, colorEncodings, IM_ARRAYSIZE(colorEncodings)))
		settings.vanillaTextureEncoding = static_cast<uint>(textureEncoding);
	if (GetTextureInputEncoding() != static_cast<ColorEncoding>(settings.vanillaTextureEncoding)) {
		ImGui::SameLine();
		ImGui::TextColored(
			globals::menu->GetSettings().Theme.StatusPalette.RestartNeeded,
			"%s",
			T(TKEY("vanilla_texture_encoding_restart"), "Restart required"));
	}

	if (ImGui::BeginTabBar("##LinearLightingTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
			ImGui::SeparatorText(T(TKEY("multipliers"), "Multipliers"));
			ImGui::SliderFloat(T(TKEY("directional_light_multiplier"), "Directional Light Multiplier"), &settings.directionalLightMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), &settings.ambientMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_multiplier"), "Glowmap Multiplier"), &settings.glowmapMult, 0.0f, 10.0f, "%.2f");

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_advanced"), "Advanced"))) {
			ImGui::SeparatorText(T(TKEY("multipliers"), "Multipliers"));
			ImGui::SliderFloat(T(TKEY("vanilla_diffuse_color_multiplier"), "Vanilla Diffuse Color Multiplier"), &settings.vanillaDiffuseColorMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("emissive_color_multiplier"), "Emissive Color Multiplier"), &settings.emitColorMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("point_light_multiplier"), "Point Light Multiplier"), &settings.pointLightMult, 0.0f, 10.0f, "%.2f");

			if (ImGui::TreeNodeEx(T(TKEY("effects"), "Effects"), ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat(T(TKEY("effect_lighting_multiplier"), "Effect Lighting Multiplier"), &settings.effectLightingMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("membrane_effects_multiplier"), "Membrane Effects Multiplier"), &settings.membraneEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("blood_effects_multiplier"), "Blood Effects Multiplier"), &settings.bloodEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("projected_effects_multiplier"), "Projected Effects Multiplier"), &settings.projectedEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("deferred_effects_multiplier"), "Deferred Effects Multiplier"), &settings.deferredEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("other_effects_multiplier"), "Other Effects Multiplier"), &settings.otherEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::TreePop();
			}

			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

void LinearLighting::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.colorEncoding = std::min(settings.colorEncoding, static_cast<uint>(ColorEncoding::GameGamma));
	settings.vanillaTextureEncoding = std::min(settings.vanillaTextureEncoding, static_cast<uint>(ColorEncoding::GameGamma));
}

void LinearLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LinearLighting::RestoreDefaultSettings()
{
	settings = {};
}

void LinearLighting::SetupResources()
{
	if (textureInputEncodingCaptured)
		return;

	startupTextureInputEncoding = static_cast<ColorEncoding>(
		std::min(settings.vanillaTextureEncoding, static_cast<uint>(ColorEncoding::GameGamma)));
	textureInputEncodingCaptured = true;
}

LinearLighting::PerFrameData LinearLighting::GetCommonBufferData()
{
	auto data = PerFrameData{};
	data.vanillaDiffuseColorMult = 1.0f;
	data.directionalLightMult = 1.0f;
	data.pointLightMult = 1.0f;
	data.ambientMult = 1.0f;
	data.glowmapMult = 1.0f;
	data.effectLightingMult = 1.0f;
	data.membraneEffectMult = 1.0f;
	data.bloodEffectMult = 1.0f;
	data.projectedEffectMult = 1.0f;
	data.deferredEffectMult = 1.0f;
	data.otherEffectMult = 1.0f;
	if (!loaded) {
		return data;
	}
	data.enableLinearLighting = IsColorManagementEnabled();
	data.enableACEScg = settings.enableACEScg && data.enableLinearLighting;

	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.enableLinearLighting = false;
			data.enableACEScg = false;
		}
	}

	if (!data.enableLinearLighting)
		return data;

	data.vanillaDiffuseColorMult = settings.vanillaDiffuseColorMult;
	data.directionalLightMult = RE::NI_PI * settings.directionalLightMult;
	data.pointLightMult = RE::NI_PI * settings.pointLightMult;
	data.ambientMult = settings.ambientMult;
	data.glowmapMult = settings.glowmapMult;
	data.effectLightingMult = settings.effectLightingMult;
	data.membraneEffectMult = settings.membraneEffectMult;
	data.bloodEffectMult = settings.bloodEffectMult;
	data.projectedEffectMult = settings.projectedEffectMult;
	data.deferredEffectMult = settings.deferredEffectMult;
	data.otherEffectMult = settings.otherEffectMult;
	return data;
}

bool LinearLighting::IsColorManagementEnabled() const
{
	if (!loaded || !settings.enableLinearLighting || !globals::shaderCache || !globals::shaderCache->IsEnabled() || globals::state->IsMainOrLoadingMenuOpen())
		return false;

	return !globals::features::effects11.loaded || !globals::features::effects11.enableEffect;
}

LinearLighting::ColorEncoding LinearLighting::GetColorEncoding() const
{
	const auto encoding = static_cast<ColorEncoding>(settings.colorEncoding);
	return encoding <= ColorEncoding::GameGamma ? encoding : ColorEncoding::SRGB;
}

ColorManagement::ColorSpace LinearLighting::GetInputColorSpace() const
{
	return { GetColorEncoding(), ColorManagement::Gamut::SRGB };
}

LinearLighting::ColorEncoding LinearLighting::GetTextureInputEncoding() const
{
	return startupTextureInputEncoding;
}

ColorManagement::ColorSpace LinearLighting::GetLightColorSpace(const RE::NiLight* light) const
{
	if (light) {
		if (const auto it = lightColorSpaceOverrides.find(light); it != lightColorSpaceOverrides.end()) {
			const auto& diffuse = light->GetLightRuntimeData().diffuse;
			if (diffuse.red == it->second.value.red && diffuse.green == it->second.value.green && diffuse.blue == it->second.value.blue)
				return it->second.space;
		}

		if (const auto pointLight = skyrim_cast<RE::NiPointLight*>(const_cast<RE::NiLight*>(light));
			pointLight && ISLCommon::RuntimeLightDataExt::Get(pointLight)->flags.any(LightLimitFix::LightFlags::Linear))
			return ColorManagement::LinearSRGB;
	}

	return GetInputColorSpace();
}

void LinearLighting::ConvertColorToWorkingSpace(float* color, ColorManagement::ColorSpace sourceSpace) const
{
	if (!IsColorManagementEnabled())
		return;

	DecodeToLinearSRGB(color, sourceSpace.encoding);

	if (sourceSpace.gamut == ColorManagement::Gamut::SRGB && settings.enableACEScg)
		ConvertSRGBToAP1(color);
}

RE::NiColor LinearLighting::ConvertColorToWorkingSpace(RE::NiColor color, ColorManagement::ColorSpace sourceSpace) const
{
	ConvertColorToWorkingSpace(&color.red, sourceSpace);
	return color;
}

void LinearLighting::DecodeColor(float* color) const
{
	ConvertColorToWorkingSpace(color, GetInputColorSpace());
}

RE::NiColor LinearLighting::DecodeColor(RE::NiColor color) const
{
	return ConvertColorToWorkingSpace(color, GetInputColorSpace());
}

void LinearLighting::ConvertLightColorToWorkingSpace(const RE::NiLight* light, float* color) const
{
	ConvertColorToWorkingSpace(color, GetLightColorSpace(light));
}

RE::NiColor LinearLighting::ConvertLightColorToWorkingSpace(const RE::NiLight* light, RE::NiColor color) const
{
	ConvertLightColorToWorkingSpace(light, &color.red);
	return color;
}

void LinearLighting::SetLightColor(RE::NiLight* light, ColorManagement::ColorValue color)
{
	if (!light)
		return;

	light->GetLightRuntimeData().diffuse = color.value;
	lightColorSpaceOverrides.insert_or_assign(light, LightColorSpaceOverride{ color.value, color.space });
}

void LinearLighting::ClearLightColorSpace(const RE::NiLight* light)
{
	if (!light)
		return;

	lightColorSpaceOverrides.erase(light);
}

void LinearLighting::BeginPassColorManagement(RE::BSRenderPass* pass, RE::BSShader::Type shaderType)
{
	auto& backups = passLightColorBackups.emplace_back();
	const auto shaderClass = static_cast<std::size_t>(shaderType) - 1;
	if (!IsColorManagementEnabled() || shaderClass >= std::size(globals::state->enabledClasses) || !globals::state->enabledClasses[shaderClass] || !pass || !pass->sceneLights)
		return;

	const bool directionalOnly = shaderType == RE::BSShader::Type::Lighting;
	const std::uint32_t lightCount = directionalOnly ? std::min<std::uint32_t>(pass->numLights, 1) : pass->numLights;
	backups.reserve(lightCount);
	for (std::uint32_t index = 0; index < lightCount; ++index) {
		auto* light = pass->sceneLights[index] ? pass->sceneLights[index]->light.get() : nullptr;
		if (!light || std::find_if(backups.begin(), backups.end(), [light](const auto& backup) { return backup.light == light; }) != backups.end())
			continue;
		const bool alreadyManaged = std::any_of(passLightColorBackups.begin(), std::prev(passLightColorBackups.end()), [light](const auto& outerBackups) {
			return std::any_of(outerBackups.begin(), outerBackups.end(), [light](const auto& backup) { return backup.light == light; });
		});
		if (alreadyManaged)
			continue;

		auto& diffuse = light->GetLightRuntimeData().diffuse;
		backups.push_back({ light, diffuse });
		diffuse = ConvertLightColorToWorkingSpace(light, diffuse);
	}
}

void LinearLighting::EndPassColorManagement()
{
	if (passLightColorBackups.empty())
		return;

	for (const auto& backup : passLightColorBackups.back())
		backup.light->GetLightRuntimeData().diffuse = backup.color;
	passLightColorBackups.pop_back();
}

void LinearLighting::TrackMappedColorBuffer(ID3D11Resource* resource, D3D11_MAPPED_SUBRESOURCE* mappedResource)
{
	if (!IsColorManagementEnabled() || !resource || !mappedResource || !mappedResource->pData || !globals::state->currentShader ||
		(!globals::state->customVertexShader && !globals::state->customPixelShader))
		return;

	const auto shaderType = globals::state->currentShader->shaderType.get();

	auto track = [&](auto* sourceShader, void* customShader, ShaderStage stage, std::uint32_t descriptor) {
		if (!sourceShader || !customShader)
			return false;

		for (std::size_t groupIndex = 0; groupIndex < 3; ++groupIndex) {
			if (sourceShader->constantBuffers[groupIndex].buffer != reinterpret_cast<REX::W32::ID3D11Buffer*>(resource))
				continue;
			D3D11_BUFFER_DESC desc{};
			static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);

			mappedColorBuffers.push_back({ resource,
				mappedResource->pData,
				desc.ByteWidth,
				shaderType,
				stage,
				static_cast<RE::BSGraphics::ConstantGroupLevel>(groupIndex),
				descriptor,
				customShader,
				sourceShader->constantTable.data(),
				sourceShader->constantTable.size(),
				currentEmissiveMult,
				currentLightColorSpaces });
			return true;
		}

		return false;
	};

	if (globals::game::currentVertexShader &&
		track(*globals::game::currentVertexShader,
			globals::state->customVertexShader ? globals::state->customVertexShader->shader : nullptr,
			ShaderStage::Vertex,
			globals::state->currentVertexDescriptor))
		return;

	if (globals::game::currentPixelShader && globals::state->customPixelShader)
		track(*globals::game::currentPixelShader, globals::state->customPixelShader->shader, ShaderStage::Pixel, globals::state->currentPixelDescriptor);
}

void LinearLighting::ConvertMappedColorBuffer(ID3D11Resource* resource)
{
	const auto mapped = std::find_if(mappedColorBuffers.rbegin(), mappedColorBuffers.rend(), [resource](const auto& entry) {
		return entry.resource == resource;
	});
	if (mapped == mappedColorBuffers.rend())
		return;

	const MappedColorBuffer buffer = *mapped;
	mappedColorBuffers.erase(std::next(mapped).base());
	if (!IsColorManagementEnabled())
		return;

	for (const auto& field : COLOR_FIELDS) {
		const auto technique = GetShaderTechnique(buffer.shaderType, buffer.descriptor);
		if (field.shaderType != buffer.shaderType || field.stage != buffer.stage || field.group != buffer.group ||
			(field.requiredDescriptor && (buffer.descriptor & field.requiredDescriptor) != field.requiredDescriptor) ||
			(field.forbiddenDescriptor && (buffer.descriptor & field.forbiddenDescriptor)) ||
			(field.includedTechniques && (technique >= 32 || !(field.includedTechniques & (1u << technique)))) ||
			(technique < 32 && (field.excludedTechniques & (1u << technique))))
			continue;

		if (field.variableIndex >= buffer.constantTableSize)
			continue;
		const auto groupIndex = static_cast<std::size_t>(field.group);
		if (groupIndex >= CONSTANT_GROUP_NAMES.size() || !Hooks::HasShaderConstant(buffer.shaderObject, CONSTANT_GROUP_NAMES[groupIndex], field.variableName))
			continue;

		const auto offset = buffer.constantTable[field.variableIndex];
		if (offset < 0)
			continue;

		for (std::size_t element = 0; element < field.elementCount; ++element) {
			const std::size_t floatOffset = static_cast<std::size_t>(offset) + element * 4;
			if ((floatOffset + 4) * sizeof(float) > buffer.byteWidth)
				break;

			auto* value = static_cast<float*>(buffer.data) + floatOffset;
			if (field.transform == ColorTransform::SRGBComposition) {
				ConvertToSRGBComposition(value, GetColorEncoding());
			} else if (field.transform == ColorTransform::Emissive) {
				if (buffer.emissiveMult != 0.0f) {
					for (std::size_t component = 0; component < 3; ++component)
						value[component] /= buffer.emissiveMult;
				}
				DecodeColor(value);
				for (std::size_t component = 0; component < 3; ++component)
					value[component] *= buffer.emissiveMult * settings.emitColorMult;
			} else if (field.transform == ColorTransform::PointLights) {
				ConvertColorToWorkingSpace(value, buffer.lightColorSpaces[element + 1]);
			} else if (field.componentMask == RGB_MASK) {
				DecodeColor(value);
			}
		}
	}
}

void LinearLighting::PrepareLightColorManagement(RE::BSRenderPass* a_pass)
{
	currentLightColorSpaces.fill(GetInputColorSpace());
	if (!IsColorManagementEnabled() || !a_pass || !a_pass->sceneLights)
		return;

	const auto lightCount = std::min<std::uint32_t>(a_pass->numLights, static_cast<std::uint32_t>(currentLightColorSpaces.size()));
	for (std::uint32_t index = 0; index < lightCount; ++index) {
		auto* light = a_pass->sceneLights[index] ? a_pass->sceneLights[index]->light.get() : nullptr;
		currentLightColorSpaces[index] = GetLightColorSpace(light);
	}
}

void LinearLighting::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	PrepareLightColorManagement(a_pass);
	currentEmissiveMult = 1.0f;
	auto& property1 = a_pass->geometry->GetGeometryRuntimeData().shaderProperty;
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;

	if (lightProperty && IsColorManagementEnabled())
		currentEmissiveMult = lightProperty->emissiveMult;
}

#undef I18N_KEY_PREFIX
