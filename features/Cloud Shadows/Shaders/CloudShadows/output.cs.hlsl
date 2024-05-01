#include "../Common/DeferredShared.hlsli"
#include "../Common/VR.hlsli"

struct PerPassCloudShadow
{
	uint EnableCloudShadows;
	float CloudHeight;
	float PlanetRadius;
	float EffectMix;
	float TransparencyPower;
	float RcpHPlusR;
};

StructuredBuffer<PerPassCloudShadow> perPassCloudShadow : register(t0);
TextureCube<float4> cloudShadows : register(t1);
Texture2D<unorm half> TexDepth : register(t2);

RWTexture2D<unorm float> RWTexShadowMask : register(u0);

SamplerState defaultSampler;

float3 getCloudShadowSampleDir(float3 rel_pos, float3 eye_to_sun)
{
	float r = perPassCloudShadow[0].PlanetRadius;
	float3 p = (rel_pos + float3(0, 0, r)) * perPassCloudShadow[0].RcpHPlusR;
	float dotprod = dot(p, eye_to_sun);
	float lengthsqr = dot(p, p);
	float t = -dotprod + sqrt(dotprod * dotprod - dot(p, p) + 1);
	float3 v = (p + eye_to_sun * t) * (r + perPassCloudShadow[0].CloudHeight) - float3(0, 0, r);
	v = normalize(v);
	return v;
}

float3 getCloudShadowSampleDirFlatEarth(float3 rel_pos, float3 eye_to_sun)
{
	float3 p = rel_pos / perPassCloudShadow[0].CloudHeight;
	float dotprod = dot(p, eye_to_sun);
	float t = -dotprod + sqrt(dotprod * dotprod - dot(p, p) + 1);
	float3 v = p + eye_to_sun * t;
	v = normalize(v);  // optional
	return v;
}

float3 getCloudShadowMult(float3 rel_pos, float3 eye_to_sun)
{
	// float3 cloudSampleDir = getCloudShadowSampleDirFlatEarth(rel_pos, eye_to_sun).xyz;
	float3 cloudSampleDir = getCloudShadowSampleDir(rel_pos, eye_to_sun).xyz;

	float4 cloudCubeSample = cloudShadows.SampleLevel(defaultSampler, cloudSampleDir, 0);  // TODO Sample in pixel shader
	float alpha = pow(saturate(cloudCubeSample.w), perPassCloudShadow[0].TransparencyPower);

	return lerp(1.0, 1.0 - alpha, perPassCloudShadow[0].EffectMix);
}

[numthreads(32, 32, 1)] void main(uint2 dtid
								  : SV_DispatchThreadID) {
	float2 uv = (dtid + .5) * RcpBufferDim;
#ifdef VR
	const uint eyeIndex = uv > .5;
#else
	const uint eyeIndex = 0;
#endif

	float3 ndc = float3(ConvertToStereoUV(uv, eyeIndex), 1);
	ndc = ndc * 2 - 1;
	ndc.y = -ndc.y;
	ndc.z = TexDepth[dtid];

	if (ndc.z > 0.9999)
		return;

	float4 worldPos = mul(InvViewMatrix[eyeIndex], mul(InvProjMatrix[eyeIndex], float4(ndc, 1)));
	worldPos.xyz /= worldPos.w;

	float3 dirLightDirWS = mul((float3x3)InvViewMatrix[eyeIndex], DirLightDirectionVS[eyeIndex].xyz);
	float cloudShadow = getCloudShadowMult(worldPos.xyz, dirLightDirWS);

	half shadow = RWTexShadowMask[dtid];
	RWTexShadowMask[dtid] = shadow * cloudShadow;
}