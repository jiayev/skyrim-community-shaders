struct VS_INPUT
{
	float4 Position									: POSITION0;
	float2 TexCoord									: TEXCOORD0;
	float4 Normal									: NORMAL0;
	float4 Color									: COLOR0;
	float4 InstanceData1							: TEXCOORD4;
	float4 InstanceData2							: TEXCOORD5;
	float4 InstanceData3							: TEXCOORD6;
	float4 InstanceData4							: TEXCOORD7;
	uint   InstanceID 								: SV_InstanceID0;
};

struct VS_OUTPUT
{
	float4 HPosition								: SV_POSITION0;
	float4 DiffuseColor								: COLOR0;
	float3 TexCoord									: TEXCOORD0;
	float4 AmbientColor								: TEXCOORD1;
	float3 ViewSpacePosition						: TEXCOORD2;
#if defined(RENDER_DEPTH)
	float2 Depth									: TEXCOORD3;
#endif
	float4 WorldPosition							: POSITION1;
	float4 PreviousWorldPosition					: POSITION2;
	float  o7 										: SV_ClipDistance0;
	float  p7 										: SV_CullDistance0;
};

#ifdef VSHADER

cbuffer cb7 : register(b7)
{
  float4 cb7[1];
}

cbuffer cb8											: register(b8) 
{
	float4 cb8[240]; 
}

cbuffer PerGeometry									: register(b2)
{
	float4 cb2[32] 									: packoffset(c0);               
	float4 FogNearColor								: packoffset(c32);               
	float3 WindVector								: packoffset(c33);                 
	float WindTimer									: packoffset(c33.w);                 
	float3 DirLightDirection						: packoffset(c34);          
	float PreviousWindTimer							: packoffset(c34.w);         
	float3 DirLightColor							: packoffset(c35);              
	float AlphaParam1								: packoffset(c35.w);               
	float3 AmbientColor								: packoffset(c36);               
	float AlphaParam2								: packoffset(c36.w);               
	float3 ScaleMask								: packoffset(c37);                  
	float ShadowClampValue							: packoffset(c37.w);   
}

cbuffer cb13 : register(b13)
{
  float4 cb13[3];
}

#define M_PI  3.1415925 // PI
#define M_2PI 6.283185 // PI * 2

const static float4x4 M_IdentityMatrix =
{
	{ 1, 0, 0, 0 },
	{ 0, 1, 0, 0 },
	{ 0, 0, 1, 0 },
	{ 0, 0, 0, 1 }
};

float4 GetMSPosition(VS_INPUT input, float windTimer)
{
	float4 r0,r1,r2,r3,r4,r5,r6;
	r0.yzw = input.InstanceData4.y * ScaleMask.xyz + float3(1,1,1);
	r0.yzw = input.Position.xyz * r0.yzw;
	r1.x = dot(input.InstanceData2.xyz, r0.yzw);
	r1.y = dot(input.InstanceData3.xyz, r0.yzw);
	r2.x = input.InstanceData4.x;
	r2.y = input.InstanceData2.w;
	r2.z = input.InstanceData3.w;
	r1.z = dot(r2.xyz, r0.yzw);
	r0.y = input.InstanceData1.x + input.InstanceData1.y;
	r0.z = -r0.y * 0.0078125 + windTimer;
	r0.z = 0.400000006 * r0.z;
	sincos(r0.z, r2.x, r3.x);
	r0.zw = float2(3.1415925,6.28318501) * r2.xx;
	r0.zw = sin(r0.zw);
	r0.z = r0.z + r0.w;
	r0.w = 3.1415925 * r3.x;
	r0.w = cos(r0.w);
	r0.w = 0.200000003 * r0.w;
	r0.z = r0.z * 0.300000012 + r0.w;
	r0.w = input.Color.w * input.Color.w;
	r0.w = 0.5 * r0.w;
	r0.z = r0.z * r0.w;
	r0.z = WindVector.z * r0.z;
	r2.xy = WindVector.xy;
	r2.z = 0;
	r3.xyz = r2.xyz * r0.zzz + r1.xyz;
	r3.xyz = input.InstanceData1.xyz + r3.xyz;
	r3.w = 1;
	return r3;
}

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float4 r0,r1,r2,r3,r4,r5,r6;
	uint4 bitmask, uiDest;
	float4 fDest;

	r0.x = (int)input.InstanceID & 1;
	r0.x = (uint)r0.x;
	r0.x = cb13[0].y * r0.x;
	r0.x = (uint)r0.x;
	r0.z = (uint)r0.x << 2;
	r0.y = (uint)r0.x << 2;

	float4 msPosition = GetMSPosition(input, WindTimer);

	float4 projSpacePosition;
	projSpacePosition.x = dot(cb2[r0.z+0].xyzw, msPosition.xyzw);
  	projSpacePosition.y = dot(cb2[r0.z+1].xyzw, msPosition.xyzw);
  	projSpacePosition.z = dot(cb2[r0.z+2].xyzw, msPosition.xyzw);
  	projSpacePosition.w = dot(cb2[r0.z+3].xyzw, msPosition.xyzw);

#if defined(RENDER_DEPTH)
	vsout.Depth = projSpacePosition.zw;
#endif

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

	vsout.WorldPosition.x = dot(cb2[r0.z+16].xyzw, msPosition.xyzw);
	vsout.WorldPosition.y = dot(cb2[r0.z+17].xyzw, msPosition.xyzw);
	vsout.WorldPosition.z = dot(cb2[r0.z+18].xyzw, msPosition.xyzw);
	vsout.WorldPosition.w = dot(cb2[r0.z+19].xyzw, msPosition.xyzw);

	float4 previousMsPosition = GetMSPosition(input, PreviousWindTimer);
	
	vsout.PreviousWorldPosition.x = dot(cb2[r0.z+24].xyzw, previousMsPosition.xyzw);
	vsout.PreviousWorldPosition.y = dot(cb2[r0.z+25].xyzw, previousMsPosition.xyzw);
	vsout.PreviousWorldPosition.z = dot(cb2[r0.z+26].xyzw, previousMsPosition.xyzw);
	vsout.PreviousWorldPosition.w = dot(cb2[r0.z+27].xyzw, previousMsPosition.xyzw);

	vsout.ViewSpacePosition.x = dot(cb2[r0.z+8].xyzw, msPosition.xyzw);
 	vsout.ViewSpacePosition.y = dot(cb2[r0.z+9].xyzw, msPosition.xyzw);
  	vsout.ViewSpacePosition.z = dot(cb2[r0.z+10].xyzw, msPosition.xyzw);

	if (0 < cb13[0].y) {
		r0.yz = dot(projSpacePosition, cb13[r0.x+1].xyzw);
	} else {
		r0.yz = float2(1,1);
	}
	
	r0.w = 2 + -cb13[0].y;
	r0.x = dot(cb13[0].zw, M_IdentityMatrix[r0.x+0].xy);
  	r0.xw = r0.xw * projSpacePosition.wx;
    r0.x = cb13[0].y * r0.x;

	vsout.HPosition.x = r0.w * 0.5 + r0.x;
	vsout.HPosition.yzw = projSpacePosition.yzw;

	vsout.o7.x = r0.z;
  	vsout.p7.x = r0.y;

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#if defined(RENDER_DEPTH)
	float4 PS										: SV_Target0;
#else
	float4 Albedo									: SV_Target0;
	float2 MotionVectors							: SV_Target1;
	float4 Normal									: SV_Target2;
#endif
};

#ifdef PSHADER
SamplerState SampBaseSampler						: register(s0);
SamplerState SampShadowMaskSampler					: register(s1);

Texture2D<float4> TexBaseSampler					: register(t0);
Texture2D<float4> TexShadowMaskSampler				: register(t1);

cbuffer AlphaTestRefCB								: register(b13) 
{ 
	float AlphaTestRefRS							: packoffset(c0); 
}

cbuffer cb0 : register(b0)
{
  float4 cb0[10];
}

struct PerEye 
{
	row_major float4x4 ScreenProj;
	row_major float4x4 PreviousScreenProj;
};

cbuffer cb12 : register(b12)
{
  float4 cb12[87];
}

cbuffer PerGeometry									: register(b2)
{
	row_major float4x4 WorldViewProj				: packoffset(c0);
	row_major float4x4 WorldView					: packoffset(c4);
	row_major float4x4 World						: packoffset(c8);
	row_major float4x4 PreviousWorld				: packoffset(c12);
	float4 FogNearColor								: packoffset(c16);
	float3 WindVector								: packoffset(c17);
	float WindTimer 								: packoffset(c17.w);
	float3 DirLightDirection						: packoffset(c18);
	float PreviousWindTimer 						: packoffset(c18.w);
	float3 DirLightColor							: packoffset(c19);
	float AlphaParam1 								: packoffset(c19.w);
	float3 AmbientColor								: packoffset(c20);
	float AlphaParam2 								: packoffset(c20.w);
	float3 ScaleMask								: packoffset(c21);
	float ShadowClampValue 							: packoffset(c21.w);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord.xy);

#if defined(RENDER_DEPTH) || defined(DO_ALPHA_TEST)
	float diffuseAlpha = input.DiffuseColor.w * baseColor.w;

	if ((diffuseAlpha - AlphaTestRefRS) < 0)
	{
		discard;
	}
#endif

#if defined(RENDER_DEPTH)
	// Depth
	psout.PS.xyz = input.Depth.xxx / input.Depth.yyy;
	psout.PS.w = diffuseAlpha;
#else
	float sunShadowMask = TexShadowMaskSampler.Load(int3(input.HPosition.xy, 0)).x;

	// Albedo
	float diffuseFraction = lerp(sunShadowMask, 1, input.AmbientColor.w);
	float3 diffuseColor = input.DiffuseColor.xyz * baseColor.xyz;
	float3 ambientColor = input.AmbientColor.xyz * baseColor.xyz;
	psout.Albedo.xyz = input.TexCoord.zzz * (diffuseColor * diffuseFraction + ambientColor);
	psout.Albedo.w = 1;

	float stereoUV = input.HPosition.x * cb0[9].x + cb0[9].z;
	stereoUV = stereoUV * cb12[86].x;

	uint eyeIndex = (stereoUV >= 0.5);

	float3 screenPosition;
	screenPosition.x = dot(cb12[eyeIndex+24].xyzw, input.WorldPosition);
	screenPosition.y = dot(cb12[eyeIndex+25].xyzw, input.WorldPosition);
	screenPosition.z = dot(cb12[eyeIndex+27].xyzw, input.WorldPosition);
	screenPosition.xy = screenPosition.xy / screenPosition.zz;

	float3 previousScreenPosition;
	previousScreenPosition.x = dot(cb12[eyeIndex+32].xyzw, input.PreviousWorldPosition);
	previousScreenPosition.y = dot(cb12[eyeIndex+33].xyzw, input.PreviousWorldPosition);
	previousScreenPosition.z = dot(cb12[eyeIndex+35].xyzw, input.PreviousWorldPosition);
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.zz;

	float2 screenMotionVector = float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);
	psout.MotionVectors = screenMotionVector;

	float3 ddx = ddx_coarse(input.ViewSpacePosition);
	float3 ddy = ddy_coarse(input.ViewSpacePosition);
	float3 normal = normalize(cross(ddx, ddy));
	float normalScale = max(1.0 / 1000.0, sqrt(normal.z * -8 + 8));
	psout.Normal.xy = float2(0.5, 0.5) + normal.xy / normalScale;
	psout.Normal.zw = float2(0, 0);

#endif

	return psout;
}
#endif
