#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Raytracing/Includes/Common.hlsli"

Texture2D<half4> NormalSmoothness : register(t0);
Texture2D<unorm half4> Albedo : register(t1);
Texture2D<half3>	   VAOMAO : register(t2);

RWTexture2D<half4> NormalRoughness : register(u0);
RWTexture2D<half3> DiffuseAlbedo : register(u1);

[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= DynamicResolution))
		return;

    const half3 normalSmoothness = NormalSmoothness[id];
	const half3 normalWS = normalize(ViewToWorldVector(GBuffer::DecodeNormal(normalSmoothness.xy), FrameBuffer::CameraViewInverse));
    NormalRoughness[id] = half4(normalWS, 1.0f - saturate(normalSmoothness.z));

#if DLSS_RR
	const float4 albedo = Albedo[id];
	const float metallic = saturate(VAOMAO[id].y);

	const bool linearLighting = SharedData::linearLightingSettings.enableLinearLighting;
	
	const float3 linearAlbedo = linearLighting ? albedo.rgb : Color::SrgbToLinear(albedo.rgb);
	
	// If Linear Lighting is enabled Albedo is already in linear space
	DiffuseAlbedo[id] = linearAlbedo * (1.0f - metallic);
#endif
}