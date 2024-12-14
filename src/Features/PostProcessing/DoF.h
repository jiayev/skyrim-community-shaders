#pragma once

#include "PostProcessFeature.h"

#include "Buffer.h"

struct CinematicDOF : public PostProcessFeature
{
    virtual inline std::string GetType() const override { return "Cinematic DOF"; }
    virtual inline std::string GetDesc() const override { return "Cinematic Depth of Field shader, using scatter-as-gather. Shader by Frans Bouma"; }

    struct Settings
    {
        bool UseAutoFocus = true;
        bool MitigateUndersampling = false;
        float2 AutoFocusPoint = { 0.5f, 0.5f };
        float AutoFocusTransitionSpeed = 0.2f;
        float ManualFocusPlane = 10.f;
        float FocalLength = 100.f;
        float FNumber = 2.8f;
        float FarPlaneMaxBlur = 1.f;
        float NearPlaneMaxBlur = 1.f;
        float BlurQuality = 7.f;
        float BokehBusyFactor = 0.5f;
        float PostBlurSmoothing = 0.0f;
        float NearFarDistanceCompensation = 1.0f;
        float HighlightAnamorphicFactor = 1.0f;
        float HighlightAnamorphicSpreadFactor = 0.0f;
        float HighlightAnamorphicAlignmentFactor = 0.0f;
        float HighlightBoost = 0.9f;
        float HighlightGammaFactor = 2.2f;
        float HighlightSharpeningFactor = 0.0f;
        int HighlightShape = 0;
        float HighlightShapeRotationAngle = 0.0f;
        float HighlightShapeGamma = 2.2f;
    } settings;

    struct alignas(16) DOFCB
    {
        bool UseAutoFocus;
        bool MitigateUndersampling;
        uint8_t pad[2];
        float2 AutoFocusPoint;
        float AutoFocusTransitionSpeed;

        float ManualFocusPlane;
        float FocalLength;
        float FNumber;
        float FarPlaneMaxBlur;

        float NearPlaneMaxBlur;
        float BlurQuality;
        float BokehBusyFactor;
        float PostBlurSmoothing;

        float NearFarDistanceCompensation;
        float HighlightAnamorphicFactor;
        float HighlightAnamorphicSpreadFactor;
        float HighlightAnamorphicAlignmentFactor;

        float HighlightBoost;
        float HighlightGammaFactor;
        float HighlightSharpeningFactor;
        int HighlightShape;

        float HighlightShapeRotationAngle;
        float HighlightShapeGamma;

        float ScreenWidth;
        float ScreenHeight;
    };
    eastl::unique_ptr<ConstantBuffer> dofCB = nullptr;

    winrt::com_ptr<ID3D11SamplerState> colorSampler = nullptr;
    winrt::com_ptr<ID3D11SamplerState> bufferSampler = nullptr;
    winrt::com_ptr<ID3D11SamplerState> cocSampler = nullptr;
    winrt::com_ptr<ID3D11SamplerState> noiseSampler = nullptr;

    eastl::unique_ptr<Texture2D> texOutput = nullptr;
    eastl::unique_ptr<Texture2D> texDepth = nullptr;

    eastl::unique_ptr<Texture2D> texCDCurrentFocus = nullptr;
    eastl::unique_ptr<Texture2D> texCDPreviousFocus = nullptr;
    eastl::unique_ptr<Texture2D> texCDCoC = nullptr;
    eastl::unique_ptr<Texture2D> texCDCoCTileTmp = nullptr;
    eastl::unique_ptr<Texture2D> texCDCoCTile = nullptr;
    eastl::unique_ptr<Texture2D> texCDCoCTileNeighbor = nullptr;
    eastl::unique_ptr<Texture2D> texCDCoCTmp1 = nullptr;
    eastl::unique_ptr<Texture2D> texCDCoCBlurred = nullptr;
    eastl::unique_ptr<Texture2D> texCDBuffer1 = nullptr;
    eastl::unique_ptr<Texture2D> texCDBuffer2 = nullptr;
    eastl::unique_ptr<Texture2D> texCDBuffer3 = nullptr;
    eastl::unique_ptr<Texture2D> texCDBuffer4 = nullptr;
    eastl::unique_ptr<Texture2D> texCDBuffer5 = nullptr;
    eastl::unique_ptr<Texture2D> texCDNoise = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> determineCurrentFocusCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> copyCurrentFocusCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> calculateCoCValuesCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> preBlurCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> bokehBlurCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> nearBokehBlurCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> cocTile1CS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> cocTile2CS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> cocTileNeighborCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> cocGaussian1CS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> cocGaussian2CS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> combinerCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> tentFilterCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> postSmoothing1CS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> postSmoothing2AndFocusingCS = nullptr;

    virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;
};