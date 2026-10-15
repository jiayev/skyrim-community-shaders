// The frame VS variable's patch registration (see the header). The sink's writes are the material write hooks' (Engine/Capture/MaterialWrites.cpp).

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"

#include <atomic>

namespace DCLF::Extensions
{
	namespace
	{
		// Written on the main thread at the features' install; read by the material write hooks on whichever thread runs the engine's update
		// (installed before the features), hence atomic.
		std::atomic<bool> frameVSVariablePatched{ false };
	}

	bool FrameVSVariablePatched() { return frameVSVariablePatched.load(std::memory_order_relaxed); }
}

namespace DCLF::Api
{
	void RegisterFrameVSVariablePatch() { Extensions::frameVSVariablePatched.store(true, std::memory_order_relaxed); }
}
