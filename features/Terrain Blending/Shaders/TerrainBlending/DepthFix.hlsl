RWTexture2D<float> TerrainOffsetTexture : register(u0);
Texture2D<unorm float> MainDepthTexture : register(t0);
Texture2D<unorm float> MainDepthTextureAfterTerrain : register(t1);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) 
{
	float mainDepth = MainDepthTexture[DTid.xy];
	float mainDepthAfterTerrain = MainDepthTextureAfterTerrain[DTid.xy];

	float fixedDepth = mainDepth;

	if (mainDepth < mainDepthAfterTerrain)
		fixedDepth = 1;

	TerrainOffsetTexture[DTid.xy] = fixedDepth;
}
