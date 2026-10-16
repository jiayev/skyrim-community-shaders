// Fades' passes at the depth epoch's cull state (fades.state, fades.events): the fade roots' states for the next frame's cull
// (FadeStateCS), on the compute queue beside the depth epoch's raster work, then their write-back events' copy to the host.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"  // MakeEventReadbackPass

namespace DCLF::Draws
{
	// In DCLF::Draws, where they were defined before the feature moved: ORG's recipe telemetry names a pass by its type (typeid).
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
			resources(std::move(a_resources)), fades(Features::Fades::SharedSceneOf(*resources->scene)) {}

		FadeStateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Compute);
			const auto& scene = *fades;
			FadeStateBindings bindings{};
			bindings.roots = a_builder.ShaderResource(*scene.fadeRoots).View();
			bindings.states = a_builder.UnorderedAccess(*scene.fadeStates).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.published[h] = a_builder.UnorderedAccess(*scene.fadeStatesOut[h]).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			// The cull binding Programs (none without one: FadeStateCS takes the frustum alone at ProgramsIndex 0).
			if (const auto& programs = Extensions::CullBindingBuffer(Api::CullBindingSlot::Programs, *resources->scene))
				bindings.programs = a_builder.ShaderResource(*programs).View();
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
			const auto& scene = *fades;
			for (const auto* buffer : { &scene.fadeRoots, &scene.fadeStates, &scene.fadeStatesOut[0], &scene.fadeStatesOut[1],
					 &Extensions::CullBindingBuffer(Api::CullBindingSlot::Programs, *resources->scene), &scene.fadeRootPrograms,
					 &scene.fadeAnimated, &scene.fadeEvents, &scene.fadeReported })
				a_out.push_back(NamedSerial(a_preparation, *buffer));
			a_out.push_back(frame ? frame->generation : 0);
		}

		FadeStatePrepared Prepare(const FadeStateBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			FadeStatePrepared prepared{};
			const auto& scene = *fades;
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
			if (a_bindings.programs.layout)
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
		std::shared_ptr<const Features::Fades::FadeScene> fades;  // Fades' scene resources (the pass's setup required them)
	};
}

namespace DCLF::Features::Fades
{
	namespace
	{
		// The scene's GPU setup: the fade roots' scene resources when they made the program (else out of the graph).
		std::shared_ptr<const void> SetUpFadeState(const Api::PassSetup& a_setup)
		{
			auto fades = SharedSceneOf(a_setup.scene);
			if (!fades || !fades->fadeState || !fades->fadeRoots)
				return nullptr;
			return fades;
		}

		std::shared_ptr<org::RenderPass> MakeFadeStateStage(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::FadeStatePass>(a_context.Main());
		}

		// Its write-back events' copy: only with the list and its staging sets.
		std::shared_ptr<const void> SetUpFadeEvents(const Api::PassSetup& a_setup)
		{
			auto fades = SharedSceneOf(a_setup.scene);
			if (!fades || !fades->fadeState || !fades->fadeRoots || !fades->fadeEvents || !fades->fadeWriteBack)
				return nullptr;
			return fades;
		}

		std::shared_ptr<org::RenderPass> MakeFadeEventsStage(const Api::PassContext& a_context)
		{
			const auto fades = a_context.State<FadeScene>();
			return Draws::MakeEventReadbackPass(a_context.Main()->scene, fades->fadeEvents, fades->fadeWriteBack);
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		// Fade roots' state for the next frame's culling, beside the depth epoch's raster work; then its write-back events, to the host.
		a_context.RegisterPass({ .key = "fades.state", .passName = "cs.dclf.z.fade-state", .queue = Api::PassQueue::Compute, .make = &MakeFadeStateStage,
			.setup = &SetUpFadeState });
		a_context.RegisterPass({ .key = "fades.events", .passName = "cs.dclf.z.fade-events", .kind = Api::PassKind::Copy, .queue = Api::PassQueue::None,
			.make = &MakeFadeEventsStage, .setup = &SetUpFadeEvents });
	}
}
