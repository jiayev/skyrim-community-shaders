#pragma once

#include "Buffer.h"
#include "PostProcessFeature.h"

struct LensFlare : public PostProcessFeature
{
    virtual inline std::string GetType() const override { return "Lens Flare"; }
	virtual inline std::string GetDesc() const override { return "Lens Flare by Jiaye, partially based on Gimle Larpes's effect shader."; }

    struct Settings
	{
        float LensFlareCurve = 0.5f;
        float GhostStrength = 0.3f;
        float HaloStrength = 0.2f;
        float HaloRadius = 0.5f;
        float HaloWidth = 0.5f;
        float LensFlareCA = 1.0f;
        float LFStrength = 0.5f;
        bool GLocalMask = true;
        bool SunGlareBoost = true;
        bool Starburst = true;
        uint8_t pad;
	} settings;

    struct alignas(16) LensFlareCB
    {
        float LensFlareCurve;
        float GhostStrength;
        float HaloStrength;
        float HaloRadius;
        float HaloWidth;
        float LensFlareCA;
        float LFStrength;
        float ScreenWidth;
        float ScreenHeight;
        float3 SunPos;
        float SunVisibility;
        int DownsizeScale;
        uint GLocalMask;
        uint SunGlareBoost;
        uint Starburst;
        uint8_t pad[12];
    };

    struct debugSettings
    {
        int downsampleTimes = 2;
        int upsampleTimes = 6;
        bool disableDownsample = false;
        bool disableUpsample = false;
        uint8_t pad[2];
    } debugsettings;

    eastl::unique_ptr<ConstantBuffer> lensFlareCB = nullptr;

    eastl::unique_ptr<Texture2D> texOutput = nullptr;
    eastl::unique_ptr<Texture2D> texFlare = nullptr;
    eastl::unique_ptr<Texture2D> texFlareD = nullptr;
    eastl::unique_ptr<Texture2D> texFlareDCopy = nullptr;
    eastl::unique_ptr<Texture2D> texFlareU = nullptr;
    eastl::unique_ptr<Texture2D> texFlareUCopy = nullptr;
    eastl::unique_ptr<Texture2D> texBurstNoise = nullptr;
    eastl::unique_ptr<Texture2D> texRGBNoise = nullptr;

    winrt::com_ptr<ID3D11SamplerState> colorSampler = nullptr;
    winrt::com_ptr<ID3D11SamplerState> resizeSampler = nullptr;
    winrt::com_ptr<ID3D11SamplerState> noiseSampler = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> lensFlareCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> downsampleCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> upsampleCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> compositeCS = nullptr;

    float3 debugSunPos;

    virtual void SetupResources() override;
    virtual void ClearShaderCache() override;
    void CompileComputeShaders();

    virtual void RestoreDefaultSettings() override;
    virtual void LoadSettings(json&) override;
    virtual void SaveSettings(json&) override;

    virtual void DrawSettings() override;

    virtual void Draw(TextureInfo&) override;

    RE::NiPoint3 GetCameraPos();
    RE::NiQuaternion GetCameraRot();
};
