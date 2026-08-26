#include "VanillaFresnel.h"
#include "I18n/I18n.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VanillaFresnel::Settings,
	Enable,
	EnableGGX,
	EnableGGXOnGrass,
	EnableDynamicCubemapsConversion,
	EnableEyeSpecialHandling,
	RoughnessMultiplier,
	SpecularRoughnessBlend,
	BaseF0Multiplier,
	MinF0,
	CubemapToF0Multiplier,
	ComplexMaterialF0Multiplier)

namespace
{
	constexpr auto IsEyeDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsEye);

	bool ContainsEye(std::string_view a_name)
	{
		constexpr std::string_view eye = "eye";
		return std::search(
				   a_name.begin(), a_name.end(),
				   eye.begin(), eye.end(),
				   [](char a_lhs, char a_rhs) {
					   return static_cast<char>(std::tolower(static_cast<unsigned char>(a_lhs))) == a_rhs;
				   }) != a_name.end();
	}

	bool IsEyePass(const RE::BSLightingShader* a_shader, const RE::BSRenderPass* a_pass)
	{
		if (a_shader) {
			const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>(0x3F & (a_shader->currentRawTechnique >> 24));
			if (technique == SIE::ShaderCache::LightingShaderTechniques::Eye) {
				return true;
			}
		}

		if (!a_pass || !a_pass->shaderProperty) {
			return false;
		}

		const auto* lightingProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ?
		                                   static_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty) :
		                                   nullptr;
		if (!lightingProperty || !lightingProperty->material) {
			return false;
		}

		const auto feature = lightingProperty->material->GetFeature();
		if (feature == RE::BSShaderMaterial::Feature::kEye) {
			return true;
		}

		return feature == RE::BSShaderMaterial::Feature::kEnvironmentMap &&
		       a_pass->geometry &&
		       ContainsEye(a_pass->geometry->name.c_str());
	}

	void UpdateEyePermutation(const RE::BSLightingShader* a_shader, const RE::BSRenderPass* a_pass)
	{
		auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
		descriptor &= ~IsEyeDescriptor;

		if (IsEyePass(a_shader, a_pass)) {
			descriptor |= IsEyeDescriptor;
		}
	}

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			UpdateEyePermutation(a_shader, a_pass);
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void VanillaFresnel::PostPostLoad()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[VanillaFresnel] Installed hooks - BSLightingShader_SetupGeometry");
}

void VanillaFresnel::RestoreDefaultSettings()
{
	settings = {};
}

void VanillaFresnel::LoadSettings(json& o_json)
{
	settings = o_json;
}

void VanillaFresnel::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VanillaFresnel::DrawSettings()
{
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_vanilla_fresnel", "Enable Vanilla Fresnel"), reinterpret_cast<bool*>(&settings.Enable));
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_phong_to_ggx", "Enable Phong to GGX"), reinterpret_cast<bool*>(&settings.EnableGGX));
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_phong_to_ggx_on_grass", "Enable Phong to GGX on Grass"), reinterpret_cast<bool*>(&settings.EnableGGXOnGrass));
	if (!settings.EnableGGX) {
		settings.EnableDynamicCubemapsConversion = false;
	}
	ImGui::BeginDisabled(!settings.EnableGGX);
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_auto_cubemaps_conversion", "Enable Auto Cubemaps Conversion"), reinterpret_cast<bool*>(&settings.EnableDynamicCubemapsConversion));
	ImGui::EndDisabled();
	ImGui::Checkbox(T("feature.vanilla_fresnel.enable_eye_special_handling", "Enable Eye Special Handling"), reinterpret_cast<bool*>(&settings.EnableEyeSpecialHandling));

	ImGui::SliderFloat(T("feature.vanilla_fresnel.roughness_multiplier", "Roughness Multiplier"), &settings.RoughnessMultiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.specular_roughness_blend", "Specular Roughness Blend"), &settings.SpecularRoughnessBlend, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.base_f0_multiplier", "Base F0 Multiplier"), &settings.BaseF0Multiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.min_f0", "Min F0"), &settings.MinF0, 0.0f, 0.04f, "%.3f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.cubemap_to_f0_multiplier", "Cubemap to F0 Multiplier"), &settings.CubemapToF0Multiplier, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat(T("feature.vanilla_fresnel.complex_material_env_f0_multiplier", "Complex Material Env F0 Multiplier"), &settings.ComplexMaterialF0Multiplier, 0.0f, 10.0f, "%.2f");
}
