cbuffer LocalNdfParameters : register(b1)
{
	float4 fieldRect;
	float4 centerSize;
	float4 rotationWeights;
	float4 heightFeather;
	float4 altitude;
	float4 auxiliary;
	uint hasMask;
	uint modelingAlpha;
	uint dimension;
	uint padding;
};

Texture2D<float2> HeightInput : register(t0);
Texture2D<float4> ModelingInput : register(t1);
Texture2D<float> MaskInput : register(t2);
Texture2D<float4> PreviousHeight : register(t3);
Texture2D<float4> PreviousModeling : register(t4);
SamplerState ClampSampler : register(s0);
RWTexture2D<float4> HeightOutput : register(u0);
RWTexture2D<float4> ModelingOutput : register(u1);

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= dimension))
		return;
	const float4 previousHeight = PreviousHeight[pixel];
	const float4 previousModel = PreviousModeling[pixel];
	HeightOutput[pixel] = previousHeight;
	ModelingOutput[pixel] = previousModel;
	const float2 world = fieldRect.xy + ((pixel + 0.5) / dimension) / fieldRect.zw;
	const float2 delta = world - centerSize.xy;
	const float2 local = float2(dot(delta, rotationWeights.xy), dot(delta, float2(-rotationWeights.y, rotationWeights.x)));
	const float2 uv = local / centerSize.zw + 0.5;
	if (any(uv <= 0.0) || any(uv >= 1.0))
		return;
	uint width, height;
	ModelingInput.GetDimensions(width, height);
	const float2 footprint = float2(width, height) / centerSize.zw / (dimension * fieldRect.zw);
	const float mip = max(log2(max(footprint.x, footprint.y)), 0.0);
	const float4 model = saturate(ModelingInput.SampleLevel(ClampSampler, uv, mip));
	const float2 heights = saturate(HeightInput.SampleLevel(ClampSampler, uv, mip));
	float mask = modelingAlpha != 0u ? model.a : 1.0;
	if (hasMask != 0u) {
		MaskInput.GetDimensions(width, height);
		const float2 maskFootprint = float2(width, height) / centerSize.zw / (dimension * fieldRect.zw);
		mask *= saturate(MaskInput.SampleLevel(ClampSampler, uv, max(log2(max(maskFootprint.x, maskFootprint.y)), 0.0)));
	}
	const float2 edge = min(uv, 1.0 - uv) * centerSize.zw;
	mask *= heightFeather.z > 0.0 ? smoothstep(0.0, heightFeather.z, min(edge.x, edge.y)) : 1.0;
	const float2 weights = mask * rotationWeights.zw;
	const float2 normalizedHeight = (heightFeather.x + heights * heightFeather.y - altitude.x) / altitude.y;
	const float shaping = saturate(lerp(auxiliary.z, auxiliary.w, saturate((model.g - auxiliary.x) / (auxiliary.y - auxiliary.x))));
	HeightOutput[pixel] = float4(normalizedHeight * weights.y + previousHeight.rg * (1.0 - weights.y),
		weights.y + previousHeight.b * (1.0 - weights.y), shaping * weights.x + previousHeight.a * (1.0 - weights.x));
	ModelingOutput[pixel] = float4(model.rgb * weights.x + previousModel.rgb * (1.0 - weights.x),
		weights.x + previousModel.a * (1.0 - weights.x));
}
