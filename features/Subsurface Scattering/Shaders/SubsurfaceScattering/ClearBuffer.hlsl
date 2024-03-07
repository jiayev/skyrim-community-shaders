RWTexture2D<unorm float4> NormalTexture : register(u0);

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	float4 normals = NormalTexture[DTid.xy];
	NormalTexture[DTid.xy] = float4(normals.xy, 0.0, normals.w);
}
