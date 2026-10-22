// Drawcall Limit Fix: the local shadow lights' screen-space masks for DCLF's receivers, resolved into their channels of kSHADOW_MASK
// from the main depth at the colour segment's start (dclf-architecture.md, "Local shadow masks"). The engine draws a local light's
// mask per registered receiver (render modes 0x10/0x11, GetRenderPasses_ShadowMapOrMask), and DCLF's receivers are not registered;
// this is that mask for every pixel, as the Utility shader's RENDER_DEPTH mask variants reconstruct it: the position from the depth
// through the frame's inverse view-projection, the map lookup LightLimitFix's (LocalShadowProjection.hlsli), and the distance fade.
// A channel without a record keeps what the engine drew. Compiled to SPIR-V at runtime (ComputeProgram) with BasicRHI's
// descriptor-heap ABI.

#include "Common/Math.hlsli"
#include "Common/Random.hlsli"

namespace LocalShadow
{
#include "LightLimitFix/LocalShadowProjection.hlsli"
}

// Must match LocalShadowMaskConstants (Core/Render/Resources/GpuLayouts.h).
cbuffer LocalShadowMaskConstants : register(b0)
{
	uint LatchIndex;   // ByteAddressBuffer: the latch block
	uint LatchRegion;  // set at record time: the frame slot's region, whose BuildDrawsLatch is the colour segment's
	uint MaskIndex;    // RWTexture2D: kSHADOW_MASK
	uint DepthIndex;   // Texture2D<float>: the main depth
	uint ShadowMap0Index;  // Texture2DArray<float>: kSHADOWMAPS_ESRAM (scalars: a cbuffer array pads each element to 16 bytes)
	uint ShadowMap1Index;  // Texture2DArray<float>: kSHADOWMAPS
	uint FrameBlockLo;  // the frame constants' PS b12 (Community Shaders' FrameBuffer), by address
	uint FrameBlockHi;
	uint Width;  // the mask's size
	uint Height;
}

// BuildDrawsLatch::localShadowMaskOffset; the region's records (GpuLocalShadowMask: 112 bytes, after a 16-byte header).
static const uint kLatchLocalShadowMask = 344;
static const uint kMaskRecordBytes = 112;
static const uint kMaskChannels = 4;

// FrameBuffer.hlsli's registers: CameraViewProjInverse (c32-c35, row-major), DynamicResolutionParams2 (c44).
float4 FrameRegister(uint a_register)
{
	const uint64_t address = (uint64_t(FrameBlockHi) << 32) | FrameBlockLo;
	return vk::RawBufferLoad<float4>(address + a_register * 16);
}

// SampleCmpLevelZero's bilinear compare (lit where the map is not nearer), by loads: no sampler is bound.
float ShadowTap(Texture2DArray<float> a_map, uint2 a_size, uint a_slice, float2 a_uv, float a_receiverDepth)
{
	const float2 position = a_uv * float2(a_size) - 0.5;
	const int2 base = int2(floor(position));
	const float2 weight = frac(position);
	float lit[4];
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		const int2 texel = clamp(base + int2(i & 1, i >> 1), 0, int2(a_size) - 1);
		lit[i] = a_map.Load(int4(texel, a_slice, 0)) >= a_receiverDepth ? 1.0 : 0.0;
	}
	return lerp(lerp(lit[0], lit[1], weight.x), lerp(lit[2], lit[3], weight.x), weight.y);
}

// The Utility shader's eight-tap rotated PCF (SampleShadowPCF, SampleDualParaboloidShadowPCF), the taps kept inside the lookup's window.
float ShadowPCF(Texture2DArray<float> a_map, uint2 a_size, uint a_slice, LocalShadow::LocalShadowLookup a_lookup, float a_radius, float a_noise)
{
	if (a_radius <= 0.0)
		return ShadowTap(a_map, a_size, a_slice, clamp(a_lookup.uv, a_lookup.clampMin, a_lookup.clampMax), a_lookup.receiverDepth);
	float2 rotation;
	sincos(Math::TAU * a_noise, rotation.y, rotation.x);
	const float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
	float sum = 0.0;
	[unroll] for (uint i = 0; i < 8; ++i)
	{
		const float2 offset = mul(Random::SpiralSampleOffset8(i), rotationMatrix) * a_radius;
		sum += ShadowTap(a_map, a_size, a_slice, clamp(a_lookup.uv + offset, a_lookup.clampMin, a_lookup.clampMax), a_lookup.receiverDepth);
	}
	return sum / 8.0;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (dispatchID.x >= Width || dispatchID.y >= Height)
		return;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint region = latch.Load(LatchRegion + kLatchLocalShadowMask);
	if (region == 0 || latch.Load(region) == 0)
		return;  // no light's mask this frame: every channel the engine's

	// The position, as RENDER_DEPTH's: the depth at the mask texel, through the inverse view-projection, relative to the eye.
	const float2 uv = (float2(dispatchID.xy) + 0.5) / float2(Width, Height);
	Texture2D<float> depthTexture = ResourceDescriptorHeap[DepthIndex];
	uint2 depthSize;
	depthTexture.GetDimensions(depthSize.x, depthSize.y);
	const float depth = depthTexture.Load(int3(min(uint2(uv * float2(depthSize)), depthSize - 1), 0));
	const float2 clipUv = uv * FrameRegister(44).xy;
	const float4 positionCS = float4(2.0 * float2(clipUv.x, 1.0 - clipUv.y) - 1.0, depth, 1.0);
	float4 positionMS = float4(dot(FrameRegister(32), positionCS), dot(FrameRegister(33), positionCS), dot(FrameRegister(34), positionCS),
		dot(FrameRegister(35), positionCS));
	positionMS.xyz /= positionMS.w;
	const float noise = Random::InterleavedGradientNoise(float2(dispatchID.xy));

	RWTexture2D<float4> mask = ResourceDescriptorHeap[MaskIndex];  // read and written: StorageImageReadWithoutFormat
	float4 channels = mask[dispatchID.xy];
	[unroll] for (uint c = 0; c < kMaskChannels; ++c)
	{
		const uint record = region + 16 + c * kMaskRecordBytes;
		const uint4 head = latch.Load4(record + 96);  // enabled, depth target, slice
		if (head.x == 0)
			continue;
		LocalShadow::LocalShadowData data;
		// The record's transform is row-major for row vectors; ShadowProj's mul(M, v) takes its transpose.
		const float4x4 rows = float4x4(asfloat(latch.Load4(record)), asfloat(latch.Load4(record + 16)), asfloat(latch.Load4(record + 32)),
			asfloat(latch.Load4(record + 48)));
		data.ShadowProj = transpose(rows);
		data.Params = asfloat(latch.Load4(record + 64));
		const float4 params2 = asfloat(latch.Load4(record + 80));
		data.Params2 = float4(params2.x, 0.0, 0.0, 0.0);
		data.Origin = 0.0;
		Texture2DArray<float> map = ResourceDescriptorHeap[(head.y & 1) ? ShadowMap1Index : ShadowMap0Index];
		uint3 mapSize;
		map.GetDimensions(mapSize.x, mapSize.y, mapSize.z);
		const LocalShadow::LocalShadowLookup lookup = LocalShadow::GetLocalShadowLookup(data, positionMS.xyz, 1.0 / float(mapSize.x));
		const float visibility = lookup.sampled ? ShadowPCF(map, mapSize.xy, head.z, lookup, params2.w, noise) * lookup.scale : lookup.unsampled;
		// The mask's distance fade (ShadowLightParam.z: the fade distance squared).
		const float fade = params2.z > 0.0 ? 1.0 - pow(saturate(dot(positionMS.xyz, positionMS.xyz) / params2.z), 8) : 1.0;
		channels[c] = fade * visibility;
	}
	mask[dispatchID.xy] = channels;
}
