// Next frame's scene state, on the compute queue beside the depth epoch: tree wind and fade states.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	struct TreeWindBindings
	{
		org::DeclaredViewToken trees, clocks, frame;
		std::array<org::DeclaredViewToken, 2> wind;
	};

	struct TreeWindFramePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		TreeWindConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Tree wind (TreeWindCS.hlsl) on the compute queue in the Z-prepass epoch: advances each tree's clock and writes its
	 * TreeParams and WindTimers into this frame's wind buffer (by parity), which next frame's draws read. Nothing this frame
	 * reads it, so it overlaps the raster work; the host orders it against adjacent frames, and readers need not declare it.
	 */
	class TreeWindPass final : public org::TypedRenderGraphPass<TreeWindPass, TreeWindFramePrepared, TreeWindBindings>
	{
	public:
		explicit TreeWindPass(std::shared_ptr<SceneBuffers> a_scene) :
			scene(std::move(a_scene)) {}

		TreeWindBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Compute);
			TreeWindBindings bindings{};
			bindings.trees = a_builder.ShaderResource(*scene->trees).View();
			bindings.frame = a_builder.ShaderResource(scene->treeFrameBuffer).View();
			bindings.clocks = a_builder.UnorderedAccess(*scene->treeClocks).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.wind[h] = a_builder.UnorderedAccess(*scene->treeWindRows[h]).View();
			return bindings;
		}

		// The recording depends on the buffers' versions; count and frame inputs are in the uploaded frame row.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			for (const auto* buffer : { &scene->trees, &scene->treeClocks, &scene->treeWindRows[0], &scene->treeWindRows[1] })
				a_out.push_back(NamedSerial(a_preparation, *buffer));
		}

		TreeWindFramePrepared Prepare(const TreeWindBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			TreeWindFramePrepared prepared{};
			if (!scene->treeWind || !scene->treeFrameBuffer)
				return prepared;
			prepared.program = scene->treeWind;
			auto& constants = prepared.constants;
			constants.treesIndex = CaptureViewIndex(a_preparation, a_bindings.trees);
			constants.clocksIndex = CaptureViewIndex(a_preparation, a_bindings.clocks);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.windIndices[h] = CaptureViewIndex(a_preparation, a_bindings.wind[h]);
			// Full capacity (the nodeless entry, then the tree slots); the shader stops at the count.
			const std::uint64_t trees = std::min({ NamedUnitsOf(a_preparation, scene->trees), NamedUnitsOf(a_preparation, scene->treeClocks),
				NamedUnitsOf(a_preparation, scene->treeWindRows[0]), NamedUnitsOf(a_preparation, scene->treeWindRows[1]) });
			prepared.groups = static_cast<std::uint32_t>((trees + 1 + kTreeWindGroup - 1) / kTreeWindGroup);
			return prepared;
		}

		static void Record(const TreeWindBindings&, const TreeWindFramePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeWindConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
	};

	std::shared_ptr<org::RenderPass> MakeTreeWindPass(std::shared_ptr<SceneBuffers> a_scene)
	{
		return std::make_shared<TreeWindPass>(std::move(a_scene));
	}

	struct FadeStateBindings
	{
		org::DeclaredViewToken roots, states, frame, log, programs, rootPrograms, animated, events, reported;
		std::array<org::DeclaredViewToken, 2> published;
	};

	struct FadeStatePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		FadeStateConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/** @brief Fade roots' state (FadeStateCS.hlsl): each root's main-camera OnVisible, once a frame, before the depth culling. */
	class FadeStatePass final : public org::TypedRenderGraphPass<FadeStatePass, FadeStatePrepared, FadeStateBindings>
	{
	public:
		explicit FadeStatePass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		FadeStateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Compute);
			const auto& scene = *resources->scene;
			FadeStateBindings bindings{};
			bindings.roots = a_builder.ShaderResource(*scene.fadeRoots).View();
			bindings.states = a_builder.UnorderedAccess(*scene.fadeStates).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.published[h] = a_builder.UnorderedAccess(*scene.fadeStatesOut[h]).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			bindings.programs = a_builder.ShaderResource(*scene.portalPrograms).View();
			bindings.rootPrograms = a_builder.ShaderResource(*scene.fadeRootPrograms).View();
			bindings.animated = a_builder.ShaderResource(*scene.fadeAnimated).View();
			bindings.log = a_builder.UnorderedAccess(scene.fadeLog).View();
			if (scene.fadeEvents) {
				bindings.events = a_builder.UnorderedAccess(*scene.fadeEvents).View();
				bindings.reported = a_builder.UnorderedAccess(*scene.fadeReported).View();
			}
			return bindings;
		}

		// The recording depends on the buffers' versions and the depth shape; counts are in the uploaded FadeFrame row.
		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			const auto& scene = *resources->scene;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &scene.fadeStatesOut[0], &scene.fadeStatesOut[1], &scene.portalPrograms, &scene.fadeRootPrograms,
					 &scene.fadeAnimated, &scene.fadeEvents, &scene.fadeReported })
				a_out.push_back(NamedSerial(a_preparation, *buffer));
			a_out.push_back(frame ? frame->generation : 0);
		}

		FadeStatePrepared Prepare(const FadeStateBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			FadeStatePrepared prepared{};
			const auto& scene = *resources->scene;
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::ZPrepass);
			// The root slots every buffer the pass reads or writes by root holds (the revision's versions: one unit group).
			std::uint64_t roots = ~0ull;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &scene.fadeStatesOut[0], &scene.fadeStatesOut[1], &scene.fadeRootPrograms, &scene.fadeAnimated })
				roots = std::min(roots, NamedUnitsOf(a_preparation, *buffer));
			if (scene.fadeReported)
				roots = std::min(roots, NamedUnitsOf(a_preparation, scene.fadeReported));
			if (!scene.fadeState || !roots || !frame || !frame->latch)
				return prepared;
			prepared.program = scene.fadeState;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.rootsIndex = CaptureViewIndex(a_preparation, a_bindings.roots);
			constants.statesIndex = CaptureViewIndex(a_preparation, a_bindings.states);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			constants.programsIndex = CaptureViewIndex(a_preparation, a_bindings.programs);
			constants.logIndex = CaptureViewIndex(a_preparation, a_bindings.log);
			constants.rootProgramsIndex = CaptureViewIndex(a_preparation, a_bindings.rootPrograms);
			constants.animatedIndex = CaptureViewIndex(a_preparation, a_bindings.animated);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.outIndices[h] = CaptureViewIndex(a_preparation, a_bindings.published[h]);
			// Only with a staging set that holds the whole bound list.
			if (const auto events = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, scene.fadeEvents));
				scene.fadeEvents && scene.fadeWriteBack && scene.fadeWriteBack->SetFor(events)) {
				constants.eventsIndex = CaptureViewIndex(a_preparation, a_bindings.events);
				constants.reportedIndex = CaptureViewIndex(a_preparation, a_bindings.reported);
				constants.eventCapacity = events;
			}
			constants.latchIndex = frame->latch->SrvIndex();
			// Full capacity; the shader stops at the count.
			prepared.groups = static_cast<std::uint32_t>((roots + kFadeStateGroup - 1) / kFadeStateGroup);
			return prepared;
		}

		static void Record(const FadeStateBindings&, const FadeStatePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot.
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kFadeStateConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	std::shared_ptr<org::RenderPass> MakeFadeStatePass(std::shared_ptr<Resources> a_resources)
	{
		return std::make_shared<FadeStatePass>(std::move(a_resources));
	}
}
