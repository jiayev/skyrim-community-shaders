cbuffer PerFrameDeferredShared : register(b11)
{
	float4 BufferDim;
	float4 CameraData;
	row_major float3x4 DirectionalAmbient;
	uint FrameCount;
	uint FrameCountAlwaysActive;
};

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

float InterleavedGradientNoise(float2 uv)
{
	// Temporal factor
	float frameStep = float(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}