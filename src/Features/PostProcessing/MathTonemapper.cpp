#include "MathTonemapper.h"

#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	MathTonemapper::Settings,
	Tonemapper,
	KeyValue,
	WhitePoint,
	Cutoff,
	Slope,
	Power,
	Offset,
	Saturation)

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

		"Minimal version of Troy Sobotka's AgX using a 6th order polynomial approximation."
		"Originally created by bwrensch, and improved by Troy Sobotka."sv,
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
		ImGui::SliderFloat("Key Value", &settings.KeyValue, 0.f, 5.f, "%.2f");
		if (settings.Tonemapper == 1)
			ImGui::SliderFloat("White Point", &settings.WhitePoint, 0.f, 10.f, "%.2f");
		if (settings.Tonemapper == 3)
			ImGui::SliderFloat("Cutoff", &settings.Cutoff, 0.f, .5f, "%.2f");
	}
}

void MathTonemapper::RestoreDefaultSettings()
{
	settings = {};
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
			.Params = { settings.Slope, settings.Power, settings.Offset, settings.Saturation }
		};
	else
		cbData = {
			.Params = { settings.KeyValue, settings.WhitePoint, settings.Cutoff, 0 }
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
