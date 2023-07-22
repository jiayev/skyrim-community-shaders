float2 GetSSMotionVector(float4 a_wsPosition, float4 a_previousWSPosition, uint a_eyeIndex = 0)
{
	float4 screenPosition = mul(CameraViewProjUnjittered[a_eyeIndex], a_wsPosition);
	float4 previousScreenPosition = mul(CameraPreviousViewProjUnjittered[a_eyeIndex], a_previousWSPosition);
#if !defined(VR)
	screenPosition.xy = screenPosition.xy / screenPosition.ww;
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.ww;
#else
	screenPosition.xy = screenPosition.xy / screenPosition.zz;
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.zz;
#endif  // !VR
	return float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);
}
