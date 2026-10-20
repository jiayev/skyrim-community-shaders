// CS_DCLF_FOLIAGE_PARITY: the colour pass's alpha-tested draws' pixels (the foliage record, Policy::RecordFoliagePixels) against the frame
// before's, reprojected, and within the frame against the Z-prepass's owners (FoliageParityCS.hlsl): the compare pass and its readback,
// appended after the colour draws, and the report of each frame's results.

#include "DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	constexpr const char* kFoliageParityShader = "DrawcallLimitFix/FoliageParityCS.hlsl";
	struct FoliageParityConstants
	{
		// Both pairs (by the epoch's parity): which is this frame's is the frame block's, as the colour pass read it.
		std::uint64_t ids[2]{}, colours[2]{};
		std::uint64_t frameBlock = 0;    // the colour epoch's words in the frame lighting block (PS b13, c14: DCLFFoliageParity)
		std::uint32_t resultsIndex = 0;  // the results' UAV (RWByteAddressBuffer)
		std::uint32_t ownersIndex = 0;   // the Z-prepass's owners (RWStructuredBuffer<uint64_t>), cleared as read
		std::uint32_t depthIndex = 0;    // the main depth (SRV)
		std::uint32_t padding0 = 0;
		std::uint32_t width = 0, height = 0, padding1 = 0, padding2 = 0;
	};
	constexpr std::uint32_t kFoliageParityConstantWords = sizeof(FoliageParityConstants) / 4;

	// One frame's results (GpuLayouts.h, FoliageCounter), read back (the readback pass's recording, on ORG's host thread).
	void FoliageParityReport(std::uint32_t a_frame, const std::uint32_t* a_results)
	{
		auto& a_foliage = DrawsState::Get().foliage;
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
							const auto& tables = Scene::FrameView::Get().GetTables();
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
				const auto& tables = Scene::FrameView::Get().GetTables();
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
		auto& landFrames = a_foliage.landFrames;
		auto& landMax = a_foliage.landMax;
		auto& landLogged = a_foliage.landLogged;
		if (const std::uint32_t lost = a_results[kFoliageLandUnshaded] + a_results[kFoliageLandOtherObject]) {
			++landFrames;
			landMax = std::max(landMax, lost);
		}
		if (a_results[kFoliageLandUnshaded] >= 32 && landLogged < 40) {
			++landLogged;
			const auto& tables = Scene::FrameView::Get().GetTables();
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
	 * @brief The compare pass (FoliageParityCS.hlsl), after the colour pass: this frame's pixels of the
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

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
			a_out.push_back(frame ? frame->generation : 0);
		}

		FoliageParityPrepared Prepare(const FoliageParityBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
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
			commands.BindPipeline(a_frame.program->Pipeline());
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
		std::shared_ptr<Resources::FoliageRecord> foliage;
	};

	/**
	 * @brief The compare pass's results, copied to the frame slot's host buffer; what that buffer held from the slot's last
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

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
			a_out.push_back(frame ? frame->generation : 0);
		}

		FoliageReadbackPrepared Prepare(const FoliageReadbackBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			FoliageReadbackPrepared prepared{};
			const auto frame = CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque);
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
					FoliageParityReport(results[kFoliageEpochTag], results);
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

	void MakeFoliageCompare(const FoliageRecordMade& a_made)
	{
		// The compare pass's program and a host copy of its results per frame slot; the record stands only with them.
		auto& foliage = a_made.record;
		foliage.program = ComputeProgram::Load(a_made.device, { .source = kFoliageParityShader, .constantWords = kFoliageParityConstantWords });
		for (std::uint32_t i = 0; i < a_made.slots; ++i)
			foliage.readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(kFoliageResultWords) * sizeof(std::uint32_t)));
		foliage.readbackFrame.assign(a_made.slots, 0u);
		if (!foliage.program || !foliage.resultsAddress || !foliage.idsAddress[0] || !foliage.idsAddress[1] || !foliage.coloursAddress[0] ||
			!foliage.coloursAddress[1]) {
			logger::warn("[DCLF] foliage parity: its program or buffers could not be created; off");
			a_made.ready = false;
		} else {
			logger::info("[DCLF] foliage parity on: {}x{}, {} frame slots, tree wind {}", foliage.width, foliage.height, a_made.slots,
				Observe().foliageFrozen ? "frozen" : "running");
			a_made.ready = true;
		}
	}

	void AppendFoliagePasses(const MainGraphPasses& a_passes)
	{
		const auto& resources = a_passes.resources;
		if (a_passes.point != MainGraphPasses::kAfterColour || !resources->foliage)
			return;
		const auto colour = a_passes.epoch;
		auto& a_out = a_passes.out;
		a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.foliage-parity",
			std::static_pointer_cast<org::RenderPass>(std::make_shared<FoliageParityPass>(resources)))
				.PreferQueue(org::QueueKind::Graphics)
				.Epoch(colour));
		a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.foliage-readback",
			std::static_pointer_cast<org::RenderPass>(std::make_shared<FoliageReadbackPass>(resources)))
				.Epoch(colour));
	}
}
