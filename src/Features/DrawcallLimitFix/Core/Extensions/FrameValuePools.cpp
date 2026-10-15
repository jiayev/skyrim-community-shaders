// The frame-value pool contract's Core side (see the header): what a pool reads of the frame's engine globals and of its backing.

#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"

namespace DCLF::Api
{
	std::uint32_t FrameValuePoolProducer::TextureTransformBuffer() const { return globals ? globals->material.vanilla.textureTransformBuffer : 0u; }
}

namespace DCLF::Extensions
{
	std::uint32_t FrameValuePoolIndex(Api::FrameValuePoolId a_pool) { return FrameValues::Get().PoolIndex(a_pool); }
}
