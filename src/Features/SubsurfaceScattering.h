#pragma once

#include "Buffer.h"
#include "Feature.h"

#define SSSS_N_SAMPLES 21

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

	struct DiffusionProfile
	{
		float BlurRadius;
		float Thickness;
		float3 Strength;
		float3 Falloff;
	};

	struct Settings
	{
		uint EnableCharacterLighting = false;
		DiffusionProfile HumanProfile{ 1.0f, 1.0f, { 0.48f, 0.41f, 0.28f }, { 1.0f, 0.37f, 0.3f } };
		DiffusionProfile BeastProfile{ 1.0f, 1.0f, { 0.48f, 0.41f, 0.28f }, { 0.56f, 0.56f, 0.56f } };
	};

	Settings settings;

	struct alignas(16) Kernel
	{
		float4 Sample[SSSS_N_SAMPLES];
	};

	struct alignas(16) BlurCB
	{
		Kernel HumanKernel;
		Kernel BeastKernel;
		float4 HumanProfile;
		float4 BeastProfile;
		float4 CameraData;
		float2 BufferDim;
		float2 RcpBufferDim;
		uint FrameCount;
		float SSSS_FOVY;
		uint pad[2];
	};

	ConstantBuffer* blurCB = nullptr;
	BlurCB blurCBData{};

	struct alignas(16) PerPass
	{
		uint ValidMaterial;
		uint IsBeastRace;
		uint pad0[2];
	};

	std::unique_ptr<Buffer> perPass = nullptr;

	bool validMaterial = true;

	Texture2D* blurHorizontalTemp = nullptr;

	ID3D11ComputeShader* horizontalSSBlur = nullptr;
	ID3D11ComputeShader* verticalSSBlur = nullptr;
	ID3D11ComputeShader* clearBuffer = nullptr;

	RE::RENDER_TARGET normalsMode = RE::RENDER_TARGET::kNONE;

	virtual inline std::string GetName() { return "Subsurface Scattering"; }
	virtual inline std::string GetShortName() { return "SubsurfaceScattering"; }
	inline std::string_view GetShaderDefineName() override { return "SSS"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources();
	virtual void Reset();
	virtual void RestoreDefaultSettings();

	virtual void DrawSettings();

	float3 Gaussian(DiffusionProfile& a_profile, float variance, float r);
	float3 Profile(DiffusionProfile& a_profile, float r);
	void CalculateKernel(DiffusionProfile& a_profile, Kernel& kernel);

	void DrawSSSWrapper(bool a_firstPerson = false);

	void DrawSSS();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void ClearShaderCache();
	ID3D11ComputeShader* GetComputeShaderHorizontalBlur();
	ID3D11ComputeShader* GetComputeShaderVerticalBlur();
	ID3D11ComputeShader* GetComputeShaderClearBuffer();

	virtual void PostPostLoad() override;

	void OverrideFirstPersonRenderTargets();

	void BSLightingShader_SetupSkin(RE::BSRenderPass* Pass);

	void Bind();

	struct Hooks
	{
		struct Main_RenderWorld_Start
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				GetSingleton()->Bind();
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderBatches
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld_End
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderSky
				GetSingleton()->DrawSSSWrapper();
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

		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				GetSingleton()->BSLightingShader_SetupSkin(Pass);
				func(This, Pass, RenderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_thunk_call<Main_RenderWorld_Start>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x8E, 0x84));
			stl::write_thunk_call<Main_RenderWorld_End>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x247, 0x237, 0x24F));
			if (!REL::Module::IsVR()) {
				stl::write_thunk_call<Main_RenderFirstPersonView_Start>(REL::RelocationID(99943, 106588).address() + REL::Relocate(0x70, 0x66));
				stl::write_thunk_call<Main_RenderFirstPersonView_End>(REL::RelocationID(99943, 106588).address() + REL::Relocate(0x49C, 0x47E, 0x4fc));
			}
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[SSS] Installed hooks");
		}
	};
};
