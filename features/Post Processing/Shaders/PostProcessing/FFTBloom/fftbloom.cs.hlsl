/// FFT Convolution Bloom
/// Bloom effect using FFT-based convolution with customizable kernel shapes.
/// Supports circular, starburst, and anamorphic bloom patterns.

#include "PostProcessing/common.hlsli"

#ifndef FFT_SIZE
#	define FFT_SIZE 256
#endif

#ifndef LOG2_FFT_SIZE
#	define LOG2_FFT_SIZE 8
#endif

static const float PI = 3.14159265358979323846;

Texture2D<float4> TexColor : register(t0);
Texture2D<float2> TexComplexIn : register(t1);

RWTexture2D<float4> RWTexOut : register(u0);
RWTexture2D<float2> RWTexComplexOut : register(u1);

SamplerState SampLinear : register(s0);

cbuffer FFTBloomCB : register(b1)
{
	float Threshold : packoffset(c0.x);
	float Intensity : packoffset(c0.y);
	int Channel : packoffset(c0.z);
	int FFTSizeCB : packoffset(c0.w);

	int KernelType : packoffset(c1.x);
	float KernelRadius : packoffset(c1.y);
	float KernelFalloff : packoffset(c1.z);
	int StarPoints : packoffset(c1.w);

	float StarSharpness : packoffset(c2.x);
	float StarRotation : packoffset(c2.y);
	float AnamorphicRatio : packoffset(c2.z);
	float _pad0 : packoffset(c2.w);

	float4 Tint : packoffset(c3);

	uint2 SourceDims : packoffset(c4.x);
	uint2 _pad1 : packoffset(c4.z);
};

// ============================================================================
// Complex number utilities
// ============================================================================

float2 complexMul(float2 a, float2 b)
{
	return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

float2 complexConj(float2 a)
{
	return float2(a.x, -a.y);
}

// ============================================================================
// Bit reversal for FFT input reordering
// ============================================================================

uint bitReverse(uint x, uint bits)
{
	return reversebits(x) >> (32u - bits);
}

// ============================================================================
// NaN/negative sanitization (shared with COD Bloom)
// ============================================================================

bool3 IsNaN(float3 x)
{
	return !(x < 0.f || x > 0.f || x == 0.f);
}

float3 Sanitise(float3 v)
{
	bool3 err = IsNaN(v) || (v < 0);
	v.x = err.x ? 0 : v.x;
	v.y = err.y ? 0 : v.y;
	v.z = err.z ? 0 : v.z;
	return v;
}

float3 ThresholdColor(float3 col, float threshold)
{
	float luma = Color::RGBToLuminance(col);
	if (luma < 1e-3)
		return 0;
	return col * (max(0, luma - threshold) / luma);
}

// ============================================================================
// CS_ThresholdAndDownsample
// Extracts bright pixels and downsamples to FFT resolution
// ============================================================================

[numthreads(32, 32, 1)] void CS_ThresholdAndDownsample(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= uint2(FFTSizeCB, FFTSizeCB)))
		return;

	float2 uv = (tid + 0.5) / float2(FFTSizeCB, FFTSizeCB);

	// Sample source with bilinear filtering
	float3 col = TexColor.SampleLevel(SampLinear, uv, 0).rgb;
	col = Sanitise(col);
	col = ThresholdColor(col, Threshold);

	RWTexOut[tid] = float4(col, 1);
}

	// ============================================================================
	// CS_PrepareChannel
	// Extracts one color channel from bright pass into complex format
	// ============================================================================

	[numthreads(32, 32, 1)] void CS_PrepareChannel(uint2 tid : SV_DispatchThreadID)
{
	if (any(tid >= uint2(FFTSizeCB, FFTSizeCB)))
		return;

	float4 color = TexColor[tid];
	float val = 0;

	if (Channel == 0)
		val = color.r;
	else if (Channel == 1)
		val = color.g;
	else
		val = color.b;

	RWTexComplexOut[tid] = float2(val, 0);
}

// ============================================================================
// CS_StoreChannel
// Stores the real part of complex result into a specific channel
// ============================================================================

[numthreads(32, 32, 1)] void CS_StoreChannel(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= uint2(FFTSizeCB, FFTSizeCB)))
		return;

	float val = max(0, TexComplexIn[tid].x);

	float4 current = RWTexOut[tid];
	if (Channel == 0)
		current.r = val;
	else if (Channel == 1)
		current.g = val;
	else
		current.b = val;
	current.a = 1;

	RWTexOut[tid] = current;
}

// ============================================================================
// Shared memory FFT (Cooley-Tukey, radix-2, decimation-in-time)
// Uses bit-reversed input order, natural output order
// ============================================================================

groupshared float2 fftBuffer[2][FFT_SIZE];

void FFTSharedMemory(uint tid, bool inverse)
{
	uint pingpong = 0;

	for (uint s = 0; s < (uint)LOG2_FFT_SIZE; s++) {
		uint dst = 1 - pingpong;
		uint m = 2u << s;
		uint halfm = 1u << s;

		uint group = tid / halfm;
		uint j = tid % halfm;

		uint idx0 = group * m + j;
		uint idx1 = idx0 + halfm;

		float angle = -2.0 * PI * float(j) / float(m);
		if (inverse)
			angle = -angle;

		float2 w = float2(cos(angle), sin(angle));

		float2 even = fftBuffer[pingpong][idx0];
		float2 odd = fftBuffer[pingpong][idx1];
		float2 t = complexMul(odd, w);

		GroupMemoryBarrierWithGroupSync();

		if (tid < (uint)FFT_SIZE / 2u) {
			fftBuffer[dst][idx0] = even + t;
			fftBuffer[dst][idx1] = even - t;
		}

		pingpong = dst;
		GroupMemoryBarrierWithGroupSync();
	}

	// Copy result to buffer[0] for output (if final ping-pong isn't 0)
	if (pingpong != 0) {
		fftBuffer[0][tid] = fftBuffer[1][tid];
		GroupMemoryBarrierWithGroupSync();
	}
}

// ============================================================================
// CS_FFT_Horizontal / CS_FFT_Vertical
// Forward FFT along rows or columns using shared memory
// Dispatch(FFT_SIZE, 1, 1) - each group processes one row/column
// ============================================================================

[numthreads(FFT_SIZE, 1, 1)] void CS_FFT_Horizontal(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
	uint row = gid.x;
	uint rev = bitReverse(tid, (uint)LOG2_FFT_SIZE);

	// Load bit-reversed input
	fftBuffer[0][tid] = TexComplexIn[uint2(rev, row)];
	GroupMemoryBarrierWithGroupSync();

	FFTSharedMemory(tid, false);

	RWTexComplexOut[uint2(tid, row)] = fftBuffer[0][tid];
}

	[numthreads(FFT_SIZE, 1, 1)] void CS_FFT_Vertical(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
	uint col = gid.x;
	uint rev = bitReverse(tid, (uint)LOG2_FFT_SIZE);

	// Load bit-reversed input (column-wise)
	fftBuffer[0][tid] = TexComplexIn[uint2(col, rev)];
	GroupMemoryBarrierWithGroupSync();

	FFTSharedMemory(tid, false);

	RWTexComplexOut[uint2(col, tid)] = fftBuffer[0][tid];
}

// ============================================================================
// CS_IFFT_Horizontal / CS_IFFT_Vertical
// Inverse FFT along rows or columns using shared memory
// ============================================================================

[numthreads(FFT_SIZE, 1, 1)] void CS_IFFT_Horizontal(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
	uint row = gid.x;
	uint rev = bitReverse(tid, (uint)LOG2_FFT_SIZE);

	fftBuffer[0][tid] = TexComplexIn[uint2(rev, row)];
	GroupMemoryBarrierWithGroupSync();

	FFTSharedMemory(tid, true);

	float2 result = fftBuffer[0][tid] / float(FFT_SIZE);
	RWTexComplexOut[uint2(tid, row)] = result;
}

	[numthreads(FFT_SIZE, 1, 1)] void CS_IFFT_Vertical(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
	uint col = gid.x;
	uint rev = bitReverse(tid, (uint)LOG2_FFT_SIZE);

	fftBuffer[0][tid] = TexComplexIn[uint2(col, rev)];
	GroupMemoryBarrierWithGroupSync();

	FFTSharedMemory(tid, true);

	float2 result = fftBuffer[0][tid] / float(FFT_SIZE);
	RWTexComplexOut[uint2(col, tid)] = result;
}

// ============================================================================
// CS_GenerateKernel
// Generates the bloom kernel in spatial domain
// Kernel is centered at (0,0) for proper FFT convolution (wrap-around)
// ============================================================================

[numthreads(32, 32, 1)] void CS_GenerateKernel(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= uint2(FFTSizeCB, FFTSizeCB)))
		return;

	float N = float(FFTSizeCB);

	// Map coordinates so that (0,0) is at the corner (for wrap-around FFT)
	// Kernel center is at pixel (0,0), wraps around edges
	float2 pos = tid;
	if (pos.x > N * 0.5)
		pos.x -= N;
	if (pos.y > N * 0.5)
		pos.y -= N;

	// Normalize to [-0.5, 0.5]
	pos /= N;

	float value = 0;
	float radius = length(pos);

	float sigma = max(KernelRadius * 0.2, 0.001);

	if (KernelType == 0) {
		// Circular (Gaussian)
		value = exp(-radius * radius / (2.0 * sigma * sigma));
	} else if (KernelType == 1) {
		// Star/Starburst
		float angle = atan2(pos.y, pos.x) + StarRotation * PI / 180.0;
		float starPattern = pow(max(abs(cos(angle * float(StarPoints) * 0.5)), 0.001), StarSharpness);
		float falloff = exp(-radius / max(sigma, 0.001));
		value = starPattern * falloff;
	} else if (KernelType == 2) {
		// Anamorphic (stretched Gaussian)
		float2 stretch = float2(lerp(1.0, 0.1, AnamorphicRatio), lerp(1.0, 2.0, AnamorphicRatio));
		float2 stretchedPos = pos * stretch;
		float stretchedRadius = length(stretchedPos);
		value = exp(-stretchedRadius * stretchedRadius / (2.0 * sigma * sigma));
	}

	// Ensure kernel is non-negative
	value = max(value, 0);

	// Store as complex (real part only)
	RWTexComplexOut[tid] = float2(value, 0);
}

// ============================================================================
// CS_NormalizeKernel
// Normalizes the kernel so its sum equals 1 (energy conservation)
// Reads the sum from a pre-computed value in the constant buffer
// For simplicity, we normalize during the multiply step instead
// ============================================================================

// ============================================================================
// CS_Multiply
// Point-wise complex multiplication in frequency domain
// Input: FFT of image (t1) * FFT of kernel (t0/u0 dual-bind)
// ============================================================================

Texture2D<float2> TexKernelFFT : register(t2);

[numthreads(32, 32, 1)] void CS_Multiply(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= uint2(FFTSizeCB, FFTSizeCB)))
		return;

	float2 imageFreq = TexComplexIn[tid];
	float2 kernelFreq = TexKernelFFT[tid];

	float kernelSum = TexKernelFFT[uint2(0, 0)].x;
	kernelFreq /= max(kernelSum, 1e-6);

	float2 result = complexMul(imageFreq, kernelFreq);

	RWTexComplexOut[tid] = result;
}

// ============================================================================
// CS_Composite
// Upsamples FFT bloom result and blends with original image
// ============================================================================

Texture2D<float4> TexBloomResult : register(t3);

[numthreads(32, 32, 1)] void CS_Composite(uint2 tid : SV_DispatchThreadID) {
	uint2 dims;
	RWTexOut.GetDimensions(dims.x, dims.y);

	if (any(tid >= dims))
		return;

	float2 uv = (tid + 0.5) / float2(dims);

	float3 original = TexColor[tid].rgb;
	float3 bloom = TexBloomResult.SampleLevel(SampLinear, uv, 0).rgb;

	// Apply tint
	bloom *= Tint.rgb;

	// Composite
	float3 result = original + bloom * Intensity;

	RWTexOut[tid] = float4(result, 1);
}
