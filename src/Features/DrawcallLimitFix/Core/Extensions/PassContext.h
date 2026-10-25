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
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF::Draws
{
	struct Resources;
	struct RevisionShapes;
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
		// A Main epoch hosting views' epochs (Api::EpochDef::hostedBy: the depth epoch, the reflection's and first person's depth): the
		// resources of each view whose passes are added (ScheduleExecutor's GatherScheduled), in the epochs' row order, for the epoch's own
		// passes (its latched copies take the views' lists too); empty otherwise.
		std::span<const std::shared_ptr<Draws::ViewResources>> hosted;
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
		 * @brief Whether the main resources have the depth pyramid (Resources::hzb; ScheduleExecutor.cpp) and the cull mode tests occlusion
		 * (Draws::GraphConditions::twoPassOcclusion): the depth epoch's phase 2 block (the pyramid, its retest build, the features' phase 2
		 * passes, its depth draw) is added only then.
		 */
		bool PhaseTwo() const;

		/**
		 * @brief Whether the graph is built for an interior cell (Draws::GraphConditions::interior): exterior-only content's passes (grass,
		 * tree LOD) are left out. Their draws stand down on their own there: their families decide nothing to draw without content.
		 */
		bool Interior() const;

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

	/**
	 * @brief What a pass's CPU-mode work is prepared from (Api::PassDef::direct; render thread, at its epoch's site after the commits:
	 * Core/Render/Direct/PreWork.cpp): its epoch, the shapes the frame's commits wrote (the ones the GPU path's recording would be
	 * selected for: RevisionShapes, read as CurrentFrame reads a preparation's), the set the frame adopted (the only source of a
	 * versioned buffer's version, descriptors and units, as a preparation's set is), the frame slot whose latch regions the commits
	 * wrote, its extension's resources and its setup's state, as PassContext has them. Valid during the call only: a closure it adds
	 * holds what it reads.
	 */
	class DirectPass
	{
	public:
		std::string_view epochId;
		RenderGraphRuntime::Segment segment{};
		const Draws::RevisionShapes& shapes;
		const org::services::NamedVersions& adopted;
		std::uint32_t frameSlot = 0;
		const std::shared_ptr<Draws::Resources>* main = nullptr;          // Main shape; a hosted epoch's pass: its view's main resources
		const std::shared_ptr<Draws::ShadowResources>* shadow = nullptr;  // Shadow shape
		const std::shared_ptr<Draws::ViewResources>* view = nullptr;      // a hosted epoch's pass: its view's (committed this frame)
		std::span<const std::shared_ptr<Draws::ViewResources>> hosted;    // a host's own passes: the views committed in it this frame
		std::shared_ptr<const void> state;                                // PassDef::setup's (null: none)

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
		template <class T>
		std::shared_ptr<const T> State() const
		{
			return std::static_pointer_cast<const T>(state);
		}

		// The adopted set's version of a versioned buffer, as a preparation resolves it (Core/Render/Direct/PreWork.cpp): its buffer (null
		// when the set does not name it), its units (0 then), its default SRV and UAV descriptors (0 then).
		std::shared_ptr<org::Buffer> Version(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const;
		std::uint64_t Units(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const;
		std::uint32_t Srv(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const;
		std::uint32_t Uav(const std::shared_ptr<org::VersionedBuffer>& a_buffer) const;
		// A plain buffer's default SRV and UAV descriptors (what a declared view without a request resolves to; 0: none).
		static std::uint32_t Srv(const std::shared_ptr<org::Buffer>& a_buffer);
		static std::uint32_t Uav(const std::shared_ptr<org::Buffer>& a_buffer);
	};

	/**
	 * @brief Where a pass's CPU-mode work goes (Api::PassDef::direct): closures DXVK's CS thread runs in the schedule's order inside the
	 * site's callback, each recording onto DXVK's own command buffer (adopted as an rhi::CommandList) outside any render pass, the heaps
	 * and push data not yet bound by the site. The site's pre-work puts a barrier between consecutive passes' work and around all of it
	 * (Core/Render/Direct/PreWork.cpp), as ORG's ordering of a declared pass's uses would; a pass orders its own dispatches within its
	 * work as its Record does.
	 */
	class DirectWork
	{
	public:
		using Record = std::function<void(rhi::CommandList&)>;
		void Add(Record a_record)
		{
			if (a_record)
				records.push_back(std::move(a_record));
		}

		std::vector<Record> records;
		std::vector<std::uint32_t> passEnds;  // per pass that added work: its records' end (where the pre-work puts a barrier)
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
