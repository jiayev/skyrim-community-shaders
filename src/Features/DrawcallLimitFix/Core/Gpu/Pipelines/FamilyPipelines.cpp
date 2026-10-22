// DrawPipelines' draw family pipelines (FamilyPipelines): each draw family's depth and colour pipelines (one per colour program), rebuilt
// with the main set, and the lane's admission of the draw family, refraction and forward builds.

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
	void DrawPipelines::RequestFamily(Api::DrawFamily a_family)
	{
		const auto f = static_cast<std::size_t>(a_family);
		if (impl->familyAsked[f])
			return;
		impl->familyAsked[f] = true;
		impl->completions->familyRequest[f].store(true, std::memory_order_release);
		WakePipelineLane();
	}

	void DrawPipelines::SetFamilyInputs(Api::DrawFamily a_family, RE::BSShader& a_shader)
	{
		auto& source = impl->renderInput.drawFamilySource[static_cast<std::size_t>(a_family)];
		if (source == &a_shader || !Enabled())
			return;
		source = &a_shader;
		impl->PostInput(*this);
	}

	void DrawPipelines::Impl::TryRequestFamilies()
	{
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f)
			TryRequestFamily(static_cast<Api::DrawFamily>(f));
	}

	void DrawPipelines::Impl::TakeFamilyRequests()
	{
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f)
			if (completions->familyRequest[f].exchange(false, std::memory_order_acq_rel))
				lane.families[f].asked = true;
	}

	void DrawPipelines::Impl::RetryFamilyPrograms(const ShaderPrograms::Finished& a_finished)
	{
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f) {
			if (a_finished.drawFamilyForward[f])
				lane.forward.RetryProgram(kDrawFamilyLaneSlots[f].forwardProgramId, [&](const auto& a_key) { TryRequestForward(a_key); });
			if (a_finished.drawFamily[f])
				TryRequestFamily(static_cast<Api::DrawFamily>(f));
		}
	}

	void DrawPipelines::Impl::TryRequestFamily(Api::DrawFamily a_family)
	{
		const auto f = static_cast<std::size_t>(a_family);
		auto& t = lane.families[f];
		if (!t.asked || t.requested || t.built || t.failed)
			return;
		const auto& family = Extensions::DrawFamilyOf(a_family);
		// What it lacks arrives with the frame's inputs or its program (Finished::drawFamily); DrainLane retries each pass.
		const auto* input = lane.input.get();
		if (!input || !input->drawFamilySource[f] || input->targets.colorCount == 0 || input->targets.depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW || !input->states ||
			!zLayout || !familySignatures[f])
			return;
		// The colour pipelines blend as the engine's write mode the program names (tree LOD's 1, the opaque mode, read unasked by
		// CaptureEngineStates); another is asked of the render thread once, and the family tried again each pass until it is read. If
		// inexpressible, the family stays native.
		const auto& program = family.contribution.program;
		const std::uint32_t stateBits = std::uint32_t(program.writeMode) << kRasterWriteModeShift;
		const auto state = input->states->find(stateBits);
		if (state == input->states->end()) {
			if (lane.statesAsked.insert(stateBits).second)
				completions->stateRequests.Push(stateBits);
			return;
		}
		if (!state->second.valid) {
			t.failed = true;
			t.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			logger::warn("[DCLF] {}'s pipelines cannot be built: write mode {}'s state cannot be read; {}", family.label, program.writeMode, family.nativeNote);
			return;
		}
		bool programFailed = false;
		const auto drawProgram = ShaderPrograms::Get().FindDrawFamily(a_family, *input->drawFamilySource[f], &programFailed);
		if (programFailed) {
			t.failed = t.programFailed = true;
			t.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			return;
		}
		if (!drawProgram)
			return;
		const auto& inputTargets = input->targets;
		const auto blend = state->second.blend;
		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.{}", family.keyName);
		// The depth vertex stage is left out of the key, as it always was (the pipeline cache's keys stay the same); the second colour
		// program's stages are mixed in only when there are (a family of one colour program keeps its key).
		recipe.shaderKey = ModuleHash(drawProgram->vertex) ^ ModuleHash(drawProgram->pixel) * 0x9E3779B97F4A7C15ull ^ ModuleHash(drawProgram->depthPixel);
		if (!drawProgram->vertex2.empty())
			recipe.shaderKey ^= (ModuleHash(drawProgram->vertex2) ^ ModuleHash(drawProgram->pixel2) * 0x9E3779B97F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&inputTargets, sizeof(inputTargets)) ^
		                          ankerl::unordered_dense::detail::wyhash::hash(&blend, sizeof(blend)) * 0x9E3779B97F4A7C15ull;
		const auto build = [device = device, zLayout = zLayout->GetHandle(), formats = inputTargets, blend, frontCCW = *input->frontCCW, colours = program.colours,
							   label = std::string_view(family.label)](const ShaderPrograms::DrawProgram& a_program) {
			return BuildDrawFamily(device, zLayout, &a_program, colours, formats, blend, frontCCW, label);
		};
		recipe.build = [build, drawProgram] { return build(*drawProgram); };
		auto live = MakeLive(recipe.id, ShaderPrograms::SourcePathOf(*input->drawFamilySource[f]), *drawProgram,
			[a_family, shader = input->drawFamilySource[f]](ShaderPrograms::Rebuilt<ShaderPrograms::DrawProgram> a_done) {
				ShaderPrograms::Get().RebuildDrawFamily(a_family, *shader, std::move(a_done));
			},
			build);
		t.requested = true;
		(void)service.Request(std::move(recipe),
			[completions = completions, f, generation = lane.generation, live = std::move(live)](const org::services::PipelineArtifact& a_artifact) {
				completions->family[f].Push({ generation, a_artifact, live });
				WakePipelineLane();
			});
		t.counters.requested.fetch_add(1, std::memory_order_relaxed);
	}

	void DrawPipelines::Impl::AdmitFamiliesAndForward()
	{
		for (std::size_t d = 0; d < kDrawFamilyLanes; ++d) {
			auto& t = lane.families[d];
			const auto& family = Extensions::DrawFamilies()[d];
			completions->family[d].Drain([&](FamilyDone&& a_done) {
				if (a_done.generation != lane.generation)
					return;  // from before a target change (RecreateMainSet)
				lane.dirty = true;
				if (!a_done.artifact) {
					t.failed = true;
					t.counters.failed.fetch_add(1, std::memory_order_relaxed);
					logger::warn("[DCLF] {}'s pipelines failed: {}; {}", family.label, a_done.artifact.error, family.nativeNote);
					return;
				}
				t.built = a_done.artifact.payload;
				t.slot = RegisterLive(a_done.live, t.built);
				t.counters.ready.fetch_add(1, std::memory_order_relaxed);
				logger::info("[DCLF] {} pipelines ready", family.label);
			});
		}
		auto& r = lane.refraction;
		completions->refraction.Drain([&](RefractionDone&& a_done) {
			if (a_done.generation != lane.generation || a_done.format != r.format || !r.requested || r.built)
				return;  // from before a target or format change (RecreateMainSet, Refraction::Reset)
			lane.dirty = true;
			if (!a_done.artifact) {
				r.failed = true;
				r.counters.failed.fetch_add(1, std::memory_order_relaxed);
				logger::warn("[DCLF] the refraction pipeline failed: {}; refraction draws nothing", a_done.artifact.error);
				return;
			}
			r.built = a_done.artifact.payload;
			r.slot = RegisterLive(a_done.live, r.built);
			r.counters.ready.fetch_add(1, std::memory_order_relaxed);
			logger::info("[DCLF] refraction pipeline ready (Utility {:08X}, refraction normals format {})", kRefractionTechnique, static_cast<int>(a_done.format));
		});
		auto& f = lane.forward;
		completions->forward.Drain([&](ForwardDone&& a_done) {
			if (a_done.generation != f.generation)
				return;  // from before a forward targets change (RecreateForward)
			const auto found = f.entries.find(a_done.key);
			if (found == f.entries.end())
				return;
			auto& entry = found->second;
			lane.dirty = true;
			if (!a_done.artifact) {
				entry.failed = true;
				f.counters.failed.fetch_add(1, std::memory_order_relaxed);
				if (f.loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] forward pipeline VS {:08X} PS {:08X} flags {:X} failed: {}", a_done.key.vertexDescriptor, a_done.key.pixelDescriptor, a_done.key.flags,
						a_done.artifact.error);
				return;
			}
			entry.built = a_done.artifact.payload;
			entry.slot = RegisterLive(a_done.live, entry.built);
			f.counters.ready.fetch_add(1, std::memory_order_relaxed);
		});
	}

	org::services::PipelinePayload DrawPipelines::Impl::BuildDrawFamily(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::DrawProgram* a_program,
		std::uint8_t a_colours, TargetFormats a_targets, rhi::BlendState a_blend, bool a_frontCCW, std::string_view a_label)
	{
		// Its stages: colour and depth, then with two colour programs the second's (DrawProgram: emptied with one).
		const bool second = a_colours == 2;
		if (second != !a_program->vertex2.empty() || second != !a_program->pixel2.empty())
			throw std::runtime_error(fmt::format("its program's second colour stages do not match its {} colour programs", a_colours));
		auto stages = Parse<4>({ &a_program->vertex, &a_program->pixel, &a_program->depthVertex, &a_program->depthPixel });
		for (std::size_t i = 0; i < stages.size(); ++i)
			CheckPulledBindings(stages[i], i % 2 == 1);
		if (second) {
			const auto stages2 = Parse<2>({ &a_program->vertex2, &a_program->pixel2 });
			CheckPulledBindings(stages2[0], false);
			CheckPulledBindings(stages2[1], true);
		}
		// The depth pipeline, then each colour program's alike (EQUAL against it, the write mode's blend).
		struct Made
		{
			const std::vector<std::byte>& vertex;
			const std::vector<std::byte>& pixel;
			rhi::PipelinePtr& out;
			std::string_view name;
		};
		auto built = std::make_shared<FamilyBuilt>();
		const std::array<Made, 3> made{ { { a_program->depthVertex, a_program->depthPixel, built->depth, "depth" },
			{ a_program->vertex, a_program->pixel, built->colours[0], "colour" }, { a_program->vertex2, a_program->pixel2, built->colours[1], "colour 2" } } };
		for (std::size_t p = 0; p < (second ? 3u : 2u); ++p) {
			const bool colour = p > 0;
			PipelineDesc desc(a_zLayout, made[p].vertex, made[p].pixel, a_targets.depth);
			desc.raster.rs.cull = rhi::CullMode::None;  // the engine draws tree LOD with culling off (cull mode 0) in both passes
			desc.raster.rs.frontCCW = a_frontCCW;
			desc.depth.ds.depthEnable = true;
			desc.depth.ds.depthWrite = !colour;
			// TEMP (tree LOD flicker): CS_DCLF_FAMILY_COLOUR_LEQUAL=1 draws the families' colour with LESS_EQUAL instead of EQUAL.
			static const bool colourLessEqual = [] { const char* v = std::getenv("CS_DCLF_FAMILY_COLOUR_LEQUAL"); return v && v[0] == '1'; }();
			desc.depth.ds.depthFunc = colour ? (colourLessEqual ? rhi::CompareOp::LessEqual : rhi::CompareOp::Equal) : rhi::CompareOp::Less;
			if (colour) {
				desc.blend.bs = a_blend;
				desc.Colours(a_targets);
			}
			desc.Create(a_device, made[p].out, fmt::format("{} {}", a_label, made[p].name));
		}
		return built;
	}

	struct ForwardPipelineAccess
	{
		static bool Family(const PipelineCatalog* a_catalog, Api::DrawFamily a_family, std::uint32_t a_targetsGeneration, FamilyPipelines& a_out)
		{
			auto& drawPipelines = DrawPipelines::Get();
			const auto& impl = *drawPipelines.impl;
			const auto f = static_cast<std::size_t>(a_family);
			// None once the targets changed after the catalog was built (as GetIndirectState).
			if (!a_catalog || !a_catalog->families[f].pipelines || a_catalog->targetsGeneration != a_targetsGeneration || !impl.familySignatures[f])
				return false;
			const auto* built = static_cast<const DrawPipelines::Impl::FamilyBuilt*>(a_catalog->families[f].pipelines.get());
			a_out = FamilyPipelines{ built->depth->GetHandle(), built->colours[0]->GetHandle(), built->colours[1] ? built->colours[1]->GetHandle() : rhi::PipelineHandle{},
				impl.familySignatures[f]->GetHandle() };
			return true;
		}
	};

	bool CatalogFamilyPipelines(const PipelineCatalog* a_catalog, Api::DrawFamily a_family, FamilyPipelines& a_out)
	{
		return ForwardPipelineAccess::Family(a_catalog, a_family, DrawPipelines::Get().Generation(), a_out);
	}

	bool CatalogFamilyPipelines(const PipelineCatalog* a_catalog, Api::DrawFamily a_family, std::uint32_t a_targetsGeneration, FamilyPipelines& a_out)
	{
		return ForwardPipelineAccess::Family(a_catalog, a_family, a_targetsGeneration, a_out);
	}
}
