float2 GetSSMotionVector(float4 wsPosition, float4 previousWSPosition, uint eyeOffset = 0)
{
	float4 screenPosition = NG_mul(CameraViewProjUnjittered, wsPosition, eyeOffset);
	float4 previousScreenPosition = NG_mul(CameraPreviousViewProjUnjittered, previousWSPosition, eyeOffset);
#if !defined(VR)
	screenPosition.xy = screenPosition.xy / screenPosition.ww;
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.ww;
#else
	screenPosition.xy = screenPosition.xy / screenPosition.zz;
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.zz;
#endif  // !VR
	return float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);
}
