#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "State.h"
#include "Util.h"

struct Skylighting : Feature
{
public:
	static Skylighting* GetSingleton()
	{
		static Skylighting singleton;
		return &singleton;
	}

	virtual bool SupportsVR() override { return true; };

	virtual inline std::string GetName() override { return "Skylighting"; }
	virtual inline std::string GetShortName() override { return "Skylighting"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SKYLIGHTING"; }
	virtual bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void PostPostLoad() override;

	virtual void Prepass() override;

	virtual void RestoreDefaultSettings() override;

	ID3D11ComputeShader* GetSkylightingCS();
	ID3D11ComputeShader* GetSkylightingShadowMapCS();
	ID3D11ComputeShader* GetSkylightingBlurHorizontalCS();
	ID3D11ComputeShader* GetSkylightingBlurVerticalCS();
	virtual void ClearShaderCache() override;

	void Bind();

	void Compute();
	void ComputeBlur(bool a_horizontal);

	ID3D11PixelShader* GetFoliagePS();

	void SkylightingShaderHacks();  // referenced in State.cpp

	//////////////////////////////////////////////////////////////////////////////////

	struct alignas(16) PerFrameCB
	{
		REX::W32::XMFLOAT4X4 OcclusionViewProj;
		float4 EyePosition;
		float4 ShadowDirection;
		float4 BufferDim;
		float4 CameraData;
		uint FrameCount;
		uint pad0[3];
	};

	ConstantBuffer* perFrameCB = nullptr;

	struct Settings
	{
		bool EnableSkylighting = true;
		bool HeightSkylighting = true;
		float MinimumBound = 128;
		bool RenderTrees = false;
	};

	Settings settings;

	Texture2D* skylightingTexture = nullptr;
	Texture2D* skylightingTempTexture = nullptr;
	ID3D11ShaderResourceView* noiseView = nullptr;
	Texture2D* occlusionTexture = nullptr;

	ID3D11ComputeShader* skylightingCS = nullptr;
	ID3D11ComputeShader* skylightingShadowMapCS = nullptr;
	ID3D11ComputeShader* skylightingBlurHorizontalCS = nullptr;
	ID3D11ComputeShader* skylightingBlurVerticalCS = nullptr;

	ID3D11SamplerState* comparisonSampler;

	ID3D11PixelShader* foliagePixelShader = nullptr;

	// cached variables
	bool inOcclusion = false;
	bool foliage = false;
	REX::W32::XMFLOAT4X4 viewProjMat;
	RE::NiPoint3 eyePosition;
	std::chrono::time_point<std::chrono::system_clock> lastUpdateTimer = std::chrono::system_clock::now();

	//////////////////////////////////////////////////////////////////////////////////

	struct BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl
	{
		static void* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t renderMode, RE::BSGraphics::BSShaderAccumulator* accumulator);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_Precipitation_RenderOcclusion
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSUtilityShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
