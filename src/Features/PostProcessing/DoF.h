#pragma once

#include "Buffer.h"
#include "PostProcessFeature.h"

struct DoF : public PostProcessFeature
{
    virtual inline std::string GetType() const override { return "Depth of Field"; }
	virtual inline std::string GetDesc() const override { return "Depth of Field, based on CinematicDOF by Frans Bouma"; }

    struct Settings
    {
        bool AutoFocus = true;
        uint8_t pad[3];
        float TransitionSpeed = 0.5f;
        float2 FocusCoord = float2(0.5f, 0.5f);
        float ManualFocusPlane = 0.4f;
        float FocalLength = 50.0f;
        float FNumber = 2.8f;
        float BlurQuality = 7.0f;
        float HighlightBoost = 0.9f;
        float Bokeh = 0.5f;
    } settings;

    struct alignas(16) DoFCB
    {
        float TransitionSpeed;
        float2 FocusCoord;
        float ManualFocusPlane;
        float FocalLength;
        float FNumber;
        float BlurQuality;
        float HighlightBoost;
        float Bokeh;
        float Width;
        float Height;
        bool AutoFocus;
        uint8_t pad[3];
    };

    eastl::unique_ptr<ConstantBuffer> dofCB = nullptr;

    eastl::unique_ptr<Texture2D> texOutput = nullptr;
    eastl::unique_ptr<Texture2D> texFocus = nullptr;
    eastl::unique_ptr<Texture2D> texPreFocus = nullptr;
    eastl::unique_ptr<Texture2D> texCoC = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> UpdateFocusCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> CalculateCoCCS = nullptr;

    winrt::com_ptr<ID3D11SamplerState> colorSampler = nullptr;
    winrt::com_ptr<ID3D11SamplerState> depthSampler = nullptr;

    virtual void SetupResources() override;
    virtual void ClearShaderCache() override;
    void CompileComputeShaders();

    virtual void RestoreDefaultSettings() override;
    virtual void LoadSettings(json&) override;
    virtual void SaveSettings(json&) override;

    virtual void DrawSettings() override;

    virtual void Draw(TextureInfo&) override;
};