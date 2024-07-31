#include "Skylighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skylighting::Settings,
	DirectionalDiffuse,
	MaxZenith,
	MaxFrames,
	MinDiffuseVisibility,
	DiffuseBrightness,
	MinSpecularVisibility,
	SpecularBrightness)

void Skylighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void Skylighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skylighting::RestoreDefaultSettings()
{
	settings = {};
}

void Skylighting::DrawSettings()
{
	ImGui::Checkbox("Directional Diffuse", &settings.DirectionalDiffuse);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Extra darkening depending on surface orientation.\n"
			"More physically correct, but may impact the intended visual of certain weathers.");

	ImGui::SliderFloat("Diffuse Min Visibility", &settings.MinDiffuseVisibility, 0, 1, "%.2f");
	ImGui::SliderFloat("Diffuse Brightness", &settings.DiffuseBrightness, 0.3, 3, "%.1f");
	ImGui::SliderFloat("Specular Min Visibility", &settings.MinSpecularVisibility, 0, 1, "%.2f");
	ImGui::SliderFloat("Specular Brightness", &settings.SpecularBrightness, 0.3, 3, "%.1f");

	ImGui::Separator();

	if (ImGui::Button("Rebuild Skylighting")) {
		auto& context = State::GetSingleton()->context;
		UINT clr[1] = { 0 };
		context->ClearUnorderedAccessViewUint(texAccumFramesArray->uav.get(), clr);
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Changes below require rebuilding, a loading screen, or moving away from the current location to apply.");

	ImGui::SliderInt("Update Frames", &settings.MaxFrames, 0, 255, "%d", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Aggregating over how many frames to build up skylighting.");

	ImGui::SliderAngle("Max Zenith Angle", &settings.MaxZenith, 0, 90);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Smaller angles creates more focused top-down shadow.");
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

void Skylighting::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& device = State::GetSingleton()->device;

	{
		skylightingCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SkylightingCB>());
	}

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(&texDesc);
		precipitationOcclusion.depthSRV->GetDesc(&srvDesc);
		precipitationOcclusion.views[0]->GetDesc(&dsvDesc);

		texOcclusion = new Texture2D(texDesc);
		texOcclusion->CreateSRV(srvDesc);
		texOcclusion->CreateDSV(dsvDesc);
	}

	{
		D3D11_TEXTURE3D_DESC texDesc{
			.Width = probeArrayDims[0],
			.Height = probeArrayDims[1],
			.Depth = probeArrayDims[2],
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MipSlice = 0,
				.FirstWSlice = 0,
				.WSize = texDesc.Depth }
		};

		texProbeArray = new Texture3D(texDesc);
		texProbeArray->CreateSRV(srvDesc);
		texProbeArray->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UINT;

		texAccumFramesArray = new Texture3D(texDesc);
		texAccumFramesArray->CreateSRV(srvDesc);
		texAccumFramesArray->CreateUAV(uavDesc);
	}

	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
	}

	CompileComputeShaders();
}

void Skylighting::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&probeUpdateCompute
	};

	for (auto shader : shaderPtrs)
		if (*shader) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
	if (foliagePixelShader) {
		foliagePixelShader->Release();
		foliagePixelShader = nullptr;
	}
}

void Skylighting::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &probeUpdateCompute, "updateProbes.cs.hlsl", {} },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\Skylighting") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}
}

void Skylighting::Prepass()
{
	auto& context = State::GetSingleton()->context;

	{
		static float3 prevCellID = { 0, 0, 0 };

		auto eyePosNI = Util::GetEyePosition(0);
		auto eyePos = float3{ eyePosNI.x, eyePosNI.y, eyePosNI.z };

		float3 cellSize = {
			occlusionDistance / probeArrayDims[0],
			occlusionDistance / probeArrayDims[1],
			occlusionDistance * .5f / probeArrayDims[2]
		};
		auto cellID = eyePos / cellSize;
		cellID = { round(cellID.x), round(cellID.y), round(cellID.z) };
		auto cellOrigin = cellID * cellSize;
		float3 cellIDDiff = prevCellID - cellID;

		cbData = {
			.OcclusionViewProj = OcclusionTransform,
			.OcclusionDir = OcclusionDir,
			.PosOffset = cellOrigin - eyePos,
			.ArrayOrigin = {
				((int)cellID.x - probeArrayDims[0] / 2) % probeArrayDims[0],
				((int)cellID.y - probeArrayDims[1] / 2) % probeArrayDims[1],
				((int)cellID.z - probeArrayDims[2] / 2) % probeArrayDims[2],
				(uint)settings.MaxFrames },
			.ValidMargin = { (int)cellIDDiff.x, (int)cellIDDiff.y, (int)cellIDDiff.z },
			.MixParams = { settings.MinDiffuseVisibility, settings.DiffuseBrightness, settings.MinSpecularVisibility, settings.SpecularBrightness },
			.DirectionalDiffuse = settings.DirectionalDiffuse,
		};

		skylightingCB->Update(cbData);

		prevCellID = cellID;
	}

	std::array<ID3D11ShaderResourceView*, 1> srvs = { texOcclusion->srv.get() };
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { texProbeArray->uav.get(), texAccumFramesArray->uav.get() };
	std::array<ID3D11SamplerState*, 1> samplers = { pointClampSampler.get() };
	auto cb = skylightingCB->CB();

	// update probe array
	{
		context->CSSetConstantBuffers(1, 1, &cb);
		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(probeUpdateCompute.get(), nullptr, 0);
		context->Dispatch((probeArrayDims[0] + 7u) >> 3, (probeArrayDims[1] + 7u) >> 3, probeArrayDims[2]);
	}

	// reset
	{
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		samplers.fill(nullptr);
		cb = nullptr;

		context->CSSetConstantBuffers(1, 1, &cb);
		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// set PS shader resource
	{
		ID3D11ShaderResourceView* srv = texProbeArray->srv.get();
		context->PSSetShaderResources(29, 1, &srv);
	}
}

void Skylighting::PostPostLoad()
{
	logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
	stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
	stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));
	stl::write_thunk_call<SetViewFrustum>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));
	stl::write_vfunc<0x6, BSUtilityShader_SetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
}

//////////////////////////////////////////////////////////////

struct BSParticleShaderRainEmitter
{
	void* vftable_BSParticleShaderRainEmitter_0;
	char _pad_8[4056];
};

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
		static REL::Relocation<BSUtilityShader**> singleton{ RELOCATION_ID(528354, 415300) };
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
		static REL::Relocation<func_t> func{ RELOCATION_ID(100717, 107497) };
		return func(shader, property, geometry, technique, numLights, lights);
	}

	static void ClearRenderPass(RE::BSRenderPass* pass)
	{
		using func_t = decltype(&RenderPassArray::ClearRenderPass);
		static REL::Relocation<func_t> func{ RELOCATION_ID(100718, 107498) };
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

//////////////////////////////////////////////////////////////

static void Precipitation_SetupMask(RE::Precipitation* a_This)
{
	using func_t = decltype(&Precipitation_SetupMask);
	static REL::Relocation<func_t> func{ REL::RelocationID(25641, 26183) };
	func(a_This);
}

static void Precipitation_RenderMask(RE::Precipitation* a_This, BSParticleShaderRainEmitter* a_emitter)
{
	using func_t = decltype(&Precipitation_RenderMask);
	static REL::Relocation<func_t> func{ REL::RelocationID(25642, 26184) };
	func(a_This, a_emitter);
}

void* Skylighting::BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl::thunk(
	RE::BSLightingShaderProperty* property,
	RE::BSGeometry* geometry,
	[[maybe_unused]] uint32_t renderMode,
	[[maybe_unused]] RE::BSGraphics::BSShaderAccumulator* accumulator)
{
	auto batch = (BSBatchRenderer*)accumulator->GetRuntimeData().batchRenderer;
	batch->m_GeometryGroups[14]->m_Flags &= ~1;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	using enum BSUtilityShader::Flags;

	auto* precipitationOcclusionMapRenderPassList = reinterpret_cast<RenderPassArray*>(&property->unk0C8);

	precipitationOcclusionMapRenderPassList->Clear();
	if (GetSingleton()->inOcclusion && !GetSingleton()->renderTrees) {
		if (property->flags.any(kSkinned) && property->flags.none(kTreeAnim))
			return precipitationOcclusionMapRenderPassList;
	} else {
		if (property->flags.any(kSkinned))
			return precipitationOcclusionMapRenderPassList;
	}

	if (property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal, kAnisotropicLighting) && !(property->flags.any(kSkinned) && property->flags.none(kTreeAnim))) {
		if (geometry->worldBound.radius > GetSingleton()->boundSize) {
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
}

void Skylighting::Main_Precipitation_RenderOcclusion::thunk()
{
	State::GetSingleton()->BeginPerfEvent("PRECIPITATION MASK");
	State::GetSingleton()->SetPerfMarker("PRECIPITATION MASK");

	static bool doPrecip = false;

	auto sky = RE::Sky::GetSingleton();
	auto precip = sky->precip;

	auto singleton = GetSingleton();

	if (singleton->doOcclusion) {
		{
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
		}

		{
			doPrecip = true;

			std::chrono::time_point<std::chrono::system_clock> currentTimer = std::chrono::system_clock::now();
			auto timePassed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTimer - singleton->lastUpdateTimer).count();

			if (timePassed >= (1000.0f / 30.0f)) {
				singleton->lastUpdateTimer = currentTimer;

				auto renderer = RE::BSGraphics::Renderer::GetSingleton();
				auto& precipitation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
				RE::BSGraphics::DepthStencilData precipitationCopy = precipitation;

				precipitation.depthSRV = singleton->texOcclusion->srv.get();
				precipitation.texture = singleton->texOcclusion->resource.get();
				precipitation.views[0] = singleton->texOcclusion->dsv.get();

				static float& PrecipitationShaderCubeSize = (*(float*)REL::RelocationID(515451, 401590).address());
				float originalPrecipitationShaderCubeSize = PrecipitationShaderCubeSize;

				static RE::NiPoint3& PrecipitationShaderDirection = (*(RE::NiPoint3*)REL::RelocationID(515509, 401648).address());
				RE::NiPoint3 originalParticleShaderDirection = PrecipitationShaderDirection;

				singleton->inOcclusion = true;
				PrecipitationShaderCubeSize = singleton->occlusionDistance;

				float originaLastCubeSize = precip->lastCubeSize;
				precip->lastCubeSize = PrecipitationShaderCubeSize;

				float2 vPoint;
				{
					constexpr float rcpRandMax = 1.f / RAND_MAX;
					static int randSeed = std::rand();
					static uint randFrameCount = 0;

					// r2 sequence
					vPoint = float2(randSeed * rcpRandMax) + (float)randFrameCount * float2(0.245122333753f, 0.430159709002f);
					vPoint.x -= static_cast<unsigned long long>(vPoint.x);
					vPoint.y -= static_cast<unsigned long long>(vPoint.y);

					randFrameCount++;
					if (randFrameCount == 1000) {
						randFrameCount = 0;
						randSeed = std::rand();
					}

					// disc transformation
					vPoint.x = sqrt(vPoint.x * sin(singleton->settings.MaxZenith));
					vPoint.y *= 6.28318530718f;

					vPoint = { vPoint.x * cos(vPoint.y), vPoint.x * sin(vPoint.y) };
				}

				float3 PrecipitationShaderDirectionF = -float3{ vPoint.x, vPoint.y, sqrt(1 - vPoint.LengthSquared()) };
				PrecipitationShaderDirectionF.Normalize();

				PrecipitationShaderDirection = { PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z };

				Precipitation_SetupMask(precip);
				Precipitation_SetupMask(precip);  // Calling setup twice fixes an issue when it is raining

				BSParticleShaderRainEmitter* rain = new BSParticleShaderRainEmitter;
				Precipitation_RenderMask(precip, rain);
				singleton->inOcclusion = false;
				RE::BSParticleShaderCubeEmitter* cube = (RE::BSParticleShaderCubeEmitter*)rain;

				singleton->OcclusionDir = -float4{ PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z, 0 };
				singleton->OcclusionTransform = cube->occlusionProjection;

				cube = nullptr;
				delete rain;

				PrecipitationShaderCubeSize = originalPrecipitationShaderCubeSize;
				precip->lastCubeSize = originaLastCubeSize;

				PrecipitationShaderDirection = originalParticleShaderDirection;

				precipitation = precipitationCopy;

				singleton->foliage = false;
			}
		}
	}
	State::GetSingleton()->EndPerfEvent();
}

void Skylighting::BSUtilityShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& feat = *GetSingleton();
	if (feat.inOcclusion) {
		feat.foliage = Pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim);
	}

	func(This, Pass, RenderFlags);
}

void Skylighting::SetViewFrustum::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum)
{
	if (GetSingleton()->inOcclusion) {
		static float frameCount = 0;

		uint corner = (uint)frameCount % 4;

		a_frustum->fBottom = (corner == 0 || corner == 1) ? -5000.0f : 0.0f;

		a_frustum->fLeft = (corner == 0 || corner == 2) ? -5000.0f : 0.0f;
		a_frustum->fRight = (corner == 1 || corner == 3) ? 5000.0f : 0.0f;

		a_frustum->fTop = (corner == 2 || corner == 3) ? 5000.0f : 0.0f;

		frameCount += 0.5f;
	}

	func(a_camera, a_frustum);
}