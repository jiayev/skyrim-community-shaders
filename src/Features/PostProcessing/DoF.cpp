#include "DoF.h"

#include "State.h"
#include "Util.h"
#include "Menu.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    DoF::Settings,
    AutoFocus,
    TransitionSpeed,
    FocusCoord,
    ManualFocusPlane,
    FocalLength,
    FNumber,
    BlurQuality,
    NearFarDistanceCompensation,
    HighlightBoost,
    BokehBusyFactor
)

void DoF::DrawSettings()
{
    ImGui::Checkbox("Auto Focus", &settings.AutoFocus);

    if (settings.AutoFocus) {
        ImGui::SliderFloat2("Focus Point", &settings.FocusCoord.x, 0.0f, 1.0f, "%.2f");
    }
    ImGui::SliderFloat("Transition Speed", &settings.TransitionSpeed, 0.1f, 1.0f, "%.2f");
    ImGui::SliderFloat("Manual Focus", &settings.ManualFocusPlane, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Focal Length", &settings.FocalLength, 1.0f, 300.0f, "%.1f mm");
    ImGui::SliderFloat("F-Number", &settings.FNumber, 1.0f, 22.0f, "f/%.1f");
    ImGui::SliderFloat("Blur Quality", &settings.BlurQuality, 2.0f, 30.0f, "%.1f");
    ImGui::SliderFloat("Near-Far Plane Distance Compenation", &settings.NearFarDistanceCompensation, 1.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Bokeh Busy Factor", &settings.BokehBusyFactor, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Highlight Boost", &settings.HighlightBoost, 0.0f, 1.0f, "%.2f");

    if (ImGui::CollapsingHeader("Debug")) {
        static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

        BUFFER_VIEWER_NODE(texFocus, 64.0f)
        BUFFER_VIEWER_NODE(texPreFocus, 64.0f)

        BUFFER_VIEWER_NODE(texCoC, debugRescale)
        BUFFER_VIEWER_NODE(texCoCBlur1, debugRescale)
        BUFFER_VIEWER_NODE(texCoCBlur2, debugRescale)

        BUFFER_VIEWER_NODE(texPreBlurred, debugRescale)
        BUFFER_VIEWER_NODE(texFarBlurred, debugRescale)
    }
}

void DoF::RestoreDefaultSettings()
{
    settings = {};
}

void DoF::LoadSettings(json& o_json)
{
    settings = o_json;
}

void DoF::SaveSettings(json& o_json)
{
    o_json = settings;
}

void DoF::SetupResources()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = State::GetSingleton()->device;

	logger::debug("Creating buffers...");
	{
        dofCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DoFCB>());
    }

    logger::debug("Creating 2D textures...");
	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texOutput = eastl::make_unique<Texture2D>(texDesc);
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateUAV(uavDesc);

        texPreBlurred = eastl::make_unique<Texture2D>(texDesc);
        texPreBlurred->CreateSRV(srvDesc);
        texPreBlurred->CreateUAV(uavDesc);

        texFarBlurred = eastl::make_unique<Texture2D>(texDesc);
        texFarBlurred->CreateSRV(srvDesc);
        texFarBlurred->CreateUAV(uavDesc);

        texDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;

        texCoC = eastl::make_unique<Texture2D>(texDesc);
        texCoC->CreateSRV(srvDesc);
        texCoC->CreateUAV(uavDesc);

        texCoCBlur1 = eastl::make_unique<Texture2D>(texDesc);
        texCoCBlur1->CreateSRV(srvDesc);
        texCoCBlur1->CreateUAV(uavDesc);

        texCoCBlur2 = eastl::make_unique<Texture2D>(texDesc);
        texCoCBlur2->CreateSRV(srvDesc);
        texCoCBlur2->CreateUAV(uavDesc);

        texDesc.Width = 1;
        texDesc.Height = 1;

        texFocus = eastl::make_unique<Texture2D>(texDesc);
        texFocus->CreateSRV(srvDesc);
        texFocus->CreateUAV(uavDesc);

        texPreFocus = eastl::make_unique<Texture2D>(texDesc);
        texPreFocus->CreateSRV(srvDesc);
        texPreFocus->CreateUAV(uavDesc);
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
        DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, colorSampler.put()));

        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, depthSampler.put()));
    }

    CompileComputeShaders();
}

void DoF::ClearShaderCache()
{
    const auto shaderPtrs = std::array{
        &UpdateFocusCS,
        &CalculateCoCCS,
        &CoCGaussian1CS,
        &CoCGaussian2CS,
        &BlurCS,
        &FarBlurCS,
        &NearBlurCS
    };

    for (auto shader : shaderPtrs)
        if ((*shader)) {
            (*shader)->Release();
            shader->detach();
        }

    CompileComputeShaders();
}

void DoF::CompileComputeShaders()
{
    struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

    std::vector<ShaderCompileInfo>
        shaderInfos = {
            { &UpdateFocusCS, "dof.cs.hlsl", {}, "CS_UpdateFocus" },
            { &CalculateCoCCS, "dof.cs.hlsl", {}, "CS_CalculateCoC" },
            { &CoCGaussian1CS, "dof.cs.hlsl", {}, "CS_CoCGaussian1" },
            { &CoCGaussian2CS, "dof.cs.hlsl", {}, "CS_CoCGaussian2" },
            { &BlurCS, "dof.cs.hlsl", {}, "CS_Blur" },
            { &FarBlurCS, "dof.cs.hlsl", {}, "CS_FarBlur" },
            { &NearBlurCS, "dof.cs.hlsl", {}, "CS_NearBlur" }
    };

    for (auto& info : shaderInfos) {
        auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\DoF") / info.filename;
        if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
            info.programPtr->attach(rawPtr);
    }
}

void DoF::Draw(TextureInfo& inout_tex)
{
    auto state = State::GetSingleton();
	auto context = state->context;
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

    float2 res = { (float)texOutput->desc.Width, (float)texOutput->desc.Height };

    state->BeginPerfEvent("Depth of Field");

    DoFCB dofData = {
        .TransitionSpeed = settings.TransitionSpeed,
        .FocusCoord = settings.FocusCoord,
        .ManualFocusPlane = settings.ManualFocusPlane,
        .FocalLength = settings.FocalLength,
        .FNumber = settings.FNumber,
        .BlurQuality = settings.BlurQuality,
        .NearFarDistanceCompensation = settings.NearFarDistanceCompensation,
        .BokehBusyFactor = settings.BokehBusyFactor,
        .HighlightBoost = settings.HighlightBoost,
        .Width = res.x,
        .Height = res.y,
        .AutoFocus = settings.AutoFocus
    };
    dofCB->Update(dofData);

    std::array<ID3D11ShaderResourceView*, 5> srvs = { inout_tex.srv, texPreFocus->srv.get(), depth.depthSRV, nullptr, nullptr };
    std::array<ID3D11UnorderedAccessView*, 3> uavs = { texOutput->uav.get(), texFocus->uav.get(), texCoC->uav.get() };
    std::array<ID3D11SamplerState*, 2> samplers = { colorSampler.get(), depthSampler.get() };
    auto cb = dofCB->CB();
    auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

    // Update Focus
    {
        srvs.at(0) = inout_tex.srv;
        srvs.at(1) = texPreFocus->srv.get();
        srvs.at(2) = depth.depthSRV;
        uavs.at(1) = texFocus->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

        context->CSSetShader(UpdateFocusCS.get(), nullptr, 0);
        context->Dispatch(1, 1, 1);
    }

    resetViews();
    context->CopyResource(texPreFocus->resource.get(), texFocus->resource.get());

    // Calculate CoC
    {
        srvs.at(0) = inout_tex.srv;
        srvs.at(1) = texPreFocus->srv.get();
        srvs.at(2) = depth.depthSRV;
        uavs.at(2) = texCoC->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

        context->CSSetShader(CalculateCoCCS.get(), nullptr, 0);
        context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);
    }

    resetViews();

    // CoC Gaussian Blur (coc uses srv3 and uav2)
    {
        srvs.at(3) = texCoC->srv.get();
        uavs.at(2) = texCoCBlur1->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

        context->CSSetShader(CoCGaussian1CS.get(), nullptr, 0);
        context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);

        resetViews();

        srvs.at(3) = texCoCBlur1->srv.get();
        uavs.at(2) = texCoCBlur2->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

        context->CSSetShader(CoCGaussian2CS.get(), nullptr, 0);
        context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);

        resetViews();
    }

    // Blur
    {
        srvs.at(0) = inout_tex.srv;
        srvs.at(3) = texCoC->srv.get();
        srvs.at(4) = texCoCBlur2->srv.get();
        uavs.at(0) = texPreBlurred->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

        context->CSSetShader(BlurCS.get(), nullptr, 0);
        context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);

        resetViews();

        srvs.at(0) = texPreBlurred->srv.get();
        srvs.at(3) = texCoC->srv.get();
        srvs.at(4) = texCoCBlur2->srv.get();
        uavs.at(0) = texFarBlurred->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

        context->CSSetShader(FarBlurCS.get(), nullptr, 0);
        context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);

        resetViews();

        srvs.at(0) = texFarBlurred->srv.get();
        srvs.at(3) = texCoC->srv.get();
        srvs.at(4) = texCoCBlur2->srv.get();
        uavs.at(0) = texOutput->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

        context->CSSetShader(NearBlurCS.get(), nullptr, 0);
        context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);

        resetViews();
    }

    samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

    inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
	state->EndPerfEvent();
}