#include "FFTBloom.h"

#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FFTBloom::Settings,
	Threshold,
	Intensity,
	FFTResolution,
	KernelType,
	KernelRadius,
	KernelFalloff,
	StarPoints,
	StarSharpness,
	StarRotation,
	AnamorphicRatio,
	Tint)

static int NearestValidFFTSize(int requested)
{
	if (requested <= 128)
		return 128;
	if (requested <= 256)
		return 256;
	return 512;
}

static int Log2FFTSize(int fftSize)
{
	switch (fftSize) {
	case 128:
		return 7;
	case 256:
		return 8;
	case 512:
		return 9;
	default:
		return 8;
	}
}

bool FFTBloom::IsKernelDirty() const
{
	return kernelDirty ||
	       lastKernelType != settings.KernelType ||
	       lastKernelRadius != settings.KernelRadius ||
	       lastKernelFalloff != settings.KernelFalloff ||
	       lastStarPoints != settings.StarPoints ||
	       lastStarSharpness != settings.StarSharpness ||
	       lastStarRotation != settings.StarRotation ||
	       lastAnamorphicRatio != settings.AnamorphicRatio;
}

void FFTBloom::MarkKernelClean()
{
	kernelDirty = false;
	lastKernelType = settings.KernelType;
	lastKernelRadius = settings.KernelRadius;
	lastKernelFalloff = settings.KernelFalloff;
	lastStarPoints = settings.StarPoints;
	lastStarSharpness = settings.StarSharpness;
	lastStarRotation = settings.StarRotation;
	lastAnamorphicRatio = settings.AnamorphicRatio;
}

void FFTBloom::DrawSettings()
{
	ImGui::SliderFloat("Threshold", &settings.Threshold, -6.f, 21.f, "%+.2f EV");
	ImGui::SliderFloat("Intensity", &settings.Intensity, 0.f, 1.f, "%.3f");

	ImGui::Separator();
	ImGui::Text("FFT Settings");

	const char* resLabels[] = { "128", "256", "512" };
	int resValues[] = { 128, 256, 512 };
	int currentRes = 1;
	for (int i = 0; i < 3; i++)
		if (settings.FFTResolution == resValues[i])
			currentRes = i;

	if (ImGui::Combo("Resolution", &currentRes, resLabels, 3)) {
		int newRes = resValues[currentRes];
		if (newRes != settings.FFTResolution) {
			settings.FFTResolution = newRes;
			CreateFFTTextures();
			CompileComputeShaders();
			kernelDirty = true;
		}
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Higher resolution = sharper bloom details but higher GPU cost");

	ImGui::Separator();
	ImGui::Text("Kernel Shape");

	const char* kernelLabels[] = { "Circular", "Star", "Anamorphic" };
	ImGui::Combo("Kernel Type", &settings.KernelType, kernelLabels, 3);

	ImGui::SliderFloat("Kernel Radius", &settings.KernelRadius, 0.01f, 1.0f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Controls the overall size of the bloom kernel");

	ImGui::SliderFloat("Kernel Falloff", &settings.KernelFalloff, 0.5f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Controls how quickly the bloom fades with distance");

	if (settings.KernelType == 1) {
		ImGui::Separator();
		ImGui::Text("Star Settings");
		ImGui::SliderInt("Star Points", &settings.StarPoints, 3, 12);
		ImGui::SliderFloat("Star Sharpness", &settings.StarSharpness, 0.5f, 10.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Higher values make the star rays thinner and more defined");
		ImGui::SliderFloat("Star Rotation", &settings.StarRotation, -90.0f, 90.0f, "%.1f deg");
	}

	if (settings.KernelType == 2) {
		ImGui::Separator();
		ImGui::Text("Anamorphic Settings");
		ImGui::SliderFloat("Anamorphic Ratio", &settings.AnamorphicRatio, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("0 = circular, 1 = fully stretched horizontal streaks");
	}

	ImGui::Separator();
	ImGui::ColorEdit3("Tint", settings.Tint.data());

	if (ImGui::CollapsingHeader("Debug")) {
		if (texBrightPass) {
			ImGui::BulletText("Bright Pass");
			ImGui::Image(texBrightPass->srv.get(), { 200.f, 200.f });
		}
		if (texBloomResult) {
			ImGui::BulletText("Bloom Result");
			ImGui::Image(texBloomResult->srv.get(), { 200.f, 200.f });
		}
		if (texKernelFFT) {
			ImGui::BulletText("Kernel FFT (magnitude)");
			ImGui::Image(texKernelFFT->srv.get(), { 200.f, 200.f });
		}
	}
}

void FFTBloom::RestoreDefaultSettings()
{
	settings = {};
	kernelDirty = true;
}

void FFTBloom::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.FFTResolution = NearestValidFFTSize(settings.FFTResolution);
	settings.StarPoints = std::clamp(settings.StarPoints, 3, 12);
	kernelDirty = true;
}

void FFTBloom::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FFTBloom::CreateFFTTextures()
{
	auto renderer = globals::game::renderer;
	int fftSize = NearestValidFFTSize(settings.FFTResolution);

	logger::debug("FFTBloom: Creating textures at {}x{}", fftSize, fftSize);

	// Bright pass and bloom result textures (RGBA16F at FFT resolution)
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = fftSize;
		texDesc.Height = fftSize;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

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

		texBrightPass = std::make_unique<Texture2D>(texDesc);
		texBrightPass->CreateSRV(srvDesc);
		texBrightPass->CreateUAV(uavDesc);

		texBloomResult = std::make_unique<Texture2D>(texDesc);
		texBloomResult->CreateSRV(srvDesc);
		texBloomResult->CreateUAV(uavDesc);
	}

	// Complex working textures (RG32F at FFT resolution)
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = fftSize;
		texDesc.Height = fftSize;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

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

		for (int i = 0; i < 2; i++) {
			texWork[i] = std::make_unique<Texture2D>(texDesc);
			texWork[i]->CreateSRV(srvDesc);
			texWork[i]->CreateUAV(uavDesc);
		}

		texKernelFFT = std::make_unique<Texture2D>(texDesc);
		texKernelFFT->CreateSRV(srvDesc);
		texKernelFFT->CreateUAV(uavDesc);
	}

	// Full-resolution output texture (matches game render target)
	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);
		texDesc.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

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

		texOutput = std::make_unique<Texture2D>(texDesc);
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateUAV(uavDesc);
	}
}

void FFTBloom::SetupResources()
{
	logger::debug("FFTBloom: Setting up resources...");

	fftCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<FFTBloomCB>());

	// Create sampler
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
		DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	}

	CreateFFTTextures();
	CompileComputeShaders();
}

void FFTBloom::ClearShaderCache()
{
	auto const shaderPtrs = std::array{
		&thresholdDownsampleCS,
		&prepareChannelCS,
		&fftHorizontalCS,
		&fftVerticalCS,
		&ifftHorizontalCS,
		&ifftVerticalCS,
		&kernelGenCS,
		&multiplyCS,
		&storeChannelCS,
		&compositeCS
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
	kernelDirty = true;
}

void FFTBloom::CompileComputeShaders()
{
	int fftSize = NearestValidFFTSize(settings.FFTResolution);
	compiledFFTSize = fftSize;
	int log2Size = Log2FFTSize(fftSize);

	auto fftSizeStr = std::to_string(fftSize);
	auto log2Str = std::to_string(log2Size);

	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry;
	};

	// Store string values for defines that need dynamic values
	auto fftSizeCStr = fftSizeStr.c_str();
	auto log2CStr = log2Str.c_str();

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &thresholdDownsampleCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_ThresholdAndDownsample" },
		{ &prepareChannelCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_PrepareChannel" },
		{ &fftHorizontalCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_FFT_Horizontal" },
		{ &fftVerticalCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_FFT_Vertical" },
		{ &ifftHorizontalCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_IFFT_Horizontal" },
		{ &ifftVerticalCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_IFFT_Vertical" },
		{ &kernelGenCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_GenerateKernel" },
		{ &multiplyCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_Multiply" },
		{ &storeChannelCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_StoreChannel" },
		{ &compositeCS, "fftbloom.cs.hlsl", { { "FFT_SIZE", fftSizeCStr }, { "LOG2_FFT_SIZE", log2CStr } }, "CS_Composite" }
	};

	for (auto& info : shaderInfos) {
		if (*info.programPtr) {
			(*info.programPtr)->Release();
			info.programPtr->detach();
		}

		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\FFTBloom") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

void FFTBloom::UpdateKernelFFT()
{
	auto context = globals::d3d::context;
	auto state = globals::state;
	int fftSize = NearestValidFFTSize(settings.FFTResolution);

	state->BeginPerfEvent("FFT Bloom Kernel Update");

	FFTBloomCB cbData = {};
	cbData.FFTSize = fftSize;
	cbData.KernelType = settings.KernelType;
	cbData.KernelRadius = settings.KernelRadius;
	cbData.KernelFalloff = settings.KernelFalloff;
	cbData.StarPoints = settings.StarPoints;
	cbData.StarSharpness = settings.StarSharpness;
	cbData.StarRotation = settings.StarRotation;
	cbData.AnamorphicRatio = settings.AnamorphicRatio;
	fftCB->Update(cbData);

	auto cb = fftCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;

	// Step 1: Generate kernel → texWork[0] (complex, imag=0)
	{
		auto uav = texWork[0]->uav.get();
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->CSSetShader(kernelGenCS.get(), nullptr, 0);
		context->Dispatch((fftSize + 31) >> 5, (fftSize + 31) >> 5, 1);

		context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
	}

	// Step 2: FFT Horizontal on kernel: texWork[0] → texWork[1]
	{
		auto srv = texWork[0]->srv.get();
		auto uav = texWork[1]->uav.get();
		context->CSSetShaderResources(1, 1, &srv);
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->CSSetShader(fftHorizontalCS.get(), nullptr, 0);
		context->Dispatch(fftSize, 1, 1);

		context->CSSetShaderResources(1, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
	}

	// Step 3: FFT Vertical on kernel: texWork[1] → texKernelFFT
	{
		auto srv = texWork[1]->srv.get();
		auto uav = texKernelFFT->uav.get();
		context->CSSetShaderResources(1, 1, &srv);
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->CSSetShader(fftVerticalCS.get(), nullptr, 0);
		context->Dispatch(fftSize, 1, 1);

		context->CSSetShaderResources(1, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
	}

	// Cleanup
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);

	MarkKernelClean();

	state->EndPerfEvent();
}

void FFTBloom::Draw(TextureInfo& inout_tex)
{
	auto context = globals::d3d::context;
	auto state = globals::state;
	int fftSize = NearestValidFFTSize(settings.FFTResolution);

	// Check if we need to recompile/resize
	if (fftSize != compiledFFTSize) {
		CreateFFTTextures();
		CompileComputeShaders();
		kernelDirty = true;
	}

	state->BeginPerfEvent("FFT Convolution Bloom");

	// Update kernel FFT if settings changed
	if (IsKernelDirty()) {
		UpdateKernelFFT();
	}

	// Get source dimensions
	D3D11_TEXTURE2D_DESC srcDesc;
	inout_tex.tex->GetDesc(&srcDesc);

	// Update constant buffer
	FFTBloomCB cbData = {};
	cbData.Threshold = exp2(settings.Threshold) * 0.125f;
	cbData.Intensity = settings.Intensity;
	cbData.Channel = 0;
	cbData.FFTSize = fftSize;
	cbData.KernelType = settings.KernelType;
	cbData.KernelRadius = settings.KernelRadius;
	cbData.KernelFalloff = settings.KernelFalloff;
	cbData.StarPoints = settings.StarPoints;
	cbData.StarSharpness = settings.StarSharpness;
	cbData.StarRotation = settings.StarRotation;
	cbData.AnamorphicRatio = settings.AnamorphicRatio;
	cbData.Tint[0] = settings.Tint[0];
	cbData.Tint[1] = settings.Tint[1];
	cbData.Tint[2] = settings.Tint[2];
	cbData.Tint[3] = 1.0f;
	cbData.SourceDimsX = srcDesc.Width;
	cbData.SourceDimsY = srcDesc.Height;
	fftCB->Update(cbData);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };

	auto cb = fftCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, samplers.data());

	// ========================================================================
	// Step 1: Threshold + Downsample → texBrightPass
	// ========================================================================
	{
		auto srv = inout_tex.srv;
		auto uav = texBrightPass->uav.get();
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(thresholdDownsampleCS.get(), nullptr, 0);
		context->Dispatch((fftSize + 31) >> 5, (fftSize + 31) >> 5, 1);

		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	}

	// ========================================================================
	// Step 2: Process each color channel through FFT pipeline
	// ========================================================================
	for (int channel = 0; channel < 3; channel++) {
		cbData.Channel = channel;
		fftCB->Update(cbData);

		// 2a: Prepare channel: texBrightPass[channel] → texWork[0]
		{
			auto srv = texBrightPass->srv.get();
			auto uav = texWork[0]->uav.get();
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
			context->CSSetShader(prepareChannelCS.get(), nullptr, 0);
			context->Dispatch((fftSize + 31) >> 5, (fftSize + 31) >> 5, 1);

			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
		}

		// 2b: Forward FFT Horizontal: texWork[0] → texWork[1]
		{
			auto srv = texWork[0]->srv.get();
			auto uav = texWork[1]->uav.get();
			context->CSSetShaderResources(1, 1, &srv);
			context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
			context->CSSetShader(fftHorizontalCS.get(), nullptr, 0);
			context->Dispatch(fftSize, 1, 1);

			context->CSSetShaderResources(1, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
		}

		// 2c: Forward FFT Vertical: texWork[1] → texWork[0]
		{
			auto srv = texWork[1]->srv.get();
			auto uav = texWork[0]->uav.get();
			context->CSSetShaderResources(1, 1, &srv);
			context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
			context->CSSetShader(fftVerticalCS.get(), nullptr, 0);
			context->Dispatch(fftSize, 1, 1);

			context->CSSetShaderResources(1, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
		}

		// 2d: Multiply with kernel FFT: texWork[0] * texKernelFFT → texWork[1]
		{
			auto srv0 = texWork[0]->srv.get();
			auto srv1 = texKernelFFT->srv.get();
			auto uav = texWork[1]->uav.get();
			context->CSSetShaderResources(1, 1, &srv0);
			context->CSSetShaderResources(2, 1, &srv1);
			context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
			context->CSSetShader(multiplyCS.get(), nullptr, 0);
			context->Dispatch((fftSize + 31) >> 5, (fftSize + 31) >> 5, 1);

			context->CSSetShaderResources(1, 1, &nullSRV);
			context->CSSetShaderResources(2, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
		}

		// 2e: Inverse FFT Vertical: texWork[1] → texWork[0]
		{
			auto srv = texWork[1]->srv.get();
			auto uav = texWork[0]->uav.get();
			context->CSSetShaderResources(1, 1, &srv);
			context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
			context->CSSetShader(ifftVerticalCS.get(), nullptr, 0);
			context->Dispatch(fftSize, 1, 1);

			context->CSSetShaderResources(1, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
		}

		// 2f: Inverse FFT Horizontal: texWork[0] → texWork[1]
		{
			auto srv = texWork[0]->srv.get();
			auto uav = texWork[1]->uav.get();
			context->CSSetShaderResources(1, 1, &srv);
			context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
			context->CSSetShader(ifftHorizontalCS.get(), nullptr, 0);
			context->Dispatch(fftSize, 1, 1);

			context->CSSetShaderResources(1, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(1, 1, &nullUAV, nullptr);
		}

		// 2g: Store channel result: texWork[1] → texBloomResult[channel]
		{
			auto srv = texWork[1]->srv.get();
			auto uav = texBloomResult->uav.get();
			context->CSSetShaderResources(1, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(storeChannelCS.get(), nullptr, 0);
			context->Dispatch((fftSize + 31) >> 5, (fftSize + 31) >> 5, 1);

			context->CSSetShaderResources(1, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		}
	}

	// ========================================================================
	// Step 3: Composite - blend bloom result with original at full resolution
	// ========================================================================
	{
		auto srv0 = inout_tex.srv;
		auto srv3 = texBloomResult->srv.get();
		auto uav = texOutput->uav.get();

		context->CSSetShaderResources(0, 1, &srv0);
		context->CSSetShaderResources(3, 1, &srv3);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(compositeCS.get(), nullptr, 0);
		context->Dispatch((srcDesc.Width + 31) >> 5, (srcDesc.Height + 31) >> 5, 1);

		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetShaderResources(3, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	}

	// Cleanup
	samplers[0] = nullptr;
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, 1, samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

	// Return composited result at full resolution
	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };

	state->EndPerfEvent();
}
