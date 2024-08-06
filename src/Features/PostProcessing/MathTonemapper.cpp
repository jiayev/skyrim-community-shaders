#include "MathTonemapper.h"

#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	MathTonemapper::Settings,
	Tonemapper,
	Exposure,
	WhitePoint,
	Cutoff,
	Slope,
	Power,
	Offset,
	Saturation,
	ContrastLottes,
	Shoulder,
	HdrMax,
	MidIn,
	MidOut,
	BlackPoint,
	CrossoverPoint,
	WhitePointDay,
	ShoulderStrength,
	ToeStrength,
	MaxBrightness,
	ContrastUchimura,
	LinearStart,
	LinearLen,
	BlackTightnessShape,
	BlackTightnessOffset)

void MathTonemapper::DrawSettings()
{
	constexpr auto tonemappers = std::array{
		"Reinhard"sv,
		"Reinhard Extended"sv,
		"Hejl Burgess-Dawson Filmic"sv,
		"Aldridge Filmic"sv,
		"ACES (Hill)"sv,
		"ACES (Narkowicz)"sv,
		"ACES (Guy)"sv,
		"AgX Minimal"sv,
		"Lottes Filmic/AMD Curve"sv,
		"Day Filmic/Insomniac Curve"sv,
		"Uchimura/Grand Turismo Curve"sv,
	};
	constexpr auto descs = std::array{
		"Mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002."sv,

		"Extended mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002. "
		"An additional user parameter specifies the smallest luminance that is mapped to 1, which allows high luminances to burn out."sv,

		"Analytical approximation of a Kodak film curve by Jim Hejl and Richard Burgess-Dawson. "
		"See the \"Filmic Tonemapping for Real-time Rendering\" SIGGRAPH 2010 course by Haarm-Pieter Duiker."sv,

		"Variation of the Hejl and Burgess-Dawson filmic curve done by Graham Aldridge. "
		"See his blog post about \"Approximating Film with Tonemapping\"."sv,

		"ACES curve fit by Stephen Hill."sv,

		"ACES curve fit by Krzysztof Narkowicz. See his blog post \"ACES Filmic Tone Mapping Curve\"."sv,

		"Curve from Unreal 3 adapted by to close to the ACES curve by Romain Guy."sv,

		"Minimal version of Troy Sobotka's AgX using a 6th order polynomial approximation. "
		"Originally created by bwrensch, and improved by Troy Sobotka."sv,

		"Filmic curve by Timothy Lottes, described in his GDC talk \"Advanced Techniques and Optimization of HDR Color Pipelines\". "
		"Also known as the \"AMD curve\"."sv,

		"Filmic curve by Mike Day, described in his document \"An efficient and user-friendly tone mapping operator\". "
		"Also known as the \"Insomniac curve\"."sv,

		"Filmic curve by Hajime Uchimura, described in his CEDEC talk \"HDR Theory and Practice\". Characterised by its middle linear section. "
		"Also known as the \"Gran Turismo curve\"."sv,
	};

	if (ImGui::BeginCombo("Tonemapping Operator", tonemappers[settings.Tonemapper].data())) {
		for (int i = 0; i < tonemappers.size(); ++i) {
			if (ImGui::Selectable(tonemappers[i].data(), i == settings.Tonemapper)) {
				settings.Tonemapper = i;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(descs[i].data());
		}
		ImGui::EndCombo();
	}
	ImGui::Spacing();
	ImGui::TextWrapped(descs[settings.Tonemapper].data());
	ImGui::Spacing();

	if (settings.Tonemapper == 7) {
		ImGui::SliderFloat("Slope", &settings.Slope, 0.f, 2.f, "%.2f");
		ImGui::SliderFloat("Power", &settings.Power, 0.f, 2.f, "%.2f");
		ImGui::SliderFloat("Offset", &settings.Offset, -1.f, 1.f, "%.2f");
		ImGui::SliderFloat("Saturation", &settings.Saturation, 0.f, 2.f, "%.2f");
	} else {
		ImGui::SliderFloat("Exposure", &settings.Exposure, -5.f, 5.f, "%+.2f EV");
		switch (settings.Tonemapper) {
		case 1:
			ImGui::SliderFloat("White Point", &settings.WhitePoint, 0.f, 10.f, "%.2f");
			break;
		case 3:
			ImGui::SliderFloat("Cutoff", &settings.Cutoff, 0.f, .5f, "%.2f");
			break;
		case 8:
			ImGui::SliderFloat("Contrast", &settings.ContrastLottes, 1.f, 2.f, "%.2f");
			ImGui::SliderFloat("Shoulder", &settings.Shoulder, 0.01f, 2.f, "%.2f");
			ImGui::SliderFloat("Maximum HDR Value", &settings.HdrMax, 1.f, 10.f, "%.2f");
			ImGui::SliderFloat("Input Mid-Level", &settings.MidIn, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Output Mid-Level", &settings.MidOut, 0.f, 1.f, "%.2f");
			break;
		case 9:
			ImGui::SliderFloat("Black Point", &settings.BlackPoint, 0.f, 5.f, "%.2f");
			ImGui::SliderFloat("White Point", &settings.WhitePointDay, 0.f, 5.f, "%.2f");

			ImGui::SliderFloat("Cross-over Point", &settings.CrossoverPoint, 0.f, 5.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Point where the toe and shoulder are pieced together into a single curve.");
			ImGui::SliderFloat("Shoulder Strength", &settings.ShoulderStrength, 0.f, 1.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the shoulder.");
			ImGui::SliderFloat("Toe Strength", &settings.ToeStrength, 0.f, 1.f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the toe.");
			break;
		case 10:
			ImGui::SliderFloat("Contrast", &settings.ContrastUchimura, 0.f, 5.f, "%.2f");
			ImGui::SliderFloat("Max Brightness", &settings.MaxBrightness, 0.01f, 2.f, "%.2f");
			ImGui::SliderFloat("Linear Section Start", &settings.LinearStart, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Linear Section Length", &settings.LinearLen, .01f, .99f, "%.2f");
			ImGui::SliderFloat("Black Tightness Shape", &settings.BlackTightnessShape, 1.f, 3.f, "%.2f");
			ImGui::SliderFloat("Black Tightness Offset", &settings.BlackTightnessOffset, 0.f, 1.f, "%.2f");
			break;
		default:
			break;
		}
	}
}

void MathTonemapper::RestoreDefaultSettings()
{
	settings = {};
	recompileFlag = true;
}

void MathTonemapper::LoadSettings(json& o_json)
{
	settings = o_json;
}

void MathTonemapper::SaveSettings(json& o_json)
{
	o_json = settings;
}

void MathTonemapper::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	logger::debug("Creating buffers...");
	{
		tonemapCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TonemapCB>());
	}

	logger::debug("Creating 2D textures...");
	{
		// texAdapt for adaptation
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

		texTonemap = std::make_unique<Texture2D>(texDesc);
		texTonemap->CreateSRV(srvDesc);
		texTonemap->CreateUAV(uavDesc);
	}

	CompileComputeShaders();
}

void MathTonemapper::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&tonemapCS
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void MathTonemapper::CompileComputeShaders()
{
	constexpr auto tonemappers = std::array{
		"Reinhard"sv,
		"ReinhardExt"sv,
		"HejlBurgessDawsonFilmic"sv,
		"AldridgeFilmic"sv,
		"AcesHill"sv,
		"AcesNarkowicz"sv,
		"AcesGuy"sv,
		"AgxMinimal"sv,
		"LottesFilmic"sv,
		"DayFilmic"sv,
		"UchimuraFilmic"sv,
	};

	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &tonemapCS, "tonemap.cs.hlsl", { { "TONEMAP_OPERATOR", tonemappers[settings.Tonemapper].data() } } },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\MathTonemapper") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

void MathTonemapper::Draw(TextureInfo& inout_tex)
{
	auto context = State::GetSingleton()->context;

	if (recompileFlag)
		ClearShaderCache();

	TonemapCB cbData;
	if (settings.Tonemapper == 7)
		cbData = {
			.Params0 = { settings.Slope, settings.Power, settings.Offset, settings.Saturation }
		};
	else if (settings.Tonemapper == 8)
		cbData = {
			.Params0 = { exp2(settings.Exposure), settings.ContrastLottes, settings.Shoulder, settings.HdrMax },
			.Params1 = { settings.MidIn, settings.MidOut, 0, 0 }
		};
	else if (settings.Tonemapper == 9)
		cbData = {
			.Params0 = { exp2(settings.Exposure), settings.BlackPoint, settings.WhitePointDay, settings.CrossoverPoint },
			.Params1 = { settings.ShoulderStrength, settings.ToeStrength, 0, 0 }
		};
	else if (settings.Tonemapper == 10)
		cbData = {
			.Params0 = { exp2(settings.Exposure), settings.MaxBrightness, settings.ContrastUchimura, settings.LinearStart },
			.Params1 = { settings.LinearLen, settings.BlackTightnessShape, settings.BlackTightnessOffset, 0 }
		};
	else
		cbData = {
			.Params0 = { exp2(settings.Exposure), settings.WhitePoint, settings.Cutoff, 0 }
		};
	tonemapCB->Update(cbData);

	ID3D11ShaderResourceView* srv = inout_tex.srv;
	ID3D11UnorderedAccessView* uav = texTonemap->uav.get();
	ID3D11Buffer* cb = tonemapCB->CB();

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetShader(tonemapCS.get(), nullptr, 0);

	context->Dispatch(((texTonemap->desc.Width - 1) >> 5) + 1, ((texTonemap->desc.Height - 1) >> 5) + 1, 1);

	// clean up
	srv = nullptr;
	uav = nullptr;
	cb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);

	inout_tex = { texTonemap->resource.get(), texTonemap->srv.get() };
}
