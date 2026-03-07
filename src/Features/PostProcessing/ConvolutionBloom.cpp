#include "ConvolutionBloom.h"

#include "State.h"
#include "Util.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <bit>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ConvolutionBloom::Settings,
	Intensity,
	Size,
	PreFilterMin,
	PreFilterMax,
	PreFilterMult,
	BufferScale,
	KernelPath)

void ConvolutionBloom::DrawSettings()
{
	ImGui::SliderFloat("Intensity", &settings.Intensity, 0.0f, 5.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Overall strength of the bloom effect.");

	if (ImGui::SliderFloat("Size", &settings.Size, 0.01f, 1.0f, "%.3f"))
		kernelSpectrumDirty = true;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How much of the screen the bloom kernel covers. Larger values produce wider bloom.");

	ImGui::Separator();

	ImGui::SliderFloat("Pre-Filter Min", &settings.PreFilterMin, 0.0f, 20.0f, "%.1f");
	ImGui::SliderFloat("Pre-Filter Max", &settings.PreFilterMax, 0.0f, 50.0f, "%.1f");
	ImGui::SliderFloat("Pre-Filter Mult", &settings.PreFilterMult, 0.0f, 5.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Boosts bright pixels above threshold before applying bloom.");

	ImGui::Separator();

	ImGui::SliderFloat("Buffer Scale", &settings.BufferScale, 0.1f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Lower values improve performance but reduce bloom quality. 0.5 is recommended.");

	if (texFFTBufA && ImGui::CollapsingHeader("Debug")) {
		ImGui::Text("FFT Dimension: %u x %u", fftDim, fftDim);
		ImGui::Text("Kernel Center UV: (%.3f, %.3f)", kernelCenterU, kernelCenterV);
		ImGui::Text("Kernel DC Energy: %.3f", kernelDCEnergy);
	}
}

void ConvolutionBloom::RestoreDefaultSettings()
{
	settings = {};
	kernelSpectrumDirty = true;
}

void ConvolutionBloom::LoadSettings(json& o_json)
{
	settings = o_json;
	kernelSpectrumDirty = true;
}

void ConvolutionBloom::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ConvolutionBloom::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("ConvolutionBloom: Creating buffers...");
	{
		convBloomCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<ConvBloomCB>());
	}

	logger::debug("ConvolutionBloom: Creating samplers...");
	{
		D3D11_SAMPLER_DESC sampDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
			.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
			.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, bilinearWrapSampler.put()));
	}

	LoadKernelTexture();
	// FFT shaders are compiled lazily in Draw() when we know the required FFT dimension
}

void ConvolutionBloom::LoadKernelTexture()
{
	auto device = globals::d3d::device;

	auto kernelPath = settings.KernelPath.empty() ? std::filesystem::path("Data\\textures\\DefaultBloomKernel.DDS") : std::filesystem::path(settings.KernelPath);

	if (!std::filesystem::exists(kernelPath)) {
		logger::warn("ConvolutionBloom: Kernel texture not found: {}", kernelPath.string());
		return;
	}

	// Load into ScratchImage for CPU pixel access (find brightest pixel)
	DirectX::ScratchImage image;
	try {
		DX::ThrowIfFailed(DirectX::LoadFromDDSFile(kernelPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
	} catch (const DX::com_exception& e) {
		logger::error("ConvolutionBloom: Failed to load kernel DDS: {}", e.what());
		return;
	}

	// Convert to R32G32B32A32_FLOAT for easy CPU pixel scanning
	DirectX::ScratchImage converted;
	try {
		DX::ThrowIfFailed(DirectX::Convert(
			*image.GetImage(0, 0, 0),
			DXGI_FORMAT_R32G32B32A32_FLOAT,
			DirectX::TEX_FILTER_DEFAULT, 1.0f, converted));
	} catch (const DX::com_exception& e) {
		logger::error("ConvolutionBloom: Failed to convert kernel pixels: {}", e.what());
		return;
	}

	// Find brightest pixel → kernel center
	const auto* img = converted.GetImage(0, 0, 0);
	float maxLuma = 0;
	uint32_t cx = 0, cy = 0;
	for (uint32_t y = 0; y < img->height; ++y) {
		const auto* row = reinterpret_cast<const float*>(img->pixels + y * img->rowPitch);
		for (uint32_t x = 0; x < img->width; ++x) {
			const float* px = row + x * 4;
			float luma = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
			if (luma > maxLuma) {
				maxLuma = luma;
				cx = x;
				cy = y;
			}
		}
	}
	kernelCenterU = (cx + 0.5f) / static_cast<float>(img->width);
	kernelCenterV = (cy + 0.5f) / static_cast<float>(img->height);

	logger::debug("ConvolutionBloom: Kernel center at ({}, {}) UV=({:.3f}, {:.3f}) maxLuma={:.1f}",
		cx, cy, kernelCenterU, kernelCenterV, maxLuma);

	// Create GPU texture from the original DDS
	ID3D11Resource* pRsrc = nullptr;
	ID3D11ShaderResourceView* pSrv = nullptr;
	try {
		DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, kernelPath.c_str(), &pRsrc, &pSrv));
	} catch (const DX::com_exception& e) {
		logger::error("ConvolutionBloom: Failed to create kernel GPU texture: {}", e.what());
		return;
	}

	texKernelSpatial = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pRsrc));
	texKernelSpatial->srv.attach(pSrv);
	kernelSpectrumDirty = true;
}

void ConvolutionBloom::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry;
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &fftCS, "convolution_bloom.cs.hlsl",
			{ { "SCAN_LINE_LENGTH", fftDimDefine.c_str() }, { "RADIX", "8" } }, "CS_FFT" },
		{ &downsampleCS, "convolution_bloom.cs.hlsl",
			{ { "SCAN_LINE_LENGTH", fftDimDefine.c_str() }, { "RADIX", "8" } }, "CS_Downsample" },
		{ &resizeKernelCS, "convolution_bloom.cs.hlsl",
			{ { "SCAN_LINE_LENGTH", fftDimDefine.c_str() }, { "RADIX", "8" } }, "CS_ResizeKernel" },
		{ &multiplyCS, "convolution_bloom.cs.hlsl",
			{ { "SCAN_LINE_LENGTH", fftDimDefine.c_str() }, { "RADIX", "8" } }, "CS_Multiply" },
		{ &compositeCS, "convolution_bloom.cs.hlsl",
			{ { "SCAN_LINE_LENGTH", fftDimDefine.c_str() }, { "RADIX", "8" } }, "CS_Composite" },
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\ConvolutionBloom") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(
				Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

void ConvolutionBloom::ClearShaderCache()
{
	auto const shaderPtrs = std::array{
		&fftCS, &downsampleCS, &resizeKernelCS, &multiplyCS, &compositeCS
	};

	for (auto shader : shaderPtrs)
		if (*shader) {
			(*shader)->Release();
			shader->detach();
		}

	if (fftDim > 0)
		CompileComputeShaders();
}

void ConvolutionBloom::PrepareKernelSpectrum()
{
	if (!texKernelSpatial || !texKernelSpectral || !resizeKernelCS || !fftCS)
		return;

	auto context = globals::d3d::context;
	auto state = globals::state;

	state->BeginPerfEvent("ConvBloom Kernel Prep");

	ConvBloomCB cbData = {};
	cbData.DstExtent[0] = fftDim;
	cbData.DstExtent[1] = fftDim;
	cbData.KernelSupportScale = settings.Size;
	cbData.KernelCenterUV[0] = kernelCenterU;
	cbData.KernelCenterUV[1] = kernelCenterV;
	cbData.KernelTexSize[0] = static_cast<float>(texKernelSpatial->desc.Width);
	cbData.KernelTexSize[1] = static_cast<float>(texKernelSpatial->desc.Height);

	auto cb = convBloomCB->CB();
	std::array<ID3D11SamplerState*, 1> samplers = { bilinearWrapSampler.get() };

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, static_cast<uint>(samplers.size()), samplers.data());

	auto clearResources = [&]() {
		ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetShaderResources(0, 2, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	};

	// Step 1: Resize kernel → texFFTBufA
	{
		convBloomCB->Update(cbData);

		// CS_ResizeKernel reads KernelTex (t1) and writes to OutputTex (u0)
		ID3D11ShaderResourceView* nullSRV = nullptr;
		auto kernelSrv = texKernelSpatial->srv.get();
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetShaderResources(1, 1, &kernelSrv);
		auto uav = texFFTBufA->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(resizeKernelCS.get(), nullptr, 0);
		context->Dispatch(((fftDim - 1) / 8) + 1, ((fftDim - 1) / 8) + 1, 1);
	}

	// Step 2: Forward FFT Horizontal → texFFTBufB
	{
		clearResources();

		cbData.TransformType = 1u | 2u;  // horizontal + forward
		cbData.SrcRect[0] = 0;
		cbData.SrcRect[1] = 0;
		cbData.SrcRect[2] = fftDim;
		cbData.SrcRect[3] = fftDim;
		convBloomCB->Update(cbData);

		auto srv = texFFTBufA->srv.get();
		context->CSSetShaderResources(0, 1, &srv);
		auto uav = texFFTBufB->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(fftCS.get(), nullptr, 0);
		context->Dispatch(1, 1, fftDim);
	}

	// Step 3: Forward FFT Vertical → texKernelSpectral
	{
		clearResources();

		cbData.TransformType = 0u | 2u;  // vertical + forward
		convBloomCB->Update(cbData);

		auto srv = texFFTBufB->srv.get();
		context->CSSetShaderResources(0, 1, &srv);
		auto uav = texKernelSpectral->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch(1, 1, fftDim);
	}

	// Read back DC component (pixel 0,0) for energy normalization
	{
		auto device = globals::d3d::device;

		D3D11_TEXTURE2D_DESC stagingDesc = {
			.Width = 1,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_STAGING,
			.CPUAccessFlags = D3D11_CPU_ACCESS_READ,
		};
		winrt::com_ptr<ID3D11Texture2D> staging;
		DX::ThrowIfFailed(device->CreateTexture2D(&stagingDesc, nullptr, staging.put()));

		D3D11_BOX box = { 0, 0, 0, 1, 1, 1 };
		context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0,
			texKernelSpectral->resource.get(), 0, &box);

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
		const auto* dcPixel = reinterpret_cast<const float*>(mapped.pData);
		float dcReal = dcPixel[0];
		float dcImag = dcPixel[1];
		kernelDCEnergy = std::sqrt(dcReal * dcReal + dcImag * dcImag);
		context->Unmap(staging.get(), 0);

		if (kernelDCEnergy < 0.0001f)
			kernelDCEnergy = 1.0f;
	}

	// Cleanup
	clearResources();
	context->CSSetShader(nullptr, nullptr, 0);

	state->EndPerfEvent();

	logger::debug("ConvolutionBloom: Kernel spectrum prepared (DC energy: {:.3f})", kernelDCEnergy);
}

void ConvolutionBloom::Draw(TextureInfo& inout_tex)
{
	if (!texKernelSpatial)
		return;

	auto context = globals::d3d::context;
	auto state = globals::state;

	state->BeginPerfEvent("Convolution Bloom");

	// --- Determine FFT dimensions from input ---
	D3D11_TEXTURE2D_DESC inputDesc;
	inout_tex.tex->GetDesc(&inputDesc);

	uint32_t scaledW = std::max(static_cast<uint32_t>(inputDesc.Width * settings.BufferScale), 1u);
	uint32_t scaledH = std::max(static_cast<uint32_t>(inputDesc.Height * settings.BufferScale), 1u);
	uint32_t newFFTDim = std::bit_ceil(std::max(scaledW, scaledH));
	newFFTDim = std::clamp(newFFTDim, 256u, 4096u);

	// --- Recreate resources if FFT dimension changed ---
	if (newFFTDim != fftDim) {
		fftDim = newFFTDim;
		fftDimDefine = std::to_string(fftDim);
		logger::debug("ConvolutionBloom: FFT dimension = {}", fftDim);

		D3D11_TEXTURE2D_DESC fftDesc = {
			.Width = fftDim,
			.Height = fftDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = fftDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = fftDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texFFTBufA = eastl::make_unique<Texture2D>(fftDesc);
		texFFTBufA->CreateSRV(srvDesc);
		texFFTBufA->CreateUAV(uavDesc);

		texFFTBufB = eastl::make_unique<Texture2D>(fftDesc);
		texFFTBufB->CreateSRV(srvDesc);
		texFFTBufB->CreateUAV(uavDesc);

		texKernelSpectral = eastl::make_unique<Texture2D>(fftDesc);
		texKernelSpectral->CreateSRV(srvDesc);
		texKernelSpectral->CreateUAV(uavDesc);

		// Output texture at input resolution
		D3D11_TEXTURE2D_DESC outDesc = {
			.Width = inputDesc.Width,
			.Height = inputDesc.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = inputDesc.Format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC outSrvDesc = {
			.Format = outDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC outUavDesc = {
			.Format = outDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texOutput = eastl::make_unique<Texture2D>(outDesc);
		texOutput->CreateSRV(outSrvDesc);
		texOutput->CreateUAV(outUavDesc);

		CompileComputeShaders();
		kernelSpectrumDirty = true;
	}

	if (!fftCS || !downsampleCS || !resizeKernelCS || !multiplyCS || !compositeCS) {
		state->EndPerfEvent();
		return;
	}

	// --- Prepare kernel spectrum if dirty ---
	if (kernelSpectrumDirty) {
		PrepareKernelSpectrum();
		kernelSpectrumDirty = false;
	}

	// --- Per-frame convolution pipeline ---
	ConvBloomCB cbData = {};
	cbData.DstExtent[0] = fftDim;
	cbData.DstExtent[1] = fftDim;
	cbData.BloomIntensity = settings.Intensity;
	cbData.PreFilterParams[0] = settings.PreFilterMin;
	cbData.PreFilterParams[1] = settings.PreFilterMax;
	cbData.PreFilterParams[2] = settings.PreFilterMult;
	cbData.KernelDCEnergy = kernelDCEnergy;
	cbData.BloomUVScale[0] = static_cast<float>(scaledW) / static_cast<float>(fftDim);
	cbData.BloomUVScale[1] = static_cast<float>(scaledH) / static_cast<float>(fftDim);

	auto cb = convBloomCB->CB();
	std::array<ID3D11SamplerState*, 1> samplers = { bilinearWrapSampler.get() };

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, static_cast<uint>(samplers.size()), samplers.data());

	auto clearResources = [&]() {
		ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetShaderResources(0, 2, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	};

	// Pass 0: Downsample scene to FFT buffer scale → texFFTBufA
	{
		cbData.DstExtent[0] = scaledW;
		cbData.DstExtent[1] = scaledH;
		convBloomCB->Update(cbData);

		auto srv = inout_tex.srv;
		context->CSSetShaderResources(0, 1, &srv);
		auto uav = texFFTBufA->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(downsampleCS.get(), nullptr, 0);
		context->Dispatch(((scaledW - 1) / 8) + 1, ((scaledH - 1) / 8) + 1, 1);

		// Restore DstExtent to FFT dimensions for FFT passes
		cbData.DstExtent[0] = fftDim;
		cbData.DstExtent[1] = fftDim;
	}

	// Pass 1: Forward FFT Horizontal (with prefilter)
	{
		clearResources();

		cbData.TransformType = 1u | 2u | 4u;  // horizontal + forward + prefilter
		cbData.SrcRect[0] = 0;
		cbData.SrcRect[1] = 0;
		cbData.SrcRect[2] = scaledW;
		cbData.SrcRect[3] = scaledH;
		convBloomCB->Update(cbData);

		auto srv = texFFTBufA->srv.get();
		context->CSSetShaderResources(0, 1, &srv);
		auto uav = texFFTBufB->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(fftCS.get(), nullptr, 0);
		context->Dispatch(1, 1, fftDim);
	}

	// Pass 2: Forward FFT Vertical
	{
		clearResources();

		cbData.TransformType = 0u | 2u;  // vertical + forward
		cbData.SrcRect[2] = fftDim;
		cbData.SrcRect[3] = fftDim;
		convBloomCB->Update(cbData);

		auto srv = texFFTBufB->srv.get();
		context->CSSetShaderResources(0, 1, &srv);
		auto uav = texFFTBufA->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch(1, 1, fftDim);
	}

	// Pass 3: Frequency domain multiply (image × kernel spectrum)
	{
		clearResources();

		convBloomCB->Update(cbData);

		auto imgSrv = texFFTBufA->srv.get();
		auto kernSrv = texKernelSpectral->srv.get();
		context->CSSetShaderResources(0, 1, &imgSrv);
		context->CSSetShaderResources(1, 1, &kernSrv);
		auto uav = texFFTBufB->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(multiplyCS.get(), nullptr, 0);
		context->Dispatch(((fftDim - 1) / 8) + 1, ((fftDim - 1) / 8) + 1, 1);
	}

	// Pass 4: Inverse FFT Vertical
	{
		clearResources();

		cbData.TransformType = 0u;  // vertical + inverse
		convBloomCB->Update(cbData);

		auto srv = texFFTBufB->srv.get();
		context->CSSetShaderResources(0, 1, &srv);
		auto uav = texFFTBufA->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(fftCS.get(), nullptr, 0);
		context->Dispatch(1, 1, fftDim);
	}

	// Pass 5: Inverse FFT Horizontal
	{
		clearResources();

		cbData.TransformType = 1u;  // horizontal + inverse
		convBloomCB->Update(cbData);

		auto srv = texFFTBufA->srv.get();
		context->CSSetShaderResources(0, 1, &srv);
		auto uav = texFFTBufB->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch(1, 1, fftDim);
	}

	// Pass 6: Composite (original scene + bloom → output)
	{
		clearResources();

		cbData.SrcRect[2] = scaledW;
		cbData.SrcRect[3] = scaledH;
		convBloomCB->Update(cbData);

		auto origSrv = inout_tex.srv;
		auto bloomSrv = texFFTBufB->srv.get();
		context->CSSetShaderResources(0, 1, &origSrv);
		context->CSSetShaderResources(1, 1, &bloomSrv);
		auto uav = texOutput->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(compositeCS.get(), nullptr, 0);
		context->Dispatch(((inputDesc.Width - 1) / 8) + 1, ((inputDesc.Height - 1) / 8) + 1, 1);
	}

	// Cleanup
	clearResources();
	{
		ID3D11SamplerState* nullSamp = nullptr;
		ID3D11Buffer* nullCB = nullptr;
		context->CSSetSamplers(0, 1, &nullSamp);
		context->CSSetConstantBuffers(1, 1, &nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// Return output
	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };

	state->EndPerfEvent();
}
