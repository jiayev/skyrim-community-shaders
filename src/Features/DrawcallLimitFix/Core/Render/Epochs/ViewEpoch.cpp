// The views' epochs (ViewEpoch.h): their traits, forward keys, bucket plans and shapes, their cameras' VS_PerFrame and their
// commits (render thread). Their passes and graph extension are ViewEpochRecord.cpp's, their captures and coverage the view
// features' (Features/Reflection, Features/FirstPerson), on the view engine's calls (ViewEngine.cpp).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"

#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

namespace DCLF
{

	namespace Draws
	{
		const ViewTraits& TraitsOf(ViewKind a_kind)
		{
			// The view's resolved shape (Extensions::Views: the feature's registration; none for a view no feature took) with its epochs
			// and segments (the schedule's: ViewEpochsOf); made once, the first time a view's traits are read (after InstallFeatures).
			static const auto kTraits = [] {
				std::array<ViewTraits, kViews> traits{};
				for (const auto& slot : kViewSlots) {
					const auto& entry = Extensions::ViewEntryOf(slot.view);
					const auto& shape = entry.shape;
					const auto& epochs = ViewEpochsOf(slot.view);
					traits[ViewIndex(slot.view)] = { slot.view, shape.faces, shape.trees, shape.forwardPart, shape.depthPass, shape.viewBits, epochs.epochs,
						{ epochs.segments[0], epochs.segments[1] }, entry.name.c_str(), entry.prefix.c_str(), entry.blocks.c_str(), entry.target.c_str(),
						{ entry.labels[0].c_str(), entry.labels[1].c_str(), entry.labels[2].c_str(), entry.labels[3].c_str(), entry.labels[4].c_str() }, shape.frustumCull };
				}
				return traits;
			}();
			return kTraits[ViewIndex(a_kind)];
		}

		bool ViewForwardKey(ViewKind a_kind, const SceneTables& a_tables, std::size_t a_slot, ForwardPipelineKey& a_key)
		{
			if (a_slot >= a_tables.pipelines.size() || !a_tables.PipelineUsed(a_slot))
				return false;
			const auto& pipelineKey = a_tables.pipelines[a_slot];
			const std::uint32_t twoSided = (pipelineKey.rasterFlags & kRasterTwoSided) ? ForwardPipelineKey::kForwardTwoSided : 0u;
			// The view's key rule (Api::ViewKeyRule). The faces': a LOD slot's. First person's: a forward key's slot draws with the main
			// set's colour pipeline in the forward range (ViewForwardSlotsOf), the rest with the first-person targets' key. The vertex
			// descriptor is the pulled vertex stage (skins read their palette as the Z-prepass's do); the pixel descriptor drops Deferred
			// (the engine draws first person forward, after the composite: Community Shaders' Deferred sets InWorld around it).
			const auto& keys = Extensions::ViewEntryOf(a_kind).keys;
			if (keys.lodTechniquesOnly && !LodLightingTechnique(pipelineKey.passDescriptor))
				return false;
			if (keys.forwardRasterToForwardPart && RasterForward(pipelineKey.rasterFlags))
				return false;
			a_key = ForwardPipelineKey{ pipelineKey.vertexDescriptor, pipelineKey.pixelDescriptor & ~kLightingPixelDeferred, keys.keyFlags | twoSided };
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

		namespace
		{
			/**
			 * @brief A view's forward range's state classes into its latch slot (CommitView): BuildDrawsCS takes a forward draw's class (its
			 * table entry, and the index its sequence stores) from its set index through this map, the shape's set version's. False (no map:
			 * every forward draw dropped, counted) without a forward range, or past what the latch holds (never: the revision reserved it).
			 */
			bool WriteViewClassMap(const ViewFrame& a_shape, std::string_view a_what, std::uint32_t a_latchSlot)
			{
				const auto& classOf = a_shape.indirect.classOf;
				const auto& layout = a_shape.latchLayout;
				if (a_shape.forwardRanges.empty() || classOf.size() > layout.classMapWords)
					return false;
				const std::uint32_t header[kClassHeaderWords] = { static_cast<std::uint32_t>(classOf.size()), 0, 0, 0 };
				LatchWrite(*a_shape.latch, a_what, a_latchSlot, layout.ClassMapOffset(), std::as_bytes(std::span(header)));
				if (!classOf.empty())
					LatchWrite(*a_shape.latch, a_what, a_latchSlot, layout.ClassMapOffset() + kClassHeaderWords * 4, std::as_bytes(std::span(classOf)));
				return true;
			}
		}

		std::uint32_t SetClassOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot)
		{
			// As ZGroupOf: the slot's lookup entry's set index's word (IndirectState::classOf).
			if (!a_lookups || a_slot >= a_lookups->pipelines.size())
				return kNoSetClass;
			const auto setIndex = a_lookups->pipelines[a_slot].setIndex;
			return setIndex != Lookups::kNone && setIndex < a_indirect.classOf.size() ? a_indirect.classOf[setIndex] : kNoSetClass;
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
			// The forward range, per state class (its draws are device-generated: one call per class, each with its class's execution set):
			// every forward slot's map word names the first class's table entry (after the buckets) with kFirstPersonForwardBucket, and
			// BuildDrawsCS adds the draw's class (from its set index, through the latch's class map). Each class's entry holds every draw
			// its forward slots can produce (their capacities in the main layout), staged then sorted: two halves after the buckets' ranges.
			a_out.forwardRanges.clear();
			std::vector<std::uint32_t> capacity(a_indirect.classes.size(), 0u);
			std::uint64_t forward = 0;
			for (std::uint32_t p = 0; p < slots && p < a_forwardSlots.size(); ++p) {
				if (!a_forwardSlots[p] || a_out.map[p] != kNoBucket)
					continue;
				const std::uint32_t setClass = SetClassOf(a_lookups, a_indirect, p);
				if (setClass == kNoSetClass || (setClass >> kSetClassShift) >= capacity.size())
					continue;  // no pipeline of the set version: none of its draws is the view's
				a_out.map[p] = kFirstPersonForwardBucket | static_cast<std::uint32_t>(a_out.buckets.size());
				const std::uint32_t slotCapacity = p < a_main.slotCapacity.size() ? a_main.slotCapacity[p] : 0u;
				capacity[setClass >> kSetClassShift] += slotCapacity;
				forward += slotCapacity;
			}
			if (forward) {
				a_out.forwardRanges.resize(capacity.size());
				for (std::size_t c = 0; c < capacity.size(); ++c) {
					a_out.forwardRanges[c] = { a_out.draws, capacity[c] };
					a_out.draws += 2 * capacity[c];
				}
			} else {
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
			frame->forwardRanges = a_in.plan.forwardRanges;
			frame->forwardEntry = static_cast<std::uint32_t>(a_in.plan.buckets.size());
			frame->latchLayout = latches.layout;
			frame->map = std::make_shared<const std::vector<std::uint32_t>>(a_in.plan.map);
			// Each draw family's faces: its cull a thread per word its lists' units hold, cullGroup a group.
			for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
				const auto& in = a_in.faces[d];
				if (!in.pipeline.valid())
					continue;
				const auto& faces = Extensions::DrawFamilies()[d].contribution.faces;
				frame->faces[d].pipeline = in.pipeline;
				frame->faces[d].signature = in.signature;
				frame->faces[d].groups = static_cast<std::uint32_t>((std::uint64_t(in.units) * faces.unitWords + faces.cullGroup - 1) / faces.cullGroup);
			}
			// Per face, the view's frame constants (its own or the colour segment's: the frame lighting at PS b13, the frame record) with VS and
			// PS b12 its block.
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
			// The latched copies: each face's block (and each draw family's row and list header) from the latch, the counters zeroed.
			auto& fromLatch = frame->latched[0];
			auto& fromZeros = frame->latched[1];
			fromLatch.latch = latches.latch;
			fromZeros.latch = latches.zeros;
			fromZeros.fixed = true;
			fromZeros.copies.push_back({ a_resources.count.get(), 0, static_cast<std::uint32_t>(sizeof(kZeroCounts)), 0 });
			const auto bucketBytes = static_cast<std::uint32_t>(std::max<std::size_t>(frame->buckets.size() + frame->forwardRanges.size(), 1) * sizeof(std::uint32_t));
			for (std::uint32_t f = 0; f < traits.faces; ++f) {
				const std::uint32_t face = latches.layout.FaceOffset(f);
				fromZeros.copies.push_back({ a_resources.bucketCounts[f]->Key(), 0, bucketBytes, 0 });
				fromLatch.copies.push_back({ a_resources.blocks.get(), face, kViewBlockBytes, std::uint64_t(f) * kViewBlockBytes });
				for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
					if (frame->faces[d].pipeline.valid() && a_resources.faceRows[d][f] && a_resources.faceLists[d][f]) {
						const auto& entry = Extensions::DrawFamilies()[d];
						fromLatch.copies.push_back({ a_resources.faceRows[d][f].get(), face + entry.faceLayout.rowInFace, entry.contribution.faces.rowBytes, 0 });
						fromLatch.copies.push_back({ a_resources.faceLists[d][f]->Key(), face + entry.faceLayout.headerInFace, entry.contribution.faces.headerBytes, 0 });
					}
			}
			// A view's own frame values (its commit's, from this frame's sources) from the latch.
			if (!a_in.frameCopies.empty()) {
				auto& fromFrame = frame->latched[ViewFrame::kFrameList];
				fromFrame.latch = latches.latch;
				fromFrame.copies = a_in.frameCopies;
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
			if (frame.size() < ShadowCameras::kBlockBytes) {
				const auto& cached = globals::game::frameBufferCached.data;
				static_assert(sizeof(cached) >= ShadowCameras::kBlockBytes);
				frame = std::as_bytes(std::span(&cached, 1));
			}
			return frame;
		}

		void PerFrameBlock(const ShadowCameras::Camera& a_camera, std::span<const std::byte> a_frame, bool a_jitter, const PreviousCamera* a_previous, std::byte* a_out)
		{
			std::memcpy(a_out, a_frame.data(), ShadowCameras::kBlockBytes);
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

	std::uint32_t IndirectDraws::Impl::CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const ViewFamilyRows* a_rows)
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
		const bool classMap = WriteViewClassMap(a_shape, labels.map, latchSlot);
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto& state = View(a_shape.kind);
		const auto* view = state.resources.get();
		std::vector<std::uint32_t> table;
		std::uint32_t drawn = 0;
		for (std::uint32_t f = 0; f < a_faces.size() && f < traits.faces; ++f) {
			const auto& face = a_faces[f];
			// The face's bucket table, its forward range's entries (one per state class) after the buckets' (ViewFrame::forwardEntry).
			table.clear();
			for (const auto& bucket : a_shape.buckets) {
				table.push_back(f * a_shape.sequenceDraws + bucket.first);
				table.push_back(bucket.capacity);
			}
			for (const auto& range : a_shape.forwardRanges) {
				table.push_back(f * a_shape.sequenceDraws + range.first);
				table.push_back(range.capacity);
			}
			if (!table.empty())
				LatchWrite(latchBlock, labels.tables, latchSlot, layout.TableOffset(f), std::as_bytes(std::span(table)));
			const std::uint32_t inputs = face.drawn ? a_inputs : 0u;
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = inputs;
			latch.cullFlags = traits.frustumCull ? 1u : 0u;  // the frustum alone, near plane included; 0: untested (BuildDrawsCS CullMode)
			latch.viewBits = traits.viewBits;  // the main list's members of the view
			latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			// The root states' seeds (the cull binding RootSeeds: a pooled buffer's index).
			latch.fadeSeedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, *scene, frameNumber, Adopted());
			// The frame's scene list: the installed payload (a covered frame has a ready one).
			PayloadLatch(*InstalledPayload(), latch);
			FoldEyeIntoViewProj(face.viewProj, face.eye, latch.viewProj);
			latch.bucketMapOffset = region + layout.MapOffset();
			latch.bucketTableOffset = region + layout.TableOffset(f);
			latch.classMapOffset = classMap ? region + layout.ClassMapOffset() : 0u;
			LatchWriteValue(latchBlock, labels.latches, latchSlot, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
			if (!face.block.empty())
				LatchWrite(latchBlock, labels.blocks, latchSlot, layout.FaceOffset(f), face.block);
			for (std::size_t d = 0; a_rows && view && d < Extensions::kDrawFamilies; ++d) {
				if ((*a_rows)[d].empty() || !a_shape.faces[d].pipeline.valid())
					continue;
				// The family's face (its faces.face: the face's row, its own list, naming nothing when the face is not drawn or the faces'
				// family is the engine's, and its list's header).
				const auto& entry = Extensions::DrawFamilies()[d];
				auto& written = state.faces[d];
				written.faceRow.resize(entry.contribution.faces.rowBytes);
				written.faceHeader.resize(entry.contribution.faces.headerBytes);
				Api::DrawFamilyFaceWrite write{ (*a_rows)[d], Adopted().Of(view->faceLists[d][f]).deviceAddress, face.drawn, written.faceRow, written.faceHeader };
				entry.contribution.faces.face(write);
				LatchWrite(latchBlock, labels.trees, latchSlot, layout.FaceOffset(f) + entry.faceLayout.rowInFace, write.row);
				LatchWrite(latchBlock, labels.trees, latchSlot, layout.FaceOffset(f) + entry.faceLayout.headerInFace, write.header);
			}
			drawn += face.drawn ? 1u : 0u;
		}
		if (auto* own = state.resources.get(); own && own->frameConstants) {
			// The view's own frame constants, from this frame's sources (a snapshot component of the frame, as the shadow views' record):
			// the record (its frame textures as SubmitViewEpoch resolved them, the installed payload, FrameValues' buffers, the frame
			// bindings), CS's SharedData and FeatureData (PS b5, b6), the shared light block and the frame-wide rows. The faces' programs
			// read no other frame slot (VS and PS b12 are each face's block).
			const auto& adopted = Adopted();
			CommitUploads uploads(commitStagedPool, adopted);
			LatchedUploads frame(a_shape.latched[ViewFrame::kFrameList], uploads, latchSlot);
			// Its copies of the main frame buffers (Deferred's sun cascades at t98, written at EarlyPrepasses and again before the cube's faces),
			// each from the buffer bound at its register now and that buffer's CPU mirror: this frame's contents, named at the register.
			// Unfillable (nothing bound there, or the mirror holds less): the copy reads zero and the register null.
			if (const auto frameBuffers = own->frameBuffers.load(std::memory_order_acquire)) {
				auto& mirror = ConstantMirror::Get();
				for (const auto& frameBuffer : *frameBuffers) {
					std::span<const std::byte> contents;
					const auto buffer = BoundPixelBuffer(frameBuffer.textureRegister);
					if (buffer) {
						mirror.Watch(buffer.get());
						contents = mirror.Contents(buffer.get());
					}
					if (LatchFrameBuffer(frame, frameBuffer, contents))
						state.frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
				}
			}
			const auto shared = SharedDataBlock(), feature = FeatureDataBlock();
			const std::uint32_t vsMask = (1u << kPerFrameVertexRegister) | (1u << kFrameFogRegister);
			const std::uint32_t psMask = (1u << kPerFrameVertexRegister) | (shared.empty() ? 0u : 1u << kSharedDataRegister) | (feature.empty() ? 0u : 1u << kFeatureDataRegister);
			auto& textures = GpuTextures::Get();
			const std::uint32_t nullIndex = textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u;
			const DrawBindings record = FrameRecordOf(own->frameConstantsAddress, vsMask, psMask, state.frameTextures, nullIndex, *InstalledPayload(),
				{ FrameValues::Get().PlacementsIndex(), FrameValues::Get().PalettesIndex(), FrameValues::Get().ShadingIndex() }, adopted, *scene, frameNumber);
			frame(own->frameConstants, &record, sizeof(record), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			frame(own->frameConstants, shared.data(), shared.size(), FrameSlotOffset(true, kSharedDataRegister));
			frame(own->frameConstants, feature.data(), feature.size(), FrameSlotOffset(true, kFeatureDataRegister));
			ZeroSharedLightOnce(uploads, own->frameConstants, own->sharedLightZeroed);
			LatchFrameRows(frame, own->frameConstants);
			(void)frame.Finish();
			revisions.latchedMisses += frame.Misses();
			// The frame texture registers left null (no source of this frame's), for the view's report.
			state.nullTextures = {};
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t)
				if (record.textures[t] == nullIndex)
					state.nullTextures[t / 64] |= 1ull << (t % 64);
		}
		return drawn;
	}

	ExecutionOwners IndirectDraws::Impl::EpochOwners(std::span<const MainPayload* const> a_payloads)
	{
		auto owners = ExecutionOwners::Make();
		if (!owners)
			return owners;
		for (const auto* payload : a_payloads)
			if (payload)
				owners.AddPayload(*payload);
		// The colour segment's frame record names the frame textures it resolved, held by their bindings since (frameTextureBindings): what
		// the shared epochs and a view drawing with the main frame constants (first person's) read. A view with its own holds its commit's
		// (SubmitViewEpoch).
		for (const auto& held : frameTextureBindings)
			owners.Add(held.binding.owner);
		owners.Add(adoptedSnapshot);
		return owners;
	}
}
