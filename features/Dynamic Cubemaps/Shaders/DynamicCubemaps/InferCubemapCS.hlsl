TextureCube EnvCaptureTexture : register(t0);

RWTexture2DArray<float4> EnvInferredTexture : register(u0);
RWTexture2DArray<float4> EnvReflectionsTexture : register(u1);

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

float3 sRGB2Lin(float3 color)
{
	return color > 0.04045 ? pow(color / 1.055 + 0.055 / 1.055, 2.4) : color / 12.92;
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
	return frac(sin(dot(p, float3(127.1, 311.7, 321.4))) * 43758.5453123);
}

float noise(in float3 p)
{
	float3 i = floor(p);
	float3 f = frac(p);
	f *= f * (3. - 2. * f);

	float2 c = float2(0, 1);

	return lerp(
		lerp(lerp(hash(i + c.xxx), hash(i + c.yxx), f.x),
			lerp(hash(i + c.xyx), hash(i + c.yyx), f.x),
			f.y),
		lerp(lerp(hash(i + c.xxy), hash(i + c.yxy), f.x),
			lerp(hash(i + c.xyy), hash(i + c.yyy), f.x),
			f.y),
		f.z);
}

/////

[numthreads(32, 32, 1)] void main(uint3 ThreadID
								  : SV_DispatchThreadID) {
	float3 uv = GetSamplingVector(ThreadID, EnvInferredTexture);
	float4 color = EnvCaptureTexture.SampleLevel(LinearSampler, uv, 0);

	float mipLevel = 0.0;

#if !defined(REFLECTIONS)
	float k = 1.5;
	float brightness = k;
#endif

	while (color.w <= 1.0 && mipLevel <= 10) {
		mipLevel++;

		float4 tempColor = 0.0;
		if (mipLevel < 10) {
			tempColor = EnvCaptureTexture.SampleLevel(LinearSampler, uv, mipLevel);
		} else {
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, float3(-1.0, 0.0, 0.0), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, float3(1.0, 0.0, 0.0), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, float3(0.0, -1.0, 0.0), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, float3(0.0, 1.0, 0.0), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, float3(0.0, 0.0, -1.0), 9);
			tempColor += EnvCaptureTexture.SampleLevel(LinearSampler, float3(0.0, 0.0, 1.0), 9);
		}

#if !defined(REFLECTIONS)
		tempColor *= brightness;
		brightness *= k;
#endif

		if ((color.w + tempColor.w) > 1.0) {
			mipLevel -= color.w;
			float alphaDiff = 1.0 - color.w;
			tempColor.xyzw *= alphaDiff / tempColor.w;
			color.xyzw += tempColor;
			break;
		} else {
			color.xyzw += tempColor;
		}
	}

#if defined(REFLECTIONS)
	color.rgb = lerp(color.rgb, sRGB2Lin(EnvReflectionsTexture[ThreadID]), saturate(mipLevel * (1.0 / 10.0)));
#else
	color.rgb = lerp(color.rgb, max(float3(0.0, 0.0, 0.0), color.rgb * noise(uv * 5.0)), mipLevel * (1.0 / 10.0));
#endif

	color.rgb = Lin2sRGB(color.rgb);
	EnvInferredTexture[ThreadID] = max(0, color);
}
