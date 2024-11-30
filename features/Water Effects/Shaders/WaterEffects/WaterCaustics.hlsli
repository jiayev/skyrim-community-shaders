Texture2D<float4> WaterCaustics : register(t70);

namespace WaterEffects
{
	float2 PanCausticsUV(float2 uv, float speed, float tiling)
	{
		return frac((float2(1, 0) * Timer * speed) + (uv * tiling));
	}

	float3 SampleCaustics(float2 uv)
	{
		return WaterCaustics.Sample(SampColorSampler, uv).r;
	}

	float3 ComputeCaustics(float4 waterData, float3 worldPosition, uint eyeIndex)
	{
		float causticsDistToWater = waterData.w - worldPosition.z;
		float shoreFactorCaustics = saturate(causticsDistToWater / 64.0);

		if (shoreFactorCaustics > 0.0) {
			float causticsFade = 1.0 - saturate(causticsDistToWater / 1024.0);
			causticsFade *= causticsFade;

			float2 causticsUV = (worldPosition.xy + CameraPosAdjust[eyeIndex].xy) * 0.005;

			float2 causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0);
			float2 causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5);

			float3 causticsHigh = 1.0;

			if (causticsFade > 0.0)
				causticsHigh = min(SampleCaustics(causticsUV1), SampleCaustics(causticsUV2)) * 4.0;

			causticsUV *= 0.5;

			causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.1, 1.0);
			causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.1, -0.5);

			float3 causticsLow = 1.0;

			if (causticsFade < 1.0)
				causticsLow = min(SampleCaustics(causticsUV1), SampleCaustics(causticsUV2)) * 4.0;

			return lerp(causticsLow, causticsHigh, causticsFade);
		}

		return 1.0;
	}
}
