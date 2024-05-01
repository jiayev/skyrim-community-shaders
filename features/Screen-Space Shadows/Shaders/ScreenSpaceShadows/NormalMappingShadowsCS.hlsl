#include "../Common/DeferredShared.hlsli"
#include "../Common/GBuffer.hlsli"
#include "../Common/VR.hlsli"

Texture2D<unorm half4> NormalRoughnessTexture : register(t0);
Texture2D<unorm half> DepthTexture : register(t1);
Texture2D<unorm half4> MasksTexture : register(t2);

RWTexture2D<unorm half> ShadowMaskTextureRW : register(u0);

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

half InterleavedGradientNoise(half2 uv)
{
	// Temporal factor
	half frameStep = half(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	half3 magic = half3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

// Inverse project UV + raw depth into the view space.
half3 DepthToView(half2 uv, half z, uint a_eyeIndex)
{
	uv.y = 1 - uv.y;
	half4 cp = half4(uv * 2 - 1, z, 1);
	half4 vp = mul(InvProjMatrix[a_eyeIndex], cp);
	return vp.xyz / vp.w;
}

half2 ViewToUV(half3 position, bool is_position, uint a_eyeIndex)
{
	half4 uv = mul(ProjMatrix[a_eyeIndex], half4(position, (half)is_position));
	return (uv.xy / uv.w) * half2(0.5f, -0.5f) + 0.5f;
}

[numthreads(32, 32, 1)] void main(uint3 globalId
								  : SV_DispatchThreadID, uint3 localId
								  : SV_GroupThreadID, uint3 groupId
								  : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim;

	half3 normalVS = DecodeNormal(NormalRoughnessTexture[globalId.xy].xy);

	half skinMask = MasksTexture[globalId.xy].x;
	if (skinMask == 1.0)
		return;

	half shadowMask = ShadowMaskTextureRW[globalId.xy].x;

	half rawDepth = DepthTexture[globalId.xy];
	if (rawDepth == 1.0)
		return;

	half depth = GetScreenDepth(rawDepth);
	if (depth < 16.5)
		return;

	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 viewPosition = DepthToView(ConvertFromStereoUV(uv, eyeIndex), rawDepth, eyeIndex);  // viewPosition is in VR nonstereospace
	viewPosition.z = depth;

	half3 endPosVS = viewPosition + DirLightDirectionVS[eyeIndex].xyz * 5;
	half2 endPosUV = ViewToUV(endPosVS, false, eyeIndex);

	half2 startPosPixel = clamp(uv * BufferDim, 0, BufferDim);                                   // uv is in VR stereo space
	half2 endPosPixel = clamp(ConvertToStereoUV(endPosUV, eyeIndex) * BufferDim, 0, BufferDim);  // convert back to stereospace since derived from viewPosition

	half NdotL = dot(normalVS, DirLightDirectionVS[eyeIndex].xyz);

	half shadow = 0;

	half3 viewDirectionVS = normalize(viewPosition);

	// Fade based on perceivable difference
	half fade = smoothstep(4, 5, length(startPosPixel - endPosPixel));

	// Only march for: not shadowed, not self-shadowed, march distance greater than 1 pixel
	bool validMarchPixel = NdotL > 0.0 && shadowMask != 0.0 && fade > 0.0;
	if (validMarchPixel) {
		half step = 1.0 / 5.0;
		half pos = step + step * (InterleavedGradientNoise(globalId.xy) * 2.0 - 1.0);
		half slope = -NdotL;

		for (int i = 0; i < 5; i++) {
			uint2 tmpCoords = lerp(startPosPixel, endPosPixel, pos);  // coords are in stereo space to access Textures
			half3 tmpNormal = DecodeNormal(NormalRoughnessTexture[tmpCoords].xy);
			half tmpDepth = GetScreenDepth(DepthTexture[tmpCoords]);
			half tmpNdotL = dot(tmpNormal, DirLightDirectionVS[eyeIndex].xyz);

			half shadowed = -tmpNdotL;
			shadowed += NdotL * pos;
			shadowed += max(0, dot(tmpNormal, viewDirectionVS));
			shadowed *= 1 - min(1, abs(depth - tmpDepth) * 0.1);

			slope += shadowed;

			shadow += max(0, slope);
			pos += step;
		}
	}

	shadow = saturate(1.0 - shadow);
	shadow = lerp(1.0, shadow, saturate(fade));

	ShadowMaskTextureRW[globalId.xy] = min(shadowMask, lerp(shadow, 1.0, skinMask));
}
