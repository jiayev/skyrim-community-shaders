#include "Common/ColorManagement.hlsli"
#include "Common/SparseRadiance.hlsli"

Texture2D<float3> Albedo : register(t0);
// The deferred reflectance target already contains the integrated specular BRDF,
// including layered materials. Applying EnvBRDFApprox again would double it.
Texture2D<float3> Reflectance : register(t1);
Texture2D<float3> NormalGlossiness : register(t2);
Texture2D<float> Depth : register(t3);
Texture2D<float> TracedHitDistance : register(t4);
Texture2D<float3> BeforeSSS : register(t5);
Texture2D<float3> AfterSSS : register(t6);
Texture2D<float3> Masks : register(t7);

RWTexture2D<float4> DiffuseAlbedo : register(u0);
RWTexture2D<float4> SpecularAlbedo : register(u1);
RWTexture2D<float4> NormalRoughness : register(u2);
RWTexture2D<float> HitDistance : register(u3);
RWTexture2D<float> SSSGuide : register(u4);

cbuffer GuideConstants : register(b1)
{
	uint SpecularEnabled;
	uint SpecularHalf;
	uint2 Padding;
};

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	uint2 p = tid.xy;
	uint2 size = (uint2)(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
	if (any(p >= size))
		return;
	bool surface = Depth[p] < 1.0 - 1e-6;
	float3 packed = NormalGlossiness[p];
	float3 normalVS = GBuffer::DecodeNormal(packed.xy);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse, float4(normalVS, 0)).xyz);
	DiffuseAlbedo[p] = float4(surface ? saturate(ColorManagement::SceneToLinear(Albedo[p] / Color::PBRLightingScale)) : 0, 1);
	SpecularAlbedo[p] = float4(surface ? max(0, Reflectance[p]) : 0, 1);
	NormalRoughness[p] = surface ? float4(normalWS, saturate(1.0 - packed.z)) : float4(0, 0, 1, 1);
	float distance = 0;
	if (surface && SpecularEnabled != 0) {
		uint2 source = SparseRadianceCoord(p, SpecularHalf, 1, Depth, NormalGlossiness);
		distance = TracedHitDistance[source];
	}
	HitDistance[p] = distance;
	// Both colors already include diffuse albedo and have identical exposure.
	// Keep the signed difference in the same pre-exposed domain as RR's color.
	float3 delta = ColorManagement::SceneToLinear(AfterSSS[p]) - ColorManagement::SceneToLinear(BeforeSSS[p]);
	SSSGuide[p] = surface && Masks[p].x > 0 ? dot(delta, float3(0.25, 0.5, 0.25)) : 0;
}
