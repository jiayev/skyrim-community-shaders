#pragma once

// The material-write sink's Core side (Api/MaterialWrites.h): whether a feature patches the frame VS variable, read where a texture-transform
// write or capture is skipped (the material write hooks, Coordinator::DrainMaterialCaptures).

#include "Features/DrawcallLimitFix/Api/MaterialWrites.h"

namespace DCLF::Extensions
{
	/**
	 * @brief Any thread, lock-free (false before the features' install): whether a feature patches every material row's frame VS variable
	 * with the frame's value (Api::RegisterFrameVSVariablePatch: Features/TextureTransforms), so the texture transforms are no record's.
	 */
	bool FrameVSVariablePatched();
}
