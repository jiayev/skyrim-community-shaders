#include "Camera.h"

#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Camera::Settings,
	UseFE,
	FEFoV,
	FECrop,
	CAStrength,
	NoiseStrength,
	NoiseType)

void Camera::DrawSettings()
{
	ImGui::Checkbox(T("feature.post_processing.camera.fisheye", "Fisheye"), &settings.UseFE);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", T("feature.post_processing.camera.enable_fisheye_effect", "Enable fisheye effect."));
	}

	if (settings.UseFE) {
		ImGui::SliderFloat(T("feature.post_processing.camera.fov", "FOV"), &settings.FEFoV, 20.0f, 180.0f, "%1.0f °");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", T("feature.post_processing.camera.fov_in_degrees_set_to_in_game_fov", "FOV in degrees.\n\nSet to in-game FOV."));
		}

		ImGui::SliderFloat(T("feature.post_processing.camera.crop", "Crop"), &settings.FECrop, 0.0f, 1.0f, "%.3f");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", T("feature.post_processing.camera.how_much_to_crop_into_the_image", "How much to crop into the image.\n\n0 = circular, 1 = full-frame."));
		}
	}

	ImGui::SliderFloat(T("feature.post_processing.camera.ca_amount", "CA amount"), &settings.CAStrength, 0.0f, 1.0f, "%.3f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", T("feature.post_processing.camera.chromatic_aberration_strength", "Chromatic aberration strength."));
	}

	ImGui::SliderFloat(T("feature.post_processing.camera.noise_amount", "Noise amount"), &settings.NoiseStrength, 0.0f, 1.0f, "%.3f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", T("feature.post_processing.camera.amount_of_noise_to_apply", "Amount of noise to apply."));
	}

	ImGui::Combo(T("feature.post_processing.camera.noise_type", "Noise type"), &settings.NoiseType, "Film grain\0Color grain\0\0");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", T("feature.post_processing.camera.type_of_noise_to_apply", "Type of noise to apply."));
	}
}

void Camera::RestoreDefaultSettings()
{
	settings = {};
}

void Camera::LoadSettings(json& o_json)
{
	settings = o_json;
}

void Camera::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Camera::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		cameraCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CameraCB>());
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

void Camera::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&cameraCS
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void Camera::CompileComputeShaders()
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
			{ &cameraCS, "camera.cs.hlsl", {}, "CS_Camera" }
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\Camera") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}

	if (!cameraCS) {
		logger::error("Failed to compile camera compute shader!");
		return;
	}
}

void Camera::Draw(TextureInfo& inout_tex)
{
	globals::profiler->BeginPass("PostProcessing::Camera");
	auto context = globals::d3d::context;
	float2 res = { (float)texOutput->desc.Width, (float)texOutput->desc.Height };
	res = Util::ConvertToDynamic(res);

	CameraCB data = {
		.FEFoV = settings.FEFoV,
		.FECrop = settings.FECrop,
		.CAStrength = settings.CAStrength,
		.NoiseStrength = settings.NoiseStrength,
		.NoiseType = settings.NoiseType,
		.res = res,
		.UseFE = settings.UseFE
	};

	cameraCB->Update(data);

	ID3D11ShaderResourceView* srv = inout_tex.srv;
	ID3D11UnorderedAccessView* uav = texOutput->uav.get();
	ID3D11Buffer* cb = cameraCB->CB();

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetShader(cameraCS.get(), nullptr, 0);
	context->Dispatch(((uint)res.x + 7) >> 3, ((uint)res.y + 7) >> 3, 1);

	srv = nullptr;
	uav = nullptr;
	cb = nullptr;

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
	globals::profiler->EndPass();
}
