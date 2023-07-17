struct VS_INPUT
{
	float4 Position : POSITION0;
#if !defined(ENVCUBE)
	float4 Normal : NORMAL0;
#endif
	float4 TexCoord0 : TEXCOORD0;
#if defined(ENVCUBE)
	float4
#else
	int4
#endif
		TexCoord1 : TEXCOORD1;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
	float4 Color : COLOR0;
	float2 TexCoord0 : TEXCOORD0;
#if defined(ENVCUBE)
	float4 PrecipitationOcclusionTexCoord : TEXCOORD1;
#endif
};

#ifdef VSHADER
cbuffer PerTechnique : register(b0)
{
	float2 ScaleAdjust : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj;
	row_major float4x4 WorldView;
#	if defined(ENVCUBE)
	row_major float4x4 PrecipitationOcclusionWorldViewProj;
#	endif
	float4 fVars0;
	float4 fVars1;
	float4 fVars2;
	float4 fVars3;
	float4 fVars4;
	float4 Color1;
	float4 Color2;
	float4 Color3;
	float4 Velocity;
	float4 Acceleration;
	float4 Wind;
}

float2x2 GetRotationMatrix(float angle)
{
	float sine, cosine;
	sincos(angle, sine, cosine);

	return float2x2(float2(cosine, -sine), float2(sine, cosine));
}

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

#	if defined(ENVCUBE)
#		if defined(RAIN)
	float2 positionOffset = input.TexCoord1.xy;
#		else
	float2x2 rotationMatrix = GetRotationMatrix(fVars0.w);
	float2 positionOffset = mul(rotationMatrix, input.TexCoord1.xy) * ScaleAdjust + mul(rotationMatrix, input.TexCoord1.zw);
#		endif

	float3 normalizedPosition = (fVars0.xyz + input.Position.xyz) / fVars2.xxx;
	normalizedPosition = normalizedPosition >= -normalizedPosition ? frac(abs(normalizedPosition)) :
	                                                                 -frac(abs(normalizedPosition));

	float4 msPosition;
	msPosition.xyz = normalizedPosition * fVars2.xxx + (-(fVars2.x * 0.5).xxx + fVars1.xyz);
	msPosition.w = 1;

	float4 viewPosition = mul(WorldViewProj, msPosition);
#		if defined(RAIN)
	float4 adjustedMsPosition = msPosition - float4(Velocity.xyz, 0);
	float positionBlendParam = 0.5 * (1 + input.TexCoord1.y);
	float4 adjustedViewPosition = mul(WorldViewProj, adjustedMsPosition);
	float4 finalViewPosition = lerp(adjustedViewPosition, viewPosition, positionBlendParam);
#		else
	float4 finalViewPosition = viewPosition;
#		endif
	vsout.Position.xy = positionOffset + finalViewPosition.xy;
	vsout.Position.zw = finalViewPosition.zw;

	vsout.Color.xyz = 1.0.xxx;
	vsout.Color.w = fVars1.w;

	vsout.TexCoord0.xy = input.TexCoord0.xy;

	float4 precipitationOcclusionTexCoord = mul(PrecipitationOcclusionWorldViewProj, msPosition);
	precipitationOcclusionTexCoord.y = -precipitationOcclusionTexCoord.y;
	vsout.PrecipitationOcclusionTexCoord = precipitationOcclusionTexCoord;
#	else
	float tmp2 = input.Normal.w * input.Position.w;
	float tmp1 = tmp2 / fVars0.y;

	float uvScale1, uvScale2, tmp3, tmp4;
	if (tmp1 > fVars2.w) {
		uvScale1 = fVars2.y;
		uvScale2 = 0;
		tmp3 = fVars2.w;
		tmp4 = 1;
	} else if (tmp1 > fVars2.z) {
		uvScale1 = fVars2.x;
		uvScale2 = fVars2.y;
		tmp3 = fVars2.z;
		tmp4 = fVars2.w;
	} else {
		uvScale1 = 0;
		uvScale2 = fVars2.x;
		tmp3 = 0;
		tmp4 = fVars2.z;
	}
	float uvScaleParam = (tmp1 - tmp3) / (tmp4 - tmp3);
	float uvScale = lerp(uvScale1, uvScale2, uvScaleParam);

	vsout.TexCoord0.xy = fVars4.xy * input.TexCoord1.xy;

	float2 uv1 = (input.TexCoord1.zw * 2.0.xx - 1.0.xx) * uvScale;
	float uvAngle = input.TexCoord0.y * input.Position.w + input.TexCoord0.x;
	float2x2 rotationMatrix = GetRotationMatrix(uvAngle);
	float2 positionOffset = mul(rotationMatrix, uv1);

	float4 msPosition;
	msPosition.xyz = -fVars3.xyz +
	                 (((input.Normal.xyz * fVars0.www + Acceleration.xyz) * (tmp2 * tmp2)) * 0.5 +
						 (((fVars0.zzz * input.Normal.xyz) * input.TexCoord0.zzz +
							  (normalize(-Wind.xyz + input.Position.xyz) * Wind.www + Velocity.xyz)) *
								 tmp2 +
							 input.Position.xyz));
	msPosition.w = 1;

	float4 viewPosition = mul(WorldViewProj, msPosition);
	vsout.Position.xy = positionOffset * ScaleAdjust + viewPosition.xy;
	vsout.Position.zw = viewPosition.zw;

	float4 color1, color2;
	float colorTmp1, colorTmp2;
	if (tmp1 > fVars1.z) {
		color1 = Color3.xyzw;
		color2 = float4(Color3.xyz, 0);
		colorTmp1 = fVars1.z;
		colorTmp2 = 1;
	} else if (tmp1 > fVars1.y) {
		color1 = Color2.xyzw;
		color2 = Color3.xyzw;
		colorTmp1 = fVars1.y;
		colorTmp2 = fVars1.z;
	} else if (tmp1 > fVars1.x) {
		color1 = Color1.xyzw;
		color2 = Color2.xyzw;
		colorTmp1 = fVars1.x;
		colorTmp2 = fVars1.y;
	} else {
		color1 = float4(Color1.xyz, 0);
		color2 = Color1.xyzw;
		colorTmp1 = 0;
		colorTmp2 = fVars1.x;
	}
	float colorParam = (tmp1 - colorTmp1) / (colorTmp2 - colorTmp1);
	float4 color = lerp(color1, color2, colorParam);

	vsout.Color.w = fVars3.w * color.w;
	vsout.Color.xyz = color.xyz;
#	endif

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
	float4 Normal : SV_Target1;
};

#ifdef PSHADER
SamplerState SampSourceTexture : register(s0);
#	if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
SamplerState SampGrayscaleTexture : register(s1);
#	endif
#	if defined(ENVCUBE)
SamplerState SampPrecipitationOcclusionTexture : register(s2);
SamplerState SampUnderwaterMask : register(s3);
#	endif

Texture2D<float4> TexSourceTexture : register(t0);
#	if defined(GRAYSCALE_TO_COLOR) || defined(GRAYSCALE_TO_ALPHA)
Texture2D<float4> TexGrayscaleTexture : register(t1);
#	endif
#	if defined(ENVCUBE)
Texture2D<float4> TexPrecipitationOcclusionTexture : register(t2);
Texture2D<float4> TexUnderwaterMask : register(t3);
#	endif

cbuffer PerGeometry : register(b2)
{
	float ColorScale : packoffset(c0);
	float3 TextureSize : packoffset(c1);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if defined(ENVCUBE)
	float3 precipitationOcclusionUv =
		float3(TextureSize.xx * (input.PrecipitationOcclusionTexCoord.xy * 0.5.xx + 0.5.xx), 0);
	float precipitationOcclusion =
		-input.PrecipitationOcclusionTexCoord.z +
		TexPrecipitationOcclusionTexture.Load(precipitationOcclusionUv).x;
	float2 underwaterMaskUv = TextureSize.yz * input.Position.xy;
	float underwaterMask = TexUnderwaterMask.Sample(SampUnderwaterMask, underwaterMaskUv).x;
	if (precipitationOcclusion - underwaterMask < 0) {
		discard;
	}
#	endif

	float4 sourceColor = TexSourceTexture.Sample(SampSourceTexture, input.TexCoord0);
	float4 baseColor = input.Color * sourceColor;
#	if defined(GRAYSCALE_TO_COLOR)
	float3 grayScaleColor =
		TexGrayscaleTexture.Sample(SampGrayscaleTexture, float2(sourceColor.y, input.Color.x)).xyz;
	baseColor.xyz = grayScaleColor;
#	endif
#	if defined(GRAYSCALE_TO_ALPHA)
	float grayScaleAlpha =
		TexGrayscaleTexture.Sample(SampGrayscaleTexture, float2(sourceColor.w, input.Color.w)).w;
	baseColor.w = grayScaleAlpha;
#	endif

	psout.Color.xyz = ColorScale * baseColor.xyz;
	psout.Color.w = baseColor.w;
	psout.Normal.w = baseColor.w;
#	if defined(ENVCUBE)
	psout.Normal.xyz = float3(0, 1, 0);
#	else
	psout.Normal.xyz = float3(1, 0, 0);
#	endif

	return psout;
}
#endif
