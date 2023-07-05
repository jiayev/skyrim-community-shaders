float2 GetSSMotionVector(float4 wsPosition, float4 previousWSPosition)
{
	float4 screenPosition = mul(CameraViewProjUnjittered, wsPosition);
	screenPosition.xy = screenPosition.xy / screenPosition.ww;
	float4 previousScreenPosition = mul(CameraPreviousViewProjUnjittered, previousWSPosition);
	previousScreenPosition.xy = previousScreenPosition.xy / previousScreenPosition.ww;
	return float2(-0.5, 0.5) * (screenPosition.xy - previousScreenPosition.xy);
}
