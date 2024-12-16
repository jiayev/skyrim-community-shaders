#pragma once

#include "Buffer.h"
#include "PostProcessFeature.h"

struct VanillaImagespace : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Vanilla Imagespace"; }
	virtual inline std::string GetDesc() const override { return "Simple node to apply vanilla imagespace settings."; }
    struct Settings
	{
		float blendFactor = 1.0f;
	} settings;

    struct alignas(16) VanillaImagespaceCB
    {
        float blendFactor;
        float2 res;
        uint8_t pad[4];
    };

    eastl::unique_ptr<ConstantBuffer> vanillaImagespaceCB = nullptr;
    REL::Relocation<ID3D11Buffer**> perGeometryBuffersArray;

    eastl::unique_ptr<Texture2D> texOutput = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> vanillaImagespaceCS = nullptr;

    winrt::com_ptr<ID3D11SamplerState> colorSampler = nullptr;

    virtual void SetupResources() override;
    virtual void ClearShaderCache() override;
    void CompileComputeShaders();

    virtual void RestoreDefaultSettings() override;
    virtual void LoadSettings(json&) override;
    virtual void SaveSettings(json&) override;

    virtual void DrawSettings() override;

    virtual void Draw(TextureInfo&) override;
};
