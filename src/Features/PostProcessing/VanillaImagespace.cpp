#include "VanillaImagespace.h"

#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    VanillaImagespace::Settings,
    blendFactor
)

void VanillaImagespace::DrawSettings()
{
    ImGui::SliderFloat("Blend Factor", &settings.blendFactor, 0.0f, 1.0f, "%.3f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Blend factor for the vanilla imagespace effect");
    }
}

void VanillaImagespace::RestoreDefaultSettings()
{
    settings = {};
}

void VanillaImagespace::LoadSettings(json& o_json)
{
    settings = o_json;
}

void VanillaImagespace::SaveSettings(json& o_json)
{
    o_json = settings;
}

void VanillaImagespace::SetupResources()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    auto device = State::GetSingleton()->device;

    logger::debug("Creating buffers...");
    {
        vanillaImagespaceCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VanillaImagespaceCB>());

        perGeometryBuffersArray = REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524765, 411381));
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
    }

    logger::debug("Creating compute shaders...");
    {
        CompileComputeShaders();
    }
}

void VanillaImagespace::ClearShaderCache()
{
    const auto shaderPtrs = std::array{
        &vanillaImagespaceCS
    };

    for (auto shader : shaderPtrs)
        if ((*shader)) {
            (*shader)->Release();
            shader->detach();
        }

    CompileComputeShaders();
}

void VanillaImagespace::CompileComputeShaders()
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
            { &vanillaImagespaceCS, "vanillais.hlsl" },
        };

    for (auto& info : shaderInfos) {
        auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\VanillaImagespace") / info.filename;
        if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
            info.programPtr->attach(rawPtr);
    }

    if (!vanillaImagespaceCS) {
        logger::error("Failed to compile vanilla imagespace compute shader!");
        return;
    }
}

void VanillaImagespace::Draw(TextureInfo& inout_tex)
{
    auto context = State::GetSingleton()->context;
    float2 res = { (float)texOutput->desc.Width, (float)texOutput->desc.Height };
	res = Util::ConvertToDynamic(res);

    VanillaImagespaceCB data = {
        .blendFactor = settings.blendFactor,
        .res = res
    };
    vanillaImagespaceCB->Update(data);

    ID3D11ShaderResourceView* srv = inout_tex.srv;
    ID3D11UnorderedAccessView* uav = texOutput->uav.get();
    std::array<ID3D11Buffer*,2> cb = { vanillaImagespaceCB->CB(), perGeometryBuffersArray.get()[1] };

    context->CSSetConstantBuffers(1, 2, cb.data());
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

    context->CSSetShader(vanillaImagespaceCS.get(), nullptr, 0);
    context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);

    srv = nullptr;
	uav = nullptr;
	cb.fill(nullptr);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetConstantBuffers(1, 2, cb.data());
	context->CSSetShader(nullptr, nullptr, 0);

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
}