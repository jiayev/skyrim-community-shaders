// The main opaque pass: DCLF's draws in the Z-prepass (depth, both phases: main-view.z.depth, main-view.depth-phase2) and the main colour
// pass (main-view.colour), and the draw families' draws (Api/DrawFamilies.h: tree LOD's) and the main ranges' (Api/MainRanges.h).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/MainView/MainView.h"
#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/DrawFamilies/FamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include "Features/RemoteControl/NvPerf.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	// In DCLF::Draws, where it was defined before the feature moved: ORG's recipe telemetry names a pass by its type (typeid).

	namespace
	{
		/** @brief A draw family's draw in this execution (Api::DrawFamilyContribution::main.record), for a family prepared with its pipelines. */
		void RecordFamily(std::size_t a_family, const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording,
			rhi::CommandList& a_commands, Api::DrawFamilyPass a_pass)
		{
			const auto& drawn = a_prepared.families[a_family];
			Extensions::DrawFamilies()[a_family].contribution.main.record(
				{ a_commands, a_pass, drawn.drawn, drawn.row, a_recording.Resolve(a_bindings.familyArguments[a_family]).GetHandle(), {}, {} });
		}

		/**
		 * @brief What MainOpaquePass::Record's stages share: the frame, the resolved sequences, count and state class counts (the colour
		 * pass's), the depth pass setup, the stats slot.
		 */
		struct MainRecording
		{
			const PassFrame& frame;
			rhi::CommandList& commands;
			rhi::ResourceHandle sequences;
			rhi::ResourceHandle count;
			rhi::ResourceHandle classCounts;
			const rhi::PassBeginInfo& begin;  // depth attached, no colours
			const rhi::DepthAttachment& depth;
			PassStats* stats;
			std::uint32_t statsSlot;
		};

		/**
		 * @brief The Z-prepass's draws: plain indexed indirect draws (DCLF_PULLED), one per bucket over its range of the phase's
		 * sequences (phase 2's past sequenceDraws), with push words kZDrawPush* and the bucket's count word. A plain draw cannot bind
		 * its own vertex buffer or push data, so the vertex stage fetches through the sequence its instance names (Lighting.hlsl).
		 */
		void RecordDepthDraws(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass)
		{
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			const auto& begin = a_pass.begin;
			auto* stats = a_pass.stats;
			const std::uint32_t statsSlot = a_pass.statsSlot;
			const auto depthKind = a_prepared.phaseTwo ? PassStats::kDepthPhaseTwo : PassStats::kDepth;
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
			auto drawCall = [&](const PassFrame::ZCall& a_call, bool a_offsetDepth) {
				RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesAddress, phaseBase + a_call.first, a_call.pipeline, counts, a_call.bucket,
					a_call.capacity, a_offsetDepth);
			};
			// A terrain group 1 twin (near blocks) draws into the main depth with Terrain Blending's offset, as its depth hook
			// does (TerrainBlending::TerrainShaderHacks, OFFSET_DEPTH).
			bool terrainNear = false;
			for (const auto& call : frame.zCalls) {
				drawCall(call, call.terrainNear);
				terrainNear = terrainNear || call.terrainNear;
			}
			// The draw families' depth (tree LOD's: one instanced draw of the phase's culled records).
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (a_prepared.families[f].drawn)
					RecordFamily(f, a_bindings, a_prepared, a_recording, commands, a_prepared.phaseTwo ? Api::DrawFamilyPass::DepthPhaseTwo : Api::DrawFamilyPass::Depth);
			if (stats)
				stats->End(commands, statsSlot, depthKind);
			commands.EndPass();
			// The twins again without the offset into Terrain Blending's terrain depth (its first draw of a near block), which
			// TerrainBlending::BlendPrepassDepths reads after this epoch.
			if (terrainNear && a_prepared.terrainDepth) {
				rhi::DepthAttachment terrainDepth = a_pass.depth;
				terrainDepth.dsv = a_recording.Resolve(a_prepared.terrainDepthView);
				rhi::PassBeginInfo terrainBegin = begin;
				terrainBegin.depth = &terrainDepth;
				terrainBegin.debugName = "DCLF terrain depth";
				commands.BeginPass(terrainBegin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.zLayout);
				commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				commands.SetIndexBuffer(rhi::IndexBufferView{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt });
				for (const auto& call : frame.zCalls)
					if (call.terrainNear)
						drawCall(call, false);
				commands.EndPass();
			}
			if (stats)
				stats->Resolve(commands, statsSlot, depthKind);
		}

		/** @brief Begins a draw pass identically on the command list and on a preprocess state list. */
		void BeginDrawPass(rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin, const PassFrame& a_frame, const PreparedDraws& a_prepared)
		{
			a_list.BeginPass(a_begin);
			a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
			a_list.BindLayout(a_frame.indirect.layout);
			a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
		}

		/**
		 * @brief The colour segment's passes (every target loaded; the decal depth pass depth only) and its calls: per range, a call per
		 * state class with a sub-range there (PassFrame::classes, in creation order), each with its class's signature, so an execution set
		 * holds pipelines of one fixed-function state alone.
		 */
		struct ColourPasses
		{
			const PassFrame& frame;
			rhi::PassBeginInfo mainBegin;
			rhi::PassBeginInfo decalBegin;
			rhi::PassBeginInfo decalDepthBegin;
			rhi::ResourceHandle count, classCounts;  // the decal groups' count words; every other sub-range's (ClassCountWord)
			bool anyDecals = false;  // any range of the second pass drawn
			bool anyDecalDepth = false;  // any depth-prepass range drawn

			/**
			 * @brief Class a_class's call of range a_range (GpuLayouts.h, kClassRangeMain) with a_variant's signature: its sub-range, and its
			 * count, a decal group's word (shared by the group's classes, every slot below it written in each) or the class's own.
			 */
			IndirectCall Call(std::uint32_t a_class, std::uint32_t a_range, std::uint32_t a_variant) const
			{
				const auto& range = frame.classes[a_class].ranges[a_range];
				const bool decal = a_range >= ClassRangeOfSlot(0) && a_range < ClassRangeOfSlot(kDecalGroups);
				const std::uint32_t word = decal ? DecalCountWord(a_range - ClassRangeOfSlot(0)) : ClassCountWord(a_class, a_range);
				return { frame.indirect.classes[a_class].signatures[a_variant], std::uint64_t(range.first) * sizeof(DrawSequence), decal ? count : classCounts,
					std::uint64_t(word) * sizeof(std::uint32_t), range.capacity };
			}
			// Each class's call of range a_range, for the classes with a sub-range there.
			template <class Each>
			void RangeCalls(std::uint32_t a_range, std::uint32_t a_variant, Each&& a_each) const
			{
				for (std::uint32_t c = 0; c < frame.classes.size() && c < frame.indirect.classes.size(); ++c)
					if (frame.classes[c].ranges[a_range].capacity)
						a_each(Call(c, a_range, a_variant));
			}
			// Whether a second-pass range slot has any class's sub-range.
			bool RangeDrawn(std::uint32_t a_slot) const
			{
				bool drawn = false;
				RangeCalls(ClassRangeOfSlot(a_slot), kColorVariant, [&](const IndirectCall&) { drawn = true; });
				return drawn;
			}
			// The main draws' calls.
			template <class Each>
			void MainDraws(Each&& a_each) const
			{
				RangeCalls(kClassRangeMain, kColorVariant, a_each);
			}
			// The second pass's calls in the ranges' order (Extensions::MainRanges: opaque decals and multi-index layers, Terrain
			// Blending's terrain and the meshes it redraws - its replay follows the engine's opaque groups - then blended decals), each
			// range's classes in order, with the range's nvperf name.
			template <class Each>
			void SecondPass(Each&& a_each) const
			{
				for (const auto& range : Extensions::MainRanges().drawn)
					RangeCalls(ClassRangeOfSlot(range.slot), kColorVariant, [&](const IndirectCall& a_call) { a_each(a_call, range.colourRange.c_str()); });
			}
			// The depth prepass's calls, in the same order: the ranges that draw one (opaque decals), with their classes' depth signatures.
			template <class Each>
			void DepthPrepass(Each&& a_each) const
			{
				for (const auto& range : Extensions::MainRanges().drawn)
					if (range.depthPrepass)
						RangeCalls(ClassRangeOfSlot(range.slot), kDepthVariant, a_each);
			}
		};

		void PreprocessCall(rhi::CommandList& a_commands, rhi::CommandList& a_state, rhi::ResourceHandle a_sequences, const IndirectCall& a_call)
		{
			a_commands.PreprocessIndirect(a_state, a_call.signature, a_sequences, a_call.argumentsOffset, a_call.counts, a_call.countOffset, a_call.capacity);
		}
		void ExecuteCall(rhi::CommandList& a_commands, rhi::ResourceHandle a_sequences, const IndirectCall& a_call)
		{
			a_commands.ExecuteIndirect(a_call.signature, a_sequences, a_call.argumentsOffset, a_call.counts, a_call.countOffset, a_call.capacity);
		}

		/**
		 * @brief Every colour call preprocessed before the first pass: generated inside the pass by NVIDIA's driver, each colour call
		 * idles the GPU for a fixed time in these eight-target passes. The state list must match the execution exactly (same pass
		 * setup and heaps); sequences and counts are final (the culling wrote them). Nothing without explicit preprocessing.
		 */
		void RecordColourPreprocess(const PreparedDraws& a_prepared, const MainRecording& a_pass, const ColourPasses& a_passes)
		{
			if (!a_prepared.preprocess)
				return;
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			// The frame slot's preprocess state list.
			auto& state = a_prepared.preprocess->Begin(a_pass.statsSlot);
			state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			if (a_passes.anyDecalDepth) {
				BeginDrawPass(state, a_passes.decalDepthBegin, frame, a_prepared);
				a_passes.DepthPrepass([&](const IndirectCall& a_call) { PreprocessCall(commands, state, sequences, a_call); });
				state.EndPass();
			}
			BeginDrawPass(state, a_passes.mainBegin, frame, a_prepared);
			if (!std::getenv("CS_DCLF_DECALS_ONLY"))  // TEMPORARY (decal loss)
				a_passes.MainDraws([&](const IndirectCall& a_call) { PreprocessCall(commands, state, sequences, a_call); });
			state.EndPass();
			if (a_passes.anyDecals) {
				BeginDrawPass(state, a_passes.decalBegin, frame, a_prepared);
				a_passes.SecondPass([&](const IndirectCall& a_call, const char*) { PreprocessCall(commands, state, sequences, a_call); });
				state.EndPass();
			}
			state.End();
		}

		/** @brief The colour passes' execution: decal depth, the main draws and the draw families' colour, then the second pass; nvperf ranges around the parts. */
		void RecordColourExecution(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass,
			const ColourPasses& a_passes, void* const a_nvCommands)
		{
			const auto& frame = a_pass.frame;
			auto& commands = a_pass.commands;
			const auto sequences = a_pass.sequences;
			auto* stats = a_pass.stats;
			const std::uint32_t statsSlot = a_pass.statsSlot;
			auto subRange = [&](const char* a_name) { return a_nvCommands && NvPerf::PushCommandBufferRange(a_nvCommands, a_name); };
			auto endSubRange = [&](bool a_pushed) { if (a_nvCommands) NvPerf::PopCommandBufferRange(a_nvCommands, a_pushed); };
			if (a_passes.anyDecalDepth) {
				const bool part = subRange("cs.dclf.colour.decal-depth");
				BeginDrawPass(commands, a_passes.decalDepthBegin, frame, a_prepared);
				a_passes.DepthPrepass([&](const IndirectCall& a_call) { ExecuteCall(commands, sequences, a_call); });
				commands.EndPass();
				endSubRange(part);
			}
			if (stats)
				stats->Start(commands, statsSlot, PassStats::kColour);
			const bool mainPart = subRange("cs.dclf.colour.main");
			BeginDrawPass(commands, a_passes.mainBegin, frame, a_prepared);
			if (stats)
				stats->Begin(commands, statsSlot, PassStats::kColour);
			{
				const bool drawPart = subRange("cs.dclf.colour.main-draws");
				if (!std::getenv("CS_DCLF_DECALS_ONLY"))  // TEMPORARY (decal loss)
					a_passes.MainDraws([&](const IndirectCall& a_call) { ExecuteCall(commands, sequences, a_call); });
				endSubRange(drawPart);
			}
			// A draw family's colour tests EQUAL against its Z-prepass draw, under the Z-prepass layout (same frame push data).
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (a_prepared.families[f].drawn && !std::getenv("CS_DCLF_DECALS_ONLY")) {  // TEMPORARY (decal loss)
					const bool familyPart = subRange(Extensions::DrawFamilies()[f].colourRange.c_str());
					commands.BindLayout(frame.indirect.zLayout);
					commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
					RecordFamily(f, a_bindings, a_prepared, a_recording, commands, Api::DrawFamilyPass::Colour);
					endSubRange(familyPart);
				}
			if (stats)
				stats->End(commands, statsSlot, PassStats::kColour);
			commands.EndPass();
			endSubRange(mainPart);
			if (stats)
				stats->Resolve(commands, statsSlot, PassStats::kColour);

			// The second pass: decals after every opaque draw in the engine's order, each group from its own range and count word,
			// LESS_EQUAL with the engine's decal bias; only the layers write depth here (opaque decal depth is already in). Before the
			// blended group, Terrain Blending's terrain (blend mode 1, LESS_EQUAL with depth writes, mask t55 in the frame record) and
			// its no-blend meshes (EQUAL), as that feature replays them (Deferred::Hooks::Main_RenderWorld_BlendedDecals).
			if (a_passes.anyDecals) {
				const bool decalPart = subRange("cs.dclf.colour.decals");
				if (stats)
					stats->Start(commands, statsSlot, PassStats::kDecals);
				BeginDrawPass(commands, a_passes.decalBegin, frame, a_prepared);
				if (stats)
					stats->Begin(commands, statsSlot, PassStats::kDecals);
				a_passes.SecondPass([&](const IndirectCall& a_call, const char* a_name) {
					const bool groupPart = subRange(a_name);
					ExecuteCall(commands, sequences, a_call);
					endSubRange(groupPart);
				});
				if (stats)
					stats->End(commands, statsSlot, PassStats::kDecals);
				commands.EndPass();
				if (stats)
					stats->Resolve(commands, statsSlot, PassStats::kDecals);
				endSubRange(decalPart);
			}
		}

		/**
		 * @brief The colour segment's draws. The main pass tests depth EQUAL against the Z-prepass and writes none.
		 * Opaque decal depth goes first: the engine draws its opaque decal group with depth writes and bias, so a host fragment
		 * under an opaque decal texel fails the colour EQUAL test (the decal overwrites every target there). The depth variants
		 * test LESS_EQUAL with the bias and alpha-test, so a transparent texel keeps the host's depth. Blended decals write no
		 * depth: their host must still be shaded.
		 */
		void RecordColourDraws(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording, const MainRecording& a_pass,
			std::array<rhi::ColorAttachment, kColorTargets>& a_colors)
		{
			const auto& frame = a_pass.frame;
			// nvperf ranges around the colour pass's parts, when a capture selects them.
			void* const nvCommands = NvPerf::CommandBufferRangesActive() ? rhi::vulkan::get_cmd_list(a_pass.commands) : nullptr;
			// The colour passes, every target loaded.
			ColourPasses passes{ .frame = frame, .mainBegin = a_pass.begin, .decalBegin = a_pass.begin, .decalDepthBegin = a_pass.begin, .count = a_pass.count,
				.classCounts = a_pass.classCounts };
			passes.mainBegin.colors = { a_colors.data(), a_prepared.targetCount };
			passes.mainBegin.debugName = "DCLF main opaque";
			passes.decalBegin.colors = { a_colors.data(), a_prepared.targetCount };
			passes.decalBegin.debugName = "DCLF decals";
			passes.decalDepthBegin.debugName = "DCLF decal depth";  // depth only
			// The second pass's ranges (Extensions::MainRanges) with any class's sub-range, and those of them with a depth prepass.
			for (const auto& range : Extensions::MainRanges().drawn)
				if (passes.RangeDrawn(range.slot)) {
					passes.anyDecals = true;
					passes.anyDecalDepth = passes.anyDecalDepth || range.depthPrepass;
				}

			RecordColourPreprocess(a_prepared, a_pass, passes);
			RecordColourExecution(a_bindings, a_prepared, a_recording, a_pass, passes, nvCommands);
		}
	}

	class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
	{
	public:
		// depth: the epoch records the main depth shape (the Z-prepass's draws; else the colour pass's). phaseTwo: the second depth draw, of
		// what the rebuilt HZB brought back (the sequence buffer's reserved part).
		MainOpaquePass(std::shared_ptr<Resources> a_resources, RenderGraphRuntime::Segment a_segment, bool a_depth, bool a_phaseTwo = false) :
			resources(std::move(a_resources)), segment(a_segment), depth(a_depth), phaseTwo(a_phaseTwo) {}

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
			if (depth) {
				// The plain draws (DCLF_PULLED) read sequences and the vertex stream (face positions) by address and index the pool.
				a_builder.ShaderResource(*resources->sequences, noViews);
				if (resources->scene->vertexStream)
					a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
				bindings.pool = a_builder.IndexBuffer(*resources->pool->indices);
				bindings.bucketCounts = a_builder.IndirectArguments(*resources->zBucketCounts[phaseTwo ? 1 : 0]).Resource();
				// Terrain Blending's terrain depth, drawn by the near terrain blocks' twin buckets.
				if (resources->terrainDepth)
					bindings.terrainDepth = a_builder.DepthReadWrite(resources->terrainDepth).View();
			} else {
				// The state class sub-ranges' counts the colour calls read (the decal groups' are the count buffer's).
				bindings.classCounts = a_builder.IndirectArguments(*resources->classCounts).Resource();
				// The stream objects' (face shapes') second vertex stream, which the vertex stages pull by address (DCLF_PULLED_VERTICES).
				if (resources->scene->vertexStream)
					a_builder.ShaderResource(*resources->scene->vertexStream, noViews);
				// The foliage record (Policy::RecordFoliagePixels): alpha-tested draws write per-pixel ids by address.
				if (const auto& foliage = resources->foliage) {
					const std::span<const org::UavView> noUavs{};
					for (std::uint32_t h = 0; h < 2; ++h) {
						a_builder.UnorderedAccess(foliage->ids[h], noUavs);
						a_builder.UnorderedAccess(foliage->colours[h], noUavs);
					}
				}
			}
			DeclareFrameBuffers(a_builder, *resources);
			a_builder.ShaderResource(resources->frameConstants, noViews);  // the frame record's slots, by address
			// The draw families' arguments, draw rows and the tables their vertex stages read by address (tree LOD's).
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (const auto& family = Extensions::DrawFamilies()[f]; family.taken) {
					Api::DrawFamilyDeclare declare{ a_builder, *resources->scene, nullptr, std::span(&bindings.familyArguments[f], 1) };
					family.contribution.main.declare(declare);
					bindings.familyRows[f] = declare.row;
				}
			return bindings;
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			const auto now = segment;
			const auto frame = CurrentFrame(a_preparation, now);
			a_out.push_back(frame ? frame->generation : 0);
			a_out.push_back(static_cast<std::uint64_t>(now));
			a_out.push_back(phaseTwo ? 1 : 0);
			a_out.push_back(resources->pool ? NamedSerial(a_preparation, resources->pool->indices) : 0);
			// The draw families' (their slots' pipelines: built late, rebuilt on a target change; tree LOD's).
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (const auto& family = Extensions::DrawFamilies()[f]; family.taken) {
					const std::shared_ptr<const void> pipelines = FamilyPipelinesOf(*resources->scene, static_cast<Api::DrawFamily>(f));
					family.contribution.main.revision({ a_preparation, *resources->scene, pipelines }, a_out);
				}
		}

		PreparedDraws Prepare(const PassBindings& a_bindings, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			PreparedDraws prepared{};
			const auto now = segment;
			auto frame = CurrentFrame(a_preparation, now);
			// The colour pass draws its state classes' sub-ranges (no class: nothing to draw); the depth pass its buckets and draw families.
			if (!frame || (!depth && frame->classes.empty()) || !frame->indirect.valid)
				return prepared;
			// The rescue draw belongs to the depth segment only.
			if (phaseTwo && !depth)
				return prepared;
			prepared.phaseTwo = phaseTwo;
			prepared.zPrepass = depth;
			prepared.stats = resources->passStats.get();
			if (!prepared.zPrepass)
				prepared.preprocess = resources->preprocessMain.get();
			prepared.framePushWords = MainFramePushWords(resources->frameConstantsAddress);
			// The draw families set up: their slots' pipelines (null until published: nothing drawn) and draw rows.
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (const auto family = static_cast<Api::DrawFamily>(f); FamilySetUp(*resources->scene, family))
					prepared.families[f] = { FamilyPipelinesOf(*resources->scene, family), a_bindings.familyRows[f] };
			prepared.frame = std::move(frame);
			prepared.targetCount = resources->targetCount;
			for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
				prepared.targetViews[i] = a_preparation.Capture(a_bindings.targets[i]);
			prepared.depthView = a_preparation.Capture(a_bindings.depth);
			if (prepared.zPrepass && resources->terrainDepth) {
				prepared.terrainDepthView = a_preparation.Capture(a_bindings.terrainDepth);
				prepared.terrainDepth = true;
			}
			return prepared;
		}

		static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
		{
			if (!a_prepared.frame)
				return;
			const auto& frame = *a_prepared.frame;
			auto& commands = a_recording.Commands();
			commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
			auto colors = LoadedColours(a_recording, a_prepared.targetViews, a_bindings.targets, a_prepared.targetCount);
			const bool zPrepass = a_prepared.zPrepass;
			if (a_prepared.phaseTwo && !zPrepass)
				return;
			const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
			const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
			const auto classCounts = zPrepass ? rhi::ResourceHandle{} : a_recording.Resolve(a_bindings.classCounts).GetHandle();
			rhi::PassBeginInfo begin{};
			begin.width = frame.width;
			begin.height = frame.height;
			begin.minDepth = frame.minDepth;
			begin.maxDepth = frame.maxDepth;

			// The Z-prepass adds DCLF's objects to the native depth, inside the native depth pass, so everything after (native
			// depth-tested draws, the sky, depth readers) sees them.
			auto depth = LoadedDepth(a_recording, a_prepared.depthView);
			begin.depth = &depth;
			begin.debugName = "DCLF depth";
			const MainRecording pass{ frame, commands, sequences, count, classCounts, begin, depth, a_prepared.stats, a_recording.FrameSlot() };
			if (zPrepass) {
				RecordDepthDraws(a_bindings, a_prepared, a_recording, pass);
				return;
			}
			RecordColourDraws(a_bindings, a_prepared, a_recording, pass, colors);
		}

	private:
		std::shared_ptr<Resources> resources;
		RenderGraphRuntime::Segment segment;
		bool depth = false;
		bool phaseTwo = false;
	};
}

namespace DCLF::Features::MainView
{
	namespace
	{
		// Whether a_context's epoch records the main depth shape (Api::MainShape::Depth): the Z-prepass's draws, else the colour pass's. A
		// search, at the graph's build.
		bool DepthShape(const Api::PassContext& a_context)
		{
			const std::uint32_t row = Draws::EpochById(a_context.epochId);
			return row != Draws::kNoEpoch && Draws::EpochTraitsOf(row).index == static_cast<std::uint32_t>(Api::MainShape::Depth);
		}

		std::shared_ptr<org::RenderPass> MainDraws(const Api::PassContext& a_context)
		{
			return std::make_shared<Draws::MainOpaquePass>(a_context.Main(), a_context.segment, DepthShape(a_context));
		}

		// Phase 2's rescues, in the depth epoch's two-phase tail after the retest (core.build-draws-phase2), only while the HZB exists.
		std::shared_ptr<org::RenderPass> DepthDrawPhaseTwo(const Api::PassContext& a_context)
		{
			return a_context.PhaseTwo() ? std::make_shared<Draws::MainOpaquePass>(a_context.Main(), a_context.segment, DepthShape(a_context), true) : nullptr;
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "main-view.z.depth", .passName = "cs.dclf.z.depth", .kind = Api::PassKind::Render, .make = &MainDraws });
		a_context.RegisterPass({ .key = "main-view.depth-phase2", .passName = "cs.dclf.depth-phase2", .kind = Api::PassKind::Render, .make = &DepthDrawPhaseTwo });
		a_context.RegisterPass({ .key = "main-view.colour", .passName = "cs.dclf.main-opaque", .kind = Api::PassKind::Render, .make = &MainDraws });
	}
}
