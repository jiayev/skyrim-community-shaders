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
	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	constexpr static size_t s_ShadowMips = 4;

	struct Settings
	{
		uint Enable = true;
		float Strength = 1.0f;
		uint StepLimit = 4;
		float RayLength = 0.2f;
		float CompareToleranceScale = 0.2f;
		float MaxDistance = 2048.0f;
		uint EnableSoftShadows = true;
		float SoftShadowScale = 2.5f;
	} settings;

	struct alignas(16) SSPLSCB
	{
		uint MipLevel;
		uint ResX;
		uint ResY;
		uint pad;
	};

	struct alignas(16) PerFrame
	{
		uint Enable;
		float Strength;
		uint StepLimit;
		float RayLength;
		float CompareToleranceScale;
		float MaxDistance;
		uint EnableSoftShadows;
		float SoftShadowScale;
	};

	winrt::com_ptr<ID3D11SamplerState> linearSampler = nullptr;

	eastl::unique_ptr<Texture2D> depthTexture = nullptr;
	eastl::unique_ptr<Texture2D> linearDepthTexture = nullptr;
	eastl::unique_ptr<Texture2D> blurredLinearDepthTexture = nullptr;

	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_ShadowMips> depthSRVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_ShadowMips> depthUAVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_ShadowMips> linearDepthSRVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_ShadowMips> linearDepthUAVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_ShadowMips> blurredLinearDepthSRVs = { nullptr };
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_ShadowMips> blurredLinearDepthUAVs = { nullptr };

	eastl::unique_ptr<ConstantBuffer> ssplsCB = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> createDepthCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurDepthCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> depthAwareBlurCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> depthAwareUpscaleCS = nullptr;

	virtual void SetupResources() override;

	virtual void DrawSettings() override;

	virtual void ClearShaderCache() override;

	virtual void Prepass() override;

	void CompileComputeShaders();

	void PrepareDepth();
	void DrawShadows();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };

	PerFrame GetCommonBufferData();
};
