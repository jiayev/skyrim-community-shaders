#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#include "Common/Game.hlsli"
#include "LightLimitFix/Attenuation.hlsli"
#include "Common/NamespacedCBuffer.hlsli"
#include "Common/DCLFObjects.hlsli"

namespace LightLimitFix
{

#include "LightLimitFix/Common.hlsli"

	cbuffer StrictLightData : register(b3)
	{
		uint NSCB(LightLimitFix, NumStrictLights);
		int NSCB(LightLimitFix, RoomIndex);
		uint NSCB(LightLimitFix, ShadowBitMask);
		uint NSCB(LightLimitFix, FirstPerson);
		float4 NSCB(LightLimitFix, WorldEyePosition);
		Light NSCB(LightLimitFix, StrictLights)[15];
	};
	NSCB_ALIAS(LightLimitFix, uint, NumStrictLights)
#if defined(DCLF_BINDLESS_DRAW)
	// Drawcall Limit Fix supplies these two per object from its own record, so that the StrictLightData
	// buffer stops being what makes a draw's binding record unique - DCLF writes nothing else into it, and
	// with NumStrictLights at 0 the StrictLights tail is never read. The cbuffer above keeps its
	// declaration and its layout: only the names change source, and NSCB_ALIAS is already exactly this
	// shape of static under DXC. Every other includer of this header compiles the aliases unchanged.
	// Read where used, not as statics: a plain draw's object word (DCLF_PULLED) is its sequence's, which its main loads after the
	// statics are initialised.
	int RoomIndexOf() { return DCLFObjects[DCLFObjectIndex].RoomIndex; }
	// The shadow lights are the draw's, selected by BuildDrawsCS this frame (the object word), not the record's.
	uint ShadowBitMaskOf() { return DCLFLocalShadowMask; }
#else
	NSCB_ALIAS(LightLimitFix, int, RoomIndex)
	NSCB_ALIAS(LightLimitFix, uint, ShadowBitMask)
	int RoomIndexOf() { return RoomIndex; }
	uint ShadowBitMaskOf() { return ShadowBitMask; }
#endif  // DCLF_BINDLESS_DRAW
	NSCB_ALIAS(LightLimitFix, uint, FirstPerson)
	NSCB_ALIAS(LightLimitFix, float4, WorldEyePosition)
	NSCB_ARRAY_ALIAS(LightLimitFix, Light, StrictLights, 15)

	StructuredBuffer<Light> lights : register(t35);
	StructuredBuffer<uint> lightList : register(t36);       //MAX_CLUSTER_LIGHTS * 16^3
	StructuredBuffer<LightGrid> lightGrid : register(t37);  //16^3

#include "LightLimitFix/LocalShadowProjection.hlsli"

	StructuredBuffer<LocalShadowData> LocalShadows : register(t102);
	Texture2DArray<float> LocalShadowMaps : register(t103);

	bool GetClusterIndex(in float2 uv, in float z, inout uint clusterIndex)
	{
		const uint3 clusterSize = SharedData::lightLimitFixSettings.ClusterSize.xyz;

		if (!FrameBuffer::FrameParams.y)  // Fix first person lights
			uv = 0.5;

		z = max(z, SharedData::CameraData.y);

		uint clusterZ = log(z / SharedData::CameraData.y) * clusterSize.z / log(SharedData::CameraData.x / SharedData::CameraData.y);
		uint3 cluster = uint3(uint2(uv * clusterSize.xy), clusterZ);

		// Bounds validation to prevent out-of-range cluster indices
		if (any(cluster >= clusterSize))
			return false;

		clusterIndex = cluster.x + (clusterSize.x * cluster.y) + (clusterSize.x * clusterSize.y * cluster.z);
		return true;
	}

	bool IsLightIgnored(Light light)
	{
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
			return !(ShadowBitMaskOf() & (1 << light.shadowLightIndex));
		}

		bool lightIgnored = false;
		const int roomIndexOfObject = RoomIndexOf();
		if ((light.lightFlags & LightFlags::PortalStrict) && roomIndexOfObject >= 0) {
			lightIgnored = true;
			int roomIndex = roomIndexOfObject;
			[unroll] for (int flagsIndex = 0; flagsIndex < 4; ++flagsIndex)
			{
				if (roomIndex < 32) {
					if (((light.roomFlags[flagsIndex] >> roomIndex) & 1) == 1) {
						lightIgnored = false;
					}
					break;
				}
				roomIndex -= 32;
			}
		}
		return lightIgnored;
	}

	float2x2 GetShadowRotationMatrix(float noise)
	{
		float angle = noise * Math::TAU;
		float sinAngle, cosAngle;
		sincos(angle, sinAngle, cosAngle);
		return float2x2(cosAngle, sinAngle, -sinAngle, cosAngle);
	}

	/** @brief Bilinear PCF tap equivalent to SampleCmpLevelZero; the host shaders have no comparison sampler bound, so this uses Gather. */
	float SampleLocalShadowTap(Texture2DArray<float> shadowMaps, SamplerState samp, uint slice, float2 uv, float receiverDepth, float texel)
	{
		float2 texelPosition = uv / texel - 0.5;
		float2 weight = frac(texelPosition);
		float4 lit = float4(shadowMaps.GatherRed(samp, float3((floor(texelPosition) + 1.0) * texel, slice)) >= receiverDepth);
		return lerp(lerp(lit.w, lit.z, weight.x), lerp(lit.x, lit.y, weight.x), weight.y);
	}

	float SampleLocalShadowPCF(Texture2DArray<float> shadowMaps, SamplerState samp, uint slice, float2 uv, float receiverDepth, float noise, float2 clampMin, float2 clampMax, float texel, uint sampleCount, float radiusUV)
	{
		float shadow = 0.0;
		[branch] if (sampleCount <= 1)
		{
			shadow = SampleLocalShadowTap(shadowMaps, samp, slice, clamp(uv, clampMin, clampMax), receiverDepth, texel);
		}
		else
		{
			const float2x2 rotationMatrix = GetShadowRotationMatrix(noise);
			const uint stride = sampleCount >= 8 ? 1 : 2;
			float sum = 0.0;
			[loop] for (uint i = 0; i < 8; i += stride)
			{
				float2 offset = mul(Random::SpiralSampleOffset8(i), rotationMatrix) * radiusUV;
				sum += SampleLocalShadowTap(shadowMaps, samp, slice, clamp(uv + offset, clampMin, clampMax), receiverDepth, texel);
			}
			shadow = sum * (stride == 1 ? 0.125 : 0.25);
		}
		return shadow;
	}

	// position is relative to eyePosition; each cached projection is relative to its own Origin.
	float GetLocalShadow(Texture2DArray<float> shadowMaps, SamplerState samp, LocalShadowData data, uint slice, float3 position, float3 eyePosition, float3 towardLight, bool skinnedReceiver, float noise, float texel, uint sampleCount, float radiusUV)
	{
		float rawShadow = 1.0;
		float fade = 0.0;

		[branch] if (data.Params.y > 0.0)
		{
			fade = data.Params.w;
			float3 shadowPosition = position + (eyePosition - data.Origin.xyz) + towardLight * (skinnedReceiver ? data.Params2.y : 0.0);
			const LocalShadowLookup lookup = GetLocalShadowLookup(data, shadowPosition, texel);
			rawShadow = lookup.sampled ? SampleLocalShadowPCF(shadowMaps, samp, slice, lookup.uv, lookup.receiverDepth, noise, lookup.clampMin, lookup.clampMax, texel,
											 sampleCount, radiusUV) * lookup.scale :
			                             lookup.unsampled;
		}

		return lerp(1.0, rawShadow, fade);
	}

	float GetLocalShadow(SamplerState samp, uint slice, float3 position, float3 eyePosition, float3 towardLight, bool skinnedReceiver, float noise)
	{
		return GetLocalShadow(LocalShadowMaps, samp, LocalShadows[slice], slice, position, eyePosition, towardLight, skinnedReceiver, noise,
			SharedData::lightLimitFixSettings.LocalShadowTexelSize, SharedData::lightLimitFixSettings.LocalShadowSamples, SharedData::lightLimitFixSettings.LocalShadowFilterRadius);
	}
}
