#include "LensFlare.h"

#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    LensFlare::Settings,
    GhostStrength,
    HaloStrength,
    HaloRadius,
    HaloWidth,
    LensFlareCA,
    LFStrength,
    GLocalMask
)

void LensFlare::DrawSettings()
{
    ImGui::SliderFloat("Lens Flare Strength", &settings.LFStrength, 0.0f, 1.0f, "%.3f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Master intensity control for the entire lens flare effect");
    }

    ImGui::Checkbox("Non-intrusive Lens Flares", &settings.GLocalMask);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Only apply flaring when looking directly at light sources");
    }

    // Ghost Settings
    ImGui::Spacing();
    ImGui::Text("Ghost Settings");
    ImGui::Separator();
    
    ImGui::SliderFloat("Ghost Strength", &settings.GhostStrength, 0.0f, 1.0f, "%.3f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Intensity of ghost artifacts");
    }

    // Halo Settings
    ImGui::Spacing();
    ImGui::Text("Halo Settings");
    ImGui::Separator();
    
    ImGui::SliderFloat("Halo Strength", &settings.HaloStrength, 0.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("Halo Radius", &settings.HaloRadius, 0.0f, 0.8f, "%.3f");
    ImGui::SliderFloat("Halo Width", &settings.HaloWidth, 0.0f, 1.0f, "%.3f");

    // Chromatic Aberration
    ImGui::Spacing();
    ImGui::Text("Chromatic Aberration");
    ImGui::Separator();
    
    ImGui::SliderFloat("CA Amount", &settings.LensFlareCA, 0.0f, 2.0f, "%.3f");
}

void LensFlare::RestoreDefaultSettings()
{
    settings = {};
}

void LensFlare::LoadSettings(json& o_json)
{
    settings = o_json;
}

void LensFlare::SaveSettings(json& o_json)
{
    o_json = settings;
}

void LensFlare::SetupResources()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    auto device = State::GetSingleton()->device;

    logger::debug("Creating buffers...");
    {
        lensFlareCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<LensFlareCB>());
    }

    logger::debug("Creating 2D textures...");
    {
        auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

        D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        logger::debug("Creating output texture with format: {}", (uint32_t)texDesc.Format);
        
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

        if (!(texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
            logger::error("Texture format doesn't support UAV!");
        }
		texOutput = eastl::make_unique<Texture2D>(texDesc);
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateUAV(uavDesc);
    }

    if (!texOutput || !texOutput->resource || !texOutput->uav || !texOutput->srv) {
        logger::error("Failed to create output texture!");
        return;
    }
    logger::debug("Output texture size: {}x{}", texOutput->desc.Width, texOutput->desc.Height);

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
	}

    CompileComputeShaders();
}

void LensFlare::ClearShaderCache()
{
    const auto shaderPtrs = std::array{
        &lensFlareCS
    };

    for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void LensFlare::CompileComputeShaders()
{
    struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines = {};
		std::string entry = "main";
	};

    std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &lensFlareCS, "lensflare.cs.hlsl" },
		};

    for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\LensFlare") / info.filename;
        if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}

    if (!lensFlareCS) {
        logger::error("Failed to compile lens flare compute shader!");
        return;
    }
}

void LensFlare::Draw(TextureInfo& inout_tex)
{
	auto context = State::GetSingleton()->context;

    uint width = texOutput->desc.Width;
    uint height = texOutput->desc.Height;

    D3D11_BOX sourceRegion = {
        .left = 0,
        .top = 0,
        .front = 0,
        .right = width,
        .bottom = height,
        .back = 1
    };

    LensFlareCB data = {
        .settings = settings,
        .ScreenWidth = (float)width,
        .ScreenHeight = (float)height
    };
    lensFlareCB->Update(data);

    ID3D11ShaderResourceView* srv = inout_tex.srv;
	ID3D11UnorderedAccessView* uav = texOutput->uav.get();
    std::array<ID3D11SamplerState*, 1> samplers = { colorSampler.get() };
	auto cb = lensFlareCB->CB();

	context->CSSetConstantBuffers(1, 1, &cb);
    context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetShader(lensFlareCS.get(), nullptr, 0);

    uint dispatchX = (width + 7) >> 3;
    uint dispatchY = (height + 7) >> 3;
    context->Dispatch(dispatchX, dispatchY, 1);
    if (!inout_tex.srv || !inout_tex.tex) {
        logger::error("Invalid input texture!");
        return;
    }

    // Cleanup
    srv = nullptr;
	uav = nullptr;
	cb = nullptr;

    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

    // context->CopySubresourceRegion(
    //     texOutput->resource.get(), 0,
    //     0, 0, 0,
    //     inout_tex.resource, 0,
    //     &sourceRegion);

    inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
}

