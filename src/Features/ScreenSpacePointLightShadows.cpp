#include "ScreenSpacePointLightShadows.h"

#include "State.h"
#include "Util.h"
#include "LightLimitFix.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ScreenSpacePointLightShadows::Settings,
    Enable,
    Strength,
    StepLimit,
    RayLength,
    CompareToleranceScale,
    MaxDistance)

void ScreenSpacePointLightShadows::DrawSettings()
{   
    ImGui::Checkbox("Enable Screen Space Point Light Shadows", (bool*)&settings.Enable);

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::SliderFloat("Strength", &settings.Strength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderInt("Max Step", (int*)&settings.StepLimit, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Ray Length", &settings.RayLength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Compare Tolerance Scale", &settings.CompareToleranceScale, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Max Distance", &settings.MaxDistance, 0.0f, 8192.0f, "%.2f");

    if (ImGui::CollapsingHeader("Debug")) {
        static int mip = 0;
        ImGui::SliderInt("Debug Mip Level", &mip, 0, (int)s_ShadowMips - 1, "%d", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
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
        D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

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

void ScreenSpacePointLightShadows::Prepass()
{
    auto context = globals::d3d::context;

    if (loaded && settings.Enable) {
        PrepareDepth();
    }

    std::array<ID3D11ShaderResourceView*, 2> srvs = { nullptr, nullptr };
    srvs.at(0) = depthTexture->srv.get();
    srvs.at(1) = blurredLinearDepthTexture->srv.get();
    context->PSSetShaderResources(56, 2, srvs.data());
}

ScreenSpacePointLightShadows::PerFrame ScreenSpacePointLightShadows::GetCommonBufferData()
{
    PerFrame data = {
        .Enable = settings.Enable,
        .Strength = settings.Strength,
        .StepLimit = settings.StepLimit,
        .RayLength = settings.RayLength,
        .CompareToleranceScale = settings.CompareToleranceScale,
        .MaxDistance = settings.MaxDistance
    };
    return data;
}
