#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

#	include "Features/DrawcallLimitFix/Common/Toggles.h"
#	include "Features/DrawcallLimitFix/Engine/SceneTracker.h"
#	include "Features/DrawcallLimitFix/Engine/SunViews.h"
#	include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"

namespace DCLF
{
	/*
	 * T7e: the first-person view drawn by DCLF (drawcall-limit-fix.md, "First person as a view bit (T7e)"; skyrim-engine-notes.md, "The
	 * first-person view"). The first-person skeleton's geometry is tracked under its root (SceneStore::CaptureCategories) and its members
	 * (kSetFirstPerson) are inputs of the one main list with the view bit kViewFirstPerson alone (MainMaskOf). On a frame the engine draws
	 * first person (the latch: Main::Draw's cVar23), two epochs draw them with the first-person camera as the engine set it up
	 * (FUN_1414cb020: the world camera's rotation at GetFirstPersonNodePosition, firstPersonFOV plus the animated offset, near
	 * fNear1stPersonDistance, the world's far), in world space: the depth epoch at RenderDepth's first-person draw, the colour epoch at
	 * FUN_1414cb6d0's accumulator render. The engine's shifted space (FUN_1414ce9d0 moves the root to the origin around those draws) is the
	 * same image; DCLF never reads the skeleton while it is shifted (FrameValues takes the members' rows and palettes at the frame's start).
	 */

	namespace Draws
	{
		bool FirstPersonForwardKey(const SceneStore::Tables& a_tables, std::size_t a_slot, ForwardPipelineKey& a_key)
		{
			if (a_slot >= a_tables.pipelines.size() || !a_tables.PipelineUsed(a_slot))
				return false;
			const auto& pipelineKey = a_tables.pipelines[a_slot];
			// T7e's forward part: a forward key's slot draws with the main set's colour pipeline in the forward range (FirstPersonForwardSlotsOf).
			if (RasterForward(pipelineKey.rasterFlags))
				return false;
			// The main pipeline's vertex descriptor (the pulled vertex stage: skins read their palette as the Z-prepass's do) and its
			// pixel descriptor without Deferred (the engine draws first person forward, after the composite: Community Shaders' Deferred
			// sets InWorld around it); back faces culled unless two-sided.
			a_key = ForwardPipelineKey{ pipelineKey.vertexDescriptor, pipelineKey.pixelDescriptor & ~kLightingPixelDeferred,
				ForwardPipelineKey::kForwardFirstPerson | ((pipelineKey.rasterFlags & kRasterTwoSided) ? ForwardPipelineKey::kForwardTwoSided : 0u) };
			return true;
		}

		void FirstPersonPipelinesOf(const PipelineCatalog* a_catalog, const SceneStore::Tables& a_tables, const ForwardTargets& a_targets, bool a_on,
			std::vector<rhi::PipelineHandle>& a_slots)
		{
			a_slots.assign(a_on ? a_tables.pipelines.size() : 0, rhi::PipelineHandle{});
			if (!a_on)
				return;
			for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p)
				if (ForwardPipelineKey key; FirstPersonForwardKey(a_tables, p, key))
					a_slots[p] = ForwardPipelineOf(a_catalog, key, a_targets);
		}

		void FirstPersonForwardSlotsOf(const SceneStore::Tables& a_tables, bool a_on, std::vector<std::uint8_t>& a_slots)
		{
			a_slots.assign(a_on ? a_tables.pipelines.size() : 0, 0);
			for (std::size_t p = 0; p < a_slots.size(); ++p)
				a_slots[p] = a_tables.PipelineUsed(p) && RasterForward(a_tables.pipelines[p].rasterFlags) && !RasterRefraction(a_tables.pipelines[p].rasterFlags) ? 1 : 0;
		}

		namespace
		{
			std::shared_ptr<const FirstPersonFrame> CurrentFirstPersonFrame(const org::PassPrepareContext& a_preparation)
			{
				if (const auto* shapes = RevisionShapesOf(a_preparation))
					return shapes->firstPerson;
				return nullptr;
			}

			template <class T>
			void RegisterResource(org::RenderGraph& a_graph, const char* a_id, const std::shared_ptr<T>& a_resource)
			{
				// A versioned buffer is declared by its passes (its resolver), never registered (Passes.cpp, Register).
				if (a_resource)
					a_graph.RegisterResource(org::ResourceIdentifier(a_id), a_resource);
			}

			struct LatchedBindings
			{
				org::ResourceBindingToken count, block, bucketCounts;
			};

			struct LatchedPrepared
			{
				std::shared_ptr<const FirstPersonFrame> frame;
			};

			/**
			 * @brief The depth epoch's per-frame values, copied from the latch to where the passes read them: the first-person camera's
			 * block, and the draw count and the bucket counts zeroed. Recorded with the epoch's ticket: the commit records no copy.
			 */
			class LatchedCopiesPass final : public org::TypedRenderGraphPass<LatchedCopiesPass, LatchedPrepared, LatchedBindings>
			{
			public:
				explicit LatchedCopiesPass(std::shared_ptr<FirstPersonResources> a_resources) :
					resources(std::move(a_resources)) {}

				LatchedBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					LatchedBindings bindings{};
					bindings.count = a_builder.CopyDestination(resources->count);
					bindings.block = a_builder.CopyDestination(resources->block);
					bindings.bucketCounts = a_builder.CopyDestination(*resources->bucketCounts).Resource();
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentFirstPersonFrame(a_preparation);
					a_out.push_back(frame ? frame->generation : 0);
				}

				LatchedPrepared Prepare(const LatchedBindings&, const org::PassPrepareContext& a_preparation) const
				{
					LatchedPrepared prepared{};
					if (auto frame = CurrentFirstPersonFrame(a_preparation); frame && frame->latch && frame->zeros)
						prepared.frame = std::move(frame);
					return prepared;
				}

				static void Record(const LatchedBindings& a_bindings, const LatchedPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.frame)
						return;
					const auto& frame = *a_prepared.frame;
					auto& commands = a_recording.Commands();
					const auto latch = frame.latch->Resource()->GetAPIResource().GetHandle();
					const auto zeros = frame.zeros->Resource()->GetAPIResource().GetHandle();
					commands.CopyBufferRegion(a_recording.Resolve(a_bindings.count).GetHandle(), 0, zeros, 0, sizeof(kZeroCounts));
					// T7e's forward part: the forward range's count word after the buckets'.
					const std::uint64_t bucketBytes = std::max<std::size_t>(frame.buckets.size() + (frame.forwardCapacity ? 1 : 0), 1) * sizeof(std::uint32_t);
					commands.CopyBufferRegion(a_recording.Resolve(a_bindings.bucketCounts).GetHandle(), 0, zeros, 0, bucketBytes);
					commands.CopyBufferRegion(a_recording.Resolve(a_bindings.block).GetHandle(), 0, latch, frame.latch->Offset(a_recording.FrameSlot()) + frame.blockOffset,
						kFirstPersonBlockBytes);
				}

			private:
				std::shared_ptr<FirstPersonResources> resources;
			};

			struct BuildBindings
			{
				org::DeclaredViewToken inputs, geometries, sequences, count, visibility, poolFirsts, bucketCounts;
			};

			struct BuildPrepared
			{
				std::shared_ptr<const ComputeProgram> program;
				std::shared_ptr<const org::LatchBlock> latch;
				rhi::CommandSignatureHandle signature{};
				BuildDrawsConstants constants{};
			};

			/**
			 * @brief The first-person cull: BuildDrawsCS in its single phase over the frame's scene list (the ring entry the latch names),
			 * frustum only, its view bit kViewFirstPerson; the latch's map sends each first-person slot to its bucket, every other to none.
			 */
			class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildPrepared, BuildBindings>
			{
			public:
				explicit BuildDrawsPass(std::shared_ptr<FirstPersonResources> a_resources) :
					resources(std::move(a_resources)) {}

				BuildBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const auto& main = *resources->main;
					BuildBindings bindings{};
					bindings.inputs = a_builder.ShaderResource(*main.inputsDepth).View();
					bindings.geometries = a_builder.ShaderResource(*main.scene->geometries).View();
					bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
					bindings.count = a_builder.UnorderedAccess(resources->count).View();
					// Read only in the single phase (an input's published verdict), never written.
					bindings.visibility = a_builder.UnorderedAccess(*main.visibility).View();
					bindings.poolFirsts = a_builder.ShaderResource(*main.scene->pool->firsts).View();
					bindings.bucketCounts = a_builder.UnorderedAccess(*resources->bucketCounts).View();
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentFirstPersonFrame(a_preparation);
					a_out.push_back(frame ? frame->generation : 0);
					a_out.push_back(resources->main->scene->pool->layout);
				}

				BuildPrepared Prepare(const BuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					BuildPrepared prepared{};
					const auto frame = CurrentFirstPersonFrame(a_preparation);
					if (!frame || !frame->latch || !resources->buildDraws || !resources->dispatchSignature)
						return prepared;
					prepared.program = resources->buildDraws;
					prepared.latch = frame->latch;
					prepared.signature = resources->dispatchSignature->GetHandle();
					auto& constants = prepared.constants;
					constants.latchIndex = frame->latch->SrvIndex();
					constants.inputsIndex = CaptureViewIndex(a_preparation, a_bindings.inputs);
					constants.geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
					constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
					constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
					constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
					constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
					constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts);
					constants.materialRowsAddressLo = static_cast<std::uint32_t>(frame->materialRows);
					constants.materialRowsAddressHi = static_cast<std::uint32_t>(frame->materialRows >> 32);
					constants.materialRowStride = kMaterialRowBytes;
					constants.pipelineRowsAddressLo = static_cast<std::uint32_t>(frame->pipelineRows);
					constants.pipelineRowsAddressHi = static_cast<std::uint32_t>(frame->pipelineRows >> 32);
					constants.pipelineRowStride = kPipelineRowBytes;
					constants.phaseBits = 0;  // the single phase: frustum only (the latch's mode), nothing published
					// The bucket path's draws are bounded by their buckets (the latch's table); this bounds the other, which none takes.
					constants.phaseTwoBase = frame->sequenceDraws;
					return prepared;
				}

				static void Record(const BuildBindings&, const BuildPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.program || !a_prepared.latch)
						return;
					auto& commands = a_recording.Commands();
					commands.BindLayout(a_prepared.program->layout->GetHandle());
					commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
					RecordLatchedDispatch(a_prepared.constants, *a_prepared.latch, a_prepared.signature, 0, a_recording);
				}

			private:
				std::shared_ptr<FirstPersonResources> resources;
			};

			struct DrawBindings
			{
				std::array<org::DeclaredViewToken, 2> targets;
				org::DeclaredViewToken depth;
				org::ResourceBindingToken sequences, pool, bucketCounts;
			};

			struct DrawPrepared
			{
				std::shared_ptr<const FirstPersonFrame> frame;
				bool colour = false;
				std::array<org::PreparedDescriptorReference, 2> targets{};
				org::PreparedDescriptorReference depth{};
			};

			/**
			 * @brief The first-person draws, per bucket a plain indirect draw of the bucket's range of the sequences (DCLF_PULLED, as the
			 * Z-prepass's), with the viewport depth range [0, f1stPersonFarDepthRange] (both of the engine's first-person passes set it):
			 * the depth epoch's with each bucket's Z-prepass depth pipeline into the main depth (after the world's and DCLF's HZB: the HZB
			 * keeps leaving the arms out); the colour epoch's with its first-person forward pipeline into kMAIN and the motion vectors (the
			 * engine's first-person slots 0 and 1; slot 2 unbound), tested against kPOST_ZPREPASS_COPY (FUN_1414cb6d0's depth target 7).
			 */
			class DrawPass final : public org::TypedRenderGraphPass<DrawPass, DrawPrepared, DrawBindings>
			{
			public:
				DrawPass(std::shared_ptr<FirstPersonResources> a_resources, bool a_colour) :
					resources(std::move(a_resources)), colour(a_colour) {}

				DrawBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const std::span<const org::SrvView> noViews{};
					const auto& main = *resources->main;
					const auto& scene = *main.scene;
					DrawBindings bindings{};
					if (colour) {
						for (std::uint32_t i = 0; i < 2; ++i)
							bindings.targets[i] = a_builder.RenderTarget(main.native[i]).View();
						bindings.depth = a_builder.DepthReadWrite(resources->depthCopy).View();
					} else {
						bindings.depth = a_builder.DepthReadWrite(main.nativeDepth).View();
					}
					// The sequences are the draws' arguments and what the vertex stage reads by device address (DCLF_PULLED).
					bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
					a_builder.ShaderResource(*resources->sequences, noViews);
					bindings.pool = a_builder.IndexBuffer(*scene.pool->indices);
					bindings.bucketCounts = a_builder.IndirectArguments(*resources->bucketCounts).Resource();
					// Read through device addresses; declared so the graph orders them after their uploads.
					a_builder.ShaderResource(*main.materialRows.buffer, noViews);
					a_builder.ShaderResource(main.frameConstants, noViews);  // the colour segment's frame slots, by address
					a_builder.ShaderResource(*main.pipelineRows.buffer, noViews);
					a_builder.ShaderResource(*scene.objects, noViews);
					a_builder.ShaderResource(*scene.extras, noViews);
					a_builder.ShaderResource(*scene.facePositions, noViews);
					a_builder.ShaderResource(resources->block, noViews);
					if (colour) {
						for (const auto& frameBuffer : main.frameBuffers)
							a_builder.ShaderResource(frameBuffer.copy, noViews);
						if (main.lightLimitFix) {
							a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.lights"), noViews);
							a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-index-list"), noViews);
							a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-grid"), noViews);
						}
					}
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentFirstPersonFrame(a_preparation);
					a_out.push_back(frame ? frame->generation : 0);
					a_out.push_back(resources->main->scene->pool->layout);
					a_out.push_back(colour ? 1u : 0u);
				}

				DrawPrepared Prepare(const DrawBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					DrawPrepared prepared{};
					auto frame = CurrentFirstPersonFrame(a_preparation);
					if (!frame || !frame->indirect.valid || !frame->width || !frame->height || frame->buckets.empty())
						return prepared;
					if (colour)
						for (std::uint32_t i = 0; i < 2; ++i)
							prepared.targets[i] = a_preparation.Capture(a_bindings.targets[i]);
					prepared.depth = a_preparation.Capture(a_bindings.depth);
					prepared.frame = std::move(frame);
					prepared.colour = colour;
					return prepared;
				}

				static void Record(const DrawBindings& a_bindings, const DrawPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.frame)
						return;
					const auto& frame = *a_prepared.frame;
					const bool colourPass = a_prepared.colour;
					auto& commands = a_recording.Commands();
					commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
					auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
					const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
					const std::uint64_t sequencesAddress = device.GetBufferDeviceAddress({ sequences, 0 });
					const rhi::IndexBufferView pool{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt };
					std::array<rhi::ColorAttachment, 2> colours{};
					if (colourPass)
						for (std::uint32_t i = 0; i < 2; ++i) {
							colours[i].rtv = a_recording.Resolve(a_prepared.targets[i]);
							colours[i].loadOp = rhi::LoadOp::Load;
							colours[i].storeOp = rhi::StoreOp::Store;
							colours[i].resource = a_recording.Resolve(a_bindings.targets[i].Resource()).GetHandle();
						}
					rhi::DepthAttachment depth{};
					depth.dsv = a_recording.Resolve(a_prepared.depth);
					depth.depthLoad = rhi::LoadOp::Load;
					depth.depthStore = rhi::StoreOp::Store;
					depth.stencilLoad = rhi::LoadOp::Load;
					depth.stencilStore = rhi::StoreOp::Store;
					rhi::PassBeginInfo begin{};
					if (colourPass)
						begin.colors = { colours.data(), static_cast<std::uint32_t>(colours.size()) };
					begin.depth = &depth;
					begin.width = frame.width;
					begin.height = frame.height;
					begin.minDepth = 0.0f;
					begin.maxDepth = frame.depthRange;
					begin.debugName = colourPass ? "DCLF first person" : "DCLF first-person depth";
					commands.BeginPass(begin);
					commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					commands.BindLayout(frame.indirect.zLayout);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, frame.push.data());
					commands.SetIndexBuffer(pool);
					const auto counts = a_recording.Resolve(a_bindings.bucketCounts).GetHandle();
					for (std::uint32_t b = 0; b < frame.buckets.size(); ++b) {
						const auto& bucket = frame.buckets[b];
						if (!bucket.capacity)
							continue;
						const std::uint64_t first = std::uint64_t(bucket.first) * sizeof(DrawSequence);
						std::uint32_t words[kDrawPushWords]{};
						words[kZDrawPushSequences] = static_cast<std::uint32_t>(sequencesAddress + first);
						words[kZDrawPushSequences + 1] = static_cast<std::uint32_t>((sequencesAddress + first) >> 32);
						commands.BindPipeline(colourPass ? bucket.colour : bucket.depth);
						commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
						commands.ExecuteIndirect(frame.indirect.zDrawSignature, sequences, first + kSequenceDrawOffset, counts, std::uint64_t(b) * sizeof(std::uint32_t),
							bucket.capacity);
					}
					commands.EndPass();
				}

			private:
				std::shared_ptr<FirstPersonResources> resources;
				bool colour = false;
			};

			struct SortBindings
			{
				org::DeclaredViewToken sequences, counts;
			};

			struct SortPrepared
			{
				std::shared_ptr<const ComputeProgram> program;
				ForwardSortConstants constants{};
				std::uint32_t groups = 0;
			};

			/**
			 * @brief T7e's forward part: the first-person forward range's sort, back to front (ForwardSortCS.hlsl, the main forward range's
			 * program), after the first-person BuildDraws staged the visible forward draws with the first-person camera's view depth; its count
			 * the range's bucket count word (kCountOffset).
			 */
			class SortPass final : public org::TypedRenderGraphPass<SortPass, SortPrepared, SortBindings>
			{
			public:
				explicit SortPass(std::shared_ptr<FirstPersonResources> a_resources) :
					resources(std::move(a_resources)) {}

				SortBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					SortBindings bindings{};
					bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
					bindings.counts = a_builder.ShaderResource(*resources->bucketCounts).View();
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentFirstPersonFrame(a_preparation);
					a_out.push_back(frame ? frame->generation : 0);
				}

				SortPrepared Prepare(const SortBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					SortPrepared prepared{};
					const auto frame = CurrentFirstPersonFrame(a_preparation);
					const auto& program = resources->main->forwardSort;
					if (!frame || !frame->latch || !frame->forwardCapacity || !program)
						return prepared;
					prepared.program = program;
					auto& constants = prepared.constants;
					constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
					constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.counts);
					constants.stagingBase = frame->forwardFirst;
					constants.capacity = frame->forwardCapacity;
					constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
					constants.countOffset = frame->forwardEntry * static_cast<std::uint32_t>(sizeof(std::uint32_t));
					constants.options = ForwardSortConstants::kCountOffset;
					prepared.groups = (frame->forwardCapacity + kForwardSortGroup - 1) / kForwardSortGroup;
					return prepared;
				}

				static void Record(const SortBindings&, const SortPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.program || !a_prepared.groups)
						return;
					auto& commands = a_recording.Commands();
					commands.BindLayout(a_prepared.program->layout->GetHandle());
					commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
					commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kForwardSortConstantWords, reinterpret_cast<const std::uint32_t*>(&a_prepared.constants));
					commands.Dispatch(a_prepared.groups, 1, 1);
				}

			private:
				std::shared_ptr<FirstPersonResources> resources;
			};

			struct ForwardBindings
			{
				std::array<org::DeclaredViewToken, kColorTargets> targets;
				org::DeclaredViewToken depth;
				org::ResourceBindingToken sequences, counts;
			};

			struct ForwardPrepared
			{
				std::shared_ptr<const FirstPersonFrame> frame;
				PreprocessStates* preprocess = nullptr;
				std::uint32_t targetCount = 0;
				std::array<org::PreparedDescriptorReference, kColorTargets> targets{};
				org::PreparedDescriptorReference depth{};
			};

			/**
			 * @brief T7e's forward part: the sorted half of the first-person forward range drawn through the main colour signature (the main
			 * set's forward pipelines, a pipeline per sequence: blended Lighting, the Effect port's meshes and particles), after the first-person
			 * opaque draws, with the first-person camera's frame push data and depth range, onto the main pass's attachments (every one loaded:
			 * the forward pipelines write the main target alone) tested against kPOST_ZPREPASS_COPY, as the engine's first-person blended list
			 * is drawn after its opaque passes into kMAIN against that depth.
			 */
			class ForwardPass final : public org::TypedRenderGraphPass<ForwardPass, ForwardPrepared, ForwardBindings>
			{
			public:
				explicit ForwardPass(std::shared_ptr<FirstPersonResources> a_resources) :
					resources(std::move(a_resources)) {}

				ForwardBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const std::span<const org::SrvView> noViews{};
					const auto& main = *resources->main;
					const auto& scene = *main.scene;
					ForwardBindings bindings{};
					for (std::uint32_t i = 0; i < main.targetCount; ++i)
						bindings.targets[i] = a_builder.RenderTarget(main.native[i]).View();
					bindings.depth = a_builder.DepthReadWrite(resources->depthCopy).View();
					bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
					bindings.counts = a_builder.IndirectArguments(*resources->bucketCounts).Resource();
					a_builder.ShaderResource(*main.materialRows.buffer, noViews);
					a_builder.ShaderResource(*main.pipelineRows.buffer, noViews);
					a_builder.ShaderResource(*scene.objects, noViews);
					a_builder.ShaderResource(*scene.extras, noViews);
					a_builder.VertexBuffer(*scene.facePositions);
					a_builder.ShaderResource(resources->block, noViews);
					for (const auto& frameBuffer : main.frameBuffers)
						a_builder.ShaderResource(frameBuffer.copy, noViews);
					a_builder.ShaderResource(main.frameConstants, noViews);
					if (main.lightLimitFix) {
						a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.lights"), noViews);
						a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-index-list"), noViews);
						a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-grid"), noViews);
					}
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentFirstPersonFrame(a_preparation);
					a_out.push_back(frame ? frame->generation : 0);
				}

				ForwardPrepared Prepare(const ForwardBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					ForwardPrepared prepared{};
					auto frame = CurrentFirstPersonFrame(a_preparation);
					if (!frame || !frame->forwardCapacity || !frame->indirect.valid || !frame->width || !frame->height || !resources->preprocessForward ||
						!resources->main->forwardSort)
						return prepared;
					prepared.preprocess = resources->preprocessForward.get();
					prepared.targetCount = resources->main->targetCount;
					for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
						prepared.targets[i] = a_preparation.Capture(a_bindings.targets[i]);
					prepared.depth = a_preparation.Capture(a_bindings.depth);
					prepared.frame = std::move(frame);
					return prepared;
				}

				static void Record(const ForwardBindings& a_bindings, const ForwardPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.frame || !a_prepared.preprocess)
						return;
					const auto& frame = *a_prepared.frame;
					auto& commands = a_recording.Commands();
					std::array<rhi::ColorAttachment, kColorTargets> colours{};
					for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i) {
						colours[i].rtv = a_recording.Resolve(a_prepared.targets[i]);
						colours[i].loadOp = rhi::LoadOp::Load;
						colours[i].storeOp = rhi::StoreOp::Store;
						colours[i].resource = a_recording.Resolve(a_bindings.targets[i].Resource()).GetHandle();
					}
					rhi::DepthAttachment depth{};
					depth.dsv = a_recording.Resolve(a_prepared.depth);
					depth.depthLoad = rhi::LoadOp::Load;
					depth.depthStore = rhi::StoreOp::Store;
					depth.stencilLoad = rhi::LoadOp::Load;
					depth.stencilStore = rhi::StoreOp::Store;
					rhi::PassBeginInfo begin{};
					begin.colors = { colours.data(), a_prepared.targetCount };
					begin.depth = &depth;
					begin.width = frame.width;
					begin.height = frame.height;
					begin.minDepth = 0.0f;
					begin.maxDepth = frame.depthRange;
					begin.debugName = "DCLF first-person forward";
					const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
					const auto counts = a_recording.Resolve(a_bindings.counts).GetHandle();
					const auto signature = frame.indirect.signatures[kColorVariant];
					const std::uint64_t arguments = std::uint64_t(frame.forwardFirst + frame.forwardCapacity) * sizeof(DrawSequence);
					const std::uint64_t countOffset = std::uint64_t(frame.forwardEntry) * sizeof(std::uint32_t);
					auto beginDrawPass = [&](rhi::CommandList& a_list) {
						a_list.BeginPass(begin);
						a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
						a_list.BindLayout(frame.indirect.layout);
						a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, frame.push.data());
					};
					// The explicit preprocess, before the pass, for the state its execution has (the forward epoch's rule: ForwardDrawPass).
					auto& state = a_prepared.preprocess->Begin(a_recording.FrameSlot());
					state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
					beginDrawPass(state);
					commands.PreprocessIndirect(state, signature, sequences, arguments, counts, countOffset, frame.forwardCapacity);
					state.EndPass();
					state.End();
					commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
					beginDrawPass(commands);
					commands.ExecuteIndirect(signature, sequences, arguments, counts, countOffset, frame.forwardCapacity);
					commands.EndPass();
				}

			private:
				std::shared_ptr<FirstPersonResources> resources;
			};

			class FirstPersonExtension final : public org::RenderGraph::IRenderGraphExtension
			{
			public:
				explicit FirstPersonExtension(std::shared_ptr<FirstPersonResources> a_resources) :
					resources(std::move(a_resources)) {}

				void PrepareForBuild(org::RenderGraph& a_graph) override
				{
					// Its own; the main pass's resources it reads are the main extension's registrations (declared by its passes here).
					RegisterResource(a_graph, "cs.dclf.first-person.draw-count", resources->count);
					RegisterResource(a_graph, "cs.dclf.first-person.block", resources->block);
					RegisterResource(a_graph, "cs.dclf.first-person.depth-copy", resources->depthCopy);
				}

				void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
				{
					if (!resources->main || !resources->main->nativeDepth || !resources->main->native[0] || !resources->main->native[1] || !resources->depthCopy ||
						!resources->main->scene || !resources->main->scene->pool)
						return;
					const auto depthEpoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::FirstPersonDepth);
					const auto colourEpoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::FirstPerson);
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.first-person.latched-copies",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<LatchedCopiesPass>(resources)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depthEpoch));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.first-person.build-draws",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depthEpoch));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.first-person.depth",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<DrawPass>(resources, false)))
							.Epoch(depthEpoch));
					// T7e's forward part: the forward range sorted with the depth epoch (after its BuildDraws), drawn in the colour epoch after
					// the opaque draws.
					if (resources->main->forwardSort)
						a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.first-person.forward-sort",
							std::static_pointer_cast<org::RenderPass>(std::make_shared<SortPass>(resources)))
								.PreferQueue(org::QueueKind::Graphics)
								.Epoch(depthEpoch));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.first-person.colour",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<DrawPass>(resources, true)))
							.Epoch(colourEpoch));
					if (resources->main->forwardSort && resources->preprocessForward)
						a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.first-person.forward",
							std::static_pointer_cast<org::RenderPass>(std::make_shared<ForwardPass>(resources)))
								.Epoch(colourEpoch));
				}

			private:
				std::shared_ptr<FirstPersonResources> resources;
			};
		}

		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeFirstPersonExtension(std::shared_ptr<FirstPersonResources> a_resources)
		{
			return std::make_unique<FirstPersonExtension>(std::move(a_resources));
		}
	}

	namespace
	{
		// The engine's first-person globals (AE 1.6.1170; skyrim-engine-notes.md, "The first-person view").
		// DrawWorld (FUN_1414cafd0), filled by FUN_1406452c0 (Main::Draw's first call, before BeginSceneFrame): +0x00 the camera position
		// (PlayerCamera::GetFirstPersonNodePosition), in world space.
		constexpr std::uintptr_t kDrawWorld = 0x338c8c0;
		// The first-person camera (an NiCamera*), set up by FUN_1414cb020 (FUN_140d2cda0: FOV firstPersonFOV + 0x14338c628, near the float at
		// 0x14202a928, far the world scene graph's GetFarDistance, the screen's aspect; the world camera's rotation), translated into the
		// root's space (its position minus the root's).
		constexpr std::uintptr_t kFirstPersonCamera = 0x338c820;
		// The viewport depth range both first-person passes set (FUN_140e57cb0(state, 0, max)): f1stPersonFarDepthRange, or
		// f1stPersonFarDepthRangeControlDriven with RenderDepth's and FUN_1414cb6d0's second argument (the movement controller's vfunc 0x78).
		constexpr std::uintptr_t kFarDepthRange = 0x202b2d0, kFarDepthRangeControlDriven = 0x202b2e8;

		/** @brief The inverse of a row-major 4x4 (cofactors, in double); zero when singular. */
		std::array<float, 16> Inverse(const std::array<float, 16>& a_m)
		{
			std::array<double, 16> m{}, inv{};
			for (std::uint32_t i = 0; i < 16; ++i)
				m[i] = a_m[i];
			inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
			inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
			inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
			inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
			inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
			inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
			inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
			inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
			inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
			inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
			inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
			inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
			inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
			inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
			inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
			inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
			const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
			std::array<float, 16> out{};
			if (det == 0.0)
				return out;
			for (std::uint32_t i = 0; i < 16; ++i)
				out[i] = static_cast<float>(inv[i] / det);
			return out;
		}

		std::array<float, 16> Multiply(const std::array<float, 16>& a_a, const std::array<float, 16>& a_b)
		{
			std::array<float, 16> out{};
			for (std::uint32_t r = 0; r < 4; ++r)
				for (std::uint32_t c = 0; c < 4; ++c)
					for (std::uint32_t k = 0; k < 4; ++k)
						out[r * 4 + c] += a_a[r * 4 + k] * a_b[k * 4 + c];
			return out;
		}
	}

	bool IndirectDraws::FirstPersonDrawable() const
	{
		// A capability (SceneStore::SetCapability, the scene lane's commit): the toggle, and the first-person targets published (the colour
		// epoch's formats, known once the main pass's targets were captured and kPOST_ZPREPASS_COPY exists). Each member then waits for its
		// own pipelines (FirstPersonSlotReady), and the engine draws it until the frame's epochs are covered (PassCapture).
		return impl && ActiveToggles().firstPerson && !failed && impl->firstPerson.LaneTargets().colour != DXGI_FORMAT_UNKNOWN;
	}

	bool IndirectDraws::FirstPersonCovered() const
	{
		return impl && impl->firstPersonCovered;
	}

	void IndirectDraws::BeginFirstPersonFrame()
	{
		ZoneScopedN("CS.DCLF.BeginFirstPersonFrame");
		if (!impl)
			return;
		auto& f = impl->firstPerson;
		const std::uint32_t frame = SceneStore::Get().GetFrame();
		const bool on = ActiveToggles().firstPerson && !failed;
		f.latch = false;
		f.latchFrame = frame;
		if (!on)
			return;
		++f.frames;
		// The latch: Main::Draw's cVar23 (AE 0x1406444b0, taken at its entry), recomputed here (Main_Draw_Early runs after FUN_1406452c0 and
		// before 0x1406445a1 hides the first-person root): the root's kHidden clear, Main::Draw's third argument false (FUN_140644360:
		// UI::IsMenuOpen("Main Menu")), and PlayerCamera +0x28 (the current state) not +0xD0 (the free camera, tfc).
		if (auto* player = RE::PlayerCharacter::GetSingleton())
			if (const auto* root = player->Get3D(true); root && root != player->Get3D(false) && !root->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
				auto* ui = RE::UI::GetSingleton();
				const auto* camera = RE::PlayerCamera::GetSingleton();
				const bool mainMenu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
				const bool freeCamera = camera && camera->IsCurrentState(RE::CameraState::kFree, RE::CameraState::kFree);
				f.latch = !mainMenu && camera && !freeCamera;
			}
		f.latched += f.latch ? 1 : 0;
		// The colour epoch's targets: the main pass's targets 0 and 1 (kMAIN and the motion vectors, as FUN_1414cb6d0 binds them; the
		// main pass's capture's formats) and kPOST_ZPREPASS_COPY's depth format.
		const auto& formats = impl->formats;
		auto* renderer = globals::game::renderer;
		if (renderer && formats.colorCount >= 2) {
			const auto& copy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			if (copy.views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				copy.views[0]->GetDesc(&dsvDesc);
				ForwardTargets targets;
				targets.colour = formats.colors[0];
				targets.colour2 = formats.colors[1];
				targets.depth = dsvDesc.Format;
				if (!(targets == f.targets)) {
					f.targets = targets;
					f.PublishTargets();
					impl->PostRevisionInputs();
					logger::info("[DCLF] first-person targets: colour {}, motion vectors {}, depth {}", static_cast<int>(targets.colour), static_cast<int>(targets.colour2),
						static_cast<int>(targets.depth));
				}
			}
		}
		if (f.targets.colour == DXGI_FORMAT_UNKNOWN)
			return;
		// The slots' first-person forward pipelines, asked of the pipeline lane (each key once) and read from the frame's catalog.
		auto& drawPipelines = DrawPipelines::Get();
		drawPipelines.SetFirstPersonTargets(f.targets);
		const auto* catalog = FrameCatalog();
		const auto& tables = SceneStore::Get().GetTables();
		// The pipeline slots the skeleton's geometry draws with: the installed tables' slots of the geometry under the first-person root
		// (the render thread's own walk of it, before Main::Draw touches it), found again every 16 frames or when the tables' slots grow.
		// Their pipelines are asked for before any is a member (the commit waits for them: FirstPersonSlotReady).
		if (frame - f.slotsFrame >= 16 || f.slotsWanted.size() != tables.pipelines.size()) {
			f.slotsFrame = frame;
			f.slotsWanted.assign(tables.pipelines.size(), 0);
			std::vector<RE::BSGeometry*> geometry;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				if (auto* root = player->Get3D(true); root && root != player->Get3D(false))
					SceneTracker::CollectGeometry(root, geometry);
			const ankerl::unordered_dense::set<const RE::BSGeometry*> skeleton(geometry.begin(), geometry.end());
			const auto claims = SceneStore::Get().FrameClaims();
			f.forwardMembersNow = 0;
			for (std::size_t o = 0; o < tables.objects.size() && !skeleton.empty(); ++o)
				if (o < tables.objectGeometry.size() && tables.objectGeometry[o] && skeleton.contains(tables.objectGeometry[o]) &&
					!(tables.objects[o].flags & kObjectFree) && tables.objects[o].pipelineIndex < f.slotsWanted.size()) {
					f.slotsWanted[tables.objects[o].pipelineIndex] = 1;
					// T7e's forward part: a forward member of the frame's claims (the report's count).
					if ((tables.objects[o].flags & kObjectForward) && claims && (claims->PhasesOf(tables.objectGeometry[o]) & kSetFirstPerson))
						++f.forwardMembersNow;
				}
			// CS_DCLF_SET_PARITY: the skeleton's slots and their claimed phases, logged when they change (which first-person geometry the
			// first-person view does not take, and what it takes instead).
			if (SwitchEnabled(Switch::SetParity) && claims) {
				std::string members;
				for (std::size_t o = 0; o < tables.objects.size() && !skeleton.empty(); ++o)
					if (o < tables.objectGeometry.size() && tables.objectGeometry[o] && skeleton.contains(tables.objectGeometry[o]) && !(tables.objects[o].flags & kObjectFree))
						members += fmt::format("{}'{}' {} slot {} flags {:#x} phases {:#x}", members.empty() ? "" : "; ", tables.objectGeometry[o]->name.c_str() ? tables.objectGeometry[o]->name.c_str() : "?", static_cast<const void*>(tables.objectGeometry[o]),
							o, tables.objects[o].flags, claims->PhasesOf(tables.objectGeometry[o]));
				if (members != f.loggedSkeleton) {
					f.loggedSkeleton = members;
					logger::info("[DCLF] first-person skeleton's slots (T7e, CS_DCLF_SET_PARITY): {} geometries walked; {}", geometry.size(), members);
				}
			}
		}
		// T7e's forward part: the main set's forward pipelines draw into this pass when its depth is the main pass's format (the colour
		// attachments are the main pass's own); the forward members the installed tables hold.
		const bool forwardCompatible = formats.depth != DXGI_FORMAT_UNKNOWN && formats.depth == f.targets.depth;
		if (f.forwardCompatible.exchange(forwardCompatible, std::memory_order_acq_rel) != forwardCompatible)
			logger::info("[DCLF] first-person forward part: the main depth format {} and kPOST_ZPREPASS_COPY's {}: {}", static_cast<int>(formats.depth),
				static_cast<int>(f.targets.depth), forwardCompatible ? "drawn by DCLF" : "the engine's");
		f.forwardMembers += f.forwardMembersNow;
		std::uint32_t slots = 0, ready = 0;
		f.slotPipelines.assign(tables.pipelines.size(), rhi::PipelineHandle{});
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			ForwardPipelineKey key;
			if (!f.slotsWanted[p] || !Draws::FirstPersonForwardKey(tables, p, key))
				continue;
			++slots;
			drawPipelines.RequestForward(key);
			if (const auto pipeline = ForwardPipelineOf(catalog, key, f.targets); pipeline.valid()) {
				f.slotPipelines[p] = pipeline;
				++ready;
			}
		}
		f.slots = slots;
		f.pipelinesReady = ready;
		if (impl->SetupFirstPerson())
			impl->ImportFirstPersonDepth();
	}

	void IndirectDraws::ExecuteFirstPersonDepth(bool a_controlDriven)
	{
		ZoneScopedN("CS.DCLF.ExecuteFirstPersonDepth");
		if (!impl)
			return;
		auto& f = impl->firstPerson;
		auto& store = SceneStore::Get();
		const std::uint32_t frameNumber = store.GetFrame();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		// The engine's depth range for the frame (a revision input: the shapes' viewport).
		if (const float range = *REL::Relocation<float*>{ REL::Offset(a_controlDriven ? kFarDepthRangeControlDriven : kFarDepthRange) }; range > 0.0f && range != f.depthRange) {
			f.depthRange = range;
			impl->PostRevisionInputs();
		}
		f.cameraCaptured = false;
		if (!ActiveToggles().firstPerson || !f.latch || f.latchFrame != frameNumber)
			return skip(0);
		const auto set = store.FrameClaims();
		if (!set || !(set->drawn & kSetFirstPerson))
			return skip(0);
		if (!impl->firstPersonCovered)
			return skip(1);
		// The first-person camera as FUN_1414cb020 set it up: its frustum and rotation, and its position in world space (DrawWorld +0x00:
		// its translation is in the root's space here).
		auto* camera = *REL::Relocation<RE::NiCamera**>{ REL::Offset(kFirstPersonCamera) };
		if (!camera)
			return skip(2);
		const auto* eyeWorld = REL::Relocation<const float*>{ REL::Offset(kDrawWorld) }.get();
		SunViews::Cascade view;
		SunViews::CameraMatrices(*camera, view);
		const RE::NiPoint3 eye{ eyeWorld[0], eyeWorld[1], eyeWorld[2] };
		// VS_PerFrame (b12) for the camera, as the engine's SetCameraData builds it for the first-person draws (the accumulator renders'
		// flags 3 and 0x43: jittered): the renderer's last block (the frame registers), the camera's registers over it.
		std::span<const std::byte> frameBlock;
		auto& mirror = ConstantMirror::Get();
		if (auto* perFrame = *globals::game::perFrame.get()) {
			mirror.Watch(perFrame);
			frameBlock = mirror.Contents(perFrame);
		}
		if (frameBlock.size() < SunViews::kBlockBytes) {
			const auto& cached = globals::game::frameBufferCached.data;
			static_assert(sizeof(cached) >= SunViews::kBlockBytes);
			frameBlock = std::as_bytes(std::span(&cached, 1));
		}
		auto& block = f.perFrame;
		block.fill(std::byte{ 0 });
		std::memcpy(block.data(), frameBlock.data(), std::min<std::size_t>(SunViews::kBlockBytes, block.size()));
		auto* out = reinterpret_cast<float*>(block.data());
		auto transposed = [&](std::uint32_t a_register, const std::array<float, 16>& a_m) {
			for (std::uint32_t row = 0; row < 4; ++row)
				for (std::uint32_t c = 0; c < 4; ++c)
					out[a_register * 4 + row * 4 + c] = a_m[c * 4 + row];
		};
		// The TAA / upscaler jitter (BSGraphics::State::projectionPosScaleX/Y, which SetCameraData adds to the projection's third row).
		auto jittered = view.proj;
		if (auto* state = RE::BSGraphics::State::GetSingleton()) {
			jittered[8] += state->projectionPosScaleX;
			jittered[9] += state->projectionPosScaleY;
		}
		const auto viewProjJittered = Multiply(view.view, jittered);
		transposed(0, view.view);
		transposed(4, jittered);
		transposed(8, viewProjJittered);
		transposed(12, view.viewProj);  // CameraViewProjUnjittered
		// CameraPreviousViewProjUnjittered and CameraPreviousPosAdjust: the camera's last frame (the motion vectors), its own on the first.
		transposed(16, f.havePrevious ? f.previousViewProjUnjittered : view.viewProj);
		const auto inverseProj = Inverse(jittered);
		transposed(20, inverseProj);
		transposed(24, jittered);
		transposed(28, Inverse(view.view));
		transposed(32, Inverse(viewProjJittered));
		transposed(36, inverseProj);
		out[160] = eye.x;
		out[161] = eye.y;
		out[162] = eye.z;
		const RE::NiPoint3 previousEye = f.havePrevious ? f.previousEye : eye;
		out[164] = previousEye.x;
		out[165] = previousEye.y;
		out[166] = previousEye.z;
		f.viewProj = view.viewProj;
		f.eye = eye;
		f.previousViewProjUnjittered = view.viewProj;
		f.previousEye = eye;
		f.havePrevious = true;
		f.cameraCaptured = true;

		// What the epoch draws from: the frame's scene list (its ring entry) with this frame's depth commit's index pool, inputs, object
		// records and geometry rows - the frame's publication whole.
		const auto main = impl->resources;
		const auto& depthCommit = main ? main->committed[kDepthShape] : Resources::Committed{};
		if (!main || !main->inputsDepth || !impl->scene || depthCommit.frame != frameNumber || depthCommit.sceneGeneration != impl->scene->generation ||
			depthCommit.objectCapacity != main->objectCapacity || depthCommit.rowsGeneration != main->MainRowsGeneration())
			return skip(3);
		const auto& ring = impl->ringFrame;
		const MainPayload* listed = ring.valid && ring.draws ? ring.draws->payloads[kAsyncZPrepass].get() : nullptr;
		if (!listed)
			return skip(3);
		const auto listInputs = static_cast<std::uint32_t>(listed->resident.Count() + listed->inputList.size());
		if (!f.resources || !f.resources->depthCopy)
			return skip(4);
		const auto revision = impl->RevisionOf(Impl::SceneRevisions::kFirstPersonDepthEpoch);
		if (!revision)
			return skip(1);
		const auto shape = revision.shape->Value<FirstPersonFrame>();
		const FirstPersonFrame& target = *shape;
		auto slots = std::max(static_cast<std::uint32_t>(target.map->size()), impl->InstalledPipelineSlots());
		slots = std::min(slots, target.latchLayout.slots);
		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup)
			return skip(4);
		auto owners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		for (const auto& held : impl->frameTextureBindings)
			if (held.binding.owner)
				owners->push_back(held.binding.owner);
		for (const auto& payload : ring.draws->payloads)
			if (payload)
				owners->insert(owners->end(), payload->bindingOwners.begin(), payload->bindingOwners.end());
		owners->push_back(ring.draws);
		if (impl->adoptedSnapshot)
			owners->push_back(impl->adoptedSnapshot);
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::FirstPersonDepth, [&](org::RenderGraph&) {
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			const auto& latchBlock = *target.latch;
			const auto& layout = target.latchLayout;
			const auto& writeMap = *target.map;
			const auto region = static_cast<std::uint32_t>(latchBlock.Offset(latchSlot));
			if (!writeMap.empty())
				LatchWrite(latchBlock, "first-person bucket map", latchSlot, FirstPersonLatchLayout::MapOffset(), std::as_bytes(std::span(writeMap)));
			// A pipeline slot the tables have gained since the map was made has no bucket (none of its draws is the frame's).
			if (slots > writeMap.size()) {
				const std::vector<std::uint32_t> none(slots - writeMap.size(), kNoBucket);
				LatchWrite(latchBlock, "first-person bucket map", latchSlot, FirstPersonLatchLayout::MapOffset() + static_cast<std::uint32_t>(writeMap.size() * sizeof(std::uint32_t)),
					std::as_bytes(std::span(none)));
			}
			std::vector<std::uint32_t> table;
			for (const auto& bucket : target.buckets) {
				table.push_back(bucket.first);
				table.push_back(bucket.capacity);
			}
			// T7e's forward part: the forward range's staging half, its entry after the buckets' (FirstPersonFrame::forwardEntry).
			if (target.forwardCapacity) {
				table.push_back(target.forwardFirst);
				table.push_back(target.forwardCapacity);
			}
			if (!table.empty())
				LatchWrite(latchBlock, "first-person bucket table", latchSlot, layout.TableOffset(), std::as_bytes(std::span(table)));
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (listInputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = listInputs;
			latch.cullFlags = 1;  // the frustum alone, near plane included
			latch.viewBits = kViewFirstPerson;  // the main list's first-person members alone
			latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			latch.fadeSeedsIndex = FrameValues::Get().FadeSeedsIndex();
			Impl::RingLatch(ring, latch);
			FoldEyeIntoViewProj(f.viewProj, f.eye, latch.viewProj);
			latch.bucketMapOffset = region + FirstPersonLatchLayout::MapOffset();
			latch.bucketTableOffset = region + layout.TableOffset();
			LatchWriteValue(latchBlock, "first-person culling latch", latchSlot, 0, latch);
			LatchWrite(latchBlock, "first-person per-frame data", latchSlot, layout.BlockOffset(), std::span<const std::byte>(f.perFrame.data(), f.perFrame.size()));
			impl->SubmitRevisionRecording(Impl::SceneRevisions::kFirstPersonDepthEpoch, *revision.recordings, 0);
		}, owners);
		if (!ok) {
			logger::error("[DCLF] the first-person depth epoch failed");
			return skip(5);
		}
		++f.depthEpochs;
		f.depthFrame = frameNumber;
	}

	void IndirectDraws::ExecuteFirstPersonColour()
	{
		ZoneScopedN("CS.DCLF.ExecuteFirstPersonColour");
		if (!impl)
			return;
		auto& f = impl->firstPerson;
		const std::uint32_t frameNumber = SceneStore::Get().GetFrame();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		if (!ActiveToggles().firstPerson || !f.latch || f.latchFrame != frameNumber || !impl->firstPersonCovered)
			return;
		// What it draws is what this frame's depth epoch culled (its sequences and bucket counts), with this frame's colour commit's frame
		// constants (the frame record, the lighting at PS b13).
		if (f.depthFrame != frameNumber)
			return skip(6);
		if (impl->committedFrame[kAsyncColour] != frameNumber || SceneStore::Get().SetWithdrawn())
			return skip(3);
		const auto revision = impl->RevisionOf(Impl::SceneRevisions::kFirstPersonEpoch);
		if (!revision)
			return skip(1);
		const auto cleanup = RenderGraphRuntime::Get().Host() ? RenderGraphRuntime::Get().Host()->ResourceCleanup() : nullptr;
		if (!cleanup)
			return skip(4);
		auto owners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		if (const MainPayload* colour = impl->committedPayload[kAsyncColour])
			*owners = colour->bindingOwners;
		for (const auto& held : impl->frameTextureBindings)
			if (held.binding.owner)
				owners->push_back(held.binding.owner);
		if (impl->ringFrame.draws)
			owners->push_back(impl->ringFrame.draws);
		if (impl->adoptedSnapshot)
			owners->push_back(impl->adoptedSnapshot);
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::FirstPerson, [&](org::RenderGraph&) {
			impl->SubmitRevisionRecording(Impl::SceneRevisions::kFirstPersonEpoch, *revision.recordings, 0);
		}, owners);
		if (!ok) {
			logger::error("[DCLF] the first-person colour epoch failed");
			return skip(5);
		}
		++f.colourEpochs;
	}

	std::string IndirectDraws::FirstPersonReport()
	{
		if (!impl)
			return {};
		auto& f = impl->firstPerson;
		const auto counts = PassCapture::Get().TakeFirstPersonCounts();
		if (!f.frames && !ActiveToggles().firstPerson && !counts.registered)
			return {};
		const auto& s = f.skipped;
		std::string text = fmt::format("[DCLF] first person (T7e, CS_DCLF_FIRST_PERSON {}): {} frames, {} the engine drew first person (latch); epochs: {} depth, {} colour; not drawn: "
									   "{} not the view's frame, {} not covered, {} without the camera, {} stale inputs, {} without resources, {} failed{}, {} colour without the "
									   "frame's depth epoch{}; targets colour {} motion {} depth {}, {} first-person pipeline slots, {} pipelines ready, depth range {:.4f}\n",
			ActiveToggles().firstPerson ? "on" : "off", f.frames, f.latched, f.depthEpochs, f.colourEpochs, s[0], s[1], s[2], s[3], s[4], s[5], s[5] ? " <- EPOCH FAILED" : "", s[6],
			s[6] ? " <- NO DEPTH EPOCH" : "", static_cast<int>(f.targets.colour), static_cast<int>(f.targets.colour2), static_cast<int>(f.targets.depth), f.slots,
			f.pipelinesReady, f.depthRange);
		const auto particles = FrameValues::TakeFirstPersonParticleStats();
		text += fmt::format("[DCLF] first person's forward part (T7e): {:.1f} forward members a frame (blended Lighting, effects, particles; the main set's pipelines "
							"through the colour signature, sorted back to front); particle members: {} expansions of {} particles faced to the first-person eye, "
							"their snapshots' bound at most {:.0f} units from it{}{}\n",
			f.frames ? static_cast<double>(f.forwardMembers) / static_cast<double>(f.frames) : 0.0, particles.expansions, particles.particles, particles.farthest,
			particles.farthest > 2000.0f ? " <- FIRST-PERSON PARTICLE SPACE" : "",
			f.forwardCompatible.load(std::memory_order_acquire) ? "" : "; the main depth format is not kPOST_ZPREPASS_COPY's: forward members stay the engine's");
		// The <- FIRST PERSON observer (CS_DCLF_SET_PARITY; PassCapture): the engine's first-person registrations of members, withheld, and
		// those it was handed (a member registered while its epochs did not draw: 0 on every covered frame).
		if (SwitchEnabled(Switch::SetParity) || counts.memberKept)
			text += fmt::format("[DCLF] first person's registrations (T7e, the first-person accumulator and RenderDepth's block): {} passes registered into the "
								"first-person accumulator, {} member passes withheld, {} member passes the engine drew{}\n",
				counts.registered, counts.withheld, counts.memberKept,
				counts.memberKept && f.depthEpochs ? " <- FIRST PERSON" : (counts.withheld || counts.registered ? " <- OK" : ""));
		const std::uint32_t depthFrame = f.depthFrame;
		f.frames = f.latched = f.depthEpochs = f.colourEpochs = f.forwardMembers = 0;
		f.skipped = {};
		f.depthFrame = depthFrame;
		return text;
	}
}
#endif
