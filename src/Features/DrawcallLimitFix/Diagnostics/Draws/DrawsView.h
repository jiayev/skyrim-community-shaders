#pragma once

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief The draws area's read-only view for its observers: IndirectDraws' implementation (the installed draws, the payloads, the
	 * latch layouts, the kept builds, the resources and their readback buffers), read at the hook points, where the main path has
	 * finished with what it reads. Impl names the type for the observers' events and state. (The reports, Diagnostics/Reports/Draws*.cpp,
	 * are IndirectDraws' own report functions: they take its counters themselves.)
	 */
	struct DrawsView
	{
		using Impl = IndirectDraws::Impl;

		static const Impl& Get() { return *IndirectDraws::Get().impl; }
		/** @brief The implementation, or null before it is made (a hook point every frame, DCLF running or not). */
		static const Impl* Find() { return IndirectDraws::Get().impl.get(); }
	};
}
#endif
