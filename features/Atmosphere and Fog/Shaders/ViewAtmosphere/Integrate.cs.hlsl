#define COMPUTESHADER
#include "Common/ViewAtmosphere.hlsli"
#include "PhysicalSky/Common.hlsli"

Texture3D<float4> MediumSource : register(t0);
RWTexture3D<float4> IntegratedLuminance : register(u0);
RWTexture3D<float4> IntegratedTransmittance : register(u1);
cbuffer MediumDispatch : register(b0)
{
	uint MediumLevel;
	uint3 MediumPadding;
};

[numthreads(8, 8, 1)] void main(uint2 coord : SV_DispatchThreadID) {
	const ViewAtmosphere::Grid grid = ViewAtmosphere::Data[0].grids[MediumLevel];
	if (any(coord >= grid.size.xy))
		return;
	float3 light = 0.0;
	float3 tr = 1.0;
	IntegratedLuminance[uint3(coord, 0) + grid.offset.xyz] = 0.0;
	IntegratedTransmittance[uint3(coord, 0) + grid.offset.xyz] = 1.0;
	[loop] for (uint layer = 0u; layer < grid.size.z; ++layer)
	{
		const float4 source = MediumSource[uint3(coord, layer) + grid.offset.xyz];
		const float3 position = ViewAtmosphere::Position(grid, uint3(coord, layer), 0.5);
		float3 rayleigh, mie, extinction;
		PhysSky::GetMediumCoefficients(position, rayleigh, mie, extinction);
		extinction = extinction * SharedData::physSkyData.apTrMix + source.a;
		const float step = (ViewAtmosphere::Distance(grid, layer + 1u) - ViewAtmosphere::Distance(grid, layer)) * (GAME_UNIT_TO_M / 1000.0);
		const float3 tau = max(extinction * step, 0.0);
		const float3 stepTr = exp(-tau);
		const float3 factor = float3(
			tau.x < 1e-3 ? step * (1.0 - 0.5 * tau.x) : (1.0 - stepTr.x) / extinction.x,
			tau.y < 1e-3 ? step * (1.0 - 0.5 * tau.y) : (1.0 - stepTr.y) / extinction.y,
			tau.z < 1e-3 ? step * (1.0 - 0.5 * tau.z) : (1.0 - stepTr.z) / extinction.z);
		light += tr * source.rgb * factor;
		tr *= stepTr;
		IntegratedLuminance[uint3(coord, layer + 1u) + grid.offset.xyz] = float4(min(light, 65504.0), 0);
		IntegratedTransmittance[uint3(coord, layer + 1u) + grid.offset.xyz] = float4(tr, 1);
	}
}
