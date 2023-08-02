#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState sourceSampler : register(s0);

Texture2D<float4> sourceTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 g_RenderTargetResolution : packoffset(c0);
	float4 g_ContrastParams : packoffset(c1);
	float g_UseDynamicSampling : packoffset(c2);
};

float2 GetMinifiedTexCoord(float2 texCoord)
{
	return ((float2)(((int2)(g_RenderTargetResolution.yx * texCoord.yx) & 1) ^ 1) * 2 - 1) *
	           g_RenderTargetResolution.zw +
	       texCoord;
}

static const float4 ContrastValues[] = { { 0.300000, 0, 0, 0 }, { 0.400000, 0, 0, 0 },
	{ 0.500000, 0, 0, 0 }, { 0.400000, 0, 0, 0 }, { 0.300000, 0, 0, 0 }, { 0.400000, 0, 0, 0 },
	{ 2.000000, 0, 0, 0 }, { 2.500000, 0, 0, 0 }, { 2.000000, 0, 0, 0 }, { 0.400000, 0, 0, 0 },
	{ 0.500000, 0, 0, 0 }, { 2.500000, 0, 0, 0 }, { 3.500000, 0, 0, 0 }, { 2.500000, 0, 0, 0 },
	{ 0.500000, 0, 0, 0 }, { 0.400000, 0, 0, 0 }, { 2.000000, 0, 0, 0 }, { 2.500000, 0, 0, 0 },
	{ 2.000000, 0, 0, 0 }, { 0.400000, 0, 0, 0 }, { 0.300000, 0, 0, 0 }, { 0.400000, 0, 0, 0 },
	{ 0.500000, 0, 0, 0 }, { 0.400000, 0, 0, 0 }, { 0.300000, 0, 0, 0 } };

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 finalTexCoord;
	if (asuint(g_UseDynamicSampling) > 0) {
		float2 drAdjustedTexCoord = DynamicResolutionParams1.xy * input.TexCoord;
		float2 minifiedTexCoord = GetMinifiedTexCoord(drAdjustedTexCoord);
		finalTexCoord = clamp(minifiedTexCoord, 0,
			DynamicResolutionParams1.xy - float2(CameraPreviousPosAdjust.w, 0));
	} else {
		finalTexCoord = GetMinifiedTexCoord(input.TexCoord);
	}

	float4 color = sourceTex.Sample(sourceSampler, finalTexCoord);

#	if defined(CONTRAST)
	int contrastIndex = (int)(5 * input.TexCoord.x) + (int)(5 * input.TexCoord.y) * 5;
	float contrastFactor = ContrastValues[contrastIndex].x * g_ContrastParams.x;
	color *= contrastFactor;
#	endif

	psout.Color = color;

	return psout;
}
#endif