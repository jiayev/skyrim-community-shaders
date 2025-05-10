#include "ScreenSpacePointLightShadows.h"

#include "State.h"
#include "Util.h"
#include "LightLimitFix.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ScreenSpacePointLightShadows::Settings,
    Enable)

void ScreenSpacePointLightShadows::DrawSettings()
{   
    ImGui::Checkbox("Enable Screen Space Point Light Shadows", (bool*)&settings.Enable);

    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Debug")) {
        static int mip = 0;
        ImGui::SliderInt("Debug Mip Level", &mip, 0, (int)s_ShadowMips - 1, "%d", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
        ImGui::BulletText("ShadowTexture");
        ImGui::Image(shadowSRVs[mip].get(), { shadowTexture->width * .2f, shadowTexture->height * .2f });
        ImGui::BulletText("DepthTexture");
        ImGui::Image(depthSRVs[mip].get(), { depthTexture->width * .2f, depthTexture->height * .2f });
        ImGui::BulletText("LinearDepthTexture");
        ImGui::Image(linearDepthSRVs[mip].get(), { linearDepthTexture->width * .2f, linearDepthTexture->height * .2f });
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
        { &createDepthCS, "CreateDepthCS.hlsl", {} },
        { &raymarchCS, "RaymarchCS.hlsl", {} },
        { &depthAwareBlurCS, "DepthAwareBlurCS.hlsl", {} },
    };

    for (auto& info : shaderInfos) {
        auto path = std::filesystem::path("Data\\Shaders\\ScreenSpacePointLightShadows") / info.filename;
        if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry)))
            info.programPtr->attach(rawPtr);
    }
}

void ScreenSpacePointLightShadows::SetupResources()
{
    auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

    logger::debug("Creating buffers...");
    {
        ssplsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SSPLSCB>());
    }

    logger::debug("Creating 2D textures...");
	{
        auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
        D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

        shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

        shadowTexture = std::make_unique<Texture2D>(texDesc);
        shadowTexture->CreateSRV(srvDesc);

        for (uint i = 0; i < s_ShadowMips; i++) {
            D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
			};

            DX::ThrowIfFailed(device->CreateShaderResourceView(shadowTexture->resource.get(), &mipSrvDesc, shadowSRVs[i].put()));

            D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
                .Format = texDesc.Format,
                .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = i }
			};

            DX::ThrowIfFailed(device->CreateUnorderedAccessView(shadowTexture->resource.get(), &mipUavDesc, shadowUAVs[i].put()));
        }

        auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

        depth->texture->GetDesc(&texDesc);
        depth->depthSRV->GetDesc(&srvDesc);

        texDesc.Format = DXGI_FORMAT_R16_FLOAT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        srvDesc.Format = texDesc.Format;
        
        depthTexture = std::make_unique<Texture2D>(texDesc);
        depthTexture->CreateSRV(srvDesc);

        linearDepthTexture = std::make_unique<Texture2D>(texDesc);
        linearDepthTexture->CreateSRV(srvDesc);

        for (uint i = 0; i < s_ShadowMips; i++) {
            D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
                .Format = texDesc.Format,
                .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
                .Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
            };
            DX::ThrowIfFailed(device->CreateShaderResourceView(depthTexture->resource.get(), &mipSrvDesc, depthSRVs[i].put()));
            DX::ThrowIfFailed(device->CreateShaderResourceView(linearDepthTexture->resource.get(), &mipSrvDesc, linearDepthSRVs[i].put()));

            D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
                .Format = texDesc.Format,
                .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
                .Texture2D = { .MipSlice = i }
            };
            DX::ThrowIfFailed(device->CreateUnorderedAccessView(depthTexture->resource.get(), &mipUavDesc, depthUAVs[i].put()));
            DX::ThrowIfFailed(device->CreateUnorderedAccessView(linearDepthTexture->resource.get(), &mipUavDesc, linearDepthUAVs[i].put()));
        }
    }

    logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
			.AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
			.AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};

		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler.put()));
    }

    CompileComputeShaders();
}

void ScreenSpacePointLightShadows::ClearShaderCache()
{
    if (createDepthCS) {
        createDepthCS->Release();
        createDepthCS = nullptr;
    }
    if (raymarchCS) {
        raymarchCS->Release();
        raymarchCS = nullptr;
    }
    if (depthAwareBlurCS) {
        depthAwareBlurCS->Release();
        depthAwareBlurCS = nullptr;
    }

    CompileComputeShaders();
}

void ScreenSpacePointLightShadows::DrawShadows()
{
    auto context = globals::d3d::context;
    auto renderer = globals::game::renderer;

    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    context->CSSetShaderResources(0, 1, &depth.depthSRV);

    // Create Depth Mips Pass
    {
        std::array<ID3D11UnorderedAccessView*, 8> uavs = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        for (uint i = 0; i < s_ShadowMips; i++) {
            uavs[i] = depthUAVs[i].get();
            uavs[i + s_ShadowMips] = linearDepthUAVs[i].get();
        }
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
        context->CSSetShader(createDepthCS.get(), nullptr, 0);
        context->CSSetSamplers(0, 1, &linearSampler.get());

        context->Dispatch(((shadowTexture->desc.Width + 7) >> 3), ((shadowTexture->desc.Height + 7) >> 3), 1);

        context->CSSetShader(nullptr, nullptr, 0);
        context->CSSetShaderResources(0, 1, nullptr);
        uavs.fill(nullptr);
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    }
}

void ScreenSpacePointLightShadows::Prepass()
{
    auto context = globals::d3d::context;

    float white = 1.0f;
    context->ClearUnorderedAccessViewFloat(shadowUAVs[0].get(), &white, 0, nullptr);

    if (globals::features::lightLimitFix->loaded && settings.Enable) {
        DrawShadows();
    }

    auto view = shadowSRVs[0].get();
    context->PSSetShaderResources(56, 1, &view);
}

