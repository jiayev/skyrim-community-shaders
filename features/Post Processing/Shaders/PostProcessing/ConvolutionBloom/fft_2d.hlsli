/// FFT 2D Helpers - Data copy between textures and local FFT registers
/// Handles 2-channel complex packing: LocalBuffer[0] = .xy, LocalBuffer[1] = .zw

#pragma once

// --- Luma and Pre-filter ---

float ConvertToLuma(in float3 Color)
{
	return dot(Color, float3(0.2126, 0.7152, 0.0722));
}

// Boost bright pixels above threshold (pre-filter for bloom)
// Filter: .x = min luma threshold, .y = max luma clamp, .z = multiplier
void FilterBrightPixels(in float3 Filter, inout Complex LocalBuffer[2][RADIX])
{
	[unroll] for (uint r = 0; r < RADIX; ++r)
	{
		float4 Color = float4(LocalBuffer[0][r], LocalBuffer[1][r]);
		float Luma = ConvertToLuma(Color.rgb);
		if (Luma > Filter.x) {
			float TargetLuma = Filter.z * (Luma - Filter.x) + Filter.x;
			TargetLuma = min(TargetLuma, Filter.y);
			Color.rgb *= (TargetLuma / max(Luma, 0.0001));
			LocalBuffer[0][r] = Color.xy;
			LocalBuffer[1][r] = Color.zw;
		}
	}
}
