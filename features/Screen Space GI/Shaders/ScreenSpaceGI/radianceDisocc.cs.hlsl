#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/VR.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<half4> srcDiffuse : register(t0);
Texture2D<half4> srcPrevGI : register(t1);          // maybe half-res
Texture2D<half4> srcPrevGISpecular : register(t2);  // maybe half-res
Texture2D<half> srcCurrDepth : register(t3);
Texture2D<half4> srcCurrNormal : register(t4);
Texture2D<half3> srcPrevGeo : register(t5);  // maybe half-res
Texture2D<float4> srcMotionVec : register(t6);
Texture2D<half3> srcPrevAmbient : register(t7);
Texture2D<unorm float> srcAccumFrames : register(t8);  // maybe half-res

RWTexture2D<float3> outRadianceDisocc : register(u0);
RWTexture2D<unorm float> outAccumFrames : register(u1);
RWTexture2D<float4> outRemappedPrevGI : register(u2);
RWTexture2D<float4> outRemappedPrevGISpecular : register(u3);

#if (defined(GI) && defined(GI_BOUNCE)) || defined(TEMPORAL_DENOISER) || defined(HALF_RATE)
#	define REPROJECTION
#endif

void readHistory(
	uint eyeIndex, float curr_depth, float3 curr_pos, int2 pixCoord, float bilinear_weight,
	inout half4 prev_gi, inout half4 prev_gi_specular, inout half3 prev_ambient, inout float accum_frames, inout float wsum)
{
	const float2 uv = (pixCoord + .5) * RCP_OUT_FRAME_DIM;
	const float2 screen_pos = Stereo::ConvertFromStereoUV(uv, eyeIndex);
	if (any(screen_pos < 0) || any(screen_pos > 1))
		return;

	const half3 prev_geo = srcPrevGeo[pixCoord];
	const float prev_depth = prev_geo.x;
	// const float3 prev_normal = GBuffer::DecodeNormal(prev_geo.yz);  // prev normal is already world
	float3 prev_pos = ScreenToViewPosition(screen_pos, prev_depth, eyeIndex);
	prev_pos = ViewToWorldPosition(prev_pos, PrevInvViewMat[eyeIndex]) + CameraPreviousPosAdjust[eyeIndex].xyz;

	float3 delta_pos = curr_pos - prev_pos;
	// float normal_prod = dot(curr_normal, prev_normal);

	const float movement_thres = curr_depth * DepthDisocclusion;

	bool depth_pass = dot(delta_pos, delta_pos) < movement_thres * movement_thres;
	// bool normal_pass = normal_prod * normal_prod > NormalDisocclusion;
	if (depth_pass) {
#if defined(GI) && defined(GI_BOUNCE)
		prev_ambient += srcPrevAmbient[pixCoord] * bilinear_weight;
#endif
#ifdef TEMPORAL_DENOISER
		prev_gi += srcPrevGI[pixCoord] * bilinear_weight;
#	ifdef GI_SPECULAR
		prev_gi_specular += srcPrevGISpecular[pixCoord] * bilinear_weight;
#	endif
		accum_frames += srcAccumFrames[pixCoord] * bilinear_weight;
#endif
		wsum += bilinear_weight;
	}
};

[numthreads(8, 8, 1)] void main(const uint2 pixCoord
								: SV_DispatchThreadID) {
	const float2 frameScale = FrameDim * RcpTexDim;

	const float2 uv = (pixCoord + .5) * RCP_OUT_FRAME_DIM;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float2 screen_pos = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float2 prev_screen_pos = screen_pos;
#ifdef REPROJECTION
	prev_screen_pos += FULLRES_LOAD(srcMotionVec, pixCoord, uv * frameScale, samplerLinearClamp).xy;
#endif
	float2 prev_uv = Stereo::ConvertToStereoUV(prev_screen_pos, eyeIndex);

	half3 prev_ambient = 0;
	half4 prev_gi = 0;
	half4 prev_gi_specular = 0;
	float accum_frames = 0;
	float wsum = 0;

	const float curr_depth = READ_DEPTH(srcCurrDepth, pixCoord);

	if (curr_depth < FP_Z) {
		outRadianceDisocc[pixCoord] = half3(0, 0, 0);
		outAccumFrames[pixCoord] = 1.0 / 255.0;
		outRemappedPrevGI[pixCoord] = half4(0, 0, 0, 0);
		return;
	}

#ifdef REPROJECTION
	if ((curr_depth <= DepthFadeRange.y) && !(any(prev_screen_pos < 0) || any(prev_screen_pos > 1))) {
		// float3 curr_normal = GBuffer::DecodeNormal(srcCurrNormal[pixCoord]);
		// curr_normal = ViewToWorldVector(curr_normal, CameraViewInverse[eyeIndex]);
		float3 curr_pos = ScreenToViewPosition(screen_pos, curr_depth, eyeIndex);
		curr_pos = ViewToWorldPosition(curr_pos, CameraViewInverse[eyeIndex]) + CameraPosAdjust[eyeIndex].xyz;

		float2 prev_px_coord = prev_uv * OUT_FRAME_DIM;
		int2 prev_px_lu = floor(prev_px_coord - 0.5);
		float2 bilinear_weights = prev_px_coord - 0.5 - prev_px_lu;

		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu, (1 - bilinear_weights.x) * (1 - bilinear_weights.y),
			prev_gi, prev_gi_specular, prev_ambient, accum_frames, wsum);
		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu + int2(1, 0), bilinear_weights.x * (1 - bilinear_weights.y),
			prev_gi, prev_gi_specular, prev_ambient, accum_frames, wsum);
		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu + int2(0, 1), (1 - bilinear_weights.x) * bilinear_weights.y,
			prev_gi, prev_gi_specular, prev_ambient, accum_frames, wsum);
		readHistory(eyeIndex, curr_depth, curr_pos,
			prev_px_lu + int2(1, 1), bilinear_weights.x * bilinear_weights.y,
			prev_gi, prev_gi_specular, prev_ambient, accum_frames, wsum);

		if (wsum > 1e-2) {
			float rcpWsum = rcp(wsum + 1e-10);
#	ifdef TEMPORAL_DENOISER
			prev_gi *= rcpWsum;
#		ifdef GI_SPECULAR
			prev_gi_specular *= rcpWsum;
#		endif
			accum_frames *= rcpWsum;
#	endif
#	if defined(GI) && defined(GI_BOUNCE)
			prev_ambient *= rcpWsum;
#	endif
		}
	}
#endif

	half3 radiance = 0;
#ifdef GI
	radiance = Color::GammaToLinear(FULLRES_LOAD(srcDiffuse, pixCoord, uv * frameScale, samplerLinearClamp).rgb * GIStrength);
#	ifdef GI_BOUNCE
	radiance += prev_ambient.rgb * GIBounceFade;
#	endif
	outRadianceDisocc[pixCoord] = radiance;
#endif

#ifdef TEMPORAL_DENOISER
#	ifdef HALF_RATE
	uint useHistory = (pixCoord.x % 2) == (FrameIndex % 2);
#	else
	uint useHistory = 1;
#	endif

	accum_frames = max(1, min(accum_frames * 255 + useHistory, MaxAccumFrames));
	outAccumFrames[pixCoord] = accum_frames / 255.0;
	outRemappedPrevGI[pixCoord] = prev_gi;
	outRemappedPrevGISpecular[pixCoord] = prev_gi_specular;
#endif
}