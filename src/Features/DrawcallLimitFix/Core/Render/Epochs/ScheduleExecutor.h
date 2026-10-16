#pragma once

// The schedule's executor: every registered pass (Api::PassDef, Core's and the features'), the frame's schedule resolved against them once
// at install, and each graph extension's epochs' passes added from it at the graph's build (GatherStructuralPasses).

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <Render/RenderGraph/RenderGraph.h>

#include <string_view>
#include <vector>

namespace DCLF::Draws
{
	/**
	 * @brief Main thread, during InstallFeatures (Api::InstallContext::RegisterPass): a_pass, copied, registered by a_who. Fails the install
	 * for no key, a key another pass has, or neither or both of make and emit.
	 */
	void RegisterPassDef(const Api::PassDef& a_pass, std::string_view a_who);
	/** @brief Main thread, at InstallFeatures, before the features install: Core's passes (Passes/CorePasses.cpp). */
	void RegisterCorePasses();
	/**
	 * @brief Main thread, once, at the end of InstallFeatures: each epoch's keys to their passes, grouped by shape (and view). Fails the
	 * install for a key nobody registered, naming it and its epoch.
	 */
	void ResolveSchedule(const Api::FrameSchedule& a_schedule);
	/**
	 * @brief Render thread, at the graph's build (an extension's GatherStructuralPasses; never per frame): a_shape's epochs (a_view's, for
	 * the View shape), in the schedule's order, each its passes in order, made from a_base with the epoch's fields set: a make's non-null
	 * pass added to a_out, an anchor's emit called.
	 */
	void GatherScheduled(Api::EpochShape a_shape, Api::EngineView a_view, const Api::PassContext& a_base, std::vector<org::RenderGraph::ExternalPassDesc>& a_out);
}
