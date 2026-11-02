#ifndef DCLF_CULL_EYE_HLSLI
#define DCLF_CULL_EYE_HLSLI

// BuildDrawsLatch::cullEye (GpuLayouts.h, kLatchCullEyeOffset): the eye the latch's view-projection is relative to, the view's own. A
// reader projects a world position less this eye, in one precise expression: a matrix with the eye folded in, or a compiler reordering
// the subtraction, would cancel large terms in float. In w, the radius margin for tests on absolute positions and planes (CullPlaneMargin).
static const uint kLatchCullEye = 368;

float4 LatchCullEye(ByteAddressBuffer a_latch, uint a_latchOffset)
{
	return asfloat(a_latch.Load4(a_latchOffset + kLatchCullEye));
}

// A bound's radius for a portal query, with the margin. Only a positive one grows: the test culls a zero radius whatever the planes.
float PortalRadiusWithMargin(float a_radius, float a_margin)
{
	return a_radius > 0.0f ? a_radius + a_margin : a_radius;
}

#endif  // DCLF_CULL_EYE_HLSLI
