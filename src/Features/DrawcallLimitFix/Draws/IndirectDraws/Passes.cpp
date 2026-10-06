#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	// CS_DCLF_FOLIAGE_PARITY: one frame's results (GpuLayouts.h, FoliageCounter), read back; render thread.
	void FoliageParityReport(Resources::FoliageParity& a_foliage, std::uint32_t a_frame, const std::uint32_t* a_results)
	{
		auto sampleAt = [&](std::uint32_t a_base, std::uint32_t a_slot) { return a_results + kFoliageCounters + (a_base + a_slot) * kFoliageSampleWords; };
		auto half2 = [](std::uint32_t a_word) {
			auto half = [](std::uint16_t a_h) {
				const std::uint32_t sign = (a_h >> 15) & 1, exponent = (a_h >> 10) & 0x1F, mantissa = a_h & 0x3FF;
				const float value = exponent == 0 ? std::ldexp(float(mantissa), -24) :
				                    exponent == 31 ? std::numeric_limits<float>::infinity() :
				                                     std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
				return sign ? -value : value;
			};
			return std::pair{ half(static_cast<std::uint16_t>(a_word)), half(static_cast<std::uint16_t>(a_word >> 16)) };
		};
		const std::lock_guard lock(a_foliage.mutex);
		++a_foliage.frames;
		for (std::uint32_t c = 0; c < kFoliageCounters; ++c)
			a_foliage.totals[c] += a_results[c];

		// Within the frame: coverage against the Z-prepass, and motion. A defect at any count past a few pixels.
		constexpr std::uint32_t kFlaggedInFrame = 16, kFlagged = 256;
		const std::uint32_t coverage = a_results[kFoliageUnshaded] + a_results[kFoliageOtherObject], motion = a_results[kFoliageMotion];
		if (coverage >= kFlaggedInFrame || motion >= kFlaggedInFrame) {
			++a_foliage.inFrameFlagged;
			// The first few, then the first of each report's span (the first are often the load's).
			if (a_foliage.inFrameLogged < 10 || a_foliage.inFrameFlagged == 1) {
				++a_foliage.inFrameLogged;
				std::map<std::uint32_t, std::uint32_t> objects;  // the sampled defects by object: one object's draw, or edges everywhere
				std::string samples;
				const std::uint32_t count = std::min(a_results[kFoliageInFrameSampleCount], kFoliageInFrameSamples);
				for (std::uint32_t s = 0; s < count; ++s) {
					const std::uint32_t* sample = sampleAt(kFoliageInFrameSampleBase, s);
					++objects[sample[2]];
					if (s >= 8)
						continue;
					if (sample[1] == kFoliageMotion) {
						const auto [mx, my] = half2(sample[5]);
						samples += fmt::format("; ({},{}) motion: object {} albedo {:08X}, error {:.1f} px of ({:.1f}, {:.1f})", sample[0] & 0xFFFF, sample[0] >> 16, sample[2],
							sample[3], std::bit_cast<float>(sample[4]), mx, my);
					} else {
						// An object word (index + 1): its name, and its chain up to the LOD root (each node's name and app-culled bit).
						auto describe = [&](std::uint32_t a_word) -> std::string {
							const auto& tables = SceneStore::Get().GetTables();
							const std::uint32_t index = a_word - 1;
							const auto* geometry = a_word && index < tables.objectGeometry.size() ? tables.objectGeometry[index] : nullptr;
							if (!geometry)
								return fmt::format("{}", a_word);
							std::string chain = fmt::format("{} '{}'{}", a_word, geometry->name.c_str() ? geometry->name.c_str() : "", geometry->GetFlags().any(RE::NiAVObject::Flag::kHidden) ? " culled" : "");
							std::uint32_t depth = 0;
							for (const RE::NiAVObject* node = geometry->parent; node && depth < 4; node = node->parent, ++depth)
								chain += fmt::format(" < '{}'{}", node->name.c_str() ? node->name.c_str() : "", node->GetFlags().any(RE::NiAVObject::Flag::kHidden) ? " culled" : "");
							return chain;
						};
						samples += fmt::format("; ({},{}) {}: owner {} (depth {:.7f}), colour pass {}", sample[0] & 0xFFFF, sample[0] >> 16,
							sample[1] == kFoliageUnshaded ? "unshaded" : "another object", describe(sample[2]), std::bit_cast<float>(sample[4]), describe(sample[3]));
					}
				}
				std::vector<std::pair<std::uint32_t, std::uint32_t>> byCount(objects.begin(), objects.end());
				std::ranges::sort(byCount, [](const auto& a, const auto& b) { return a.second > b.second; });
				std::string tally = fmt::format("{} objects in {} samples:", byCount.size(), count);
				// The sampled objects by name (their words are the object's index + 1), and whether each is terrain LOD.
				const auto& tables = SceneStore::Get().GetTables();
				for (std::size_t o = 0; o < byCount.size() && o < 8; ++o) {
					const std::uint32_t index = byCount[o].first - 1;
					const auto* geometry = byCount[o].first && index < tables.objectGeometry.size() ? tables.objectGeometry[index] : nullptr;
					const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
					tally += fmt::format(" {}x{} ('{}'{})", byCount[o].first, byCount[o].second, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?",
						property && property->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape) ? ", terrain LOD" : "");
				}
				logger::warn("[DCLF] foliage parity: frame tag {}: of {} owned pixels {} unshaded and {} shaded by another object; {} foliage pixels with a motion vector "
							 "off a static object's ({} by more than 8 px); {}{}",
					a_frame, a_results[kFoliageOwned], a_results[kFoliageUnshaded], a_results[kFoliageOtherObject], motion, a_results[kFoliageMotionFar], tally, samples);
			}
		}

		// Near-white foliage: a frame well above the running average (a jump, not the scene's own white flowers).
		{
			const std::uint32_t white = a_results[kFoliageWhiteAlbedo] + a_results[kFoliageWhiteDiffuse];
			a_foliage.whiteMax = std::max(a_foliage.whiteMax, white);
			if (a_foliage.frames > 16 && white > 4.0 * a_foliage.whiteAverage + 1000.0) {
				++a_foliage.whiteFlagged;
				if (a_foliage.whiteLogged < 20) {
					++a_foliage.whiteLogged;
					std::string samples;
					const std::uint32_t count = std::min(a_results[kFoliageWhiteSampleCount], kFoliageWhiteSamples);
					for (std::uint32_t s = 0; s < count && s < 8; ++s) {
						const std::uint32_t* sample = sampleAt(kFoliageWhiteSampleBase, s);
						samples += fmt::format("; ({},{}) object {} albedo {:08X} diffuse {:08X}", sample[0] & 0xFFFF, sample[0] >> 16, sample[2], sample[3], sample[4]);
					}
					logger::warn("[DCLF] foliage parity: frame tag {}: {} near-white foliage pixels ({} albedo, {} diffuse) against an average of {:.0f}{}", a_frame,
						white, a_results[kFoliageWhiteAlbedo], a_results[kFoliageWhiteDiffuse], a_foliage.whiteAverage, samples);
				}
			}
			a_foliage.whiteAverage = a_foliage.frames <= 16 ? white : 0.95 * a_foliage.whiteAverage + 0.05 * white;
		}

		// Against the frame before, reprojected: a frame whose foliage changes colour or flashes brighter all at once is a jump of
		// these over their running average (the camera's own changes - shadows sweeping, specular angles - are the average).
		{
			const std::uint32_t changed = a_results[kFoliageRecoloured] + a_results[kFoliageBrightened] + a_results[kFoliageWhitened];
			a_foliage.changeMax = std::max(a_foliage.changeMax, changed);
			if (a_foliage.frames > 16 && changed >= kFlagged && changed > 4.0 * a_foliage.changeAverage + 1000.0) {
				++a_foliage.flagged;
				if (a_foliage.logged < 30) {
					++a_foliage.logged;
					std::map<std::uint32_t, std::uint32_t> objects;
					std::string samples;
					const std::uint32_t count = std::min(a_results[kFoliageFrameSampleCount], kFoliageFrameSamples);
					for (std::uint32_t s = 0; s < count; ++s) {
						const std::uint32_t* sample = sampleAt(kFoliageFrameSampleBase, s);
						++objects[sample[2]];
						if (s >= 8)
							continue;
						static const char* kKinds[] = { "", "vanished", "appeared", "recoloured", "whitened" };
						const auto [mx, my] = half2(sample[7]);
						samples += fmt::format("; ({},{}) {} object {}: diffuse {:08X} from {:08X}, specular {:08X} from {:08X}, motion ({:.1f}, {:.1f})", sample[0] & 0xFFFF,
							sample[0] >> 16, sample[1] == kFoliageBrightened ? "brightened" : kKinds[std::min(sample[1], 4u)], sample[2], sample[3], sample[4], sample[5],
							sample[6], mx, my);
					}
					logger::warn("[DCLF] foliage parity: frame tag {}: {} of {} foliage pixels changed from where they were the frame before ({} recoloured, {} brightened, "
								 "{} whitened; {} vanished, {} appeared) against an average of {:.0f}; {} objects sampled{}",
						a_frame, changed, a_results[kFoliageCompared], a_results[kFoliageRecoloured], a_results[kFoliageBrightened], a_results[kFoliageWhitened],
						a_results[kFoliageVanished], a_results[kFoliageAppeared], a_foliage.changeAverage, objects.size(), samples);
				}
			}
			a_foliage.changeAverage = a_foliage.frames <= 16 ? changed : 0.95 * a_foliage.changeAverage + 0.05 * changed;
		}

		// Terrain LOD's coverage (its owners, bit 31): the frames it lost pixels in, and the most in one. Its pixels no draw shaded are
		// sampled apart (the others are equal-depth ties), and a frame with a few dozen of them is logged with its samples.
		static std::uint32_t landFrames = 0, landMax = 0, landLogged = 0;
		if (const std::uint32_t lost = a_results[kFoliageLandUnshaded] + a_results[kFoliageLandOtherObject]) {
			++landFrames;
			landMax = std::max(landMax, lost);
		}
		if (a_results[kFoliageLandUnshaded] >= 32 && landLogged < 40) {
			++landLogged;
			const auto& tables = SceneStore::Get().GetTables();
			std::string samples;
			const std::uint32_t count = std::min(a_results[kFoliageLandSampleCount], kFoliageLandSamples);
			for (std::uint32_t s = 0; s < count; ++s) {
				const std::uint32_t* sample = sampleAt(kFoliageLandSampleBase, s);
				const std::uint32_t index = sample[2] - 1;
				const auto* geometry = sample[2] && index < tables.objectGeometry.size() ? tables.objectGeometry[index] : nullptr;
				samples += fmt::format("; ({},{}) owner {} {} level '{}' bound ({:.0f} {:.0f}) r {:.0f} depth {:.7f}", sample[0] & 0xFFFF, sample[0] >> 16, sample[2],
					fmt::ptr(geometry), geometry && geometry->parent && geometry->parent->parent && geometry->parent->parent->name.c_str() ? geometry->parent->parent->name.c_str() : "?",
					geometry ? geometry->worldBound.center.x : 0.0f, geometry ? geometry->worldBound.center.y : 0.0f, geometry ? geometry->worldBound.radius : 0.0f,
					std::bit_cast<float>(sample[4]));
			}
			float range[4];
			LodHighDetailRange(range);
			const auto eye = RE::Main::WorldRootCamera() ? RE::Main::WorldRootCamera()->world.translate : RE::NiPoint3{};
			logger::warn("[DCLF] terrain LOD coverage: frame tag {}: {} of {} owned pixels unshaded (no draw), {} by another object; loaded range centre ({:.0f} {:.0f}) "
						 "half ({:.0f} {:.0f}), camera ({:.0f} {:.0f} {:.0f}){}",
				a_frame, a_results[kFoliageLandUnshaded], a_results[kFoliageLandOwned], a_results[kFoliageLandOtherObject], range[0], range[1], range[2], range[3], eye.x,
				eye.y, eye.z, samples);
		}
		if (a_foliage.frames % 300 == 0) {
			const double n = 300.0;
			const auto& t = a_foliage.totals;
			logger::info("[DCLF] terrain LOD coverage over 300 frames: per frame {:.0f} owned pixels, {:.1f} unshaded, {:.1f} by another object; {} frames lost "
						 "some (at most {}){}",
				t[kFoliageLandOwned] / n, t[kFoliageLandUnshaded] / n, t[kFoliageLandOtherObject] / n, landFrames, landMax,
				landFrames ? " <- TERRAIN LOD COVERAGE" : " <- OK");
			landFrames = landMax = 0;
			logger::info("[DCLF] foliage parity over 300 frames: within the frame {} flagged; per frame {:.0f} owned, {:.1f} unshaded, {:.1f} by another object, "
						 "{:.1f} with their motion off a static object's ({:.1f} by more than 8 px); near white: {} jumps, average {:.0f}, max {}; against the frame "
						 "before, reprojected: {} jumps (average {:.0f}, max {}), per frame {:.0f} compared, {:.1f} recoloured, {:.1f} brightened, {:.1f} whitened, "
						 "{:.1f} vanished, {:.1f} appeared",
				a_foliage.inFrameFlagged, t[kFoliageOwned] / n, t[kFoliageUnshaded] / n, t[kFoliageOtherObject] / n, t[kFoliageMotion] / n, t[kFoliageMotionFar] / n,
				a_foliage.whiteFlagged, a_foliage.whiteAverage, a_foliage.whiteMax, a_foliage.flagged, a_foliage.changeAverage, a_foliage.changeMax,
				t[kFoliageCompared] / n, t[kFoliageRecoloured] / n, t[kFoliageBrightened] / n, t[kFoliageWhitened] / n, t[kFoliageVanished] / n,
				t[kFoliageAppeared] / n);
			a_foliage.totals = {};
			a_foliage.flagged = a_foliage.inFrameFlagged = a_foliage.whiteFlagged = 0;
			a_foliage.whiteMax = a_foliage.changeMax = 0;
		}
	}

	/**
	 * @brief Tree LOD's draw (dclf-lod.md, "Tree LOD: the draws"): its pipeline, its push data (the draw row's address, and whether
	 * its instances start after phase 1's: phase 2's depth draw), its arguments in the list's header (TreeLod::VisibleHeader).
	 */
	void RecordTreeLodDraw(rhi::CommandList& a_commands, const TreeLodPipelines& a_pipelines, bool a_colour, bool a_phaseTwo, std::uint64_t a_draw, rhi::ResourceHandle a_visible)
	{
		std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_draw), static_cast<std::uint32_t>(a_draw >> 32), a_phaseTwo ? 1u : 0u };
		const std::uint64_t arguments = a_colour ? offsetof(TreeLod::VisibleHeader, colour) :
		                                a_phaseTwo ? offsetof(TreeLod::VisibleHeader, phaseTwo) : offsetof(TreeLod::VisibleHeader, phaseOne);
		a_commands.BindPipeline(a_colour ? a_pipelines.colour : a_pipelines.depth);
		a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
		a_commands.ExecuteIndirect(a_pipelines.drawSignature, a_visible, arguments, {}, 0, 1);
	}

	struct TreeLodCullBindings
	{
		org::DeclaredViewToken shapes, instances, draw, visible, hzb;
	};

	struct TreeLodCullPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		TreeLodCullConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Tree LOD's cull (TreeLodCullCS.hlsl), in the depth segment's two phases like BuildDraws': phase 1 tests every instance
	 * record against the frustum and the HZB the previous frame left, before the Z-prepass's draw; phase 2 tests phase 1's
	 * occluded ones against the HZB rebuilt from this frame's depth, before the rescues' draw. Both append to the list the passes
	 * draw. The depth commit uploaded the tables' changes, the draw row and the list's header with no instances.
	 */
	class TreeLodCullPass final : public org::TypedRenderGraphPass<TreeLodCullPass, TreeLodCullPrepared, TreeLodCullBindings>
	{
	public:
		TreeLodCullPass(std::shared_ptr<Resources> a_resources, std::uint32_t a_phase) :
			resources(std::move(a_resources)), phase(a_phase) {}

		TreeLodCullBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& scene = *resources->scene;
			TreeLodCullBindings bindings{};
			bindings.shapes = a_builder.ShaderResource(*scene.treeLodShapes).View();
			bindings.instances = a_builder.ShaderResource(*scene.treeLodInstances).View();
			bindings.draw = a_builder.ShaderResource(scene.treeLodDraw).View();
			bindings.visible = a_builder.UnorderedAccess(*scene.treeLodVisible).View();
			// As BuildDraws': phase 1 sees the HZB the previous frame left, phase 2 the one just rebuilt; their places around the
			// build (GatherStructuralPasses) are what makes the difference.
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			return bindings;
		}

		// What the recording depends on: the tables' layout (a growth gives them new views and a larger dispatch) and the depth
		// segment's shape (its latch).
		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::ZPrepass);
			a_out.push_back(resources->scene->layout.load(std::memory_order_acquire));
			a_out.push_back(frame ? frame->generation : 0);
		}

		TreeLodCullPrepared Prepare(const TreeLodCullBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			TreeLodCullPrepared prepared{};
			const auto& scene = *resources->scene;
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::ZPrepass);
			if (!scene.treeLodCull || !scene.treeLodShapeCapacity || !frame || !frame->latch)
				return prepared;
			prepared.program = scene.treeLodCull;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.shapesIndex = CaptureViewIndex(a_preparation, a_bindings.shapes);
			constants.instancesIndex = CaptureViewIndex(a_preparation, a_bindings.instances);
			constants.drawIndex = CaptureViewIndex(a_preparation, a_bindings.draw);
			constants.visibleIndex = CaptureViewIndex(a_preparation, a_bindings.visible);
			constants.latchIndex = frame->latch->SrvIndex();
			constants.phase = phase;
			const std::uint64_t records = std::uint64_t(scene.treeLodShapeCapacity) * TreeLod::kMaxGroupInstances;
			constants.retestOffset = static_cast<std::uint32_t>(sizeof(TreeLod::VisibleHeader) + records * sizeof(std::uint32_t));
			if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			// Every record the tables hold (phase 1), or every retest entry they can (phase 2): the shader stops at the draw row's
			// slots and each slot's count, or at the retest count.
			prepared.groups = static_cast<std::uint32_t>((records + kTreeLodCullGroup - 1) / kTreeLodCullGroup);
			return prepared;
		}

		static void Record(const TreeLodCullBindings&, const TreeLodCullPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot (RecordLatchedDispatch's offset 0).
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeLodCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::uint32_t phase = 1;
	};

	struct TreeLodReadbackBindings
	{
		org::ResourceBindingToken visible;
	};

	struct TreeLodReadbackPrepared
	{
		std::shared_ptr<SceneBuffers::TreeLodCounts> counts;
	};

	/**
	 * @brief Tree LOD's cull counts (SceneBuffers::TreeLodCounts): the list's header after the depth segment's second phase, to the
	 * frame slot's host buffer; what the slot held from its last frame is summed first.
	 */
	class TreeLodReadbackPass final : public org::TypedRenderGraphPass<TreeLodReadbackPass, TreeLodReadbackPrepared, TreeLodReadbackBindings>
	{
	public:
		explicit TreeLodReadbackPass(std::shared_ptr<SceneBuffers> a_scene) :
			scene(std::move(a_scene)) {}

		TreeLodReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			return { a_builder.CopySource(*scene->treeLodVisible) };
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(scene->layout.load(std::memory_order_acquire));
		}

		TreeLodReadbackPrepared Prepare(const TreeLodReadbackBindings&, const org::PassPrepareContext&) const
		{
			return { scene->treeLodCounts };
		}

		static void Record(const TreeLodReadbackBindings& a_bindings, const TreeLodReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.counts)
				return;
			auto& counts = *a_frame.counts;
			const std::uint32_t slot = a_recording.FrameSlot();
			if (slot >= counts.readback.size())
				return;
			if (counts.filled[slot]) {
				auto resource = counts.readback[slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					TreeLod::VisibleHeader header;
					std::memcpy(&header, mapped, sizeof(header));
					counts.frames.fetch_add(1, std::memory_order_relaxed);
					counts.phaseOne.fetch_add(header.phaseOne[1], std::memory_order_relaxed);
					counts.retests.fetch_add(header.retests, std::memory_order_relaxed);
					counts.phaseTwo.fetch_add(header.phaseTwo[1], std::memory_order_relaxed);
					resource.Unmap(0, 0);
				}
			}
			a_recording.Commands().CopyBufferRegion(counts.readback[slot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.visible).GetHandle(), 0,
				sizeof(TreeLod::VisibleHeader));
			counts.filled[slot] = 1;
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
	};

	class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
	{
	public:
		// phaseTwo: the second depth draw of the two-phase culling, which draws only what the rebuilt HZB
		// brought back, from the reserved part of the sequence buffer.
		MainOpaquePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_phaseTwo = false) :
			resources(std::move(a_resources)), segment(a_segment), phaseTwo(a_phaseTwo) {}

		PassBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			PassBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.targets[i] = a_builder.RenderTarget(resources->native[i]).View();
			bindings.depth = a_builder.DepthReadWrite(resources->nativeDepth).View();
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			bindings.count = a_builder.IndirectArguments(resources->count);
			// Read through device addresses; declared so the graph orders them after their uploads.
			bindings.materialRows = a_builder.ShaderResource(*resources->materialRows.buffer, noViews).Resource();
			bindings.pipelineRows = a_builder.ShaderResource(*resources->pipelineRows.buffer, noViews).Resource();
			bindings.objects = a_builder.ShaderResource(*resources->scene->objects, noViews).Resource();
			bindings.bones = a_builder.ShaderResource(*resources->scene->bones, noViews).Resource();
			if (segment == RenderGraphRuntime::Segment::ZPrepass) {
				// The plain draws (DCLF_PULLED): the vertex stage reads the sequences and the face positions through their addresses,
				// the draws index the pool, and each bucket's count word is its draw's count.
				a_builder.ShaderResource(*resources->sequences, noViews);
				a_builder.ShaderResource(*resources->scene->facePositions, noViews);
				bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
				bindings.bucketCounts = a_builder.IndirectArguments(*resources->zBucketCounts[phaseTwo ? 1 : 0]).Resource();
			} else {
				// Read by the input assembler (the face draws' second stream), after the commit's uploads into it.
				a_builder.VertexBuffer(*resources->scene->facePositions);
				// CS_DCLF_FOLIAGE_PARITY: the alpha-tested draws write what each pixel shows, through the buffers' addresses.
				if (const auto& foliage = resources->foliage) {
					const std::span<const org::UavView> noUavs{};
					for (std::uint32_t h = 0; h < 2; ++h) {
						a_builder.UnorderedAccess(foliage->ids[h], noUavs);
						a_builder.UnorderedAccess(foliage->colours[h], noUavs);
					}
				}
			}
			for (const auto& frameBuffer : resources->frameBuffers)
				bindings.frameBuffers.push_back(a_builder.ShaderResource(frameBuffer.copy, noViews).Resource());
			a_builder.ShaderResource(resources->frameConstants, noViews);  // the frame record's slots, by address
			// Tree LOD's draw: its arguments, and the tables its vertex stage reads through their addresses.
			if (const auto& scene = *resources->scene; scene.treeLodCull) {
				bindings.treeLodVisible = a_builder.IndirectArguments(*scene.treeLodVisible).Resource();
				for (const auto& table : { scene.treeLodShapes, scene.treeLodInstances, scene.treeLodMeshes })
					a_builder.ShaderResource(*table, noViews);
				a_builder.ShaderResource(scene.treeLodDraw, noViews);
			}
			if (resources->lightLimitFix) {
				bindings.lights = a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.lights"), noViews).Resource();
				bindings.lightIndexList = a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-index-list"), noViews).Resource();
				bindings.lightGrid = a_builder.ShaderResource(org::ResourceIdentifier("cs.llf.light-grid"), noViews).Resource();
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(phaseTwo ? 1 : 0);
			a_out.push_back(resources->pool ? resources->pool->layout : 0);
			// Tree LOD's pipelines, once built (and again after a target change).
			a_out.push_back(reinterpret_cast<std::uintptr_t>(resources->scene->treeLodPipelines.load(std::memory_order_acquire).get()));
		}

		PreparedDraws Prepare(const PassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			const auto now = segment;
			auto frame = CurrentFrame(*resources, now);
			if (!frame || (!frame->drawCapacity && !frame->decalCapacity[0] && !frame->decalCapacity[1] && !frame->decalCapacity[2]) || !frame->indirect.valid)
				return prepared;
			// The rescue draw belongs to the depth segment only.
			if (phaseTwo && now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			prepared.phaseTwo = phaseTwo;
			prepared.zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
			prepared.stats = resources->passStats.get();
			if (!prepared.zPrepass)
				prepared.preprocess = resources->preprocessMain.get();
			prepared.framePushWords = FramePushWords(resources->frameConstantsAddress);
			if (resources->scene->treeLodCull) {
				prepared.treeLod = resources->scene->treeLodPipelines.load(std::memory_order_acquire);
				prepared.treeLodDraw = resources->scene->treeLodDrawAddress;
			}
			prepared.frame = std::move(frame);
			prepared.targetCount = resources->targetCount;
			for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
				prepared.targetViews[i] = a_preparation.Capture(a_bindings.targets[i]);
			prepared.depthView = a_preparation.Capture(a_bindings.depth);
			return prepared;
		}

		static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			std::array<rhi::ColorAttachment, kColorTargets> colors{};
			for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i) {
				colors[i].rtv = a_recording.Resolve(a_prepared.targetViews[i]);
				colors[i].loadOp = rhi::LoadOp::Load;
				colors[i].storeOp = rhi::StoreOp::Store;
				colors[i].resource = a_recording.Resolve(a_bindings.targets[i].Resource()).GetHandle();
			}
			const bool zPrepass = a_prepared.zPrepass;
			if (a_prepared.phaseTwo && !zPrepass)
				return;
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;

			// DCLF's Z-prepass: depth only, like the native one. It adds DCLF's objects to the depth the native
			// passes already wrote, so they occlude and are occluded correctly. It runs in its own segment,
			// inside the native depth pass, so that the rest of the frame - the native draws that test depth,
			// the sky and everything that reads the depth buffer afterwards - sees DCLF's objects.
			rhi::DepthAttachment depth{};
			depth.dsv = a_recording.Resolve(a_prepared.depthView);
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			begin.depth = &depth;
			begin.debugName = "DCLF depth";
			auto* stats = a_prepared.stats;
			const std::uint32_t statsSlot = a_recording.FrameSlot();
			const auto depthKind = a_prepared.phaseTwo ? PassStats::kDepthPhaseTwo : PassStats::kDepth;
			// A pass of DCLF's draws: its attachments, the layout, the topology and the frame push data - on the command list,
			// and on a preprocess state list, identically.
			auto beginDrawPass = [&](rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin) {
				a_list.BeginPass(a_begin);
				a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				a_list.BindLayout(frame.indirect.layout);
				a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
			};
			// The frame slot's preprocess state list, with this pass's heaps; null without explicit preprocessing.
			auto preprocessState = [&]() -> rhi::CommandList* {
				if (!a_prepared.preprocess)
					return nullptr;
				auto& state = a_prepared.preprocess->Begin(statsSlot);
				state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				return &state;
			};
			if (zPrepass) {
				// Plain indexed indirect draws (DCLF_PULLED): one per bucket, a pipeline slot's range of the phase's sequences (phase 2's
				// past sequenceDraws, its rescues), with the slot's pulled depth pipeline, its push words (kZDrawPush*: the range's first
				// sequence, the slot's pipeline row, its vertex layout) and its count word, from the pool. The vertex stage fetches each
				// draw's vertices and rows through the sequence its instance names (Lighting.hlsl), since a plain draw cannot bind a
				// vertex buffer or push data of its own.
				(void)count;
				if (stats)
					stats->Start(commands, statsSlot, depthKind);
				commands.BeginPass(begin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
				if (stats)
					stats->Begin(commands, statsSlot, depthKind);
				auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
				const auto counts = a_recording.Resolve(a_bindings.bucketCounts).GetHandle();
				const std::uint64_t sequencesAddress = device.GetBufferDeviceAddress({ sequences, 0 });
				const std::uint64_t phaseBase = a_prepared.phaseTwo ? std::uint64_t(frame.sequenceDraws) : 0;
				auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
					a_words[0] = static_cast<std::uint32_t>(a_value);
					a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
				};
				for (const auto& call : frame.zCalls) {
					const std::uint64_t first = (phaseBase + call.first) * sizeof(DrawSequence);
					std::uint32_t words[kDrawPushWords]{};
					split(words + kZDrawPushSequences, sequencesAddress + first);
					commands.BindPipeline(call.pipeline);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
					commands.ExecuteIndirect(frame.indirect.zDrawSignature, sequences, first + kSequenceDrawOffset, counts, std::uint64_t(call.bucket) * sizeof(std::uint32_t),
						call.capacity);
				}
				// Tree LOD's depth: one instanced draw of the phase's culled records (TreeLodCullPass).
				if (a_prepared.treeLod)
					RecordTreeLodDraw(commands, *a_prepared.treeLod, false, a_prepared.phaseTwo, a_prepared.treeLodDraw, a_recording.Resolve(a_bindings.treeLodVisible).GetHandle());
				if (stats)
					stats->End(commands, statsSlot, depthKind);
				commands.EndPass();
				if (stats)
					stats->Resolve(commands, statsSlot, depthKind);
				return;
			}

			// The main pass: depth test EQUAL against the Z-prepass's (its pipelines do not write depth; the
			// attachment stays in the layout the pass declared).

			// The opaque decals' depth, before any colour: the engine's main pass
			// draws its opaque decal group with depth writes and its bias, so this is where the frame's depth has them, and
			// with it every host fragment under an opaque decal texel fails the colour pass's EQUAL test and is not shaded -
			// the decal overwrites every target there (blending off, full masks). The group's depth variants test LESS_EQUAL
			// with the decal's bias and run the alpha test, so a transparent texel leaves the host's depth. The blended
			// group has none: it writes no depth natively, and it blends over its host, which must still be shaded.
			// nvperf ranges around the colour pass's parts (NvPerfBridge::PushPassRange), when a capture selects them.
			void* const nvCommands = NvPerfBridge::Active() ? rhi::vulkan::get_cmd_list(commands) : nullptr;
			auto subRange = [&](const char* a_name) { return nvCommands && NvPerfBridge::PushPassRange(nvCommands, a_name); };
			auto endSubRange = [&](bool a_pushed) { if (nvCommands) NvPerfBridge::PopPassRange(nvCommands, a_pushed); };
			// The colour passes: the main one with the targets as the frame loads them, the decals' with all of them loaded; each
			// with the layout, the topology and the frame push data.
			auto decalColors = colors;
			for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i)
				decalColors[i].loadOp = rhi::LoadOp::Load;
			rhi::PassBeginInfo mainBegin = begin;
			mainBegin.colors = { colors.data(), a_prepared.targetCount };
			mainBegin.debugName = "DCLF main opaque";
			rhi::PassBeginInfo decalBegin = begin;
			decalBegin.colors = { decalColors.data(), a_prepared.targetCount };
			decalBegin.debugName = "DCLF decals";
			rhi::PassBeginInfo decalDepthBegin = begin;  // depth only
			decalDepthBegin.debugName = "DCLF decal depth";
			const auto colourSignature = frame.indirect.signatures[kColorVariant];
			auto decalArguments = [&](std::uint32_t a_group) {
				return (2 * std::uint64_t(frame.sequenceDraws) + std::uint64_t(a_group) * frame.sequenceDecals) * sizeof(DrawSequence);
			};
			auto decalCount = [](std::uint32_t a_group) { return std::uint64_t(DecalCountWord(a_group)) * sizeof(std::uint32_t); };
			const bool anyDecals = frame.decalCapacity[0] || frame.decalCapacity[1] || frame.decalCapacity[2];

			// Every call below is preprocessed here, before the first pass: generated inside the pass
			// instead, by NVIDIA's driver, each colour call cost a fixed ~170 us of idle GPU in these eight-target passes. The
			// preprocess is generated for the state list's state - each call's pass begun and set up exactly as it is below,
			// with the same heaps - which the execution must match; the sequences and counts are final before this pass (the
			// culling wrote them).
			if (auto* preprocessList = preprocessState()) {
				auto& state = *preprocessList;
				if (frame.decalCapacity[0]) {
					beginDrawPass(state, decalDepthBegin);
					commands.PreprocessIndirect(state, frame.indirect.signatures[kDepthVariant], sequences, decalArguments(0), count, decalCount(0), frame.decalCapacity[0]);
					state.EndPass();
				}
				beginDrawPass(state, mainBegin);
				if (frame.drawCapacity)
					commands.PreprocessIndirect(state, colourSignature, sequences, 0, count, 0, frame.drawCapacity);
				state.EndPass();
				if (anyDecals) {
					beginDrawPass(state, decalBegin);
					for (const std::uint32_t group : kDecalDrawOrder)
						if (frame.decalCapacity[group])
							commands.PreprocessIndirect(state, colourSignature, sequences, decalArguments(group), count, decalCount(group), frame.decalCapacity[group]);
					state.EndPass();
				}
				state.End();
			}

			if (frame.decalCapacity[0]) {
				const bool part = subRange("cs.dclf.colour.decal-depth");
				beginDrawPass(commands, decalDepthBegin);
				commands.ExecuteIndirect(frame.indirect.signatures[kDepthVariant], sequences, decalArguments(0), count, decalCount(0), frame.decalCapacity[0]);
				commands.EndPass();
				endSubRange(part);
			}
			if (stats)
				stats->Start(commands, statsSlot, PassStats::kColour);
			const bool mainPart = subRange("cs.dclf.colour.main");
			beginDrawPass(commands, mainBegin);
			if (stats)
				stats->Begin(commands, statsSlot, PassStats::kColour);
			if (frame.drawCapacity) {
				const bool drawPart = subRange("cs.dclf.colour.main-draws");
				commands.ExecuteIndirect(colourSignature, sequences, 0, count, 0, frame.drawCapacity);
				endSubRange(drawPart);
			}
			// Tree LOD's colour, testing EQUAL against its Z-prepass draw: the pulled pipeline under the Z-prepass's layout, whose
			// frame push data is the main layout's.
			if (a_prepared.treeLod) {
				const bool treePart = subRange("cs.dclf.colour.tree-lod");
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				RecordTreeLodDraw(commands, *a_prepared.treeLod, true, false, a_prepared.treeLodDraw, a_recording.Resolve(a_bindings.treeLodVisible).GetHandle());
				endSubRange(treePart);
			}
			if (stats)
				stats->End(commands, statsSlot, PassStats::kColour);
			commands.EndPass();
			endSubRange(mainPart);
			if (stats)
				stats->Resolve(commands, statsSlot, PassStats::kColour);

			// The second pass: decals, after every opaque draw, in the engine's order - its opaque decal
			// group, the multi-index layers, then its blended group, each from its own fixed-slot range and its own count
			// word. The pipelines test depth LESS_EQUAL with the engine's decal bias; only the layers write depth here (the
			// opaque group's is already there, from the decal depth pass). Same attachments, all loaded.
			if (anyDecals) {
				const bool decalPart = subRange("cs.dclf.colour.decals");
				beginDrawPass(commands, decalBegin);
				for (const std::uint32_t group : kDecalDrawOrder) {
					if (!frame.decalCapacity[group])
						continue;
					const bool groupPart = subRange(group == 0 ? "cs.dclf.colour.decals-opaque" : group == 2 ? "cs.dclf.colour.decals-layers" : "cs.dclf.colour.decals-blended");
					commands.ExecuteIndirect(colourSignature, sequences, decalArguments(group), count, decalCount(group), frame.decalCapacity[group]);
					endSubRange(groupPart);
				}
				commands.EndPass();
				endSubRange(decalPart);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool phaseTwo = false;
	};

	void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16])
	{
		std::memcpy(a_out, a_viewProj.data(), sizeof(a_out));
		for (std::uint32_t row = 0; row < 4; ++row) {
			a_out[row * 4 + 3] = a_viewProj[row * 4 + 3] -
			                     (a_viewProj[row * 4 + 0] * a_eye.x + a_viewProj[row * 4 + 1] * a_eye.y +
								     a_viewProj[row * 4 + 2] * a_eye.z);
		}
	}

	struct BuildDrawsBindings
	{
		org::DeclaredViewToken inputs, inputsDepth, geometries, objects, sequences, count, hzb, visibility, frustum;
		org::DeclaredViewToken sortCounts, sortStaging, sortRanks;
		org::DeclaredViewToken fadeRoots;
		org::DeclaredViewToken bucketCounts, poolFirsts;  // the depth segment's buckets (Resources::zBucketCounts) and the pool's firsts
	};

	struct BuildDrawsFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		BuildDrawsConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
	};

	void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
		rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording)
	{
		auto& commands = a_recording.Commands();
		const std::uint64_t offset = a_latch.Offset(a_recording.FrameSlot()) + a_latchOffset;
		a_constants.latchOffset = static_cast<std::uint32_t>(offset);
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_constants));
		commands.ExecuteIndirect(a_signature, a_latch.Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
	}

	// Writes this frame's draw sequences and their count from the draw inputs (BuildDrawsCS.hlsl).
	class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildDrawsFrame, BuildDrawsBindings>
	{
	public:
		BuildDrawsPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, std::uint32_t a_phase = 0) :
			resources(std::move(a_resources)), segment(a_segment), fixedPhase(a_phase) {}

		BuildDrawsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			BuildDrawsBindings bindings{};
			bindings.inputs = a_builder.ShaderResource(*resources->inputs).View();
			if (resources->inputsDepth)
				bindings.inputsDepth = a_builder.ShaderResource(*resources->inputsDepth).View();
			bindings.geometries = a_builder.ShaderResource(*resources->scene->geometries).View();
			bindings.objects = a_builder.ShaderResource(*resources->scene->objects).View();
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			bindings.count = a_builder.UnorderedAccess(resources->count).View();
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			if (resources->frustum)
				bindings.frustum = a_builder.UnorderedAccess(*resources->frustum).View();
			// The depth segment's first phase reads the fade roots' static rows, and the states FadeStateCS published the frame before
			// (its latch's; undeclared, as nothing this frame writes them: SceneBuffers::fadeStatesOut).
			if (segment == RenderGraphRuntime::Segment::ZPrepass && fixedPhase != 2 && resources->scene->fadeRoots)
				bindings.fadeRoots = a_builder.ShaderResource(*resources->scene->fadeRoots).View();
			// Phase 1 sees the HZB the previous frame left, phase 2 the one just rebuilt from this
			// frame's depth. Both read the same resource; what differs is where they sit relative to
			// the build, which is why the ordering below is the whole design.
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			// The depth segment's phases write their draws into the pipeline slots' buckets (Resources::zBucketCounts), indexed
			// from the pool (PoolFirstsIndex), for the Z-prepass's plain draws.
			if (segment == RenderGraphRuntime::Segment::ZPrepass && resources->pool) {
				bindings.bucketCounts = a_builder.UnorderedAccess(*resources->zBucketCounts[fixedPhase == 2 ? 1 : 0]).View();
				bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			}
			if (Sorts()) {
				bindings.sortCounts = a_builder.UnorderedAccess(resources->sort->counts).View();
				bindings.sortStaging = a_builder.UnorderedAccess(*resources->sort->staging).View();
				bindings.sortRanks = a_builder.UnorderedAccess(*resources->sort->ranks).View();
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(fixedPhase);
			a_out.push_back(resources->pool ? resources->pool->layout : 0);
		}

		BuildDrawsFrame Prepare(const BuildDrawsBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			BuildDrawsFrame prepared{};
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			if (!frame || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			const std::uint32_t phase = Phase(now);
			// The second phase belongs to the depth segment only: it is what re-tests phase 1's rejects
			// against the HZB that has just been rebuilt from this frame's depth.
			if (fixedPhase == 2 && now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			auto& constants = prepared.constants;
			constants.latchIndex = frame->latch->SrvIndex();
			// The depth segment's phases read its own inputs (Resources::inputsDepth), the colour segment the colour inputs.
			const bool depthInputs = (phase == 1 || phase == 2) && resources->inputsDepth;
			constants.inputsIndex = CaptureViewIndex(a_preparation, depthInputs ? a_bindings.inputsDepth : a_bindings.inputs);
			constants.geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
			constants.objectsIndex = CaptureViewIndex(a_preparation, a_bindings.objects);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			// The rows both segments' draws name (MainRows), at their tables' addresses as the epoch sized them.
			constants.materialRowsAddressLo = static_cast<std::uint32_t>(frame->materialRows);
			constants.materialRowsAddressHi = static_cast<std::uint32_t>(frame->materialRows >> 32);
			constants.materialRowStride = kMaterialRowBytes;
			constants.pipelineRowsAddressLo = static_cast<std::uint32_t>(frame->pipelineRows);
			constants.pipelineRowsAddressHi = static_cast<std::uint32_t>(frame->pipelineRows >> 32);
			constants.pipelineRowStride = kPipelineRowBytes;
			constants.phaseBits = (phase & 0xFu) << 4;
			constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			// The frustum stamps: the depth segment's first phase alone tests every candidate's frustum.
			if (resources->frustum && phase == 1)
				constants.frustumIndex = CaptureViewIndex(a_preparation, a_bindings.frustum);
			if (phase == 1 && resources->scene->fadeRoots && resources->scene->fadeRootCount) {
				constants.fadeRootsIndex = CaptureViewIndex(a_preparation, a_bindings.fadeRoots);
			}
			if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
				constants.hzbIndex = CaptureViewIndex(a_preparation, a_bindings.hzb);
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			if (now == RenderGraphRuntime::Segment::ZPrepass && resources->pool) {
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts);
				constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			}
			if (Sorts()) {
				constants.sortCountsIndex = CaptureViewIndex(a_preparation, a_bindings.sortCounts);
				constants.sortStagingIndex = CaptureViewIndex(a_preparation, a_bindings.sortStaging);
				constants.sortRanksIndex = CaptureViewIndex(a_preparation, a_bindings.sortRanks);
			}
			// The sequence buffer's ranges as the epoch sized them.
			constants.phaseTwoBase = frame->sequenceDraws;
			constants.decalBase = 2 * frame->sequenceDraws;
			constants.decalStride = frame->sequenceDecals;
			return prepared;
		}

		static void Record(const BuildDrawsBindings&, const BuildDrawsFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			RecordLatchedDispatch(a_frame.constants, *a_frame.latch, a_frame.signature, 0, a_recording);
		}

		std::uint32_t Phase(RenderGraphRuntime::Segment a_segment) const
		{
			if (fixedPhase)
				return fixedPhase;
			return a_segment == RenderGraphRuntime::Segment::ZPrepass ? 1u : 3u;
		}

	private:
		// Phase 2 appends its few rescues into its own range, unsorted; the other builds are followed by the sort's passes.
		// The colour segment's draws, sorted by pipeline for its device-generated draw; the depth segment's are bucketed instead.
		bool Sorts() const { return resources->sort && segment != RenderGraphRuntime::Segment::ZPrepass; }

		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		std::uint32_t fixedPhase = 0;
	};

	struct SortSequencesBindings
	{
		org::DeclaredViewToken staging, ranks, offsets, count, sequences;
	};

	struct SortSequencesFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		SortSequencesConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief The sort's scatter (DrawSort), after its segment's BuildDraws and the scan. It covers every slot BuildDraws can
	 * append (the draws' range as the epoch sized it), each thread past the count returning.
	 */
	class SortSequencesPass final : public org::TypedRenderGraphPass<SortSequencesPass, SortSequencesFrame, SortSequencesBindings>
	{
	public:
		SortSequencesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		SortSequencesBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			SortSequencesBindings bindings{};
			bindings.staging = a_builder.ShaderResource(*resources->sort->staging).View();
			bindings.ranks = a_builder.ShaderResource(*resources->sort->ranks).View();
			bindings.offsets = a_builder.ShaderResource(resources->sort->offsets).View();
			bindings.count = a_builder.ShaderResource(resources->count).View();
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		SortSequencesFrame Prepare(const SortSequencesBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			SortSequencesFrame prepared{};
			if (!BuildsDraws(*resources, segment))
				return prepared;
			prepared.program = resources->sort->scatter;
			auto& constants = prepared.constants;
			constants.stagingIndex = CaptureViewIndex(a_preparation, a_bindings.staging);
			constants.ranksIndex = CaptureViewIndex(a_preparation, a_bindings.ranks);
			constants.offsetsIndex = CaptureViewIndex(a_preparation, a_bindings.offsets);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
			const auto frame = CurrentFrame(*resources, segment);
			constants.drawLimit = frame ? frame->sequenceDraws : 0;
			prepared.groups = (constants.drawLimit + kSortSequencesGroup - 1) / kSortSequencesGroup;
			return prepared;
		}

		static void Record(const SortSequencesBindings&, const SortSequencesFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kSortSequencesConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	struct TreeWindBindings
	{
		org::DeclaredViewToken trees, clocks, frame;
		std::array<org::DeclaredViewToken, 2> wind;
	};

	struct TreeWindFramePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		TreeWindConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Tree wind (TreeWindCS.hlsl), on the compute queue in the Z-prepass epoch: each tree's clock brought to this frame
	 * and its TreeParams and WindTimers into this frame's wind buffer (SceneBuffers::treeWindRows, by the frame's parity), which
	 * the next frame's draws read. Nothing in any epoch reads what it writes, so nothing waits for it: it has the epoch's raster
	 * work to run beside, and the host orders it against the frames either side (RenderGraphRuntime, the compute queue's entry
	 * and exit). The readers do not declare the wind buffers for the same reason: the one they read was finished a frame ago.
	 */
	class TreeWindPass final : public org::TypedRenderGraphPass<TreeWindPass, TreeWindFramePrepared, TreeWindBindings>
	{
	public:
		explicit TreeWindPass(std::shared_ptr<SceneBuffers> a_scene) :
			scene(std::move(a_scene)) {}

		TreeWindBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Compute);
			TreeWindBindings bindings{};
			bindings.trees = a_builder.ShaderResource(*scene->trees).View();
			bindings.frame = a_builder.ShaderResource(scene->treeFrameBuffer).View();
			bindings.clocks = a_builder.UnorderedAccess(*scene->treeClocks).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.wind[h] = a_builder.UnorderedAccess(*scene->treeWindRows[h]).View();
			return bindings;
		}

		// What the recording depends on: the buffers' layout (a growth gives them new views). The count and the frame's inputs
		// are the frame row, which every commit uploads.
		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(scene->layout.load(std::memory_order_acquire));
		}

		TreeWindFramePrepared Prepare(const TreeWindBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			TreeWindFramePrepared prepared{};
			if (!scene->treeWind || !scene->treeFrameBuffer)
				return prepared;
			prepared.program = scene->treeWind;
			auto& constants = prepared.constants;
			constants.treesIndex = CaptureViewIndex(a_preparation, a_bindings.trees);
			constants.clocksIndex = CaptureViewIndex(a_preparation, a_bindings.clocks);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.windIndices[h] = CaptureViewIndex(a_preparation, a_bindings.wind[h]);
			// Every entry the buffers hold (the nodeless one, then the tree slots): the shader stops at the frame row's count.
			prepared.groups = (scene->treeCapacity + 1 + kTreeWindGroup - 1) / kTreeWindGroup;
			return prepared;
		}

		static void Record(const TreeWindBindings&, const TreeWindFramePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeWindConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
	};

	struct FadeStateBindings
	{
		org::DeclaredViewToken roots, states, frame, objects, log, visibility, rootLists, animated, events, reported;
		std::array<org::DeclaredViewToken, 2> published;
	};

	struct FadeStatePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		FadeStateConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
	};

	/**
	 * @brief The fade roots' state (FadeStateCS.hlsl): each root's OnVisible for the main camera whenever its bound is in the
	 * depth segment's frustum, once a frame, ahead of that segment's culling.
	 */
	class FadeStatePass final : public org::TypedRenderGraphPass<FadeStatePass, FadeStatePrepared, FadeStateBindings>
	{
	public:
		explicit FadeStatePass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		FadeStateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Compute);
			const auto& scene = *resources->scene;
			FadeStateBindings bindings{};
			bindings.roots = a_builder.ShaderResource(*scene.fadeRoots).View();
			bindings.states = a_builder.UnorderedAccess(*scene.fadeStates).View();
			for (std::uint32_t h = 0; h < 2; ++h)
				bindings.published[h] = a_builder.UnorderedAccess(*scene.fadeStatesOut[h]).View();
			bindings.frame = a_builder.ShaderResource(scene.fadeFrameBuffer).View();
			bindings.visibility = a_builder.ShaderResource(scene.fadeVisibility).View();
			bindings.rootLists = a_builder.ShaderResource(*scene.fadeRootLists).View();
			bindings.animated = a_builder.ShaderResource(*scene.fadeAnimated).View();
			bindings.objects = a_builder.ShaderResource(*scene.objects).View();
			bindings.log = a_builder.UnorderedAccess(scene.fadeLog).View();
			if (scene.fadeEvents) {
				bindings.events = a_builder.UnorderedAccess(*scene.fadeEvents).View();
				bindings.reported = a_builder.UnorderedAccess(*scene.fadeReported).View();
			}
			return bindings;
		}

		// What the recording depends on: the buffers' layout and the depth segment's shape (its latch). The root count, the frame
		// and the log are the frame row (Records.h, FadeFrame), which the depth commit uploads.
		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::ZPrepass);
			a_out.push_back(resources->scene->layout.load(std::memory_order_acquire));
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->scene->fadeWriteBack ? resources->scene->fadeWriteBack->capacity.load(std::memory_order_acquire) : 0u);
		}

		FadeStatePrepared Prepare(const FadeStateBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			FadeStatePrepared prepared{};
			const auto& scene = *resources->scene;
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::ZPrepass);
			if (!scene.fadeState || !scene.fadeRootCapacity || !frame || !frame->latch)
				return prepared;
			prepared.program = scene.fadeState;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.rootsIndex = CaptureViewIndex(a_preparation, a_bindings.roots);
			constants.statesIndex = CaptureViewIndex(a_preparation, a_bindings.states);
			constants.frameIndex = CaptureViewIndex(a_preparation, a_bindings.frame);
			constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			constants.objectsIndex = CaptureViewIndex(a_preparation, a_bindings.objects);
			constants.logIndex = CaptureViewIndex(a_preparation, a_bindings.log);
			constants.rootListsIndex = CaptureViewIndex(a_preparation, a_bindings.rootLists);
			constants.animatedIndex = CaptureViewIndex(a_preparation, a_bindings.animated);
			for (std::uint32_t h = 0; h < 2; ++h)
				constants.outIndices[h] = CaptureViewIndex(a_preparation, a_bindings.published[h]);
			if (scene.fadeEvents && scene.fadeWriteBack) {
				constants.eventsIndex = CaptureViewIndex(a_preparation, a_bindings.events);
				constants.reportedIndex = CaptureViewIndex(a_preparation, a_bindings.reported);
				constants.eventCapacity = scene.fadeWriteBack->capacity.load(std::memory_order_acquire);
			}
			constants.latchIndex = frame->latch->SrvIndex();
			// Every slot the buffers hold: the shader stops at the frame row's count.
			prepared.groups = (scene.fadeRootCapacity + kFadeStateGroup - 1) / kFadeStateGroup;
			return prepared;
		}

		static void Record(const FadeStateBindings&, const FadeStatePrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			// The depth segment's BuildDrawsLatch in this frame slot (RecordLatchedDispatch's offset 0).
			constants.latchOffset = static_cast<std::uint32_t>(a_frame.latch->Offset(a_recording.FrameSlot()));
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kFadeStateConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	struct FadeEventReadbackBindings
	{
		org::ResourceBindingToken events;
	};
	struct FadeEventReadbackPrepared
	{
		std::shared_ptr<FadeWriteBack> writeBack;
	};

	/**
	 * @brief The fade write-back's events (FadeStateCS), copied to the frame slot's host buffer after the pass; what that buffer
	 * held from the slot's last execution (finished by now: the slot is the host's) is read first and handed to the main thread
	 * (FadeWriteBack, IndirectDraws::ApplyFadeWriteBack).
	 */
	class FadeEventReadbackPass final : public org::TypedRenderGraphPass<FadeEventReadbackPass, FadeEventReadbackPrepared, FadeEventReadbackBindings>
	{
	public:
		explicit FadeEventReadbackPass(std::shared_ptr<SceneBuffers> a_scene) :
			scene(std::move(a_scene)) {}

		FadeEventReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			FadeEventReadbackBindings bindings{};
			bindings.events = a_builder.CopySource(*scene->fadeEvents);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(scene->layout.load(std::memory_order_acquire));
		}

		FadeEventReadbackPrepared Prepare(const FadeEventReadbackBindings&, const org::PassPrepareContext&) const
		{
			return { scene->fadeWriteBack };
		}

		static void Record(const FadeEventReadbackBindings& a_bindings, const FadeEventReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.writeBack)
				return;
			auto& writeBack = *a_frame.writeBack;
			const std::uint32_t slot = a_recording.FrameSlot();
			if (slot >= writeBack.readback.size())
				return;
			if (writeBack.filled[slot]) {
				auto resource = writeBack.readback[slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					const auto* words = static_cast<const std::uint32_t*>(mapped);
					const std::uint32_t count = words[0];
					const std::uint32_t events = std::min(count, writeBack.readbackEvents[slot]);
					// The roots past the list's end try again; the list grows to them (ReserveSceneTables).
					if (count > writeBack.readbackEvents[slot]) {
						std::uint32_t wanted = writeBack.wanted.load(std::memory_order_relaxed);
						while (wanted < count && !writeBack.wanted.compare_exchange_weak(wanted, count, std::memory_order_release, std::memory_order_relaxed)) {}
					}
					if (events) {
						auto* batch = new FadeWriteBack::Batch();
						batch->events.resize(events);
						std::memcpy(batch->events.data(), words + kFadeEventHeaderWords, std::size_t(events) * sizeof(FadeEvent));
						batch->frame = words[1];
						writeBack.Push(batch);
					}
					resource.Unmap(0, 0);
				}
			}
			// A larger buffer after a growth, once this one is read; the shader may append to the fewest every slot now holds.
			if (auto larger = writeBack.next[slot].exchange(nullptr, std::memory_order_acq_rel)) {
				writeBack.readback[slot] = std::move(larger);
				writeBack.readbackEvents[slot] = static_cast<std::uint32_t>(writeBack.readback[slot]->GetSize() / sizeof(std::uint32_t) - kFadeEventHeaderWords) / 4;
				writeBack.capacity.store(*std::min_element(writeBack.readbackEvents.begin(), writeBack.readbackEvents.end()), std::memory_order_release);
			}
			// The count and the events the shader may have appended (never more than any slot's buffer or the list hold).
			const std::uint32_t copied = std::min(writeBack.readbackEvents[slot], writeBack.capacity.load(std::memory_order_acquire));
			a_recording.Commands().CopyBufferRegion(writeBack.readback[slot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.events).GetHandle(), 0,
				FadeWriteBack::BytesFor(copied));
			writeBack.filled[slot] = 1;
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
	};

	struct HzbBindings
	{
		org::DeclaredViewToken depth;
		org::DeclaredViewToken counter;
		std::vector<org::DeclaredViewToken> hzbMips;
	};

	struct HzbFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		// One single-pass downsample per kHzbDispatchMips levels at most, the first reading the scene depth and each other
		// the previous one's last level. A domain of up to 4096 texels a side takes one.
		struct Dispatch
		{
			HzbConstants constants{};
			std::uint32_t groupsX = 0, groupsY = 0;
		};
		std::vector<Dispatch> dispatches;
	};

	/**
	 * @brief Builds the hierarchical depth buffer after the world's depth draws.
	 *
	 * It runs in the ZPrepass segment, after DCLF's own depth draws, so the HZB describes the depth the
	 * frame actually has: the native occluders the engine drew (terrain and everything ineligible) plus
	 * the objects DCLF drew itself. On AE that is inside the depth pass, before the first-person model's
	 * depth and the rooms' stencil draws, which it therefore leaves out: fewer occluders, never more.
	 *
	 * The whole mip chain is one dispatch of the single-pass downsampler (HzbCS.hlsl, FidelityFX SPD): a
	 * dispatch per level with a full barrier between each cost about 0.1 ms, nearly all of it the drains.
	 * Levels of a single texture are not separate resources to the graph, so the chain is one pass either way.
	 */
	class HzbPass final : public org::TypedRenderGraphPass<HzbPass, HzbFrame, HzbBindings>
	{
	public:
		HzbPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		HzbBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			HzbBindings bindings{};
			bindings.depth = a_builder.ShaderResource(resources->nativeDepth).View();
			bindings.counter = a_builder.UnorderedAccess(resources->hzbCounter).View();
			bindings.hzbMips.reserve(resources->hzbMips);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip)
				bindings.hzbMips.push_back(a_builder.UnorderedAccess(resources->hzb, org::UavView{ UINT32_MAX, mip }).View());
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		HzbFrame Prepare(const HzbBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			HzbFrame prepared{};
			// Only in the Z-prepass segment: anywhere else the depth is not the world's final depth.
			const auto now = segment;
			if (now != RenderGraphRuntime::Segment::ZPrepass)
				return prepared;
			if (!resources->hzb || !resources->hzbProgram || !resources->hzbCounter)
				return prepared;
			// A published depth frame: its commit zeroed the group counter (hzbCounterZeroed) before this first dispatch.
			const auto frame = CurrentFrame(*resources, now);
			if (!frame)
				return prepared;
			const std::uint32_t renderWidth = frame->width ? frame->width : resources->width;
			const std::uint32_t renderHeight = frame->height ? frame->height : resources->height;
			prepared.program = resources->hzbProgram;
			const std::uint32_t depthIndex = CaptureViewIndex(a_preparation, a_bindings.depth);
			const std::uint32_t counterIndex = CaptureViewIndex(a_preparation, a_bindings.counter);
			std::vector<std::uint32_t> levelIndices(resources->hzbMips);
			for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip)
				levelIndices[mip] = CaptureViewIndex(a_preparation, a_bindings.hzbMips[mip]);
			const auto levelSize = [&](std::uint32_t a_mip, std::uint32_t a_axis) {
				return std::max(1u, (a_axis == 0 ? resources->hzbWidth : resources->hzbHeight) >> a_mip);
			};
			for (std::uint32_t level = 0; level < resources->hzbMips;) {
				HzbFrame::Dispatch dispatch{};
				auto& constants = dispatch.constants;
				constants.counterIndex = counterIndex;
				if (level == 0) {
					// Mip 0 is half the padded domain, of which the rendered area holds depth: the depth image can be
					// larger than the viewport the draws use, and everything past that viewport is untouched.
					constants.fromDepth = 1;
					constants.sourceIndex = depthIndex;
					constants.domainSize[0] = resources->hzbWidth * 2;
					constants.domainSize[1] = resources->hzbHeight * 2;
					constants.validSize[0] = std::min(renderWidth, constants.domainSize[0]);
					constants.validSize[1] = std::min(renderHeight, constants.domainSize[1]);
				} else {
					constants.fromDepth = 0;
					constants.sourceIndex = levelIndices[level - 1];
					constants.domainSize[0] = constants.validSize[0] = levelSize(level - 1, 0);
					constants.domainSize[1] = constants.validSize[1] = levelSize(level - 1, 1);
				}
				constants.targetSize[0] = levelSize(level, 0);
				constants.targetSize[1] = levelSize(level, 1);
				// A group reduces its tile through six levels; the last group carries on from the sixth only when that
				// level fits its own tile, which a domain of more than kHzbTile tiles a side does not.
				const std::uint32_t domain = std::max(constants.domainSize[0], constants.domainSize[1]);
				const std::uint32_t reach = domain / kHzbTile <= kHzbTile ? kHzbDispatchMips : 6u;
				constants.mips = std::min(resources->hzbMips - level, reach);
				for (std::uint32_t i = 0; i < constants.mips; ++i)
					constants.targetIndices[i] = levelIndices[level + i];
				dispatch.groupsX = (constants.domainSize[0] + kHzbTile - 1) / kHzbTile;
				dispatch.groupsY = (constants.domainSize[1] + kHzbTile - 1) / kHzbTile;
				constants.workGroups = dispatch.groupsX * dispatch.groupsY;
				level += constants.mips;
				prepared.dispatches.push_back(dispatch);
			}
			return prepared;
		}

		static void Record(const HzbBindings&, const HzbFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || a_frame.dispatches.empty())
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			for (std::size_t i = 0; i < a_frame.dispatches.size(); ++i) {
				const auto& dispatch = a_frame.dispatches[i];
				if (i != 0) {
					// The previous dispatch's last level must be complete before this one reads it.
					const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
					const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
					commands.Barriers(batch);
				}
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kHzbConstantWords, reinterpret_cast<const std::uint32_t*>(&dispatch.constants));
				commands.Dispatch(dispatch.groupsX, dispatch.groupsY, 1);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	struct FoliageParityBindings
	{
		org::DeclaredViewToken results, owners, depth;
	};
	struct FoliageParityPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		FoliageParityConstants constants{};
		std::uint32_t groupsX = 0, groupsY = 0;
	};

	/**
	 * @brief CS_DCLF_FOLIAGE_PARITY's compare pass (FoliageParityCS.hlsl), after the colour pass: this frame's pixels of the
	 * alpha-tested draws against the frame before's, into the results (GpuLayouts.h, FoliageCounter).
	 */
	class FoliageParityPass final : public org::TypedRenderGraphPass<FoliageParityPass, FoliageParityPrepared, FoliageParityBindings>
	{
	public:
		explicit FoliageParityPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		FoliageParityBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};  // read through their addresses
			const auto& foliage = *resources->foliage;
			for (std::uint32_t h = 0; h < 2; ++h) {
				a_builder.ShaderResource(foliage.ids[h], noViews);
				a_builder.ShaderResource(foliage.colours[h], noViews);
			}
			FoliageParityBindings bindings{};
			bindings.results = a_builder.UnorderedAccess(foliage.results).View();
			a_builder.ShaderResource(resources->frameConstants, std::span<const org::SrvView>{});  // its frame block, by address
			bindings.owners = a_builder.UnorderedAccess(foliage.owners).View();
			bindings.depth = a_builder.ShaderResource(resources->nativeDepth).View();
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::MainOpaque);
			a_out.push_back(frame ? frame->generation : 0);
		}

		FoliageParityPrepared Prepare(const FoliageParityBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			FoliageParityPrepared prepared{};
			const auto& foliage = *resources->foliage;
			auto& constants = prepared.constants;
			for (std::uint32_t h = 0; h < 2; ++h) {
				constants.ids[h] = foliage.idsAddress[h];
				constants.colours[h] = foliage.coloursAddress[h];
			}
			constants.frameBlock = resources->frameConstantsAddress + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame);
			constants.resultsIndex = CaptureViewIndex(a_preparation, a_bindings.results);
			constants.ownersIndex = CaptureViewIndex(a_preparation, a_bindings.owners);
			constants.depthIndex = CaptureViewIndex(a_preparation, a_bindings.depth);
			constants.width = foliage.width;
			constants.height = foliage.height;
			prepared.program = foliage.program;
			prepared.groupsX = (foliage.width + 7) / 8;
			prepared.groupsY = (foliage.height + 7) / 8;
			return prepared;
		}

		static void Record(const FoliageParityBindings&, const FoliageParityPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kFoliageParityConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
			commands.Dispatch(a_frame.groupsX, a_frame.groupsY, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	struct FoliageReadbackBindings
	{
		org::ResourceBindingToken results;
	};
	struct FoliageReadbackPrepared
	{
		std::shared_ptr<Resources::FoliageParity> foliage;
	};

	/**
	 * @brief CS_DCLF_FOLIAGE_PARITY's results, copied to the frame slot's host buffer; what that buffer held from the slot's last
	 * frame (finished by now: the slot is the host's) is read first and reported (FoliageParityReport).
	 */
	class FoliageReadbackPass final : public org::TypedRenderGraphPass<FoliageReadbackPass, FoliageReadbackPrepared, FoliageReadbackBindings>
	{
	public:
		explicit FoliageReadbackPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		FoliageReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			FoliageReadbackBindings bindings{};
			bindings.results = a_builder.CopySource(resources->foliage->results);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::MainOpaque);
			a_out.push_back(frame ? frame->generation : 0);
		}

		FoliageReadbackPrepared Prepare(const FoliageReadbackBindings&, const org::PassPrepareContext&) const
		{
			FoliageReadbackPrepared prepared{};
			const auto frame = CurrentFrame(*resources, RenderGraphRuntime::Segment::MainOpaque);
			if (!frame)
				return prepared;
			prepared.foliage = resources->foliage;
			return prepared;
		}

		static void Record(const FoliageReadbackBindings& a_bindings, const FoliageReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.foliage)
				return;
			auto& foliage = *a_frame.foliage;
			const std::uint32_t slot = a_recording.FrameSlot();
			if (slot >= foliage.readback.size())
				return;
			auto resource = foliage.readback[slot]->GetAPIResource();
			if (foliage.readbackFrame[slot]) {
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					const auto* results = static_cast<const std::uint32_t*>(mapped);
					FoliageParityReport(foliage, results[kFoliageEpochTag], results);
					resource.Unmap(0, 0);
				}
			}
			a_recording.Commands().CopyBufferRegion(resource.GetHandle(), 0, a_recording.Resolve(a_bindings.results).GetHandle(), 0,
				std::uint64_t(kFoliageResultWords) * sizeof(std::uint32_t));
			foliage.readbackFrame[slot] = 1;
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	struct ProbeBindings
	{
		std::array<org::ResourceBindingToken, kColorTargets> sources;
		org::ResourceBindingToken depth;
		org::ResourceBindingToken destination;
	};

	struct ProbeFrame
	{
		std::uint32_t count = 0;
		std::uint32_t base = 0;        // first slot this pass writes
		std::uint32_t depthSlot = ~0u;  // where this pass puts the depth texel, if it samples it
		std::uint32_t x = 0, y = 0;
	};

	// Copies one texel of each main-pass target into the probe buffer. Two of these are declared, one
	// either side of the opaque pass, so the same texel is sampled before and after DCLF's colour draws
	// within a single frame.
	class ProbePass final : public org::TypedRenderGraphPass<ProbePass, ProbeFrame, ProbeBindings>
	{
	public:
		ProbePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_after) :
			resources(std::move(a_resources)), segment(a_segment), after(a_after) {}

		ProbeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ProbeBindings bindings{};
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				bindings.sources[i] = a_builder.CopySource(resources->native[i]);
			bindings.depth = a_builder.CopySource(resources->nativeDepth);
			bindings.destination = a_builder.CopyDestination(resources->probe);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(*resources,
				now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling ? RenderGraphRuntime::Segment::ZPrepass : now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
		}

		ProbeFrame Prepare(const ProbeBindings&, const org::PassPrepareContext&) const
		{
			ProbeFrame prepared{};
			const auto now = segment;
			const bool zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
			const bool gapProbe = now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling;
			if (now != RenderGraphRuntime::Segment::MainOpaque && !(zPrepass && after) && !gapProbe)
				return prepared;
			const auto frame = CurrentFrame(*resources, gapProbe ? RenderGraphRuntime::Segment::ZPrepass : now);
			if (!frame || !frame->probePixel)
				return prepared;
			prepared.x = frame->probeX;
			prepared.y = frame->probeY;
			if (zPrepass) {
				prepared.depthSlot = kProbeDepthAfterPrepass;  // what the Z-prepass left in the buffer
				return prepared;
			}
			if (gapProbe) {
				prepared.depthSlot = now == RenderGraphRuntime::Segment::SkyOcclusion ? kProbeDepthAfterSky : kProbeDepthAfterLightCulling;
				return prepared;
			}
			prepared.count = resources->targetCount;
			prepared.base = after ? kColorTargets : 0;
			// What the colour pass tests against.
			prepared.depthSlot = after ? kProbeDepthAfterColour : kProbeDepthBeforeColour;
			return prepared;
		}

		static void Record(const ProbeBindings& a_bindings, const ProbeFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.count && a_frame.depthSlot == ~0u)
				return;
			auto& commands = a_recording.Commands();
			const auto buffer = a_recording.Resolve(a_bindings.destination).GetHandle();
			if (a_frame.depthSlot != ~0u) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_recording.Resolve(a_bindings.depth).GetHandle();
				copy.buffer = buffer;
				copy.x = a_frame.x;
				copy.y = a_frame.y;
				copy.footprint.offset = std::uint64_t(a_frame.depthSlot) * kProbeSlotBytes;
				copy.footprint.rowPitch = kProbeSlotBytes;
				copy.footprint.width = 1;
				copy.footprint.height = 1;
				copy.footprint.depth = 1;
				commands.CopyTextureToBuffer(copy);
			}
			for (std::uint32_t i = 0; i < a_frame.count; ++i) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
				copy.buffer = buffer;
				copy.mip = 0;
				copy.arraySlice = 0;
				copy.x = a_frame.x;
				copy.y = a_frame.y;
				copy.z = 0;
				copy.footprint.offset = std::uint64_t(a_frame.base + i) * kProbeSlotBytes;
				copy.footprint.rowPitch = kProbeSlotBytes;
				copy.footprint.width = 1;
				copy.footprint.height = 1;
				copy.footprint.depth = 1;
				commands.CopyTextureToBuffer(copy);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool after = false;
	};

	bool SetParityEnabled()
	{
		return SwitchEnabled(Switch::SetParity);
	}

	bool BuildParityEnabled()
	{
		return SwitchEnabled(Switch::BuildParity);
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
		// An array image publishes a view per slice (ORG builds one DSV per slice for arrays), which is
		// how a shadow view's epoch attaches the one slice the engine gave that view.
		a_desc.isArray = a_image.arrayLayers > 1;
		a_desc.arraySize = a_desc.isArray ? a_image.arrayLayers : 1;
		a_desc.initialLayout = rhi::ResourceLayout::Common;
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, true);
		if (result)
			result->SetName(a_name);
		return result;
	}

	/** @brief The frame shape a shadow epoch's passes draw: the shadow views', or (a_sky) the occlusion views'. */
	std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const ShadowResources& a_resources, bool a_sky)
	{
		return (a_sky ? a_resources.occlusionFrame : a_resources.frame).load(std::memory_order_acquire);
	}

	struct ShadowBuildBindings
	{
		std::array<org::DeclaredViewToken, kShadowModeCount> inputs;
		std::vector<org::DeclaredViewToken> sequences, count, bucketCounts;  // per view slot
		org::DeclaredViewToken geometries, objects, visibility, poolFirsts;
		org::DeclaredViewToken fadeRoots;
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
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				bindings.inputs[m] = a_builder.ShaderResource(*resources->inputs[m]).View();
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.UnorderedAccess(*resources->sequences[s]).View());
				bindings.count.push_back(a_builder.UnorderedAccess(resources->count[s]).View());
				bindings.bucketCounts.push_back(a_builder.UnorderedAccess(*resources->bucketCounts[s]).View());
			}
			bindings.geometries = a_builder.ShaderResource(*resources->scene->geometries).View();
			bindings.objects = a_builder.ShaderResource(*resources->scene->objects).View();
			bindings.visibility = a_builder.UnorderedAccess(*resources->visibility).View();
			bindings.poolFirsts = a_builder.ShaderResource(*resources->pool->firsts).View();
			// The fade roots' rows: a shadow view's casters under stood-in roots follow FadeStateCS's state, and an occlusion view's
			// occluders their roots' OnVisible (kCullFadeOnVisible); the states are the latch's (published the frame before, or a
			// parity frame's node states), undeclared like the depth segment's.
			if (resources->scene->fadeRoots)
				bindings.fadeRoots = a_builder.ShaderResource(*resources->scene->fadeRoots).View();
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(*resources, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(FadeRows() ? 1u : 0u);
			a_out.push_back(resources->pool->layout);
		}

		ShadowBuildPrepared Prepare(const ShadowBuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			ShadowBuildPrepared prepared{};
			const auto frame = CurrentShadowFrame(*resources, sky);
			if (!frame || frame->views.empty() || !resources->buildDraws || !frame->latch || !resources->dispatchSignature)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			const auto geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
			const auto objectsIndex = CaptureViewIndex(a_preparation, a_bindings.objects);
			const auto visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			const auto poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			const bool fadeRows = FadeRows();
			const auto fadeRootsIndex = fadeRows ? CaptureViewIndex(a_preparation, a_bindings.fadeRoots) : 0u;
			for (const auto& view : frame->views) {
				if (view.slot >= a_bindings.sequences.size() || view.modeIndex >= kShadowModeCount)
					continue;
				ShadowBuildPrepared::Dispatch dispatch{};
				dispatch.latchOffset = view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
				auto& constants = dispatch.constants;
				constants.latchIndex = frame->latch->SrvIndex();
				constants.inputsIndex = CaptureViewIndex(a_preparation, a_bindings.inputs[view.modeIndex]);
				constants.geometriesIndex = geometriesIndex;
				constants.objectsIndex = objectsIndex;
				constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences[view.slot]);
				constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count[view.slot]);
				constants.bucketCountsIndex = CaptureViewIndex(a_preparation, a_bindings.bucketCounts[view.slot]);
				constants.poolFirstsIndex = poolFirstsIndex;
				// A draw's words name its material row (the table every view reads).
				constants.materialRowsAddressLo = static_cast<std::uint32_t>(view.materialRows);
				constants.materialRowsAddressHi = static_cast<std::uint32_t>(view.materialRows >> 32);
				constants.materialRowStride = sizeof(ShadowMaterialRow);  // no pipeline rows: their stride stays 0
				// The bucket path's draws are bounded by their buckets' capacities (the latch's bucket table); this bounds the other.
				constants.phaseTwoBase = view.sequenceDraws;
				// The single phase (the latch holds the frustum-only mode, with no engine-visibility gate).
				constants.phaseBits = 0;
				// The visibility words are written per object by every dispatch; nothing reads them here.
				constants.visibilityIndex = visibilityIndex;
				constants.fadeRootsIndex = fadeRootsIndex;
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
		// The fade roots' rows hold the tables' (the depth commit's upload), so a root slot names its row.
		bool FadeRows() const { return resources->scene->fadeRoots && resources->scene->fadeRootCount; }

		std::shared_ptr<ShadowResources> resources;
		bool sky = false;
	};

	struct ShadowLatchedCopiesBindings
	{
		org::ResourceBindingToken viewBlocks, constants;
		std::vector<org::ResourceBindingToken> count, bucketCounts;  // per view slot
	};

	struct ShadowLatchedCopiesPrepared
	{
		std::shared_ptr<const ShadowFrame> frame;
	};

	/**
	 * @brief The views' per-frame values the commit wrote into the latch, copied to where the views read them: each view's blocks
	 * (ShadowLatchLayout::ViewBlockOffset) to its slot of the view-blocks buffer, and its draw count and bucket counts zeroed from
	 * the zeros (ShadowResources::zeros). The copies depend on the frame's shape alone, so they are recorded with the epoch's
	 * ticket on ORG's host thread, and the commit records none.
	 */
	class ShadowLatchedCopiesPass final : public org::TypedRenderGraphPass<ShadowLatchedCopiesPass, ShadowLatchedCopiesPrepared, ShadowLatchedCopiesBindings>
	{
	public:
		ShadowLatchedCopiesPass(std::shared_ptr<ShadowResources> a_resources, bool a_sky) :
			resources(std::move(a_resources)), sky(a_sky) {}

		ShadowLatchedCopiesBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ShadowLatchedCopiesBindings bindings{};
			bindings.viewBlocks = a_builder.CopyDestination(*resources->viewBlocks.buffer).Resource();
			// The shadow commit's latched values (ShadowFrame::latched) go to the constants alone.
			bindings.constants = a_builder.CopyDestination(resources->constants);
			resources->latchedTargets.store(std::make_shared<const std::vector<const void*>>(1, resources->constants.get()), std::memory_order_release);
			for (std::size_t s = 0; s < resources->count.size(); ++s) {
				bindings.count.push_back(a_builder.CopyDestination(resources->count[s]));
				bindings.bucketCounts.push_back(a_builder.CopyDestination(*resources->bucketCounts[s]).Resource());
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(*resources, sky);
			a_out.push_back(frame ? frame->generation : 0);
		}

		ShadowLatchedCopiesPrepared Prepare(const ShadowLatchedCopiesBindings&, const org::PassPrepareContext&) const
		{
			ShadowLatchedCopiesPrepared prepared{};
			if (auto frame = CurrentShadowFrame(*resources, sky); frame && frame->latch && frame->zeros && (!frame->views.empty() || !frame->latched.copies.empty()))
				prepared.frame = std::move(frame);
			return prepared;
		}

		static void Record(const ShadowLatchedCopiesBindings& a_bindings, const ShadowLatchedCopiesPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			const auto latch = frame.latch->Resource()->GetAPIResource().GetHandle();
			const auto zeros = frame.zeros->Resource()->GetAPIResource().GetHandle();
			const std::uint64_t region = frame.latch->Offset(a_recording.FrameSlot()) + frame.viewBlocksOffset;
			const auto viewBlocks = a_recording.Resolve(a_bindings.viewBlocks).GetHandle();
			for (const auto& view : frame.views) {
				if (view.slot >= a_bindings.count.size())
					continue;
				commands.CopyBufferRegion(viewBlocks, std::uint64_t(view.slot) * kShadowViewSlotBytes, latch, region + std::uint64_t(view.slot) * kShadowViewSlotBytes,
					kShadowViewSlotBytes);
				commands.CopyBufferRegion(a_recording.Resolve(a_bindings.count[view.slot]).GetHandle(), 0, zeros, 0, sizeof(kZeroCounts));
				const std::uint64_t bucketBytes = std::max<std::size_t>(view.buckets.size(), 1) * sizeof(std::uint32_t);
				commands.CopyBufferRegion(a_recording.Resolve(a_bindings.bucketCounts[view.slot]).GetHandle(), 0, zeros, 0, bucketBytes);
			}
			if (const auto& list = frame.latched; list.latch) {
				const auto source = list.latch->Resource()->GetAPIResource().GetHandle();
				const std::uint64_t latched = list.latch->Offset(a_recording.FrameSlot());
				const auto constants = a_recording.Resolve(a_bindings.constants).GetHandle();
				for (const auto& copy : list.copies)
					commands.CopyBufferRegion(constants, copy.dstOffset, source, latched + copy.latchOffset, copy.bytes);
			}
		}

	private:
		std::shared_ptr<ShadowResources> resources;
		bool sky = false;
	};

	struct IndexPoolBindings
	{
		org::DeclaredViewToken copies, indices;
	};

	struct IndexPoolPrepared
	{
		std::shared_ptr<const IndexPool> pool;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t poolOffset = 0;  // ShadowLatchLayout::PoolOffset
		IndexPoolConstants constants{};
	};

	/**
	 * @brief The index pool's copies (IndexPool, IndexPoolCS.hlsl): the ranges the shadow commit gave out this frame,
	 * copied from their index buffers before the shadow epoch's views draw from the pool. Its dispatch is in the latch.
	 */
	class IndexPoolPass final : public org::TypedRenderGraphPass<IndexPoolPass, IndexPoolPrepared, IndexPoolBindings>
	{
	public:
		// The epoch's latch (its frame's) and the pool's dispatch in it, as of the shape the epoch runs: the shadow epoch's, or the
		// depth segment's (whichever commit gave the pool's ranges out this frame wrote the copies).
		struct LatchOf
		{
			std::uint64_t generation = 0;
			std::shared_ptr<const org::LatchBlock> latch;
			std::uint32_t poolOffset = 0;
		};
		IndexPoolPass(std::shared_ptr<IndexPool> a_pool, std::function<LatchOf()> a_latch) :
			pool(std::move(a_pool)), latchOf(std::move(a_latch)) {}

		IndexPoolBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			IndexPoolBindings bindings{};
			bindings.copies = a_builder.ShaderResource(*pool->copies).View();
			bindings.indices = a_builder.UnorderedAccess(*pool->indices).View();
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto latch = latchOf();
			a_out.push_back(latch.generation);
			a_out.push_back(latch.poolOffset);
			a_out.push_back(pool->layout);
		}

		IndexPoolPrepared Prepare(const IndexPoolBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			IndexPoolPrepared prepared{};
			const auto latch = latchOf();
			if (!latch.latch)
				return prepared;
			prepared.pool = pool;
			prepared.latch = latch.latch;
			prepared.poolOffset = latch.poolOffset;
			prepared.constants.latchIndex = latch.latch->SrvIndex();
			prepared.constants.copiesIndex = CaptureViewIndex(a_preparation, a_bindings.copies);
			prepared.constants.poolIndex = CaptureViewIndex(a_preparation, a_bindings.indices);
			return prepared;
		}

		static void Record(const IndexPoolBindings&, const IndexPoolPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.pool || !a_prepared.latch)
				return;
			auto constants = a_prepared.constants;
			const auto& layout = a_prepared.latch;
			const std::uint64_t offset = layout->Offset(a_recording.FrameSlot()) + a_prepared.poolOffset;
			constants.latchOffset = static_cast<std::uint32_t>(offset);
			auto& commands = a_recording.Commands();
			const auto& program = *a_prepared.pool->program;
			commands.BindLayout(program.layout->GetHandle());
			commands.BindPipeline(program.pipeline->GetHandle());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kIndexPoolConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.ExecuteIndirect(a_prepared.pool->dispatchSignature->GetHandle(), layout->Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
		}

	private:
		std::shared_ptr<IndexPool> pool;
		std::function<LatchOf()> latchOf;
	};

	struct ShadowPassBindings
	{
		std::array<std::vector<org::DeclaredViewToken>, kShadowDepthTargets> depthViews{};
		std::vector<org::ResourceBindingToken> sequences, bucketCounts;  // per view slot
		org::ResourceBindingToken pool;  // the index pool (IndexPool)
		org::ResourceBindingToken materialRows, constants, viewBlocks, objects, bones;
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

	/** @brief The shadow views' draws: each view's culled sequences into its slice and viewport, in one pass. */
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
			// The shadow views draw into the shadow maps, the occlusion epoch into the occlusion maps alone.
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i] && IsOcclusionTarget(i) == sky) {
					bindings.depthViews[i].reserve(resources->depthLayers[i]);
					for (std::uint32_t slice = 0; slice < resources->depthLayers[i]; ++slice)
						bindings.depthViews[i].push_back(a_builder.DepthReadWrite(resources->depth[i], org::DsvView{ UINT32_MAX, 0, slice }).View());
				}
			}
			// A slot's sequences are both the draws' arguments and what their vertex stage reads by device address (DCLF_PULLED).
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				bindings.sequences.push_back(a_builder.IndirectArguments(*resources->sequences[s]).Resource());
				a_builder.ShaderResource(*resources->sequences[s], noViews);
				bindings.bucketCounts.push_back(a_builder.IndirectArguments(*resources->bucketCounts[s]).Resource());
			}
			bindings.materialRows = a_builder.ShaderResource(*resources->materialRows.buffer, noViews).Resource();
			bindings.constants = a_builder.ShaderResource(resources->constants, noViews).Resource();
			bindings.viewBlocks = a_builder.ShaderResource(*resources->viewBlocks.buffer, noViews).Resource();
			bindings.objects = a_builder.ShaderResource(*resources->scene->objects, noViews).Resource();
			bindings.bones = a_builder.ShaderResource(*resources->scene->bones, noViews).Resource();
			// The face positions (a dynamic shape's second stream), and the geometries' vertices and indices, are read by the
			// vertex stage through their addresses: the face positions after the commit's uploads into them.
			a_builder.ShaderResource(*resources->scene->facePositions, noViews);
			bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentShadowFrame(*resources, sky);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->pool->layout);
		}

		ShadowPrepared Prepare(const ShadowPassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			ShadowPrepared prepared{};
			auto frame = CurrentShadowFrame(*resources, sky);
			if (!frame || frame->views.empty() || !frame->indirect.valid)
				return prepared;
			for (std::uint32_t i = 0; i < frame->views.size(); ++i) {
				const auto& view = frame->views[i];
				if (!view.capacity || view.slot >= a_bindings.sequences.size() || view.target >= kShadowDepthTargets || !resources->depth[view.target] ||
					IsOcclusionTarget(view.target) != sky)
					continue;
				if (view.slice >= resources->depthLayers[view.target])
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
			// A view's pass: its slice and viewport, loaded, added to, stored.
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
				// The slice the engine drew its own casters into: loaded, added to, stored.
				auto& depth = depths[i];
				depth.dsv = a_recording.Resolve(prepared.depthView);
				depth.depthLoad = rhi::LoadOp::Load;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = rhi::LoadOp::Load;
				depth.stencilStore = rhi::StoreOp::Store;
				begin.depth = &depth;
				begin.debugName = a_prepared.sky ? "DCLF occlusion map" : "DCLF shadow view";
			}
			// A view: its pass (its slice, the layout, the topology, its push data: its blocks, and the index pool), then a plain
			// indexed indirect draw per bucket (ShadowBucket), the depth-only class's first: the bucket's pipeline, its push words
			// (kShadowDrawPushSequences and after), and its range of the slot's sequences, as many as its count word holds. The
			// vertex stage fetches each draw's vertices through the sequence its instance names (Utility.hlsl, DCLF_PULLED), since
			// a plain draw cannot bind a vertex buffer of its own.
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
					split(words + kShadowDrawPushMaterialRows, view.materialRows);
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

	/** @brief The shape the reflection epoch's passes draw (ExecuteReflection), null while it has none. */
	std::shared_ptr<const ReflectionFrame> CurrentReflectionFrame(const ReflectionResources& a_resources)
	{
		return a_resources.frame.load(std::memory_order_acquire);
	}

	struct ReflectionBuildBindings
	{
		org::DeclaredViewToken inputs, geometries, objects, sequences, count, visibility, poolFirsts;
		std::array<org::DeclaredViewToken, kReflectionFaces> bucketCounts;
	};

	struct ReflectionBuildPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		rhi::CommandSignatureHandle signature{};
		BuildDrawsConstants constants{};
		std::array<std::uint32_t, kReflectionFaces> bucketCountsIndex{};
	};

	/**
	 * @brief The reflection faces' culling (ExecuteReflection): BuildDrawsCS in its single phase over the depth segment's inputs the
	 * frame before left, one dispatch per face, frustum only; its latch's slots' map sends a LOD slot to its forward pipeline's
	 * bucket and every other slot to none. A face the update does not render has a dispatch of 0 groups.
	 */
	class ReflectionBuildDrawsPass final : public org::TypedRenderGraphPass<ReflectionBuildDrawsPass, ReflectionBuildPrepared, ReflectionBuildBindings>
	{
	public:
		explicit ReflectionBuildDrawsPass(std::shared_ptr<ReflectionResources> a_resources) :
			resources(std::move(a_resources)) {}

		ReflectionBuildBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& main = *resources->main;
			ReflectionBuildBindings bindings{};
			bindings.inputs = a_builder.ShaderResource(*main.inputsDepth).View();
			bindings.geometries = a_builder.ShaderResource(*main.scene->geometries).View();
			bindings.objects = a_builder.ShaderResource(*main.scene->objects).View();
			bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
			bindings.count = a_builder.UnorderedAccess(resources->count).View();
			// Read only in the single phase (an input's published verdict), never written.
			bindings.visibility = a_builder.UnorderedAccess(*main.visibility).View();
			bindings.poolFirsts = a_builder.ShaderResource(*main.scene->pool->firsts).View();
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f)
				bindings.bucketCounts[f] = a_builder.UnorderedAccess(*resources->bucketCounts[f]).View();
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentReflectionFrame(*resources);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->main->scene->pool->layout);
		}

		ReflectionBuildPrepared Prepare(const ReflectionBuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			ReflectionBuildPrepared prepared{};
			const auto frame = CurrentReflectionFrame(*resources);
			if (!frame || !frame->latch || !resources->buildDraws || !resources->dispatchSignature)
				return prepared;
			prepared.program = resources->buildDraws;
			prepared.latch = frame->latch;
			prepared.signature = resources->dispatchSignature->GetHandle();
			auto& constants = prepared.constants;
			constants.latchIndex = frame->latch->SrvIndex();
			constants.inputsIndex = CaptureViewIndex(a_preparation, a_bindings.inputs);
			constants.geometriesIndex = CaptureViewIndex(a_preparation, a_bindings.geometries);
			constants.objectsIndex = CaptureViewIndex(a_preparation, a_bindings.objects);
			constants.sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
			constants.countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
			constants.visibilityIndex = CaptureViewIndex(a_preparation, a_bindings.visibility);
			constants.poolFirstsIndex = CaptureViewIndex(a_preparation, a_bindings.poolFirsts);
			// The main rows, which the inputs' y names (RowsOf).
			constants.materialRowsAddressLo = static_cast<std::uint32_t>(frame->materialRows);
			constants.materialRowsAddressHi = static_cast<std::uint32_t>(frame->materialRows >> 32);
			constants.materialRowStride = kMaterialRowBytes;
			constants.pipelineRowsAddressLo = static_cast<std::uint32_t>(frame->pipelineRows);
			constants.pipelineRowsAddressHi = static_cast<std::uint32_t>(frame->pipelineRows >> 32);
			constants.pipelineRowStride = kPipelineRowBytes;
			constants.phaseBits = 0;  // the single phase: frustum only (the latch's mode), nothing published
			// The bucket path's draws are bounded by their buckets (the latch's tables); this bounds the other, which no face takes.
			constants.phaseTwoBase = kReflectionFaces * frame->sequenceDraws;
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f)
				prepared.bucketCountsIndex[f] = CaptureViewIndex(a_preparation, a_bindings.bucketCounts[f]);
			return prepared;
		}

		static void Record(const ReflectionBuildBindings&, const ReflectionBuildPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.program || !a_prepared.latch)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_prepared.program->layout->GetHandle());
			commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				auto constants = a_prepared.constants;
				constants.bucketCountsIndex = a_prepared.bucketCountsIndex[f];
				RecordLatchedDispatch(constants, *a_prepared.latch, a_prepared.signature, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), a_recording);
			}
		}

	private:
		std::shared_ptr<ReflectionResources> resources;
	};

	struct ReflectionTreeBindings
	{
		org::DeclaredViewToken shapes, instances;
		std::array<org::DeclaredViewToken, kReflectionFaces> rows, visible;
	};

	struct ReflectionTreePrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		TreeLodCullConstants constants{};
		std::array<std::uint32_t, kReflectionFaces> rowIndex{}, visibleIndex{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief The reflection faces' tree LOD (ExecuteReflection): TreeLodCullCS's phase 1 per face, against the face's latch entry
	 * (cull mode 1: no occlusion), into the face's list; a face's row names no shape slot when it is not drawn.
	 */
	class ReflectionTreeCullPass final : public org::TypedRenderGraphPass<ReflectionTreeCullPass, ReflectionTreePrepared, ReflectionTreeBindings>
	{
	public:
		explicit ReflectionTreeCullPass(std::shared_ptr<ReflectionResources> a_resources) :
			resources(std::move(a_resources)) {}

		ReflectionTreeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& scene = *resources->main->scene;
			ReflectionTreeBindings bindings{};
			bindings.shapes = a_builder.ShaderResource(*scene.treeLodShapes).View();
			bindings.instances = a_builder.ShaderResource(*scene.treeLodInstances).View();
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				bindings.rows[f] = a_builder.ShaderResource(resources->treeRows[f]).View();
				bindings.visible[f] = a_builder.UnorderedAccess(*resources->treeVisible[f]).View();
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentReflectionFrame(*resources);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->main->scene->layout.load(std::memory_order_acquire));
		}

		ReflectionTreePrepared Prepare(const ReflectionTreeBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			ReflectionTreePrepared prepared{};
			const auto& scene = *resources->main->scene;
			const auto frame = CurrentReflectionFrame(*resources);
			if (!frame || !frame->latch || !frame->treeGroups || !scene.treeLodCull)
				return prepared;
			prepared.program = scene.treeLodCull;
			prepared.latch = frame->latch;
			prepared.groups = frame->treeGroups;
			auto& constants = prepared.constants;
			constants.shapesIndex = CaptureViewIndex(a_preparation, a_bindings.shapes);
			constants.instancesIndex = CaptureViewIndex(a_preparation, a_bindings.instances);
			constants.latchIndex = frame->latch->SrvIndex();
			constants.phase = 1;
			constants.retestOffset = static_cast<std::uint32_t>(sizeof(TreeLod::VisibleHeader));  // never written: no occlusion test
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				prepared.rowIndex[f] = CaptureViewIndex(a_preparation, a_bindings.rows[f]);
				prepared.visibleIndex[f] = CaptureViewIndex(a_preparation, a_bindings.visible[f]);
			}
			return prepared;
		}

		static void Record(const ReflectionTreeBindings&, const ReflectionTreePrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.program || !a_prepared.groups || !a_prepared.latch)
				return;
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_prepared.program->layout->GetHandle());
			commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
			const auto region = static_cast<std::uint32_t>(a_prepared.latch->Offset(a_recording.FrameSlot()));
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				auto constants = a_prepared.constants;
				constants.latchOffset = region + f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
				constants.drawIndex = a_prepared.rowIndex[f];
				constants.visibleIndex = a_prepared.visibleIndex[f];
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kTreeLodCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.Dispatch(a_prepared.groups, 1, 1);
			}
		}

	private:
		std::shared_ptr<ReflectionResources> resources;
	};

	struct ReflectionDrawBindings
	{
		std::array<org::DeclaredViewToken, kReflectionFaces> faces;
		org::DeclaredViewToken depth;
		org::ResourceBindingToken sequences, pool;
		std::array<org::ResourceBindingToken, kReflectionFaces> bucketCounts, treeVisible;
	};

	struct ReflectionDrawPrepared
	{
		std::shared_ptr<const ReflectionFrame> frame;
		std::array<org::PreparedDescriptorReference, kReflectionFaces> faces{};
		org::PreparedDescriptorReference depth{};
		std::array<std::uint64_t, kReflectionFaces> treeRows{};
	};

	/**
	 * @brief The reflection faces' draws: per face, a pass on its slice of the engine's cube target (loaded and stored: the engine's
	 * sky is there) with DCLF's depth cleared; per bucket a plain indirect draw with its forward pipeline over the face's range of the
	 * sequences, as the Z-prepass's (DCLF_PULLED), then the face's tree LOD.
	 */
	class ReflectionDrawPass final : public org::TypedRenderGraphPass<ReflectionDrawPass, ReflectionDrawPrepared, ReflectionDrawBindings>
	{
	public:
		explicit ReflectionDrawPass(std::shared_ptr<ReflectionResources> a_resources) :
			resources(std::move(a_resources)) {}

		ReflectionDrawBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const std::span<const org::SrvView> noViews{};
			const auto& main = *resources->main;
			const auto& scene = *main.scene;
			ReflectionDrawBindings bindings{};
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f)
				bindings.faces[f] = a_builder.RenderTarget(resources->cube, org::RtvView{ UINT32_MAX, 0, f }).View();
			bindings.depth = a_builder.DepthReadWrite(resources->depth).View();
			// The sequences are the draws' arguments and what the vertex stage reads by device address (DCLF_PULLED).
			bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
			a_builder.ShaderResource(*resources->sequences, noViews);
			bindings.pool = a_builder.IndexBuffer(*scene.pool->indices);
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f)
				bindings.bucketCounts[f] = a_builder.IndirectArguments(*resources->bucketCounts[f]).Resource();
			// Read through device addresses; declared so the graph orders them after their uploads.
			a_builder.ShaderResource(*main.materialRows.buffer, noViews);
			a_builder.ShaderResource(main.frameConstants, noViews);  // the colour segment's frame slots, by address
			a_builder.ShaderResource(*main.pipelineRows.buffer, noViews);
			a_builder.ShaderResource(*scene.objects, noViews);
			a_builder.ShaderResource(*scene.bones, noViews);
			a_builder.ShaderResource(*scene.facePositions, noViews);
			a_builder.ShaderResource(resources->faceBlocks, noViews);
			if (scene.treeLodCull && resources->treeVisible[0]) {
				for (const auto& table : { scene.treeLodShapes, scene.treeLodInstances, scene.treeLodMeshes })
					a_builder.ShaderResource(*table, noViews);
				for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
					a_builder.ShaderResource(resources->treeRows[f], noViews);
					bindings.treeVisible[f] = a_builder.IndirectArguments(*resources->treeVisible[f]).Resource();
					a_builder.ShaderResource(*resources->treeVisible[f], noViews);
				}
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentReflectionFrame(*resources);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(resources->main->scene->pool->layout);
		}

		ReflectionDrawPrepared Prepare(const ReflectionDrawBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			ReflectionDrawPrepared prepared{};
			auto frame = CurrentReflectionFrame(*resources);
			if (!frame || !frame->indirect.valid || !frame->width || !frame->height)
				return prepared;
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				prepared.faces[f] = a_preparation.Capture(a_bindings.faces[f]);
				prepared.treeRows[f] = resources->treeRowsAddress[f];
			}
			prepared.depth = a_preparation.Capture(a_bindings.depth);
			prepared.frame = std::move(frame);
			return prepared;
		}

		static void Record(const ReflectionDrawBindings& a_bindings, const ReflectionDrawPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			auto device = RenderGraphRuntime::Get().Host()->GetDesc().device;
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const std::uint64_t sequencesAddress = device.GetBufferDeviceAddress({ sequences, 0 });
			const rhi::IndexBufferView pool{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt };
			auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
				a_words[0] = static_cast<std::uint32_t>(a_value);
				a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
			};
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				rhi::ColorAttachment colour{};
				colour.rtv = a_recording.Resolve(a_prepared.faces[f]);
				colour.loadOp = rhi::LoadOp::Load;
				colour.storeOp = rhi::StoreOp::Store;
				rhi::DepthAttachment depth{};
				depth.dsv = a_recording.Resolve(a_prepared.depth);
				depth.depthLoad = rhi::LoadOp::Clear;
				depth.depthStore = rhi::StoreOp::Store;
				depth.clear.type = rhi::ClearValueType::DepthStencil;
				depth.clear.format = rhi::Format::D24_UNorm_S8_UInt;
				depth.clear.depthStencil = { 1.0f, 0 };
				rhi::PassBeginInfo begin{};
				begin.colors = { &colour, 1 };
				begin.depth = &depth;
				begin.width = frame.width;
				begin.height = frame.height;
				begin.minDepth = 0.0f;
				begin.maxDepth = 1.0f;
				begin.debugName = "DCLF reflection face";
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
					const std::uint64_t first = (std::uint64_t(f) * frame.sequenceDraws + bucket.first) * sizeof(DrawSequence);
					std::uint32_t words[kDrawPushWords]{};
					split(words + kZDrawPushSequences, sequencesAddress + first);
					commands.BindPipeline(bucket.pipeline);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
					commands.ExecuteIndirect(frame.indirect.zDrawSignature, sequences, first + kSequenceDrawOffset, counts, std::uint64_t(b) * sizeof(std::uint32_t),
						bucket.capacity);
				}
				// Tree LOD: one instanced draw of the face's list, its row the face's (phase 1's arguments, and its instances from the list's start).
				if (frame.tree.valid() && frame.treeGroups) {
					std::uint32_t words[kDrawPushWords]{ static_cast<std::uint32_t>(a_prepared.treeRows[f]), static_cast<std::uint32_t>(a_prepared.treeRows[f] >> 32), 0u };
					commands.BindPipeline(frame.tree);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
					commands.ExecuteIndirect(frame.treeSignature, a_recording.Resolve(a_bindings.treeVisible[f]).GetHandle(), offsetof(TreeLod::VisibleHeader, phaseOne), {},
						0, 1);
				}
				commands.EndPass();
			}
		}

	private:
		std::shared_ptr<ReflectionResources> resources;
	};

	/**
	 * @brief Registers a graph resource under a_id. A versioned buffer is not registered: its passes declare it (its resolver),
	 * and a registration would hold the version registered for as long as the graph, where a version should go when nothing uses
	 * it. Nothing looks these identifiers up.
	 */
	template <class T>
	void Register(org::RenderGraph& a_graph, org::ResourceIdentifier a_id, const std::shared_ptr<T>& a_resource)
	{
		if constexpr (!std::is_same_v<T, org::VersionedBuffer>)
			a_graph.RegisterResource(std::move(a_id), a_resource);
	}

	/** @brief The scene tables, which both extensions register (the same identifiers: the second registration is an update). */
	void RegisterSceneBuffers(org::RenderGraph& a_graph, const SceneBuffers& a_scene)
	{
		Register(a_graph, org::ResourceIdentifier("cs.dclf.objects"), a_scene.objects);
		if (a_scene.pool) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.index-pool"), a_scene.pool->indices);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pool-firsts"), a_scene.pool->firsts);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pool-copies"), a_scene.pool->copies);
		}
		Register(a_graph, org::ResourceIdentifier("cs.dclf.bones"), a_scene.bones);
		Register(a_graph, org::ResourceIdentifier("cs.dclf.geometries"), a_scene.geometries);
		Register(a_graph, org::ResourceIdentifier("cs.dclf.face-positions"), a_scene.facePositions);
		if (a_scene.trees) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.trees"), a_scene.trees);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-clocks"), a_scene.treeClocks);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-wind0"), a_scene.treeWindRows[0]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-wind1"), a_scene.treeWindRows[1]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-frame"), a_scene.treeFrameBuffer);
		}
		if (a_scene.fadeRoots) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-roots"), a_scene.fadeRoots);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-states"), a_scene.fadeStates);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-states-out0"), a_scene.fadeStatesOut[0]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-states-out1"), a_scene.fadeStatesOut[1]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-frame"), a_scene.fadeFrameBuffer);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-visibility"), a_scene.fadeVisibility);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-log"), a_scene.fadeLog);
			if (a_scene.fadeEvents) {
				Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-events"), a_scene.fadeEvents);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.fade-reported"), a_scene.fadeReported);
			}
		}
		if (a_scene.treeLodCull) {
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-shapes"), a_scene.treeLodShapes);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-instances"), a_scene.treeLodInstances);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-meshes"), a_scene.treeLodMeshes);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-draw"), a_scene.treeLodDraw);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.tree-lod-visible"), a_scene.treeLodVisible);
		}
	}

	class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit ShadowExtension(std::shared_ptr<ShadowResources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.material-rows"), resources->materialRows.buffer);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-inputs{}", m)), resources->inputs[m]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.shadow.view-blocks"), resources->viewBlocks.buffer);
			for (std::size_t s = 0; s < resources->sequences.size(); ++s) {
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.sequences{}", s)), resources->sequences[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-count{}", s)), resources->count[s]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.bucket-counts{}", s)), resources->bucketCounts[s]);
			}
			for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
				if (resources->depth[i])
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.shadow.depth{}", i)), resources->depth[i]);
			}
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			const auto epoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::ShadowView);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.shadow.latched-copies",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowLatchedCopiesPass>(resources, false)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.shadow.index-pool",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources->pool, [shadow = resources] {
					const auto frame = CurrentShadowFrame(*shadow, false);
					return IndexPoolPass::LatchOf{ frame ? frame->generation : 0, frame ? frame->latch : nullptr, shadow->latchLayout.PoolOffset() };
				})))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.shadow.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowBuildDrawsPass>(resources, false)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.shadow.view",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowViewPass>(resources, false)))
					.Epoch(epoch));
			// Skylighting's occlusion map: the same passes over its one view, in its own epoch after RenderMask.
			const auto skyEpoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::SkyOcclusion);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.sky.latched-copies",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowLatchedCopiesPass>(resources, true)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(skyEpoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.sky.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowBuildDrawsPass>(resources, true)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(skyEpoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.sky.view",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowViewPass>(resources, true)))
					.Epoch(skyEpoch));
		}

	private:
		std::shared_ptr<ShadowResources> resources;
	};

	/** @brief The buffers a main commit's per-frame values go to through the latch (LatchedUploads), as the passes declare them. */
	/** @brief A target a commit may latch: a buffer, or a versioned buffer (copied into the version its preparation resolved). */
	struct LatchedTarget
	{
		std::shared_ptr<org::Buffer> buffer;
		Versioned versioned;
		const void* Key() const { return versioned ? versioned->Key() : buffer.get(); }
	};

	std::vector<LatchedTarget> MainLatchedTargets(const Resources& a_resources)
	{
		std::vector<LatchedTarget> out;
		for (const auto& buffer : { a_resources.frameConstants, a_resources.count })
			if (buffer)
				out.push_back({ buffer, nullptr });
		for (const auto& counts : a_resources.zBucketCounts)
			if (counts)
				out.push_back({ nullptr, counts });
		for (const auto& frameBuffer : a_resources.frameBuffers)
			if (frameBuffer.copy)
				out.push_back({ frameBuffer.copy, nullptr });
		// Tree LOD's draw row and its list's header; not the trees' or the fades' frame rows, which the compute queue reads.
		if (const auto& scene = *a_resources.scene; scene.treeLodCull) {
			out.push_back({ scene.treeLodDraw, nullptr });
			out.push_back({ nullptr, scene.treeLodVisible });
		}
		return out;
	}

	struct MainLatchedCopiesBindings
	{
		std::vector<std::pair<const void*, org::ResourceBindingToken>> targets;  // LatchedTarget::Key
	};

	struct MainLatchedCopiesPrepared
	{
		std::shared_ptr<const PassFrame> frame;
	};

	/**
	 * @brief A main epoch's first pass: the copies of its commit's per-frame values (PassFrame::latched) from the latch to where its
	 * passes read them. Recorded with the epoch's ticket, on ORG's host thread: the commit records no copy for them. It declares
	 * every target a commit may latch (MainLatchedTargets) and publishes them (Resources::latchedTargets), so a commit latches only
	 * what a pass will copy.
	 */
	class MainLatchedCopiesPass final : public org::TypedRenderGraphPass<MainLatchedCopiesPass, MainLatchedCopiesPrepared, MainLatchedCopiesBindings>
	{
	public:
		MainLatchedCopiesPass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment) :
			resources(std::move(a_resources)), segment(a_segment) {}

		MainLatchedCopiesBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			MainLatchedCopiesBindings bindings{};
			auto declared = std::make_shared<std::vector<const void*>>();
			for (const auto& target : MainLatchedTargets(*resources)) {
				bindings.targets.emplace_back(target.Key(), target.versioned ? a_builder.CopyDestination(*target.versioned).Resource() : a_builder.CopyDestination(target.buffer));
				declared->push_back(target.Key());
			}
			resources->latchedTargets.store(std::move(declared), std::memory_order_release);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(*resources, segment);
			a_out.push_back(frame ? frame->generation : 0);
		}

		MainLatchedCopiesPrepared Prepare(const MainLatchedCopiesBindings&, const org::PassPrepareContext&) const
		{
			MainLatchedCopiesPrepared prepared{};
			if (auto frame = CurrentFrame(*resources, segment); frame && frame->latched.latch && !frame->latched.copies.empty())
				prepared.frame = std::move(frame);
			return prepared;
		}

		static void Record(const MainLatchedCopiesBindings& a_bindings, const MainLatchedCopiesPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& list = a_prepared.frame->latched;
			auto& commands = a_recording.Commands();
			const auto latch = list.latch->Resource()->GetAPIResource().GetHandle();
			const std::uint64_t region = list.latch->Offset(a_recording.FrameSlot());
			for (const auto& copy : list.copies) {
				const auto target = std::find_if(a_bindings.targets.begin(), a_bindings.targets.end(), [&](const auto& a_target) { return a_target.first == copy.target; });
				if (target == a_bindings.targets.end())
					continue;  // latched only for a declared target (LatchedUploads)
				commands.CopyBufferRegion(a_recording.Resolve(target->second).GetHandle(), copy.dstOffset, latch, region + copy.latchOffset, copy.bytes);
			}
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
	};

	class MainOpaqueExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit MainOpaqueExtension(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			Register(a_graph, org::ResourceIdentifier("cs.dclf.material-rows"), resources->materialRows.buffer);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.pipeline-rows"), resources->pipelineRows.buffer);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.sequences"), resources->sequences);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-inputs"), resources->inputs);
			if (resources->inputsDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-inputs-depth"), resources->inputsDepth);
			RegisterSceneBuffers(a_graph, *resources->scene);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.draw-count"), resources->count);
			// Read through device addresses (the frame record's slots), declared by their readers so the graph orders them after
			// the epochs' latched copies.
			Register(a_graph, org::ResourceIdentifier("cs.dclf.frame-constants"), resources->frameConstants);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.z.bucket-counts"), resources->zBucketCounts[0]);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.z.bucket-counts2"), resources->zBucketCounts[1]);
			if (const auto& foliage = resources->foliage) {
				for (std::uint32_t h = 0; h < 2; ++h) {
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.foliage-ids{}", h)), foliage->ids[h]);
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.foliage-colours{}", h)), foliage->colours[h]);
				}
				Register(a_graph, org::ResourceIdentifier("cs.dclf.foliage-results"), foliage->results);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.foliage-owners"), foliage->owners);
			}
			Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility"), resources->visibility);
			if (resources->frustum)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.frustum"), resources->frustum);
			for (const auto& frameBuffer : resources->frameBuffers)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
			for (std::uint32_t i = 0; i < resources->targetCount; ++i)
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
			if (resources->nativeDepth)
				Register(a_graph, org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
			if (resources->hzb) {
				Register(a_graph, org::ResourceIdentifier("cs.dclf.hzb"), resources->hzb);
				Register(a_graph, org::ResourceIdentifier("cs.dclf.hzb-counter"), resources->hzbCounter);
			}
			if (resources->sort)
				resources->sort->Register(a_graph);
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			using Segment = RenderGraphRuntime::Segment;
			const auto colour = RenderGraphRuntime::EpochOf(Segment::MainOpaque);
			const auto depth = RenderGraphRuntime::EpochOf(Segment::ZPrepass);
			const auto colourSegment = Segment::MainOpaque;
			const auto depthSegment = Segment::ZPrepass;
			// Each epoch's first pass: its commit's per-frame values, from the latch (MainLatchedCopiesPass).
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.z.latched-copies",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<MainLatchedCopiesPass>(resources, depthSegment)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(depth));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.latched-copies",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<MainLatchedCopiesPass>(resources, colourSegment)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(colour));
			// A pass instance runs only in its own epoch and its segment is fixed here, so the Z-prepass has its own
			// build-draws and draw pass.
			// The sort by pipeline after a build (SortDraws): the counts' prefix sum, then the scatter. The scan runs in every
			// execution, so the counts it clears are zero whether or not a build ran; the scatter only after a build.
			const auto addSort = [&](const char* a_scan, const char* a_scatter, RenderGraphRuntime::Segment a_segment, auto a_epoch) {
				if (!resources->sort)
					return;
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scan, resources->sort->ScanPass())
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(a_epoch));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scatter,
					std::static_pointer_cast<org::RenderPass>(std::make_shared<SortSequencesPass>(resources, a_segment)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(a_epoch));
			};
			// The trees' wind for the next frame, beside this epoch's raster work.
			if (resources->scene->treeWind && resources->scene->trees)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.tree-wind",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<TreeWindPass>(resources->scene)))
						.PreferQueue(org::QueueKind::Compute)
						.Epoch(depth));
			// The fade roots' state for the next frame's culling, beside this epoch's raster work.
			if (resources->scene->fadeState && resources->scene->fadeRoots)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.fade-state",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<FadeStatePass>(resources)))
						.PreferQueue(org::QueueKind::Compute)
						.Epoch(depth));
			// Its write-back's events, to the host (FadeWriteBack).
			if (resources->scene->fadeState && resources->scene->fadeRoots && resources->scene->fadeEvents && resources->scene->fadeWriteBack)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.z.fade-events",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<FadeEventReadbackPass>(resources->scene)))
						.Epoch(depth));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(depth));
			// The pool's copies the depth commit gave out (in a frame whose shadow epoch has not already), before the builds name
			// the ranges and the draws read them.
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.index-pool",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<IndexPoolPass>(resources->pool, [main = resources] {
					const auto frame = CurrentFrame(*main, RenderGraphRuntime::Segment::ZPrepass);
					return IndexPoolPass::LatchOf{ frame ? frame->generation : 0, frame ? frame->latch : nullptr, main->latchLayout.PoolOffset() };
				})))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(depth));
			// Tree LOD's list, phase 1's, before the depth draw that draws it.
			if (resources->scene->treeLodCull)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.tree-lod-cull",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<TreeLodCullPass>(resources, 1)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(depth));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.z.depth",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment)))
					.Epoch(depth));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, colourSegment)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(colour));
			addSort("cs.dclf.sort-scan", "cs.dclf.sort-scatter", colourSegment, colour);
			if (resources->probe)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-before",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, false)))
						.Epoch(colour));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.main-opaque",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, colourSegment)))
					.Epoch(colour));
			if (resources->probe)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-after",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, true)))
						.Epoch(colour));
			if (resources->foliage) {
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.foliage-parity",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<FoliageParityPass>(resources)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(colour));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.foliage-readback",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<FoliageReadbackPass>(resources)))
						.Epoch(colour));
			}
			// The two-phase tail, all of it inside the depth segment and all of it in this order:
			//
			//   build-draws (phase 1, against the HZB the previous frame left)
			//   main-opaque (the phase-1 depth draw)
			//   hzb         (rebuilt from the depth that draw has just finished)
			//   build-draws-phase2 (phase 1's rejects, re-tested against the rebuilt HZB)
			//   tree-lod-cull-phase2 (tree LOD's, the same)
			//   depth-phase2       (the rescues, so their depth is in the frame too)
			//
			// Phase 1 tests against a depth buffer that is a frame old, which is what makes it cheap and
			// also what makes it wrong at the edges: anything that has just come out from behind an
			// occluder was hidden in that buffer. Phase 2 exists to take those back, after a rebuild
			// that can see them.
			if (resources->hzb) {
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.hzb",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<HzbPass>(resources, depthSegment)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(depth));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws-phase2",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment, 2)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(depth));
				if (resources->scene->treeLodCull)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.tree-lod-cull-phase2",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<TreeLodCullPass>(resources, 2)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depth));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.depth-phase2",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment, true)))
						.Epoch(depth));
			}
			// Tree LOD's cull counts, after both phases.
			if (resources->scene->treeLodCull && resources->scene->treeLodCounts)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.z.tree-lod-counts",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<TreeLodReadbackPass>(resources->scene)))
						.Epoch(depth));
			if (resources->probe)
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.z.probe-after",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, depthSegment, true)))
						.Epoch(depth));
			if (resources->probe) {
				for (const auto segment : { Segment::SkyOcclusion, Segment::LightCulling })
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy(segment == Segment::SkyOcclusion ? "cs.dclf.sky.probe-depth" : "cs.dclf.light.probe-depth",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, segment, false)))
							.Epoch(RenderGraphRuntime::EpochOf(segment)));
			}
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources)
	{
		return std::make_unique<MainOpaqueExtension>(std::move(a_resources));
	}

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources)
	{
		return std::make_unique<ShadowExtension>(std::move(a_resources));
	}

	struct ReflectionLatchedCopiesBindings
	{
		org::ResourceBindingToken count, faceBlocks;
		std::array<org::ResourceBindingToken, kReflectionFaces> bucketCounts{}, treeRows{}, treeVisible{};
		bool trees = false;
	};

	struct ReflectionLatchedCopiesPrepared
	{
		std::shared_ptr<const ReflectionFrame> frame;
	};

	/**
	 * @brief The reflection commit's per-update values, copied from the latch to where the faces read them: each face's block, its
	 * tree LOD row and visible list header (ReflectionLatchLayout::FaceOffset), and the draw count and bucket counts zeroed. Recorded
	 * with the epoch's ticket on ORG's host thread: the commit records no copy.
	 */
	class ReflectionLatchedCopiesPass final : public org::TypedRenderGraphPass<ReflectionLatchedCopiesPass, ReflectionLatchedCopiesPrepared, ReflectionLatchedCopiesBindings>
	{
	public:
		explicit ReflectionLatchedCopiesPass(std::shared_ptr<ReflectionResources> a_resources) :
			resources(std::move(a_resources)) {}

		ReflectionLatchedCopiesBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ReflectionLatchedCopiesBindings bindings{};
			bindings.count = a_builder.CopyDestination(resources->count);
			bindings.faceBlocks = a_builder.CopyDestination(resources->faceBlocks);
			bindings.trees = resources->main->scene->treeLodCull && resources->treeVisible[0];
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				bindings.bucketCounts[f] = a_builder.CopyDestination(*resources->bucketCounts[f]).Resource();
				if (bindings.trees) {
					bindings.treeRows[f] = a_builder.CopyDestination(resources->treeRows[f]);
					bindings.treeVisible[f] = a_builder.CopyDestination(*resources->treeVisible[f]).Resource();
				}
			}
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentReflectionFrame(*resources);
			a_out.push_back(frame ? frame->generation : 0);
		}

		ReflectionLatchedCopiesPrepared Prepare(const ReflectionLatchedCopiesBindings&, const org::PassPrepareContext&) const
		{
			ReflectionLatchedCopiesPrepared prepared{};
			if (auto frame = CurrentReflectionFrame(*resources); frame && frame->latch && frame->zeros)
				prepared.frame = std::move(frame);
			return prepared;
		}

		static void Record(const ReflectionLatchedCopiesBindings& a_bindings, const ReflectionLatchedCopiesPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			const auto latch = frame.latch->Resource()->GetAPIResource().GetHandle();
			const auto zeros = frame.zeros->Resource()->GetAPIResource().GetHandle();
			const std::uint64_t faces = frame.latch->Offset(a_recording.FrameSlot()) + frame.facesOffset;
			commands.CopyBufferRegion(a_recording.Resolve(a_bindings.count).GetHandle(), 0, zeros, 0, sizeof(kZeroCounts));
			const auto faceBlocks = a_recording.Resolve(a_bindings.faceBlocks).GetHandle();
			const std::uint64_t bucketBytes = std::max<std::size_t>(frame.buckets.size(), 1) * sizeof(std::uint32_t);
			const bool trees = a_bindings.trees && frame.tree.valid();
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				commands.CopyBufferRegion(a_recording.Resolve(a_bindings.bucketCounts[f]).GetHandle(), 0, zeros, 0, bucketBytes);
				const std::uint64_t face = faces + std::uint64_t(f) * ReflectionLatchLayout::kFaceBytes;
				commands.CopyBufferRegion(faceBlocks, std::uint64_t(f) * kReflectionFaceBlockBytes, latch, face, kReflectionFaceBlockBytes);
				if (trees) {
					commands.CopyBufferRegion(a_recording.Resolve(a_bindings.treeRows[f]).GetHandle(), 0, latch, face + ReflectionLatchLayout::kTreeRowInFace,
						sizeof(TreeLod::DrawRow));
					commands.CopyBufferRegion(a_recording.Resolve(a_bindings.treeVisible[f]).GetHandle(), 0, latch, face + ReflectionLatchLayout::kTreeHeaderInFace,
						sizeof(TreeLod::VisibleHeader));
				}
			}
		}

	private:
		std::shared_ptr<ReflectionResources> resources;
	};

	class ReflectionExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit ReflectionExtension(std::shared_ptr<ReflectionResources> a_resources) :
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
			Register(a_graph, org::ResourceIdentifier("cs.dclf.reflection.sequences"), resources->sequences);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.reflection.draw-count"), resources->count);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.reflection.face-blocks"), resources->faceBlocks);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.reflection.depth"), resources->depth);
			Register(a_graph, org::ResourceIdentifier("cs.dclf.reflection.cube"), resources->cube);
			for (std::uint32_t f = 0; f < kReflectionFaces; ++f) {
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.reflection.bucket-counts{}", f)), resources->bucketCounts[f]);
				Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.reflection.tree-row{}", f)), resources->treeRows[f]);
				if (resources->treeVisible[f])
					Register(a_graph, org::ResourceIdentifier(fmt::format("cs.dclf.reflection.tree-visible{}", f)), resources->treeVisible[f]);
			}
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			const auto epoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::Reflection);
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.reflection.latched-copies",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ReflectionLatchedCopiesPass>(resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.reflection.build-draws",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ReflectionBuildDrawsPass>(resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(epoch));
			if (resources->main->scene->treeLodCull && resources->treeVisible[0])
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.reflection.tree-lod-cull",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ReflectionTreeCullPass>(resources)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.reflection.faces",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<ReflectionDrawPass>(resources)))
					.Epoch(epoch));
		}

	private:
		std::shared_ptr<ReflectionResources> resources;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeReflectionExtension(std::shared_ptr<ReflectionResources> a_resources)
	{
		return std::make_unique<ReflectionExtension>(std::move(a_resources));
	}
}

#endif
