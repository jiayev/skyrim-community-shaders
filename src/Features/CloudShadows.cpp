#include "CloudShadows.h"

#include "Util.h"

#include "magic_enum_flags.hpp"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudShadows::Settings,
	EnableCloudShadows,
	CloudHeight,
	PlanetRadius,
	EffectMix,
	TransparencyPower,
	AbsorptionAmbient)

class FrameChecker
{
private:
	uint32_t last_frame = UINT32_MAX;

public:
	inline bool isNewFrame(uint32_t frame)
	{
		bool retval = last_frame != frame;
		last_frame = frame;
		return retval;
	}
	inline bool isNewFrame() { return isNewFrame(RE::BSGraphics::State::GetSingleton()->uiFrameCount); }
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

		ImGui::SliderFloat("Ambient Absorption", &settings.AbsorptionAmbient, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"By default, ambient light is not affected by cloud, as it is an approximation of reflected light. "
				"However, if you want darker ambient, you may turn it up a bit. "
				"Not entirely physical, nonetheless helpful.");

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
	static FrameChecker frame_checker[6];
	if (!frame_checker[side].isNewFrame())
		return;

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	float black[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(cubemapCloudOccRTVs[side], black);
}

void CloudShadows::ModifySky(const RE::BSShader*, const uint32_t descriptor)
{
	if (!settings.EnableCloudShadows)
		return;

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	auto cubeMapRenderTarget = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cubeMapRenderTarget : shadowState->GetVRRuntimeData().cubeMapRenderTarget;
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
		auto context = renderer->GetRuntimeData().context;
		auto device = renderer->GetRuntimeData().forwarder;

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
		ID3D11BlendState* blendState;
		FLOAT blendFactor[4];
		UINT sampleMask;

		context->OMGetBlendState(&blendState, blendFactor, &sampleMask);

		if (!mappedBlendStates.contains(blendState)) {
			if (!modifiedBlendStates.contains(blendState)) {
				D3D11_BLEND_DESC blendDesc;
				blendState->GetDesc(&blendDesc);

				blendDesc.RenderTarget[3] = blendDesc.RenderTarget[0];

				ID3D11BlendState* modifiedBlendState;
				DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &modifiedBlendState));

				mappedBlendStates.insert(modifiedBlendState);
				modifiedBlendStates.insert({ blendState, modifiedBlendState });
			}
			blendState = modifiedBlendStates[blendState];
			context->OMSetBlendState(blendState, blendFactor, sampleMask);
		}
	}
}

void CloudShadows::ModifyLighting()
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	auto cubeMapRenderTarget = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cubeMapRenderTarget : shadowState->GetVRRuntimeData().cubeMapRenderTarget;
	if (cubeMapRenderTarget != RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
		static FrameChecker frame_checker;
		if (frame_checker.isNewFrame())
			context->GenerateMips(texCubemapCloudOcc->srv.get());

		auto srv = texCubemapCloudOcc->srv.get();
		context->PSSetShaderResources(40, 1, &srv);
	} else {
		ID3D11ShaderResourceView* srv = nullptr;
		context->PSSetShaderResources(40, 1, &srv);
	}

	ID3D11ShaderResourceView* views[1]{};
	views[0] = perPass->srv.get();
	context->PSSetShaderResources(23, ARRAYSIZE(views), views);
}

void CloudShadows::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	static FrameChecker frame_checker;
	if (frame_checker.isNewFrame()) {
		// update settings buffer
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		PerPass perPassData{};

		perPassData.Settings = settings;
		perPassData.Settings.TransparencyPower = exp2(perPassData.Settings.TransparencyPower);
		perPassData.RcpHPlusR = 1.f / (settings.CloudHeight + settings.PlanetRadius);

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, &perPassData, bytes);
		context->Unmap(perPass->resource.get(), 0);
	}

	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Sky:
		ModifySky(shader, descriptor);
		break;
	case RE::BSShader::Type::Lighting:
	case RE::BSShader::Type::DistantTree:
	case RE::BSShader::Type::Grass:
		// case RE::BSShader::Type::Water:
		ModifyLighting();
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
	auto device = renderer->GetRuntimeData().forwarder;

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
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);
		perPass = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		perPass->CreateSRV(srvDesc);
	}
}

void CloudShadows::RestoreDefaultSettings()
{
	settings = {};
}