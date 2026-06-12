#pragma once

#include "Buffer.h"
#include "NRD.h"
#include "NRDReblurIntegration.h"

#include <NRDSettings.h>

/**
 * ScreenSpaceGI: indirect lighting and ambient occlusion via XeGTAO-style
 * horizon scanning, optionally directional via spherical harmonics, denoised
 * by NVIDIA REBLUR.
 *
 * Diffuse-only. Specular reflections live in the standalone
 * ScreenSpaceReflections feature; denoising guides come from the core NRD
 * service.
 */
struct ScreenSpaceGI : Feature
{
private:
	static constexpr std::string_view MOD_ID = "130375";

public:
	virtual inline std::string GetName() override { return "Screen Space GI"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_gi.name", "Screen Space GI"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceGI"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		std::string desc = T("feature.screen_space_gi.description", "Screen Space Global Illumination adds realistic indirect lighting and ambient occlusion to the game. This technique simulates how light bounces off surfaces to illuminate other objects naturally.");
		return std::make_pair(
			desc,
			std::vector<std::string>{
				T("feature.screen_space_gi.key_feature_1", "Realistic indirect lighting"),
				T("feature.screen_space_gi.key_feature_2", "Enhanced ambient occlusion"),
				T("feature.screen_space_gi.key_feature_3", "Improved visual depth and atmosphere"),
				T("feature.screen_space_gi.key_feature_4", "NVIDIA REBLUR temporal denoising"),
				T("feature.screen_space_gi.key_feature_5", "Configurable quality and performance settings") });
	}

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
	bool ShadersOK();

	void DrawSSGI();
	void UpdateSB();
	void SetupNRDResources();

	//////////////////////////////////////////////////////////////////////////////////

	bool recompileFlag = false;

	struct Settings
	{
		bool Enabled = true;
		bool EnableGI = true;
		bool EnableVanillaSSAO = false;
		bool EnableSH = true;
		// performance/quality
		uint NumSteps = 8u;
		bool HalfRes = true;
		// visual
		float Thickness = 0.1f;
		// mix
		float AOPower = 1.0f;
		float GIStrength = 1.0f;
		bool UseDynamicCubemapsAsFallback = true;
		float DiffuseCubemapMult = 1.0f;
		// NRD REBLUR
		bool EnableREBLUR = true;
		NRD::REBLURSettings Reblur;
	} settings;

	struct alignas(16) SSGICB
	{
		float4x4 PrevInvViewMat;
		float2 NDCToViewMul;
		float2 NDCToViewAdd;

		float2 TexDim;
		float2 RcpTexDim;
		float2 FrameDim;
		float2 RcpFrameDim;
		uint FrameIndex;

		uint NumSteps;

		float Thickness;
		float AOPower;

		float GIStrength;
		float DiffuseCubemapMult;
		uint UseDynamicCubemap;
		uint pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(SSGICB);
	eastl::unique_ptr<ConstantBuffer> ssgiCB;

	struct alignas(16) SharedData
	{
		float DiffuseMult;
		uint DebugMode;
		uint pad0;
		uint pad1;
	};

	SharedData GetCommonBufferData();

	eastl::unique_ptr<Texture2D> texNoise = nullptr;
	eastl::unique_ptr<Texture2D> texWorkingDepth = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavWorkingDepth[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texPrevGeo = nullptr;
	eastl::unique_ptr<Texture2D> texRadiance = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavRadiance[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texNormal = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavNormal[5] = { nullptr };

	// REBLUR diffuse radiance in/out (RGBA16F, full-res)
	eastl::unique_ptr<Texture2D> texNRDInput = nullptr;
	eastl::unique_ptr<Texture2D> texNRDOutput = nullptr;
	eastl::unique_ptr<Texture2D> texNRDInputSH1 = nullptr;
	eastl::unique_ptr<Texture2D> texNRDOutputSH1 = nullptr;

	ID3D11ShaderResourceView* GetDiffuseOutputTexture();
	ID3D11ShaderResourceView* GetDiffuseSH1Texture();

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterRadianceCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterNormalCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giCompute = nullptr;

	NRDReblurIntegration nrdReblur;
	nrd::ReblurSettings reblurSettings{};
};
