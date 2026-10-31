#pragma once

// The primary view contract (Api::InstallContext::RegisterPrimaryView): the main view's handlers at the main pass's engine points, which
// the shell's hooks reach through Core's FrameDriver (src/Deferred.cpp -> DrawcallLimitFix -> FrameDriver::BeforeOpaquePass and
// AfterOpaquePass). FrameDriver keeps the frame's bookkeeping there (the engine's pending state applied, the capture's frame for the
// native draws' observers) and calls the handler where the main view's work was. Features/MainView's.

namespace DCLF::Api
{
	/**
	 * @brief The main view (one: a second fails the install). Render thread, while DCLF runs, once a frame each:
	 * - beforeOpaque: where the main pass's opaque batches start, after the engine's pending state is applied: the main pass's capture for
	 *   the frame's colour epoch. Returns whether it was captured (its targets bound).
	 * - afterOpaque: where the opaque batches end (the opaque.after trigger's point): the colour epoch's submission.
	 */
	struct PrimaryViewContribution
	{
		bool (*beforeOpaque)() = nullptr;
		void (*afterOpaque)() = nullptr;
	};
}
