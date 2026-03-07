/// Convolution Bloom - FFT-based bloom with arbitrary kernel shapes
/// Pipeline: Downscale -> FFT -> Multiply Kernel -> IFFT -> Composite
///
/// Entry points (selected via CompileShader entry point):
///   CS_FFT           - Forward/inverse FFT pass (direction via CB flags)
///   CS_ResizeKernel  - Center and resize kernel to FFT domain
///   CS_Multiply      - Frequency domain convolution multiply
///   CS_Composite     - Blend bloom result with original scene
///
/// Required defines (set by C++ at compile time):
///   SCAN_LINE_LENGTH - FFT signal length (power of 2, e.g. 512, 1024, 2048)
///   RADIX           - FFT radix (8 recommended)

#include "fft_2d.hlsli"
#include "fft_core.hlsli"

// --- Resources ---

Texture2D<float4> InputTex : register(t0);
Texture2D<float4> KernelTex : register(t1);
SamplerState BilinearSampler : register(s0);
RWTexture2D<float4> OutputTex : register(u0);

// --- Constant Buffer ---
// Packed to 16-byte alignment rows

cbuffer ConvBloomCB : register(b1)
{
	uint4 SrcRect;             // c0: source window (min.xy, max.xy)
	uint2 DstExtent;           // c1.xy: destination buffer dimensions
	uint TransformType;        // c1.z: bit flags (1=horizontal, 2=forward, 4=prefilter)
	float BloomIntensity;      // c1.w: bloom intensity multiplier
	float3 PreFilterParams;    // c2.xyz: (minLuma, maxLuma, multiplier)
	float KernelSupportScale;  // c2.w: fraction of screen the kernel covers (0-1)
	float2 KernelCenterUV;     // c3.xy: UV of kernel's brightest pixel
	float2 KernelTexSize;      // c3.zw: kernel texture dimensions
	float KernelDCEnergy;      // c4.x: DC component magnitude for normalization
	float2 BloomUVScale;       // c4.yz: (scaledW/fftDim, scaledH/fftDim) for composite UV
	uint Padding0;             // c4.w
};

// --- Data Copy Helpers ---
// Copies between textures and local FFT registers
// LocalBuffer[0] = .xy channels (R+iG), LocalBuffer[1] = .zw channels (B+iA)

void LoadFromTextureWindowed(inout Complex LocalBuffer[2][RADIX],
	bool bIsHorizontal, uint ScanIdx, uint Head, uint Stride, uint4 Window)
{
	[unroll] for (uint i = 0; i < RADIX; ++i)
	{
		LocalBuffer[0][i] = 0;
		LocalBuffer[1][i] = 0;
	}

	if (bIsHorizontal) {
		uint2 Pixel = uint2(Head, ScanIdx);
		[unroll] for (uint i = 0; i < RADIX; ++i, Pixel.x += Stride)
		{
			if (all(Pixel >= Window.xy) && all(Pixel < Window.zw)) {
				float4 Val = InputTex[Pixel];
				LocalBuffer[0][i] = Val.xy;
				LocalBuffer[1][i] = Val.zw;
			}
		}
	} else {
		uint2 Pixel = uint2(ScanIdx, Head);
		[unroll] for (uint i = 0; i < RADIX; ++i, Pixel.y += Stride)
		{
			if (all(Pixel >= Window.xy) && all(Pixel < Window.zw)) {
				float4 Val = InputTex[Pixel];
				LocalBuffer[0][i] = Val.xy;
				LocalBuffer[1][i] = Val.zw;
			}
		}
	}
}

void LoadFromTextureDirect(inout Complex LocalBuffer[2][RADIX],
	bool bIsHorizontal, uint ScanIdx, uint Head, uint Stride)
{
	if (bIsHorizontal) {
		uint2 Pixel = uint2(Head, ScanIdx);
		[unroll] for (uint i = 0; i < RADIX; ++i, Pixel.x += Stride)
		{
			float4 Val = InputTex[Pixel];
			LocalBuffer[0][i] = Val.xy;
			LocalBuffer[1][i] = Val.zw;
		}
	} else {
		uint2 Pixel = uint2(ScanIdx, Head);
		[unroll] for (uint i = 0; i < RADIX; ++i, Pixel.y += Stride)
		{
			float4 Val = InputTex[Pixel];
			LocalBuffer[0][i] = Val.xy;
			LocalBuffer[1][i] = Val.zw;
		}
	}
}

void StoreToTexture(in Complex LocalBuffer[2][RADIX],
	bool bIsHorizontal, uint ScanIdx, uint Head, uint Stride, uint2 Extent)
{
	if (bIsHorizontal) {
		uint2 Pixel = uint2(Head, ScanIdx);
		[unroll] for (uint r = 0; r < RADIX; ++r, Pixel.x += Stride)
		{
			if (Pixel.x < Extent.x && Pixel.y < Extent.y)
				OutputTex[Pixel] = float4(LocalBuffer[0][r], LocalBuffer[1][r]);
		}
	} else {
		uint2 Pixel = uint2(ScanIdx, Head);
		[unroll] for (uint r = 0; r < RADIX; ++r, Pixel.y += Stride)
		{
			if (Pixel.x < Extent.x && Pixel.y < Extent.y)
				OutputTex[Pixel] = float4(LocalBuffer[0][r], LocalBuffer[1][r]);
		}
	}
}

// ==========================================================================
// CS_FFT - Forward or inverse 1D FFT along one dimension
// TransformType flags: bit0 = horizontal, bit1 = forward, bit2 = prefilter
// ==========================================================================

[numthreads(NUMTHREADSX, 1, 1)] void CS_FFT(uint3 GroupID : SV_GroupID, uint3 GroupThreadID : SV_GroupThreadID) {
	bool bIsHorizontal = (TransformType & 1u) != 0;
	bool bIsForward = (TransformType & 2u) != 0;
	bool bDoPreFilter = (TransformType & 4u) != 0;

	uint ThreadIdx = GroupThreadID.x;
	uint ScanIdx = GroupID.z;
	uint Head = ThreadIdx;

	Complex LocalBuffer[2][RADIX];

	if (bIsForward) {
		LoadFromTextureWindowed(LocalBuffer, bIsHorizontal, ScanIdx, Head, STRIDE, SrcRect);
		ScrubNANs(LocalBuffer);
		if (bDoPreFilter)
			FilterBrightPixels(PreFilterParams, LocalBuffer);
	} else {
		LoadFromTextureDirect(LocalBuffer, bIsHorizontal, ScanIdx, Head, STRIDE);
	}

	// Perform FFT on both complex channels
	GroupSharedFFT(bIsForward, LocalBuffer, SCAN_LINE_LENGTH, ThreadIdx);

	// Apply 1/N scale on inverse transform
	if (!bIsForward) {
		float InvN = 1.0 / float(SCAN_LINE_LENGTH);
		Scale(LocalBuffer, InvN);
		ScrubNANs(LocalBuffer);
	}

	StoreToTexture(LocalBuffer, bIsHorizontal, ScanIdx, Head, STRIDE, DstExtent);
}

	// ==========================================================================
	// CS_ResizeKernel - Resize kernel to FFT dimensions with center at origin
	// Maps kernel so its brightest pixel (center) lands at output pixel (0,0)
	// Uses periodic extension (frac) for circular convolution
	// Converts RGB kernel to grayscale luminance, packed as dual complex channels
	// ==========================================================================

	[numthreads(8, 8, 1)] void CS_ResizeKernel(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= DstExtent.x || DTid.y >= DstExtent.y)
		return;

	// Kernel covers KernelSupportScale fraction along the major axis
	float MajorAxis = (float)max(DstExtent.x, DstExtent.y);
	float KernelPixelSize = max(MajorAxis * KernelSupportScale, 1.0);

	// Map output pixel to kernel UV space
	// frac() provides periodic wrapping (essential for FFT circular convolution)
	float2 PixelUV = float2(DTid.xy) / KernelPixelSize;
	float2 UV = frac(PixelUV + KernelCenterUV);

	// Sample kernel with bilinear filtering
	float4 KernelColor = KernelTex.SampleLevel(BilinearSampler, UV, 0);

	// Convert to grayscale luminance
	float Lum = ConvertToLuma(KernelColor.rgb);

	// Pack as dual identical complex channels: (Lum+0i, Lum+0i)
	OutputTex[DTid.xy] = float4(Lum, 0, Lum, 0);
}

// ==========================================================================
// CS_Multiply - Frequency domain multiply (image spectrum * kernel spectrum)
// Both inputs are complex-packed float4: .xy = channel0, .zw = channel1
// Normalizes by kernel DC energy to preserve average brightness
// ==========================================================================

[numthreads(8, 8, 1)] void CS_Multiply(uint3 DTid : SV_DispatchThreadID) {
	if (DTid.x >= DstExtent.x || DTid.y >= DstExtent.y)
		return;

	float4 ImageVal = InputTex[DTid.xy];
	float4 KernelVal = KernelTex[DTid.xy];

	// Normalize by kernel DC component (total energy)
	float NormFactor = 1.0 / max(KernelDCEnergy, 0.0001);

	// Complex multiply per channel pair, then normalize
	Complex Prod0 = ComplexMult(ImageVal.xy, KernelVal.xy) * NormFactor;
	Complex Prod1 = ComplexMult(ImageVal.zw, KernelVal.zw) * NormFactor;

	OutputTex[DTid.xy] = float4(Prod0, Prod1);
}

	// ==========================================================================
	// CS_Downsample - Bilinear downsample from full-res scene to FFT buffer scale
	// InputTex (t0) = full-res scene, OutputTex (u0) = downscaled target
	// DstExtent controls the output region (may be smaller than OutputTex)
	// ==========================================================================

	[numthreads(8, 8, 1)] void CS_Downsample(uint3 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= DstExtent.x || DTid.y >= DstExtent.y)
		return;

	float2 UV = (float2(DTid.xy) + 0.5) / float2(DstExtent);
	OutputTex[DTid.xy] = InputTex.SampleLevel(BilinearSampler, UV, 0);
}

// ==========================================================================
// CS_Composite - Additive blend of bloom result with original scene
// InputTex (t0) = original scene, KernelTex (t1) = bloom result (FFT buffer)
// BloomUVScale maps screen UV to the valid region of the FFT buffer
// ==========================================================================

[numthreads(8, 8, 1)] void CS_Composite(uint3 DTid : SV_DispatchThreadID) {
	uint2 OutputDim;
	OutputTex.GetDimensions(OutputDim.x, OutputDim.y);
	if (DTid.x >= OutputDim.x || DTid.y >= OutputDim.y)
		return;

	float4 Original = InputTex[DTid.xy];

	// Map screen UV to FFT buffer UV (bloom occupies the top-left portion)
	float2 ScreenUV = (float2(DTid.xy) + 0.5) / float2(OutputDim);
	float2 BloomUV = ScreenUV * BloomUVScale;
	float4 BloomVal = KernelTex.SampleLevel(BilinearSampler, BloomUV, 0);

	// Extract real parts from complex packing: R=.x, G=.y, B=.z, A=.w
	float3 BloomColor = max(BloomVal.xyz, 0);

	OutputTex[DTid.xy] = float4(Original.rgb + BloomColor * BloomIntensity, Original.a);
}
