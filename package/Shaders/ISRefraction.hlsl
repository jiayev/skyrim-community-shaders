#include "Common/Color.hlsl"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState Src0Sampler : register(s0);
SamplerState Src1Sampler : register(s1);

Texture2D<float4> Src0Tex : register(t0);
Texture2D<float4> Src1Tex : register(t1);

cbuffer PerGeometry : register(b2)
{
	float4 Tint : packoffset(c0);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 adjustedTexCoord = GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	float4 src1 = Src1Tex.Sample(Src1Sampler, adjustedTexCoord);
	float4 src00 = Src0Tex.Sample(Src0Sampler, adjustedTexCoord);

	float2 texCoord10 = input.TexCoord + float2(1, -1) * (2 * (0.05 * src1.z) * (src1.xy - 0.5));
	float2 texCoord11 = texCoord10 > 0.85 ? lerp(texCoord10, 0.85, 0.78) : texCoord10;
	texCoord11 = texCoord10 < 0.15 ? lerp(texCoord10, 0.15, 0.78) : texCoord11;

	float2 texCoord1 = lerp(texCoord10, texCoord11, src1.z);
	float2 adjustedTexCoord1 = GetDynamicResolutionAdjustedScreenPosition(texCoord1);

	float unk1 = Src1Tex.Sample(Src1Sampler, adjustedTexCoord1).w;
	float4 src01 = Src0Tex.Sample(Src0Sampler, adjustedTexCoord1);
	float4 src0 = unk1 != 0 ? src01 : src00;

	if (src1.w > 0.8 && src1.w < 1) {
		psout.Color.xyz =
			(1 - Tint.w) * src0.xyz + Tint.xyz * (Tint.w * RGBToLuminance2(src01.xyz));
	} else {
		psout.Color.xyz = src0.xyz;
	}

	psout.Color.w = src0.w;

	return psout;
}
#endif