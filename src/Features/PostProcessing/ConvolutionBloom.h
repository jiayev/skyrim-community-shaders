#pragma once

#include "PostProcessFeature.h"

#include "Buffer.h"

struct ConvolutionBloom : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Convolution Bloom"; }
	virtual inline std::string GetDesc() const override { return "FFT-based bloom with arbitrary kernel shapes. Supports custom bloom kernels for realistic lens effects."; }

	struct Settings
	{
		float Intensity = 1.0f;
		float Size = 0.5f;           // kernel support scale (0-1)
		float PreFilterMin = 5.0f;   // bright pixel min threshold (luma)
		float PreFilterMax = 15.0f;  // bright pixel max clamp (luma)
		float PreFilterMult = 1.0f;  // bright pixel gain multiplier
		float BufferScale = 0.5f;    // resolution scale for FFT (lower = faster)
		std::string KernelPath;      // custom kernel texture path
	} settings;

	struct alignas(16) ConvBloomCB
	{
		uint32_t SrcRect[4];       // source window min.xy, max.xy
		uint32_t DstExtent[2];     // FFT buffer dimensions
		uint32_t TransformType;    // bit flags (1=horizontal, 2=forward, 4=prefilter)
		float BloomIntensity;      // bloom intensity multiplier
		float PreFilterParams[3];  // (minLuma, maxLuma, multiplier)
		float KernelSupportScale;  // fraction of screen kernel covers
		float KernelCenterUV[2];   // UV of kernel center (brightest pixel)
		float KernelTexSize[2];    // kernel texture dimensions
		float KernelDCEnergy;      // DC component magnitude for normalization
		float BloomUVScale[2];     // scaledW/fftDim, scaledH/fftDim for composite UV
		uint32_t Padding0;
	};
	STATIC_ASSERT_ALIGNAS_16(ConvBloomCB)

	eastl::unique_ptr<ConstantBuffer> convBloomCB;
	winrt::com_ptr<ID3D11SamplerState> bilinearWrapSampler;

	// Kernel texture (loaded from DDS)
	eastl::unique_ptr<Texture2D> texKernelSpatial;

	// FFT ping-pong buffers (R32G32B32A32_FLOAT)
	eastl::unique_ptr<Texture2D> texFFTBufA;
	eastl::unique_ptr<Texture2D> texFFTBufB;

	// Cached kernel spectrum (persistent between frames)
	eastl::unique_ptr<Texture2D> texKernelSpectral;

	// Output texture
	eastl::unique_ptr<Texture2D> texOutput;

	// Compute shaders
	winrt::com_ptr<ID3D11ComputeShader> fftCS;
	winrt::com_ptr<ID3D11ComputeShader> downsampleCS;
	winrt::com_ptr<ID3D11ComputeShader> resizeKernelCS;
	winrt::com_ptr<ID3D11ComputeShader> multiplyCS;
	winrt::com_ptr<ID3D11ComputeShader> compositeCS;

	// FFT state
	uint32_t fftDim = 0;         // current FFT dimension (power of 2)
	float kernelCenterU = 0.5f;  // kernel center UV coordinates
	float kernelCenterV = 0.5f;
	float kernelDCEnergy = 1.0f;  // kernel DC magnitude for normalization
	bool kernelSpectrumDirty = true;
	std::string fftDimDefine;  // cached define string for SCAN_LINE_LENGTH

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
	void LoadKernelTexture();
	void PrepareKernelSpectrum();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;
};
