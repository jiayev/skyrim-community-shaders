#include "Skylighting.h"
#include <DDSTextureLoader.h>
#include <Deferred.h>
#include <Features/TerrainBlending.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skylighting::Settings,
	EnableSkylighting,
	HeightSkylighting,
	MinimumBound,
	RenderTrees)

void Skylighting::DrawSettings()
{
	ImGui::Checkbox("Enable Skylighting", &settings.EnableSkylighting);
	ImGui::Checkbox("Enable Height Map Rendering", &settings.HeightSkylighting);
	ImGui::SliderFloat("Minimum Bound", &settings.MinimumBound, 1, 256, "%.0f");
	ImGui::Checkbox("Render Trees", &settings.RenderTrees);
}

void Skylighting::Draw(const RE::BSShader*, const uint32_t)
{
}

void Skylighting::SetupResources()
{
	GetSkylightingCS();
	GetSkylightingShadowMapCS();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.UAV->GetDesc(&uavDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		skylightingTexture = new Texture2D(texDesc);
		skylightingTexture->CreateSRV(srvDesc);
		skylightingTexture->CreateUAV(uavDesc);

		skylightingTempTexture = new Texture2D(texDesc);
		skylightingTempTexture->CreateSRV(srvDesc);
		skylightingTempTexture->CreateUAV(uavDesc);
	}

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(&texDesc);
		precipitationOcclusion.depthSRV->GetDesc(&srvDesc);
		precipitationOcclusion.views[0]->GetDesc(&dsvDesc);

		occlusionTexture = new Texture2D(texDesc);
		occlusionTexture->CreateSRV(srvDesc);
		occlusionTexture->CreateDSV(dsvDesc);
	}

	{
		perFrameCB = new ConstantBuffer(ConstantBufferDesc<PerFrameCB>());
	}

	{
		auto& device = State::GetSingleton()->device;
		auto& context = State::GetSingleton()->context;

		DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\Skylighting\\bluenoise.dds", nullptr, &noiseView);
	}

	{
		auto& device = State::GetSingleton()->device;

		D3D11_SAMPLER_DESC sampDesc;

		sampDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MipLODBias = 0.0f;
		sampDesc.MaxAnisotropy = 1;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		sampDesc.BorderColor[0] = sampDesc.BorderColor[1] = sampDesc.BorderColor[2] = sampDesc.BorderColor[3] = 0;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, &comparisonSampler));
	}
}

void Skylighting::Reset()
{
}

void Skylighting::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void Skylighting::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void Skylighting::PostPostLoad()
{
	logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
	stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
	stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));
	stl::write_vfunc<0x6, BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
}

void Skylighting::RestoreDefaultSettings()
{
	settings = {};
}

ID3D11ComputeShader* Skylighting::GetSkylightingCS()
{
	if (!skylightingCS) {
		logger::debug("Compiling SkylightingCS");
		skylightingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingCS.hlsl", {}, "cs_5_0");
	}
	return skylightingCS;
}

ID3D11ComputeShader* Skylighting::GetSkylightingShadowMapCS()
{
	if (!skylightingShadowMapCS) {
		logger::debug("Compiling SkylightingCS SHADOWMAP");
		skylightingShadowMapCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingCS.hlsl", { { "SHADOWMAP", nullptr } }, "cs_5_0");
	}
	return skylightingShadowMapCS;
}

ID3D11ComputeShader* Skylighting::GetSkylightingBlurHorizontalCS()
{
	if (!skylightingBlurHorizontalCS) {
		logger::debug("Compiling SkylightingBlurCS HORIZONTAL");
		skylightingBlurHorizontalCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingBlurCS.hlsl", { { "HORIZONTAL", nullptr } }, "cs_5_0");
	}
	return skylightingBlurHorizontalCS;
}

ID3D11ComputeShader* Skylighting::GetSkylightingBlurVerticalCS()
{
	if (!skylightingBlurVerticalCS) {
		logger::debug("Compiling SkylightingVerticalCS");
		skylightingBlurVerticalCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingBlurCS.hlsl", {}, "cs_5_0");
	}
	return skylightingBlurVerticalCS;
}

ID3D11PixelShader* Skylighting::GetFoliagePS()
{
	if (!foliagePixelShader) {
		logger::debug("Compiling Utility.hlsl");
		foliagePixelShader = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" }, { "FOLIAGE", "" } }, "ps_5_0");
	}
	return foliagePixelShader;
}

void Skylighting::SkylightingShaderHacks()
{
	if (inOcclusion) {
		auto& context = State::GetSingleton()->context;

		if (foliage) {
			context->PSSetShader(GetFoliagePS(), NULL, NULL);
		} else {
			context->PSSetShader(nullptr, NULL, NULL);
		}
	}
}

void Skylighting::ClearShaderCache()
{
	if (skylightingCS) {
		skylightingCS->Release();
		skylightingCS = nullptr;
	}
	if (skylightingShadowMapCS) {
		skylightingShadowMapCS->Release();
		skylightingShadowMapCS = nullptr;
	}
	if (skylightingBlurHorizontalCS) {
		skylightingBlurHorizontalCS->Release();
		skylightingBlurHorizontalCS = nullptr;
	}
	if (skylightingBlurVerticalCS) {
		skylightingBlurVerticalCS->Release();
		skylightingBlurVerticalCS = nullptr;
	}
	if (foliagePixelShader) {
		foliagePixelShader->Release();
		foliagePixelShader = nullptr;
	}
}

void Skylighting::Compute()
{
	auto state = State::GetSingleton();
	auto& context = state->context;

	if (!settings.EnableSkylighting) {
		float clear[4] = { 1, 1, 1, 1 };
		context->ClearUnorderedAccessViewFloat(GetSingleton()->skylightingTexture->uav.get(), clear);
		return;
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto dispatchCount = Util::GetScreenDispatchCount();

	bool temporal = Util::GetTemporal();

	{
		PerFrameCB data{};
		data.OcclusionViewProj = viewProjMat;
		data.EyePosition = { eyePosition.x, eyePosition.y, eyePosition.z, 0 };

		auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto shadowDirLight = (RE::BSShadowDirectionalLight*)shadowSceneNode->GetRuntimeData().shadowDirLight;
		bool dirShadow = shadowDirLight && shadowDirLight->shadowLightIndex == 0;

		if (dirShadow) {
			data.ShadowDirection = float4(shadowDirLight->lightDirection.x, shadowDirLight->lightDirection.y, shadowDirLight->lightDirection.z, 0);
		}

		data.BufferDim.x = state->screenSize.x;
		data.BufferDim.y = state->screenSize.y;
		data.BufferDim.z = 1.0f / data.BufferDim.x;
		data.BufferDim.w = 1.0f / data.BufferDim.y;

		data.CameraData = Util::GetCameraData();

		data.FrameCount = temporal ? RE::BSGraphics::State::GetSingleton()->frameCount : 0;

		perFrameCB->Update(data);
	}

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto terrainBlending = TerrainBlending::GetSingleton();

	ID3D11ShaderResourceView* srvs[5]{
		terrainBlending->loaded ? terrainBlending->blendedDepthTexture16->srv.get() : depth.depthSRV,
		Deferred::GetSingleton()->shadowView,
		Deferred::GetSingleton()->perShadow->srv.get(),
		noiseView,
		occlusionTexture->srv.get()
	};

	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	ID3D11UnorderedAccessView* uavs[1]{ skylightingTexture->uav.get() };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	auto buffer = perFrameCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	ID3D11SamplerState* samplers[2] = { Deferred::GetSingleton()->linearSampler, comparisonSampler };
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	{
		REL::Relocation<ID3D11Buffer**> perFrame{ REL::RelocationID(524768, 411384) };
		ID3D11Buffer* buffers[1] = { *perFrame.get() };

		context->CSSetConstantBuffers(12, ARRAYSIZE(buffers), buffers);
	}

	context->CSSetShader(settings.HeightSkylighting ? GetSkylightingCS() : GetSkylightingShadowMapCS(), nullptr, 0);

	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	srvs[2] = nullptr;
	srvs[3] = nullptr;
	srvs[4] = nullptr;
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	samplers[0] = nullptr;
	samplers[1] = nullptr;
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Skylighting::ComputeBlur(bool a_horizontal)
{
	auto state = State::GetSingleton();
	auto& context = state->context;

	if (!settings.EnableSkylighting) {
		return;
	}

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto dispatchCount = Util::GetScreenDispatchCount();

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	auto terrainBlending = TerrainBlending::GetSingleton();

	ID3D11ShaderResourceView* srvs[2]{
		terrainBlending->loaded ? terrainBlending->blendedDepthTexture16->srv.get() : depth.depthSRV,
		(a_horizontal ? skylightingTexture : skylightingTempTexture)->srv.get()
	};

	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	ID3D11UnorderedAccessView* uavs[1]{
		(!a_horizontal ? skylightingTexture : skylightingTempTexture)->uav.get()
	};

	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	auto buffer = perFrameCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	ID3D11SamplerState* samplers[1] = { Deferred::GetSingleton()->linearSampler };
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	{
		REL::Relocation<ID3D11Buffer**> perFrame{ REL::RelocationID(524768, 411384) };
		ID3D11Buffer* buffers[1] = { *perFrame.get() };

		context->CSSetConstantBuffers(12, ARRAYSIZE(buffers), buffers);
	}

	context->CSSetShader(a_horizontal ? GetSkylightingBlurHorizontalCS() : GetSkylightingBlurVerticalCS(), nullptr, 0);

	context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	samplers[0] = nullptr;
	context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Skylighting::Prepass()
{
	Compute();
	ComputeBlur(true);
	ComputeBlur(false);
	Bind();
}

void Skylighting::Bind()
{
	auto state = State::GetSingleton();
	auto& context = state->context;

	auto buffer = perFrameCB->CB();
	context->PSSetConstantBuffers(8, 1, &buffer);

	ID3D11ShaderResourceView* srvs[2]{
		occlusionTexture->srv.get(),
		skylightingTexture->srv.get(),
	};

	context->PSSetShaderResources(29, ARRAYSIZE(srvs), srvs);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// RE Code

struct BSParticleShaderRainEmitter
{
	void* vftable_BSParticleShaderRainEmitter_0;
	char _pad_8[4056];
};

static void Precipitation_SetupMask(RE::Precipitation* a_This)
{
	using func_t = decltype(&Precipitation_SetupMask);
	REL::Relocation<func_t> func{ REL::RelocationID(25641, 26183) };
	func(a_This);
}

static void Precipitation_RenderMask(RE::Precipitation* a_This, BSParticleShaderRainEmitter* a_emitter)
{
	using func_t = decltype(&Precipitation_RenderMask);
	REL::Relocation<func_t> func{ REL::RelocationID(25642, 26184) };
	func(a_This, a_emitter);
}

enum class ShaderTechnique
{
	// Sky
	SkySunOcclude = 0x2,

	// Grass
	GrassNoAlphaDirOnlyFlatLit = 0x3,
	GrassNoAlphaDirOnlyFlatLitSlope = 0x5,
	GrassNoAlphaDirOnlyVertLitSlope = 0x6,
	GrassNoAlphaDirOnlyFlatLitBillboard = 0x13,
	GrassNoAlphaDirOnlyFlatLitSlopeBillboard = 0x14,

	// Utility
	UtilityGeneralStart = 0x2B,

	// Effect
	EffectGeneralStart = 0x4000002C,

	// Lighting
	LightingGeneralStart = 0x4800002D,

	// DistantTree
	DistantTreeDistantTreeBlock = 0x5C00002E,
	DistantTreeDepth = 0x5C00002F,

	// Grass
	GrassDirOnlyFlatLit = 0x5C000030,
	GrassDirOnlyFlatLitSlope = 0x5C000032,
	GrassDirOnlyVertLitSlope = 0x5C000033,
	GrassDirOnlyFlatLitBillboard = 0x5C000040,
	GrassDirOnlyFlatLitSlopeBillboard = 0x5C000041,
	GrassRenderDepth = 0x5C00005C,

	// Sky
	SkySky = 0x5C00005E,
	SkyMoonAndStarsMask = 0x5C00005F,
	SkyStars = 0x5C000060,
	SkyTexture = 0x5C000061,
	SkyClouds = 0x5C000062,
	SkyCloudsLerp = 0x5C000063,
	SkyCloudsFade = 0x5C000064,

	// Particle
	ParticleParticles = 0x5C000065,
	ParticleParticlesGryColorAlpha = 0x5C000066,
	ParticleParticlesGryColor = 0x5C000067,
	ParticleParticlesGryAlpha = 0x5C000068,
	ParticleEnvCubeSnow = 0x5C000069,
	ParticleEnvCubeRain = 0x5C00006A,

	// Water
	WaterSimple = 0x5C00006B,
	WaterSimpleVc = 0x5C00006C,
	WaterStencil = 0x5C00006D,
	WaterStencilVc = 0x5C00006E,
	WaterDisplacementStencil = 0x5C00006F,
	WaterDisplacementStencilVc = 0x5C000070,
	WaterGeneralStart = 0x5C000071,

	// Sky
	SkySunGlare = 0x5C006072,

	// BloodSplater
	BloodSplaterFlare = 0x5C006073,
	BloodSplaterSplatter = 0x5C006074,
};

class BSUtilityShader : public RE::BSShader
{
public:
	enum class Flags
	{
		None = 0,
		Vc = 1 << 0,
		Texture = 1 << 1,
		Skinned = 1 << 2,
		Normals = 1 << 3,
		BinormalTangent = 1 << 4,
		AlphaTest = 1 << 7,
		LodLandscape = 1 << 8,
		RenderNormal = 1 << 9,
		RenderNormalFalloff = 1 << 10,
		RenderNormalClamp = 1 << 11,
		RenderNormalClear = 1 << 12,
		RenderDepth = 1 << 13,
		RenderShadowmap = 1 << 14,
		RenderShadowmapClamped = 1 << 15,
		GrayscaleToAlpha = 1 << 15,
		RenderShadowmapPb = 1 << 16,
		AdditionalAlphaMask = 1 << 16,
		DepthWriteDecals = 1 << 17,
		DebugShadowSplit = 1 << 18,
		DebugColor = 1 << 19,
		GrayscaleMask = 1 << 20,
		RenderShadowmask = 1 << 21,
		RenderShadowmaskSpot = 1 << 22,
		RenderShadowmaskPb = 1 << 23,
		RenderShadowmaskDpb = 1 << 24,
		RenderBaseTexture = 1 << 25,
		TreeAnim = 1 << 26,
		LodObject = 1 << 27,
		LocalMapFogOfWar = 1 << 28,
		OpaqueEffect = 1 << 29,
	};

	static BSUtilityShader* GetSingleton()
	{
		REL::Relocation<BSUtilityShader**> singleton{ RELOCATION_ID(528354, 415300) };
		return *singleton;
	}
	~BSUtilityShader() override;  // 00

	// override (BSShader)
	bool SetupTechnique(std::uint32_t globalTechnique) override;            // 02
	void RestoreTechnique(std::uint32_t globalTechnique) override;          // 03
	void SetupGeometry(RE::BSRenderPass* pass, uint32_t flags) override;    // 06
	void RestoreGeometry(RE::BSRenderPass* pass, uint32_t flags) override;  // 07
};

struct RenderPassArray
{
	static RE::BSRenderPass* MakeRenderPass(RE::BSShader* shader, RE::BSShaderProperty* property, RE::BSGeometry* geometry,
		uint32_t technique, uint8_t numLights, RE::BSLight** lights)
	{
		using func_t = decltype(&RenderPassArray::MakeRenderPass);
		REL::Relocation<func_t> func{ RELOCATION_ID(100717, 107497) };
		return func(shader, property, geometry, technique, numLights, lights);
	}

	static void ClearRenderPass(RE::BSRenderPass* pass)
	{
		using func_t = decltype(&RenderPassArray::ClearRenderPass);
		REL::Relocation<func_t> func{ RELOCATION_ID(100718, 107498) };
		func(pass);
	}

	void Clear()
	{
		while (head != nullptr) {
			RE::BSRenderPass* next = head->next;
			ClearRenderPass(head);
			head = next;
		}
		head = nullptr;
	}

	RE::BSRenderPass* EmplacePass(RE::BSShader* shader, RE::BSShaderProperty* property, RE::BSGeometry* geometry,
		uint32_t technique, uint8_t numLights = 0, RE::BSLight* light0 = nullptr, RE::BSLight* light1 = nullptr,
		RE::BSLight* light2 = nullptr, RE::BSLight* light3 = nullptr)
	{
		RE::BSLight* lights[4];
		lights[0] = light0;
		lights[1] = light1;
		lights[2] = light2;
		lights[3] = light3;
		auto* newPass = MakeRenderPass(shader, property, geometry, technique, numLights, lights);
		if (head != nullptr) {
			RE::BSRenderPass* lastPass = head;
			while (lastPass->next != nullptr) {
				lastPass = lastPass->next;
			}
			lastPass->next = newPass;
		} else {
			head = newPass;
		}
		return newPass;
	}

	RE::BSRenderPass* head;  // 00
	uint64_t unk08;          // 08
};

class BSBatchRenderer
{
public:
	struct PersistentPassList
	{
		RE::BSRenderPass* m_Head;
		RE::BSRenderPass* m_Tail;
	};

	struct GeometryGroup
	{
		BSBatchRenderer* m_BatchRenderer;
		PersistentPassList m_PassList;
		uintptr_t UnkPtr4;
		float m_Depth;  // Distance from geometry to camera location
		uint16_t m_Count;
		uint8_t m_Flags;  // Flags
	};

	struct PassGroup
	{
		RE::BSRenderPass* m_Passes[5];
		uint32_t m_ValidPassBits;  // OR'd with (1 << PassIndex)
	};

	RE::BSTArray<void*> unk008;                       // 008
	RE::BSTHashMap<RE::UnkKey, RE::UnkValue> unk020;  // 020
	std::uint64_t unk050;                             // 050
	std::uint64_t unk058;                             // 058
	std::uint64_t unk060;                             // 060
	std::uint64_t unk068;                             // 068
	GeometryGroup* m_GeometryGroups[16];
	GeometryGroup* m_AlphaGroup;
	void* unk1;
	void* unk2;
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Hooks

void* Skylighting::BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl::thunk(
	RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, [[maybe_unused]] uint32_t renderMode, [[maybe_unused]] RE::BSGraphics::BSShaderAccumulator* accumulator)
{
	auto batch = (BSBatchRenderer*)accumulator->GetRuntimeData().batchRenderer;
	batch->m_GeometryGroups[14]->m_Flags &= ~1;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	using enum BSUtilityShader::Flags;

	auto* precipitationOcclusionMapRenderPassList = reinterpret_cast<RenderPassArray*>(&property->unk0C8);

	precipitationOcclusionMapRenderPassList->Clear();

	if (!GetSingleton()->inOcclusion && property->flags.any(kSkinned, kTreeAnim) && !GetSingleton()->settings.RenderTrees)
		return precipitationOcclusionMapRenderPassList;

	if (property->flags.any(kSkinned) && !property->flags.any(kTreeAnim))
		return precipitationOcclusionMapRenderPassList;

	if (!GetSingleton()->settings.RenderTrees && property->flags.any(kTreeAnim))
		return precipitationOcclusionMapRenderPassList;

	if (property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal, kAnisotropicLighting)) {
		if (geometry->worldBound.radius > GetSingleton()->settings.MinimumBound) {
			stl::enumeration<BSUtilityShader::Flags> technique;
			technique.set(RenderDepth);

			auto pass = precipitationOcclusionMapRenderPassList->EmplacePass(
				BSUtilityShader::GetSingleton(),
				property,
				geometry,
				technique.underlying() + static_cast<uint32_t>(ShaderTechnique::UtilityGeneralStart));
			if (property->flags.any(kTreeAnim))
				pass->accumulationHint = 11;
		}
	}
	return precipitationOcclusionMapRenderPassList;
};

void Skylighting::Main_Precipitation_RenderOcclusion::thunk()
{
	State::GetSingleton()->BeginPerfEvent("PRECIPITATION MASK");
	State::GetSingleton()->SetPerfMarker("PRECIPITATION MASK");

	static bool doPrecip = false;

	auto sky = RE::Sky::GetSingleton();
	auto precip = sky->precip;

	if (GetSingleton()->settings.EnableSkylighting && GetSingleton()->settings.HeightSkylighting) {
		if (doPrecip) {
			doPrecip = false;

			auto precipObject = precip->currentPrecip;
			if (!precipObject) {
				precipObject = precip->lastPrecip;
			}
			if (precipObject) {
				Precipitation_SetupMask(precip);
				Precipitation_SetupMask(precip);  // Calling setup twice fixes an issue when it is raining
				auto effect = precipObject->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect];
				auto shaderProp = netimmerse_cast<RE::BSShaderProperty*>(effect.get());
				auto particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(shaderProp);
				auto rain = (BSParticleShaderRainEmitter*)(particleShaderProperty->particleEmitter);

				Precipitation_RenderMask(precip, rain);
			}

		} else {
			doPrecip = true;

			auto singleton = GetSingleton();

			std::chrono::time_point<std::chrono::system_clock> currentTimer = std::chrono::system_clock::now();
			auto timePassed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTimer - singleton->lastUpdateTimer).count();

			if (timePassed >= (1000.0f / 30.0f)) {
				singleton->lastUpdateTimer = currentTimer;

				singleton->eyePosition = Util::GetEyePosition(0);

				auto renderer = RE::BSGraphics::Renderer::GetSingleton();
				auto& precipitation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
				RE::BSGraphics::DepthStencilData precipitationCopy = precipitation;

				precipitation.depthSRV = singleton->occlusionTexture->srv.get();
				precipitation.texture = singleton->occlusionTexture->resource.get();
				precipitation.views[0] = singleton->occlusionTexture->dsv.get();

				static float& PrecipitationShaderCubeSize = (*(float*)REL::RelocationID(515451, 401590).address());
				float originalPrecipitationShaderCubeSize = PrecipitationShaderCubeSize;

				static RE::NiPoint3& PrecipitationShaderDirection = (*(RE::NiPoint3*)REL::RelocationID(515509, 401648).address());
				RE::NiPoint3 originalParticleShaderDirection = PrecipitationShaderDirection;

				singleton->inOcclusion = true;
				PrecipitationShaderCubeSize = 10000;

				float originaLastCubeSize = precip->lastCubeSize;
				precip->lastCubeSize = PrecipitationShaderCubeSize;

				PrecipitationShaderDirection = { 0, 0, -1 };

				Precipitation_SetupMask(precip);
				Precipitation_SetupMask(precip);  // Calling setup twice fixes an issue when it is raining

				BSParticleShaderRainEmitter* rain = new BSParticleShaderRainEmitter;
				Precipitation_RenderMask(precip, rain);
				singleton->inOcclusion = false;
				RE::BSParticleShaderCubeEmitter* cube = (RE::BSParticleShaderCubeEmitter*)rain;
				singleton->viewProjMat = cube->occlusionProjection;

				cube = nullptr;
				delete rain;

				PrecipitationShaderCubeSize = originalPrecipitationShaderCubeSize;
				precip->lastCubeSize = originaLastCubeSize;

				PrecipitationShaderDirection = originalParticleShaderDirection;

				precipitation = precipitationCopy;

				singleton->foliage = false;
			}
		}
	} else {
		func();
	}
	State::GetSingleton()->EndPerfEvent();
}

void Skylighting::BSUtilityShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	// update foliage
	if (GetSingleton()->inOcclusion) {
		GetSingleton()->foliage = Pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim);
	}
	func(This, Pass, RenderFlags);
}