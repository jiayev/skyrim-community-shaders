#pragma once

#include "PostProcessFeature.h"

#include "Buffer.h"

struct FFTBloom : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "FFT Convolution Bloom"; }
	virtual inline std::string GetDesc() const override
	{
		return "Bloom effect using FFT-based convolution with customizable kernel shapes. "
			   "Supports circular, starburst, and anamorphic bloom patterns. Expects HDR linear RGB inputs.";
	}

	static constexpr int s_MaxFFTSize = 512;
	static constexpr int s_MinFFTSize = 128;
	static constexpr int s_DefaultFFTSize = 256;

	struct Settings
	{
		float Threshold = -6.f;                            // EV
		float Intensity = 0.05f;                           // Bloom strength
		int FFTResolution = s_DefaultFFTSize;              // 128, 256, or 512
		int KernelType = 0;                                // 0=Circular, 1=Star, 2=Anamorphic
		float KernelRadius = 0.3f;                         // Kernel size relative to FFT dimension
		float KernelFalloff = 3.0f;                        // Falloff rate
		int StarPoints = 6;                                // Number of star points (4-12)
		float StarSharpness = 2.0f;                        // Ray sharpness
		float StarRotation = 15.0f;                        // Rotation in degrees
		float AnamorphicRatio = 0.5f;                      // 0=circular, 1=fully horizontal
		std::array<float, 3> Tint = { 1.0f, 1.0f, 1.0f };  // Bloom color tint
	} settings;

	struct alignas(16) FFTBloomCB
	{
		float Threshold;
		float Intensity;
		int Channel;  // 0=R, 1=G, 2=B
		int FFTSize;

		int KernelType;
		float KernelRadius;
		float KernelFalloff;
		int StarPoints;

		float StarSharpness;
		float StarRotation;
		float AnamorphicRatio;
		float _pad0;

		float Tint[4];  // rgb + padding

		uint32_t SourceDimsX;
		uint32_t SourceDimsY;
		uint32_t _pad1[2];
	};

	std::unique_ptr<ConstantBuffer> fftCB = nullptr;
	winrt::com_ptr<ID3D11SamplerState> linearSampler = nullptr;

	// Working textures at FFT resolution
	std::unique_ptr<Texture2D> texBrightPass = nullptr;   // R16G16B16A16_FLOAT, FFT_SIZE^2
	std::unique_ptr<Texture2D> texWork[2];                // R32G32_FLOAT (complex), FFT_SIZE^2
	std::unique_ptr<Texture2D> texBloomResult = nullptr;  // R16G16B16A16_FLOAT, FFT_SIZE^2
	std::unique_ptr<Texture2D> texKernelFFT = nullptr;    // R32G32_FLOAT (complex kernel spectrum)

	// Full-resolution output texture
	std::unique_ptr<Texture2D> texOutput = nullptr;  // Same format as source

	// Compute shaders
	winrt::com_ptr<ID3D11ComputeShader> thresholdDownsampleCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prepareChannelCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> fftHorizontalCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> fftVerticalCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> ifftHorizontalCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> ifftVerticalCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> kernelGenCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> multiplyCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> storeChannelCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> compositeCS = nullptr;

	// Track kernel state for lazy recomputation
	bool kernelDirty = true;
	int lastKernelType = -1;
	float lastKernelRadius = -1;
	float lastKernelFalloff = -1;
	int lastStarPoints = -1;
	float lastStarSharpness = -1;
	float lastStarRotation = -1;
	float lastAnamorphicRatio = -1;

	// Track compiled FFT size for shader recompilation
	int compiledFFTSize = 0;

	bool IsKernelDirty() const;
	void MarkKernelClean();

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
	void CreateFFTTextures();
	void UpdateKernelFFT();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;

	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;
};
