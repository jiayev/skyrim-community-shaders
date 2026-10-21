#define COMPUTESHADER

cbuffer Parameters : register(b1)
{
	uint mip;
	uint slice;
	uint channel;
	uint volume;
	float2 displayRange;
	float2 padding;
};

Texture2D<float4> Image2D : register(t0);
Texture3D<float4> Image3D : register(t1);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	uint2 size;
	Output.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	float4 value;
	if (volume != 0u)
		value = Image3D.Load(int4(tid, slice, mip));
	else
		value = Image2D.Load(int3(tid, mip));
	float3 color = value.rgb;
	if (channel != 0u)
		color = value[min(channel - 1u, 3u)].xxx;
	Output[tid] = float4(saturate((color - displayRange.x) / max(displayRange.y - displayRange.x, 1e-6)), 1.0);
}
