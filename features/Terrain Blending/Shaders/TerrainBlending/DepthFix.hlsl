RWTexture2D<float> TerrainOffsetTexture : register(u0);
Texture2D<unorm float> MainDepthTexture : register(t0);
Texture2D<unorm float> TerrainDepthTexture : register(t1);
Texture2D<unorm float> MainDepthTextureNoGrass : register(t2);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {

	float mainDepth = MainDepthTexture[DTid.xy];
	float mainDepthNoGrass = MainDepthTextureNoGrass[DTid.xy];

	float mixedDepth = mainDepth;

	if (mainDepth != mainDepthNoGrass)
		mixedDepth = TerrainDepthTexture[DTid.xy];

	TerrainOffsetTexture[DTid.xy] = mixedDepth;
}
