// The forward epoch's passes: the forward range's blended draws (forward.draw) and the refraction normals (forward.refraction), drawn
// against the colour epoch's shape (the epoch shares it: PassContext::ShapeSegment). The forward range's sort is Core's (core.forward-sort,
// shared with the views' forward parts).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Forward/Forward.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	// In DCLF::Draws, where they were defined before the feature moved: ORG's recipe telemetry names a pass by its type (typeid).

	/*
	 * The forward epoch's draws (dclf-architecture.md, "The forward epoch"). The colour BuildDraws appends visible forward draws (kObjectForward) with their
	 * view depth to the staging half; Core's ForwardSortPass (colour epoch) sorts them back to front into the sorted half; ForwardDrawPass
	 * draws it in the forward epoch (the engine's blended draw, FUN_1414b39d0, after the deferred composite) on the colour pass's
	 * attachments; forward pipelines write the main target alone.
	 */
	class ForwardDrawPass final : public org::TypedRenderGraphPass<ForwardDrawPass, PreparedDraws, PassBindings>
	{
	public:
		ForwardDrawPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_shapeSegment) :
			resources(std::move(a_resources)), shapeSegment(a_shapeSegment) {}

		PassBindings Declare(org::PassBuilder& a_builder)
		{
			// As MainOpaquePass: the forward pipelines are the main set's.
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			PassBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.targets[i] = a_builder.RenderTarget(resources->native[i]).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			bindings.count = a_builder.IndirectArguments(resources->count);
			// The face shapes' second vertex stream, pulled by address (DCLF_PULLED_VERTICES).
			if (resources->scene->vertexStream)
				a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
			DeclareFrameBuffers(a_builder, *resources);
			a_builder.ShaderResource(resources->frameConstants, noViews);
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, shapeSegment);
			a_out.push_back(frame ? frame->generation : 0);
		}

		PreparedDraws Prepare(const PassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			// The forward epoch is prepared for the colour shape (Revision/Assemble.cpp).
			auto frame = CurrentFrame(a_preparation, shapeSegment);
			if (!frame || !frame->forwardCapacity || !frame->indirect.valid || !resources->forwardSort || !resources->preprocessForward)
				return prepared;
			prepared.preprocess = resources->preprocessForward.get();
			prepared.framePushWords = MainFramePushWords(resources->frameConstantsAddress);
			prepared.frame = std::move(frame);
			prepared.targetCount = resources->targetCount;
			for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
				prepared.targetViews[i] = a_preparation.Capture(a_bindings.targets[i]);
			prepared.depthView = a_preparation.Capture(a_bindings.depth);
			return prepared;
		}

		static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame || !a_prepared.preprocess)
				return;
			const auto& frame = *a_prepared.frame;
			// Every attachment loaded: the draws blend onto the composited frame; G-buffer targets are masked off.
			auto colors = LoadedColours(a_recording, a_prepared.targetViews, a_bindings.targets, a_prepared.targetCount);
			auto depth = LoadedDepth(a_recording, a_prepared.depthView);
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;
			begin.depth = &depth;
			begin.colors = { colors.data(), a_prepared.targetCount };
			begin.debugName = "DCLF forward";
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			// The sorted half, counted by the forward count word.
			const std::uint64_t arguments = (ForwardStagingSlot(frame) + frame.forwardCapacity) * sizeof(DrawSequence);
			const std::uint64_t countOffset = std::uint64_t(kCountForwardWord) * sizeof(std::uint32_t);
			RecordPreprocessedDraw(a_recording, *a_prepared.preprocess, begin, frame.resourceHeap, frame.samplerHeap, frame.indirect.layout, a_prepared.framePushWords,
				frame.indirect.signatures[kColorVariant], sequences, arguments, count, countOffset, frame.forwardCapacity);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment shapeSegment;  // the colour epoch's: the shape the forward epoch records against
	};

	/**
	 * The refraction pass (dclf-architecture.md, "Refraction"): the refraction range's draws, drawn in the forward epoch into kREFRACTION_NORMALS
	 * with one plain indexed indirect draw (DCLF_REFRACTION), testing the main depth. The target is first cleared to FUN_1414b4860's
	 * colour (0.5, 0.5, 0, 0: no offset, no mask), as the engine does before its group 5; Features/Forward's blended-draw hook then marks the normals
	 * begun so the engine's later draws add to DCLF's.
	 */
	struct RefractionBindings
	{
		org::DeclaredViewToken target, depth;
		org::ResourceBindingToken sequences, count, pool;
	};

	struct RefractionPrepared
	{
		std::shared_ptr<const PassFrame> frame;
		org::PreparedDescriptorReference targetView{}, depthView{};
		std::uint64_t frameConstants = 0;
	};

	class RefractionDrawPass final : public org::TypedRenderGraphPass<RefractionDrawPass, RefractionPrepared, RefractionBindings>
	{
	public:
		RefractionDrawPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_shapeSegment) :
			resources(std::move(a_resources)), shapeSegment(a_shapeSegment) {}

		RefractionBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			RefractionBindings bindings{};
			bindings.target = a_builder.RenderTarget(resources->refractionNormals).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			// The sequences are both arguments and read by address (DCLF_PULLED).
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			a_builder.ShaderResource(*resources->sequences, noViews);
			bindings.count = a_builder.IndirectArguments(resources->count);
			bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			// The vertex stream (face positions), read by address.
			if (resources->scene->vertexStream)
				a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
			DeclareFrameBuffers(a_builder, *resources, false);
			a_builder.ShaderResource(resources->frameConstants, noViews);
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, shapeSegment);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->pool ? NamedSerial(a_preparation, resources->pool->indices) : 0);
		}

		RefractionPrepared Prepare(const RefractionBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			RefractionPrepared prepared{};
			// Prepared for the colour shape, as ForwardDrawPass.
			auto frame = CurrentFrame(a_preparation, shapeSegment);
			if (!frame || !frame->refractionCapacity || !frame->indirect.valid || !frame->indirect.refraction.valid() || !frame->indirect.refractionDrawSignature.valid() ||
				frame->indirect.refractionFormat != resources->refractionFormat || !resources->pool)
				return prepared;
			prepared.frame = std::move(frame);
			prepared.targetView = a_preparation.Capture(a_bindings.target);
			prepared.depthView = a_preparation.Capture(a_bindings.depth);
			prepared.frameConstants = resources->frameConstantsAddress;
			return prepared;
		}

		static void Record(const RefractionBindings& a_bindings, const RefractionPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame || !a_prepared.frameConstants)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			rhi::ColorAttachment colour{};
			colour.rtv = a_recording.Resolve(a_prepared.targetView);
			colour.loadOp = rhi::LoadOp::Clear;
			colour.storeOp = rhi::StoreOp::Store;
			colour.clear.type = rhi::ClearValueType::Color;
			colour.clear.rgba[0] = colour.clear.rgba[1] = 0.5f;  // FUN_1414b4860's clear colour (0x141ad285c, 0.5), as RENDER_NORMAL_CLEAR writes
			colour.clear.rgba[2] = colour.clear.rgba[3] = 0.0f;
			colour.resource = a_recording.Resolve(a_bindings.target.Resource()).GetHandle();
			auto depth = LoadedDepth(a_recording, a_prepared.depthView);
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;
			begin.depth = &depth;
			begin.colors = { &colour, 1 };
			begin.debugName = "DCLF refraction normals";
			auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
			auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
				a_words[0] = static_cast<std::uint32_t>(a_value);
				a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
			};
			// Blocks in the shadow views' layout (kShadowPush*): the colour epoch's frame record and slots (main camera), and the zeroed
			// shared light block as the zero block.
			const std::uint64_t constants = a_prepared.frameConstants;
			std::uint32_t pass[kShadowPushWords]{};
			MainShadowLayoutPush(constants, pass);
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			const std::uint64_t first = RefractionSlot(frame) * sizeof(DrawSequence);
			// Call words: the range's first sequence, and the frame's LodFadeFrame (after FrameLighting in the lighting slot) in the
			// material rows' words, which the refraction stage reads instead.
			std::uint32_t words[kDrawPushWords]{};
			split(words + kShadowDrawPushSequences, device.GetBufferDeviceAddress({ sequences, 0 }) + first);
			split(words + kShadowDrawPushMaterialRows, LodFadeFrameAddress(constants));
			commands.BeginPass(begin);
			commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			commands.BindLayout(frame.indirect.refractionLayout);
			commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, pass);
			commands.BindPipeline(frame.indirect.refraction);
			commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			commands.ExecuteIndirect(frame.indirect.refractionDrawSignature, sequences, first + kSequenceDrawOffset, count,
				std::uint64_t(kCountRefractionWord) * sizeof(std::uint32_t), frame.refractionCapacity);
			commands.EndPass();
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment shapeSegment;  // as ForwardDrawPass's
	};
}

namespace DCLF::Features::Forward
{
	namespace
	{
		// The blended draws, while the colour epoch sorts a forward range.
		std::shared_ptr<org::RenderPass> ForwardDraw(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->forwardSort ? std::make_shared<Draws::ForwardDrawPass>(resources, a_context.ShapeSegment()) : nullptr;
		}

		// Refraction normals, in the forward epoch after the blended draws.
		std::shared_ptr<org::RenderPass> RefractionDraw(const Api::PassContext& a_context)
		{
			const auto& resources = a_context.Main();
			return resources->refractionNormals && resources->nativeDepth && resources->pool ?
			           std::make_shared<Draws::RefractionDrawPass>(resources, a_context.ShapeSegment()) :
			           nullptr;
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "forward.draw", .passName = "cs.dclf.forward", .kind = Api::PassKind::Render, .make = &ForwardDraw });
		a_context.RegisterPass({ .key = "forward.refraction", .passName = "cs.dclf.refraction", .kind = Api::PassKind::Render, .make = &RefractionDraw });
	}
}
