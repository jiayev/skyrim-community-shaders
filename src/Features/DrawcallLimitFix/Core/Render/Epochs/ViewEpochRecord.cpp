// The views' epochs' recording (ViewEpoch.h): their passes (cull, a trees view's draw family, draws, forward range), their PassDefs
// (view.*, which Passes/CorePasses.cpp registers) and graph extension.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"

namespace DCLF
{
	namespace Draws
	{
		namespace
		{
			std::shared_ptr<const ViewFrame> CurrentViewFrame(const org::PassPrepareContext& a_preparation, ViewKind a_kind)
			{
				if (const auto* shapes = RevisionShapesOf(a_preparation))
					return shapes->views[ViewIndex(a_kind)];
				return nullptr;
			}

			struct BuildBindings
			{
				org::DeclaredViewToken sequences, count, visibility, poolFirsts;
				std::array<org::DeclaredViewToken, kMaxViewFaces> bucketCounts;
			};

			struct BuildPrepared
			{
				std::shared_ptr<const ComputeProgram> program;
				std::shared_ptr<const org::LatchBlock> latch;
				rhi::CommandSignatureHandle signature{};
				BuildDrawsConstants constants{};
				std::uint32_t faces = 0;
				std::array<std::uint32_t, kMaxViewFaces> bucketCountsIndex{};
			};

			/**
			 * @brief A view's cull: BuildDrawsCS in its single phase over the frame's scene list (the installed payload the latch names), frustum
			 * only, one dispatch per face; the latch's map sends each of the view's slots to its bucket and every other to none. A face
			 * not drawn has a dispatch of 0 groups.
			 */
			class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildPrepared, BuildBindings>
			{
			public:
				explicit BuildDrawsPass(std::shared_ptr<ViewResources> a_resources) :
					resources(std::move(a_resources)) {}

				BuildBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const auto& main = *resources->main;
					BuildBindings bindings{};
					bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
					bindings.count = a_builder.UnorderedAccess(resources->count).View();
					// Read only in the single phase (an input's published verdict), never written.
					bindings.visibility = a_builder.UnorderedAccess(*main.visibility).View();
					bindings.poolFirsts = a_builder.ShaderResource(*main.scene->pool->firsts).View();
					for (std::uint32_t f = 0; f < resources->traits->faces; ++f)
						bindings.bucketCounts[f] = a_builder.UnorderedAccess(*resources->bucketCounts[f]).View();
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					a_out.push_back(frame ? frame->generation : 0);
					a_out.push_back(NamedSerial(a_preparation, resources->main->scene->pool->firsts));
				}

				BuildPrepared Prepare(const BuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					BuildPrepared prepared{};
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					if (!frame || !frame->latch || !resources->buildDraws || !resources->dispatchSignature)
						return prepared;
					prepared.program = resources->buildDraws;
					prepared.latch = frame->latch;
					prepared.signature = resources->dispatchSignature->GetHandle();
					prepared.faces = resources->traits->faces;
					auto& constants = prepared.constants;
					constants.latchIndex = frame->latch->SrvIndex();
					constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
					constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
					constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
					constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
					SetRowStrides(constants);
					constants.phaseBits = 0;  // the single phase: frustum only (the latch's mode), nothing published
					// The bucket path's draws are bounded by their buckets (the latch's tables); this bounds the other, which none takes.
					constants.phaseTwoBase = prepared.faces * frame->sequenceDraws;
					for (std::uint32_t f = 0; f < prepared.faces; ++f)
						prepared.bucketCountsIndex[f] = CaptureViewIndex(a_preparation, a_bindings.bucketCounts[f]);
					return prepared;
				}

				static void Record(const BuildBindings&, const BuildPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.program || !a_prepared.latch)
						return;
					auto& commands = a_recording.Commands();
					commands.BindLayout(a_prepared.program->layout->GetHandle());
					commands.BindPipeline(a_prepared.program->Pipeline());
					for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
						auto constants = a_prepared.constants;
						constants.bucketCountsIndex = a_prepared.bucketCountsIndex[f];
						RecordLatchedDispatch(constants, *a_prepared.latch, a_prepared.signature, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), a_recording);
					}
				}

			private:
				std::shared_ptr<ViewResources> resources;
			};

			/** @brief What a view's draw pass draws into: the reflection's faces, first person's main depth, first person's colour. */
			enum class DrawMode : std::uint8_t
			{
				Faces,
				Depth,
				Colour,
			};

			struct DrawBindings
			{
				std::array<org::DeclaredViewToken, kMaxViewFaces> colours;  // a face's cube slice each, or the main targets
				org::DeclaredViewToken depth;
				org::ResourceBindingToken sequences, pool;
				std::array<org::ResourceBindingToken, kMaxViewFaces> bucketCounts;
				std::array<std::array<org::ResourceBindingToken, kMaxViewFaces>, Extensions::kDrawFamilies> faceLists;  // by draw family
			};

			struct DrawPrepared
			{
				std::shared_ptr<const ViewFrame> frame;
				DrawMode mode = DrawMode::Faces;
				std::uint32_t faces = 0;
				std::array<org::PreparedDescriptorReference, kMaxViewFaces> colours{};
				org::PreparedDescriptorReference depth{};
				std::array<std::array<std::uint64_t, kMaxViewFaces>, Extensions::kDrawFamilies> faceRows{};  // by draw family
			};

			/**
			 * @brief A view's draws, per face a pass and per bucket a plain indirect draw of the bucket's range of the face's sequences
			 * (DCLF_PULLED, as the Z-prepass's):
			 * - Faces: on the face's slice of the engine's cube target (loaded and stored: the engine's sky is there) with DCLF's depth
			 *   cleared, each bucket's forward pipeline, then the face's draw families (tree LOD).
			 * - Depth: each bucket's Z-prepass depth pipeline into the main depth (after the world's and DCLF's HZB: the HZB keeps leaving
			 *   the arms out).
			 * - Colour: each bucket's first-person forward pipeline into kMAIN and the motion vectors (the engine's first-person slots 0 and
			 *   1; slot 2 unbound), tested against kPOST_ZPREPASS_COPY (FUN_1414cb6d0's depth target 7).
			 * The view's viewport depth range ([0, f1stPersonFarDepthRange]: both of the engine's first-person passes set it).
			 */
			class DrawPass final : public org::TypedRenderGraphPass<DrawPass, DrawPrepared, DrawBindings>
			{
			public:
				DrawPass(std::shared_ptr<ViewResources> a_resources, DrawMode a_mode) :
					resources(std::move(a_resources)), mode(a_mode) {}

				DrawBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const std::span<const org::SrvView> noViews{};
					const auto& main = *resources->main;
					const auto& scene = *main.scene;
					const std::uint32_t faces = resources->traits->faces;
					DrawBindings bindings{};
					if (mode == DrawMode::Faces) {
						for (std::uint32_t f = 0; f < faces; ++f)
							bindings.colours[f] = a_builder.RenderTarget(resources->target, org::RtvView{ UINT32_MAX, 0, f }).View();
						bindings.depth = a_builder.DepthReadWrite(resources->depth).View();
					} else if (mode == DrawMode::Colour) {
						for (std::uint32_t i = 0; i < 2; ++i)
							bindings.colours[i] = a_builder.RenderTarget(main.native[i]).View();
						bindings.depth = a_builder.DepthReadWrite(resources->target).View();
					} else {
						bindings.depth = a_builder.DepthReadWrite(main.nativeDepth).View();
					}
					// The sequences are the draws' arguments and what the vertex stage reads by device address (DCLF_PULLED).
					bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
					a_builder.ShaderResource(*resources->sequences, noViews);
					bindings.pool = a_builder.IndexBuffer(*scene.pool->indices);
					for (std::uint32_t f = 0; f < faces; ++f)
						bindings.bucketCounts[f] = a_builder.IndirectArguments(*resources->bucketCounts[f]).Resource();
					// Read through device addresses; declared so the graph orders them after their uploads.
					// The view's own frame slots (the faces': its commit's) or the colour segment's, by address.
					a_builder.ShaderResource(resources->frameConstants ? resources->frameConstants : main.frameConstants, noViews);
					// With its own frame constants, its copies of the main frame buffers (its commit's), named by its frame record.
					if (const auto frameBuffers = resources->frameBuffers.load(std::memory_order_acquire))
						for (const auto& frameBuffer : *frameBuffers)
							a_builder.ShaderResource(frameBuffer.copy, noViews);
					if (scene.vertexStream)
						a_builder.ShaderResource(*scene.vertexStream, noViews);
					a_builder.ShaderResource(resources->blocks, noViews);
					if (mode == DrawMode::Colour)
						DeclareFrameBuffers(a_builder, main);
					// The faces' draw families (tree LOD's tables, the faces' rows and lists).
					for (std::size_t d = 0; d < Extensions::kDrawFamilies && mode == DrawMode::Faces; ++d)
						if (const auto& family = Extensions::DrawFamilies()[d].contribution.faces; Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d))) {
							Api::DrawFamilyDeclare declare{ a_builder, scene, resources.get(), std::span(bindings.faceLists[d]) };
							family.declare(declare);
						}
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					a_out.push_back(frame ? frame->generation : 0);
					a_out.push_back(NamedSerial(a_preparation, resources->main->scene->pool->indices));
					a_out.push_back(NamedSerial(a_preparation, resources->sequences));
					a_out.push_back(static_cast<std::uint64_t>(mode));
				}

				DrawPrepared Prepare(const DrawBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					DrawPrepared prepared{};
					auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					if (!frame || !frame->indirect.valid || !frame->width || !frame->height || (mode != DrawMode::Faces && frame->buckets.empty()))
						return prepared;
					prepared.faces = resources->traits->faces;
					const std::uint32_t colours = mode == DrawMode::Faces ? prepared.faces : mode == DrawMode::Colour ? 2u : 0u;
					for (std::uint32_t i = 0; i < colours; ++i)
						prepared.colours[i] = a_preparation.Capture(a_bindings.colours[i]);
					for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
						if (Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)))
							for (std::uint32_t f = 0; f < prepared.faces; ++f)
								prepared.faceRows[d][f] = resources->faceRowsAddress[d][f];
					prepared.depth = a_preparation.Capture(a_bindings.depth);
					prepared.frame = std::move(frame);
					prepared.mode = mode;
					return prepared;
				}

				static void Record(const DrawBindings& a_bindings, const DrawPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.frame)
						return;
					const auto& frame = *a_prepared.frame;
					const auto mode = a_prepared.mode;
					auto& commands = a_recording.Commands();
					commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
					auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
					const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
					const std::uint64_t sequencesAddress = device.GetBufferDeviceAddress({ sequences, 0 });
					const rhi::IndexBufferView pool{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt };
					const auto& families = Extensions::DrawFamilies();
					static const std::shared_ptr<const void> kNothing;  // a face's draw has no prepared state: its pipeline is the shape's
					for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
						std::array<rhi::ColorAttachment, 2> colours{};
						rhi::PassBeginInfo begin{};
						if (mode == DrawMode::Faces) {
							colours[0].rtv = a_recording.Resolve(a_prepared.colours[f]);
							colours[0].loadOp = rhi::LoadOp::Load;
							colours[0].storeOp = rhi::StoreOp::Store;
							begin.colors = { colours.data(), 1 };
						} else if (mode == DrawMode::Colour) {
							for (std::uint32_t i = 0; i < 2; ++i) {
								colours[i].rtv = a_recording.Resolve(a_prepared.colours[i]);
								colours[i].loadOp = rhi::LoadOp::Load;
								colours[i].storeOp = rhi::StoreOp::Store;
								colours[i].resource = a_recording.Resolve(a_bindings.colours[i].Resource()).GetHandle();
							}
							begin.colors = { colours.data(), 2 };
						}
						rhi::DepthAttachment depth{};
						depth.dsv = a_recording.Resolve(a_prepared.depth);
						depth.depthLoad = mode == DrawMode::Faces ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
						depth.depthStore = rhi::StoreOp::Store;
						if (mode == DrawMode::Faces) {
							depth.clear.type = rhi::ClearValueType::DepthStencil;
							depth.clear.format = rhi::Format::D24_UNorm_S8_UInt;
							depth.clear.depthStencil = { 1.0f, 0 };
						} else {
							depth.stencilLoad = rhi::LoadOp::Load;
							depth.stencilStore = rhi::StoreOp::Store;
						}
						begin.depth = &depth;
						begin.width = frame.width;
						begin.height = frame.height;
						begin.minDepth = 0.0f;
						begin.maxDepth = frame.depthRange;
						begin.debugName = mode == DrawMode::Faces ? "DCLF reflection face" : mode == DrawMode::Colour ? "DCLF first person" : "DCLF first-person depth";
						commands.BeginPass(begin);
						commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
						commands.BindLayout(frame.indirect.zLayout);
						commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, frame.push[f].data());
						commands.SetIndexBuffer(pool);
						const auto counts = a_recording.Resolve(a_bindings.bucketCounts[f]).GetHandle();
						for (std::uint32_t b = 0; b < frame.buckets.size(); ++b) {
							const auto& bucket = frame.buckets[b];
							if (!bucket.capacity)
								continue;
							RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesAddress, std::uint64_t(f) * frame.sequenceDraws + bucket.first,
								mode == DrawMode::Depth ? bucket.depth : bucket.colour, counts, b, bucket.capacity);
						}
						// The faces' draw families (tree LOD: one instanced draw of the face's list, its row the face's), with the shape's pipeline for each.
						for (std::size_t d = 0; d < Extensions::kDrawFamilies && mode == DrawMode::Faces; ++d) {
							const auto& faces = families[d].contribution.faces;
							const auto& drawn = frame.faces[d];
							if (Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)) && drawn.pipeline.valid() && drawn.groups) {
								faces.record({ commands, Api::DrawFamilyPass::Face, kNothing, a_prepared.faceRows[d][f], a_recording.Resolve(a_bindings.faceLists[d][f]).GetHandle(),
									drawn.pipeline, drawn.signature });
							}
						}
						commands.EndPass();
					}
				}

			private:
				std::shared_ptr<ViewResources> resources;
				DrawMode mode;
			};


			struct ForwardBindings
			{
				std::array<org::DeclaredViewToken, kColorTargets> targets;
				org::DeclaredViewToken depth;
				org::ResourceBindingToken sequences, counts;
			};

			struct ForwardPrepared
			{
				std::shared_ptr<const ViewFrame> frame;
				PreprocessStates* preprocess = nullptr;
				std::uint32_t targetCount = 0;
				std::array<org::PreparedDescriptorReference, kColorTargets> targets{};
				org::PreparedDescriptorReference depth{};
			};

			/**
			 * @brief A view's forward range's sorted halves drawn through the main set's colour signatures, a call per state class (the main
			 * set's forward pipelines, a pipeline per sequence: blended Lighting, the Effect port's meshes and particles), after the view's bucket draws, with the view's
			 * frame push data and depth range, onto the main pass's attachments (every one loaded: the forward pipelines write the main target
			 * alone) tested against the view's depth target, as the engine's first-person blended list is drawn after its opaque passes.
			 */
			class ForwardPass final : public org::TypedRenderGraphPass<ForwardPass, ForwardPrepared, ForwardBindings>
			{
			public:
				explicit ForwardPass(std::shared_ptr<ViewResources> a_resources) :
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
					bindings.depth = a_builder.DepthReadWrite(resources->target).View();
					bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
					bindings.counts = a_builder.IndirectArguments(*resources->bucketCounts[0]).Resource();
					// The face shapes' second vertex stream, pulled by address (DCLF_PULLED_VERTICES).
					if (scene.vertexStream)
						a_builder.ShaderResource(*scene.vertexStream, noViews);
					a_builder.ShaderResource(resources->blocks, noViews);
					DeclareFrameBuffers(a_builder, main);
					a_builder.ShaderResource(main.frameConstants, noViews);
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					a_out.push_back(frame ? frame->generation : 0);
				}

				ForwardPrepared Prepare(const ForwardBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					ForwardPrepared prepared{};
					auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					if (!frame || frame->forwardRanges.empty() || !frame->indirect.valid || !frame->width || !frame->height || !resources->preprocessForward ||
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
					auto colours = LoadedColours(a_recording, a_prepared.targets, a_bindings.targets, a_prepared.targetCount);
					auto depth = LoadedDepth(a_recording, a_prepared.depth);
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
					// Per state class with forward slots, its sorted half (after its staging half), counted by its entry's bucket count word,
					// with its class's colour signature; the classes in creation order.
					std::vector<IndirectCall> calls;
					for (std::uint32_t c = 0; c < frame.forwardRanges.size() && c < frame.indirect.classes.size(); ++c)
						if (const auto& range = frame.forwardRanges[c]; range.capacity)
							calls.push_back({ frame.indirect.classes[c].signatures[kColorVariant], (std::uint64_t(range.first) + range.capacity) * sizeof(DrawSequence), counts,
								std::uint64_t(frame.forwardEntry + c) * sizeof(std::uint32_t), range.capacity });
					RecordPreprocessedDraws(a_recording, *a_prepared.preprocess, begin, frame.resourceHeap, frame.samplerHeap, frame.indirect.layout, frame.push[0], sequences,
						calls);
				}

			private:
				std::shared_ptr<ViewResources> resources;
			};

			class ViewExtension final : public org::RenderGraph::IRenderGraphExtension
			{
			public:
				explicit ViewExtension(std::shared_ptr<ViewResources> a_resources) :
					resources(std::move(a_resources)) {}

				void PrepareForBuild(org::RenderGraph& a_graph) override
				{
					// The main pass's, under the main extension's identifiers (the same registration again).
					const auto& main = *resources->main;
					Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility"), main.visibility);
					RegisterSceneBuffers(a_graph, *main.scene);
					// Its own.
					const auto& traits = *resources->traits;
					const auto id = [&](std::string_view a_name) { return org::ResourceIdentifier(fmt::format("{}.{}", traits.prefix, a_name)); };
					Register(a_graph, id("draw-count"), resources->count);
					Register(a_graph, id(traits.blocks), resources->blocks);
					if (resources->frameConstants)
						Register(a_graph, id("frame-constants"), resources->frameConstants);
					if (const auto frameBuffers = resources->frameBuffers.load(std::memory_order_acquire))
						for (const auto& frameBuffer : *frameBuffers)
							Register(a_graph, id(fmt::format("frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
					if (resources->depth)
						Register(a_graph, id("depth"), resources->depth);
					Register(a_graph, id(traits.target), resources->target);
					for (std::size_t d = 0; d < Extensions::kDrawFamilies && traits.trees; ++d)
						if (Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)))
							for (std::uint32_t f = 0; f < traits.faces; ++f)
								Register(a_graph, id(fmt::format("{}{}", Extensions::DrawFamilies()[d].rowName, f)), resources->faceRows[d][f]);
				}

				void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
				{
					if (!resources->main || !ViewPassesReady(*resources, *resources->main))
						return;
					// The view's epochs' passes (Schedule/Frame.cpp; none for a hosted epoch: its host's extension adds them).
					Api::PassContext context{};
					context.main = &resources->main;
					context.view = &resources;
					GatherScheduled(Api::EpochShape::View, resources->traits->kind, context, a_out);
				}

			private:
				std::shared_ptr<ViewResources> resources;
			};

			std::shared_ptr<org::RenderPass> LatchedCopies(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				return MakeLatchedCopiesPass([resources] { return ViewLatchedTargets(*resources); }, [kind = resources->traits->kind](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentViewFrame(a_preparation, kind);
					return frame ? LatchedSource{ frame->generation, frame, frame->latched } : LatchedSource{};
				});
			}

			std::shared_ptr<org::RenderPass> BuildDraws(const Api::PassContext& a_context)
			{
				return std::make_shared<BuildDrawsPass>(a_context.View());
			}

			// A view without a depth pass draws its faces in its one epoch (the reflection's: hosted by the depth epoch); one with a depth
			// pass its depth (first person's: hosted by the depth epoch, after the HZB and both phases' depth), then its colour in the
			// colour epoch.
			std::shared_ptr<org::RenderPass> FacesDraw(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				return !resources->traits->depthPass ? std::make_shared<DrawPass>(resources, DrawMode::Faces) : nullptr;
			}

			std::shared_ptr<org::RenderPass> DepthDraw(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				return resources->traits->depthPass ? std::make_shared<DrawPass>(resources, DrawMode::Depth) : nullptr;
			}

			// The forward range sorted with the depth epoch (after its BuildDraws), drawn in the colour epoch after the bucket draws.
			std::shared_ptr<org::RenderPass> ForwardSort(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				const auto& traits = *resources->traits;
				if (!traits.depthPass || !traits.forwardPart || !resources->main->forwardSort)
					return nullptr;
				return MakeForwardSortPass(resources->sequences, { nullptr, resources->bucketCounts[0] }, [view = resources](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentViewFrame(a_preparation, view->traits->kind);
					if (!frame || !frame->latch)
						return ForwardSortSource{ frame ? frame->generation : 0 };
					// Each state class's forward pair, its count its entry's bucket count word, after the buckets'.
					ForwardSortSource source{ frame->generation, view->main->forwardSort };
					for (std::uint32_t c = 0; c < frame->forwardRanges.size(); ++c)
						if (const auto& range = frame->forwardRanges[c]; range.capacity)
							source.ranges.push_back({ range.first, range.capacity, (frame->forwardEntry + c) * static_cast<std::uint32_t>(sizeof(std::uint32_t)) });
					return source;
				});
			}

			std::shared_ptr<org::RenderPass> ColourDraw(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				return resources->traits->depthPass ? std::make_shared<DrawPass>(resources, DrawMode::Colour) : nullptr;
			}

			std::shared_ptr<org::RenderPass> ForwardDraw(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				const auto& traits = *resources->traits;
				return traits.depthPass && traits.forwardPart && resources->main->forwardSort && resources->preprocessForward ? std::make_shared<ForwardPass>(resources) : nullptr;
			}

			// Named with the view's prefix (ViewTraits::prefix).
			constexpr Api::PassDef kViewPasses[] = {
				{ .key = "view.latched-copies", .passName = "{view}.latched-copies", .kind = PassKind::Copy, .make = &LatchedCopies },
				{ .key = "view.build-draws", .passName = "{view}.build-draws", .make = &BuildDraws },
				{ .key = "view.faces", .passName = "{view}.faces", .kind = PassKind::Render, .make = &FacesDraw },
				{ .key = "view.depth", .passName = "{view}.depth", .kind = PassKind::Render, .make = &DepthDraw },
				{ .key = "view.forward-sort", .passName = "{view}.forward-sort", .make = &ForwardSort },
				{ .key = "view.colour", .passName = "{view}.colour", .kind = PassKind::Render, .make = &ColourDraw },
				{ .key = "view.forward", .passName = "{view}.forward", .kind = PassKind::Render, .make = &ForwardDraw },
			};
		}

		std::span<const Api::PassDef> ViewPassDefs()
		{
			return kViewPasses;
		}

		bool ViewPassesReady(const ViewResources& a_view, const Resources& a_main)
		{
			return a_view.main.get() == &a_main && a_main.scene && a_main.scene->pool && a_view.target &&
			       (!a_view.traits->depthPass || (a_main.nativeDepth && a_main.native[0] && a_main.native[1]));
		}

		std::vector<LatchedTarget> ViewLatchedTargets(const ViewResources& a_view)
		{
			std::vector<LatchedTarget> out{ { a_view.count, nullptr }, { a_view.blocks, nullptr } };
			if (a_view.frameConstants)
				out.push_back({ a_view.frameConstants, nullptr });
			if (const auto frameBuffers = a_view.frameBuffers.load(std::memory_order_acquire))
				for (const auto& frameBuffer : *frameBuffers)
					out.push_back({ frameBuffer.copy, nullptr });
			std::array<bool, Extensions::kDrawFamilies> families{};
			for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
				families[d] = FamilySetUp(*a_view.main->scene, static_cast<Api::DrawFamily>(d)) && a_view.faceLists[d][0];
			for (std::uint32_t f = 0; f < a_view.traits->faces; ++f) {
				out.push_back({ nullptr, a_view.bucketCounts[f] });
				for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
					if (families[d]) {
						out.push_back({ a_view.faceRows[d][f], nullptr });
						out.push_back({ nullptr, a_view.faceLists[d][f] });
					}
			}
			return out;
		}

		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeViewExtension(std::shared_ptr<ViewResources> a_resources)
		{
			return std::make_unique<ViewExtension>(std::move(a_resources));
		}
	}
}
