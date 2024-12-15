#include "DoF.h"
#include <DirectXTex.h>

#include "State.h"
#include "Util.h"
#include "Menu.h"

#define BUFFER_WIDTH ScreenSize.x
#define BUFFER_HEIGHT ScreenSize.y
#define TILE_SIZE 1

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    CinematicDOF::Settings,
    UseAutoFocus,
    MitigateUndersampling,
    AutoFocusPoint,
    AutoFocusTransitionSpeed,
    ManualFocusPlane,
    FocalLength,
    FNumber,
    FarPlaneMaxBlur,
    NearPlaneMaxBlur,
    BlurQuality,
    BokehBusyFactor,
    PostBlurSmoothing,
    NearFarDistanceCompensation,
    HighlightAnamorphicFactor,
    HighlightAnamorphicSpreadFactor,
    HighlightAnamorphicAlignmentFactor,
    HighlightBoost,
    HighlightGammaFactor,
    HighlightSharpeningFactor,
    HighlightShape,
    HighlightShapeRotationAngle,
    HighlightShapeGamma
    )

void CinematicDOF::DrawSettings()
{
    ImGui::SeparatorText("Focusing");
    ImGui::Checkbox("Use auto-focus", &settings.UseAutoFocus);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("If enabled it will make the shader focus on the point specified as 'Auto-focus point',\notherwise it will put the focus plane at the depth specified in 'Manual-focus plane'.");
    ImGui::SliderFloat2("Auto-focus point", (float*)&settings.AutoFocusPoint, 0.0f, 1.0f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The X and Y coordinates of the auto-focus point. 0,0 is the upper left corner,\nand 0.5, 0.5 is at the center of the screen. Only used if 'Use auto focus' is enabled.");
    ImGui::SliderFloat("Auto-focus transition speed", &settings.AutoFocusTransitionSpeed, 0.001f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The speed the shader will transition between different focus points when using auto-focus.\n0.001 means very slow, 1.0 means instantly. Only used if 'Use auto-focus' is enabled.");
    ImGui::SliderFloat("Manual-focus plane", &settings.ManualFocusPlane, 0.0f, 1.0f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The depth of focal plane related to the camera when 'Use auto-focus' is off.\n'1.0' means the ManualFocusPlaneMaxRange. 0 means at the camera.\nOnly used if 'Use auto-focus' is disabled.");
    ImGui::SliderFloat("Focal length (mm)", &settings.FocalLength, 10.0f, 300.0f, "%.1f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Focal length of the used lens. The longer the focal length, the narrower the\ndepth of field and thus the more is out of focus. For portraits, start with 120 or 150.");
    ImGui::SliderFloat("Aperture (f-number)", &settings.FNumber, 1.0f, 22.0f);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The f-number (also known as f-stop) to use. The higher the number, the wider\nthe depth of field, meaning the more is in-focus and thus the less is out of focus.\nFor portraits, start with 2.8.");

    ImGui::SeparatorText("Blur tweaking");
    ImGui::SliderFloat("Far plane max blur", &settings.FarPlaneMaxBlur, 0.0f, 8.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The maximum blur a pixel can have when it has its maximum CoC in the far plane. Use this as a tweak\nto adjust the max far plane blur defined by the lens parameters. Don't use this as your primarily\nblur factor, use the lens parameters Focal Length and Aperture for that instead.");
    ImGui::SliderFloat("Near plane max blur", &settings.NearPlaneMaxBlur, 0.0f, 4.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The maximum blur a pixel can have when it has its maximum CoC in the near Plane. Use this as a tweak to\nadjust the max near plane blur defined by the lens parameters.  Don't use this as your primarily blur factor,\nuse the lens parameters Focal Length and Aperture for that instead.");
    ImGui::SliderFloat("Overall blur quality", &settings.BlurQuality, 2.0f, 30.0f, "%.0f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The number of rings to use in the disc-blur algorithm. The more rings the better\nthe blur results, but also the slower it will get.");
    ImGui::SliderFloat("Bokeh busy factor", &settings.BokehBusyFactor, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The 'bokeh busy factor' for the blur: 0 means no busyness boost, 1.0 means extra busyness boost.");
    ImGui::SliderFloat("Post-blur smoothing factor", &settings.PostBlurSmoothing, 0.0f, 2.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The amount of post-blur smoothing blur to apply. 0.0 means no smoothing blur is applied.");
    ImGui::SliderFloat("Near-Far plane distance compenation", &settings.NearFarDistanceCompensation, 1.0f, 5.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The amount of compensation is applied for edges which are far away from the background.\nIncrease to a value > 1.0 to avoid hard edges in areas out of focus which\nare close to the in-focus area.");

    ImGui::SeparatorText("Highlight tweaking, anamorphism");
    ImGui::SliderFloat("Anamorphic factor", &settings.HighlightAnamorphicFactor, 0.01f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The anamorphic factor of the bokeh highlights. A value of 1.0 (default) gives perfect\ncircles, a factor of e.g. 0.1 gives thin ellipses");
    ImGui::SliderFloat("Anamorphic spread factor", &settings.HighlightAnamorphicSpreadFactor, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The spread factor for the anamorphic factor. 0.0 means it's relative to the distance\nto the center of the screen, 1.0 means the factor is applied everywhere evenly,\nno matter how far the pixel is to the center of the screen.");
    ImGui::SliderFloat("Anamorphic alignment factor", &settings.HighlightAnamorphicAlignmentFactor, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The alignment factor for the anamorphic deformation. 0.0 means you get evenly rotated\nellipses around the center of the screen, 1.0 means all bokeh highlights are\naligned vertically.");
    ImGui::SliderFloat("Highlight boost factor", &settings.HighlightBoost, 0.0f, 1.0f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Will boost/dim the highlights a small amount");
    ImGui::SliderFloat("Highlight gamma factor", &settings.HighlightGammaFactor, 0.001f, 5.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Controls the gamma factor to boost/dim highlights\n2.2, the default, gives natural colors and brightness");
    ImGui::SliderFloat("Highlight sharpening factor", &settings.HighlightSharpeningFactor, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Controls the sharpness of the bokeh highlight edges.");

    ImGui::SeparatorText("Advanced");
    ImGui::Checkbox("Mitigate undersampling", &settings.MitigateUndersampling);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("If you see bright pixels in the highlights,\ncheck this checkbox to smoothen the highlights.\nOnly needed with high blur factors and low blur quality.");

    ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
        static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);
        BUFFER_VIEWER_NODE(texOutput, debugRescale)
        BUFFER_VIEWER_NODE(texDepth, debugRescale)
        BUFFER_VIEWER_NODE(texCDCurrentFocus, debugRescale)
        BUFFER_VIEWER_NODE(texCDPreviousFocus, debugRescale)
        BUFFER_VIEWER_NODE(texCDCoC, debugRescale)
        BUFFER_VIEWER_NODE(texCDCoCTileTmp, debugRescale)
        BUFFER_VIEWER_NODE(texCDCoCTile, debugRescale)
        BUFFER_VIEWER_NODE(texCDCoCTileNeighbor, debugRescale)
        BUFFER_VIEWER_NODE(texCDCoCTmp1, debugRescale)
        BUFFER_VIEWER_NODE(texCDCoCBlurred, debugRescale)
        BUFFER_VIEWER_NODE(texCDBuffer1, debugRescale)
        BUFFER_VIEWER_NODE(texCDBuffer2, debugRescale)
        BUFFER_VIEWER_NODE(texCDBuffer3, debugRescale)
        BUFFER_VIEWER_NODE(texCDBuffer4, debugRescale)
        BUFFER_VIEWER_NODE(texCDBuffer5, debugRescale)
        BUFFER_VIEWER_NODE(texCDNoise, debugRescale)
        ImGui::TreePop();
    }
}

void CinematicDOF::RestoreDefaultSettings()
{
    settings = {};
}

void CinematicDOF::LoadSettings(json& o_json)
{
    settings = o_json;
}

void CinematicDOF::SaveSettings(json& o_json)
{
    o_json = settings;
}

void CinematicDOF::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = State::GetSingleton()->device;

    logger::debug("Creating buffers...");
	{
        dofCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DOFCB>());
    }

    logger::debug("Reading noise texture...");
    {
        DirectX::ScratchImage image;

        D3D11_TEXTURE2D_DESC texDesc = {
            .Width = 512,
            .Height = 512,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = texDesc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };
        std::filesystem::path path = { "data/texture/noise/monochrome_gaussnoise.png" };
        try {
            DX::ThrowIfFailed(LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image));
        } catch (std::runtime_error& e) {
            logger::error("Failed to load noise texture! Error: {}", e.what());
            texCDNoise = eastl::make_unique<Texture2D>(texDesc);
            texCDNoise->CreateSRV(srvDesc);
            return;
        }
        ID3D11Resource* pRsrc = nullptr;
        
        try {
            DX::ThrowIfFailed(CreateTexture(device, image.GetImages(), image.GetImageCount(), image.GetMetadata(), &pRsrc));
        } catch (std::runtime_error& e) {
            logger::error("Failed to create texture! Error: {}", e.what());
            // Create a dummy texture
            texCDNoise = eastl::make_unique<Texture2D>(texDesc);
            texCDNoise->CreateSRV(srvDesc);
            return;
        }

        texCDNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pRsrc));
        texCDNoise->CreateSRV(srvDesc);
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

        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

        texOutput = eastl::make_unique<Texture2D>(texDesc);
        texOutput->CreateSRV(srvDesc);
        texOutput->CreateUAV(uavDesc);

        float2 ScreenSize = float2(float(texOutput->desc.Width), float(texOutput->desc.Height));

        D3D11_TEXTURE2D_DESC texCDCurrentFocusDesc = {
            .Width = 1,
            .Height = 1,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R16_FLOAT,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvCDCurrentFocusDesc = {
            .Format = texCDCurrentFocusDesc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavCDCurrentFocusDesc = {
            .Format = texCDCurrentFocusDesc.Format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };

        texCDCurrentFocus = eastl::make_unique<Texture2D>(texCDCurrentFocusDesc);
        texCDCurrentFocus->CreateSRV(srvCDCurrentFocusDesc);
        texCDCurrentFocus->CreateUAV(uavCDCurrentFocusDesc);

        texCDPreviousFocus = eastl::make_unique<Texture2D>(texCDCurrentFocusDesc);
        texCDPreviousFocus->CreateSRV(srvCDCurrentFocusDesc);
        texCDPreviousFocus->CreateUAV(uavCDCurrentFocusDesc);

        D3D11_TEXTURE2D_DESC texCDCoCDesc = {
            .Width = uint(BUFFER_WIDTH),
            .Height = uint(BUFFER_HEIGHT),
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R16_FLOAT,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvCDCoCDesc = {
            .Format = texCDCoCDesc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavCDCoCDesc = {
            .Format = texCDCoCDesc.Format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };

        texCDCoC = eastl::make_unique<Texture2D>(texCDCoCDesc);
        texCDCoC->CreateSRV(srvCDCoCDesc);
        texCDCoC->CreateUAV(uavCDCoCDesc);

        D3D11_TEXTURE2D_DESC texCDCoCTileTmpDesc = {
            .Width = uint(BUFFER_WIDTH) / TILE_SIZE,
            .Height = uint(BUFFER_HEIGHT) / TILE_SIZE,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R16_FLOAT,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvCDCoCTileTmpDesc = {
            .Format = texCDCoCTileTmpDesc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavCDCoCTileTmpDesc = {
            .Format = texCDCoCTileTmpDesc.Format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };

        texCDCoCTileTmp = eastl::make_unique<Texture2D>(texCDCoCTileTmpDesc);
        texCDCoCTileTmp->CreateSRV(srvCDCoCTileTmpDesc);
        texCDCoCTileTmp->CreateUAV(uavCDCoCTileTmpDesc);

        texCDCoCTile = eastl::make_unique<Texture2D>(texCDCoCTileTmpDesc);
        texCDCoCTile->CreateSRV(srvCDCoCTileTmpDesc);
        texCDCoCTile->CreateUAV(uavCDCoCTileTmpDesc);

        texCDCoCTileNeighbor = eastl::make_unique<Texture2D>(texCDCoCTileTmpDesc);
        texCDCoCTileNeighbor->CreateSRV(srvCDCoCTileTmpDesc);
        texCDCoCTileNeighbor->CreateUAV(uavCDCoCTileTmpDesc);

        D3D11_TEXTURE2D_DESC texCDCoCTmp1Desc = {
            .Width = uint(BUFFER_WIDTH) / 2,
            .Height = uint(BUFFER_HEIGHT) / 2,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R16_FLOAT,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvCDCoCTmp1Desc = {
            .Format = texCDCoCTmp1Desc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavCDCoCTmp1Desc = {
            .Format = texCDCoCTmp1Desc.Format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };

        texCDCoCTmp1 = eastl::make_unique<Texture2D>(texCDCoCTmp1Desc);
        texCDCoCTmp1->CreateSRV(srvCDCoCTmp1Desc);
        texCDCoCTmp1->CreateUAV(uavCDCoCTmp1Desc);

        D3D11_TEXTURE2D_DESC texCDCoCBlurredDesc = {
            .Width = uint(BUFFER_WIDTH) / 2,
            .Height = uint(BUFFER_HEIGHT) / 2,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R16G16_FLOAT,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvCDCoCBlurredDesc = {
            .Format = texCDCoCBlurredDesc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavCDCoCBlurredDesc = {
            .Format = texCDCoCBlurredDesc.Format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };

        texCDCoCBlurred = eastl::make_unique<Texture2D>(texCDCoCBlurredDesc);
        texCDCoCBlurred->CreateSRV(srvCDCoCBlurredDesc);
        texCDCoCBlurred->CreateUAV(uavCDCoCBlurredDesc);

        D3D11_TEXTURE2D_DESC texCDBuffer1Desc = {
            .Width = uint(BUFFER_WIDTH) / 2,
            .Height = uint(BUFFER_HEIGHT) / 2,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvCDBuffer1Desc = {
            .Format = texCDBuffer1Desc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavCDBuffer1Desc = {
            .Format = texCDBuffer1Desc.Format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };

        texCDBuffer1 = eastl::make_unique<Texture2D>(texCDBuffer1Desc);
        texCDBuffer1->CreateSRV(srvCDBuffer1Desc);
        texCDBuffer1->CreateUAV(uavCDBuffer1Desc);

        texCDBuffer2 = eastl::make_unique<Texture2D>(texCDBuffer1Desc);
        texCDBuffer2->CreateSRV(srvCDBuffer1Desc);
        texCDBuffer2->CreateUAV(uavCDBuffer1Desc);

        texCDBuffer3 = eastl::make_unique<Texture2D>(texCDBuffer1Desc);
        texCDBuffer3->CreateSRV(srvCDBuffer1Desc);
        texCDBuffer3->CreateUAV(uavCDBuffer1Desc);

        D3D11_TEXTURE2D_DESC texCDBuffer4Desc = {
            .Width = uint(BUFFER_WIDTH),
            .Height = uint(BUFFER_HEIGHT),
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
            .CPUAccessFlags = 0,
            .MiscFlags = 0
        };

        D3D11_SHADER_RESOURCE_VIEW_DESC srvCDBuffer4Desc = {
            .Format = texCDBuffer4Desc.Format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavCDBuffer4Desc = {
            .Format = texCDBuffer4Desc.Format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };

        texCDBuffer4 = eastl::make_unique<Texture2D>(texCDBuffer4Desc);
        texCDBuffer4->CreateSRV(srvCDBuffer4Desc);
        texCDBuffer4->CreateUAV(uavCDBuffer4Desc);

        texCDBuffer5 = eastl::make_unique<Texture2D>(texCDBuffer4Desc);
        texCDBuffer5->CreateSRV(srvCDBuffer4Desc);
        texCDBuffer5->CreateUAV(uavCDBuffer4Desc);
    }

    logger::debug("Creating samplers...");
    {
        D3D11_SAMPLER_DESC colorSamplerDesc = {
            .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
            .AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
            .AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
            .MaxAnisotropy = 1,
            .MinLOD = 0,
            .MaxLOD = D3D11_FLOAT32_MAX
        };

        DX::ThrowIfFailed(device->CreateSamplerState(&colorSamplerDesc, colorSampler.put()));

        D3D11_SAMPLER_DESC bufferSamplerDesc = {
            .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D11_TEXTURE_ADDRESS_MIRROR,
            .AddressV = D3D11_TEXTURE_ADDRESS_MIRROR,
            .AddressW = D3D11_TEXTURE_ADDRESS_MIRROR,
            .MaxAnisotropy = 1,
            .MinLOD = 0,
            .MaxLOD = D3D11_FLOAT32_MAX
        };

        DX::ThrowIfFailed(device->CreateSamplerState(&bufferSamplerDesc, bufferSampler.put()));

        D3D11_SAMPLER_DESC cocSamplerDesc = {
            .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D11_TEXTURE_ADDRESS_MIRROR,
            .AddressV = D3D11_TEXTURE_ADDRESS_MIRROR,
            .AddressW = D3D11_TEXTURE_ADDRESS_MIRROR,
            .MaxAnisotropy = 1,
            .MinLOD = 0,
            .MaxLOD = D3D11_FLOAT32_MAX
        };

        DX::ThrowIfFailed(device->CreateSamplerState(&cocSamplerDesc, cocSampler.put()));

        D3D11_SAMPLER_DESC noiseSamplerDesc = {
            .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
            .AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
            .AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
            .MaxAnisotropy = 1,
            .MinLOD = 0,
            .MaxLOD = D3D11_FLOAT32_MAX
        };

        DX::ThrowIfFailed(device->CreateSamplerState(&noiseSamplerDesc, noiseSampler.put()));
    }

    CompileComputeShaders();
}

void CinematicDOF::ClearShaderCache()
{
    auto const shaderPtrs = std::array{
        &cdofCS,
    };

    for (auto shader : shaderPtrs)
        if ((*shader)) {
            (*shader)->Release();
            shader->detach();
        }

    CompileComputeShaders();
}

void CinematicDOF::CompileComputeShaders()
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
            { &cdofCS, "CinematicDOF.hlsl", {}, "CS_CinematicDOF" },
        };
    
    for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\DoF") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

void CinematicDOF::Draw(TextureInfo& inout_tex)
{
    auto state = State::GetSingleton();
    auto context = state->context;
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    logger::debug("Drawing Cinematic DOF...");
    
    float2 ScreenSize = state->screenSize;
    state->BeginPerfEvent("Cinematic DOF");

    // update cb
    DOFCB cbData = {
        .UseAutoFocus = settings.UseAutoFocus,
        .MitigateUndersampling = settings.MitigateUndersampling,
        .AutoFocusPoint = settings.AutoFocusPoint,
        .AutoFocusTransitionSpeed = settings.AutoFocusTransitionSpeed,
        .ManualFocusPlane = settings.ManualFocusPlane,
        .FocalLength = settings.FocalLength,
        .FNumber = settings.FNumber,
        .FarPlaneMaxBlur = settings.FarPlaneMaxBlur,
        .NearPlaneMaxBlur = settings.NearPlaneMaxBlur,
        .BlurQuality = settings.BlurQuality,
        .BokehBusyFactor = settings.BokehBusyFactor,
        .PostBlurSmoothing = settings.PostBlurSmoothing,
        .NearFarDistanceCompensation = settings.NearFarDistanceCompensation,
        .HighlightAnamorphicFactor = settings.HighlightAnamorphicFactor,
        .HighlightAnamorphicSpreadFactor = settings.HighlightAnamorphicSpreadFactor,
        .HighlightAnamorphicAlignmentFactor = settings.HighlightAnamorphicAlignmentFactor,
        .HighlightBoost = settings.HighlightBoost,
        .HighlightGammaFactor = settings.HighlightGammaFactor,
        .HighlightSharpeningFactor = settings.HighlightSharpeningFactor,
        .HighlightShape = 0,
        .HighlightShapeRotationAngle = settings.HighlightShapeRotationAngle,
        .HighlightShapeGamma = settings.HighlightShapeGamma,
        .ScreenWidth = BUFFER_WIDTH,
        .ScreenHeight = BUFFER_HEIGHT
    };
    dofCB->Update(cbData);

    std::array<ID3D11ShaderResourceView*, 3> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 14> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 4> samplers = { colorSampler.get(), bufferSampler.get(), cocSampler.get(), noiseSampler.get() };

    auto cb = dofCB->CB();

    srvs[0] = inout_tex.srv;
    srvs[1] = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
    srvs[2] = texCDNoise->srv.get();
    
    uavs[0] = texOutput->uav.get();
    uavs[1] = texCDCurrentFocus->uav.get();
    uavs[2] = texCDPreviousFocus->uav.get();
    uavs[3] = texCDCoC->uav.get();
    uavs[4] = texCDCoCTileTmp->uav.get();
    uavs[5] = texCDCoCTile->uav.get();
    uavs[6] = texCDCoCTileNeighbor->uav.get();
    uavs[7] = texCDCoCTmp1->uav.get();
    uavs[8] = texCDCoCBlurred->uav.get();
    uavs[9] = texCDBuffer1->uav.get();
    uavs[10] = texCDBuffer2->uav.get();
    uavs[11] = texCDBuffer3->uav.get();
    uavs[12] = texCDBuffer4->uav.get();
    uavs[13] = texCDBuffer5->uav.get();

    auto resetViews = [&]() {
        srvs.fill(nullptr);
        uavs.fill(nullptr);
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    // The multiple passes from original code
	// {
	// 	pass DetermineCurrentFocus { VertexShader = PostProcessVS; PixelShader = PS_DetermineCurrentFocus; RenderTarget = texCDCurrentFocus; }
	// 	pass CopyCurrentFocus { VertexShader = PostProcessVS; PixelShader = PS_CopyCurrentFocus; RenderTarget = texCDPreviousFocus; }
	// 	pass CalculateCoC { VertexShader = VS_Focus; PixelShader = PS_CalculateCoCValues; RenderTarget = texCDCoC; }
	// 	pass CoCTile1 { VertexShader = PostProcessVS; PixelShader = PS_CoCTile1; RenderTarget = texCDCoCTileTmp; }
	// 	pass CoCTile2 { VertexShader = PostProcessVS; PixelShader = PS_CoCTile2; RenderTarget = texCDCoCTile; }
	// 	pass CoCTileNeighbor { VertexShader = PostProcessVS; PixelShader = PS_CoCTileNeighbor; RenderTarget = texCDCoCTileNeighbor; }
	// 	pass CoCBlur1 { VertexShader = PostProcessVS; PixelShader = PS_CoCGaussian1; RenderTarget = texCDCoCTmp1; }
	// 	pass CoCBlur2 { VertexShader = PostProcessVS; PixelShader = PS_CoCGaussian2; RenderTarget = texCDCoCBlurred; }
	// 	pass PreBlur { VertexShader = VS_DiscBlur; PixelShader = PS_PreBlur; RenderTarget = texCDBuffer1; }
	// 	pass BokehBlur { VertexShader = VS_DiscBlur; PixelShader = PS_BokehBlur; RenderTarget = texCDBuffer2; }
	// 	pass NearBokehBlur { VertexShader = VS_DiscBlur; PixelShader = PS_NearBokehBlur; RenderTarget = texCDBuffer1; }
	// 	pass TentFilter { VertexShader = PostProcessVS; PixelShader = PS_TentFilter; RenderTarget = texCDBuffer3; }
	// 	pass Combiner { VertexShader = PostProcessVS; PixelShader = PS_Combiner; RenderTarget = texCDBuffer4; }
	// 	pass PostSmoothing1 { VertexShader = PostProcessVS; PixelShader = PS_PostSmoothing1; RenderTarget = texCDBuffer5; }
	// 	pass PostSmoothing2AndFocusing { VertexShader = VS_Focus; PixelShader = PS_PostSmoothing2AndFocusing;}
	// }

    context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

    uint32_t dispatchX = ((uint)BUFFER_WIDTH + 7) >> 3;
    uint32_t dispatchY = ((uint)BUFFER_HEIGHT + 7) >> 3;
    if (!(dispatchX && dispatchY))
        return;
    context->CSSetShader(cdofCS.get(), nullptr, 0);
    context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
    context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    context->Dispatch(dispatchX, dispatchY, 1);

    samplers.fill(nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    resetViews();
    context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
    cb = nullptr;
    context->CSSetConstantBuffers(1, 1, &cb);
    inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
    renderer = nullptr;

    state->EndPerfEvent();
}
