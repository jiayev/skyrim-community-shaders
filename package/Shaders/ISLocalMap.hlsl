#include "Common/Color.hlsl"
#include "Common/DummyVSTexCoord.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState ImageSampler : register(s0);

Texture2D<float4> ImageTex : register(t0);

cbuffer PerGeometry : register(b2)
{
	float4 TexelSize : packoffset(c0);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float3 colorLT =
		ImageTex.Sample(ImageSampler, input.TexCoord + float2(-TexelSize.x, TexelSize.y)).xyz;
	float3 colorCT = ImageTex.Sample(ImageSampler, input.TexCoord + float2(0, TexelSize.y)).xyz;
	float3 colorRT = ImageTex.Sample(ImageSampler, input.TexCoord + TexelSize.xy).xyz;
	float3 colorLB = ImageTex.Sample(ImageSampler, input.TexCoord - TexelSize.xy).xyz;
	float3 colorCB = ImageTex.Sample(ImageSampler, input.TexCoord + float2(0, -TexelSize.y)).xyz;
	float3 colorRB =
		ImageTex.Sample(ImageSampler, input.TexCoord + float2(TexelSize.x, -TexelSize.y)).xyz;
	float3 colorLC = ImageTex.Sample(ImageSampler, input.TexCoord + float2(-TexelSize.x, 0)).xyz;
	float3 colorRC = ImageTex.Sample(ImageSampler, input.TexCoord + float2(TexelSize.x, 0)).xyz;

	float3 colorTB = -colorRT * float3(0.5, 0.25, 1) - colorCT * float3(1, 0.5, 2) -
	                 colorLT * float3(0.5, 0.25, 1) + colorRB * float3(0.5, 0.25, 1) +
	                 colorCB * float3(1, 0.5, 2) + float3(0.5, 0.25, 1) * colorLB;
	float3 colorLR = -colorRT * float3(0.5, 0.25, 1) -
	                 colorRB * float3(0.5, 0.25, 1) + colorLT * float3(0.5, 0.25, 1) +
	                 colorLC * float3(1, 0.5, 2) + float3(0.5, 0.25, 1) * colorLB;

	float3 unk1 = 4 * (pow(colorLR, 2) + pow(colorTB, 2));

	float4 colorCC = ImageTex.Sample(ImageSampler, input.TexCoord);
	float luminance = RGBToLuminanceAlternative(colorCC.xyz);

	float unk2 = (dot(4 * unk1, 1.75) + 0.04 * luminance) * (1 - colorCC.w);
	float2 unk3 = 1 - pow(2 * abs(input.TexCoord - 0.5), 5);

	psout.Color.xyz = unk1.x + 0.04 * luminance;
	psout.Color.w = unk2 * unk3.x * unk3.y;

	return psout;
}
#endif