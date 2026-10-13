// The views' epochs (ViewEpoch.h): their traits, forward keys, bucket plans and shapes, their passes and graph extension, their
// cameras' VS_PerFrame and their commits (render thread). Their captures and coverage are Reflection.cpp's and FirstPerson.cpp's.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"

#	include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

namespace DCLF
{

	namespace Draws
	{
		const ViewTraits& TraitsOf(ViewKind a_kind)
		{
			using Segment = RenderGraphRuntime::Segment;
			static const ViewTraits kTraits[kViews] = {
				{ ViewKind::Reflection, kReflectionFaces, true, false, false, kViewReflection, { Epoch::Reflection, Epoch::Count }, { Segment::Reflection, Segment::Reflection },
					"reflection", "cs.dclf.reflection", "face-blocks", "cube",
					{ "reflection bucket map", "reflection bucket tables", "reflection culling latches", "reflection face per-frame data", "reflection tree rows" } },
				{ ViewKind::FirstPerson, 1, false, true, true, kViewFirstPerson, { Epoch::FirstPersonDepth, Epoch::FirstPersonColour },
					{ Segment::FirstPersonDepth, Segment::FirstPerson }, "first-person", "cs.dclf.first-person", "block", "depth-copy",
					{ "first-person bucket map", "first-person bucket table", "first-person culling latch", "first-person per-frame data", "first-person tree rows" } },
			};
			return kTraits[ViewIndex(a_kind)];
		}

		bool ViewForwardKey(ViewKind a_kind, const SceneTables& a_tables, std::size_t a_slot, ForwardPipelineKey& a_key)
		{
			if (a_slot >= a_tables.pipelines.size() || !a_tables.PipelineUsed(a_slot))
				return false;
			const auto& pipelineKey = a_tables.pipelines[a_slot];
			const std::uint32_t twoSided = (pipelineKey.rasterFlags & kRasterTwoSided) ? ForwardPipelineKey::kForwardTwoSided : 0u;
			if (a_kind == ViewKind::Reflection) {
				if (!LodLightingTechnique(pipelineKey.passDescriptor))
					return false;
				a_key = ForwardPipelineKey{ pipelineKey.vertexDescriptor, pipelineKey.pixelDescriptor & ~kLightingPixelDeferred, twoSided };
				return true;
			}
			// First person: a forward key's slot draws with the main set's colour pipeline in the forward range (ViewForwardSlotsOf). The
			// vertex descriptor is the pulled vertex stage (skins read their palette as the Z-prepass's do); the pixel descriptor drops
			// Deferred (the engine draws first person forward, after the composite: Community Shaders' Deferred sets InWorld around it).
			if (RasterForward(pipelineKey.rasterFlags))
				return false;
			a_key = ForwardPipelineKey{ pipelineKey.vertexDescriptor, pipelineKey.pixelDescriptor & ~kLightingPixelDeferred, ForwardPipelineKey::kForwardFirstPerson | twoSided };
			return true;
		}

		void ViewPipelinesOf(ViewKind a_kind, const PipelineCatalog* a_catalog, const SceneTables& a_tables, const ForwardTargets& a_targets, bool a_on,
			std::vector<rhi::PipelineHandle>& a_slots)
		{
			a_slots.assign(a_on ? a_tables.pipelines.size() : 0, rhi::PipelineHandle{});
			for (std::size_t p = 0; p < a_slots.size(); ++p)
				if (ForwardPipelineKey key; ViewForwardKey(a_kind, a_tables, p, key))
					a_slots[p] = ForwardPipelineOf(a_catalog, key, a_targets);
		}

		void ViewForwardSlotsOf(const SceneTables& a_tables, bool a_on, std::vector<std::uint8_t>& a_slots)
		{
			a_slots.assign(a_on ? a_tables.pipelines.size() : 0, 0);
			for (std::size_t p = 0; p < a_slots.size(); ++p)
				a_slots[p] = a_tables.PipelineUsed(p) && RasterForward(a_tables.pipelines[p].rasterFlags) && !RasterRefraction(a_tables.pipelines[p].rasterFlags) ? 1 : 0;
		}

		std::uint32_t ZGroupOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot)
		{
			// The Z-prepass groups are the set's (IndirectState::zGroups, by set index): a pipeline slot's is its lookup entry's set
			// index's, as PlanZBuckets takes it.
			if (!a_lookups || a_slot >= a_lookups->pipelines.size())
				return kNoBucket;
			const auto setIndex = a_lookups->pipelines[a_slot].setIndex;
			return setIndex != Lookups::kNone && setIndex < a_indirect.zGroups.size() ? a_indirect.zGroups[setIndex] : kNoBucket;
		}

		void PlanViewBuckets(const ViewTraits& a_traits, const MainSizing& a_main, const Lookups* a_lookups, const IndirectState& a_indirect,
			std::span<const rhi::PipelineHandle> a_slotPipelines, std::span<const std::uint8_t> a_forwardSlots, ViewPlan& a_out)
		{
			const auto slots = static_cast<std::uint32_t>(a_main.zBucketCapacity.size());
			a_out.map.assign(slots, kNoBucket);
			a_out.buckets.clear();
			for (std::uint32_t p = 0; p < slots; ++p) {
				if (p >= a_slotPipelines.size() || !a_slotPipelines[p].valid() || !a_main.zBucketCapacity[p])
					continue;
				rhi::PipelineHandle depth{};
				if (a_traits.depthPass) {
					const std::uint32_t group = ZGroupOf(a_lookups, a_indirect, p);
					if (group >= a_indirect.zPipelines.size() || !a_indirect.zPipelines[group].valid())
						continue;
					depth = a_indirect.zPipelines[group];
				}
				const auto colour = a_slotPipelines[p];
				auto it = std::find_if(a_out.buckets.begin(), a_out.buckets.end(),
					[&](const auto& a_bucket) { return SameHandle(a_bucket.depth, depth) && SameHandle(a_bucket.colour, colour); });
				if (it == a_out.buckets.end())
					it = a_out.buckets.insert(a_out.buckets.end(), ViewFrame::Bucket{ depth, colour, 0, 0 });
				it->capacity += a_main.zBucketCapacity[p];
				a_out.map[p] = static_cast<std::uint32_t>(it - a_out.buckets.begin());
			}
			a_out.draws = 0;
			for (auto& bucket : a_out.buckets) {
				bucket.first = a_out.draws;
				a_out.draws += bucket.capacity;
			}
			// The forward range: every forward slot's map word names its table entry (after the buckets) with kFirstPersonForwardBucket
			// (BuildDrawsCS.hlsl). It holds every draw the forward slots can produce (the main forward range's capacity), staged then
			// sorted: two halves after the buckets' ranges.
			a_out.forwardFirst = a_out.forwardCapacity = 0;
			bool forward = false;
			for (std::uint32_t p = 0; p < slots && p < a_forwardSlots.size(); ++p)
				if (a_forwardSlots[p] && a_out.map[p] == kNoBucket) {
					a_out.map[p] = kFirstPersonForwardBucket | static_cast<std::uint32_t>(a_out.buckets.size());
					forward = true;
				}
			if (forward && a_main.forwardCapacity) {
				a_out.forwardFirst = a_out.draws;
				a_out.forwardCapacity = a_main.forwardCapacity;
				a_out.draws += 2 * a_main.forwardCapacity;
			} else if (forward) {
				for (auto& word : a_out.map)
					if (word != kNoBucket && (word & kFirstPersonForwardBucket))
						word = kNoBucket;
			}
		}

		std::shared_ptr<ViewFrame> MakeViewShape(const ViewResources& a_resources, const ViewShapeInputs& a_in)
		{
			const auto& traits = *a_resources.traits;
			const auto& latches = *a_in.latches;
			auto frame = std::make_shared<ViewFrame>();
			frame->kind = traits.kind;
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->latch = latches.latch;
			frame->width = a_in.width;
			frame->height = a_in.height;
			frame->depthRange = a_in.depthRange;
			frame->materialRows = a_in.materialRows;
			frame->pipelineRows = a_in.pipelineRows;
			frame->sequenceDraws = (a_in.sizing ? *a_in.sizing : static_cast<const ViewSizing&>(a_resources)).sequenceDraws;
			frame->buckets = a_in.plan.buckets;
			frame->forwardFirst = a_in.plan.forwardFirst;
			frame->forwardCapacity = a_in.plan.forwardCapacity;
			frame->forwardEntry = static_cast<std::uint32_t>(a_in.plan.buckets.size());
			frame->latchLayout = latches.layout;
			frame->map = std::make_shared<const std::vector<std::uint32_t>>(a_in.plan.map);
			if (a_in.tree.valid()) {
				frame->tree = a_in.tree;
				frame->treeSignature = a_in.treeSignature;
				frame->treeGroups = static_cast<std::uint32_t>((std::uint64_t(a_in.treeShapes) * TreeLod::kMaxGroupInstances + kTreeLodCullGroup - 1) / kTreeLodCullGroup);
			}
			// Per face, the colour segment's frame constants (the frame lighting at PS b13, the frame record) with VS and PS b12 its block.
			const auto base = FramePushWords(a_in.frameConstantsAddress);
			frame->push.assign(traits.faces, base);
			for (std::uint32_t f = 0; f < traits.faces; ++f) {
				const std::uint64_t block = a_resources.blocksAddress + std::uint64_t(f) * kViewBlockBytes;
				for (const bool pixel : { false, true })
					if (const std::uint32_t word = FramePushWord(pixel, kPerFrameVertexRegister); word != ~0u) {
						frame->push[f][word] = static_cast<std::uint32_t>(block);
						frame->push[f][word + 1] = static_cast<std::uint32_t>(block >> 32);
					}
			}
			// The latched copies: each face's block (and tree LOD row and list header) from the latch, the counters zeroed.
			auto& fromLatch = frame->latched[0];
			auto& fromZeros = frame->latched[1];
			fromLatch.latch = latches.latch;
			fromZeros.latch = latches.zeros;
			fromZeros.fixed = true;
			fromZeros.copies.push_back({ a_resources.count.get(), 0, static_cast<std::uint32_t>(sizeof(kZeroCounts)), 0 });
			const auto bucketBytes = static_cast<std::uint32_t>(std::max<std::size_t>(frame->buckets.size() + (frame->forwardCapacity ? 1 : 0), 1) * sizeof(std::uint32_t));
			for (std::uint32_t f = 0; f < traits.faces; ++f) {
				const std::uint32_t face = latches.layout.FaceOffset(f);
				fromZeros.copies.push_back({ a_resources.bucketCounts[f]->Key(), 0, bucketBytes, 0 });
				fromLatch.copies.push_back({ a_resources.blocks.get(), face, kViewBlockBytes, std::uint64_t(f) * kViewBlockBytes });
				if (frame->tree.valid() && a_resources.treeRows[f] && a_resources.treeVisible[f]) {
					fromLatch.copies.push_back({ a_resources.treeRows[f].get(), face + ViewLatchLayout::kTreeRowInFace, static_cast<std::uint32_t>(sizeof(TreeLod::DrawRow)), 0 });
					fromLatch.copies.push_back({ a_resources.treeVisible[f]->Key(), face + ViewLatchLayout::kTreeHeaderInFace, static_cast<std::uint32_t>(sizeof(TreeLod::VisibleHeader)), 0 });
				}
			}
			return frame;
		}

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

		std::span<const std::byte> FrameBlockNow()
		{
			std::span<const std::byte> frame;
			auto& mirror = ConstantMirror::Get();
			if (auto* perFrame = *globals::game::perFrame.get()) {
				mirror.Watch(perFrame);
				frame = mirror.Contents(perFrame);
			}
			if (frame.size() < SunViews::kBlockBytes) {
				const auto& cached = globals::game::frameBufferCached.data;
				static_assert(sizeof(cached) >= SunViews::kBlockBytes);
				frame = std::as_bytes(std::span(&cached, 1));
			}
			return frame;
		}

		void PerFrameBlock(const SunViews::Cascade& a_camera, std::span<const std::byte> a_frame, bool a_jitter, const PreviousCamera* a_previous, std::byte* a_out)
		{
			std::memcpy(a_out, a_frame.data(), SunViews::kBlockBytes);
			auto* out = reinterpret_cast<float*>(a_out);
			auto transposed = [&](std::uint32_t a_register, const std::array<float, 16>& a_m) {
				for (std::uint32_t row = 0; row < 4; ++row)
					for (std::uint32_t c = 0; c < 4; ++c)
						out[a_register * 4 + row * 4 + c] = a_m[c * 4 + row];
			};
			auto proj = a_camera.proj;
			auto viewProj = a_camera.viewProj;
			if (a_jitter) {
				if (auto* state = RE::BSGraphics::State::GetSingleton()) {
					proj[8] += state->projectionPosScaleX;
					proj[9] += state->projectionPosScaleY;
				}
				viewProj = {};
				for (std::uint32_t r = 0; r < 4; ++r)
					for (std::uint32_t c = 0; c < 4; ++c)
						for (std::uint32_t k = 0; k < 4; ++k)
							viewProj[r * 4 + c] += a_camera.view[r * 4 + k] * proj[k * 4 + c];
			}
			transposed(0, a_camera.view);
			transposed(4, proj);
			transposed(8, viewProj);
			transposed(12, a_camera.viewProj);  // CameraViewProjUnjittered
			if (a_previous)
				transposed(16, a_previous->viewProjUnjittered);
			else
				std::memset(out + 16 * 4, 0, 4 * 4 * sizeof(float));
			const auto inverseProj = Inverse(proj);
			transposed(20, inverseProj);
			transposed(24, proj);
			transposed(28, Inverse(a_camera.view));
			transposed(32, Inverse(viewProj));
			transposed(36, inverseProj);
			out[160] = a_camera.eye[0];
			out[161] = a_camera.eye[1];
			out[162] = a_camera.eye[2];
			if (a_previous) {
				out[164] = a_previous->eye.x;
				out[165] = a_previous->eye.y;
				out[166] = a_previous->eye.z;
				return;
			}
			std::memset(out + 41 * 4, 0, 4 * sizeof(float));
			if (auto* state = RE::BSGraphics::State::GetSingleton()) {
				const auto& data = state->GetRuntimeData();
				static auto* clampSetting = RE::GetINISetting("fDRClampOffset:Display");
				const float clamp = clampSetting ? clampSetting->data.f : 0.0f;
				const float w = data.dynamicResolutionWidthRatio, h = data.dynamicResolutionHeightRatio;
				const float pw = data.dynamicResolutionPreviousWidthRatio, ph = data.dynamicResolutionPreviousHeightRatio;
				const float c43[4] = { w, h, pw, ph };
				const float c44[4] = { 1.0f / w, 1.0f / h, w - clamp, pw - clamp };
				std::memcpy(out + 43 * 4, c43, sizeof(c43));
				std::memcpy(out + 44 * 4, c44, sizeof(c44));
			}
		}

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
				org::DeclaredViewToken inputs, geometries, sequences, count, visibility, poolFirsts;
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
			 * @brief A view's cull: BuildDrawsCS in its single phase over the frame's scene list (the ring entry the latch names), frustum
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
					bindings.inputs = a_builder.ShaderResource(*main.inputsDepth).View();
					bindings.geometries = a_builder.ShaderResource(*main.scene->geometries).View();
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
					a_out.push_back(resources->main->scene->pool->layout);
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
					constants.inputsIndex = CaptureViewIndex(a_preparation, a_bindings.inputs);
					constants.geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
					constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
					constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
					constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
					constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
					SetRows(constants, frame->materialRows, frame->pipelineRows);
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
					commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
					for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
						auto constants = a_prepared.constants;
						constants.bucketCountsIndex = a_prepared.bucketCountsIndex[f];
						RecordLatchedDispatch(constants, *a_prepared.latch, a_prepared.signature, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), a_recording);
					}
				}

			private:
				std::shared_ptr<ViewResources> resources;
			};

			struct TreeBindings
			{
				org::DeclaredViewToken shapes, instances;
				std::array<org::DeclaredViewToken, kMaxViewFaces> rows, visible;
			};

			struct TreePrepared
			{
				std::shared_ptr<const ComputeProgram> program;
				std::shared_ptr<const org::LatchBlock> latch;
				TreeLodCullConstants constants{};
				std::uint32_t faces = 0;
				std::array<std::uint32_t, kMaxViewFaces> rowIndex{}, visibleIndex{};
				std::uint32_t groups = 0;
			};

			/**
			 * @brief A view's tree LOD: TreeLodCullCS's phase 1 per face, against the face's latch entry (cull mode 1: no occlusion), into
			 * the face's list; a face's row names no shape slot when it is not drawn.
			 */
			class TreeCullPass final : public org::TypedRenderGraphPass<TreeCullPass, TreePrepared, TreeBindings>
			{
			public:
				explicit TreeCullPass(std::shared_ptr<ViewResources> a_resources) :
					resources(std::move(a_resources)) {}

				TreeBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const auto& scene = *resources->main->scene;
					TreeBindings bindings{};
					bindings.shapes = a_builder.ShaderResource(*scene.treeLodShapes).View();
					bindings.instances = a_builder.ShaderResource(*scene.treeLodInstances).View();
					for (std::uint32_t f = 0; f < resources->traits->faces; ++f) {
						bindings.rows[f] = a_builder.ShaderResource(resources->treeRows[f]).View();
						bindings.visible[f] = a_builder.UnorderedAccess(*resources->treeVisible[f]).View();
					}
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					a_out.push_back(frame ? frame->generation : 0);
					a_out.push_back(resources->main->scene->layout.load(std::memory_order_acquire));
				}

				TreePrepared Prepare(const TreeBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
				{
					TreePrepared prepared{};
					const auto& scene = *resources->main->scene;
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					if (!frame || !frame->latch || !frame->treeGroups || !scene.treeLodCull)
						return prepared;
					prepared.program = scene.treeLodCull;
					prepared.latch = frame->latch;
					prepared.groups = frame->treeGroups;
					prepared.faces = resources->traits->faces;
					auto& constants = prepared.constants;
					constants.shapesIndex = CaptureViewIndex(a_preparation, a_bindings.shapes);
					constants.instancesIndex = CaptureViewIndex(a_preparation, a_bindings.instances);
					constants.latchIndex = frame->latch->SrvIndex();
					constants.phase = 1;
					constants.retestOffset = static_cast<std::uint32_t>(sizeof(TreeLod::VisibleHeader));  // never written: no occlusion test
					for (std::uint32_t f = 0; f < prepared.faces; ++f) {
						prepared.rowIndex[f] = CaptureViewIndex(a_preparation, a_bindings.rows[f]);
						prepared.visibleIndex[f] = CaptureViewIndex(a_preparation, a_bindings.visible[f]);
					}
					return prepared;
				}

				static void Record(const TreeBindings&, const TreePrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.program || !a_prepared.groups || !a_prepared.latch)
						return;
					auto& commands = a_recording.Commands();
					commands.BindLayout(a_prepared.program->layout->GetHandle());
					commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
					const auto region = static_cast<std::uint32_t>(a_prepared.latch->Offset(a_recording.FrameSlot()));
					for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
						auto constants = a_prepared.constants;
						constants.latchOffset = region + f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
						constants.drawIndex = a_prepared.rowIndex[f];
						constants.visibleIndex = a_prepared.visibleIndex[f];
						commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeLodCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
						commands.Dispatch(a_prepared.groups, 1, 1);
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
				std::array<org::ResourceBindingToken, kMaxViewFaces> bucketCounts, treeVisible;
			};

			struct DrawPrepared
			{
				std::shared_ptr<const ViewFrame> frame;
				DrawMode mode = DrawMode::Faces;
				std::uint32_t faces = 0;
				std::array<org::PreparedDescriptorReference, kMaxViewFaces> colours{};
				org::PreparedDescriptorReference depth{};
				std::array<std::uint64_t, kMaxViewFaces> treeRows{};
			};

			/**
			 * @brief A view's draws, per face a pass and per bucket a plain indirect draw of the bucket's range of the face's sequences
			 * (DCLF_PULLED, as the Z-prepass's):
			 * - Faces: on the face's slice of the engine's cube target (loaded and stored: the engine's sky is there) with DCLF's depth
			 *   cleared, each bucket's forward pipeline, then the face's tree LOD.
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
					a_builder.ShaderResource(*main.materialRows.buffer, noViews);
					a_builder.ShaderResource(main.frameConstants, noViews);  // the colour segment's frame slots, by address
					a_builder.ShaderResource(*main.pipelineRows.buffer, noViews);
					a_builder.ShaderResource(*scene.objects, noViews);
					a_builder.ShaderResource(*scene.extras, noViews);
					a_builder.ShaderResource(*scene.facePositions, noViews);
					a_builder.ShaderResource(resources->blocks, noViews);
					if (mode == DrawMode::Colour)
						DeclareFrameBuffers(a_builder, main);
					if (mode == DrawMode::Faces && scene.treeLodCull && resources->treeVisible[0]) {
						for (const auto& table : { scene.treeLodShapes, scene.treeLodInstances, scene.treeLodMeshes })
							a_builder.ShaderResource(*table, noViews);
						for (std::uint32_t f = 0; f < faces; ++f) {
							a_builder.ShaderResource(resources->treeRows[f], noViews);
							bindings.treeVisible[f] = a_builder.IndirectArguments(*resources->treeVisible[f]).Resource();
							a_builder.ShaderResource(*resources->treeVisible[f], noViews);
						}
					}
					return bindings;
				}

				void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
				{
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					a_out.push_back(frame ? frame->generation : 0);
					a_out.push_back(resources->main->scene->pool->layout);
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
					for (std::uint32_t f = 0; f < prepared.faces; ++f)
						prepared.treeRows[f] = resources->treeRowsAddress[f];
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
						// Tree LOD: one instanced draw of the face's list, its row the face's (phase 1's arguments, its instances from the list's start).
						if (mode == DrawMode::Faces && frame.tree.valid() && frame.treeGroups) {
							std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_prepared.treeRows[f]), static_cast<std::uint32_t>(a_prepared.treeRows[f] >> 32), 0u };
							commands.BindPipeline(frame.tree);
							commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
							commands.ExecuteIndirect(frame.treeSignature, a_recording.Resolve(a_bindings.treeVisible[f]).GetHandle(), offsetof(TreeLod::VisibleHeader, phaseOne),
								{}, 0, 1);
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
			 * @brief A view's forward range's sorted half drawn through the main colour signature (the main set's forward pipelines, a
			 * pipeline per sequence: blended Lighting, the Effect port's meshes and particles), after the view's bucket draws, with the view's
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
					a_builder.ShaderResource(*main.materialRows.buffer, noViews);
					a_builder.ShaderResource(*main.pipelineRows.buffer, noViews);
					a_builder.ShaderResource(*scene.objects, noViews);
					a_builder.ShaderResource(*scene.extras, noViews);
					a_builder.VertexBuffer(*scene.facePositions);
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
					const std::uint64_t arguments = std::uint64_t(frame.forwardFirst + frame.forwardCapacity) * sizeof(DrawSequence);
					const std::uint64_t countOffset = std::uint64_t(frame.forwardEntry) * sizeof(std::uint32_t);
					RecordPreprocessedDraw(a_recording, *a_prepared.preprocess, begin, frame.resourceHeap, frame.samplerHeap, frame.indirect.layout, frame.push[0],
						frame.indirect.signatures[kColorVariant], sequences, arguments, counts, countOffset, frame.forwardCapacity);
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
					Register(a_graph, org::ResourceIdentifier("cs.dclf.material-rows"), main.materialRows.buffer);
					Register(a_graph, org::ResourceIdentifier("cs.dclf.pipeline-rows"), main.pipelineRows.buffer);
					Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-inputs-depth"), main.inputsDepth);
					Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility"), main.visibility);
					RegisterSceneBuffers(a_graph, *main.scene);
					// Its own.
					const auto& traits = *resources->traits;
					const auto id = [&](std::string_view a_name) { return org::ResourceIdentifier(fmt::format("{}.{}", traits.prefix, a_name)); };
					Register(a_graph, id("draw-count"), resources->count);
					Register(a_graph, id(traits.blocks), resources->blocks);
					if (resources->depth)
						Register(a_graph, id("depth"), resources->depth);
					Register(a_graph, id(traits.target), resources->target);
					for (std::uint32_t f = 0; f < traits.faces && traits.trees; ++f)
						Register(a_graph, id(fmt::format("tree-row{}", f)), resources->treeRows[f]);
				}

				void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
				{
					const auto& traits = *resources->traits;
					const auto& main = resources->main;
					if (!main || !main->scene || !main->scene->pool || !resources->target || (traits.depthPass && (!main->nativeDepth || !main->native[0] || !main->native[1])))
						return;
					const auto depthEpoch = RenderGraphRuntime::EpochOf(traits.segments[0]);
					const auto colourEpoch = RenderGraphRuntime::EpochOf(traits.segments[1]);
					const auto name = [&](const char* a_pass) { return fmt::format("{}.{}", traits.prefix, a_pass); };
					AddPass(a_out, PassKind::Copy, name("latched-copies"), MakeLatchedCopiesPass(LatchedTargets(resources), [kind = traits.kind](const org::PassPrepareContext& a_preparation) {
						const auto frame = CurrentViewFrame(a_preparation, kind);
						return frame ? LatchedSource{ frame->generation, frame, frame->latched } : LatchedSource{};
					}),
						depthEpoch);
					AddPass(a_out, PassKind::Compute, name("build-draws"), std::make_shared<BuildDrawsPass>(resources), depthEpoch);
					if (traits.trees && main->scene->treeLodCull && resources->treeVisible[0])
						AddPass(a_out, PassKind::Compute, name("tree-lod-cull"), std::make_shared<TreeCullPass>(resources), depthEpoch);
					if (!traits.depthPass) {
						AddPass(a_out, PassKind::Render, name("faces"), std::make_shared<DrawPass>(resources, DrawMode::Faces), depthEpoch);
						return;
					}
					AddPass(a_out, PassKind::Render, name("depth"), std::make_shared<DrawPass>(resources, DrawMode::Depth), depthEpoch);
					// The forward range sorted with the depth epoch (after its BuildDraws), drawn in the colour epoch after the bucket draws.
					if (traits.forwardPart && main->forwardSort)
						AddPass(a_out, PassKind::Compute, name("forward-sort"),
							MakeForwardSortPass(resources->sequences, { nullptr, resources->bucketCounts[0] }, [view = resources](const org::PassPrepareContext& a_preparation) {
								const auto frame = CurrentViewFrame(a_preparation, view->traits->kind);
								if (!frame || !frame->latch)
									return ForwardSortSource{ frame ? frame->generation : 0 };
								// Its count is the range's bucket count word, after the buckets'.
								return ForwardSortSource{ frame->generation, view->main->forwardSort, frame->forwardFirst, frame->forwardCapacity,
									frame->forwardEntry * static_cast<std::uint32_t>(sizeof(std::uint32_t)), true };
							}),
							depthEpoch);
					AddPass(a_out, PassKind::Render, name("colour"), std::make_shared<DrawPass>(resources, DrawMode::Colour), colourEpoch);
					if (traits.forwardPart && main->forwardSort && resources->preprocessForward)
						AddPass(a_out, PassKind::Render, name("forward"), std::make_shared<ForwardPass>(resources), colourEpoch);
				}

			private:
				/** @brief The targets the view's latched copies write: the draw count, the blocks, each face's bucket counts and tree LOD row and list. */
				static std::function<std::vector<LatchedTarget>()> LatchedTargets(std::shared_ptr<ViewResources> a_resources)
				{
					return [resources = std::move(a_resources)] {
						std::vector<LatchedTarget> out{ { resources->count, nullptr }, { resources->blocks, nullptr } };
						const bool trees = resources->main->scene->treeLodCull && resources->treeVisible[0];
						for (std::uint32_t f = 0; f < resources->traits->faces; ++f) {
							out.push_back({ nullptr, resources->bucketCounts[f] });
							if (trees) {
								out.push_back({ resources->treeRows[f], nullptr });
								out.push_back({ nullptr, resources->treeVisible[f] });
							}
						}
						return out;
					};
				}

				std::shared_ptr<ViewResources> resources;
			};
		}

		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeViewExtension(std::shared_ptr<ViewResources> a_resources)
		{
			return std::make_unique<ViewExtension>(std::move(a_resources));
		}
	}

	std::uint32_t IndirectDraws::Impl::CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const TreeLod::DrawRow* a_treeRow)
	{
		const auto& traits = TraitsOf(a_shape.kind);
		const auto& labels = traits.labels;
		const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		const auto& latchBlock = *a_shape.latch;
		const auto& layout = a_shape.latchLayout;
		const auto& map = *a_shape.map;
		const auto region = static_cast<std::uint32_t>(latchBlock.Offset(latchSlot));
		// Every pipeline slot the installed publication's members can draw with, as many as the latch holds: a slot the tables have
		// gained since the map was made has no bucket (none of its draws is the frame's).
		const std::uint32_t slots = std::min(std::max(static_cast<std::uint32_t>(map.size()), InstalledPipelineSlots()), layout.slots);
		WriteBucketMap(latchBlock, labels.map, latchSlot, layout.MapOffset(), map, slots);
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		const auto* view = View(a_shape.kind).resources.get();
		std::vector<std::uint32_t> table;
		std::uint32_t drawn = 0;
		for (std::uint32_t f = 0; f < a_faces.size() && f < traits.faces; ++f) {
			const auto& face = a_faces[f];
			// The face's bucket table, its forward range's entry after the buckets' (ViewFrame::forwardEntry).
			table.clear();
			for (const auto& bucket : a_shape.buckets) {
				table.push_back(f * a_shape.sequenceDraws + bucket.first);
				table.push_back(bucket.capacity);
			}
			if (a_shape.forwardCapacity) {
				table.push_back(f * a_shape.sequenceDraws + a_shape.forwardFirst);
				table.push_back(a_shape.forwardCapacity);
			}
			if (!table.empty())
				LatchWrite(latchBlock, labels.tables, latchSlot, layout.TableOffset(f), std::as_bytes(std::span(table)));
			const std::uint32_t inputs = face.drawn ? a_inputs : 0u;
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = inputs;
			latch.cullFlags = 1;  // the frustum alone, near plane included
			latch.viewBits = traits.viewBits;  // the main list's members of the view
			latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			latch.fadeSeedsIndex = FrameValues::Get().FadeSeedsIndex();
			// The frame's scene list: its ring entry.
			RingLatch(ringFrame, latch);
			FoldEyeIntoViewProj(face.viewProj, face.eye, latch.viewProj);
			latch.bucketMapOffset = region + layout.MapOffset();
			latch.bucketTableOffset = region + layout.TableOffset(f);
			LatchWriteValue(latchBlock, labels.latches, latchSlot, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
			if (!face.block.empty())
				LatchWrite(latchBlock, labels.blocks, latchSlot, layout.FaceOffset(f), face.block);
			if (a_treeRow && a_shape.tree.valid() && view) {
				// The face's row (its own list), naming no slot when the face is not drawn or the faces' tree LOD is the engine's.
				TreeLod::DrawRow row = *a_treeRow;
				row.visible = view->treeVisibleAddress[f] + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
				row.shapeSlots = face.drawn ? a_treeRow->shapeSlots : 0u;
				LatchWriteValue(latchBlock, labels.trees, latchSlot, layout.FaceOffset(f) + ViewLatchLayout::kTreeRowInFace, row);
				TreeLod::VisibleHeader header{};
				header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = std::max(row.maxIndices, 1u);
				LatchWriteValue(latchBlock, labels.trees, latchSlot, layout.FaceOffset(f) + ViewLatchLayout::kTreeHeaderInFace, header);
			}
			drawn += face.drawn ? 1u : 0u;
		}
		return drawn;
	}

	std::shared_ptr<std::vector<std::shared_ptr<const void>>> IndirectDraws::Impl::EpochOwners(std::span<const MainPayload* const> a_payloads)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		const auto cleanup = host ? host->ResourceCleanup() : nullptr;
		if (!cleanup)
			return nullptr;
		auto owners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		for (const auto* payload : a_payloads)
			if (payload)
				owners->insert(owners->end(), payload->bindingOwners.begin(), payload->bindingOwners.end());
		// The colour segment's frame record names the frame textures it resolved, held by their bindings since (frameTextureBindings).
		for (const auto& held : frameTextureBindings)
			if (held.binding.owner)
				owners->push_back(held.binding.owner);
		if (ringFrame.draws)
			owners->push_back(ringFrame.draws);
		if (adoptedSnapshot)
			owners->push_back(adoptedSnapshot);
		return owners;
	}
}
#endif
