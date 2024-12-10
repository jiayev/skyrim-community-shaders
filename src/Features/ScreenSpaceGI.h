#pragma once

#include "Buffer.h"
#include "Feature.h"

struct ScreenSpaceGI : Feature
{
	static ScreenSpaceGI* GetSingleton()
	{
		static ScreenSpaceGI singleton;
		return &singleton;
	}

	bool inline SupportsVR() override { return true; }

	virtual inline std::string GetName() override { return "Screen Space GI"; }
	virtual inline std::string GetShortName() override { return "ScreenSpaceGI"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SSGI"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting ||
		       t == RE::BSShader::Type::Grass ||
		       t == RE::BSShader::Type::DistantTree;
	};

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
	bool ShadersOK();

	void DrawSSGI(Texture2D* srcPrevAmbient);
	void UpdateSB();

	//////////////////////////////////////////////////////////////////////////////////

	bool recompileFlag = false;
	uint outputAoIdx = 0;
	uint outputIlIdx = 0;

	struct Settings
	{
		bool Enabled = true;
		bool EnableGI = true;
		// performance/quality
		uint NumSlices = 2;
		uint NumSteps = 4;
		bool HalfRes = true;
		// visual
		float MinScreenRadius = 0.01f;
		float AORadius = 100.f;
		float GIRadius = 500.f;
		float Thickness = 75.f;
		float2 DepthFadeRange = { 4e4, 5e4 };
		// gi
		float BackfaceStrength = 0.f;
		bool EnableGIBounce = true;
		float GIBounceFade = 1.f;
		float GIDistanceCompensation = 0.f;
		// mix
		float AOPower = 2.f;
		float GIStrength = 1.f;
		// denoise
		bool EnableTemporalDenoiser = true;
		bool EnableBlur = true;
		float DepthDisocclusion = .03f;
		float NormalDisocclusion = .1f;
		uint MaxAccumFrames = 10;
		float BlurRadius = 5.f;
		float DistanceNormalisation = 2.f;
	} settings;

	struct alignas(16) SSGICB
	{
		float4x4 PrevInvViewMat[2];
		float2 NDCToViewMul[2];
		float2 NDCToViewAdd[2];

		float2 TexDim;
		float2 RcpTexDim;  //
		float2 FrameDim;
		float2 RcpFrameDim;  //
		uint FrameIndex;

		uint NumSlices;
		uint NumSteps;

		float MinScreenRadius;  //
		float AORadius;
		float GIRadius;
		float EffectRadius;
		float Thickness;  //
		float2 DepthFadeRange;
		float DepthFadeScaleConst;

		float BackfaceStrength;  //
		float GIBounceFade;
		float GIDistanceCompensation;
		float GICompensationMaxDist;

		float AOPower;  //
		float GIStrength;

		float DepthDisocclusion;
		float NormalDisocclusion;
		uint MaxAccumFrames;  //

		float BlurRadius;
		float DistanceNormalisation;

		float pad[2];
	};
	eastl::unique_ptr<ConstantBuffer> ssgiCB;

	eastl::unique_ptr<Texture2D> texNoise = nullptr;
	eastl::unique_ptr<Texture2D> texWorkingDepth = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavWorkingDepth[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texPrevGeo = nullptr;
	eastl::unique_ptr<Texture2D> texRadiance = nullptr;
	eastl::unique_ptr<Texture2D> texAccumFrames[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texAo[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlY[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlCoCg[2] = { nullptr };

	inline auto GetOutputTextures()
	{
		return (loaded && settings.Enabled) ?
		           std::make_tuple(texAo[outputAoIdx]->srv.get(), texIlY[outputIlIdx]->srv.get(), texIlCoCg[outputIlIdx]->srv.get()) :
		           std::make_tuple(nullptr, nullptr, nullptr);
	}

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> radianceDisoccCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> upsampleCompute = nullptr;
};