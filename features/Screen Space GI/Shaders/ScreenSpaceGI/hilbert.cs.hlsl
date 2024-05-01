///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>         (\_/)
// Version:         (see XeGTAO.h)                                                                            (='.'=)
// Details:         https://github.com/GameTechDev/XeGTAO                                                     (")_(")
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RWTexture2D<uint> outHilbertLUT : register(u0);

// From https://www.shadertoy.com/view/3tB3z3 - except we're using R2 here
#define XE_HILBERT_LEVEL 6U
#define XE_HILBERT_WIDTH ((1U << XE_HILBERT_LEVEL))
#define XE_HILBERT_AREA (XE_HILBERT_WIDTH * XE_HILBERT_WIDTH)
inline uint HilbertIndex(uint posX, uint posY)
{
	uint index = 0U;
	for (uint curLevel = XE_HILBERT_WIDTH / 2U; curLevel > 0U; curLevel /= 2U) {
		uint regionX = (posX & curLevel) > 0U;
		uint regionY = (posY & curLevel) > 0U;
		index += curLevel * curLevel * ((3U * regionX) ^ regionY);
		if (regionY == 0U) {
			if (regionX == 1U) {
				posX = uint((XE_HILBERT_WIDTH - 1U)) - posX;
				posY = uint((XE_HILBERT_WIDTH - 1U)) - posY;
			}

			uint temp = posX;
			posX = posY;
			posY = temp;
		}
	}
	return index;
}

[numthreads(32, 32, 1)] void main(uint2 tid
								  : SV_DispatchThreadID) {
	outHilbertLUT[tid] = HilbertIndex(tid.x, tid.y);
}