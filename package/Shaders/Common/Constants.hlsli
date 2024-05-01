#ifndef __CONSTANTS_DEPENDENCY_HLSL__
#define __CONSTANTS_DEPENDENCY_HLSL__

#define M_HALFPI 1.57079637;
#define M_PI 3.1415925  // PI
#define M_2PI 6.283185  // PI * 2

const static float4x4 M_IdentityMatrix = {
	{ 1, 0, 0, 0 },
	{ 0, 1, 0, 0 },
	{ 0, 0, 1, 0 },
	{ 0, 0, 0, 1 }
};
#endif  //__CONSTANTS_DEPENDENCY_HLSL__