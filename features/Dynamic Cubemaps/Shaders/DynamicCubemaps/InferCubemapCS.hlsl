TextureCube EnvCaptureTexture : register(t0);

RWTexture2DArray<float4> EnvInferredTexture : register(u0);

SamplerState LinearSampler : register(s0);

// Calculate normalized sampling direction vector based on current fragment coordinates.
// This is essentially "inverse-sampling": we reconstruct what the sampling vector would be if we wanted it to "hit"
// this particular fragment in a cubemap.
float3 GetSamplingVector(uint3 ThreadID, in RWTexture2DArray<float4> OutputTexture)
{
	float width = 0.0f;
	float height = 0.0f;
	float depth = 0.0f;
	OutputTexture.GetDimensions(width, height, depth);

	float2 st = ThreadID.xy / float2(width, height);
	float2 uv = 2.0 * float2(st.x, 1.0 - st.y) - 1.0;

	// Select vector based on cubemap face index.
	float3 result = float3(0.0f, 0.0f, 0.0f);
	switch (ThreadID.z) {
	case 0:
		result = float3(1.0, uv.y, -uv.x);
		break;
	case 1:
		result = float3(-1.0, uv.y, uv.x);
		break;
	case 2:
		result = float3(uv.x, 1.0, -uv.y);
		break;
	case 3:
		result = float3(uv.x, -1.0, uv.y);
		break;
	case 4:
		result = float3(uv.x, uv.y, 1.0);
		break;
	case 5:
		result = float3(-uv.x, uv.y, -1.0);
		break;
	}
	return normalize(result);
}

[numthreads(32, 32, 1)] void main(uint3 ThreadID
								  : SV_DispatchThreadID) {
	float3 uv = GetSamplingVector(ThreadID, EnvInferredTexture);
	float4 color = EnvCaptureTexture.SampleLevel(LinearSampler, uv, 0);
	uint mipLevel = 1;
	float brightness = 4.0;
	float3 startUV = uv;
	while (color.w < 1.0 && mipLevel <= 10) {
		float4 tempColor = 0.0;
		if (mipLevel < 10) {
			tempColor = EnvCaptureTexture.SampleLevel(LinearSampler, uv, mipLevel);
		} else {  // The lowest cubemap mip is 6x6, a 1x1 result needs to be calculated
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(-1.0, 0.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(1.0, 0.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, -1.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 1.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 0.0, -1.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 0.0, 1.0), 0.5), 9);
		}
		tempColor *= brightness;

		if (color.w + tempColor.w > 1.0) {
			float alphaDiff = 1.0 - color.w;
			tempColor.xyz *= alphaDiff / tempColor.w;
			color.xyzw += tempColor;
			break;
		} else {
			color.xyzw += tempColor;
		}

		mipLevel++;
		brightness *= 4.0;
	}
	color.rgb = pow(color.rgb, 1.0 / 2.2);
	EnvInferredTexture[ThreadID] = color;
}
