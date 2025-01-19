#include "PBRSkin.h"

#include "Hooks.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    PBRSkin::Settings,
    EnablePBRSkin,
    EnablePBRHair,
    SkinRoughnessScale,
    SkinSpecularLevel,
    SkinSpecularTexMultiplier,
    HairRoughnessScale,
    HairSpecularLevel,
    HairSpecularTexMultiplier
)

void PBRSkin::DrawSettings()
{
    ImGui::Checkbox("Enable Skin", &settings.EnablePBRSkin);
    ImGui::SliderFloat("Skin Roughness", &settings.SkinRoughnessScale, 0.f, 1.f, "%.3f");
    ImGui::SliderFloat("Skin Specular Level", &settings.SkinSpecularLevel, 0.f, 0.1f, "%.4f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("This \"Specular\" here is the same as the \"Specular\" in the PBR material object data. 0.028 is the default value for skin.");
    ImGui::SliderFloat("Skin Specular Texture Multiplier", &settings.SkinSpecularTexMultiplier, 0.f, 10.f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("A multiplier for the vanilla specular texture. It will be used to modify the roughness of the skin.");

    ImGui::Checkbox("Enable Hair", &settings.EnablePBRHair);
    ImGui::SliderFloat("Hair Roughness", &settings.HairRoughnessScale, 0.f, 1.f, "%.3f");
    ImGui::SliderFloat("Hair Specular Level", &settings.HairSpecularLevel, 0.f, 0.1f, "%.4f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("This \"Specular\" here is the same as the \"Specular\" in the PBR material object data. 0.045 is the default value for hair.");
    ImGui::SliderFloat("Hair Specular Texture Multiplier", &settings.HairSpecularTexMultiplier, 0.f, 10.f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("A multiplier for the vanilla specular texture. It will be used to modify the roughness of the hair.");
}

PBRSkin::PBRSkinData PBRSkin::GetCommonBufferData()
{
    PBRSkinData data{};
    data.skinParams = float4(settings.SkinRoughnessScale, settings.SkinSpecularLevel, settings.SkinSpecularTexMultiplier, float(settings.EnablePBRSkin));
    data.hairParams = float4(settings.HairRoughnessScale, settings.HairSpecularLevel, settings.HairSpecularTexMultiplier, float(settings.EnablePBRHair));
    return data;
}

void PBRSkin::LoadSettings(json& o_json)
{
    settings = o_json;
}

void PBRSkin::SaveSettings(json& o_json)
{
    o_json = settings;
}

void PBRSkin::RestoreDefaultSettings()
{
    settings = {};
}

struct BSLightingShaderProperty_GetRenderPasses
{
    static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, std::uint32_t renderFlags, RE::BSShaderAccumulator* accumulator)
    {
        auto renderPasses = func(property, geometry, renderFlags, accumulator);
		if (renderPasses == nullptr) {
			return renderPasses;
		}
        auto currentPass = renderPasses->head;
        while (currentPass != nullptr) {
			if (currentPass->shader->shaderType == RE::BSShader::Type::Lighting) {
				constexpr uint32_t LightingTechniqueStart = 0x4800002D;
				auto lightingTechnique = currentPass->passEnum - LightingTechniqueStart;
				auto lightingFlags = lightingTechnique & ~(~0u << 24);
                auto lightingType = static_cast<SIE::ShaderCache::LightingShaderTechniques>((lightingTechnique >> 24) & 0x3F);
				lightingFlags &= ~0b111000u;
                if (PBRSkin::GetSingleton()->settings.EnablePBRHair && property->material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint) {
					lightingFlags |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::PbrHS);
				}
				if (PBRSkin::GetSingleton()->settings.EnablePBRSkin && (property->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGen || property->material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint)) {
					lightingFlags |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::PbrHS);
				}
                lightingTechnique = (static_cast<uint32_t>(lightingType) << 24) | lightingFlags;
				currentPass->passEnum = lightingTechnique + LightingTechniqueStart;
			}
			currentPass = currentPass->next;
		}
        return renderPasses;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

struct BSLightingShader_SetupGeometry
{
    static void thunk(RE::BSLightingShader* shader, RE::BSRenderPass* pass, uint32_t renderFlags)
    {
        const auto originalTechnique = shader->currentRawTechnique;
        if (PBRSkin::GetSingleton()->settings.EnablePBRSkin)
        {
            shader->currentRawTechnique |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::PbrHS);
        }
        if (PBRSkin::GetSingleton()->settings.EnablePBRHair)
        {
            shader->currentRawTechnique |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::PbrHS);
        }
        func(shader, pass, renderFlags);
        shader->currentRawTechnique = originalTechnique;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

void PBRSkin::PostPostLoad()
{
    logger::info("[PBR Skin] BSLightingShaderProperty_GetRenderPasses hooking");
    stl::write_vfunc<0x2A, BSLightingShaderProperty_GetRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
    logger::info("[PBR Skin] BSLightingShader_SetupGeometry hooking");
    stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
}