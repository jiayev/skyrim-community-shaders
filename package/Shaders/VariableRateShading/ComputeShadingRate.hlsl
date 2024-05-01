RWTexture2D<uint> vrsSurface : register(u0);
Texture2D<float4> nasDataSurface : register(t0);

[numthreads(32, 32, 1)] void main(uint3 DispatchThreadID
								  : SV_DispatchThreadID, uint3 GroupThreadID
								  : SV_GroupThreadID, uint3 GroupID
								  : SV_GroupID) {
	float4 nasData = nasDataSurface[DispatchThreadID.xy];

	float2 mVec = abs(nasData.zw);

	// Error scalers (equations from the I3D 2019 paper)
	// bhv for half rate, bqv for quarter rate
	float2 bhv = pow(1.0 / (1 + pow(1.05 * mVec, 3.1)), 0.35);
	float2 bqv = 2.13 * pow(1.0 / (1 + pow(0.55 * mVec, 2.41)), 0.49);

	// Sample block error data from NAS data pass and apply the error scalars
	float2 diff = nasData.xy;
	float2 diff2 = diff * bhv;
	float2 diff4 = diff * bqv;

	uint screenWidth, screenHeight;
	nasDataSurface.GetDimensions(screenWidth, screenHeight);

	float2 uv = DispatchThreadID.xy * rcp(float2(screenWidth, screenHeight));
	float threshold = lerp(0.07, 0.14, distance(float2(0.5, 0.5), uv));

	/*`
        D3D12_SHADING_RATE_1X1	= 0,   // 0b0000
        D3D12_SHADING_RATE_1X2	= 0x1, // 0b0001
        D3D12_SHADING_RATE_2X1	= 0x4, // 0b0100
        D3D12_SHADING_RATE_2X2	= 0x5, // 0b0101
        D3D12_SHADING_RATE_2X4	= 0x6, // 0b0110
        D3D12_SHADING_RATE_4X2	= 0x9, // 0b1001
        D3D12_SHADING_RATE_4X4	= 0xa  // 0b1010
    */

	// Compute block shading rate based on if the error computation goes over the threshold
	// shading rates in D3D are purposely designed to be able to combined, e.g. 2x1 | 1x2 = 2x2
	uint ShadingRate = 0;
	ShadingRate |= ((diff2.x >= threshold) ? 0 : ((diff4.x > threshold) ? 0x4 : 0x8));
	ShadingRate |= ((diff2.y >= threshold) ? 0 : ((diff4.y > threshold) ? 0x1 : 0x2));

	// Disable 4x4 shading rate (low quality, limited perf gain)
	if (ShadingRate == 0xa) {
		ShadingRate = (diff2.x > diff2.y) ? 0x6 : 0x9;  // use 2x4 or 4x2 based on directional gradient
	}
	// Disable 4x1 or 1x4 shading rate (unsupported)
	else if (ShadingRate == 0x8) {
		ShadingRate = 0x4;
	} else if (ShadingRate == 0x2) {
		ShadingRate = 0x1;
	}

	// vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
	// vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
	// vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
	// vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
	// vsrd[i].shadingRateTable[4] = NV_PIXEL_X1_PER_4X2_RASTER_PIXELS;
	// vsrd[i].shadingRateTable[5] = NV_PIXEL_X1_PER_2X4_RASTER_PIXELS;
	// vsrd[i].shadingRateTable[6] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;

	if (ShadingRate == 0x1)
		ShadingRate = 2;
	else if (ShadingRate == 0x4)
		ShadingRate = 1;
	else if (ShadingRate == 0x5)
		ShadingRate = 3;
	else if (ShadingRate == 0x6)
		ShadingRate = 5;
	else if (ShadingRate == 0x9)
		ShadingRate = 4;
	else if (ShadingRate == 0xa)
		ShadingRate = 6;

	vrsSurface[DispatchThreadID.xy] = ShadingRate;
}