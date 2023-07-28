#include "Common/Color.hlsl"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsl"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState ImageSampler : register(s0);
SamplerState AvgLumSampler : register(s1);

Texture2D<float4> ImageTex : register(t0);
Texture2D<float4> AvgLumTex : register(t1);

cbuffer PerGeometry : register(b2)
{
	float4 BlurBrightPass : packoffset(c0);
	float4 BlurScale : packoffset(c1);
	float BlurRadius : packoffset(c2);
	float4 BlurOffsets[16] : packoffset(c3);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 color = 0.0.xxxx;

	float blurRadius = BLUR_RADIUS;
	float2 blurScale = BlurScale.zw;
#	if BLUR_RADIUS == 0
	blurRadius = BlurRadius;
#	endif
#	if BLUR_RADIUS == 0 || defined(BLUR_NON_HDR)
	blurScale = 1.0.xx;
#	endif

	for (int blurIndex = 0; blurIndex < blurRadius; ++blurIndex) {
		float2 screenPosition = BlurOffsets[blurIndex].xy + input.TexCoord.xy;
		if (BlurScale.x < 0.5) {
			screenPosition = GetDynamicResolutionAdjustedScreenPosition(screenPosition);
		}
		float4 imageColor = ImageTex.Sample(ImageSampler, screenPosition) * float4(blurScale.yyy, 1);
#	if defined(BLUR_BRIGHT_PASS)
		imageColor = BlurBrightPass.y * max(0.0.xxxx, -BlurBrightPass.x + imageColor);
#	endif
		color += imageColor * BlurOffsets[blurIndex].z;
	}

#	if defined(BLUR_BRIGHT_PASS)
	float avgLum = RGBToLuminance(AvgLumTex.Sample(AvgLumSampler, input.TexCoord.xy).xyz);
	color.w = avgLum;
#	endif

	psout.Color = color * float4(blurScale.xxx, 1);

	return psout;
}
#endif