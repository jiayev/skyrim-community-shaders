#undef LUM_709
#undef DELTA
#define LUM_709 float3(0.212500006, 0.715399981, 0.0720999986)
#define DELTA 9.99999975e-06

// Vanilla post-processing based off of work by kingeric1992, aers and nukem
// http://enbseries.enbdev.com/forum/viewtopic.php?f=7&t=5278
// Adapted by doodlez

Texture2D<float4> TextureAdaptation : register(t2);

Texture2D<float4> TextureColor : register(t1);

Texture2D<float4> TextureBloom : register(t0);

SamplerState TextureAdaptationSampler : register(s2);

SamplerState TextureColorSampler : register(s1);

SamplerState TextureBloomSampler : register(s0);

cbuffer cb2 : register(b2)
{
#ifdef FADE
	float4 Params01[6];
#else
	float4 Params01[5];
#endif
}

// Shared PerFrame buffer
cbuffer PerFrame : register(b12)
{
	row_major float4x4 ViewMatrix : packoffset(c0);
	row_major float4x4 ProjMatrix : packoffset(c4);
	row_major float4x4 ViewProjMatrix : packoffset(c8);
	row_major float4x4 ViewProjMatrixUnjittered : packoffset(c12);
	row_major float4x4 PreviousViewProjMatrixUnjittered : packoffset(c16);
	row_major float4x4 InvProjMatrixUnjittered : packoffset(c20);
	row_major float4x4 ProjMatrixUnjittered : packoffset(c24);
	row_major float4x4 InvViewMatrix : packoffset(c28);
	row_major float4x4 InvViewProjMatrix : packoffset(c32);
	row_major float4x4 InvProjMatrix : packoffset(c36);
	float4 CurrentPosAdjust : packoffset(c40);
	float4 PreviousPosAdjust : packoffset(c41);
	// notes: FirstPersonY seems 1.0 regardless of third/first person, could be LE legacy stuff
	float4 GammaInvX_FirstPersonY_AlphaPassZ_CreationKitW : packoffset(c42);
	float4 DynamicRes_WidthX_HeightY_PreviousWidthZ_PreviousHeightW : packoffset(c43);
	float4 DynamicRes_InvWidthX_InvHeightY_WidthClampZ_HeightClampW : packoffset(c44);
}

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
	float3 TexCoord : TEXCOORD0;
};

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

PS_OUTPUT main(PS_INPUT input)
{
	float2 scaledUV = clamp(0.0, float2(DynamicRes_InvWidthX_InvHeightY_WidthClampZ_HeightClampW.z, DynamicRes_WidthX_HeightY_PreviousWidthZ_PreviousHeightW.y), DynamicRes_WidthX_HeightY_PreviousWidthZ_PreviousHeightW.xy * input.TexCoord.xy);

	float3 color = TextureColor.Sample(TextureColorSampler, scaledUV.xy);
#ifdef FADE
	float3 fade = Params01[4].xyz;      // fade current scene to specified color, mostly used in special effects
	float fade_weight = Params01[4].w;  // 0 == no fade
	color = lerp(color, fade, fade_weight);
#endif

	PS_OUTPUT psout;
	psout.Color.rgb = color;
	psout.Color.a = 1.0f;
	return psout;
}