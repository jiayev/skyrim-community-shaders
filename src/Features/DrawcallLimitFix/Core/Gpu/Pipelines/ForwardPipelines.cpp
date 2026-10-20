// DrawPipelines' forward pipelines (ForwardPipelines): the forward views' keys and the refraction pipeline, built for their
// targets and bound by handle.

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
		/** @brief The draw family whose lane slot's forward flags a_flags (a forward key's) carry, or Count for a Lighting key. */
		Api::DrawFamily ForwardFamilyOf(std::uint32_t a_flags)
		{
			for (const auto& slot : kDrawFamilyLaneSlots)
				if (a_flags & slot.forwardFlags)
					return slot.family;
			return Api::DrawFamily::Count;
		}
	}

	void DrawPipelines::RequestRefraction()
	{
		if (impl->refractionAsked)
			return;
		impl->refractionAsked = true;
		impl->completions->refractionRequest.store(true, std::memory_order_release);
		WakePipelineLane();
	}

	void DrawPipelines::SetRefractionInputs(RE::BSShader& a_utility, DXGI_FORMAT a_format, std::uint8_t a_writeMask)
	{
		auto& input = impl->renderInput;
		if ((input.refractionUtility == &a_utility && input.refractionFormat == a_format && input.refractionWriteMask == a_writeMask) || !Enabled())
			return;
		input.refractionUtility = &a_utility;
		input.refractionFormat = a_format;
		input.refractionWriteMask = a_writeMask;
		impl->PostInput(*this);
	}

	void DrawPipelines::RequestForward(const ForwardPipelineKey& a_key)
	{
		if (!impl->forwardAsked.insert(a_key).second)
			return;
		impl->completions->forwardRequests.Push(a_key);
		WakePipelineLane();
	}

	void DrawPipelines::SetViewForwardTargets(Api::EngineView a_view, const ForwardTargets& a_targets)
	{
		auto& input = impl->renderInput;
		auto& viewTargets = input.viewTargets[static_cast<std::size_t>(a_view)];
		if (a_targets.colour == DXGI_FORMAT_UNKNOWN || a_targets.depth == DXGI_FORMAT_UNKNOWN || viewTargets == a_targets || !Enabled())
			return;
		viewTargets = a_targets;
		impl->PostInput(*this);
	}

	void DrawPipelines::Impl::Lane::Refraction::Reset()
	{
		if (built)
			retired.push_back(std::move(built));
		built = {};
		slot = {};
		requested = false;
		failed = programFailed;
		format = DXGI_FORMAT_UNKNOWN;
	}

	void DrawPipelines::Impl::TryRequestRefraction()
	{
		auto& r = lane.refraction;
		if (!r.asked || r.requested || r.built || r.failed)
			return;
		// What it lacks arrives with the frame's inputs or its program (Finished::shadow); DrainLane retries each pass.
		const auto* input = lane.input.get();
		if (!input || !input->refractionUtility || input->refractionFormat == DXGI_FORMAT_UNKNOWN || input->targets.depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW ||
			!shadowLayout)
			return;
		bool programFailed = false;
		const auto program = ShaderPrograms::Get().FindShadow(kRefractionTechnique, *input->refractionUtility, true, nullptr, nullptr, &programFailed);
		if (programFailed) {
			r.failed = r.programFailed = true;
			r.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			logger::warn("[DCLF] the refraction program (Utility {:08X}, pulled) failed; refraction draws nothing", kRefractionTechnique);
			return;
		}
		if (!program)
			return;
		const DXGI_FORMAT format = input->refractionFormat, depthFormat = input->targets.depth;
		const std::uint8_t writeMask = input->refractionWriteMask;
		const bool frontCCW = *input->frontCCW;
		org::services::PipelineRecipe recipe;
		recipe.id = "dclf.refraction";
		recipe.shaderKey = ModuleHash(program->vertex) ^ ModuleHash(program->pixel) * 0x9E3779B97F4A7C15ull;
		const std::uint64_t fixedFunction[] = { static_cast<std::uint64_t>(format), static_cast<std::uint64_t>(depthFormat), writeMask, frontCCW ? 1ull : 0ull };
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(fixedFunction, sizeof(fixedFunction));
		const auto build = [device = device, layout = ShadowLayout(), format, depthFormat, writeMask, frontCCW](const ShaderPrograms::ShadowProgram& a_program) {
			return BuildRefraction(device, layout, &a_program, format, depthFormat, writeMask, frontCCW);
		};
		recipe.build = [build, program] { return build(*program); };
		auto live = MakeLive(recipe.id, ShaderPrograms::SourcePathOf(*input->refractionUtility), *program,
			[utility = input->refractionUtility](ShaderPrograms::Rebuilt<ShaderPrograms::ShadowProgram> a_done) {
				ShaderPrograms::Get().RebuildShadow(kRefractionTechnique, *utility, std::move(a_done));
			},
			build);
		r.requested = true;
		r.format = format;
		(void)service.Request(std::move(recipe),
			[completions = completions, generation = lane.generation, format, live = std::move(live)](const org::services::PipelineArtifact& a_artifact) {
				completions->refraction.Push({ generation, format, a_artifact, live });
				WakePipelineLane();
			});
		r.counters.requested.fetch_add(1, std::memory_order_relaxed);
	}

	void DrawPipelines::Impl::TryRequestForward(const ForwardPipelineKey& a_key)
	{
		auto& f = lane.forward;
		if (f.entries.contains(a_key))
			return;  // requested, or failed for good
		// A draw family's key (its lane slot's forward flags: tree LOD's) draws its forward program.
		const auto family = ForwardFamilyOf(a_key.flags);
		const bool familyKey = family != Api::DrawFamily::Count;
		const auto d = static_cast<std::size_t>(family);
		// A key draws into its view's targets (Extensions::ForwardKeyView), with its view's culling (a plain projection's back faces).
		const auto view = Extensions::ForwardKeyView(a_key.flags);
		const bool cullBack = view != Api::EngineView::Count && Extensions::ViewEntryOf(view).keys.cullBackFaces;
		const auto* input = lane.input.get();
		const ForwardTargets* keyTargets = input && view != Api::EngineView::Count ? &input->viewTargets[static_cast<std::size_t>(view)] : nullptr;
		if (!input || !keyTargets || keyTargets->colour == DXGI_FORMAT_UNKNOWN || keyTargets->depth == DXGI_FORMAT_UNKNOWN || !input->frontCCW ||
			!(familyKey ? input->drawFamilySource[d] : input->lighting) || !zLayout) {
			f.blocked.push_back(a_key);
			return;
		}
		bool programFailed = false;
		const auto program = familyKey ? ShaderPrograms::Get().FindDrawFamilyForward(family, *input->drawFamilySource[d], &programFailed) :
		                                 ShaderPrograms::Get().FindForward(a_key.vertexDescriptor, a_key.pixelDescriptor, *input->lighting, &programFailed);
		if (programFailed) {
			// Its objects stay out of the reflection phase (the engine draws them).
			f.FailForGood(a_key);
			f.counters.failed.fetch_add(1, std::memory_order_relaxed);
			lane.dirty = true;
			return;
		}
		if (!program) {
			f.programWaiters[familyKey ? kDrawFamilyLaneSlots[d].forwardProgramId : ShaderPrograms::ForwardProgramId(a_key.vertexDescriptor, a_key.pixelDescriptor)].push_back(a_key);
			return;
		}
		const ForwardTargets forwardTargets = *keyTargets;
		const auto cull = (a_key.flags & ForwardPipelineKey::kForwardTwoSided) ? rhi::CullMode::None : cullBack ? rhi::CullMode::Back : rhi::CullMode::Front;
		org::services::PipelineRecipe recipe;
		recipe.id = familyKey ? fmt::format("dclf.forward.{}", Extensions::DrawFamilyOf(family).keyName) :
		                        fmt::format("dclf.forward.{:08X}.{:08X}.{:X}", a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.flags);
		recipe.shaderKey = ModuleHash(program->vertex) ^ ModuleHash(program->pixel) * 0x9E3779B97F4A7C15ull;
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&forwardTargets, sizeof(forwardTargets)) ^
		                          static_cast<std::uint64_t>(cull) * 0x9E3779B97F4A7C15ull;
		const auto build = [device = device, zLayout = zLayout->GetHandle(), forwardTargets, cull, frontCCW = *input->frontCCW](const ShaderPrograms::ForwardProgram& a_program) {
			return BuildForward(device, zLayout, &a_program, forwardTargets, cull, frontCCW);
		};
		recipe.build = [build, program] { return build(*program); };
		// Its program as the lane made it: a draw family's forward descriptor's, or the Lighting pair's.
		const RE::BSShader* shader = familyKey ? input->drawFamilySource[d] : input->lighting;
		auto live = MakeLive(recipe.id, ShaderPrograms::SourcePathOf(*shader), *program,
			[family, familyKey, shader, key = a_key](ShaderPrograms::Rebuilt<ShaderPrograms::ForwardProgram> a_done) {
				if (familyKey)
					ShaderPrograms::Get().RebuildDrawFamilyForward(family, *shader, std::move(a_done));
				else
					ShaderPrograms::Get().RebuildForward(key.vertexDescriptor, key.pixelDescriptor, *shader, std::move(a_done));
			},
			build);
		f.entries.try_emplace(a_key).first->second.requested = true;
		(void)service.Request(std::move(recipe),
			[completions = completions, key = a_key, generation = f.generation, live = std::move(live)](const org::services::PipelineArtifact& a_artifact) {
				completions->forward.Push({ key, generation, a_artifact, live });
				WakePipelineLane();
			});
		f.counters.requested.fetch_add(1, std::memory_order_relaxed);
		lane.dirty = true;  // the catalog reports its program
	}

	void DrawPipelines::Impl::RecreateForward(const ForwardTargets& a_from, const ForwardTargets& a_to)
	{
		auto& f = lane.forward;
		// Start over (the new generation drops queued builds' completions); built ones are kept for the process: a recording may still
		// draw with them.
		logger::info("[DCLF] pipeline lane: forward targets colour {} depth {} -> colour {} depth {}; rebuilding {} forward pipelines", static_cast<int>(a_from.colour),
			static_cast<int>(a_from.depth), static_cast<int>(a_to.colour), static_cast<int>(a_to.depth), f.entries.size());
		ResetForward();
	}

	void DrawPipelines::Impl::ResetForward()
	{
		auto& f = lane.forward;
		// Their slots go with their entries (StartOver), and their registry entries with them.
		for (auto& entry : f.entries)
			if (entry.second.built)
				f.retired.push_back(std::move(entry.second.built));
		f.StartOver();
		lane.dirty = true;
	}

	org::services::PipelinePayload DrawPipelines::Impl::BuildRefraction(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, const ShaderPrograms::ShadowProgram* a_program,
		DXGI_FORMAT a_format, DXGI_FORMAT a_depthFormat, std::uint8_t a_writeMask, bool a_frontCCW)
	{
		auto stages = Parse<2>({ &a_program->vertex, &a_program->pixel });
		CheckBindings(stages[0], false);
		CheckBindings(stages[1], true);
		auto built = std::make_shared<rhi::PipelinePtr>();
		PipelineDesc desc(a_layout, a_program->vertex, a_program->pixel, a_depthFormat);
		desc.raster.rs.cull = rhi::CullMode::None;
		desc.raster.rs.frontCCW = a_frontCCW;
		desc.depth.ds.depthEnable = true;
		desc.depth.ds.depthWrite = false;
		desc.depth.ds.depthFunc = rhi::CompareOp::LessEqual;
		desc.blend.bs.attachments[0].enable = false;
		desc.blend.bs.attachments[0].writeMask = static_cast<rhi::ColorWriteEnable>(a_writeMask & 0xF);
		desc.Colour(a_format);
		desc.Create(a_device, *built, "refraction");
		return built;
	}

	org::services::PipelinePayload DrawPipelines::Impl::BuildForward(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::ForwardProgram* a_program,
		ForwardTargets a_targets, rhi::CullMode a_cull, bool a_frontCCW)
	{
		auto stages = Parse<2>({ &a_program->vertex, &a_program->pixel });
		CheckPulledBindings(stages[0], false);
		CheckPulledBindings(stages[1], true);
		auto built = std::make_shared<rhi::PipelinePtr>();
		PipelineDesc desc(a_zLayout, a_program->vertex, a_program->pixel, a_targets.depth);
		desc.raster.rs.cull = a_cull;
		desc.raster.rs.frontCCW = a_frontCCW;
		// The engine's depth mode 3 for the face's LOD: test and write, LESS_EQUAL.
		desc.depth.ds.depthEnable = true;
		desc.depth.ds.depthWrite = true;
		desc.depth.ds.depthFunc = rhi::CompareOp::LessEqual;
		// No blending, every channel written (the engine's write mask there is stale state; the cube's alpha is not read).
		desc.Colour(a_targets.colour);
		if (a_targets.colour2 != DXGI_FORMAT_UNKNOWN)
			desc.Colour(a_targets.colour2);
		desc.Create(a_device, *built, desc.targets.rt.count > 1 ? "first-person forward" : "forward");
		return built;
	}

	rhi::PipelineHandle ForwardPipelineOf(const PipelineCatalog* a_catalog, const ForwardPipelineKey& a_key, const ForwardTargets& a_targets)
	{
		if (!a_catalog)
			return {};
		const auto view = Extensions::ForwardKeyView(a_key.flags);
		if (view == Api::EngineView::Count || !(a_catalog->viewTargets[static_cast<std::size_t>(view)] == a_targets))
			return {};
		const auto* entry = a_catalog->FindForward(a_key);
		if (!entry || !entry->pipeline)
			return {};
		return (*static_cast<const rhi::PipelinePtr*>(entry->pipeline.get()))->GetHandle();
	}
}
