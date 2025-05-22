// Do expotential blur on the linear depth texture

Texture2D<float4> LinearDepth : register(t0);

RWTexture2D<float4> outBlurredDepth : register(u0);

SamplerState linearSampler : register(s0);

cbuffer blurBuffer : register(b1)
{
	uint MipLevel;
	float Scale;
	uint ResX;
	uint ResY;
};

#define BLUR_RADIUS 2

[numthreads(4, 4, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
	if (MipLevel == 0) {
		outBlurredDepth[dtid] = LinearDepth[dtid];
		return;
	}
	float4 sum = 0;
	float totalWeight = 0;

	float2 texCoord = (dtid + 0.5) / float2(ResX, ResY);

	for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; i++) {
		for (int j = -BLUR_RADIUS; j <= BLUR_RADIUS; j++) {
			float2 offset = float2(i, j) * (1.0 / float2(ResX, ResY));
			float4 depth = LinearDepth.SampleLevel(linearSampler, texCoord + offset, 0);
			float weight = exp(-abs(i * j));
			sum += depth * weight;
			totalWeight += weight;
		}
	}

	outBlurredDepth[dtid] = sum / totalWeight;
}