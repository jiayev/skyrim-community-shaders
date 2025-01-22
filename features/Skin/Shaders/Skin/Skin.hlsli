#include "Common/PBR.hlsli"
#include "Common/Math.hlsli"

namespace Skin{
    struct SkinSurfaceProperties
    {
        float RoughnessPrimary;
        float RoughnessSecondary;
        float3 F0Primary;
        float3 F0Secondary;
        float SecondarySpecIntensity;
        float CurvatureScale;
        float3 Albedo;
        float Thickness;
    };

    SkinSurfaceProperties InitSkinSurfaceProperties()
    {
        SkinSurfaceProperties skin;
        skin.RoughnessPrimary = 0.55;
        skin.RoughnessSecondary = 0.35;
        skin.F0Primary = float3(0.02, 0.017, 0.014);
        skin.F0Secondary = float3(0.04, 0.035, 0.03);
        skin.SecondarySpecIntensity = 0.15;
        skin.CurvatureScale = 1.0;
        skin.Albedo = float3(0.8, 0.6, 0.5);
        skin.Thickness = 0.15;
        return skin;
    }

    float CalculateCurvature(float3 N)
    {
        const float3 dNdx = ddx(N);
        const float3 dNdy = ddy(N);
        return length(float2(dot(dNdx, dNdx), dot(dNdy, dNdy)));
    }

    float DisneyDiffuse(float NdotV, float NdotL, float VdotH, float roughness)
    {
        const float FD90 = 0.5 + 2.0 * VdotH * VdotH * roughness;
        const float fdv = 1.0 + (FD90 - 1.0) * pow(1.0 - NdotV, 5.0);
        const float fdl = 1.0 + (FD90 - 1.0) * pow(1.0 - NdotL, 5.0);
        
        return fdv * fdl;
    }

    float SmithG1_GGX(float NdotV, float alpha)
    {
        const float k = alpha * 0.5;
        const float numerator = NdotV;
        const float denominator = NdotV * (1.0 - k) + k;
        return numerator / (denominator + 1e-5);
    }

    float SmithG_GGX(float NdotL, float NdotV, float alpha)
    {
        return SmithG1_GGX(NdotL, alpha) * SmithG1_GGX(NdotV, alpha);
    }

    float SimplifiedBeckmannNDF(float NdotH, float roughness)
    {
        float alpha = roughness * roughness;
        alpha = clamp(alpha * 1.5 - 0.2, 0.08, 0.95);
        
        float cosTheta = NdotH;
        float tan2Theta = (1.0 - cosTheta * cosTheta) / (cosTheta * cosTheta);
        float root = tan2Theta / (alpha * alpha);
        
        float approxExp = 1.0 / (1.0 + root + 0.6 * root * root);
        return approxExp / (Math::PI * alpha * alpha * cosTheta * cosTheta * cosTheta);
    }

    float3 GetBeckmannSpecular(
        float roughness,
        float3 F0,
        float NdotL,
        float NdotV,
        float NdotH,
        float VdotH,
        out float3 F)
    {
        float D = SimplifiedBeckmannNDF(NdotH, roughness);
        F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 4.0);
        const float G = SmithG_GGX(NdotL, NdotV, roughness * roughness);
        
        return max((D * G * F) / (4.0 * NdotL * NdotV + 1e-5), 1e-5);
    }

    void SkinDirectLightInput(
        out float3 diffuse,
        out float3 specularPrimary,
        out float3 specularSecondary,
        PBR::LightProperties light,
        SkinSurfaceProperties skin,
        float3 N, float3 V, float3 L)
    {
        diffuse = 0;
        specularPrimary = 0;
        specularSecondary = 0;

        const float3 H = normalize(V + L);
        const float NdotL = clamp(dot(N, L), 1e-5, 1.0);
        const float NdotV = saturate(abs(dot(N, V)) + 1e-5);
        const float NdotH = saturate(dot(N, H));
        const float VdotH = saturate(dot(V, H));

        diffuse += light.LinearLightColor * NdotL * DisneyDiffuse(NdotV, NdotL, VdotH, skin.RoughnessPrimary) / Math::PI;

        float3 F_primary;
        specularPrimary = PBR::GetSpecularDirectLightMultiplierMicrofacet(
            skin.RoughnessPrimary, 
            skin.F0Primary,
            NdotL, NdotV, NdotH, VdotH,
            F_primary) * light.LinearLightColor * NdotL;

        float2 specularBRDF = PBR::GetEnvBRDFApproxLazarov(skin.RoughnessPrimary, NdotV);
        specularPrimary *= 1 + skin.F0Primary * (1 / (specularBRDF.x + specularBRDF.y) - 1);

        float3 F_secondary;
        const float3 specSecondary = GetBeckmannSpecular(
            skin.RoughnessSecondary,
            skin.F0Secondary,
            NdotL, NdotV, NdotH, VdotH,
            F_secondary) * light.LinearLightColor * NdotL;

        const float energyCompensation = 1.0 + skin.Thickness * 0.3;
        specularPrimary *= energyCompensation;
        specularSecondary = specSecondary * skin.SecondarySpecIntensity * energyCompensation;
    }

    float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
    {
        return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * 
            pow(1.0 - cosTheta, 5.0);
    }

    float3 SchlickFresnelAverage(float3 F0, float dielectricF0)
    {
        return F0 + (1.0 - F0) * 0.047619; // 1/21 ≈ 0.047619
    }

    void SkinIndirectLobeWeights(
        out float3 diffuseWeight,
        out float3 specularWeight,
        SkinSurfaceProperties skin,
        float3 N, float3 V, float3 VN)
    {
        const float NdotV = saturate(dot(N, V));
        
        const float2 brdfPrimary = PBR::GetEnvBRDFApproxLazarov(skin.RoughnessPrimary, NdotV);
        const float3 specPrimary = skin.F0Primary * brdfPrimary.x + brdfPrimary.y;
        
        const float2 brdfSecondary = PBR::GetEnvBRDFApproxLazarov(skin.RoughnessSecondary, NdotV);
        const float3 specSecondary = skin.F0Secondary * brdfSecondary.x + brdfSecondary.y;
        
        specularWeight = specPrimary + specSecondary * skin.SecondarySpecIntensity;
        specularWeight *= 1.0 + skin.Thickness * 0.2;

        diffuseWeight = skin.Albedo * (1.0 - FresnelSchlickRoughness(NdotV, 0.04, skin.RoughnessPrimary)) / Math::PI;
        
        const float curvature = CalculateCurvature(N);
        specularWeight *= 1.0 - saturate(curvature * skin.CurvatureScale);

        specularWeight *= 1 + skin.F0Primary * (1 / (brdfPrimary.x + brdfPrimary.y) - 1) + skin.F0Secondary * (1 / (brdfSecondary.x + brdfSecondary.y) - 1);

        float3 R = reflect(-V, N);
        float horizon = min(1.0 + dot(R, VN), 1.0);
        horizon *= horizon;
        specularWeight *= horizon;
    }
}