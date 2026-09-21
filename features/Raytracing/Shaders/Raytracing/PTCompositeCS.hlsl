#include "Raytracing/Includes/Common.hlsli"

#if defined(PHYSICAL_SKY)
#	define PS_DEFERRED_RSRCS
#	define PS_DEFERRED_SAMPLERS
#	include "PhysicalSky/Common.hlsli"
#endif

Texture2D<float> DepthInput : register(t2);
Texture2D<float4> MainInput : register(t0);
Texture2D<float4> MotionVectorInput : register(t1);
#if defined(PHYSICAL_SKY)
Texture2D<float3> SkyShadowContribution : register(t3);
#endif

RWTexture2D<float4> MainOutput : register(u0);
RWTexture2D<float2> MotionVectorOutput : register(u1);

[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= DynamicResolution))
		return;

	float4 mainPT = MainInput[id.xy];
#if defined(PHYSICAL_SKY)
	const float depth = DepthInput[id];
	if (SharedData::physSkyData.enabled && depth < 1.0 - 1e-6 && mainPT.a > 0.0) {
		const float2 uv = (float2(id) + 0.5) / float2(DynamicResolution);
		float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
		position.xyz /= position.w;
		mainPT.rgb = PhysSky::CompositeAerialPerspective(mainPT.rgb, normalize(position.xyz), id, length(position.xyz), PhysSky::SampSv);
	}
#endif
	const float blend = mainPT.a;

	// Main
	float4 mainRaster = MainOutput[id.xy];
#if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled && SharedData::DeferredSkyShadow && blend < 1.0) {
		mainRaster.rgb = max(0.0, mainRaster.rgb - SkyShadowContribution[id] * PhysSky::GetApShadow(id));
		if (SharedData::physSkyData.enableVolumetricClouds && !SharedData::PostWaterComposite)
			mainRaster.rgb = PhysSky::CompositeVolumetricClouds(mainRaster.rgb, id);
	}
#endif
	float3 mainFinal = lerp(mainRaster.rgb, mainPT.rgb, blend);
#if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		mainFinal = ColorManagement::SceneColor::ScaleAndAddLinear(mainRaster.rgb, 1.0 - blend, ColorManagement::SceneToLinear(mainPT.rgb));
#endif
	MainOutput[id.xy] = float4(mainFinal, mainRaster.a);

	// Motion Vector
	const float2 mvRaster = MotionVectorOutput[id.xy];
	const float4 mvPT = MotionVectorInput[id.xy];
	const float2 mvFinal = lerp(mvRaster.rg, mvPT.rg, blend);
	MotionVectorOutput[id.xy] = mvFinal.rg;
}
