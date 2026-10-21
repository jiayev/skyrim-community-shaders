// What the passes and the graph extensions share (PassHelpers.h): the eye fold, DXVK image import, scene buffer registration,
// the latched copies pass, frame buffer declaration and the preprocessed indirect draw.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
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
				auto& commands = a_recording.Commands();
				auto copyLists = [&](std::span<const LatchedList> a_lists) {
					for (const auto& list : a_lists) {
						if (!list.latch)
							continue;
						const auto latch = list.latch->Resource()->GetAPIResource().GetHandle();
						const std::uint64_t region = list.fixed ? 0 : list.latch->Offset(a_recording.FrameSlot());
						for (const auto& copy : list.copies) {
							const auto target = std::find_if(a_bindings.targets.begin(), a_bindings.targets.end(), [&](const auto& a_target) { return a_target.first == copy.target; });
							if (target != a_bindings.targets.end())
								commands.CopyBufferRegion(a_recording.Resolve(target->second).GetHandle(), copy.dstOffset, latch, region + copy.latchOffset, copy.bytes);
						}
					}
				};
				// The epoch's own lists, then each hosted shape's (none in a recording without that view's work).
				copyLists(a_prepared.lists);
				for (const auto& hosted : a_prepared.hosted)
					copyLists(hosted.lists);
			}

		private:
			std::function<std::vector<LatchedTarget>()> targets;
			std::function<LatchedSource(const org::PassPrepareContext&)> source;
			std::atomic<std::shared_ptr<const std::vector<const void*>>>* declared = nullptr;
		};
	}

	std::shared_ptr<org::RenderPass> MakeLatchedCopiesPass(std::function<std::vector<LatchedTarget>()> a_targets,
		std::function<LatchedSource(const org::PassPrepareContext&)> a_source, std::atomic<std::shared_ptr<const std::vector<const void*>>>* a_declared)
	{
		return std::make_shared<LatchedCopiesPass>(std::move(a_targets), std::move(a_source), a_declared);
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
