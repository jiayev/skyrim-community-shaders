#pragma once

// The engine event contracts (Api::InstallContext::RegisterStandInObserver): what DCLF's engine side tells a feature that follows the
// engine state DCLF takes over, as it changes. The first: the primary cull's stood-in roots (Engine/Cull/PrimaryCull).

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/EngineEvents.h), which the features inside DCLF include for it.
	class StandInRootsChange;

	/**
	 * @brief An observer of the primary cull's stood-in roots: the fade roots DCLF stands in for whole (state on the GPU alone; no engine
	 * cull visits them), and the BSTreeNodes among them. Called per change, never per root; the roots are alive during the call (the
	 * cut's snapshot holds them).
	 *
	 * - changed: render thread (the main thread), when the admitted entries' ownership moved (PrimaryCull::SyncFadeOwnership): the roots
	 *   that left and joined, left first (a root in both stays). Optional.
	 * - cleared: render thread, when the stand-in is dropped whole (PrimaryCull::ClearFadeOwnership): no root is stood in. Optional.
	 * - updateEntry: the main thread at Main::Update's entry (PrimaryCull::AtUpdateEntry), after the stood-in roots' kAccumulated is
	 *   written: whether DCLF runs this frame. Optional.
	 */
	struct StandInObserver
	{
		void (*changed)(const StandInRootsChange& a_change) = nullptr;
		void (*cleared)() = nullptr;
		void (*updateEntry)(bool a_running) = nullptr;
	};
}
