// What the passes and the graph extensions share (PassHelpers.h): the eye fold, DXVK image import, scene buffer registration,
// the latched copies pass, frame buffer declaration and the preprocessed indirect draw.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/RemoteControl/NvPerf.h"

namespace DCLF::Draws
{
	void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16])
	{
		std::memcpy(a_out, a_viewProj.data(), sizeof(a_out));
		for (std::uint32_t row = 0; row < 4; ++row) {
			a_out[row * 4 + 3] = a_viewProj[row * 4 + 3] -
			                     (a_viewProj[row * 4 + 0] * a_eye.x + a_viewProj[row * 4 + 1] * a_eye.y +
								     a_viewProj[row * 4 + 2] * a_eye.z);
		}
	}

	std::shared_ptr<org::ExternalTextureResource> ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
		const char* a_name)
	{
		rhi::vulkan::ImportedImageDesc import{};
		import.image = a_image.image;
		import.createInfo.flags = a_image.flags;
		import.createInfo.imageType = a_image.type;
		import.createInfo.format = a_image.format;
		import.createInfo.extent = a_image.extent;
		import.createInfo.mipLevels = a_image.mipLevels;
		import.createInfo.arrayLayers = a_image.arrayLayers;
		import.createInfo.samples = a_image.samples;
		import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		import.createInfo.usage = a_image.usage;
		import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		import.currentLayout = a_image.layout;
		import.simultaneousAccess = true;
		import.debugName = a_name;
		rhi::ResourcePtr resource;
		if (a_image.layout != VK_IMAGE_LAYOUT_GENERAL || rhi::vulkan::import_image(a_device, import, resource) != rhi::Result::Ok)
			return nullptr;
		a_desc.imageDimensions.clear();
		a_desc.imageDimensions.push_back({ a_image.extent.width, a_image.extent.height, 0, 0 });
		// ORG builds a DSV per slice for arrays, so a shadow view attaches its own slice.
		a_desc.isArray = a_image.arrayLayers > 1;
		a_desc.arraySize = a_desc.isArray ? a_image.arrayLayers : 1;
		a_desc.initialLayout = rhi::ResourceLayout::Common;
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, true);
		if (result)
			result->SetName(a_name);
		return result;
	}

	void RegisterSceneBuffers(org::RenderGraph& a_graph, const SceneBuffers& a_scene)
	{
		if (a_scene.pool) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.index-pool"), a_scene.pool->indices);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pool-firsts"), a_scene.pool->firsts);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pool-copies"), a_scene.pool->copies);
		}
		// The features' scene resources (Api::SceneResources::registerGraph: the vertex stream's first, Features/Faces' face positions).
		// The root states' buffers are among them (Features/Fades'). The visibility readback is made only with their rows.
		Extensions::RegisterSceneResources(a_graph, a_scene.sceneResources);
		if (a_scene.visibilityEvents) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility-events"), a_scene.visibilityEvents);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility-root-reported"), a_scene.visibilityRootReported);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility-object-reported"), a_scene.visibilityObjectReported);
		}
	}

	namespace
	{
		/**
		 * @brief The latched copies a_source names, as LatchedCopiesPass records them: the epoch's own lists, then each hosted shape's
		 * (none in a recording without that view's work), each copy from frame slot a_frameSlot's latch region (a fixed list's one) to
		 * the buffer a_target resolves its target to (a null handle: a target not declared, skipped). a_withZeros false: the fixed lists
		 * (the zeroed draw and bucket counts) are left out.
		 */
		template <class TargetOf>
		void CopyLatchedLists(rhi::CommandList& a_commands, const LatchedSource& a_source, std::uint32_t a_frameSlot, const TargetOf& a_target,
			bool a_withZeros = true)
		{
			// A copy continuing the one before it (the same latch and target, both ranges adjacent) joins it: the copies are one command.
			struct Run
			{
				rhi::ResourceHandle target, latch;
				std::uint64_t dst = 0, src = 0, bytes = 0;
			} run;
			constexpr rhi::HandleEqual<rhi::ResourceHandle> same{};
			auto flush = [&] {
				if (!run.bytes)
					return;
				a_commands.CopyBufferRegion(run.target, run.dst, run.latch, run.src, run.bytes);
				run.bytes = 0;
			};
			auto copyLists = [&](std::span<const LatchedList> a_lists) {
				for (const auto& list : a_lists) {
					if (!list.latch || (list.fixed && !a_withZeros))
						continue;
					const auto latch = list.latch->Resource()->GetAPIResource().GetHandle();
					const std::uint64_t region = list.fixed ? 0 : list.latch->Offset(a_frameSlot);
					for (const auto& copy : list.copies) {
						const auto target = a_target(copy.target);
						if (!target.valid())
							continue;
						const std::uint64_t src = region + copy.latchOffset;
						if (run.bytes && same(run.target, target) && same(run.latch, latch) && run.dst + run.bytes == copy.dstOffset && run.src + run.bytes == src) {
							run.bytes += copy.bytes;
							continue;
						}
						flush();
						run = { target, latch, copy.dstOffset, src, copy.bytes };
					}
				}
			};
			copyLists(a_source.lists);
			for (const auto& hosted : a_source.hosted)
				copyLists(hosted.lists);
			flush();
		}

		struct LatchedCopiesBindings
		{
			std::vector<std::pair<const void*, org::ResourceBindingToken>> targets;  // LatchedTarget::Key
		};

		class LatchedCopiesPass final : public org::TypedRenderGraphPass<LatchedCopiesPass, LatchedSource, LatchedCopiesBindings>
		{
		public:
			LatchedCopiesPass(std::function<std::vector<LatchedTarget>()> a_targets, std::function<LatchedSource(const org::PassPrepareContext&)> a_source,
				std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared) :
				targets(std::move(a_targets)), source(std::move(a_source)), declared(a_declared) {}

			/** @brief What Declare publishes, without a graph (the CPU pipeline's build: the pass is not added, its copies are the pre-work's). */
			void PublishTargets() const
			{
				auto keys = std::make_shared<std::vector<const void*>>();
				for (const auto& target : targets())
					keys->push_back(target.Key());
				if (declared)
					declared->store(std::move(keys), std::memory_order_release);
			}

			LatchedCopiesBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				LatchedCopiesBindings bindings{};
				auto keys = std::make_shared<std::vector<const void*>>();
				for (const auto& target : targets()) {
					bindings.targets.emplace_back(target.Key(), target.versioned ? a_builder.CopyDestination(*target.versioned).Resource() : a_builder.CopyDestination(target.buffer));
					keys->push_back(target.Key());
				}
				if (declared)
					declared->store(std::move(keys), std::memory_order_release);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
			{
				const auto latched = source(a_preparation);
				a_out.push_back(latched.generation);
				for (const auto& hosted : latched.hosted)
					a_out.push_back(hosted.generation);
			}

			LatchedSource Prepare(const LatchedCopiesBindings&, const org::PassPrepareContext& a_preparation) const { return source(a_preparation); }

			static void Record(const LatchedCopiesBindings& a_bindings, const LatchedSource& a_prepared, org::PassRecordContext& a_recording)
			{
				if (LatchedRecordCut())
					return;
				CopyLatchedLists(a_recording.Commands(), a_prepared, a_recording.FrameSlot(), [&](const void* a_key) {
					const auto target = std::find_if(a_bindings.targets.begin(), a_bindings.targets.end(), [&](const auto& a_target) { return a_target.first == a_key; });
					return target != a_bindings.targets.end() ? a_recording.Resolve(target->second).GetHandle() : rhi::ResourceHandle{};
				});
			}

		private:
			std::function<std::vector<LatchedTarget>()> targets;
			std::function<LatchedSource(const org::PassPrepareContext&)> source;
			std::atomic<std::shared_ptr<const std::vector<const void*>>>* declared = nullptr;
		};
	}

	bool PublishLatchedTargets(const org::RenderPass& a_pass)
	{
		const auto* latched = dynamic_cast<const LatchedCopiesPass*>(&a_pass);
		if (latched)
			latched->PublishTargets();
		return latched != nullptr;
	}

	std::shared_ptr<org::RenderPass> MakeLatchedCopiesPass(std::function<std::vector<LatchedTarget>()> a_targets,
		std::function<LatchedSource(const org::PassPrepareContext&)> a_source, std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared)
	{
		return std::make_shared<LatchedCopiesPass>(std::move(a_targets), std::move(a_source), a_declared);
	}

	void DirectLatchedCopies(const Api::DirectPass& a_pass, std::span<const LatchedTarget> a_targets, LatchedSource a_source, Api::DirectWork& a_out)
	{
		if (!a_source.shape)
			return;
		// Each target's buffer as a preparation resolves it: a versioned one's version the adopted set names (none: skipped, as an
		// undeclared target is), held with the work.
		std::vector<std::pair<const void*, std::shared_ptr<org::Buffer>>> targets;
		targets.reserve(a_targets.size());
		for (const auto& target : a_targets)
			if (auto buffer = target.versioned ? a_pass.Version(target.versioned) : target.buffer)
				targets.emplace_back(target.Key(), std::move(buffer));
		// The zeroed counts are the GPU culls' (BuildDraws' draw and bucket counts, the indirect draws' arguments): the CPU pipeline's
		// sites draw their own lists, and nothing in a CPU frame writes or reads them. Each copy is a copy engine command the queue waits
		// on, so they are left out.
		a_out.Add([source = std::move(a_source), targets = std::move(targets), slot = a_pass.frameSlot](rhi::CommandList& a_commands) {
			if (LatchedRecordCut())
				return;
			CopyLatchedLists(a_commands, source, slot, [&](const void* a_key) {
				const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& a_target) { return a_target.first == a_key; });
				return target != targets.end() ? target->second->GetAPIResource().GetHandle() : rhi::ResourceHandle{};
			}, false);
		});
	}

	void DeclareFrameBuffers(org::PassBuilder& a_builder, const Resources& a_resources, bool a_lights)
	{
		const std::span<const org::SrvView> noViews{};
		for (const auto& frameBuffer : a_resources.frameBuffers)
			a_builder.ShaderResource(frameBuffer.copy, noViews);
		// The frame graph buffers on the graph (Api::FrameGraphBuffers: Integrations/LightLimitFix's lights, light index list and grid).
		const auto graphBuffers = Extensions::RegisteredFrameGraphBuffers();
		for (std::size_t b = 0; a_lights && b < graphBuffers.size(); ++b)
			if ((a_resources.graphBuffers >> b) & 1)
				for (std::uint32_t r = 0; r < graphBuffers[b].count; ++r)
					a_builder.ShaderResource(org::ResourceIdentifier(graphBuffers[b].resources[r].c_str()), noViews);
	}

	void RecordPreprocessedDraws(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::ResourceHandle a_arguments,
		std::span<const IndirectCall> a_calls, const char* a_preprocessRange)
	{
		if (a_calls.empty())
			return;
		auto& commands = a_recording.Commands();
		void* const nvCommands = NvPerf::CommandBufferRangesActive() ? rhi::vulkan::get_cmd_list(commands) : nullptr;
		auto beginDrawPass = [&](rhi::CommandList& a_list) {
			a_list.SetDescriptorHeaps(a_resourceHeap, a_samplerHeap);
			a_list.BeginPass(a_begin);
			a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			a_list.BindLayout(a_layout);
			a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_push.data());
		};
		// Every call preprocessed before the pass, with the execution's exact state (as MainOpaquePass), then executed in order.
		auto& state = a_preprocess.Begin(a_recording.FrameSlot());
		beginDrawPass(state);
		const bool preprocessRange = nvCommands && a_preprocessRange && NvPerf::PushCommandBufferRange(nvCommands, a_preprocessRange);
		for (const auto& call : a_calls) {
			const bool callRange = nvCommands && call.preprocessRange && NvPerf::PushCommandBufferRange(nvCommands, call.preprocessRange);
			commands.PreprocessIndirect(state, call.signature, a_arguments, call.argumentsOffset, call.counts, call.countOffset, call.capacity);
			if (nvCommands)
				NvPerf::PopCommandBufferRange(nvCommands, callRange);
		}
		if (nvCommands)
			NvPerf::PopCommandBufferRange(nvCommands, preprocessRange);
		state.EndPass();
		state.End();
		beginDrawPass(commands);
		for (const auto& call : a_calls) {
			const bool range = nvCommands && call.range && NvPerf::PushCommandBufferRange(nvCommands, call.range);
			commands.ExecuteIndirect(call.signature, a_arguments, call.argumentsOffset, call.counts, call.countOffset, call.capacity);
			if (nvCommands)
				NvPerf::PopCommandBufferRange(nvCommands, range);
		}
		commands.EndPass();
	}

	void RecordPreprocessedDraw(org::PassRecordContext& a_recording, PreprocessStates& a_preprocess, const rhi::PassBeginInfo& a_begin, rhi::DescriptorHeapHandle a_resourceHeap,
		rhi::DescriptorHeapHandle a_samplerHeap, rhi::PipelineLayoutHandle a_layout, const std::array<std::uint32_t, kFramePushWords>& a_push, rhi::CommandSignatureHandle a_signature,
		rhi::ResourceHandle a_arguments, std::uint64_t a_argumentsOffset, rhi::ResourceHandle a_counts, std::uint64_t a_countOffset, std::uint32_t a_capacity)
	{
		const IndirectCall call{ a_signature, a_argumentsOffset, a_counts, a_countOffset, a_capacity };
		RecordPreprocessedDraws(a_recording, a_preprocess, a_begin, a_resourceHeap, a_samplerHeap, a_layout, a_push, a_arguments, std::span(&call, 1));
	}
}
