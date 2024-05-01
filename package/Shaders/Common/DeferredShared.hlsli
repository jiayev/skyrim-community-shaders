cbuffer PerFrameDeferredShared : register(b0)
{
	float4 CamPosAdjust[2];
	float4 DirLightDirectionVS[2];
	float4 DirLightColor;
	float4 CameraData;
	float2 BufferDim;
	float2 RcpBufferDim;
	float4x4 ViewMatrix[2];
	float4x4 ProjMatrix[2];
	float4x4 ViewProjMatrix[2];
	float4x4 InvViewMatrix[2];
	float4x4 InvProjMatrix[2];
	float4x4 InvViewProjMatrix[2];
	row_major float3x4 DirectionalAmbient;
	uint FrameCount;
	uint pad0[3];
};