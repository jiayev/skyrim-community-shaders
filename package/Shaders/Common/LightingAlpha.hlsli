// Shared material alpha and coverage tests; included inside the Lighting pixel shader.
float alpha = baseColor.w;
#if defined(EMAT) && !defined(LANDSCAPE)
#	if defined(PARALLAX)
alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
#	elif defined(TRUE_PBR)
[branch] if (PBRParallax)
{
	alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
}
#	endif
#endif
#if defined(DO_ALPHA_TEST)
[branch] if ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::AdditionalAlphaMask) != 0)
{
	uint2 alphaMask = input.Position.xy;
	alphaMask.x = ((alphaMask.x << 2) & 12);
	alphaMask.x = (alphaMask.y & 3) | (alphaMask.x & ~3);
	const float maskValues[16] = {
		0.003922,
		0.533333,
		0.133333,
		0.666667,
		0.800000,
		0.266667,
		0.933333,
		0.400000,
		0.200000,
		0.733333,
		0.066667,
		0.600000,
		0.996078,
		0.466667,
		0.866667,
		0.333333,
	};

	float testTmp = 0;
	if (MaterialData.z - maskValues[alphaMask.x] < 0) {
		discard;
	}
}
else
#endif  // defined(DO_ALPHA_TEST)
{
	alpha *= MaterialData.z;
}
#if !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
alpha *= input.Color.w;
#endif  // !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
#if defined(DO_ALPHA_TEST)
#	if defined(DEPTH_WRITE_DECALS)
if (alpha - 0.0156862754 < 0) {
	discard;
}
alpha = saturate(1.05 * alpha);
#	endif  // DEPTH_WRITE_DECALS
if (alpha - AlphaTestRefRS < 0) {
	discard;
}
#endif  // DO_ALPHA_TEST
