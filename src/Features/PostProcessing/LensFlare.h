#pragma once

#include "Buffer.h"
#include "PostProcessFeature.h"

struct LensFlare : public PostProcessFeature
{
    virtual inline std::string GetType() const override { return "Lens Flare"; }
	virtual inline std::string GetDesc() const override { return "Simulates Lens Flare reacting to bright part."; }

    struct Settings
	{
        float GhostStrength = 0.3f;
        float HaloStrength = 0.2f;
        float HaloRadius = 0.5f;
        float HaloWidth = 0.5f;
        float LensFlareCA = 1.0f;
        float LFStrength = 0.25f;
        bool GLocalMask = true;
        uint8_t pad[3];
	} settings;

    struct alignas(16) LensFlareCB
    {
        Settings settings;
        float ScreenWidth;
        float ScreenHeight;
        uint8_t pad1[12];
    };

    eastl::unique_ptr<ConstantBuffer> lensFlareCB = nullptr;

    eastl::unique_ptr<Texture2D> texOutput = nullptr;
    winrt::com_ptr<ID3D11SamplerState> colorSampler = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> lensFlareCS = nullptr;

    virtual void SetupResources() override;
    virtual void ClearShaderCache() override;
    void CompileComputeShaders();

    virtual void RestoreDefaultSettings() override;
    virtual void LoadSettings(json&) override;
    virtual void SaveSettings(json&) override;

    virtual void DrawSettings() override;

    virtual void Draw(TextureInfo&) override;
};
