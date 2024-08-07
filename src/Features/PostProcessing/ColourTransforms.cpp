#include "ColourTransforms.h"

#include "State.h"
#include "Util.h"

#include "ColourSpace.h"

struct SavedSettings
{
	std::string TransformType = "nothingburger";
	float4 Params0;
	float4 Params1;
	float4 Params2;
	float4 Params3;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SavedSettings,
	TransformType,
	Params0,
	Params1,
	Params2,
	Params3)

template <int num = 3>
bool shiftSlider(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
	static_assert(num > 1 && num < 5);

	if (ImGui::GetIO().KeyShift) {
		auto changed = ImGui::SliderFloat(label, v, v_min, v_max, format, flags);
		if (changed)
			for (int i = 1; i < num; i++)
				v[i] = v[0];
		return changed;
	} else {
		if constexpr (num == 2)
			return ImGui::SliderFloat2(label, v, v_min, v_max, format, flags);
		else if constexpr (num == 3)
			return ImGui::SliderFloat3(label, v, v_min, v_max, format, flags);
		else if constexpr (num == 4)
			return ImGui::SliderFloat4(label, v, v_min, v_max, format, flags);
	}
}

template <int num = 1>
bool exposureSlider(float* val)
{
	float tempVal[num];
	for (int i = 0; i < 3; i++)
		tempVal[i] = log2(val[i]);

	bool retval;
	if constexpr (num == 1)
		retval = ImGui::SliderFloat("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 2)
		retval = shiftSlider<2>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 3)
		retval = shiftSlider<3>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");
	else if constexpr (num == 4)
		retval = shiftSlider<4>("Exposure", tempVal, -4.f, 4.f, "%+.2f EV");

	for (int i = 0; i < 3; i++)
		val[i] = exp2(tempVal[i]);

	return retval;
}

struct TransformInfo
{
	std::string_view name;
	std::string_view func_name;
	std::string_view desc;

	using CTP = ColourTransforms::TonemapCB;
	std::function<void(CTP&)> draw_settings_func;
	CTP default_settings;

	CTP cached_settings;

	static auto& GetTransforms()
	{
		constexpr auto shiftHint = []() { ImGui::TextWrapped("Press Shift to control all channels at the same time."); };

		// TODO: this might be read from files.
		static std::vector<TransformInfo> transforms = {
			{ "_"sv, "Basic Transforms"sv,
				"Primary operators with basic functions."sv,
				[](CTP&) {},
				{} },

			{ "Clamp"sv, "Clamp"sv,
				"Clamping inputs between min and max values."sv,
				[](CTP& params) {
					shiftSlider("Min", &params.Params0.x, 0.f, 4.f, "%.2f");
					shiftSlider("Max", &params.Params1.x, 0.f, 4.f, "%.2f");
					shiftHint();
				},
				{ { 0.f, 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f, 0.f } } },

			{ "Linear -> Log"sv, "LogSpace"sv,
				"Convert linear values to log space."sv,
				[](CTP&) {},
				{} },

			{ "Log -> Linear"sv, "exp2"sv,
				"Convert log values to linear space."sv,
				[](CTP&) {},
				{} },

			{ "Gamma"sv, "Gamma"sv,
				"Apply gamma curve. Negative values will be mirrored."sv,
				[](CTP& params) {
					shiftSlider("Gamma", &params.Params0.x, 0.f, 4.f, "%.2f");
					shiftSlider("Black Pivot", &params.Params1.x, 0.f, 1.f, "%.2f");
					shiftSlider("White Pivot", &params.Params2.x, 0.f, 5.f, "%.2f");
					shiftHint();
				},
				{ { 1.f, 1.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f, 1.f } } },

			{ "Exposure/Constrast"sv, "ExposureContrast"sv,
				"Basic exposure and contrast adjustment in linear space. "sv,
				[](CTP& params) {
					exposureSlider<3>(&params.Params0.x);
					shiftSlider("Contrast", &params.Params1.x, 0.f, 3.f, "%.2f");
					shiftSlider("Pivot", &params.Params2.x, 0.f, 4.f, "%.2f");
					shiftHint();
				},
				{ { 1.f, 1.f, 1.f, 0.f }, { 1.f, 1.f, 1.f, 0.f }, { .5f, .5f, .5f, .5f } } },

			{ "ASC CDL"sv, "ASC_CDL"sv,
				"ASC Color Decision List.\n"
				"out = clamp( (in * slope) + offset ) ^ power"sv,
				[](CTP& params) {
					shiftSlider("Slope", &params.Params0.x, 0.f, 2.f, "%.2f");
					shiftSlider("Power", &params.Params1.x, 0.f, 2.f, "%.2f");
					shiftSlider("Offset", &params.Params2.x, -1.f, 1.f, "%.2f");
					shiftHint();
				},
				{ { 1.f, 1.f, 1.f, 0.f }, { 1.f, 1.f, 1.f, 0.f }, { 0.f, 0.f, 0.f, 0.f } } },

			{ "Lift Gamma Gain"sv, "LiftGammaGain"sv,
				"Basic lift gamma gain control (Luma+RGB) like Davinci Resolve, affecting dark tones/midtones/highlights respectively. "
				"Expects inputs between [0, 1]."sv,
				[](CTP& params) {
					shiftSlider<4>("Lift", &params.Params0.x, -1.f, 1.f, "%.2f");
					shiftSlider<4>("Gamma", &params.Params1.x, -2.f, 2.f, "%.2f");
					shiftSlider<4>("Gain", &params.Params2.x, 0.f, 2.f, "%.2f");
					shiftHint();
				},
				{ { 0.f, 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 0.f }, { 1.f, 1.f, 1.f, 1.f } } },

			{ "Saturation/Hue"sv, "SaturationHue"sv,
				"Adjust saturation and hue shift. Expects linear RGB inputs."sv,
				[](CTP& params) {
					ImGui::SliderFloat("Saturation", &params.Params0.x, 0.f, 4.f, "%.2f");
					ImGui::SliderFloat("Hue Shift", &params.Params0.y, -1.f, 1.f, "%.2f");
				},
				{ { 1.f, 0.f, 0.f, 0.f } } },

			{ "_"sv, "Colour Space Conversions"sv,
				"Converting to other colour spaces to exploit their characteristic."sv,
				[](CTP&) {},
				{} },

			{ "RGB Spaces"sv, "MatMul"sv,
				"Convert between linear RGB spaces with different sets of gamuts, or any colour spaces using matrix multiplication."sv,
				[](CTP& params) {
					auto& spaces = getAvailableColourSpaces();

					bool manualInput = params.Params2.w > 0;
					if (ImGui::Checkbox("Manual Input", &manualInput))
						params.Params2.w = manualInput * 2.f - 1.f;

					if (manualInput) {
						ImGui::InputFloat3("Row 1", &params.Params0.x);
						ImGui::InputFloat3("Row 2", &params.Params1.x);
						ImGui::InputFloat3("Row 3", &params.Params2.x);
					} else {
						int in_space = (int)params.Params0.w;
						int out_space = (int)params.Params1.w;
						if (ImGui::Combo("Input Space", &in_space, spaces.data(), (int)spaces.size()))
							params.Params0.w = (float)in_space;
						if (ImGui::Combo("Output Space", &out_space, spaces.data(), (int)spaces.size()))
							params.Params1.w = (float)out_space;

						auto mat = getRGBMatrix(spaces[in_space], spaces[out_space]);

						params.Params0 = { mat(0, 0), mat(0, 1), mat(0, 2), params.Params0.w };
						params.Params1 = { mat(1, 0), mat(1, 1), mat(1, 2), params.Params1.w };
						params.Params2 = { mat(2, 0), mat(2, 1), mat(2, 2), params.Params2.w };
					}
				},
				{ { 1.f, 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f, -1.f } } },

			{ "_"sv, "Tonemapping Operators"sv,
				"Transforms HDR values into a displayable image while retaining contrast and colours. "
				"The outputs of below operators are in linear space, as the game will apply gamma afterwards."sv,
				[](CTP&) {},
				{} },

			{ "Reinhard"sv, "Reinhard"sv,
				"Mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002."sv,
				[](CTP& params) { exposureSlider(&params.Params0.x); },
				{ { 2.f, 0.f, 0.f, 0.f } } },

			{ "Reinhard Extended"sv, "ReinhardExt"sv,
				"Extended mapping proposed in \"Photographic Tone Reproduction for Digital Images\" by Reinhard et al. 2002. "
				"An additional user parameter specifies the smallest luminance that is mapped to 1, which allows high luminances to burn out."sv,
				[](CTP& params) {
					exposureSlider(&params.Params0.x);
					ImGui::SliderFloat("White Point", &params.Params0.y, 0.f, 10.f, "%.2f"); },
				{ { 2.f, 2.f, 0.f, 0.f } } },

			{ "Hejl Burgess-Dawson Filmic"sv, "HejlBurgessDawsonFilmic"sv,
				"Variation of the Hejl and Burgess-Dawson filmic curve done by Graham Aldridge. "
				"See his blog post about \"Approximating Film with Tonemapping\"."sv,
				[](CTP& params) { exposureSlider(&params.Params0.x); },
				{ { 2.f, 0.f, 0.f, 0.f } } },

			{ "Aldridge Filmic"sv, "AldridgeFilmic"sv,
				"Variation of the Hejl and Burgess-Dawson filmic curve done by Graham Aldridge. "
				"See his blog post about \"Approximating Film with Tonemapping\"."sv,
				[](CTP& params) { 
					exposureSlider(&params.Params0.x);
					ImGui::SliderFloat("Cutoff", &params.Params0.y, 0.f, .5f, "%.2f"); },
				{ { 2.f, .19f, 0.f, 0.f } } },

			{ "Lottes Filmic/AMD Curve"sv, "LottesFilmic"sv,
				"Filmic curve by Timothy Lottes, described in his GDC talk \"Advanced Techniques and Optimization of HDR Color Pipelines\". "
				"Also known as the \"AMD curve\"."sv,
				[](CTP& params) { 
					exposureSlider(&params.Params0.x);
					ImGui::SliderFloat("Contrast", &params.Params0.y, 1.f, 2.f, "%.2f");
					ImGui::SliderFloat("Shoulder", &params.Params0.z, 0.01f, 2.f, "%.2f");
					ImGui::SliderFloat("Maximum HDR Value", &params.Params0.w, 1.f, 10.f, "%.2f");
					ImGui::SliderFloat("Input Mid-Level", &params.Params1.x, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Output Mid-Level", &params.Params1.y, 0.f, 1.f, "%.2f"); },
				{ { 2.f, 1.6f, 0.977f, 8.f }, { 0.18f, 0.267f, 0.f, 0.f } } },

			{ "Day Filmic/Insomniac Curve"sv, "DayFilmic"sv,
				"Filmic curve by Mike Day, described in his document \"An efficient and user-friendly tone mapping operator\". "
				"Also known as the \"Insomniac curve\"."sv,
				[](CTP& params) { 
					exposureSlider(&params.Params0.x);
					ImGui::SliderFloat("Black Point", &params.Params0.y, 0.f, 5.f, "%.2f");
					ImGui::SliderFloat("White Point", &params.Params0.z, 0.f, 5.f, "%.2f");

					ImGui::SliderFloat("Cross-over Point", &params.Params0.w, 0.f, 5.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Point where the toe and shoulder are pieced together into a single curve.");
					ImGui::SliderFloat("Shoulder Strength", &params.Params1.x, 0.f, 1.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the shoulder.");
					ImGui::SliderFloat("Toe Strength", &params.Params1.y, 0.f, 1.f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Amount of blending between a straight-line curve and a purely asymptotic curve for the toe."); },
				{ { 2.f, 0.f, 2.f, 0.3f }, { 0.8f, 0.7f, 0.f, 0.f } } },

			{ "Uchimura/Grand Turismo Curve"sv, "UchimuraFilmic"sv,
				"Filmic curve by Hajime Uchimura, described in his CEDEC talk \"HDR Theory and Practice\". Characterised by its middle linear section. "
				"Also known as the \"Gran Turismo curve\"."sv,
				[](CTP& params) { 
					exposureSlider(&params.Params0.x);
					ImGui::SliderFloat("Max Brightness", &params.Params0.y, 0.01f, 2.f, "%.2f");
					ImGui::SliderFloat("Contrast", &params.Params0.z, 0.f, 5.f, "%.2f");
					ImGui::SliderFloat("Linear Section Start", &params.Params0.w, 0.f, 1.f, "%.2f");
					ImGui::SliderFloat("Linear Section Length", &params.Params1.x, .01f, .99f, "%.2f");
					ImGui::SliderFloat("Black Tightness Shape", &params.Params1.y, 1.f, 3.f, "%.2f");
					ImGui::SliderFloat("Black Tightness Offset", &params.Params1.z, 0.f, 1.f, "%.2f"); },
				{ { 2.f, 1.f, 1.f, .22f }, { 0.4f, 1.33f, 0.f, 0.f } } },

			{ "ACES (Hill)"sv, "AcesHill"sv,
				"ACES curve fit by Stephen Hill."sv,
				[](CTP& params) { exposureSlider(&params.Params0.x); },
				{ { 2.f, 0.f, 0.f, 0.f } } },

			{ "ACES (Narkowicz)"sv, "AcesNarkowicz"sv,
				"ACES curve fit by Krzysztof Narkowicz. See his blog post \"ACES Filmic Tone Mapping Curve\"."sv,
				[](CTP& params) { exposureSlider(&params.Params0.x); },
				{ { 2.f, 0.f, 0.f, 0.f } } },

			{ "ACES (Guy)"sv, "AcesGuy"sv,
				"Curve from Unreal 3 adapted by to close to the ACES curve by Romain Guy."sv,
				[](CTP& params) { exposureSlider(&params.Params0.x); },
				{ { 2.f, 0.f, 0.f, 0.f } } },

			{ "AgX Minimal"sv, "AgxMinimal"sv,
				"Minimal version of Troy Sobotka's AgX using a 6th order polynomial approximation. "
				"Originally created by bwrensch, and improved by Troy Sobotka."sv,
				[](CTP& params) { 
					ImGui::SliderFloat("Slope", &params.Params0.x, 0.f, 2.f, "%.2f");
					ImGui::SliderFloat("Power", &params.Params0.y, 0.f, 2.f, "%.2f");
					ImGui::SliderFloat("Offset", &params.Params0.z, -1.f, 1.f, "%.2f");
					ImGui::SliderFloat("Saturation", &params.Params0.w, 0.f, 2.f, "%.2f"); },
				{ { 1.2f, 1.3f, 0.f, 1.f } } },
		};

		static std::once_flag flag;
		std::call_once(flag,
			[&]() {
				for (auto& t : transforms)
					t.cached_settings = t.default_settings;
			});

		return transforms;
	}

	static void GetDefaultParams(int& transformType, CTP& params)
	{
		auto& transforms = GetTransforms();
		if (auto it = std::ranges::find_if(transforms, [&](TransformInfo& x) { return "ASC CDL"sv == x.name; });
			it != transforms.end()) {
			transformType = (int)(it - transforms.begin());
			params = it->default_settings;
		} else
			logger::error("Somehow, the default settings are invalid. Please contact the author.");
	}
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void ColourTransforms::DrawSettings()
{
	auto& transforms = TransformInfo::GetTransforms();

	if (ImGui::BeginCombo("Transforms", transforms[transformType].name.data(), ImGuiComboFlags_HeightLargest)) {
		for (int i = 0; i < transforms.size(); ++i) {
			if (transforms[i].name == "_"sv) {
				ImGui::SeparatorText(transforms[i].func_name.data());
			} else {
				if (ImGui::Selectable(transforms[i].name.data(), i == transformType)) {
					transforms[transformType].cached_settings = settings;
					settings = transforms[i].cached_settings;
					transformType = i;
					recompileFlag = true;
				}
			}

			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(transforms[i].desc.data());
		}
		ImGui::EndCombo();
	}
	ImGui::Spacing();
	ImGui::TextWrapped(transforms[transformType].desc.data());
	ImGui::Spacing();
	if (ImGui::Button("Reset", { -1, 0 }))
		settings = transforms[transformType].default_settings;
	ImGui::Spacing();

	transforms[transformType].draw_settings_func(settings);
}

void ColourTransforms::RestoreDefaultSettings()
{
	TransformInfo::GetDefaultParams(transformType, settings);
	recompileFlag = true;
}

void ColourTransforms::LoadSettings(json& o_json)
{
	auto& transforms = TransformInfo::GetTransforms();

	SavedSettings tempSettings = o_json;

	if (auto it = std::ranges::find_if(transforms, [&](TransformInfo& x) { return tempSettings.TransformType == x.name; });
		it != transforms.end()) {
		transformType = (int)(it - transforms.begin());
		settings.Params0 = tempSettings.Params0;
		settings.Params1 = tempSettings.Params1;
		settings.Params2 = tempSettings.Params2;
		settings.Params3 = tempSettings.Params3;
	} else {
		TransformInfo::GetDefaultParams(transformType, settings);
	}
}

void ColourTransforms::SaveSettings(json& o_json)
{
	auto& transforms = TransformInfo::GetTransforms();

	SavedSettings tempSettings = {
		.TransformType = transforms[transformType].name.data(),
		.Params0 = settings.Params0,
		.Params1 = settings.Params1,
	};

	o_json = tempSettings;
}

void ColourTransforms::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	logger::debug("Creating buffers...");
	{
		tonemapCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TonemapCB>());
	}

	logger::debug("Creating 2D textures...");
	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

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

void ColourTransforms::ClearShaderCache()
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

void ColourTransforms::CompileComputeShaders()
{
	auto& transforms = TransformInfo::GetTransforms();

	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &tonemapCS, "transform.cs.hlsl", { { "TRANSFORM_FUNC", transforms[transformType].func_name.data() } } },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\ColourTransforms") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

void ColourTransforms::Draw(TextureInfo& inout_tex)
{
	auto context = State::GetSingleton()->context;

	if (recompileFlag)
		ClearShaderCache();

	tonemapCB->Update(settings);

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
