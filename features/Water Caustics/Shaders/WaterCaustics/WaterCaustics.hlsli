struct PerPassWaterCaustics
{
	bool EnableWaterCaustics;
	float pad[3];
};

StructuredBuffer<PerPassWaterCaustics> perPassWaterCaustics : register(t71);

#if defined(WATER)

Texture2D<float4> WaterCaustics : register(t70);

float2 PanCausticsUV(float2 uv, float speed, float tiling)
{
	return (float2(1, 0) * lightingData[0].Timer * speed) + (uv * tiling);
}

float3 SampleCaustics(float2 uv, float split)
{
	float2 uv1 = uv + float2(split, split);
	float2 uv2 = uv + float2(split, -split);
	float2 uv3 = uv + float2(-split, -split);

	float r = WaterCaustics.Sample(SampColorSampler, uv1).r;
	float g = WaterCaustics.Sample(SampColorSampler, uv2).r;
	float b = WaterCaustics.Sample(SampColorSampler, uv3).r;

	return float3(r, g, b);
}

float3 ComputeWaterCaustics(float2 uv)
{
	float2 causticsUV = uv * 5.0;

	float2 causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0);
	float2 causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5);

	float3 caustics1 = SampleCaustics(causticsUV1, 0.001);
	float3 caustics2 = SampleCaustics(causticsUV2, 0.002);

	float3 caustics = min(caustics1, caustics2) * 5.0;

	return caustics;
}

#else

Texture2D<float4> WaterCaustics : register(t70);

float2 PanCausticsUV(float2 uv, float speed, float tiling)
{
	return (float2(1, 0) * lightingData[0].Timer * speed) + (uv * tiling);
}

float3 SampleCaustics(float2 uv, float split)
{
	float2 uv1 = uv + float2(split, split);
	float2 uv2 = uv + float2(split, -split);
	float2 uv3 = uv + float2(-split, -split);

	float r = WaterCaustics.Sample(SampColorSampler, uv1).r;
	float g = WaterCaustics.Sample(SampColorSampler, uv2).r;
	float b = WaterCaustics.Sample(SampColorSampler, uv3).r;

	return float3(r, g, b);
}

float3 ComputeWaterCaustics(float3 waterHeight, float3 worldPosition, float3 worldSpaceNormal)
{
	float causticsDistToWater = waterHeight - worldPosition.z;
	float shoreFactorCaustics = saturate(causticsDistToWater / 64.0);

	float causticsFade = 1.0 - saturate(causticsDistToWater / 1024.0);
	causticsFade *= causticsFade;

	float2 causticsUV = (worldPosition.xyz + CameraPosAdjust[0].xyz) * 0.005;

	float2 causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0);
	float2 causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5);

	float3 caustics1 = SampleCaustics(causticsUV1, 0.001);
	float3 caustics2 = SampleCaustics(causticsUV2, 0.002);

	float3 causticsHigh = min(caustics1, caustics2) * 5.0;

	causticsUV *= 0.5;

	causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.1, 1.0);
	causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.1, -0.5);

	caustics1 = SampleCaustics(causticsUV1, 0.001);
	caustics2 = SampleCaustics(causticsUV2, 0.002);

	float3 causticsLow = sqrt(min(caustics1, caustics2));

	float upAmount = (worldSpaceNormal.z + 1.0) * 0.5;

	float3 caustics = lerp(causticsLow, causticsHigh, causticsFade);
	caustics = lerp(causticsLow, caustics, upAmount);
	return lerp(1.0, caustics, shoreFactorCaustics * upAmount);
}

#endif