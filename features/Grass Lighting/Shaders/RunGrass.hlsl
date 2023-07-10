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
	float4 VertexColor								: COLOR0;
	float3 TexCoord									: TEXCOORD0;
	float3 ViewSpacePosition						: TEXCOORD2;
#if defined(RENDER_DEPTH)
	float2 Depth									: TEXCOORD3;
#endif
	float4 WorldPosition							: POSITION1;
	float4 PreviousWorldPosition					: POSITION2;
	float3 VertexNormal 					        : POSITION4;
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

float4 GetMSPosition(VS_INPUT input, float windTimer, float3x3 world3x3)
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
	float3 InstanceData4 = mul(world3x3, inputPosition);

	float3 windVector = float3(WindVector.xy, 0);

	float4 msPosition;
	msPosition.xyz = input.InstanceData1.xyz + (windVector * windPower + InstanceData4);
	msPosition.w = 1;

	return msPosition;
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

	float3x3 world3x3 = float3x3(input.InstanceData2.xyz, input.InstanceData3.xyz, float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w));

	float4 msPosition = GetMSPosition(input, WindTimer, world3x3);

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

	// Note: input.Color.w is used for wind speed
	vsout.VertexColor.xyz = input.Color.xyz * input.InstanceData1.www;
	vsout.VertexColor.w = distanceFade * perInstanceFade;

	vsout.TexCoord.xy = input.TexCoord.xy;
	vsout.TexCoord.z = FogNearColor.w;

	vsout.WorldPosition.x = dot(cb2[r0.z+16].xyzw, msPosition.xyzw);
	vsout.WorldPosition.y = dot(cb2[r0.z+17].xyzw, msPosition.xyzw);
	vsout.WorldPosition.z = dot(cb2[r0.z+18].xyzw, msPosition.xyzw);
	vsout.WorldPosition.w = dot(cb2[r0.z+19].xyzw, msPosition.xyzw);

	float4 previousMsPosition = GetMSPosition(input, PreviousWindTimer, world3x3);
	
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

	// Vertex normal needs to be transformed to world-space for lighting calculations.
	float3 vertexNormal = input.Normal.xyz * 2.0 - 1.0;
	vertexNormal = mul(world3x3, vertexNormal);
	vsout.VertexNormal.xyz = vertexNormal;

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

cbuffer PerFrame : register(b3)
{
	float4  			EyePosition[2];
	row_major float3x4 	DirectionalAmbient;
	float 				SunlightScale;
	float 				Glossiness;
	float 				SpecularStrength;
	float 				SubsurfaceScatteringAmount;
	bool 				EnableDirLightFix;
	bool 				EnablePointLights;
	float pad[2];
}

float GetSoftLightMultiplier(float angle, float strength)
{
	float softLightParam = saturate((strength + angle) / (1 + strength));
	float arg1 = (softLightParam * softLightParam) * (3 - 2 * softLightParam);
	float clampedAngle = saturate(angle);
	float arg2 = (clampedAngle * clampedAngle) * (3 - 2 * clampedAngle);
	float softLigtMul = saturate(arg1 - arg2);
	return softLigtMul;
}

float3 GetLightSpecularInput(float3 L, float3 V, float3 N, float3 lightColor, float shininess)
{
	float3 H = normalize(V + L);
	float HdotN = saturate(dot(H, N));

	float lightColorMultiplier = exp2(shininess * log2(HdotN));
	return lightColor * lightColorMultiplier.xxx;
}

float3 TransformNormal(float3 normal)
{
	return normal * 2 + -1.0.xxx;
}

// http://www.thetenthplanet.de/archives/1180
float3x3 CalculateTBN(float3 N, float3 p, float2 uv)
{
	// get edge vectors of the pixel triangle
	float3 dp1 = ddx_coarse(p);
	float3 dp2 = ddy_coarse(p);
	float2 duv1 = ddx_coarse(uv);
	float2 duv2 = ddy_coarse(uv);

	// solve the linear system
	float3 dp2perp = cross(dp2, N);
	float3 dp1perp = cross(N, dp1);
	float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float invmax = rsqrt(max(dot(T, T), dot(B, B)));
	return float3x3(T * invmax, B * invmax, N);
}

struct StructuredLight
{
	float4  color;
	float4  positionWS[2];
	float   radius;
	bool    shadow;
	float   mask;
	bool    active;
};

StructuredBuffer<StructuredLight> lights : register(t17);


PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	PS_OUTPUT psout;

	float x;
	float y;
	TexBaseSampler.GetDimensions(x, y);

	bool complex = x != y;

	float4 baseColor;
	if (complex) {
		baseColor = TexBaseSampler.Sample(SampBaseSampler, float2(input.TexCoord.x, input.TexCoord.y * 0.5));
	}
	else
	{
		baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord.xy);
	}

#if defined(RENDER_DEPTH) || defined(DO_ALPHA_TEST)
	float diffuseAlpha = input.VertexColor.w * baseColor.w;

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
	float4 specColor = complex ? TexBaseSampler.Sample(SampBaseSampler, float2(input.TexCoord.x, 0.5 + input.TexCoord.y * 0.5)) : 1;
	float4 shadowColor = TexShadowMaskSampler.Load(int3(input.HPosition.xy, 0));

	// Albedo
	// float diffuseFraction = lerp(sunShadowMask, 1, input.AmbientColor.w);
	// float3 diffuseColor = input.DiffuseColor.xyz * baseColor.xyz;
	// float3 ambientColor = input.AmbientColor.xyz * baseColor.xyz;
	// psout.Albedo.xyz = input.TexCoord.zzz * (diffuseColor * diffuseFraction + ambientColor);
	// psout.Albedo.w = 1;

	float stereoUV = input.HPosition.x * cb0[2].xy + cb0[2].zw;
	stereoUV = stereoUV * cb12[86].x;

	uint eyeIndex = (stereoUV >= 0.5) ? 1 : 0;
	uint eyeOffset = eyeIndex;
	uint bitmask;
	bitmask = ((~(-1 << 1)) << 2) & 0xffffffff;  eyeOffset = (((uint)eyeOffset<< 2) & bitmask) | ((uint)0 & ~bitmask);

	float3 screenPosition;
	screenPosition.x = dot(cb12[eyeOffset+24].xyzw, input.WorldPosition);
	screenPosition.y = dot(cb12[eyeOffset+25].xyzw, input.WorldPosition);
	screenPosition.z = dot(cb12[eyeOffset+27].xyzw, input.WorldPosition);
	screenPosition.xy = screenPosition.xy / screenPosition.zz;

	float3 previousScreenPosition;
	previousScreenPosition.x = dot(cb12[eyeOffset+32].xyzw, input.PreviousWorldPosition);
	previousScreenPosition.y = dot(cb12[eyeOffset+33].xyzw, input.PreviousWorldPosition);
	previousScreenPosition.z = dot(cb12[eyeOffset+35].xyzw, input.PreviousWorldPosition);
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.zz;

	float2 screenMotionVector = float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);
	psout.MotionVectors = screenMotionVector;

	float3 ddx = ddx_coarse(input.ViewSpacePosition);
	float3 ddy = ddy_coarse(input.ViewSpacePosition);
	float3 normal = normalize(cross(ddx, ddy));
	float normalScale = max(1.0 / 1000.0, sqrt(normal.z * -8 + 8));
	psout.Normal.xy = float2(0.5, 0.5) + normal.xy / normalScale;
	psout.Normal.zw = float2(0, 0);

	float3 worldNormal = normalize(input.VertexNormal);

	// // Swaps direction of the backfaces otherwise they seem to get lit from the wrong direction.
	if (!frontFace) worldNormal.xyz = -worldNormal.xyz;

	if (complex) {
		float3 normalColor = float4(TransformNormal(specColor.xyz), 1);
		// Inverting x as well as y seems to look more correct.
		normalColor.xy = -normalColor.xy;
		// world-space -> tangent-space -> world-space. 
		// This is because we don't have pre-computed tangents.
		worldNormal.xyz = normalize(mul(normalColor.xyz, CalculateTBN(worldNormal.xyz, input.WorldPosition.xyz, input.TexCoord.xy)));
	}

	float3 dirLightColor = DirLightColor.xyz;
	if (EnableDirLightFix)
	{
		dirLightColor *= SunlightScale;
	}

	dirLightColor *= shadowColor.x;

	float3 diffuseColor = 0;
	float3 specularColor = 0;

	float3 lightsDiffuseColor = 0;
	float3 lightsSpecularColor = 0;

	float dirLightAngle = dot(worldNormal.xyz, DirLightDirection.xyz);
	float3 dirDiffuseColor = dirLightColor * saturate(dirLightAngle);

	lightsDiffuseColor += dirDiffuseColor;

	float3 viewDirection = normalize(-input.WorldPosition.xyz);

	// Generated texture to simulate light transport.
	// Numerous attempts were made to use a more interesting algorithm however they were mostly fruitless.
	float3 subsurfaceColor = baseColor.xyz;

	// Applies lighting across the whole surface apart from what is already lit.
	lightsDiffuseColor += subsurfaceColor * dirLightColor * GetSoftLightMultiplier(dirLightAngle, SubsurfaceScatteringAmount);
	// Applies lighting from the opposite direction. Does not account for normals perpendicular to the light source.
	lightsDiffuseColor += subsurfaceColor * dirLightColor * saturate(-dirLightAngle) * SubsurfaceScatteringAmount;

	if (complex) {
		lightsSpecularColor = GetLightSpecularInput(DirLightDirection, viewDirection, worldNormal.xyz, dirLightColor.xyz, Glossiness);
		// Not physically accurate but grass will otherwise look too flat.
		lightsSpecularColor += subsurfaceColor * GetLightSpecularInput(-DirLightDirection, viewDirection, worldNormal.xyz, dirLightColor.xyz, Glossiness);
	}

	if (EnablePointLights) {
		uint counter = 0;
		uint light_count, dummy;
		lights.GetDimensions(light_count, dummy);
		for (uint light_index = 0; light_index < light_count; light_index++)
		{
			StructuredLight light = lights[light_index];
			if (light.active) {
				float3 lightDirection = light.positionWS[eyeIndex].xyz - input.WorldPosition.xyz;
				float lightDist = length(lightDirection);
				float intensityFactor = saturate(lightDist / light.radius);
				float intensityMultiplier = 1 - intensityFactor * intensityFactor;
				if (intensityMultiplier) {
					counter++;
					float3 lightColor = light.color.xyz;

					if (light.shadow) {
						lightColor *= shadowColor[light.mask];
					}

					float3 normalizedLightDirection = normalize(lightDirection);

					float lightAngle = dot(worldNormal.xyz, normalizedLightDirection.xyz);
					float3 lightDiffuseColor = lightColor * saturate(lightAngle.xxx);

					lightDiffuseColor += subsurfaceColor * lightColor * GetSoftLightMultiplier(lightAngle, SubsurfaceScatteringAmount);
					lightDiffuseColor += subsurfaceColor * lightColor * saturate(-lightAngle) * SubsurfaceScatteringAmount;

					if (complex) {
						lightsSpecularColor += GetLightSpecularInput(normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, Glossiness) * intensityMultiplier;
						lightsSpecularColor += subsurfaceColor * GetLightSpecularInput(-normalizedLightDirection, viewDirection, worldNormal.xyz, lightColor, Glossiness) * intensityMultiplier;
					}

					lightsDiffuseColor += lightDiffuseColor * intensityMultiplier;
				}
			}
		}
	}

	float3 directionalAmbientColor = mul(DirectionalAmbient, float4(worldNormal.xyz, 1));
	lightsDiffuseColor += directionalAmbientColor;

	diffuseColor += lightsDiffuseColor;

	float3 color = diffuseColor * baseColor.xyz * input.VertexColor.xyz;

	if (complex) {
		specularColor += lightsSpecularColor;
		specularColor *= specColor.w * SpecularStrength;
		color.xyz += specularColor;
	}

	psout.Albedo.xyz = color;
	psout.Albedo.w = 1;

#endif

	return psout;
}
#endif
