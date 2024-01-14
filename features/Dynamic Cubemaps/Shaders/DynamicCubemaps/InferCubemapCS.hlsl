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

float3 Lin2sRGB(float3 color)
{
	return color > 0.0031308 ? 1.055 * pow(color, 1.0 / 2.4) - 0.055 : 12.92 * color;
}


// https://www.shadertoy.com/view/XdlGzX
// Nikos Papadopoulos, 4rknova / 2013
// Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported License.

float hash(in float3 p)
{
    return frac(sin(dot(p,float3(127.1,311.7, 321.4)))*43758.5453123);
}

float noise(in float3 p)
{
    float3 i = floor(p);
	float3 f = frac(p); 
	f *= f * (3.-2.*f);

    float2 c = float2(0,1);

    return lerp(
		lerp(lerp(hash(i + c.xxx), hash(i + c.yxx),f.x),
			lerp(hash(i + c.xyx), hash(i + c.yyx),f.x),
			f.y),
		lerp(lerp(hash(i + c.xxy), hash(i + c.yxy),f.x),
			lerp(hash(i + c.xyy), hash(i + c.yyy),f.x),
			f.y),
		f.z);
}

/////

[numthreads(32, 32, 1)] void main(uint3 ThreadID
								  : SV_DispatchThreadID) {
	float3 uv = GetSamplingVector(ThreadID, EnvInferredTexture);
	float4 color = EnvCaptureTexture.SampleLevel(LinearSampler, uv, 0);
	uint mipLevel = 1;
	while (color.w < 1.0 && mipLevel <= 11) {
		float4 tempColor = 0.0;
		if (mipLevel < 10) {
			tempColor = EnvCaptureTexture.SampleLevel(LinearSampler, uv, mipLevel);
		} else if (mipLevel < 11) {  // The lowest cubemap mip is 6x6, a 1x1 result needs to be calculated
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(-1.0, 0.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(1.0, 0.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, -1.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 1.0, 0.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 0.0, -1.0), 0.5), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 0.0, 1.0), 0.5), 9);
		} else {
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(-1.0, 0.0, 0.0), 1), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(1.0, 0.0, 0.0), 1), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, -1.0, 0.0), 1), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 1.0, 0.0), 1), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 0.0, -1.0), 1), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, lerp(uv, float3(0.0, 0.0, 1.0), 1), 9);
		}

		if (color.w + tempColor.w > 1.0) {
			float alphaDiff = 1.0 - color.w;
			tempColor.xyz *= alphaDiff / tempColor.w;
			color.xyzw += tempColor;
		} else {
			color.xyzw += tempColor;
		}

		mipLevel++;
	}
	color.rgb = lerp(color.rgb, color.rgb * noise(uv * 4), smoothstep(1.0, 12.0, mipLevel));
	color.rgb = Lin2sRGB(color.rgb);
	EnvInferredTexture[ThreadID] = color;
}
