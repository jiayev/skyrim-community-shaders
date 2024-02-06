struct PerPassCloudShadow
{
	uint EnableCloudShadows;

	float CloudHeight;
	float PlanetRadius;

	float EffectMix;

	float TransparencyPower;
	float AbsorptionAmbient;

	float RcpHPlusR;
};

StructuredBuffer<PerPassCloudShadow> perPassCloudShadow : register(t23);
TextureCube<float4> cloudShadows : register(t40);

float3 getCloudShadowSampleDir(float3 rel_pos, float3 eye_to_sun)
{
	float r = perPassCloudShadow[0].PlanetRadius;
	float3 p = (rel_pos + float3(0, 0, r)) * perPassCloudShadow[0].RcpHPlusR;
	float dotprod = dot(p, eye_to_sun);
	float lengthsqr = dot(p, p);
	float t = -dotprod + sqrt(dotprod * dotprod - dot(p, p) + 1);
	float3 v = (p + eye_to_sun * t) * (r + perPassCloudShadow[0].CloudHeight) - float3(0, 0, r);
	v = normalize(v);
	return v;
}

float3 getCloudShadowSampleDirFlatEarth(float3 rel_pos, float3 eye_to_sun)
{
	float3 p = rel_pos / perPassCloudShadow[0].CloudHeight;
	float dotprod = dot(p, eye_to_sun);
	float t = -dotprod + sqrt(dotprod * dotprod - dot(p, p) + 1);
	float3 v = p + eye_to_sun * t;
	v = normalize(v);  // optional
	return v;
}

float3 getCloudShadowMult(float3 rel_pos, float3 eye_to_sun, SamplerState samp)
{
	// float3 cloudSampleDir = getCloudShadowSampleDirFlatEarth(rel_pos, eye_to_sun).xyz;
	float3 cloudSampleDir = getCloudShadowSampleDir(rel_pos, eye_to_sun).xyz;

	float4 cloudCubeSample = cloudShadows.Sample(samp, cloudSampleDir);
	float alpha = pow(saturate(cloudCubeSample.w), perPassCloudShadow[0].TransparencyPower);

	return lerp(1.0, 1.0 - alpha, perPassCloudShadow[0].EffectMix);
}