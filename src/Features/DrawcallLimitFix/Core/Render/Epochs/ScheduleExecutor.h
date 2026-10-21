#pragma once

// The schedule's executor: every registered pass (Api::PassDef, Core's and the features'), the frame's schedule resolved against them once
// at install, the passes' setups (the scene's GPU setup) and latch word (the depth commit), and each graph extension's epochs' passes added
// from it at the graph's build (GatherStructuralPasses).

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <Render/RenderGraph/RenderGraph.h>
#include <rhi.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace DCLF::Draws
{
	struct SceneBuffers;

	/**
	 * @brief Main thread, during InstallFeatures (Api::InstallContext::RegisterPass): a_pass, copied, registered by a_who. Fails the install
	 * for no key, a key another pass has, neither or both of make and emit, or an anchor with a setup or latch word.
	 */
	void RegisterPassDef(const Api::PassDef& a_pass, std::string_view a_who);
	/** @brief Main thread, at InstallFeatures, before the features install: Core's passes (Passes/CorePasses.cpp). */
	void RegisterCorePasses();
	/**
	 * @brief Main thread, once, at the end of InstallFeatures: the epochs' definitions (the revisions' epochs, Revision/Epochs.h: EpochCount,
	 * EpochTraitsOf, EpochOfShape, EpochById, ViewEpochsOf) and the frame's orders (Extensions::ScheduledFrameOrders), then each epoch's
	 * keys to their passes, grouped by shape (and view). Fails the install for an epoch id unknown or duplicated, a segment used twice, more
	 * epochs than kMaxEpochs, a shape Core has not or one of Core's with no epoch, a sharesWith naming no earlier epoch recording that shape
	 * itself, an order not naming every epoch and foreign epoch once (a hosted one never); a hosted epoch (Api::EpochDef::hostedBy) whose
	 * host is not an earlier Main epoch of its trigger and segment hosting no other, which is no View epoch of its own, has passes of its
	 * own or whose shape another shares; a host's key of an epoch it does not host; a trigger id missing or duplicated, an epoch's trigger
	 * the schedule has not, a trigger no epoch names, one of Core's (CoreTrigger) missing; a key nobody registered, naming it and its epoch;
	 * a pass with a setup in a Shadow epoch; a second pass with a latch word, or one the schedule names anywhere but in main.depth before
	 * core.z.build-draws.
	 */
	void ResolveSchedule(const Api::FrameSchedule& a_schedule);
	/**
	 * @brief Main thread, during InstallFeatures (Api::InstallContext::FindTrigger, and Core's own at ResolveSchedule): a_schedule's
	 * trigger a_id, its row. Fails the install, naming a_who, for an id it has not.
	 */
	Api::TriggerId FindTrigger(const Api::FrameSchedule& a_schedule, std::string_view a_id, std::string_view a_who);

	/**
	 * @brief Core's trigger points (FrameDriver's), resolved by id at ResolveSchedule: a schedule without one fails the install. The main
	 * pass's opaque.after is Features/MainView's (its handler, called from FrameDriver: Api/PrimaryView.h); shadow-maps.after and
	 * occlusion.draw are Features/Shadows/Engine's (its handlers, called from FrameDriver: Api/ShadowEpochs.h).
	 */
	enum class CoreTrigger : std::uint32_t
	{
		DepthCommitted,  // depth.committed
		Count,
	};
	/** @brief Any thread, after InstallFeatures: Core's trigger a_trigger. */
	Api::TriggerId CoreTriggerOf(CoreTrigger a_trigger);
	/** @brief Any thread, after InstallFeatures: epoch a_epoch's trigger's row (Api::EpochDef::trigger). */
	std::uint32_t EpochTrigger(std::uint32_t a_epoch);
	/** @brief Any thread, after InstallFeatures: trigger row a_trigger's id ("none" for Extensions::kNoTrigger). */
	std::string_view TriggerName(std::uint32_t a_trigger);
	/**
	 * @brief Render thread (EnsureSceneBuffers), after every table: every registered pass's setup (Api::PassDef::setup), in registration
	 * order: its state by pass (SceneBuffers::passStates; null: no setup, or the pass is not added).
	 */
	std::vector<std::shared_ptr<const void>> SetUpPassStates(rhi::Device a_device, const SceneBuffers& a_scene);
	/** @brief Render thread (the depth commit): the depth segment's latch stage word (Api::PassDef::latchWord of the pass with one; 0: none). */
	std::uint32_t PassLatchWord();
	/**
	 * @brief Render thread, at the graph's build (an extension's GatherStructuralPasses; never per frame): a_shape's epochs (a_view's, for
	 * the View shape), in the schedule's order, each its passes in order, made from a_base with the epoch's fields set: a make's non-null
	 * pass added to a_out, an anchor's emit called. A Main epoch hosting a view's epoch makes that epoch's passes, in its list's order,
	 * with the view's resources (Resources::views) as their view, while they can be added (ViewPassesReady); its own passes see them
	 * (PassContext::hosted).
	 */
	void GatherScheduled(Api::EpochShape a_shape, Api::EngineView a_view, const Api::PassContext& a_base, std::vector<org::RenderGraph::ExternalPassDesc>& a_out);
}
