#pragma once

// The pass contract's Core side (Api/Passes.h): what a pass's setup is handed, what a scheduled pass is made from, and where an anchor
// adds its passes. The schedule's executor (Core/Render/Epochs/ScheduleExecutor) runs the setups and fills the others at the graph's build.
// And the authoring helpers a pass or a scene resource uses (Draws::Register, Draws::RecordDispatch). A pass reads its epoch from the
// context it was made with (segment), never a literal one: the schedule decides where it runs.

#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/Buffers/VersionedBuffer.h>
#include <rhi.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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
		const std::shared_ptr<Draws::ViewResources>* view = nullptr;      // View shape; a hosted epoch's pass in its Main host: its view's
		// A Main epoch hosting a view's epoch (Api::EpochDef::hostedBy: the depth epoch, the reflection's): that view's resources while its
		// passes are added (ScheduleExecutor's GatherScheduled), for the epoch's own passes (its latched copies take the view's lists too);
		// null otherwise.
		const std::shared_ptr<Draws::ViewResources>* hosted = nullptr;
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

		/**
		 * @brief The segment of the epoch whose shape the pass records against: its epoch's own, or the epoch it shares the shape of
		 * (Api::EpochDef::sharesWith: the forward epoch's passes read the colour epoch's shape). A search: call it from a make, not per frame.
		 */
		RenderGraphRuntime::Segment ShapeSegment() const
		{
			const std::uint32_t row = Draws::EpochById(epochId);
			const std::uint32_t shared = row == Draws::kNoEpoch ? Draws::kNoEpoch : Draws::EpochTraitsOf(row).sharesWith;
			return shared == Draws::kNoEpoch ? segment : Draws::EpochTraitsOf(shared).segment;
		}
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

namespace DCLF::Draws
{
	/**
	 * @brief Registers a graph resource under a_id. A versioned buffer is not registered: its passes declare it (its resolver), and a
	 * registration would hold the version registered for as long as the graph, where a version should go when nothing uses it. Nothing
	 * looks these identifiers up, but the registration is not only a name: the graph holds the resource strongly and tracks it (an
	 * unregistered one gets a weak anonymous handle, pinned a frame at a time) and tags its memory with the identifier.
	 */
	template <class T>
	void Register(org::RenderGraph& a_graph, org::ResourceIdentifier a_id, const std::shared_ptr<T>& a_resource)
	{
		if constexpr (!std::is_same_v<T, org::VersionedBuffer>)
			if (a_resource)
				a_graph.RegisterResource(std::move(a_id), a_resource);
	}

	/** @brief One compute dispatch of a_program with its push constants. */
	template <class Constants>
	void RecordDispatch(rhi::CommandList& a_commands, const ComputeProgram& a_program, const Constants& a_constants, std::uint32_t a_groups)
	{
		a_commands.BindLayout(a_program.layout->GetHandle());
		a_commands.BindPipeline(a_program.Pipeline());
		a_commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(sizeof(Constants) / 4), reinterpret_cast<const std::uint32_t*>(&a_constants));
		a_commands.Dispatch(a_groups, 1, 1);
	}
}
