struct VS_INPUT
{
	float4 Position : POSITION0;
	float2 TexCoord : TEXCOORD0;
	float4 Normal : NORMAL0;
	float4 Color : COLOR0;
	float4 InstanceData1 : TEXCOORD4;
	float4 InstanceData2 : TEXCOORD5;
	float4 InstanceData3 : TEXCOORD6;
	float4 InstanceData4 : TEXCOORD7;
};

struct VS_OUTPUT
{
	float4 HPosition : SV_POSITION0;
	float4 DiffuseColor : COLOR0;
	float3 TexCoord : TEXCOORD0;
	float4 AmbientColor : TEXCOORD1;
	float3 ViewSpacePosition : TEXCOORD2;
#if defined(RENDER_DEPTH)
	float2 Depth : TEXCOORD3;
#endif
	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
};

#ifdef VSHADER
cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float4x4 WorldView : packoffset(c4);
	row_major float4x4 World : packoffset(c8);
	row_major float4x4 PreviousWorld : packoffset(c12);
	float4 FogNearColor : packoffset(c16);
	float3 WindVector : packoffset(c17);
	float WindTimer : packoffset(c17.w);
	float3 DirLightDirection : packoffset(c18);
	float PreviousWindTimer : packoffset(c18.w);
	float3 DirLightColor : packoffset(c19);
	float AlphaParam1 : packoffset(c19.w);
	float3 AmbientColor : packoffset(c20);
	float AlphaParam2 : packoffset(c20.w);
	float3 ScaleMask : packoffset(c21);
	float ShadowClampValue : packoffset(c21.w);
}

cbuffer cb7 : register(b7)
{
	float4 cb7[1];
}

cbuffer cb8 : register(b8)
{
	float4 cb8[240];
}

#	define M_PI 3.1415925  // PI
#	define M_2PI 6.283185  // PI * 2

const static float4x4 M_IdentityMatrix = {
	{ 1, 0, 0, 0 },
	{ 0, 1, 0, 0 },
	{ 0, 0, 1, 0 },
	{ 0, 0, 0, 1 }
};

float4 GetMSPosition(VS_INPUT input, float windTimer)
{
	float windAngle = 0.4 * ((input.InstanceData1.x + input.InstanceData1.y) * -0.0078125 + windTimer);
	float windAngleSin, windAngleCos;
	sincos(windAngle, windAngleSin, windAngleCos);

	float windTmp3 = 0.2 * cos(M_PI * windAngleCos);
	float windTmp1 = sin(M_PI * windAngleSin);
	float windTmp2 = sin(M_2PI * windAngleSin);
	float windPower = WindVector.z * (((windTmp1 + windTmp2) * 0.3 + windTmp3) *
										 (0.5 * (input.Color.w * input.Color.w)));

	float3 inputPosition = input.Position.xyz * (input.InstanceData4.yyy * ScaleMask.xyz + float3(1, 1, 1));

	float3 instancePosition;
	instancePosition.z = dot(
		float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w), inputPosition);
	instancePosition.x = dot(input.InstanceData2.xyz, inputPosition);
	instancePosition.y = dot(input.InstanceData3.xyz, inputPosition);

	float3 windVector = float3(WindVector.xy, 0);

	float4 msPosition;
	msPosition.xyz = input.InstanceData1.xyz + (windVector * windPower + instancePosition);
	msPosition.w = 1;

	return msPosition;
}

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float4 msPosition = GetMSPosition(input, WindTimer);

	float4 projSpacePosition = mul(WorldViewProj, msPosition);
	vsout.HPosition = projSpacePosition;

#	if defined(RENDER_DEPTH)
	vsout.Depth = projSpacePosition.zw;
#	endif

	float3 instanceNormal = float3(input.InstanceData2.z, input.InstanceData3.zw);
	float dirLightAngle = dot(DirLightDirection.xyz, instanceNormal);
	float3 diffuseMultiplier = input.InstanceData1.www * input.Color.xyz * saturate(dirLightAngle.xxx);

	float perInstanceFade = dot(cb8[(asuint(cb7[0].x) >> 2)].xyzw, M_IdentityMatrix[(asint(cb7[0].x) & 3)].xyzw);
	float distanceFade = 1 - saturate((length(projSpacePosition.xyz) - AlphaParam1) / AlphaParam2);

	vsout.DiffuseColor.xyz = DirLightColor.xyz * diffuseMultiplier;
	vsout.DiffuseColor.w = distanceFade * perInstanceFade;

	vsout.TexCoord.xy = input.TexCoord.xy;
	vsout.TexCoord.z = FogNearColor.w;

	vsout.AmbientColor.xyz = input.InstanceData1.www * (AmbientColor.xyz * input.Color.xyz);
	vsout.AmbientColor.w = ShadowClampValue;

	vsout.ViewSpacePosition = mul(WorldView, msPosition).xyz;
	vsout.WorldPosition = mul(World, msPosition);

	float4 previousMsPosition = GetMSPosition(input, PreviousWindTimer);

	vsout.PreviousWorldPosition = mul(PreviousWorld, previousMsPosition);

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#if defined(RENDER_DEPTH)
	float4 PS : SV_Target0;
#else
	float4 Albedo : SV_Target0;
	float2 MotionVectors : SV_Target1;
	float4 Normal : SV_Target2;
#endif
};

#ifdef PSHADER
SamplerState SampBaseSampler : register(s0);
SamplerState SampShadowMaskSampler : register(s1);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexShadowMaskSampler : register(t1);

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

cbuffer PerFrame : register(b12)
{
	float4 UnknownPerFrame1[12] : packoffset(c0);
	row_major float4x4 ScreenProj : packoffset(c12);
	row_major float4x4 PreviousScreenProj : packoffset(c16);
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord.xy);

#	if defined(RENDER_DEPTH) || defined(DO_ALPHA_TEST)
	float diffuseAlpha = input.DiffuseColor.w * baseColor.w;

	if ((diffuseAlpha - AlphaTestRefRS) < 0) {
		discard;
	}
#	endif

#	if defined(RENDER_DEPTH)
	// Depth
	psout.PS.xyz = input.Depth.xxx / input.Depth.yyy;
	psout.PS.w = diffuseAlpha;
#	else
	float sunShadowMask = TexShadowMaskSampler.Load(int3(input.HPosition.xy, 0)).x;

	// Albedo
	float diffuseFraction = lerp(sunShadowMask, 1, input.AmbientColor.w);
	float3 diffuseColor = input.DiffuseColor.xyz * baseColor.xyz;
	float3 ambientColor = input.AmbientColor.xyz * baseColor.xyz;
	psout.Albedo.xyz = input.TexCoord.zzz * (diffuseColor * diffuseFraction + ambientColor);
	psout.Albedo.w = 1;

	float4 screenPosition = mul(ScreenProj, input.WorldPosition);
	screenPosition.xy = screenPosition.xy / screenPosition.ww;
	float4 previousScreenPosition = mul(PreviousScreenProj, input.PreviousWorldPosition);
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.ww;
	float2 screenMotionVector = float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);

	psout.MotionVectors = screenMotionVector;

	float3 ddx = ddx_coarse(input.ViewSpacePosition);
	float3 ddy = ddy_coarse(input.ViewSpacePosition);
	float3 normal = normalize(cross(ddx, ddy));
	float normalScale = max(1.0 / 1000.0, sqrt(normal.z * -8 + 8));
	psout.Normal.xy = float2(0.5, 0.5) + normal.xy / normalScale;
	psout.Normal.zw = float2(0, 0);
#	endif

	return psout;
}
#endif
