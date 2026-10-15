// The views' epochs (ViewEpoch.h): their traits, forward keys, bucket plans and shapes, their cameras' VS_PerFrame and their
// commits (render thread). Their passes and graph extension are ViewEpochRecord.cpp's, their captures and coverage
// Reflection.cpp's and FirstPerson.cpp's.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

namespace DCLF
{

	namespace Draws
	{
		const ViewTraits& TraitsOf(ViewKind a_kind)
		{
			// The epochs and segments are the view's slot's (kViewSlots).
			const auto& reflection = kViewSlots[ViewIndex(ViewKind::Reflection)];
			const auto& firstPerson = kViewSlots[ViewIndex(ViewKind::FirstPerson)];
			static const ViewTraits kTraits[kViews] = {
				{ ViewKind::Reflection, kReflectionFaces, true, false, false, kViewReflection, reflection.epochs, { reflection.segments[0], reflection.segments[1] },
					"reflection", "cs.dclf.reflection", "face-blocks", "cube",
					{ "reflection bucket map", "reflection bucket tables", "reflection culling latches", "reflection face per-frame data", "reflection tree rows" } },
				{ ViewKind::FirstPerson, 1, false, true, true, kViewFirstPerson, firstPerson.epochs, { firstPerson.segments[0], firstPerson.segments[1] },
					"first-person", "cs.dclf.first-person", "block", "depth-copy",
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

		void PlanViewBuckets(const ViewTraits& a_traits, const MainLayout& a_main, const Lookups* a_lookups, const IndirectState& a_indirect,
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
			frame->sequenceDraws = a_in.sequenceDraws;
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
			// The frame's scene list: the installed payload (a covered frame has a ready one).
			PayloadLatch(*InstalledPayload(), latch);
			FoldEyeIntoViewProj(face.viewProj, face.eye, latch.viewProj);
			latch.bucketMapOffset = region + layout.MapOffset();
			latch.bucketTableOffset = region + layout.TableOffset(f);
			LatchWriteValue(latchBlock, labels.latches, latchSlot, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
			if (!face.block.empty())
				LatchWrite(latchBlock, labels.blocks, latchSlot, layout.FaceOffset(f), face.block);
			if (a_treeRow && a_shape.tree.valid() && view) {
				// The face's row (its own list), naming no slot when the face is not drawn or the faces' tree LOD is the engine's.
				TreeLod::DrawRow row = *a_treeRow;
				row.visible = Adopted().Of(view->treeVisible[f]).deviceAddress + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
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
		if (adoptedSnapshot)
			owners->push_back(adoptedSnapshot);
		return owners;
	}
}
