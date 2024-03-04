#pragma once

#include "Buffer.h"
#include "Feature.h"

struct SubsurfaceScattering : Feature
{
public:
	static SubsurfaceScattering* GetSingleton()
	{
		static SubsurfaceScattering singleton;
		return &singleton;
	}

	static void InstallHooks()
	{
		Hooks::Install();
	}

	struct Settings
	{
		uint EnableCharacterLighting = false;
		uint UseLinear = false;
		float BlurRadius = 1.0f;
		float DepthFalloff = 0.0f;
		float BacklightingAmount = 0.0f;
		float3 Strength = { 1.0f, 0.66f, 0.0f };
		float3 Falloff = { 1.0f, 0.37f, 0.3f };
	};

	Settings settings;

	struct alignas(16) BlurCB
	{
		float4 Kernel[33];
		float4 CameraData;
		float2 BufferDim;
		float2 RcpBufferDim;
		uint FrameCount;
		float SSSS_FOVY;
		uint UseLinear;
		float BlurRadius;
		float DepthFalloff;
		float Backlighting;
		uint pad[2];
	};

	ConstantBuffer* blurCB = nullptr;

	struct PerPass
	{
		uint SkinMode;
		uint pad[3];
	};

	std::unique_ptr<Buffer> perPass = nullptr;

	Texture2D* blurHorizontalTemp = nullptr;

	ID3D11ComputeShader* horizontalSSBlur = nullptr;
	ID3D11ComputeShader* verticalSSBlur = nullptr;

	ID3D11SamplerState* linearSampler = nullptr;
	ID3D11SamplerState* pointSampler = nullptr;

	uint skinMode = false;
	uint normalsMode = 0;

	float4 kernel[33];

	virtual inline std::string GetName() { return "Subsurface Scattering"; }
	virtual inline std::string GetShortName() { return "SubsurfaceScattering"; }
	inline std::string_view GetShaderDefineName() override { return "SSS"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources();
	virtual void Reset();
	virtual void RestoreDefaultSettings();

	virtual void DrawSettings();

	float3 Gaussian(float variance, float r);
	float3 Profile(float r);
	void CalculateKernel();

	void DrawSSSWrapper(bool a_firstPerson = false);

	void DrawSSS();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void ClearShaderCache();
	ID3D11ComputeShader* GetComputeShaderHorizontalBlur();
	ID3D11ComputeShader* GetComputeShaderVerticalBlur();

	virtual void PostPostLoad() override;

	void OverrideFirstPersonRenderTargets();

	struct Hooks
	{
		struct BSShaderAccumulator_FinishAccumulating_Standard_PreResolveDepth_QPassesWithinRange_WaterStencil
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges)
			{
				GetSingleton()->DrawSSSWrapper();
				func(This, StartRange, EndRanges);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderFirstPersonView_Start
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				GetSingleton()->OverrideFirstPersonRenderTargets();
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderFirstPersonView_End
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				GetSingleton()->DrawSSSWrapper(true);
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_thunk_call<BSShaderAccumulator_FinishAccumulating_Standard_PreResolveDepth_QPassesWithinRange_WaterStencil>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x3C5, 0x3B4));
			stl::write_thunk_call<Main_RenderFirstPersonView_Start>(REL::RelocationID(99943, 106588).address() + REL::Relocate(0x70, 0x66));
			stl::write_thunk_call<Main_RenderFirstPersonView_End>(REL::RelocationID(99943, 106588).address() + REL::Relocate(0x49C, 0x47E));
			logger::info("[SSS] Installed hooks");
		}
	};
};
