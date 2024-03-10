#include "Common/Color.hlsl"
#include "Common/FrameBuffer.hlsl"
#include "Common/LightingData.hlsl"
#include "Common/MotionBlur.hlsl"

cbuffer PerFrame : register(b3)
{
	row_major float3x4 DirectionalAmbient;
	float4 DirLightColor;
	float4 DirLightDirection;
	float DirLightScale;
	bool ComplexAtlasTexture;
	bool EnableComplexTreeLOD;
	bool EnableDirLightFix;
	float SubsurfaceScatteringAmount;
	float pad[3];
}

struct VS_INPUT
{
	float3 Position : POSITION0;
	float2 TexCoord0 : TEXCOORD0;
	float4 InstanceData1 : TEXCOORD4;
	float4 InstanceData2 : TEXCOORD5;
	float4 InstanceData3 : TEXCOORD6;  // Unused
	float4 InstanceData4 : TEXCOORD7;  // Unused
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
	float3 TexCoord : TEXCOORD0;
#if defined(RENDER_DEPTH)
	float4 Depth : TEXCOORD3;
#else
	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
#endif
	float3 SphereNormal : TEXCOORD4;

	row_major float3x4 World : POSITION3;
};

#ifdef VSHADER
cbuffer PerTechnique : register(b0)
{
	float4 FogParam : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float4x4 World : packoffset(c4);
	row_major float4x4 PreviousWorld : packoffset(c8);
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float3 scaledModelPosition = input.InstanceData1.www * input.Position.xyz;
	float3 adjustedModelPosition = 0.0.xxx;
	adjustedModelPosition.x = dot(float2(1, -1) * input.InstanceData2.xy, scaledModelPosition.xy);
	adjustedModelPosition.y = dot(input.InstanceData2.yx, scaledModelPosition.xy);
	adjustedModelPosition.z = scaledModelPosition.z;

	float4 finalModelPosition = float4(input.InstanceData1.xyz + adjustedModelPosition.xyz, 1.0);
	float4 viewPosition = mul(WorldViewProj, finalModelPosition);

#	ifdef RENDER_DEPTH
	vsout.Depth.xy = viewPosition.zw;
	vsout.Depth.zw = input.InstanceData2.zw;
#	else
	vsout.WorldPosition = mul(World, finalModelPosition);
	vsout.PreviousWorldPosition = mul(PreviousWorld, finalModelPosition);
#	endif

	vsout.Position = viewPosition;
	vsout.TexCoord = float3(input.TexCoord0.xy, FogParam.z);

	scaledModelPosition = input.Position.xyz;
	adjustedModelPosition.x = dot(float2(1, -1) * input.InstanceData2.xy, scaledModelPosition.xy);
	adjustedModelPosition.y = dot(input.InstanceData2.yx, scaledModelPosition.xy);
	adjustedModelPosition.z = scaledModelPosition.z;

	vsout.SphereNormal.xyz = mul(World, normalize(adjustedModelPosition));

	vsout.World = World;

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Albedo : SV_Target0;

#if !defined(RENDER_DEPTH)
	float2 MotionVector : SV_Target1;
	float4 Normal : SV_Target2;
#endif
};

#ifdef PSHADER
SamplerState SampDiffuse : register(s0);
SamplerState SampShadowMaskSampler : register(s14);
Texture2D<float4> TexDiffuse : register(t0);

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

cbuffer PerTechnique : register(b0)
{
	float4 DiffuseColor : packoffset(c0);
	float4 AmbientColor : packoffset(c1);
};

const static float DepthOffsets[16] = {
	0.003921568,
	0.533333361,
	0.133333340,
	0.666666687,
	0.800000000,
	0.266666681,
	0.933333337,
	0.400000000,
	0.200000000,
	0.733333349,
	0.066666670,
	0.600000000,
	0.996078432,
	0.466666669,
	0.866666675,
	0.333333343
};

float GetSoftLightMultiplier(float angle, float strength)
{
	float softLightParam = saturate((strength + angle) / (1 + strength));
	float arg1 = (softLightParam * softLightParam) * (3 - 2 * softLightParam);
	float clampedAngle = saturate(angle);
	float arg2 = (clampedAngle * clampedAngle) * (3 - 2 * clampedAngle);
	float softLigtMul = saturate(arg1 - arg2);
	return softLigtMul;
}

float3 TransformNormal(float3 normal)
{
	return normal * 2 + -1.0.xxx;
}

// http://www.thetenthplanet.de/archives/1180
float3x3 CalculateTBN(float3 N, float3 p, float2 uv)
{
	// get edge vectors of the pixel triangle
	float3 dp1 = ddx_coarse(p);
	float3 dp2 = ddy_coarse(p);
	float2 duv1 = ddx_coarse(uv);
	float2 duv2 = ddy_coarse(uv);

	// solve the linear system
	float3 dp2perp = cross(dp2, N);
	float3 dp1perp = cross(N, dp1);
	float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float invmax = rsqrt(max(dot(T, T), dot(B, B)));
	return float3x3(T * invmax, B * invmax, N);
}

#	if defined(SCREEN_SPACE_SHADOWS)
#		include "ScreenSpaceShadows/ShadowsPS.hlsli"
#	endif

#	if defined(CLOUD_SHADOWS)
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

PS_OUTPUT main(PS_INPUT input, bool frontFace
			   : SV_IsFrontFace)
{
	PS_OUTPUT psout;

#	if defined(RENDER_DEPTH)
	uint2 temp = uint2(input.Position.xy);
	uint index = ((temp.x << 2) & 12) | (temp.y & 3);

	float depthOffset = 0.5 - DepthOffsets[index];
	float depthModifier = (input.Depth.w * depthOffset) + input.Depth.z - 0.5;

	if (depthModifier < 0) {
		discard;
	}

	float alpha = TexDiffuse.Sample(SampDiffuse, input.TexCoord.xy).w;

	if ((alpha - AlphaTestRefRS) < 0) {
		discard;
	}

	psout.Albedo.xyz = input.Depth.xxx / input.Depth.yyy;
	psout.Albedo.w = 0;
#	else

	float4 baseColor = TexDiffuse.Sample(SampDiffuse, input.TexCoord.xy);

#		if defined(DO_ALPHA_TEST)
	if ((baseColor.w - AlphaTestRefRS) < 0) {
		discard;
	}
#		endif

	float2 screenMotionVector = GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);

	psout.MotionVector = screenMotionVector;

	float3 ddx = ddx_coarse(input.WorldPosition);
	float3 ddy = ddy_coarse(input.WorldPosition);
	float3 normal = normalize(cross(ddx, ddy));

	float3 viewDirection = -normalize(input.WorldPosition.xyz);
	float3 worldNormal = normal;

	worldNormal = normalize(input.SphereNormal.xyz);
	worldNormal.xy *= 2;
	worldNormal = normalize(worldNormal);
	worldNormal = normalize(lerp(-worldNormal, normal, 0.25));

	if (ComplexAtlasTexture && EnableComplexTreeLOD) {
		float3 normalColor = TexDiffuse.Sample(SampDiffuse, float2(input.TexCoord.x, 0.5 + input.TexCoord.y));
		normalColor = TransformNormal(normalColor);
		// Increases the strength of the normal to simulate more advanced lighting.
		normalColor.xy *= 2;
		normalColor = normalize(normalColor);
		// world-space -> tangent-space -> world-space.
		// This is because we don't have pre-computed tangents.
		worldNormal.xyz = normalize(mul(normalColor.xyz, CalculateTBN(worldNormal.xyz, -input.WorldPosition.xyz, input.TexCoord.xy)));
	}

	float3 dirLightColor = lerp(RGBToLuminance(DirLightColor.xyz), DirLightColor.xyz, 0.5) * 0.5;

	if (EnableDirLightFix) {
		dirLightColor *= DirLightScale;
	}

#		if defined(CLOUD_SHADOWS)
	float3 normalizedDirLightDirectionWS = -normalize(mul(input.World, float4(DirLightDirection.xyz, 0))).xyz;

	float3 cloudShadowMult = 1.0;
	if (perPassCloudShadow[0].EnableCloudShadows && !lightingData[0].Reflections) {
		cloudShadowMult = getCloudShadowMult(input.WorldPosition.xyz, normalizedDirLightDirectionWS.xyz, SampDiffuse);
		dirLightColor *= cloudShadowMult;
	}
#		endif

	float3 nsDirLightColor = dirLightColor;

#		if defined(SCREEN_SPACE_SHADOWS)
	float dirLightSShadow = PrepassScreenSpaceShadows(input.WorldPosition);
#		endif

	float3 diffuseColor = 0;

	float3 lightsDiffuseColor = 0;

	float dirLightAngle = dot(worldNormal.xyz, DirLightDirection.xyz);
	float3 dirDiffuseColor = dirLightColor * saturate(dirLightAngle);

	lightsDiffuseColor += dirDiffuseColor * dirLightColor;

	float3 subsurfaceColor = lerp(RGBToLuminance(baseColor.xyz), baseColor.xyz, 2.0);

	// Applies lighting across the whole surface apart from what is already lit.
	lightsDiffuseColor += subsurfaceColor * nsDirLightColor * GetSoftLightMultiplier(dirLightAngle, SubsurfaceScatteringAmount);

	// Applies lighting from the opposite direction. Does not account for normals perpendicular to the light source.
	lightsDiffuseColor += subsurfaceColor * dirLightColor * saturate(-dirLightAngle) * SubsurfaceScatteringAmount;

	float3 directionalAmbientColor = mul(DirectionalAmbient, float4(worldNormal.xyz, 1));
#		if defined(CLOUD_SHADOWS)
	if (perPassCloudShadow[0].EnableCloudShadows && !lightingData[0].Reflections)
		directionalAmbientColor *= lerp(1.0, cloudShadowMult, perPassCloudShadow[0].AbsorptionAmbient);
#		endif
	lightsDiffuseColor += directionalAmbientColor;

	diffuseColor += lightsDiffuseColor;

	float3 color = diffuseColor * baseColor.xyz;
	psout.Albedo.xyz = color;
	psout.Albedo.w = 1;
#	endif

	return psout;
}
#endif
