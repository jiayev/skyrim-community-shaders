#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState ImageSampler : register(s0);
#	if defined(DOWNSAMPLE)
SamplerState AdaptSampler : register(s1);
#	elif defined(BLEND)
SamplerState BlendSampler : register(s1);
#	endif
SamplerState AvgSampler : register(s2);

Texture2D<float4> ImageTex : register(t0);
#	if defined(DOWNSAMPLE)
Texture2D<float4> AdaptTex : register(t1);
#	elif defined(BLEND)
Texture2D<float4> BlendTex : register(t1);
#	endif
Texture2D<float4> AvgTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float4 Flags : packoffset(c0);
	float4 TimingData : packoffset(c1);
	float4 Param : packoffset(c2);
	float4 Cinematic : packoffset(c3);
	float4 Tint : packoffset(c4);
	float4 Fade : packoffset(c5);
	float4 BlurScale : packoffset(c6);
	float4 BlurOffsets[16] : packoffset(c7);
};

float GetTonemapFactorReinhard(float luminance)
{
	return (luminance * (luminance * Param.y + 1)) / (luminance + 1);
}

float GetTonemapFactorHejlBurgessDawson(float luminance)
{
	float tmp = max(0, luminance - 0.004);
	return Param.y *
	       pow(((tmp * 6.2 + 0.5) * tmp) / (tmp * (tmp * 6.2 + 1.7) + 0.06), GammaCorrectionValue);
}

// SafeDivision and RestorePostProcess by Pumbo https://github.com/Filoppi

// Returns 0, 1 or FLT_MAX if "dividend" is 0
float SafeDivision(float quotient, float dividend, int fallbackMode = 0)
{
	if (dividend == 0.0) {
		if (fallbackMode == 0)
			return 0;
		if (fallbackMode == 1)
			return sign(quotient);
		return asfloat(0x7F7FFFFF) * sign(quotient);
	}
	return quotient / dividend;
}

// Returns 0, 1 or FLT_MAX if "dividend" is 0
float3 SafeDivision(float3 quotient, float3 dividend, int fallbackMode = 0)
{
	return float3(SafeDivision(quotient.x, dividend.x, fallbackMode), SafeDivision(quotient.y, dividend.y, fallbackMode), SafeDivision(quotient.z, dividend.z, fallbackMode));
}

// Takes any original color (before some post process is applied to it) and re-applies the same transformation the post process had applied to it on a different (but similar) color.
// The images are expected to have roughly the same mid gray.
// It can be used for example to apply any SDR LUT or SDR Color Correction on an HDR color.
float3 RestorePostProcess(float3 NonPostProcessedTargetColor, float3 NonPostProcessedSourceColor, float3 PostProcessedSourceColor)
{
	static const float MaxShadowsColor = pow(1.0 / 3.0, 2.2);
	const float3 PostProcessColorRatio = SafeDivision(PostProcessedSourceColor, NonPostProcessedSourceColor);
	const float3 PostProcessColorOffset = PostProcessedSourceColor - NonPostProcessedSourceColor;
	const float3 PostProcessedRatioColor = NonPostProcessedTargetColor * PostProcessColorRatio;
	const float3 PostProcessedOffsetColor = NonPostProcessedTargetColor + PostProcessColorOffset;
	// Near black, we prefer using the "offset" (sum) pp restoration method, as otherwise any raised black from post processing would not work to apply,
	// for example if any zero was shifted to a more raised color, "PostProcessColorRatio" would not be able to replicate that shift due to a division by zero.
	const float3 PostProcessedTargetColor = lerp(PostProcessedOffsetColor, PostProcessedRatioColor, max(saturate(abs(NonPostProcessedTargetColor) / MaxShadowsColor), saturate(abs(NonPostProcessedSourceColor) / MaxShadowsColor)));
	return PostProcessedTargetColor;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if defined(DOWNSAMPLE)
	float3 downsampledColor = 0;
	for (int sampleIndex = 0; sampleIndex < DOWNSAMPLE; ++sampleIndex) {
		float2 texCoord = BlurOffsets[sampleIndex].xy * BlurScale.xy + input.TexCoord;
		if (Flags.x > 0.5) {
			texCoord = GetDynamicResolutionAdjustedScreenPosition(texCoord);
		}
		float3 imageColor = ImageTex.Sample(ImageSampler, texCoord).xyz;
#		if defined(RGB2LUM)
		imageColor = RGBToLuminance(imageColor);
#		elif (defined(LUM) || defined(LUMCLAMP)) && !defined(DOWNADAPT)
		imageColor = imageColor.x;
#		endif
		downsampledColor += imageColor * BlurOffsets[sampleIndex].z;
	}
#		if defined(DOWNADAPT)
	float2 adaptValue = AdaptTex.Sample(AdaptSampler, input.TexCoord).xy;
	if (isnan(downsampledColor.x) || isnan(downsampledColor.y) || isnan(downsampledColor.z)) {
		downsampledColor.xy = adaptValue;
	} else {
		float2 adaptDelta = downsampledColor.xy - adaptValue;
		downsampledColor.xy =
			sign(adaptDelta) * clamp(abs(Param.wz * adaptDelta), 0.00390625, abs(adaptDelta)) +
			adaptValue;
	}
#		endif
	psout.Color = float4(downsampledColor, BlurScale.z);

#	elif defined(BLEND)
	float2 uv = GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	float3 inputColor = BlendTex.Sample(BlendSampler, uv).xyz;

	float3 bloomColor = 0;
	if (Flags.x > 0.5) {
		bloomColor = ImageTex.Sample(ImageSampler, uv).xyz;
	} else {
		bloomColor = ImageTex.Sample(ImageSampler, input.TexCoord.xy).xyz;
	}

	float2 avgValue = AvgTex.Sample(AvgSampler, input.TexCoord.xy).xy;

	// Vanilla tonemapping and post-processing
	float3 gameSdrColor = 0.0;
	float3 ppColor = 0.0;
	{
		float luminance = max(1e-5, RGBToLuminance(inputColor));
		float exposureAdjustedLuminance = (avgValue.y / avgValue.x) * luminance;
		float blendFactor;
		if (Param.z > 0.5) {
			blendFactor = GetTonemapFactorHejlBurgessDawson(exposureAdjustedLuminance);
		} else {
			blendFactor = GetTonemapFactorReinhard(exposureAdjustedLuminance);
		}

		float3 blendedColor = inputColor * (blendFactor / luminance);
		blendedColor += saturate(Param.x - blendFactor) * bloomColor;

		gameSdrColor = blendedColor;

		float blendedLuminance = RGBToLuminance(blendedColor);

		float4 linearColor = lerp(avgValue.x,
			Cinematic.w * lerp(lerp(blendedLuminance, float4(blendedColor, 1), Cinematic.x),
							  blendedLuminance * Tint, Tint.w),
			Cinematic.z);

		gameSdrColor = max(0, gameSdrColor);
		ppColor = max(0, linearColor);
	}

	// Apply bloom directly to the HDR input
	inputColor += saturate(Param.x - inputColor) * bloomColor;

	// Eye adaptation
	inputColor *= avgValue.y / avgValue.x;

	// Gamma to linear
	inputColor = pow(inputColor, 2.2);
	gameSdrColor = pow(gameSdrColor, 2.2);
	ppColor = pow(ppColor, 2.2);

	// Map SDR post-processing data to HDR
	inputColor = max(0, RestorePostProcess(inputColor, gameSdrColor, ppColor));

	// Tonemap HDR input which contains post-processing

	float3 linearColor = max(0, ApplyHuePreservingShoulder(inputColor, 1.0 - smoothstep(0.6, 1.4, FrameParams.x)));
	// Linear to gamma
	float3 srgbColor = pow(linearColor, 1.0 / 2.2);

#		if defined(FADE)
	srgbColor = lerp(srgbColor, Fade.xyz, Fade.w);
#		endif

	psout.Color = float4(srgbColor, 1.0);

#	endif

	return psout;
}
#endif
