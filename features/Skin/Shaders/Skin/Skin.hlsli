#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/PBR.hlsli"
#include "Common/SharedData.hlsli"

#define WATER_ROUGHNESS 0.1f
#define WATER_F0 0.02f

namespace Skin
{
#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
	cbuffer SkinPerGeometry : register(b7)
	{
		float4 skinPerGeometry;
	};
#endif

	Texture2D<float4> TexSkinDetailNormal : register(t72);

	struct SkinSurfaceProperties
	{
		float RoughnessPrimary;
		float RoughnessSecondary;
		float3 F0;
		float SecondarySpecIntensity;
		float Curvature;
		float3 Albedo;
		float Thickness;
		float3 SubsurfaceColor;
		float AO;
		float FuzzRoughness;
		float3 FuzzColor;
		float FuzzWeight;
		float Wetness;
	};

	SkinSurfaceProperties InitSkinSurfaceProperties()
	{
		SkinSurfaceProperties skin;
		skin.RoughnessPrimary = 0.55;
		skin.RoughnessSecondary = 0.35;
		skin.F0 = float3(0.0278, 0.0278, 0.0278);
		skin.SecondarySpecIntensity = 0.15;
		skin.Curvature = 0.0;
		skin.Albedo = float3(0.8, 0.6, 0.5);
		skin.Thickness = 0.15;
		skin.SubsurfaceColor = float3(0.6, 0.3, 0.2);
		skin.AO = 0.0;
		skin.FuzzRoughness = 0.35;
		skin.FuzzColor = float3(0.045, 0.045, 0.045);
		skin.FuzzWeight = 0.0;
		skin.Wetness = 0.0;
		return skin;
	}

	float CalculateCurvature(float3 N)
	{
		const float3 dNdx = ddx(N);
		const float3 dNdy = ddy(N);
		return length(float2(dot(dNdx, dNdx), dot(dNdy, dNdy)));
	}

	// [Jorge Jimenez, Diego Gutierrez 2015, "Separable Subsurface Scattering"]
	// https://www.iryoku.com/separable-sss/
	float3 SSSSTransmittance(float translucency, float sssWidth, float3 worldNormal, float3 light, float d)
	{
		/**
		* Calculate the scale of the effect.
		*/
		float scale = 8.25 * (1.0 - translucency) / sssWidth;

		/**
		* First we shrink the position inwards the surface to avoid artifacts:
		* (Note that this can be done once for all the lights)
		*/
		// float4 shrinkedPos = float4(worldPosition - 0.005 * worldNormal, 1.0);

		/**
		* Now we calculate the thickness from the light point of view:
		*/
		// float4 shadowPosition = mul(shrinkedPos, lightViewProjection);
		// float d1 = SSSSSampleShadowmap(shadowPosition.xy / shadowPosition.w).r; // 'd1' has a range of 0..1
		// float d2 = shadowPosition.z; // 'd2' has a range of 0..'lightFarPlane'
		// d1 *= lightFarPlane; // So we scale 'd1' accordingly:
		// float d = scale * abs(d1 - d2);
		d = scale * abs(d);  // Use the passed 'd' value instead of calculating it here.

		/**
		* Armed with the thickness, we can now calculate the color by means of the
		* precalculated transmittance profile.
		* (It can be precomputed into a texture, for maximum performance):
		*/
		float dd = -d * d;
		float3 profile = float3(0.233, 0.455, 0.649) * exp(dd / 0.0064) +
		                 float3(0.1, 0.336, 0.344) * exp(dd / 0.0484) +
		                 float3(0.118, 0.198, 0.0) * exp(dd / 0.187) +
		                 float3(0.113, 0.007, 0.007) * exp(dd / 0.567) +
		                 float3(0.358, 0.004, 0.0) * exp(dd / 1.99) +
		                 float3(0.078, 0.0, 0.0) * exp(dd / 7.41);

		/** 
		* Using the profile, we finally approximate the transmitted lighting from
		* the back of the object:
		*/
		return profile * saturate(0.3 + dot(light, -worldNormal));
	}

	float3 GetDualSpecularGGX(float AverageRoughness, float Lobe0Roughness, float Lobe1Roughness, float LobeMix, float3 SpecularColor, float NdotL, float NdotV, float NdotH, float VdotH, out float3 F)
	{
		float D = lerp(PBR::GetNormalDistributionFunctionGGX(Lobe0Roughness, NdotH), PBR::GetNormalDistributionFunctionGGX(Lobe1Roughness, NdotH), LobeMix);
		float G = PBR::GetVisibilityFunctionSmithJointApprox(AverageRoughness, NdotV, NdotL);
		F = PBR::GetFresnelFactorSchlick(SpecularColor, VdotH);

		return D * G * F;
	}

	// a contact shadow approximation, totally not physically correct; a riff on "Chan 2018, "Material Advances in Call of Duty: WWII" and "The Technical Art of Uncharted 4" http://advances.realtimerendering.com/other/2016/naughty_dog/NaughtyDog_TechArt_Final.pdf (microshadowing)"
	float ApproximateDirectOcculusion(float aoVisibility, float NdotL)
	{
		float aperture = rsqrt(1.0000001 - aoVisibility);
		NdotL += 0.1;  // when using bent normals, avoids overshadowing - bent normals are just approximation anyhow
		return saturate(NdotL * aperture);
	}

	void SkinDirectLightInput(
		out float3 diffuse,
		out float3 transmission,
		out float3 specular,
		PBR::LightProperties light,
		SkinSurfaceProperties skin,
		float3 N, float3 V, float3 L, float3 WetN)
	{
		diffuse = 0;
		transmission = 0;
		specular = 0;

		light.LightColor *= Math::PI;

		float3 H = normalize(V + L);
		const float oNdotL = dot(N, L);
		float NdotL = clamp(oNdotL, 1e-5, 1.0);
		float NdotV = saturate(abs(dot(N, V)) + 1e-5);
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(V, H));
		float VdotL = dot(V, L);
		float oVdotH = VdotH;

		if (skin.Wetness > 0.0 && dot(WetN, L) > 0.0) {
			float eta = 1.33;
			eta = lerp(eta, 1.0, saturate(dot(WetN, N)));
			float3 RefractedL = -refract(-L, WetN, 1.0 / eta);
			float3 RefractedV = -refract(-V, WetN, 1.0 / eta);
			NdotL = saturate(dot(N, RefractedL));
			NdotV = saturate(abs(dot(N, RefractedV)) + 1e-5);
			float3 RefractedH = normalize(RefractedV + RefractedL);
			NdotH = saturate(dot(N, RefractedH));
			VdotH = saturate(dot(RefractedV, RefractedH));
			VdotL = dot(RefractedV, RefractedL);
		}

		light.LightColor *= ApproximateDirectOcculusion(skin.AO, NdotL);

		float averageRoughness = lerp(skin.RoughnessPrimary, skin.RoughnessSecondary, skin.SecondarySpecIntensity);

		diffuse += light.LightColor * NdotL * PBR::GetDiffuseDirectLightMultiplierChan(averageRoughness, NdotV, NdotL, VdotH, NdotH);

		float3 F;
		float3 F0 = skin.F0 * (1 - skin.Curvature);

		specular += GetDualSpecularGGX(averageRoughness, skin.RoughnessPrimary, skin.RoughnessSecondary, skin.SecondarySpecIntensity, F0, NdotL, NdotV, NdotH, VdotH, F) * light.LightColor * NdotL;

		float2 specularBRDF = PBR::GetEnvBRDFApproxLazarov(averageRoughness, NdotV);
		specular *= 1 + F0 * (1 / (specularBRDF.x + specularBRDF.y) - 1);

		if (skin.FuzzWeight > 0.0) {
			float3 FuzzF0 = skin.FuzzColor * (1 - skin.Curvature);
			float3 fuzzSpecular = PBR::GetSpecularDirectLightMultiplierMicroflakes(skin.FuzzRoughness, FuzzF0, NdotL, NdotV, NdotH, VdotH) * light.LightColor * NdotL;
			float2 fuzzSpecularBRDF = PBR::GetEnvBRDFApproxLazarov(skin.FuzzRoughness, NdotV);
			fuzzSpecular *= 1 + skin.FuzzColor * (1 / (fuzzSpecularBRDF.x + fuzzSpecularBRDF.y) - 1);

			specular += fuzzSpecular * skin.FuzzWeight;
		}

		if (skin.Wetness > 0.0) {
			const float WNdotL = saturate(dot(WetN, L));
			const float WNdotV = saturate(abs(dot(WetN, V)) + 1e-5);
			const float WNdotH = saturate(dot(WetN, H));
			float3 wetnessF;
			float3 wetSpecular = PBR::GetSpecularDirectLightMultiplierMicrofacet(WATER_ROUGHNESS, WATER_F0, WNdotL, WNdotV, WNdotH, oVdotH, wetnessF) * light.LightColor * WNdotL;
			float2 wetSpecularBRDF = PBR::GetEnvBRDFApproxLazarov(WATER_ROUGHNESS, WNdotV);
			wetSpecular *= 1 + WATER_F0 * (1 / (wetSpecularBRDF.x + wetSpecularBRDF.y) - 1);
			const float waterTransmission = 1 - wetnessF.x;
			specular *= waterTransmission;
			specular += wetSpecular;
			diffuse *= waterTransmission;
		}
	}

	void SkinIndirectLobeWeights(
		out float3 diffuseWeight,
		out float3 specularWeight,
		SkinSurfaceProperties skin,
		float3 N, float3 V, float3 VN, float3 WetN)
	{
		float NdotV = saturate(dot(N, V));
		if (skin.Wetness > 0.0) {
			float eta = 1.33;
			eta = lerp(eta, 1.0, saturate(dot(WetN, N)));
			float3 RefractedV = -refract(-V, WetN, 1.0 / eta);
			NdotV = saturate(dot(N, RefractedV));
		}

		float averageRoughness = lerp(skin.RoughnessPrimary, skin.RoughnessSecondary, skin.SecondarySpecIntensity);

		float2 specularBRDF = PBR::GetEnvBRDFApproxLazarov(averageRoughness, NdotV);
		specularWeight = skin.F0 * specularBRDF.x + specularBRDF.y;

		if (skin.Wetness > 0.0) {
			const float WNdotV = saturate(abs(dot(WetN, V)) + 1e-5);
			float2 wetSpecularBRDF = PBR::GetEnvBRDFApproxLazarov(WATER_ROUGHNESS, WNdotV);
			float3 wetSpecular = WATER_F0 * wetSpecularBRDF.x + wetSpecularBRDF.y;
			wetSpecular *= 1 + WATER_F0 * (1 / (wetSpecularBRDF.x + wetSpecularBRDF.y) - 1);
			const float waterTransmission = 1 - (WATER_F0 * wetSpecularBRDF.x + wetSpecularBRDF.y);
			specularWeight = specularWeight * waterTransmission + wetSpecular;
		}

		diffuseWeight = skin.Albedo * (1.0 - specularWeight);

		specularWeight *= 1 + skin.F0 * (1 / (specularBRDF.x + specularBRDF.y) - 1);

		float3 R = reflect(-V, N);
		float horizon = min(1.0 + dot(R, VN), 1.0);
		horizon *= horizon;
		specularWeight *= horizon;

		float3 diffuseAO = skin.AO;
		float3 specularAO = PBR::SpecularAOLagarde(NdotV, skin.AO, averageRoughness);

		diffuseAO = PBR::MultiBounceAO(skin.Albedo, diffuseAO.x).y;
		specularAO = PBR::MultiBounceAO(skin.F0, specularAO.x).y;

		diffuseWeight *= diffuseAO;
		specularWeight *= specularAO;

		specularWeight *= 1 - skin.Curvature;
	}

	// https://blog.selfshadow.com/publications/blending-in-detail/
	// geometric normal s, a base normal t and a secondary (or detail) normal u
	float3 ReorientNormal(float3 u, float3 t, float3 s)
	{
		// Build the shortest-arc quaternion
		float4 q = float4(cross(s, t), dot(s, t) + 1) / sqrt(2 * (dot(s, t) + 1));

		// Rotate the normal
		return u * (q.w * q.w - dot(q.xyz, q.xyz)) + 2 * q.xyz * dot(q.xyz, u) + 2 * q.w * cross(q.xyz, u);
	}

	// for when s = (0,0,1)
	float3 ReorientNormal(float3 n1, float3 n2)
	{
		n1 += float3(0, 0, 1);
		n2 *= float3(-1, -1, 1);

		return n1 * dot(n1, n2) / n1.z - n2;
	}

	float3x3 ReconstructTBN(float3 worldPos, float3 worldNormal, float2 uv)
	{
		float3 dFdx = ddx(worldPos);
		float3 dFdy = ddy(worldPos);
		float2 dUVdx = ddx(uv);
		float2 dUVdy = ddy(uv);
		float3 tangent = normalize(dFdx * dUVdy.y - dFdy * dUVdx.y);
		float3 bitangent = normalize(dFdy * dUVdx.x - dFdx * dUVdy.x);
		tangent = normalize(tangent - worldNormal * dot(worldNormal, tangent));
		bitangent = normalize(bitangent - worldNormal * dot(worldNormal, bitangent));
		
		return float3x3(tangent, bitangent, normalize(worldNormal));
	}

	float3 CalculateNormalFromHeight(float height, float heightScale, float2 uv)
	{
		float dHdx = ddx(height);
		float dHdy = ddy(height);
		float2 dUVdx = ddx(uv);
		float2 dUVdy = ddy(uv);

		float det = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
		if (det == 0.0f) {
			return float3(0, 0, 1); // Avoid division by zero
		}

		float dHdx_Tex = (dHdx * dUVdy.y - dHdy * dUVdx.y) / det;
		float dHdy_Tex = (dHdy * dUVdx.x - dHdx * dUVdy.x) / det;
		float3 normal = float3(-dHdx_Tex, -dHdy_Tex, 0);
		return normal * heightScale + float3(0, 0, 1);
	}

	float FBM(float2 uv, float base_scale, int octaves, float lacunarity, float persistence, float z_offset_multiplier)
	{
		float total = 0.0;
		float frequency = base_scale;
		float amplitude = 1.0;
		float max_amplitude = 0.0;
		for (int i = 0; i < octaves; i++)
		{
			total += amplitude * (Random::perlinNoise(float3(uv * frequency, (float)i * z_offset_multiplier)) + 1.0) * 0.5;
			
			max_amplitude += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		if (max_amplitude > 0.0) {
			return total / max_amplitude;
		}
		return 0.0;
	}

	float PerlinNoise(float2 uv, float scale, float lacunarity, float persistence, float strength)
	{
		if (strength <= 0.001f)
		{
			return 0.0f;
		}
		if (strength >= 0.999f)
		{
			return 1.0f;
		}
		int octaves = 5;
		float z_offset_multiplier = 7.375f;

		float noise_value = FBM(uv, scale, octaves, lacunarity, persistence, z_offset_multiplier);

		float dynamic_threshold = 1.0f - strength;

		float sweat_intensity = saturate((noise_value - dynamic_threshold) / strength);

		sweat_intensity = pow(sweat_intensity, 1.5f);

		if (strength > 0.8f)
		{
			sweat_intensity = sweat_intensity * saturate(0.99f - (strength - 0.8f) * 5.0f) + (strength - 0.8f) * 5.0f;
		}
		return saturate(sweat_intensity);
	}

	float GetWetness(float z)
	{
		float waterWet = 0.0f;
		if (z <= skinPerGeometry.z + skinPerGeometry.w)
		{
			waterWet = skinPerGeometry.y;
		}

		float sweatWet = skinPerGeometry.x;
		return clamp(waterWet + sweatWet, 0.0f, 2.0f);
	}
}