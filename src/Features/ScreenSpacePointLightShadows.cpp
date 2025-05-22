#include "ScreenSpacePointLightShadows.h"

#include "LightLimitFix.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpacePointLightShadows::Settings,
	Enable,
	Scale)

void ScreenSpacePointLightShadows::DrawSettings()
{
	ImGui::Checkbox("Enable Screen Space Point Light Shadows", (bool*)&settings.Enable);
	ImGui::SliderFloat("Raymarch Scale", &settings.Scale, 0.5f, 10.f, "%.2f");

	ImGui::Spacing();
	ImGui::Spacing();

	if (ImGui::CollapsingHeader("Debug")) {
		static int mip = 0;
		ImGui::SliderInt("Debug Mip Level", &mip, 0, (int)s_ShadowMips - 1, "%d", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
		ImGui::BulletText("shadowTexture");
		ImGui::Image(shadowSRVs[mip].get(), { shadowTexture->desc.Width * .2f, shadowTexture->desc.Height * .2f });
		ImGui::BulletText("blurredShadowTexture");
		ImGui::Image(blurredShadowSRVs[mip].get(), { blurredShadowTexture->desc.Width * .2f, blurredShadowTexture->desc.Height * .2f });
		ImGui::BulletText("depthTexture");
		ImGui::Image(depthSRVs[mip].get(), { depthTexture->desc.Width * .2f, depthTexture->desc.Height * .2f });
		ImGui::BulletText("linearDepthTexture");
		ImGui::Image(linearDepthSRVs[mip].get(), { linearDepthTexture->desc.Width * .8f, linearDepthTexture->desc.Height * .8f });
		ImGui::BulletText("blurredLinearDepthTexture");
		ImGui::Image(blurredLinearDepthSRVs[mip].get(), { blurredLinearDepthTexture->desc.Width * .8f, blurredLinearDepthTexture->desc.Height * .8f });
	}
}

void ScreenSpacePointLightShadows::RestoreDefaultSettings()
{
	settings = {};
}

void ScreenSpacePointLightShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ScreenSpacePointLightShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpacePointLightShadows::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &createDepthCS, "createDepthCS.hlsl", {} },
		{ &blurDepthCS, "blurDepthCS.hlsl", {} },
		{ &raymarchCS, "raymarchCS.hlsl", {} },
		{ &depthAwareBlurCS, "depthAwareBlurCS.hlsl", {} },
		{ &depthAwareUpscaleCS, "depthAwareUpscaleCS.hlsl", {} },
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpacePointLightShadows") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

void ScreenSpacePointLightShadows::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		ssplsCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSPLSCB>());
	}

	logger::debug("Creating 2D textures...");
	{
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = s_ShadowMips;
		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		shadowTexture = eastl::make_unique<Texture2D>(texDesc);
		shadowTexture->CreateSRV(srvDesc);
		shadowTexture->CreateUAV(uavDesc);
		blurredShadowTexture = eastl::make_unique<Texture2D>(texDesc);
		blurredShadowTexture->CreateSRV(srvDesc);
		blurredShadowTexture->CreateUAV(uavDesc);

		for (uint i = 0; i < s_ShadowMips; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
			};
			DX::ThrowIfFailed(device->CreateShaderResourceView(shadowTexture->resource.get(), &mipSrvDesc, shadowSRVs[i].put()));
			DX::ThrowIfFailed(device->CreateShaderResourceView(blurredShadowTexture->resource.get(), &mipSrvDesc, blurredShadowSRVs[i].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = i }
			};
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(shadowTexture->resource.get(), &mipUavDesc, shadowUAVs[i].put()));
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(blurredShadowTexture->resource.get(), &mipUavDesc, blurredShadowUAVs[i].put()));
		}

		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

		depth.texture->GetDesc(&texDesc);
		depth.depthSRV->GetDesc(&srvDesc);

		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = s_ShadowMips;
		srvDesc.Format = texDesc.Format;

		depthTexture = eastl::make_unique<Texture2D>(texDesc);
		depthTexture->CreateSRV(srvDesc);

		for (uint i = 0; i < s_ShadowMips; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
			};
			DX::ThrowIfFailed(device->CreateShaderResourceView(depthTexture->resource.get(), &mipSrvDesc, depthSRVs[i].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = i }
			};
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(depthTexture->resource.get(), &mipUavDesc, depthUAVs[i].put()));
		}

		texDesc.Width /= 4;
		texDesc.Height /= 4;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		srvDesc.Format = texDesc.Format;

		linearDepthTexture = eastl::make_unique<Texture2D>(texDesc);
		linearDepthTexture->CreateSRV(srvDesc);
		blurredLinearDepthTexture = eastl::make_unique<Texture2D>(texDesc);
		blurredLinearDepthTexture->CreateSRV(srvDesc);

		for (uint i = 0; i < s_ShadowMips; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
			};
			DX::ThrowIfFailed(device->CreateShaderResourceView(linearDepthTexture->resource.get(), &mipSrvDesc, linearDepthSRVs[i].put()));
			DX::ThrowIfFailed(device->CreateShaderResourceView(blurredLinearDepthTexture->resource.get(), &mipSrvDesc, blurredLinearDepthSRVs[i].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = i }
			};
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(linearDepthTexture->resource.get(), &mipUavDesc, linearDepthUAVs[i].put()));
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(blurredLinearDepthTexture->resource.get(), &mipUavDesc, blurredLinearDepthUAVs[i].put()));
		}
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};

		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	}

	CompileComputeShaders();
}

void ScreenSpacePointLightShadows::ClearShaderCache()
{
	if (createDepthCS) {
		createDepthCS->Release();
		createDepthCS = nullptr;
	}
	if (blurDepthCS) {
		blurDepthCS->Release();
		blurDepthCS = nullptr;
	}
	if (raymarchCS) {
		raymarchCS->Release();
		raymarchCS = nullptr;
	}
	if (depthAwareBlurCS) {
		depthAwareBlurCS->Release();
		depthAwareBlurCS = nullptr;
	}
	if (depthAwareUpscaleCS) {
		depthAwareUpscaleCS->Release();
		depthAwareUpscaleCS = nullptr;
	}

	CompileComputeShaders();
}

void ScreenSpacePointLightShadows::PrepareDepth()
{
	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	state->BeginPerfEvent("ScreenSpacePointLightShadows::PrepareDepth");

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	context->CSSetShaderResources(0, 1, &depth.depthSRV);

	std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };

	SSPLSCB cbData = {
		.MipLevel = 0,
		.Scale = settings.Scale,
		.ResX = 0,
		.ResY = 0
	};

	auto cb = ssplsCB->CB();
	ID3D11ShaderResourceView* srv = nullptr;
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { nullptr };

	// Create Depth and Downsample Linear Depth Textures
	{
		cbData.ResX = depthTexture->desc.Width / 4;
		cbData.ResY = depthTexture->desc.Height / 4;
		ssplsCB->Update(cbData);
		context->CSSetConstantBuffers(1, 1, &cb);

		uavs.at(0) = depthUAVs[0].get();
		uavs.at(1) = linearDepthUAVs[0].get();
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(createDepthCS.get(), nullptr, 0);
		context->CSSetSamplers(0, 1, samplers.data());

		context->Dispatch(((depthTexture->desc.Width - 1) >> 5) + 1, ((depthTexture->desc.Height - 1) >> 5) + 1, 1);

		context->CSSetShader(nullptr, nullptr, 0);
		srv = nullptr;
		context->CSSetShaderResources(0, 1, &srv);
		uavs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		ID3D11SamplerState* sampler = nullptr;
		context->CSSetSamplers(0, 1, &sampler);
	}

	context->GenerateMips(depthTexture->srv.get());
	context->GenerateMips(linearDepthTexture->srv.get());

	// Blur Linear Depth Map
	for (uint i = 0; i < s_ShadowMips; i++) {
		cbData.MipLevel = i;
		srv = linearDepthSRVs[i].get();
		context->CSSetShaderResources(0, 1, &srv);
		uavs.at(0) = blurredLinearDepthUAVs[i].get();
		context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
		context->CSSetSamplers(0, 1, samplers.data());
		context->CSSetShader(blurDepthCS.get(), nullptr, 0);

		uint mipWidth = linearDepthTexture->desc.Width >> i;
		uint mipHeight = linearDepthTexture->desc.Height >> i;

		cbData.ResX = mipWidth;
		cbData.ResY = mipHeight;

		ssplsCB->Update(cbData);

		context->CSSetConstantBuffers(1, 1, &cb);
		context->Dispatch(((mipWidth - 1) >> 2) + 1, ((mipHeight - 1) >> 2) + 1, 1);

		context->CSSetShader(nullptr, nullptr, 0);
		srv = nullptr;
		context->CSSetShaderResources(0, 1, &srv);
		uavs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		ID3D11SamplerState* sampler = nullptr;
		context->CSSetSamplers(0, 1, &sampler);
	}

	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);

	state->EndPerfEvent();
}

void ScreenSpacePointLightShadows::DrawShadows()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	auto llf = globals::features::lightLimitFix;

	state->BeginPerfEvent("ScreenSpacePointLightShadows::DrawShadows");

	std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };

	SSPLSCB cbData = {
		.MipLevel = 0,
		.Scale = settings.Scale,
		.ResX = 0,
		.ResY = 0
	};

	auto cb = ssplsCB->CB();

	std::array<ID3D11ShaderResourceView*, 2> srvs = { nullptr, nullptr };

	// Raymarch, Blur and Upscale
	for (int i = s_ShadowMips - 1; i >= 0; i--) {
		cbData.MipLevel = i;
		uint mipWidth = depthTexture->desc.Width >> i;
		uint mipHeight = depthTexture->desc.Height >> i;

		cbData.ResX = mipWidth;
		cbData.ResY = mipHeight;
		ssplsCB->Update(cbData);
		context->CSSetConstantBuffers(1, 1, &cb);

		// Depthaware Upscale
		if (i != s_ShadowMips - 1) {
			srvs.at(0) = depthTexture->srv.get();
			srvs.at(1) = blurredShadowSRVs[i + 1].get();
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			auto uav = shadowUAVs[i].get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetSamplers(0, 1, samplers.data());
			context->CSSetShader(depthAwareUpscaleCS.get(), nullptr, 0);

			context->Dispatch(((mipWidth - 1) >> 3) + 1, ((mipHeight - 1) >> 3) + 1, 1);

			context->CSSetShader(nullptr, nullptr, 0);
			srvs.fill(nullptr);
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			context->Flush();
		}

		ID3D11ShaderResourceView* views[3]{};
		views[0] = llf->lights->srv.get();
		views[1] = llf->lightIndexList->srv.get();
		views[2] = llf->lightGrid->srv.get();
		context->CSSetShaderResources(35, ARRAYSIZE(views), views);

		ID3D11Buffer* buffer = { llf->strictLightDataCB->CB() };
		context->CSSetConstantBuffers(3, 1, &buffer);

		// Raymarch
		srvs.at(0) = blurredLinearDepthTexture->srv.get();
		srvs.at(1) = depthTexture->srv.get();
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		auto uav = shadowUAVs[i].get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetSamplers(0, 1, samplers.data());
		context->CSSetShader(raymarchCS.get(), nullptr, 0);

		context->Dispatch(((mipWidth - 1) >> 3) + 1, ((mipHeight - 1) >> 3) + 1, 1);

		context->CSSetShader(nullptr, nullptr, 0);
		srvs.fill(nullptr);
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		views[0] = nullptr;
		views[1] = nullptr;
		views[2] = nullptr;
		context->CSSetShaderResources(35, ARRAYSIZE(views), views);
		buffer = nullptr;
		context->CSSetConstantBuffers(3, 1, &buffer);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		// Depthaware Blur
		srvs.at(0) = depthSRVs[i].get();
		srvs.at(1) = shadowSRVs[i].get();
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		uav = blurredShadowUAVs[i].get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetSamplers(0, 1, samplers.data());
		context->CSSetShader(depthAwareBlurCS.get(), nullptr, 0);

		context->Dispatch(((mipWidth - 1) >> 3) + 1, ((mipHeight - 1) >> 3) + 1, 1);

		context->CSSetShader(nullptr, nullptr, 0);
		srvs.fill(nullptr);
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		ID3D11SamplerState* sampler = nullptr;
		context->CSSetSamplers(0, 1, &sampler);
	}

	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);

	state->EndPerfEvent();
}

void ScreenSpacePointLightShadows::Prepass()
{
	auto context = globals::d3d::context;

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(shadowTexture->uav.get(), white);

	if (globals::features::lightLimitFix->loaded && settings.Enable) {
		PrepareDepth();
		DrawShadows();
	}

	auto view = shadowSRVs[0].get();
	context->PSSetShaderResources(56, 1, &view);
}
