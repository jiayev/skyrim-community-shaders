// Psuedo Sun Bounce for Skyrim Community Shaders
// Author: Jiaye
// Date: 2025-12-14
// Ref: https://advances.realtimerendering.com/s2021/jpatry_advances2021/index.html

#ifndef SUNBOUNCE_HLSLI
#define SUNBOUNCE_HLSLI

#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

// SH helpers
namespace SunBounce
{
    struct SH2_RGB
    {
        sh2 R;
        sh2 G;
        sh2 B;
    };

    SH2_RGB ZeroSH2_RGB()
    {
        SH2_RGB res;
        res.R = SphericalHarmonics::Zero();
        res.G = SphericalHarmonics::Zero();
        res.B = SphericalHarmonics::Zero();
        return res;
    }

    SH2_RGB AddSH2_RGB(SH2_RGB a, SH2_RGB b)
    {
        SH2_RGB res;
        res.R = SphericalHarmonics::Add(a.R, b.R);
        res.G = SphericalHarmonics::Add(a.G, b.G);
        res.B = SphericalHarmonics::Add(a.B, b.B);
        return res;
    }

    SH2_RGB ScaleSH2_RGB(SH2_RGB sh, float3 color)
    {
        SH2_RGB res;
        res.R = SphericalHarmonics::Scale(sh.R, color.r);
        res.G = SphericalHarmonics::Scale(sh.G, color.g);
        res.B = SphericalHarmonics::Scale(sh.B, color.b);
        return res;
    }

    SH2_RGB CalcSunBounceSH(float3 DirLightDirection, float3 DirLightColor, float3 groundAlbedo, float3 wallAlbedo)
    {
        float3 L = normalize(DirLightDirection);
        
        float3 N_ground = float3(0, 0, 1);

        float3 L_horiz = float3(L.x, L.y, 0);
        float L_horiz_len = length(L_horiz);
        float3 N_wall = (L_horiz_len > 0.001f) ? (L_horiz / L_horiz_len) : float3(1, 0, 0);

        sh2 sh_g = SphericalHarmonics::EvaluateCosineLobe(N_ground);
        sh2 sh_w = SphericalHarmonics::EvaluateCosineLobe(N_wall);

        float NdotL_g = saturate(dot(N_ground, L));
        float NdotL_w = saturate(L_horiz_len); 
        float3 bounceColor_g = DirLightColor * groundAlbedo * NdotL_g / Math::PI;
        float3 bounceColor_w = DirLightColor * wallAlbedo * NdotL_w / Math::PI;

        SH2_RGB result = ZeroSH2_RGB();
        
        result.R = SphericalHarmonics::Add(result.R, SphericalHarmonics::Scale(sh_g, bounceColor_g.r));
        result.G = SphericalHarmonics::Add(result.G, SphericalHarmonics::Scale(sh_g, bounceColor_g.g));
        result.B = SphericalHarmonics::Add(result.B, SphericalHarmonics::Scale(sh_g, bounceColor_g.b));
        
        result.R = SphericalHarmonics::Add(result.R, SphericalHarmonics::Scale(sh_w, bounceColor_w.r));
        result.G = SphericalHarmonics::Add(result.G, SphericalHarmonics::Scale(sh_w, bounceColor_w.g));
        result.B = SphericalHarmonics::Add(result.B, SphericalHarmonics::Scale(sh_w, bounceColor_w.b));
        return result;
    }

    float3 CalcFauxSpecularBounce(float3 N, float3 V, float roughness, SH2_RGB sunSH)
    {
        sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(N, V, roughness);
        float3 specularColor;
        specularColor.r = SphericalHarmonics::FuncProductIntegral(-sunSH.R, specularLobe);
        specularColor.g = SphericalHarmonics::FuncProductIntegral(-sunSH.G, specularLobe);
        specularColor.b = SphericalHarmonics::FuncProductIntegral(-sunSH.B, specularLobe);
        return specularColor;
    }
}
#endif