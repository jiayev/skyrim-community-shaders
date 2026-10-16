#pragma once

// The pass contract's Core side (Api/Passes.h): what a pass's setup is handed, what a scheduled pass is made from, and where an anchor
// adds its passes. The schedule's executor (Core/Render/Epochs/ScheduleExecutor) runs the setups and fills the others at the graph's build.

#include "Features/DrawcallLimitFix/Api/Passes.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/RenderGraph/RenderGraph.h>
#include <rhi.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Draws
{
	struct Resources;
	struct SceneBuffers;
	struct ShadowResources;
	struct ViewResources;
}

namespace DCLF::Api
{
	/**
	 * @brief A pass's setup (render thread, EnsureSceneBuffers): the device its programs are made on, and the scene's tables as far as
	 * they are made (the features' scene resources included: Extensions::SceneResourcesOf).
	 */
	class PassSetup
	{
	public:
		rhi::Device device;
		const Draws::SceneBuffers& scene;
	};

	/** @brief What a pass is made from (render thread, the graph's build): its epoch, and its extension's resources (Api::EpochShape). */
	class PassContext
	{
	public:
		std::string_view epochId;
		RenderGraphRuntime::Segment segment{};
		std::uint32_t epoch = 0;                                          // RenderGraphRuntime::EpochOf(segment)
		const std::shared_ptr<Draws::Resources>* main = nullptr;          // Main shape; View shape: the view's main resources
		const std::shared_ptr<Draws::ShadowResources>* shadow = nullptr;  // Shadow shape
		const std::shared_ptr<Draws::ViewResources>* view = nullptr;      // View shape
		std::shared_ptr<const void> state;                                // the state its setup made (PassDef::setup); null: none

		const std::shared_ptr<Draws::Resources>& Main() const
		{
			assert(main);
			return *main;
		}
		const std::shared_ptr<Draws::ShadowResources>& Shadow() const
		{
			assert(shadow);
			return *shadow;
		}
		const std::shared_ptr<Draws::ViewResources>& View() const
		{
			assert(view);
			return *view;
		}

		/** @brief The setup's state as the pass's type. */
		template <class T>
		std::shared_ptr<const T> State() const
		{
			return std::static_pointer_cast<const T>(state);
		}

		/**
		 * @brief Whether the main resources have the depth pyramid (Resources::hzb; ScheduleExecutor.cpp): the depth epoch's phase 2 block
		 * (its retest build, the features' phase 2 passes, its depth draw) is added only then.
		 */
		bool PhaseTwo() const;
	};

	/** @brief What an anchor adds to its epoch: in order, each with its name, kind and queue. */
	class PassOut
	{
	public:
		PassOut(std::vector<org::RenderGraph::ExternalPassDesc>& a_out, std::uint32_t a_epoch) :
			out(a_out), epoch(a_epoch) {}

		/** @brief a_pass in the anchor's epoch (ScheduleExecutor.cpp), as Draws::AddPass adds it: a null pass is added as given. */
		void Add(std::string a_name, PassKind a_kind, std::shared_ptr<org::RenderPass> a_pass, PassQueue a_queue = PassQueue::Default);
		// Diagnostics' MainGraphPasses observers append finished descs; the executor keeps their order.
		std::vector<org::RenderGraph::ExternalPassDesc>& Descs() { return out; }

	private:
		std::vector<org::RenderGraph::ExternalPassDesc>& out;
		std::uint32_t epoch;
	};
}
