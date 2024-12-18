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
    HighlightBoost,
    Bokeh
)

void DoF::DrawSettings()
{
    ImGui::Checkbox("Auto Focus", &settings.AutoFocus);

    if (settings.AutoFocus) {
        ImGui::SliderFloat("Transition Speed", &settings.TransitionSpeed, 0.1f, 1.0f, "%.2f");

        ImGui::SliderFloat2("Focus Point", &settings.FocusCoord.x, 0.0f, 1.0f, "%.2f");
    }

    ImGui::SliderFloat("Manual Focus", &settings.ManualFocusPlane, 0.0f, 1.0f, "%.2f");

    ImGui::SliderFloat("Focal Length", &settings.FocalLength, 1.0f, 300.0f, "%.1f mm");

    ImGui::SliderFloat("F-Number", &settings.FNumber, 1.0f, 22.0f, "f/%.1f");

    ImGui::SliderFloat("Blur Quality", &settings.BlurQuality, 2.0f, 30.0f, "%.1f");

    ImGui::SliderFloat("Highlight Boost", &settings.HighlightBoost, 0.0f, 1.0f, "%.2f");

    ImGui::SliderFloat("Bokeh", &settings.Bokeh, 0.0f, 1.0f, "%.2f");

    if (ImGui::CollapsingHeader("Debug")) {
        static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

        BUFFER_VIEWER_NODE(texFocus, 64.0f)
        BUFFER_VIEWER_NODE(texPreFocus, 64.0f)

        BUFFER_VIEWER_NODE(texCoC, debugRescale)
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

        texDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;

        texCoC = eastl::make_unique<Texture2D>(texDesc);
        texCoC->CreateSRV(srvDesc);
        texCoC->CreateUAV(uavDesc);

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
        &CalculateCoCCS
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
            { &CalculateCoCCS, "dof.cs.hlsl", {}, "CS_CalculateCoC" }
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
        .HighlightBoost = settings.HighlightBoost,
        .Bokeh = settings.Bokeh,
        .Width = res.x,
        .Height = res.y,
        .AutoFocus = settings.AutoFocus
    };
    dofCB->Update(dofData);

    std::array<ID3D11ShaderResourceView*, 3> srvs = { inout_tex.srv, texPreFocus->srv.get(), depth.depthSRV };
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

    samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

	state->EndPerfEvent();
}