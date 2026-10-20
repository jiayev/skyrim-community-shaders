// DrawPipelines' main set (MainPipelines): the Z-prepass and colour pipelines of the Lighting and program family keys, their
// requests, builds, admission into set versions, and the indirect state the epochs bind.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Shading/VertexInput.h"
#include "ShaderCache.h"

#include <Tracy/Tracy.hpp>

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <ORGModuleServices/PipelineService.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesImpl.h"

namespace DCLF
{
	namespace
	{
		/**
		 * @brief A key's variable names: its family's port space's (Extensions::MaterialSpaceOf: the Effect port's names for an effect key,
		 * Features/Effects/EffectConstants.h), else Lighting's (kind 0's space names none: these).
		 */
		std::span<const std::string_view> VariableNames(const PipelineKey& a_key, bool a_pixel)
		{
			const auto& space = Extensions::MaterialSpaceOf(Extensions::FamilyRules(a_key.rasterFlags).kind);
			const auto names = a_pixel ? space.ps.names : space.vs.names;
			return names.empty() ? LightingNames(a_pixel) : names;
		}

		/**
		 * @brief A stage's constant table from its modules, as ShaderCache's ReflectConstantBuffers: each variable's float offset in its
		 * PerTechnique/PerMaterial/PerGeometry block, 0 where absent. The modules are one permutation's builds of one stage, and
		 * Lighting.hlsl places every member with packoffset, so a variable has one offset. Pulled stages declare no blocks.
		 */
		std::vector<std::uint8_t> ConstantTableOf(std::initializer_list<const SpirvReflection*> a_modules, bool a_pixel, const PipelineKey& a_key)
		{
			static std::atomic<std::uint32_t> logged{ 0 };
			const auto names = VariableNames(a_key, a_pixel);
			std::vector<std::uint8_t> table(names.size(), 0);
			std::vector<std::uint8_t> found(names.size(), 0);
			for (const auto* module : a_modules) {
				for (const auto& member : module->blockMembers) {
					if (member.block != "PerTechnique" && member.block != "PerMaterial" && member.block != "PerGeometry")
						continue;
					const auto it = std::find(names.begin(), names.end(), member.name);
					if (it == names.end()) {
						if (logged.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedFailures)
							logger::warn("[DCLF] Constant tables: {} {} member {} is no Lighting variable (VS {:08X} PS {:08X}); left out", a_pixel ? "PS" : "VS",
								member.block, member.name, a_key.vertexDescriptor, a_key.pixelDescriptor);
						continue;
					}
					const auto variable = static_cast<std::size_t>(it - names.begin());
					const std::uint32_t offset = member.offset / 4;
					if (offset > std::numeric_limits<std::uint8_t>::max() || (found[variable] && table[variable] != offset)) {
						if (logged.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedFailures)
							logger::error("[DCLF] Constant tables: {} {} at float {} ({}) cannot be a table entry (VS {:08X} PS {:08X})", a_pixel ? "PS" : "VS", member.name,
								offset, found[variable] ? fmt::format("another stage has it at {}", table[variable]) : std::string("beyond 255"), a_key.vertexDescriptor,
								a_key.pixelDescriptor);
						continue;
					}
					table[variable] = static_cast<std::uint8_t>(offset);
					found[variable] = 1;
				}
			}
			return table;
		}
	}

	inline namespace DrawPipelinesDetail
	{
		// The main pass: pass-wide blocks by push address, b0/b1/b2/b4 from the draw's rows, the rest from the frame record.
		std::vector<rhi::LayoutBindingRange> MainLayoutRanges(bool a_pulled)
		{
			// A pulled draw (Z-prepass plain draws) reads its rows itself (DCLF_PULLED): no row ranges.
			auto fromRow = [](rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 0, kDrawPushPipelineRow); };
			std::vector<rhi::LayoutBindingRange> mainRanges;
			std::uint32_t word = kFramePushRegisters;
			for (const bool pixel : { false, true }) {
				const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
				const auto stage = pixel ? rhi::ShaderStage::Pixel : rhi::ShaderStage::Vertex;
				const std::size_t frame = pixel ? offsetof(DrawBindings, pixelConstants) : offsetof(DrawBindings, vertexConstants);
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					const std::uint32_t b = kBindingShiftB + r;
					const auto address = rhi::LayoutRangeSource::IndirectAddress;
					if ((mask >> r) & 1) {
						mainRanges.push_back(PushedRange(b, stage, 1, word));
						word += 2;
					} else if (a_pulled && (r <= 2 || r == 4)) {
						continue;
					} else if (r == 0) {
						mainRanges.push_back(fromRow(LayoutRange(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psTechnique) : offsetof(PipelineRowHeader, vsTechnique)))));
					} else if (r == 1) {
						mainRanges.push_back(FromMaterialRow(LayoutRange(b, 1, stage, address,
							kMaterialRowHeader + (pixel ? offsetof(MaterialRowHeader, psMaterial) : offsetof(MaterialRowHeader, vsMaterial)))));
					} else if (r == 2) {
						mainRanges.push_back(fromRow(LayoutRange(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psGeometry) : offsetof(PipelineRowHeader, vsGeometry)))));
					} else if (r == 4) {
						mainRanges.push_back(fromRow(LayoutRange(b, 1, stage, address,
							kPipelineRowHeader + (pixel ? offsetof(PipelineRowHeader, psPermutation) : offsetof(PipelineRowHeader, vsPermutation)))));
					} else {
						mainRanges.push_back(FromFrameRecord(LayoutRange(b, 1, stage, address, frame + 8 * std::size_t{ r })));
					}
				}
			}
			// Textures and samplers: t0-t15 and the features' from the material row, except the shadow mask (t14, pipeline row); the rest
			// from the frame record.
			const auto index = rhi::LayoutRangeSource::IndirectIndex;
			const auto pixelStage = rhi::ShaderStage::Pixel;
			for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
				if (a_pulled) {
					continue;
				} else if (t == kShadowMaskSlot) {
					mainRanges.push_back(fromRow(LayoutRange(kBindingShiftT + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMask))));
					mainRanges.push_back(fromRow(LayoutRange(kBindingShiftS + t, 1, pixelStage, index, kPipelineRowHeader + offsetof(PipelineRowHeader, shadowMaskSampler), true)));
				} else {
					mainRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftT + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, textures) + 4 * std::size_t{ t })));
					mainRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftS + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, samplers) + 4 * std::size_t{ t }, true)));
				}
			}
			std::uint32_t first = kPixelTextureSlots;
			// The feature slots' registers ascend (Extensions::ResolveMaterialContributors), as the ranges between them need.
			const std::uint32_t featureTextures = Extensions::FeatureTextureCount();
			for (std::uint32_t f = 0; f < featureTextures; ++f) {
				const std::uint32_t t = Extensions::FeatureTextureRegister(f);
				if (t > first)
					mainRanges.push_back(FromFrameRecord(LayoutRange(kBindingShiftT + first, t - first, pixelStage, index, offsetof(DrawBindings, textures) + 4 * std::size_t{ first })));
				if (!a_pulled)
					mainRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftT + t, 1, pixelStage, index, kMaterialRowHeader + offsetof(MaterialRowHeader, features) + 4 * std::size_t{ f })));
				first = t + 1;
			}
			mainRanges.push_back(FromFrameRecord(LayoutRange(kBindingShiftT + first, kTextureRegisters - first, pixelStage, index, offsetof(DrawBindings, textures) + 4 * std::size_t{ first })));
			// The vertex stage's buffers (t121-t127), from the frame record.
			mainRanges.push_back(FromFrameRecord(LayoutRange(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kShadingBufferRegister })));
			return mainRanges;
		}
	}

	void DrawPipelines::RequestLighting(const PipelineKey& a_key, std::uint32_t a_slot)
	{
		impl->completions->requests.Push({ a_key, a_slot });
		WakePipelineLane();
	}

	std::string DrawPipelines::Impl::NearestKey(const PipelineKey& a_key) const
	{
		const PipelineKey* nearest = nullptr;
		KeyDifference best;
		for (const auto& [key, entry] : lane.entries) {
			if (key == a_key)
				continue;
			KeyDifference difference;
			difference.Field("vertex", key.vertexDescriptor, a_key.vertexDescriptor);
			difference.Field("pixel", key.pixelDescriptor, a_key.pixelDescriptor);
			difference.Field("pass", key.passDescriptor, a_key.passDescriptor);
			difference.Field("raster", key.rasterFlags, a_key.rasterFlags);
			difference.Field("vertex layout", key.vertexLayout, a_key.vertexLayout);
			if (!nearest || difference.bits < best.bits) {
				nearest = &key;
				best = std::move(difference);
			}
		}
		if (!nearest)
			return lane.recreatedBy.empty() ? std::string("the first pipeline") : fmt::format("no other pipeline since the set was recreated ({})", lane.recreatedBy);
		return fmt::format("nearest requested pipeline differs in {}", best.fields);
	}

	void DrawPipelines::Impl::TryRequest(const PipelineKey& a_key)
	{
		auto& l = lane;
		if (l.entries.contains(a_key))
			return;  // requested, or failed for good
		const auto* input = l.input.get();
		// A program family's key (Api::ProgramFamily: the Effect port's): its programs are its source's, of its family's shader.
		const auto family = KeyFamily(a_key.rasterFlags);
		const bool member = family != Api::ProgramFamily::Count;
		const auto f = static_cast<std::size_t>(family);
		if (!input || !input->lighting || (member && !input->familySource[f]) || input->targets.colorCount == 0 || input->targets.depth == DXGI_FORMAT_UNKNOWN ||
			!input->frontCCW || !input->states || !layout || !zLayout) {
			l.blocked.push_back(a_key);
			return;
		}
		const auto asked = l.asked.find(a_key);
		const std::uint32_t slot = asked != l.asked.end() ? asked->second : ~0u;
		// A build here is one the precompile missed: each is logged with its key, slot and nearest relative to find the gap.
		std::uint8_t onDemand = 0;
		bool programFailed = false;
		// A family key's program is its source's (Effect.hlsl for an effect key).
		const auto* program = member ? ShaderPrograms::Get().FindFamily(family, a_key, *static_cast<const RE::BSShader*>(input->familySource[f]), &programFailed) :
		                               ShaderPrograms::Get().Find(a_key, *input->lighting, &onDemand, &programFailed);
		if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Lighting {} that no precompile requested ({}), for pipeline slot {}: {}", ShaderPrograms::OnDemandStages(onDemand),
				SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle", slot,
				DescribeKey(a_key));
		if (programFailed) {
			// Its objects stay native (no set index ever comes).
			l.FailForGood(a_key);
			l.dirty = true;
			if (member)
				l.programFamilyCounters[f].failed.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (!program) {
			l.programWaiters[member ? ShaderPrograms::FamilyProgramId(family, a_key) : ShaderPrograms::LightingProgramId(a_key)].push_back(a_key);
			return;
		}
		if (l.Full()) {
			l.blocked.push_back(a_key);
			return;
		}
		// The engine state behind the key's bits (CaptureEngineStates): asked for once, the key parked until the reply. An inexpressible
		// state keeps its objects native.
		const auto& states = *input->states;
		auto stateOf = [&](std::uint32_t a_bits) -> const EngineState* {
			if (const auto it = states.find(a_bits); it != states.end())
				return &it->second;
			if (l.statesAsked.insert(a_bits).second)
				completions->stateRequests.Push(a_bits);
			l.blocked.push_back(a_key);
			return nullptr;
		};
		EngineState state{};
		if (const auto bits = RasterStateBits(a_key.rasterFlags)) {
			const auto* found = stateOf(bits);
			if (!found || !found->valid)
				return;
			state = *found;
		} else {
			// An opaque key: the engine's blend state for its group's write mode.
			constexpr std::uint32_t kDoAlphaTest = 1u << 20;
			// LOD (LODLand 9, LODObjects 13, LODObjectHD 15, LODLandNoise 18) draws in write mode 1 even alpha-tested, as the engine's
			// main pass does (dclf-architecture.md, "LOD"), so the G-buffer alphas keep what lies beneath.
			const std::uint32_t technique = (a_key.passDescriptor >> 24) & 0x3f;
			const bool lod = technique == 9 || technique == 13 || technique == 15 || technique == 18;
			const std::uint32_t writeMode = (a_key.pixelDescriptor & kDoAlphaTest) && !lod ? kAlphaTestedWriteMode : kOpaqueWriteMode;
			const auto* found = stateOf(writeMode << kRasterWriteModeShift);
			if (!found || !found->valid)
				return;
			state.blend = found->blend;
			state.blendOnly = true;
		}
		const auto& inputTargets = input->targets;
		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.{}.{:08X}.{:08X}.{:08X}.{:X}.{:016X}", member ? std::string_view(Extensions::ProgramFamilyOf(family).keyName) : std::string_view("lighting"),
			a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.passDescriptor, a_key.rasterFlags, a_key.vertexLayout);
		recipe.shaderKey = PipelineKeyHash{}(a_key);
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&inputTargets, sizeof(inputTargets));
		// The blend state too: its write masks are the engine's, so other masks are another pipeline.
		recipe.fixedFunctionKey ^= ankerl::unordered_dense::detail::wyhash::hash(&state.blend, sizeof(state.blend)) * 0x9E3779B97F4A7C15ull;
		// No vertex input: never an artifact built with an input layout (kPulledVerticesSalt).
		recipe.fixedFunctionKey ^= kPulledVerticesSalt;
		recipe.build = [device = device, layout = layout->GetHandle(), zLayout = zLayout->GetHandle(), key = a_key, program, formats = inputTargets, state,
							frontCCW = *input->frontCCW] {
			return Build(device, layout, zLayout, key, program, formats, state, frontCCW);
		};
		l.entries.try_emplace(a_key);
		(void)service.Request(std::move(recipe), [completions = completions, key = a_key, generation = l.generation](const org::services::PipelineArtifact& a_artifact) {
			completions->main.Push({ key, generation, a_artifact });
			WakePipelineLane();
		});
		l.counters.requested.fetch_add(1, std::memory_order_relaxed);
		if (member)
			l.programFamilyCounters[f].requested.fetch_add(1, std::memory_order_relaxed);
		logger::warn("[DCLF] on-demand pipeline build: {} pipeline slot {}: {}; {}", member ? std::string_view(Extensions::ProgramFamilyOf(family).name) : std::string_view("Lighting"),
			slot, DescribeKey(a_key), NearestKey(a_key));
	}

	void DrawPipelines::Impl::RecreateMainSet(const TargetFormats& a_from, const TargetFormats& a_to)
	{
		auto& l = lane;
		auto describe = [](const TargetFormats& a_formats) {
			std::string text;
			for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
				text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
			return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
		};
		// Every pipeline depends on the targets: start over (the new generation drops queued builds' completions).
		logger::info("[DCLF] pipeline lane: main pass targets {} -> {}; rebuilding {} indirect pipelines", describe(a_from), describe(a_to), l.setPipelines.size());
		l.StartOver();
		l.versions.Retire();
		l.setPipelines.clear();
		l.zGroupOf.clear();
		l.zGroupFirst.clear();
		l.zGroups.clear();
		l.usage.clear();
		l.constantTables.clear();
		// The state classes are the set's: the new one numbers its own.
		l.classOf.clear();
		l.classMembers.clear();
		l.classIndex.clear();
		l.stateClasses.store(0, std::memory_order_relaxed);
		l.largestClass.store(0, std::memory_order_relaxed);
		for (auto& programCounters : l.programFamilyCounters)
			for (auto* counter : { &programCounters.requested, &programCounters.ready, &programCounters.failed })
				counter->store(0, std::memory_order_relaxed);
		l.recreatedBy = fmt::format("main pass targets {} -> {}", describe(a_from), describe(a_to));
		// The draw families depend on the targets too: rebuilt (TryRequestFamily), the last kept for the process.
		for (auto& t : l.families) {
			if (t.built)
				t.retired.push_back(std::move(t.built));
			t.built = {};
			t.requested = false;
			t.failed = t.programFailed;
			for (auto* counter : { &t.counters.requested, &t.counters.ready, &t.counters.failed })
				counter->store(0, std::memory_order_relaxed);
			t.counters.failed.store(t.programFailed ? 1u : 0u, std::memory_order_relaxed);
		}
		// The refraction pipeline tests the main depth.
		l.refraction.Reset();
		l.dirty = true;
	}

	void DrawPipelines::Impl::AdmitLane()
	{
		auto& l = lane;
		// Completed builds, admitted in completion order; one from an earlier generation is dropped (its entry went with the set).
		completions->main.Drain([&](MainDone&& a_done) {
			if (a_done.generation != l.generation)
				return;
			const auto& key = a_done.key;
			const auto& artifact = a_done.artifact;
			const auto found = l.entries.find(key);
			if (found == l.entries.end())
				return;
			auto& entry = found->second;
			l.dirty = true;
			if (!artifact) {
				entry.failed = true;
				++l.failedEntries;
				l.counters.failed.fetch_add(1, std::memory_order_relaxed);
				if (const auto family = KeyFamily(key.rasterFlags); family != Api::ProgramFamily::Count)
					l.programFamilyCounters[static_cast<std::size_t>(family)].failed.fetch_add(1, std::memory_order_relaxed);
				if (l.loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] Indirect pipeline VS {:08X} PS {:08X} layout {:016X} failed: {}", key.vertexDescriptor, key.pixelDescriptor, key.vertexLayout, artifact.error);
				return;
			}
			const auto* built = static_cast<const Built*>(artifact.payload.get());
			entry.slot = static_cast<std::uint32_t>(l.setPipelines.size());
			const auto [group, added] = l.zGroups.try_emplace(built->zKey, static_cast<std::uint32_t>(l.zGroupFirst.size()));
			if (added) {
				l.zGroupFirst.push_back(entry.slot);
				l.counters.zPipelines.fetch_add(1, std::memory_order_relaxed);
				if (built->zKey.pixel == 0)
					l.counters.zDepthOnly.fetch_add(1, std::memory_order_relaxed);
			}
			l.zGroupOf.push_back(group->second);
			AdmitToClass(*built, entry.slot);
			l.setPipelines.push_back(artifact.payload);
			l.usage.push_back(built->usage);
			l.constantTables.push_back(built->tables);
		});
		const auto admitted = static_cast<std::uint32_t>(l.setPipelines.size());
		if (!admitted)
			return;
		const auto published = PublishAdmitted(l, l.versions, admitted, "indirect pipeline sets", [&](SetVersion& a_version) {
			if (!ApplyClasses(a_version, admitted))
				return false;
			a_version.pipelines.assign(l.setPipelines.begin(), l.setPipelines.begin() + admitted);
			a_version.zGroupOf.assign(l.zGroupOf.begin(), l.zGroupOf.begin() + admitted);
			// A group's first pipeline is admitted before any other of it: the groups of [0, admitted) are a prefix.
			const auto groups = static_cast<std::size_t>(std::ranges::lower_bound(l.zGroupFirst, admitted) - l.zGroupFirst.begin());
			a_version.zGroupFirst.assign(l.zGroupFirst.begin(), l.zGroupFirst.begin() + static_cast<std::ptrdiff_t>(groups));
			return true;
		}, [&](const PipelineKey& a_key) {
			if (const auto family = KeyFamily(a_key.rasterFlags); family != Api::ProgramFamily::Count)
				l.programFamilyCounters[static_cast<std::size_t>(family)].ready.fetch_add(1, std::memory_order_relaxed);
		});
		if (published == Versions<SetVersion>::PublishResult::Published && Diagnostics::Observe().claims)
			if (const auto version = l.versions.published.load())
				CheckClasses(*version);
	}

	void DrawPipelines::Impl::AdmitToClass(const Built& a_built, std::uint32_t a_index)
	{
		auto& l = lane;
		// Its state class: the first pipeline of equal state (both variants, SameState) made one, in admission order.
		const auto [found, added] = l.classIndex.try_emplace(a_built.stateClass, static_cast<std::uint32_t>(l.classMembers.size()));
		const std::uint32_t c = found->second;
		if (added)
			l.classMembers.emplace_back();
		auto& members = l.classMembers[c];
		l.classOf.resize(std::max<std::size_t>(l.classOf.size(), std::size_t(a_index) + 1), kNoSetClass);
		l.classOf[a_index] = (c << kSetClassShift) | static_cast<std::uint32_t>(members.size());
		members.push_back(a_index);
		l.stateClasses.store(static_cast<std::uint32_t>(l.classMembers.size()), std::memory_order_relaxed);
		l.largestClass.store(std::max(l.largestClass.load(std::memory_order_relaxed), static_cast<std::uint32_t>(members.size())), std::memory_order_relaxed);
	}

	bool DrawPipelines::Impl::ApplyClasses(SetVersion& a_version, std::uint32_t a_admitted)
	{
		auto& l = lane;
		// Per state class, its own sets and signatures (its members only: a set's pipelines may differ in their shaders alone). A version
		// brought up from an older generation catches up on every class; the classes it has not seen are made (initial pipeline: their
		// first member). The classes' members are admitted in order, so those of [0, a_admitted) are a prefix of each.
		a_version.classes.resize(l.classMembers.size());
		for (std::size_t c = 0; c < l.classMembers.size(); ++c) {
			const auto& members = l.classMembers[c];
			auto& sets = a_version.classes[c];
			const auto count = static_cast<std::uint32_t>(std::ranges::lower_bound(members, a_admitted) - members.begin());
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				const auto handleOf = [&](std::uint32_t a_member) {
					return static_cast<const Built*>(l.setPipelines[members[a_member]].get())->pipelines[variant]->GetHandle();
				};
				const auto name = fmt::format("DCLF indirect pipelines class {}{}", c, variant == kDepthVariant ? " (depth)" : "");
				if (!ApplyToSet(sets.sets[variant], sets.applied, count, name.c_str(), handleOf, [&] { return CreateSignature(sets, variant); }))
					return false;
				auto& written = sets.written[variant];
				for (auto m = static_cast<std::uint32_t>(written.size()); m < count; ++m)
					written.push_back(handleOf(m));
			}
			sets.applied = count;
		}
		a_version.classOf.assign(l.classOf.begin(), l.classOf.begin() + a_admitted);
		return true;
	}

	template <class Check>
	void DrawPipelines::Impl::CheckClassMembers(const SetVersion& a_version, std::uint32_t a_class, const std::vector<std::uint32_t>& a_members,
		std::vector<std::uint8_t>& a_seen, Check&& a_check)
	{
		const auto& sets = a_version.classes[a_class];
		const std::uint32_t applied = a_version.applied;
		auto builtOf = [&](std::uint32_t a_index) {
			return a_index < a_version.pipelines.size() ? static_cast<const Built*>(a_version.pipelines[a_index].get()) : nullptr;
		};
		const Built* initial = a_members.empty() ? nullptr : builtOf(a_members[0]);
		for (std::uint32_t i = 0; i < sets.applied && i < a_members.size(); ++i) {
			const std::uint32_t member = a_members[i];
			const Built* built = builtOf(member);
			a_check(member < applied && built, [&] { return fmt::format("class {} index {}: member {} past the version's {} pipelines", a_class, i, member, applied); });
			if (member >= applied || !built)
				continue;
			++a_seen[member];
			const std::uint32_t word = (a_class << kSetClassShift) | i;
			const std::uint32_t stored = member < a_version.classOf.size() ? a_version.classOf[member] : kNoSetClass;
			a_check(stored == word, [&] {
				return fmt::format("pipeline {} at class {} index {}: class word {:#x}, expected {:#x}", member, a_class, i, stored, word);
			});
			// The original defect: a pipeline admitted into a set whose initial pipeline's fixed-function state is not its own.
			if (initial && i != 0) {
				KeyDifference fields;
				a_check(SameState(built->stateClass, initial->stateClass, &fields), [&] {
					return fmt::format("pipeline {} (class {} index {}) differs from the class's initial pipeline {}: {}", member, a_class, i, a_members[0], fields.fields);
				});
			}
			for (std::uint32_t v = 0; v < kVariantCount; ++v) {
				if (i >= sets.written[v].size() || !built->pipelines[v])
					continue;
				const auto expected = built->pipelines[v]->GetHandle();
				const auto& written = sets.written[v][i];
				a_check(written.index == expected.index && written.generation == expected.generation, [&] {
					return fmt::format("class {} variant {} index {}: handle {}/{} written, pipeline {}'s is {}/{}", a_class, v, i, written.index, written.generation,
						member, expected.index, expected.generation);
				});
			}
		}
	}

	void DrawPipelines::Impl::CheckClasses(const SetVersion& a_version)
	{
		auto& l = lane;
		auto& parity = l.classParity;
		const std::uint32_t applied = a_version.applied;
		std::uint32_t logged = 0;
		// One comparison; a failure is logged at once (the first few of a version), its description the counter's first if it is.
		auto check = [&](bool a_equal, auto&& a_describe) {
			if (a_equal) {
				parity.Check(true);
				return;
			}
			const std::string text = a_describe();
			parity.Check(false, [&] { return text; });
			if (logged++ < kMaxLoggedFailures)
				logger::warn("[DCLF] set class parity (version of {} pipelines, {} classes): {} <- SET CLASS", applied, a_version.classes.size(), text);
		};
		check(a_version.pipelines.size() == applied && a_version.classOf.size() == applied && a_version.classes.size() <= l.classMembers.size(),
			[&] { return fmt::format("the version holds {} pipelines and {} class words for {} applied, {} classes of the lane's {}", a_version.pipelines.size(),
					  a_version.classOf.size(), applied, a_version.classes.size(), l.classMembers.size()); });
		std::vector<std::uint8_t> seen(applied, 0);
		std::uint64_t members = 0;
		// The classes' initial pipelines' keys, each class's own (none equal: StateClassEqual).
		ankerl::unordered_dense::map<StateClassKey, std::uint32_t, StateClassHash, StateClassEqual> initials;
		for (std::uint32_t c = 0; c < a_version.classes.size() && c < l.classMembers.size(); ++c) {
			const auto& sets = a_version.classes[c];
			const auto& classMembers = l.classMembers[c];
			members += sets.applied;
			check(sets.applied <= classMembers.size() && sets.applied > 0, [&] {
				return fmt::format("class {} applies {} pipelines of its {} members", c, sets.applied, classMembers.size());
			});
			for (std::uint32_t v = 0; v < kVariantCount; ++v)
				check(sets.written[v].size() == sets.applied && sets.sets[v] && sets.signatures[v], [&] {
					return fmt::format("class {} variant {}: {} handles written for {} applied, set {}, signature {}", c, v, sets.written[v].size(), sets.applied,
						sets.sets[v] ? "made" : "missing", sets.signatures[v] ? "made" : "missing");
				});
			if (!classMembers.empty() && classMembers[0] < a_version.pipelines.size()) {
				const auto inserted = initials.try_emplace(static_cast<const Built*>(a_version.pipelines[classMembers[0]].get())->stateClass, c);
				check(inserted.second, [&] {
					const std::uint32_t other = inserted.first->second;
					return fmt::format("classes {} and {} have equal state (their initial pipelines {} and {})", other, c, l.classMembers[other][0], classMembers[0]);
				});
			}
			CheckClassMembers(a_version, c, classMembers, seen, check);
		}
		check(members == applied, [&] { return fmt::format("the classes apply {} pipelines in all, the version {}", members, applied); });
		for (std::uint32_t g = 0; g < applied; ++g)
			check(seen[g] == 1, [&] {
				return fmt::format("pipeline {} is in {} classes (class word {:#x})", g, seen[g], g < a_version.classOf.size() ? a_version.classOf[g] : kNoSetClass);
			});
		l.classParityChecks.store(parity.checks, std::memory_order_relaxed);
		l.classParityMismatches.store(parity.mismatches, std::memory_order_relaxed);
	}

	void DrawPipelines::Impl::BuildPulledColour(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const std::vector<std::byte>& a_vertex,
		const std::vector<std::byte>& a_pixel, const PipelineDesc& a_colour, std::string_view a_name, Built& a_built, rhi::PipelinePtr Built::* a_into)
	{
		static std::atomic<std::uint32_t> loggedFailures{ 0 };
		try {
			auto [vertex, pixel] = Parse<2>({ &a_vertex, &a_pixel });
			CheckPulledBindings(vertex, false);
			CheckPulledBindings(pixel, true);
			PipelineDesc plain(a_zLayout, a_vertex, a_pixel, DXGI_FORMAT_UNKNOWN);
			plain.depthFormat = a_colour.depthFormat;
			plain.raster = a_colour.raster;
			plain.depth = a_colour.depth;
			plain.blend = a_colour.blend;
			plain.targets = a_colour.targets;
			plain.Create(a_device, a_built.*a_into, fmt::format("{} pulled {}", a_name, a_into == &Built::pulledColour ? "color" : "depth"));
		} catch (const std::exception& e) {
			a_built.*a_into = {};
			if (loggedFailures.fetch_add(1) < 8)
				logger::warn("[DCLF] {}: no plain colour pipeline (its forward draws stay device-generated): {}", a_name, e.what());
		}
	}

	org::services::PipelinePayload DrawPipelines::Impl::BuildForwardOrDecal(const ProgramFamilySlot& a_slot, std::string_view a_keyName, rhi::Device a_device,
		rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key, const ShaderPrograms::Program* a_program, TargetFormats a_targets,
		EngineState a_state, bool a_frontCCW)
	{
		auto [vertex, pixel] = Parse<2>({ &a_program->vertex, &a_program->pixel });
		const std::uint32_t decalGroup = RasterDecalGroup(a_key.rasterFlags);
		if (!a_state.valid || (!RasterForward(a_key.rasterFlags) && decalGroup != 1 && decalGroup != 2))
			throw std::runtime_error(fmt::format("an {} key without the forward or a decal group's state", a_keyName));
		CheckBindings(vertex, false);
		CheckBindings(pixel, true);
		CheckVertexAttributes(a_program->vertexAttributes, a_key.vertexLayout);
		auto built = std::make_shared<Built>();
		for (const auto variant : { kColorVariant, kDepthVariant }) {
			AddUsage(vertex, false, built->usage[variant]);
			AddUsage(pixel, true, built->usage[variant]);
		}
		built->tables = std::make_shared<const ConstantTables>(ConstantTables{ ConstantTableOf({ &vertex }, false, a_key), ConstantTableOf({ &pixel }, true, a_key) });
		built->zKey.vertex = ModuleHash(a_program->vertex) ^ a_slot.zSalt;  // no Lighting pipeline's
		built->zKey.vertexBytes = static_cast<std::uint32_t>(a_program->vertex.size());
		built->zKey.pixel = ModuleHash(a_program->pixel);
		built->zKey.pixelBytes = static_cast<std::uint32_t>(a_program->pixel.size());
		const bool test = (a_key.rasterFlags & kRasterForwardDepthTest) != 0, write = (a_key.rasterFlags & kRasterForwardDepthWrite) != 0;
		for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
			const bool depthOnly = variant == kDepthVariant;
			PipelineDesc desc(a_layout, a_program->vertex, a_program->pixel, a_targets.depth);
			desc.flags.flags = rhi::PipelineFlags_IndirectBindable;
			desc.raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : rhi::CullMode::Back;
			desc.raster.rs.frontCCW = a_frontCCW;
			auto& depth = desc.depth.ds;
			depth.depthWrite = !depthOnly && write;
			depth.depthEnable = depthOnly || test || write;
			depth.depthFunc = (depthOnly || test) ? rhi::CompareOp::LessEqual : rhi::CompareOp::Always;
			// Decal rules: LESS_EQUAL; only the opaque group's depth variant writes depth. No rasterizer bias: these pipelines are switched
			// by device-generated commands, whose execution sets may change shaders only (VK_EXT_device_generated_commands); the vertex
			// stage moves a decal toward the eye instead (kRasterDecalNudge), a first-person effect decal drawn forward
			// (Coordinator::ViewDecalForward) too.
			if (decalGroup) {
				depth.depthEnable = true;
				depth.depthWrite = depthOnly && decalGroup == 1;
				depth.depthFunc = rhi::CompareOp::LessEqual;
			}
			if (!depthOnly) {
				desc.blend.bs = a_state.blend;
				desc.Colours(a_targets);
				for (std::uint32_t i = decalGroup ? 0u : 1u; i < a_targets.colorCount && i < 8; ++i) {
					if (decalGroup && ((pixel.outputLocations >> i) & 1))
						continue;
					desc.blend.bs.attachments[i].enable = false;
					desc.blend.bs.attachments[i].writeMask = static_cast<rhi::ColorWriteEnable>(0);
				}
			}
			built->stateClass.variants[variant] = desc.State(pixel);
			desc.Create(a_device, built->pipelines[variant], fmt::format("{} {}", a_keyName, depthOnly ? "depth" : "color"));
			if (!depthOnly)
				BuildPulledColour(a_device, a_zLayout, a_program->pulledVertex, a_program->pulledPixel, desc, a_keyName, *built);
			else if (decalGroup == 1)
				BuildPulledColour(a_device, a_zLayout, a_program->pulledVertex, a_program->pulledPixel, desc, a_keyName, *built, &Built::pulledDecalDepth);
		}
		return built;
	}

	org::services::PipelinePayload DrawPipelines::Impl::Build(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key,
		const ShaderPrograms::Program* a_program, TargetFormats a_targets, EngineState a_state, bool a_frontCCW)
	{
		// A program family's key: its build rule (Api::ProgramBuild; ForwardOrDecal, the one there is).
		if (const auto family = KeyFamily(a_key.rasterFlags); family != Api::ProgramFamily::Count)
			return BuildForwardOrDecal(ProgramFamilySlotOf(family), Extensions::ProgramFamilyOf(family).keyName, a_device, a_layout, a_zLayout, a_key, a_program, a_targets, a_state,
				a_frontCCW);
		auto [vertex, pixel, depthPixel, pulledVertex, pulledDepthPixel] =
			Parse<5>({ &a_program->vertex, &a_program->pixel, &a_program->depthPixel, &a_program->pulledVertex, &a_program->pulledDepthPixel });
		CheckBindings(vertex, false);
		CheckBindings(pixel, true);
		CheckBindings(depthPixel, true);
		CheckPulledBindings(pulledVertex, false);
		CheckPulledBindings(pulledDepthPixel, true);
		// Both vertex stages (DCLF_PULLED_VERTICES, DCLF_PULLED) read the same attributes (Lighting.hlsl, DCLFPulledInput).
		CheckVertexAttributes(a_program->vertexAttributes, a_key.vertexLayout);
		auto built = std::make_shared<Built>();
		// CS_DCLF_FOLIAGE_PARITY: terrain LOD's depth stage records its pixels (DCLF_OWNED_PARITY), so it keeps its pixel stage.
		const std::uint32_t technique = (a_key.passDescriptor >> 24) & 0x3f;
		const bool pulledPixel = FragmentDefersDepth(a_program->pulledDepthPixel) || (Policy::RecordFoliagePixels() && (technique == 9 || technique == 18));
		AddUsage(vertex, false, built->usage[kColorVariant]);
		AddUsage(pixel, true, built->usage[kColorVariant]);
		AddUsage(vertex, false, built->usage[kDepthVariant]);
		AddUsage(depthPixel, true, built->usage[kDepthVariant]);
		built->tables = std::make_shared<const ConstantTables>(ConstantTables{ ConstantTableOf({ &vertex }, false, a_key), ConstantTableOf({ &pixel, &depthPixel }, true, a_key) });
		// Decal groups (engine notes, "Decals"): an opaque-group decal (hint 2) writes depth in the decal depth pass, before the colour
		// pass, with the colour variant's bias and test. A blended decal writes none (depth mode 1). A multi-index layer (group 3, the
		// engine's group 2: depth mode 3) writes depth from its colour draw after its host is shaded; its depth variant is never drawn.
		const std::uint32_t decalGroup = a_state.valid ? RasterDecalGroup(a_key.rasterFlags) : 0u;
		// A terrain group's key carries the engine's blend (PackTerrainRasterFlags) but no decal depth: an opaque depth variant without
		// bias; the colour variant tests as Terrain Blending's replay: group 1 LESS_EQUAL with writes (terrainDepthStencilState),
		// group 2 EQUAL.
		const std::uint32_t terrainGroup = RasterTerrainGroup(a_key.rasterFlags);
		const bool engineDepth = a_state.valid && terrainGroup == 0;
		// A forward key (kRasterForward) draws after the deferred composite on the colour pass's eight attachments (so it lives in the
		// main set): the main target alone, blended per the engine's state, the G-buffer masked off (unbound in the engine); depth per
		// the property (SetupGeometry: kZBufferTest, kZBufferWrite). Its depth variant is never drawn.
		const bool forward = a_state.valid && RasterForward(a_key.rasterFlags);
		for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
			const bool depthOnly = variant == kDepthVariant;
			PipelineDesc desc(a_layout, a_program->vertex, depthOnly ? a_program->depthPixel : a_program->pixel, a_targets.depth);
			desc.flags.flags = rhi::PipelineFlags_IndirectBindable;
			auto& raster = desc.raster.rs;
			raster.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : rhi::CullMode::Back;
			// No rasterizer bias for a decal: its variants are switched by device-generated commands, whose execution sets may change
			// shaders only (VK_EXT_device_generated_commands). The vertex stage moves a decal toward the eye instead (kRasterDecalNudge,
			// DCLFDecalNudged), in its depth variant as in its colour one.
			// The engine's winding (EngineFrontCCW); RasterState::frontCCW means the same on every backend.
			raster.frontCCW = a_frontCCW;
			auto& depth = desc.depth.ds;
			depth.depthEnable = true;
			// The colour variant tests EQUAL against DCLF's Z-prepass: where the prepass's alpha test discarded (foliage cards), LESS_EQUAL
			// would pass those fragments and run full lighting before they discard. A decal keeps LESS_EQUAL: its depth bias never passes EQUAL.
			depth.depthWrite = depthOnly ? decalGroup != 2 : (decalGroup == 3 || terrainGroup == 1);
			depth.depthFunc = depthOnly ? (engineDepth ? rhi::CompareOp::LessEqual : rhi::CompareOp::Less) :
			                              ((engineDepth || terrainGroup == 1) ? rhi::CompareOp::LessEqual : rhi::CompareOp::Equal);
			if (forward) {
				const bool test = (a_key.rasterFlags & kRasterForwardDepthTest) != 0, write = (a_key.rasterFlags & kRasterForwardDepthWrite) != 0;
				depth.depthWrite = !depthOnly && write;
				depth.depthEnable = depthOnly || test || write;
				depth.depthFunc = (depthOnly || test) ? rhi::CompareOp::LessEqual : rhi::CompareOp::Always;
			}
			if (!depthOnly) {
				// A key with engine state blends exactly as the engine's state says, write masks included; others don't blend.
				if (a_state.valid || a_state.blendOnly)
					desc.blend.bs = a_state.blend;
				desc.Colours(a_targets);
				if (forward)
					for (std::uint32_t i = 1; i < a_targets.colorCount && i < 8; ++i) {
						desc.blend.bs.attachments[i].enable = false;
						desc.blend.bs.attachments[i].writeMask = static_cast<rhi::ColorWriteEnable>(0);
					}
			}
			built->stateClass.variants[variant] = desc.State(depthOnly ? depthPixel : pixel);
			const std::string keyName = fmt::format("vs {:08X} ps {:08X} flags {:08X}", a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.rasterFlags);
			desc.Create(a_device, built->pipelines[variant], fmt::format("{} {}", keyName, depthOnly ? "depth" : "color"));
			if (!depthOnly) {
				// The plain colour draws' pipeline: a forward key's (the forward buckets), a main key's (the main buckets), a terrain group's
				// (the terrain buckets) and a decal group's (the decal buckets).
				BuildPulledColour(a_device, a_zLayout, a_program->pulledVertex, a_program->pulledPixel, desc,
					fmt::format("{} {}", forward ? "Lighting forward" : terrainGroup ? "Lighting terrain" : RasterDecalGroup(a_key.rasterFlags) ? "Lighting decal" : "Lighting main", keyName), *built);
				continue;
			}
			// The Z-prepass plain-draw pipeline: the depth variant's state, pulled stages, no vertex input, bound per call.
			auto& zKey = built->zKey;
			zKey.vertex = ModuleHash(a_program->pulledVertex);
			zKey.vertexBytes = static_cast<std::uint32_t>(a_program->pulledVertex.size());
			zKey.pixel = pulledPixel ? ModuleHash(a_program->pulledDepthPixel) : 0;
			zKey.pixelBytes = pulledPixel ? static_cast<std::uint32_t>(a_program->pulledDepthPixel.size()) : 0;
			zKey.cull = static_cast<std::uint32_t>(raster.cull);
			zKey.depthWrite = depth.depthWrite ? 1u : 0u;
			zKey.depthFunc = static_cast<std::uint32_t>(depth.depthFunc);
			zKey.depthBias = std::bit_cast<std::uint32_t>(static_cast<float>(raster.depthBias));
			zKey.depthBiasClamp = std::bit_cast<std::uint32_t>(raster.depthBiasClamp);
			zKey.slopeScaledDepthBias = std::bit_cast<std::uint32_t>(raster.slopeScaledDepthBias);
			zKey.terrainNear = terrainGroup == 1 ? 1u : 0u;
			PipelineDesc pulled(a_zLayout, a_program->pulledVertex, pulledPixel ? std::span<const std::byte>(a_program->pulledDepthPixel) : std::span<const std::byte>{},
				a_targets.depth);
			pulled.raster = desc.raster;
			pulled.depth = desc.depth;
			pulled.blend = desc.blend;
			pulled.targets = desc.targets;
			pulled.Create(a_device, built->pulledDepth, "pulled depth");
		}
		return built;
	}

	bool DrawPipelines::Impl::CreateSignature(SetVersion::ClassSets& a_class, std::uint32_t a_variant)
	{
		// No vertex buffer arguments: the vertex stages pull their attributes through the buffers' views in the push data
		// (DCLF_PULLED_VERTICES), so no pipeline has vertex bindings or a dynamic stride (VUID-VkGeneratedCommandsInfoEXT-
		// indirectCommandsLayout-11079).
		rhi::IndirectArg args[4]{};
		args[0].kind = rhi::IndirectArgKind::PipelineIndex;
		args[1].kind = rhi::IndirectArgKind::Constant;
		// The rows' addresses, the object word and both vertex buffer views (DrawSequence bytes 4-55) -> the layout's push data.
		args[1].u.rootConstants = { 0, 0, kDrawPushArgumentWords };
		args[2].kind = rhi::IndirectArgKind::IndexBuffer;
		args[3].kind = rhi::IndirectArgKind::DrawIndexed;
		rhi::CommandSignatureDesc desc{};
		desc.args = { args, 4 };
		desc.byteStride = sizeof(DrawSequence);
		// One per state class and variant: a call that draws a class's sub-range selects from that class's set alone.
		desc.pipelineSet = a_class.sets[a_variant]->GetHandle();
		desc.explicitPreprocess = true;
		return device.CreateCommandSignature(desc, layout->GetHandle(), a_class.signatures[a_variant]) == rhi::Result::Ok;
	}

	IndirectState GetIndirectState(const PipelineCatalog& a_catalog, std::uint32_t a_targetsGeneration)
	{
		// a_catalog's set version (its lookups' indices are of it); none once the targets differ from the set's.
		const auto& pipelines = DrawPipelines::Get();
		const auto& impl = *pipelines.impl;
		IndirectState state;
		if (!a_catalog.setVersion || a_catalog.targetsGeneration != a_targetsGeneration || !impl.layout)
			return state;
		auto version = std::static_pointer_cast<const DrawPipelines::Impl::SetVersion>(a_catalog.setVersion);
		state.classes.resize(version->classes.size());
		for (std::size_t c = 0; c < version->classes.size(); ++c) {
			const auto& sets = version->classes[c];
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				state.classes[c].sets[variant] = sets.sets[variant]->GetHandle();
				state.classes[c].signatures[variant] = sets.signatures[variant]->GetHandle();
			}
			state.classes[c].pipelines = sets.applied;
		}
		state.classOf = version->classOf;
		state.zGroups = version->zGroupOf;
		state.zPipelines.reserve(version->zGroupFirst.size());
		for (const auto first : version->zGroupFirst) {
			// An effect pipeline's group has no Z-prepass pipeline, and no bucket names it (PlanZBuckets).
			const auto& pulledDepth = static_cast<const DrawPipelines::Impl::Built*>(version->pipelines[first].get())->pulledDepth;
			state.zPipelines.push_back(pulledDepth ? pulledDepth->GetHandle() : decltype(pulledDepth->GetHandle()){});
		}
		state.plainColour.reserve(version->pipelines.size());
		state.plainDepth.reserve(version->pipelines.size());
		for (const auto& payload : version->pipelines) {
			const auto* built = static_cast<const DrawPipelines::Impl::Built*>(payload.get());
			const auto& pulledColour = built->pulledColour;
			state.plainColour.push_back(pulledColour ? pulledColour->GetHandle() : decltype(pulledColour->GetHandle()){});
			const auto& pulledDepth = built->pulledDecalDepth ? built->pulledDecalDepth : built->pulledDepth;
			state.plainDepth.push_back(pulledDepth ? pulledDepth->GetHandle() : decltype(pulledDepth->GetHandle()){});
		}
		state.zLayout = impl.zLayout->GetHandle();
		state.zDrawSignature = impl.zDrawSignature->GetHandle();
		// The refraction draw, under the shadow views' layout.
		if (a_catalog.refraction.pipeline && impl.shadowLayout && impl.shadowDrawSignature) {
			state.refraction = (*static_cast<const rhi::PipelinePtr*>(a_catalog.refraction.pipeline.get()))->GetHandle();
			state.refractionFormat = a_catalog.refraction.format;
			state.refractionLayout = impl.shadowLayout->GetHandle();
			state.refractionDrawSignature = impl.shadowDrawSignature->GetHandle();
		}
		state.version = std::move(version);
		state.layout = impl.layout->GetHandle();
		state.valid = true;
		return state;
	}

	IndirectState FrameIndirectState()
	{
		const auto* catalog = FrameCatalog();
		return catalog ? GetIndirectState(*catalog, DrawPipelines::Get().Generation()) : IndirectState{};
	}
}
