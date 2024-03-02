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

	struct alignas(16) BlurCB
	{
		float4 DynamicRes;
		float4 CameraData;
		float2 BufferDim;
		float2 RcpBufferDim;
		uint FrameCount;
		float SSSS_FOVY;
		uint pad0[2];
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

	virtual inline std::string GetName() { return "Subsurface Scattering"; }
	virtual inline std::string GetShortName() { return "SubsurfaceScattering"; }
	inline std::string_view GetShaderDefineName() override { return "SSS"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources();
	virtual void Reset();
	virtual void RestoreDefaultSettings();

	virtual void DrawSettings();

	void DrawSSSWrapper(bool a_firstPerson = false);

	void DrawSSS();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void ClearShaderCache();
	ID3D11ComputeShader* GetComputeShaderHorizontalBlur();
	ID3D11ComputeShader* GetComputeShaderVerticalBlur();

	void BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* Pass);

	virtual void PostPostLoad() override;

	void OverrideFirstPerson();

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				GetSingleton()->BSLightingShader_SetupGeometry_Before(Pass);
				func(This, Pass, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

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
				GetSingleton()->OverrideFirstPerson();
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderFirstPersonView_End
		{
			static int64_t thunk(RE::BSShaderAccumulator* a1, char a2)
			{
				GetSingleton()->DrawSSSWrapper(true);
				return func(a1, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			//stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			stl::write_thunk_call<BSShaderAccumulator_FinishAccumulating_Standard_PreResolveDepth_QPassesWithinRange_WaterStencil>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x3C5, 0x3B4));
			stl::write_thunk_call<Main_RenderFirstPersonView_Start>(REL::RelocationID(99943, 99943).address() + REL::Relocate(0x70, 0x492));
			stl::write_thunk_call<Main_RenderFirstPersonView_End>(REL::RelocationID(99943, 99943).address() + REL::Relocate(0x49C, 0x492));
			logger::info("[SSS] Installed hooks");
		}
	};
};
