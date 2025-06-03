namespace GrassLighting
{
	float3 F_Schlick(float3 F0, float VdotH)
	{
		float Fc = pow(1 - VdotH, 5);
		return Fc + (1 - Fc) * F0;
	}

	float D_GGX(float roughness, float NdotH)
	{
		float a2 = pow(roughness, 4);
		float d = max((NdotH * a2 - NdotH) * NdotH + 1, 1e-5);
		return a2 / (Math::PI * d * d);
	}

	float G_SmithJointApprox(float roughness, float NdotL, float NdotV)
	{
		float a = roughness * roughness;
		float visSmithV = NdotL * (NdotV * (1 - a) + a);
		float visSmithL = NdotV * (NdotL * (1 - a) + a);
		return 0.5 * rcp(visSmithV + visSmithL);
	}

	float3 GetLightSpecularInput(float3 L, float3 V, float3 N, float3 lightColor, float roughness, float3 F0)
	{
		float3 H = normalize(V + L);
		float NdotL = saturate(dot(N, L));
		float NdotV = saturate(dot(N, V));
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(V, H));

		float D = D_GGX(roughness, NdotH);
		float G = G_SmithJointApprox(roughness, NdotL, NdotV);
		float3 F = F_Schlick(F0, VdotH);
		float3 specular = D * G * F;
		return specular * lightColor * NdotL;
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
}
