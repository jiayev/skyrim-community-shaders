#include "CloudShadows.h"

#include "State.h"

#include "Deferred.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudShadows::Settings,
	EnableCloudShadows,
	CloudHeight,
	PlanetRadius,
	EffectMix,
	TransparencyPower)

enum class SkyShaderTechniques
{
	SunOcclude = 0,
	SunGlare = 1,
	MoonAndStarsMask = 2,
	Stars = 3,
	Clouds = 4,
	CloudsLerp = 5,
	CloudsFade = 6,
	Texture = 7,
	Sky = 8,
};

void CloudShadows::DrawSettings()
{
	ImGui::Checkbox("Enable Cloud Shadows", (bool*)&settings.EnableCloudShadows);

	if (ImGui::TreeNodeEx("Mixing", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Effect Mix", &settings.EffectMix, 0.0f, 1.0f, "%.2f");

		ImGui::SliderFloat("Transparency Power", &settings.TransparencyPower, -2.f, 2.f, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The amount of light absorbed by the cloud is determined by the alpha of the cloud. "
				"Negative value will result in more light absorbed, and more contrast between lit and occluded areas.");

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Geometry")) {
		ImGui::SliderFloat("Cloud Height", &settings.CloudHeight, 1e3f / 1.428e-2f, 10e3f / 1.428e-2f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::BulletText(std::format("approx: {:.2f} km / {:.2f} miles", settings.CloudHeight * 1.428e-5f, settings.CloudHeight * 8.877e-6f).data());
			ImGui::Text("This setting affects the scale of the cloud movement. Higher clouds casts greater shadow.");
		}

		ImGui::SliderFloat("Planet Radius", &settings.PlanetRadius, 2000e3f / 1.428e-2f, 10000e3f / 1.428e-2f, "%.1f units");

		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::BulletText(std::format("approx: {:.1f} km / {:.1f} miles", settings.PlanetRadius * 1.428e-5f, settings.PlanetRadius * 8.877e-6f).data());
			ImGui::Text("This setting affects distortion of clouds near horizon.");
		}
		ImGui::TreePop();
	}
}

void CloudShadows::CheckResourcesSide(int side)
{
	static Util::FrameChecker frame_checker[6];
	if (!frame_checker[side].isNewFrame())
		return;

	auto& context = State::GetSingleton()->context;

	float black[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(cubemapCloudOccRTVs[side], black);
}

void CloudShadows::CompileComputeShaders()
{
	logger::debug("Compiling shaders...");
	{
		outputProgram = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\CloudShadows\\output.cs.hlsl", { {} }, "cs_5_0"));
	}
}

void CloudShadows::ClearShaderCache()
{
	if (outputProgram)
		outputProgram->Release();
	CompileComputeShaders();
}

void CloudShadows::ModifySky(const RE::BSShader*, const uint32_t descriptor)
{
	if (!settings.EnableCloudShadows)
		return;

	auto& shadowState = State::GetSingleton()->shadowState;

	GET_INSTANCE_MEMBER(cubeMapRenderTarget, shadowState);

	if (cubeMapRenderTarget != RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS)
		return;

	enum class SkyShaderTechniques
	{
		SunOcclude = 0,
		SunGlare = 1,
		MoonAndStarsMask = 2,
		Stars = 3,
		Clouds = 4,
		CloudsLerp = 5,
		CloudsFade = 6,
		Texture = 7,
		Sky = 8,
	};

	auto tech_enum = static_cast<SkyShaderTechniques>(descriptor);
	if (tech_enum == SkyShaderTechniques::Clouds || tech_enum == SkyShaderTechniques::CloudsLerp || tech_enum == SkyShaderTechniques::CloudsFade) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& context = State::GetSingleton()->context;
		auto& device = State::GetSingleton()->device;

		{
			ID3D11ShaderResourceView* srv = nullptr;
			context->PSSetShaderResources(40, 1, &srv);
		}

		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		// render targets
		ID3D11RenderTargetView* rtvs[4];
		ID3D11DepthStencilView* depthStencil;
		context->OMGetRenderTargets(3, rtvs, &depthStencil);

		int side = -1;
		for (int i = 0; i < 6; ++i)
			if (rtvs[0] == reflections.cubeSideRTV[i]) {
				side = i;
				break;
			}
		if (side == -1)
			return;

		CheckResourcesSide(side);

		rtvs[3] = cubemapCloudOccRTVs[side];
		context->OMSetRenderTargets(4, rtvs, depthStencil);

		// blend states

		ID3D11BlendState* blendState = nullptr;
		FLOAT blendFactor[4] = { 0 };
		UINT sampleMask = 0;

		context->OMGetBlendState(&blendState, blendFactor, &sampleMask);

		if (!mappedBlendStates.contains(blendState)) {
			if (modifiedBlendStates.contains(blendState)) {
				D3D11_BLEND_DESC blendDesc;
				blendState->GetDesc(&blendDesc);

				blendDesc.RenderTarget[3] = blendDesc.RenderTarget[0];

				ID3D11BlendState* modifiedBlendState;
				DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &modifiedBlendState));

				mappedBlendStates.insert(modifiedBlendState);
				modifiedBlendStates.insert({ blendState, modifiedBlendState });
			}
			context->OMSetBlendState(modifiedBlendStates[blendState], blendFactor, sampleMask);

			resetBlendState = blendState;
		}
	}
}

void CloudShadows::DrawShadows()
{
	if (!settings.EnableCloudShadows ||
		(RE::Sky::GetSingleton()->mode.get() != RE::Sky::Mode::kFull) ||
		!RE::Sky::GetSingleton()->currentClimate)
		return;

	auto& shadowState = State::GetSingleton()->shadowState;

	GET_INSTANCE_MEMBER(cubeMapRenderTarget, shadowState);

	if (cubeMapRenderTarget != RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
		static Util::FrameChecker frame_checker;

		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& context = State::GetSingleton()->context;
		auto deferred = Deferred::GetSingleton();

		if (frame_checker.isNewFrame())
			context->GenerateMips(texCubemapCloudOcc->srv.get());

		std::array<ID3D11ShaderResourceView*, 3> srvs = { nullptr };
		std::array<ID3D11UnorderedAccessView*, 1> uavs = { nullptr };

		srvs.at(0) = perPass->SRV();
		srvs.at(1) = texCubemapCloudOcc->srv.get();
		srvs.at(2) = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;

		uavs.at(0) = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK].UAV;

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(outputProgram, nullptr, 0);
		context->Dispatch((deferred->giTexture->desc.Width + 31u) >> 5, (deferred->giTexture->desc.Height + 31u) >> 5, 1);

		// clean up
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	}
}

void CloudShadows::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	if (!settings.EnableCloudShadows ||
		(RE::Sky::GetSingleton()->mode.get() != RE::Sky::Mode::kFull) ||
		!RE::Sky::GetSingleton()->currentClimate)
		return;

	static Util::FrameChecker frame_checker;
	if (frame_checker.isNewFrame()) {
		// update settings buffer
		PerPass perPassData{};

		perPassData.Settings = settings;
		perPassData.Settings.TransparencyPower = exp2(perPassData.Settings.TransparencyPower);
		perPassData.RcpHPlusR = 1.f / (settings.CloudHeight + settings.PlanetRadius);

		perPass->Update(&perPassData, sizeof(perPassData));
	}

	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Sky:
		ModifySky(shader, descriptor);
		break;
	default:
		break;
	}
}

void CloudShadows::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void CloudShadows::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void CloudShadows::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& device = State::GetSingleton()->device;

	{
		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};

		reflections.texture->GetDesc(&texDesc);
		reflections.SRV->GetDesc(&srvDesc);

		texDesc.Format = srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;
		texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

		texCubemapCloudOcc = new Texture2D(texDesc);
		texCubemapCloudOcc->CreateSRV(srvDesc);

		for (int i = 0; i < 6; ++i) {
			reflections.cubeSideRTV[i]->GetDesc(&rtvDesc);
			rtvDesc.Format = texDesc.Format;
			DX::ThrowIfFailed(device->CreateRenderTargetView(texCubemapCloudOcc->resource.get(), &rtvDesc, cubemapCloudOccRTVs + i));
		}
	}

	{
		perPass = std::make_unique<StructuredBuffer>(StructuredBufferDesc<PerPass>(), 1);
		perPass->CreateSRV();
	}

	CompileComputeShaders();
}

void CloudShadows::RestoreDefaultSettings()
{
	settings = {};
}

void CloudShadows::Hooks::BSBatchRenderer__RenderPassImmediately::thunk(RE::BSRenderPass* Pass, uint32_t Technique, bool AlphaTest, uint32_t RenderFlags)
{
	auto feat = GetSingleton();
	auto& context = State::GetSingleton()->context;

	func(Pass, Technique, AlphaTest, RenderFlags);

	if (feat->resetBlendState) {
		ID3D11BlendState* blendState = nullptr;
		FLOAT blendFactor[4] = { 0 };
		UINT sampleMask = 0;

		context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
		context->OMSetBlendState(feat->resetBlendState, blendFactor, sampleMask);

		feat->resetBlendState = nullptr;
	}
}
