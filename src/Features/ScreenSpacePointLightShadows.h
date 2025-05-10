#pragma once

struct ScreenSpacePointLightShadows : Feature
{
    static ScreenSpacePointLightShadows* GetSingleton()
    {
        static ScreenSpacePointLightShadows singleton;
        return &singleton;
    }

    virtual inline std::string GetName() override { return "Screen Space Point Light Shadows"; }
    virtual inline std::string GetShortName() override { return "ScreenSpacePointLightShadows"; }
    virtual inline std::string_view GetShaderDefineName() override { return "SSPLS"; }
    bool HasShaderDefine(RE::BSShader::Type shaderType) override;

    constexpr static size_t s_ShadowMips = 4;

    struct Settings
    {
        uint Enable = true;
    } settings;

    struct alignas(16) SSPLSCB
    {
        uint Enable;
        uint MipLevel;
        float2 DynamicRes;
    };
    
    winrt::com_ptr<ID3D11SamplerState> linearSampler = nullptr;
    Texture2D* shadowTexture = nullptr;
    Texture2D* depthTexture = nullptr;
    Texture2D* linearDepthTexture = nullptr;

    std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_ShadowMips> shadowSRVs = { nullptr };
    std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_ShadowMips> shadowUAVs = { nullptr };
    std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_ShadowMips> depthSRVs = { nullptr };
    std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_ShadowMips> depthUAVs = { nullptr };
    std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_ShadowMips> linearDepthSRVs = { nullptr };
    std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_ShadowMips> linearDepthUAVs = { nullptr };

    SSPLSCBConstantBuffer* ssplsCB = nullptr;

    winrt::com_ptr<ID3D11ComputeShader> createDepthCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> raymarchCS = nullptr;
    winrt::com_ptr<ID3D11ComputeShader> depthAwareBlurCS = nullptr;

    virtual void SetupResources() override;

	virtual void DrawSettings() override;

	virtual void ClearShaderCache() override;

    virtual void Prepass() override;

    void CompileComputeShaders();

    void DrawShadows();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
    virtual void RestoreDefaultSettings() override;
};
