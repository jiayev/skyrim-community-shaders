// DrawPipelines' shadow set (ShadowPipelines): the shadow views' rasterizer states, pipelines and set versions, and their
// indirect state.

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
		/** @brief A shadow request's casters, for the on-demand warnings. */
		std::string DescribeShadowCasters(const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
		{
			return fmt::format("for the casters of key technique {:08X}, raster {:X}, vertex layout {:016X} ({})", a_casterKey.technique, a_casterKey.rasterFlags,
				a_casterKey.vertexLayout, a_occlusion == ~0u ? std::string("the shadow views") : fmt::format("occlusion view {}", a_occlusion));
		}
	}

	inline namespace DrawPipelinesDetail
	{
		// The shadow views': the draw's words name its material row, the view's push words and frame record the rest.
		std::vector<rhi::LayoutBindingRange> ShadowLayoutRanges()
		{
			const auto index = rhi::LayoutRangeSource::IndirectIndex;
			const auto pixelStage = rhi::ShaderStage::Pixel;
			auto fromShadowFrameRecord = [](rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 1, kShadowPushFrameRecord); };
			std::vector<rhi::LayoutBindingRange> shadowRanges;
			for (const bool pixel : { false, true }) {
				const auto stage = pixel ? rhi::ShaderStage::Pixel : rhi::ShaderStage::Vertex;
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					const std::uint32_t b = kBindingShiftB + r;
					if (!pixel && r == 1)
						shadowRanges.push_back(PushedRange(b, stage, 0, kDrawPushMaterialRow));  // the material row itself
					else if (r == 0)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushViewBlock));
					else if (r == kPerFrameVertexRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushPerFrame));
					else if (r == kSharedDataRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushSharedData));
					else if (r == kFeatureDataRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushFeatureData));
					else
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushZeros));
				}
			}
			// The diffuse (t0) from the draw's row; t1 and up, the samplers and the vertex stage's buffers from the frame record.
			shadowRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftT, 1, pixelStage, index, kShadowRowDiffuseOffset)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kBindingShiftT + 1, kTextureRegisters - 1, pixelStage, index, offsetof(DrawBindings, textures) + 4)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kBindingShiftS, kSamplerRegisters, pixelStage, index, offsetof(DrawBindings, samplers), true)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kShadingBufferRegister })));
			return shadowRanges;
		}
	}

	void DrawPipelines::RequestShadow(const ShadowPipelineKey& a_viewKey, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
	{
		impl->completions->shadowRequests.Push({ a_viewKey, a_casterKey, a_occlusion });
		WakePipelineLane();
	}

	void DrawPipelines::SetShadowInputs(DXGI_FORMAT a_format, RE::BSShader& a_utility)
	{
		auto& input = impl->renderInput;
		// The view format of a shadow map the engine has drawn: unknown until then, and not posted.
		if (a_format == DXGI_FORMAT_UNKNOWN || (input.shadowFormat == a_format && input.utility == &a_utility) || !Enabled())
			return;
		input.shadowFormat = a_format;
		input.utility = &a_utility;
		impl->PostInput(*this);
	}

	std::string DrawPipelines::Impl::NearestShadowKey(const ShadowPipelineKey& a_key) const
	{
		const ShadowPipelineKey* nearest = nullptr;
		KeyDifference best;
		for (const auto& [key, entry] : lane.shadow.entries) {
			if (key == a_key)
				continue;
			KeyDifference difference;
			difference.Field("technique", key.technique, a_key.technique);
			difference.Field("raster", key.rasterFlags, a_key.rasterFlags);
			difference.Field("vertex layout", key.vertexLayout, a_key.vertexLayout);
			difference.Field("view state", key.viewState, a_key.viewState);
			if (!nearest || difference.bits < best.bits) {
				nearest = &key;
				best = std::move(difference);
			}
		}
		if (!nearest)
			return lane.shadow.recreatedBy.empty() ? std::string("the first shadow pipeline") :
			                                         fmt::format("no other shadow pipeline since the set was recreated ({})", lane.shadow.recreatedBy);
		return fmt::format("nearest requested shadow pipeline differs in {}", best.fields);
	}

	void DrawPipelines::Impl::TryRequestShadow(const ShadowPipelineKey& a_key)
	{
		auto& s = lane.shadow;
		if (s.entries.contains(a_key))
			return;  // requested, or failed for good
		// An inexpressible view rasterizer state has id 0: its views stay native.
		if (a_key.viewState == 0) {
			s.FailForGood(a_key);
			lane.dirty = true;
			return;
		}
		// The view's rasterizer state reaches the lane with the inputs after the render thread registers it.
		const auto* input = lane.input.get();
		if (!input || !input->utility || input->shadowFormat == DXGI_FORMAT_UNKNOWN || !input->shadowStates || a_key.viewState > input->shadowStates->size() ||
			!shadowLayout || !shadowDrawSignature) {
			s.blocked.push_back(a_key);
			return;
		}
		const auto asked = s.asked.find(a_key);
		const std::string casters = asked != s.asked.end() ? DescribeShadowCasters(asked->second.casterKey, asked->second.occlusion) : std::string("for no recorded caster");
		// As TryRequest: each build is logged with its casters and nearest relative.
		std::uint8_t onDemand = 0;
		bool programFailed = false;
		const auto* program = ShaderPrograms::Get().FindShadow(a_key.technique, *input->utility, true, nullptr, &onDemand, &programFailed);
		if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Utility {} of technique {:08X} that no precompile requested ({}), {}", ShaderPrograms::OnDemandStages(onDemand),
				a_key.technique, SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle",
				casters);
		if (programFailed) {
			// Its casters stay native in this key's views.
			s.FailForGood(a_key);
			lane.dirty = true;
			return;
		}
		if (!program) {
			s.programWaiters[a_key.technique].push_back(a_key);
			return;
		}
		if (s.Full()) {
			s.blocked.push_back(a_key);
			return;
		}
		const ShadowRasterState rasterState = (*input->shadowStates)[a_key.viewState - 1];
		const DXGI_FORMAT depthFormat = input->shadowFormat;
		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.shadow.{:08X}.{:X}.{:016X}.{}", a_key.technique, a_key.rasterFlags, a_key.vertexLayout, a_key.viewState);
		recipe.shaderKey = ShadowPipelineKeyHash{}(a_key);
		// No vertex input: never an artifact built with an input layout (kPulledVerticesSalt).
		recipe.fixedFunctionKey = static_cast<std::uint64_t>(depthFormat) ^ kPulledVerticesSalt;
		recipe.build = [device = device, layout = ShadowLayout(), key = a_key, program, depthFormat, rasterState] {
			return BuildShadow(device, layout, key, program, depthFormat, rasterState);
		};
		s.entries.try_emplace(a_key);
		(void)service.Request(std::move(recipe), [completions = completions, key = a_key, generation = s.generation](const org::services::PipelineArtifact& a_artifact) {
			completions->shadow.Push({ key, generation, a_artifact });
			WakePipelineLane();
		});
		s.counters.requested.fetch_add(1, std::memory_order_relaxed);
		logger::warn("[DCLF] on-demand pipeline build: shadow technique {:08X}, raster {:X}, vertex layout {:016X}, view state {}, format {}, {}; {}", a_key.technique,
			a_key.rasterFlags, a_key.vertexLayout, a_key.viewState, static_cast<int>(depthFormat), casters, NearestShadowKey(a_key));
	}

	void DrawPipelines::Impl::RecreateShadowSet(DXGI_FORMAT a_from, DXGI_FORMAT a_to)
	{
		auto& s = lane.shadow;
		// Every shadow pipeline depends on the format: start over (the new generation drops queued builds' completions).
		logger::info("[DCLF] pipeline lane: shadow map format {} -> {}; rebuilding {} shadow pipelines", static_cast<int>(a_from), static_cast<int>(a_to),
			s.setPipelines.size());
		s.StartOver();
		s.versions.Retire();
		s.setPipelines.clear();
		s.layouts.clear();
		s.discards.clear();
		s.recreatedBy = fmt::format("shadow map format {} -> {}", static_cast<int>(a_from), static_cast<int>(a_to));
		lane.dirty = true;
	}

	void DrawPipelines::Impl::AdmitShadow()
	{
		auto& s = lane.shadow;
		// As AdmitLane.
		completions->shadow.Drain([&](ShadowDone&& a_done) {
			if (a_done.generation != s.generation)
				return;
			const auto& key = a_done.key;
			const auto& artifact = a_done.artifact;
			const auto found = s.entries.find(key);
			if (found == s.entries.end())
				return;
			auto& entry = found->second;
			lane.dirty = true;
			if (!artifact) {
				entry.failed = true;
				++s.failedEntries;
				s.counters.failed.fetch_add(1, std::memory_order_relaxed);
				if (s.loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] shadow pipeline technique {:08X} layout {:016X} failed: {}", key.technique, key.vertexLayout, artifact.error);
				return;
			}
			const auto* built = static_cast<const Built*>(artifact.payload.get());
			entry.slot = static_cast<std::uint32_t>(s.setPipelines.size());
			s.setPipelines.push_back(artifact.payload);
			s.layouts.push_back(key.vertexLayout);
			s.discards.push_back(built->discards ? 1 : 0);
		});
		const auto admitted = static_cast<std::uint32_t>(s.setPipelines.size());
		if (!admitted)
			return;
		PublishAdmitted(s, s.versions, admitted, "shadow pipeline set", [&](ShadowSetVersion& a_version) {
			a_version.pipelines.assign(s.setPipelines.begin(), s.setPipelines.begin() + admitted);
			a_version.layouts.assign(s.layouts.begin(), s.layouts.begin() + admitted);
			a_version.discards.assign(s.discards.begin(), s.discards.begin() + admitted);
			return true;
		}, [](const ShadowPipelineKey&) {});
	}

	org::services::PipelinePayload DrawPipelines::Impl::BuildShadow(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, ShadowPipelineKey a_key,
		const ShaderPrograms::ShadowProgram* a_program, DXGI_FORMAT a_depthFormat, ShadowRasterState a_state)
	{
		auto [vertex, pixel] = Parse<2>({ &a_program->vertex, &a_program->pixel });
		CheckBindings(vertex, false);
		CheckBindings(pixel, true);
		auto built = std::make_shared<Built>();
		AddUsage(vertex, false, built->usage[kColorVariant]);
		AddUsage(pixel, true, built->usage[kColorVariant]);
		// The pulling vertex stage (DCLF_PULLED) has no vertex inputs, so the pipeline has no vertex input state; it is bound by its
		// draws, not from an execution set.
		PipelineDesc desc(a_layout, a_program->vertex, a_program->pixel, a_depthFormat);
		desc.raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : a_state.cull;
		desc.raster.rs.frontCCW = a_state.frontCCW;
		// The integer DepthBias passes through as the constant factor: the shadow maps are D16_UNORM, where Vulkan's default
		// representation (the least representable value) is what D3D11 and DXVK mean by it.
		desc.raster.rs.depthBias = a_state.depthBias;
		desc.raster.rs.depthBiasClamp = a_state.depthBiasClamp;
		desc.raster.rs.slopeScaledDepthBias = a_state.slopeScaledDepthBias;
		desc.depth.ds.depthEnable = true;
		desc.depth.ds.depthWrite = true;
		desc.depth.ds.depthFunc = rhi::CompareOp::Less;
		desc.Create(a_device, built->pipelines[kColorVariant], "shadow");
		built->discards = FragmentDefersDepth(a_program->pixel);
		return built;
	}

	std::uint32_t DrawPipelines::ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode)
	{
		// The scissor is not part of it: the epoch's pass is bounded by the view's viewport.
		if (a_desc.FillMode != D3D11_FILL_SOLID || !a_desc.DepthClipEnable) {
			if (impl->loggedShadowRasterFailures++ < kMaxLoggedFailures)
				logger::warn("[DCLF] shadow view rasterizer state not expressible (fill {}, depth clip {}); the view stays native",
					static_cast<int>(a_desc.FillMode), a_desc.DepthClipEnable);
			return 0;
		}
		Impl::ShadowRasterState state;
		state.depthBias = static_cast<float>(a_desc.DepthBias);
		state.depthBiasClamp = a_desc.DepthBiasClamp;
		state.slopeScaledDepthBias = a_desc.SlopeScaledDepthBias;
		state.cull = a_desc.CullMode == D3D11_CULL_NONE ? rhi::CullMode::None : a_desc.CullMode == D3D11_CULL_FRONT ? rhi::CullMode::Front : rhi::CullMode::Back;
		state.frontCCW = a_desc.FrontCounterClockwise != FALSE;
		auto& states = impl->shadowRasterStates;
		for (std::size_t i = 0; i < states.size(); ++i)
			if (states[i] == state)
				return static_cast<std::uint32_t>(i + 1);
		states.push_back(state);
		logger::info("[DCLF] shadow view rasterizer state {} (mode {:#x}): depth bias {} (clamp {}, slope {}), cull {}, front {}", states.size(), a_renderMode,
			state.depthBias, state.depthBiasClamp, state.slopeScaledDepthBias, static_cast<int>(a_desc.CullMode), a_desc.FrontCounterClockwise ? "CCW" : "CW");
		// Keys of this id wait in the lane until it arrives with the frame's inputs.
		impl->renderInput.shadowStates = std::make_shared<const std::vector<Impl::ShadowRasterState>>(states);
		if (Enabled())
			impl->PostInput(*this);
		return static_cast<std::uint32_t>(states.size());
	}

	std::uint32_t DrawPipelines::ShadowRasterStateCount() const
	{
		return static_cast<std::uint32_t>(impl->shadowRasterStates.size());
	}

	ShadowIndirectState GetShadowIndirectState(const PipelineCatalog& a_catalog, DXGI_FORMAT a_shadowFormat)
	{
		// a_catalog's shadow set version (its lookups' indices are of it); none once the shadow map format differs from the set's.
		const auto& pipelines = DrawPipelines::Get();
		const auto& impl = *pipelines.impl;
		ShadowIndirectState state;
		if (!a_catalog.shadowSetVersion || a_catalog.shadowFormat != a_shadowFormat || !impl.layout || !impl.shadowLayout)
			return state;
		auto version = std::static_pointer_cast<const DrawPipelines::Impl::ShadowSetVersion>(a_catalog.shadowSetVersion);
		state.layout = impl.ShadowLayout();
		state.pipelines.reserve(version->pipelines.size());
		for (const auto& payload : version->pipelines)
			state.pipelines.push_back(static_cast<const DrawPipelines::Impl::Built*>(payload.get())->pipelines[kColorVariant]->GetHandle());
		state.vertexLayouts = version->layouts;
		state.discards = version->discards;
		state.drawSignature = impl.shadowDrawSignature->GetHandle();
		state.version = std::move(version);
		state.valid = true;
		return state;
	}

	ShadowIndirectState FrameShadowIndirectState()
	{
		const auto* catalog = FrameCatalog();
		return catalog ? GetShadowIndirectState(*catalog, DrawPipelines::Get().ShadowFormat()) : ShadowIndirectState{};
	}

	DXGI_FORMAT DrawPipelines::ShadowFormat() const
	{
		return impl->renderInput.shadowFormat;
	}
}
