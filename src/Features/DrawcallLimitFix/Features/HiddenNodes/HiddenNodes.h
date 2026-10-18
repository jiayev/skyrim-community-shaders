#pragma once

// Hidden nodes (dclf-architecture.md, "Hidden bits by event"): the source of the hidden events - every engine store that can change
// NiAVObject::flags' kHidden bit patched to push the node's mirror update with its site (HiddenStores.cpp, at the feature's early hooks:
// in right after Core's scene events, before Core's engine side reads HiddenStoresLive) - and CS_DCLF_HIDDEN_WATCH, the hardware watch
// that names a store the source misses (Diagnostics/HiddenWatch.cpp: its switch, its observer and its report lines).
//
// Core keeps the hidden-bit vocabulary every object is judged by: the channel (Core/Engine/Capture/SceneEvents.h: Scene::PushHiddenUpdate,
// hiddenEventsInstalled, the reserved sites' Extensions::PushReservedHidden; the mirror update's hiddenSite), the coordinator's chains and
// their drain (hiddenDependents, DrainHiddenEvents: the walk's dependency tracking for the Hidden verdict, as propertyDependents is for the
// property events), the frame's capture of the culls' bits (FrameGlobals::cullHidden), the Hidden verdict itself (Coordinator::ClassifyFrame,
// HiddenForWalk), and the show veto the source's show stubs call through Core's cells (Api::ShowVeto, Extensions::RegisteredShowVeto: the
// LOD gates'). Without the source the walk retakes every frame verdict, and neither the LOD gates nor the kept scene lists engage. Included
// by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>
#include <string>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Features::HiddenNodes
{
	// HiddenNodesFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// HiddenStores.cpp: the source.
	/**
	 * @brief Patches the stores (after checking every site's bytes; none is patched when one differs) and, patched, marks Core's channel
	 * installed (Scene::hiddenEventsInstalled), with its sites noted (Extensions::NoteHiddenSourceSites: the reserved sites follow them).
	 * The render (main) thread at PostPostLoad, the feature's early hooks.
	 */
	void InstallStores();
	/** @brief The patched store (its address in the image) whose stub holds a_address, or 0 (the watch's report). */
	std::uintptr_t StoreSiteOf(std::uintptr_t a_address);

	// Diagnostics/HiddenWatch.cpp: CS_DCLF_HIDDEN_WATCH.
	/**
	 * @brief CS_DCLF_HIDDEN_WATCH=1: hardware write watchpoints (the debug registers, every thread of the process) on up to
	 * four NiAVObject::flags fields, armed when an actor's frame verdict changed with no hidden event
	 * (Coordinator::FrameVerdictKept). A write that flips kHidden (bit 0) records the instruction after it, so the store the
	 * hidden events miss (HiddenStores.cpp) can be named. At most one arming at a time; disarmed after 600 reports' worth
	 * of frames or 16 flips. Diagnostic only.
	 */
	void Arm(const std::array<const RE::NiAVObject*, 4>& a_nodes);
	/** @brief The report, once per interval: what the watch recorded (empty when nothing), and the disarm when it is due. */
	std::string TakeReport(std::uint32_t a_frame);
	bool Enabled();
	/** @brief The watch's switch, and with it on its observer (Diagnostics::ActorVerdictUnannounced) and report lines (Statistics). */
	void RegisterWatch(Api::InstallContext& a_context);
}
