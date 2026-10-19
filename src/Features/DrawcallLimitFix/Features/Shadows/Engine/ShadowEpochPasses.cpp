// The shadow and occlusion epochs' passes (shadows.*, Schedule/Frame.cpp's kShadow and kOcclusion): the views' latched copies, the index
// pool's copies (Core's IndexPoolPass), the culling (ShadowBuildDrawsPass), the views' draws (ShadowViewPass) and the culling counters'
// readback (Core's), made from the shadow extension's resources (Api::PassContext::Shadow; Core registers them with the graph).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	// In DCLF::Draws, where they were defined before the feature moved: ORG's recipe telemetry names a pass by its type (typeid).

	struct ShadowBuildBindings
	{
		std::vector<org::DeclaredViewToken> sequences, count, bucketCounts;  // per view slot
		org::DeclaredViewToken visibility, poolFirsts;
		org::DeclaredViewToken fadeRoots;
		org::DeclaredViewToken objectGates;  // the cull binding ObjectGates: a portal-strict light's casters' gates
	};

	struct ShadowBuildPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
		struct Dispatch
		{
			BuildDrawsConstants constants{};
			std::uint32_t latchOffset = 0;  // the view's BuildDrawsLatch within the slot's region
		};
		std::vector<Dispatch> dispatches;  // one per captured view
	};

	/** @brief The shadow views' culling: BuildDrawsCS in its single phase, frustum only, one dispatch per view. */
	class ShadowBuildDrawsPass final : public org::TypedRenderGraphPass<ShadowBuildDrawsPass, ShadowBuildPrepared, ShadowBuildBindings>
	{
	public:
		ShadowBuildDrawsPass(std::shared_ptr<ShadowResources> a_resources, bool a_sky) :
			resources(std::move(a_resources)), sky(a_sky) {}

		ShadowBuildBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ShadowBuildBindings bindings{};
			// The scene list, the geometry table and the shadow rows are the installed payload's (the latch names them).
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.UnorderedAccess(*resources->sequences[s]).View());
				bindings.count.push_back(a_builder.UnorderedAccess(resources->count[s]).View());
				bindings.bucketCounts.push_back(a_builder.UnorderedAccess(*resources->bucketCounts[s]).View());
			}
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			// Fade roots' rows (the cull binding RootRows): casters under stood-in roots follow FadeStateCS's state, occluders their roots'
			// OnVisible (kCullFadeOnVisible). The states are last frame's (RootStates, the latch's), undeclared as in the depth segment.
			if (const auto& roots = Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, *resources->scene))
				bindings.fadeRoots = a_builder.ShaderResource(*roots).View();
			if (const auto& gates = Extensions::CullBindingBuffer(Api::CullBindingSlot::ObjectGates, *resources->scene))
				bindings.objectGates = a_builder.ShaderResource(*gates).View();
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(a_preparation, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(FadeRows() ? 1u : 0u);
			a_out.push_back(NamedSerial(a_preparation, resources->pool->firsts));
			a_out.push_back(NamedSerial(a_preparation, Extensions::CullBindingBuffer(Api::CullBindingSlot::ObjectGates, *resources->scene)));
		}

		ShadowBuildPrepared Prepare(const ShadowBuildBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			ShadowBuildPrepared prepared{};
			const auto frame = CurrentShadowFrame(a_preparation, sky);
			if (!frame || frame->views.empty() || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			const auto visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			const auto poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			const bool fadeRows = FadeRows();
			const auto fadeRootsIndex = fadeRows ? CaptureViewIndex(a_preparation, a_bindings.fadeRoots) : 0u;
			const auto objectGatesIndex = a_bindings.objectGates.layout ? CaptureViewIndex(a_preparation, a_bindings.objectGates) : 0u;
			for (const auto& view : frame->views) {
				if (view.slot >= a_bindings.sequences.size() || view.modeIndex >= kShadowModeCount)
					continue;
				ShadowBuildPrepared::Dispatch dispatch{};
				dispatch.latchOffset = view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
				auto& constants = dispatch.constants;
				constants.latchIndex = frame->latch->SrvIndex();
				constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences[view.slot]);
				constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count[view.slot]);
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts[view.slot]);
				constants.poolFirstsIndex = poolFirstsIndex;
				// ShadowMaterialRow's stride, one constant block (ShadowPayload.h asserts it); no pipeline rows: their stride stays 0.
				constants.materialRowStride = static_cast<std::uint32_t>(kConstantAlignment);
				// Bucketed draws are bounded by their buckets; this bounds the rest.
				constants.phaseTwoBase = view.sequenceDraws;
				// The single phase: frustum only, no engine-visibility gate.
				constants.phaseBits = 0;
				// Every dispatch writes the visibility words; nothing reads them here.
				constants.visibilityIndex = visibilityIndex;
				constants.fadeRootsIndex = fadeRootsIndex;
				constants.objectGatesIndex = objectGatesIndex;
				prepared.dispatches.push_back(dispatch);
			}
			return prepared;
		}

		static void Record(const ShadowBuildBindings&, const ShadowBuildPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.program || !a_prepared.latch || a_prepared.dispatches.empty())
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_prepared.program->layout->GetHandle());
			commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
			for (const auto& dispatch : a_prepared.dispatches)
				RecordLatchedDispatch(dispatch.constants, *a_prepared.latch, a_prepared.signature, dispatch.latchOffset, a_recording);
		}

	private:
		// The fade roots' rows mirror the tables (the depth commit uploads them: the cull binding RootRows' rows).
		bool FadeRows() const { return RootRowsCurrent(*resources->scene); }

		std::shared_ptr<ShadowResources> resources;
		bool sky = false;
	};

	struct ShadowPassBindings
	{
		std::array<std::vector<org::DeclaredViewToken>, kShadowDepthTargets> depthViews{};
		std::vector<org::ResourceBindingToken> sequences, bucketCounts;  // per view slot
		org::ResourceBindingToken pool;  // the index pool (IndexPool)
		org::ResourceBindingToken constants, viewBlocks;
	};

	struct ShadowPrepared
	{
		std::shared_ptr<const ShadowFrame> frame;
		bool sky = false;
		struct View
		{
			std::uint32_t index = 0;  // into frame->views
			org::PreparedDescriptorReference depthView{};
		};
		std::vector<View> views;
	};

	/** @brief The shadow views' draws: each view's culled sequences into its slice and viewport. */
	class ShadowViewPass final : public org::TypedRenderGraphPass<ShadowViewPass, ShadowPrepared, ShadowPassBindings>
	{
	public:
		ShadowViewPass(std::shared_ptr<ShadowResources> a_resources, bool a_sky) :
			resources(std::move(a_resources)), sky(a_sky) {}

		ShadowPassBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			ShadowPassBindings bindings{};
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i] && IsOcclusionTarget(i) == sky) {
					bindings.depthViews[i].reserve(resources->depthLayers[i]);
					for (std::uint32_t slice = 0; slice < resources->depthLayers[i]; ++slice)
						bindings.depthViews[i].push_back(a_builder.DepthReadWrite(resources->depth[i], org::DsvView{ UINT32_MAX, 0, slice }).View());
				}
			}
			// A slot's sequences are both arguments and read by address (DCLF_PULLED).
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.IndirectArguments(*resources->sequences[s]).Resource());
				a_builder.ShaderResource(*resources->sequences[s], noViews);
				bindings.bucketCounts.push_back(a_builder.IndirectArguments(*resources->bucketCounts[s]).Resource());
			}
			bindings.constants = a_builder.ShaderResource(resources->constants, noViews).Resource();
			bindings.viewBlocks = a_builder.ShaderResource(*resources->viewBlocks.buffer, noViews).Resource();
			// The vertex stream (face positions), read by address after the commit's uploads.
			if (resources->scene->vertexStream)
				a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
			bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(a_preparation, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(NamedSerial(a_preparation, resources->pool->indices));
		}

		ShadowPrepared Prepare(const ShadowPassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			ShadowPrepared prepared{};
			auto frame = CurrentShadowFrame(a_preparation, sky);
			if (!frame || frame->views.empty() || !frame->indirect.valid)
				return prepared;
			for (std::uint32_t i = 0; i < frame->views.size(); ++i) {
				const auto& view = frame->views[i];
				if (!view.capacity || view.slot >= a_bindings.sequences.size() || view.target >= kShadowDepthTargets || !resources->depth[view.target] ||
					IsOcclusionTarget(view.target) != sky)
					continue;
				// The declared slices, not the live target's: a target grown since has slices these bindings cannot name.
				if (view.slice >= resources->depthLayers[view.target] || view.slice >= a_bindings.depthViews[view.target].size())
					continue;
				prepared.views.push_back({ i, a_preparation.Capture(a_bindings.depthViews[view.target][view.slice]) });
			}
			if (!prepared.views.empty())
				prepared.frame = std::move(frame);
			prepared.sky = sky;
			return prepared;
		}

		static void Record(const ShadowPassBindings& a_bindings, const ShadowPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			std::vector<rhi::DepthAttachment> depths(a_prepared.views.size());
			std::vector<rhi::PassBeginInfo> begins(a_prepared.views.size());
			for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
				const auto& prepared = a_prepared.views[i];
				const auto& view = frame.views[prepared.index];
				auto& begin = begins[i];
				begin.x = view.x;
				begin.y = view.y;
				begin.width = view.width;
				begin.height = view.height;
				begin.minDepth = view.minDepth;
				begin.maxDepth = view.maxDepth;
				// The engine's casters are already in the slice: loaded, added to, stored.
				auto& depth = depths[i];
				depth.dsv = a_recording.Resolve(prepared.depthView);
				depth.depthLoad = rhi::LoadOp::Load;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = rhi::LoadOp::Load;
				depth.stencilStore = rhi::StoreOp::Store;
				begin.depth = &depth;
				begin.debugName = a_prepared.sky ? "DCLF occlusion map" : "DCLF shadow view";
			}
			// Per view: a plain indexed indirect draw per bucket over its range of the slot's sequences, counted by its count word.
			// A plain draw cannot bind its own vertex buffer, so the vertex stage fetches through the sequence (Utility.hlsl, DCLF_PULLED).
			auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
			auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
				a_words[0] = static_cast<std::uint32_t>(a_value);
				a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
			};
			const rhi::IndexBufferView pool{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt };  // the whole buffer
			for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
				const auto& view = frame.views[a_prepared.views[i].index];
				commands.BeginPass(begins[i]);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.layout);
				commands.SetIndexBuffer(pool);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, view.push.data());
				const auto sequences = a_recording.Resolve(a_bindings.sequences[view.slot]).GetHandle();
				const auto counts = a_recording.Resolve(a_bindings.bucketCounts[view.slot]).GetHandle();
				const std::uint64_t sequencesAddress = device.GetBufferDeviceAddress({ sequences, 0 });
				for (std::uint32_t b = 0; b < view.buckets.size(); ++b) {
					const auto& bucket = view.buckets[b];
					if (!bucket.capacity || bucket.pipeline >= frame.indirect.pipelines.size())
						continue;
					const std::uint64_t first = std::uint64_t(bucket.first) * sizeof(DrawSequence);
					std::uint32_t words[kDrawPushWords]{};
					split(words + kShadowDrawPushSequences, sequencesAddress + first);
					// The layout's rows word: 0, as the Z-prepass's pulled draws (each draw's row is its sequence's; the rows are the
					// installed payload's, which no recording names).
					split(words + kShadowDrawPushMaterialRows, 0);
					split(words + kShadowDrawPushVertexLayout, frame.indirect.vertexLayouts[bucket.pipeline]);
					commands.BindPipeline(frame.indirect.pipelines[bucket.pipeline]);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
					commands.ExecuteIndirect(frame.indirect.drawSignature, sequences, first + kSequenceDrawOffset, counts, std::uint64_t(b) * sizeof(std::uint32_t),
						bucket.capacity);
				}
				commands.EndPass();
			}
		}

	private:
		std::shared_ptr<ShadowResources> resources;
		bool sky = false;
	};
}

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		/** @brief The views' latched copies: blocks, counters, and the commit's values in the constants. */
		std::shared_ptr<org::RenderPass> ShadowLatchedCopies(const std::shared_ptr<Draws::ShadowResources>& a_resources, bool a_sky)
		{
			auto targets = [shadow = a_resources] {
				std::vector<Draws::LatchedTarget> out{ { nullptr, shadow->viewBlocks.buffer }, { shadow->constants, nullptr } };
				for (std::size_t s = 0; s < shadow->count.size(); ++s) {
					out.push_back({ shadow->count[s], nullptr });
					out.push_back({ nullptr, shadow->bucketCounts[s] });
				}
				return out;
			};
			return Draws::MakeLatchedCopiesPass(std::move(targets), [a_sky](const org::PassPrepareContext& a_preparation) {
				const auto frame = Draws::CurrentShadowFrame(a_preparation, a_sky);
				return frame ? Draws::LatchedSource{ frame->generation, frame, frame->latched } : Draws::LatchedSource{};
			});
		}

		template <bool Sky>
		std::shared_ptr<org::RenderPass> LatchedCopies(const Api::PassContext& a_context)
		{
			return ShadowLatchedCopies(a_context.Shadow(), Sky);
		}

		// Core's IndexPoolPass (the depth epoch's too), from the shadow shape's latch.
		std::shared_ptr<org::RenderPass> IndexPoolCopies(const Api::PassContext& a_context)
		{
			return Draws::MakeShadowIndexPoolPass(a_context.Shadow());
		}

		template <bool Sky>
		std::shared_ptr<org::RenderPass> BuildDraws(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::ShadowBuildDrawsPass>(a_context.Shadow(), Sky);
		}

		template <bool Sky>
		std::shared_ptr<org::RenderPass> ViewDraws(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::ShadowViewPass>(a_context.Shadow(), Sky);
		}

		// The views' culling counters, to the host (ShadowEngine::ReadShadowCullCounters).
		std::shared_ptr<org::RenderPass> CullCounters(const Api::PassContext& a_context)
		{
			return Draws::MakeShadowCullCounterPass(a_context.Shadow());
		}

		using PassKind = Api::PassKind;
		using PassQueue = Api::PassQueue;

		// The shadow epoch's, then Skylighting's occlusion map: the same passes in its own epoch after RenderMask.
		constexpr Api::PassDef kShadowPasses[] = {
			{ .key = "shadows.latched-copies", .passName = "cs.dclf.shadow.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies<false> },
			{ .key = "shadows.index-pool", .passName = "cs.dclf.shadow.index-pool", .make = &IndexPoolCopies },
			{ .key = "shadows.build-draws", .passName = "cs.dclf.shadow.build-draws", .make = &BuildDraws<false> },
			{ .key = "shadows.view", .passName = "cs.dclf.shadow.view", .kind = PassKind::Render, .make = &ViewDraws<false> },
			{ .key = "shadows.cull-counters", .passName = "cs.dclf.shadow.cull-counters", .kind = PassKind::Copy, .queue = PassQueue::None, .make = &CullCounters },
			{ .key = "shadows.sky.latched-copies", .passName = "cs.dclf.sky.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies<true> },
			{ .key = "shadows.sky.build-draws", .passName = "cs.dclf.sky.build-draws", .make = &BuildDraws<true> },
			{ .key = "shadows.sky.view", .passName = "cs.dclf.sky.view", .kind = PassKind::Render, .make = &ViewDraws<true> },
		};
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		for (const auto& pass : kShadowPasses)
			a_context.RegisterPass(pass);
	}
}
