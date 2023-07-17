struct VS_INPUT
{
	float4 Position : POSITION0;

#if defined(TEX) || defined(HORIZFADE)
	float2 TexCoord : TEXCOORD0;
#endif

	float4 Color : COLOR0;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;

#if defined(DITHER) && defined(TEX)
	float4 TexCoord0 : TEXCOORD0;
#elif defined(DITHER)
	float2 TexCoord0 : TEXCOORD3;
#elif defined(TEX) || defined(HORIZFADE)
	float2 TexCoord0 : TEXCOORD0;
#endif

#if defined(TEXLERP)
	float2 TexCoord1 : TEXCOORD1;
#endif

#if defined(HORIZFADE)
	float TexCoord2 : TEXCOORD2;
#endif

#if defined(TEX) || defined(DITHER) || defined(HORIZFADE)
	float4 Color : COLOR0;
#endif

	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
};

#ifdef VSHADER
cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float4x4 World : packoffset(c4);
	row_major float4x4 PreviousWorld : packoffset(c8);
	float3 EyePosition : packoffset(c12);
	float VParams : packoffset(c12.w);
	float4 BlendColor[3] : packoffset(c13);
	float2 TexCoordOff : packoffset(c16);
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float4 inputPosition = float4(input.Position.xyz, 1.0);

#	if defined(OCCLUSION)

	// Intentionally left blank

#	elif defined(MOONMASK)

	vsout.TexCoord0 = input.TexCoord;
	vsout.Color = float4(VParams.xxx, 1.0);

#	elif defined(HORIZFADE)

	float worldHeight = mul(World, inputPosition).z;
	float eyeHeightDelta = -EyePosition.z + worldHeight;

	vsout.TexCoord0.xy = input.TexCoord;
	vsout.TexCoord2.x = saturate((1.0 / 17.0) * eyeHeightDelta);
	vsout.Color.xyz = BlendColor[0].xyz * VParams;
	vsout.Color.w = BlendColor[0].w;

#	else  // MOONMASK HORIZFADE

#		if defined(DITHER)

#			if defined(TEX)
	vsout.TexCoord0.xyzw = input.TexCoord.xyxy * float4(1.0, 1.0, 501.0, 501.0);
#			else
	float3 inputDirection = normalize(input.Position.xyz);
	inputDirection.y += inputDirection.z;

	vsout.TexCoord0.x = 501 * acos(inputDirection.x);
	vsout.TexCoord0.y = 501 * asin(inputDirection.y);
#			endif  // TEX

#		elif defined(CLOUDS)
	vsout.TexCoord0.xy = TexCoordOff + input.TexCoord;
#		else
	vsout.TexCoord0.xy = input.TexCoord;
#		endif  // DITHER CLOUDS

#		ifdef TEXLERP
	vsout.TexCoord1.xy = TexCoordOff + input.TexCoord;
#		endif  // TEXLERP

	float3 skyColor = BlendColor[0].xyz * input.Color.xxx + BlendColor[1].xyz * input.Color.yyy +
	                  BlendColor[2].xyz * input.Color.zzz;

	vsout.Color.xyz = VParams * skyColor;
	vsout.Color.w = BlendColor[0].w * input.Color.w;

#	endif  // OCCLUSION MOONMASK HORIZFADE

	vsout.Position = mul(WorldViewProj, inputPosition).xyww;
	vsout.WorldPosition = mul(World, inputPosition);
	vsout.PreviousWorldPosition = mul(PreviousWorld, inputPosition);

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
	float2 MotionVectors : SV_Target1;
	float4 Normal : SV_Target2;
};

#ifdef PSHADER
SamplerState SampBaseSampler : register(s0);
SamplerState SampBlendSampler : register(s1);
SamplerState SampNoiseGradSampler : register(s2);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexBlendSampler : register(t1);
Texture2D<float4> TexNoiseGradSampler : register(t2);

cbuffer PerFrame : register(b12)
{
	float4 UnknownPerFrame1[12] : packoffset(c0);
	row_major float4x4 ScreenProj : packoffset(c12);
	row_major float4x4 PreviousScreenProj : packoffset(c16);
}

cbuffer PerGeometry : register(b2)
{
	float2 PParams : packoffset(c0);
};

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	ifndef OCCLUSION
#		ifndef TEXLERP
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
#			ifdef TEXFADE
	baseColor.w *= PParams.x;
#			endif
#		else
	float4 blendColor = TexBlendSampler.Sample(SampBlendSampler, input.TexCoord1.xy);
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	baseColor = PParams.xxxx * (-baseColor + blendColor) + baseColor;
#		endif

#		if defined(DITHER)
	float2 noiseGradUv = float2(0.125, 0.125) * input.Position.xy;
	float noiseGrad =
		TexNoiseGradSampler.Sample(SampNoiseGradSampler, noiseGradUv).x * 0.03125 + -0.0078125;

#			ifdef TEX
	psout.Color.xyz = (input.Color.xyz * baseColor.xyz + PParams.yyy) + noiseGrad;
	psout.Color.w = baseColor.w * input.Color.w;
#			else
	psout.Color.xyz = (PParams.yyy + input.Color.xyz) + noiseGrad;
	psout.Color.w = input.Color.w;
#			endif  // TEX
#		elif defined(MOONMASK)
	psout.Color.xyzw = baseColor;

	if (baseColor.w - AlphaTestRefRS.x < 0) {
		discard;
	}

#		elif defined(HORIZFADE)
	psout.Color.xyz = float3(1.5, 1.5, 1.5) * (input.Color.xyz * baseColor.xyz + PParams.yyy);
	psout.Color.w = input.TexCoord2.x * (baseColor.w * input.Color.w);
#		else
	psout.Color.w = input.Color.w * baseColor.w;
	psout.Color.xyz = input.Color.xyz * baseColor.xyz + PParams.yyy;
#		endif

#	else
	psout.Color = float4(0, 0, 0, 1.0);
#	endif  // OCCLUSION

	float4 screenPosition = mul(ScreenProj, input.WorldPosition);
	screenPosition.xy = screenPosition.xy / screenPosition.ww;
	float4 previousScreenPosition = mul(PreviousScreenProj, input.PreviousWorldPosition);
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.ww;
	float2 screenMotionVector = float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);

	psout.MotionVectors = screenMotionVector;
	psout.Normal = float4(0.5, 0.5, 0, 0);

	return psout;
}
#endif
