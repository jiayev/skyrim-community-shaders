// The shadow phases' readiness (render thread): the rasterizer state catalog, the view placements, the capability the scene
// lane reads, and a slot's readiness for a phase.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"
#include "EngineFixes/ShadowmapCascadeRasterizerFix.h"

namespace DCLF
{
	void ShadowEngine::BuildShadowCatalog()
	{
		// The engine's table is filled once the renderer is up; until then the catalog waits.
		auto& table = EngineRasterStates();
		std::vector<D3D11_RASTERIZER_DESC> engine, cascades;
		for (std::uint32_t bias = 0; bias < 12; ++bias) {
			// The entry at cull mode 1, else 0 (as CaptureShadowView). Fill and depth clip DCLF cannot express are skipped: such a
			// view stays the engine's.
			auto* entry = table[0][1][bias][0] ? table[0][1][bias][0] : table[0][0][bias][0];
			if (!entry)
				continue;
			D3D11_RASTERIZER_DESC desc{};
			entry->GetDesc(&desc);
			if (desc.FillMode != D3D11_FILL_SOLID || !desc.DepthClipEnable)
				continue;
			engine.push_back(desc);
			// The cascades draw with ShadowmapCascadeRasterizerFix's clones, each with the cascade's bias.
			for (std::uint32_t cascade = 0; cascade < ShadowmapRasterizerFix::CascadeCount(); ++cascade) {
				auto clone = desc;
				ShadowmapRasterizerFix::CascadeRasterDesc(clone, cascade);
				cascades.push_back(clone);
			}
		}
		if (engine.empty())
			return;
		auto& pipelines = DrawPipelines::Get();
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			const std::uint32_t renderMode = IsOcclusionMode(m) ? kOcclusionRenderMode : PassCapture::kFirstShadowMode + m;
			auto& states = readyStates[m];
			const auto add = [&](const D3D11_RASTERIZER_DESC& a_desc) {
				if (const std::uint32_t id = pipelines.ShadowRasterStateId(a_desc, renderMode)) {
					states.Add(id, false);
					// Only the sun's views have the volumetric class.
					if (m == kSunShadowMode)
						states.Add(id, true);
				}
			};
			for (const auto& desc : engine)
				add(desc);
			if (m == kSunShadowMode)
				for (const auto& desc : cascades)
					add(desc);
		}
		shadowCatalogBuilt = true;
		logger::info("[DCLF] shadow rasterizer state catalog: {} states ({} engine entries, {} cascade clones); sun {}, paraboloid {}, occlusion {}",
			pipelines.ShadowRasterStateCount(), engine.size(), cascades.size(), readyStates[kSunShadowMode].All().size(), readyStates[kParabolicShadowMode].All().size(),
			readyStates[kFirstOcclusionMode].All().size());
	}

	bool ShadowEngine::BuildShadowPlacements()
	{
		auto* renderer = globals::game::renderer;
		auto* plain = renderer ? EngineRasterStates()[0][1][0][0] : nullptr;
		if (!plain)
			return false;
		// Cascade c draws into slice c with its ShadowmapCascadeRasterizerFix clone; every other view with depth bias mode 0.
		const std::uint32_t plainState = EngineRasterStateId(0, 1, 0, 0, PassCapture::kFirstShadowMode + kSunShadowMode);
		(void)EngineRasterStateId(0, 1, 0, 0, PassCapture::kFirstShadowMode + kParabolicShadowMode);
		const std::uint32_t unclampedState = EngineRasterStateId(0, 1, 0, 0, PassCapture::kFirstShadowMode + kPlainShadowMode);
		const auto cascadeState = [&](std::uint32_t a_cascade) {
			if (a_cascade >= ShadowmapRasterizerFix::CascadeCount())
				return plainState;
			D3D11_RASTERIZER_DESC desc{};
			plain->GetDesc(&desc);
			ShadowmapRasterizerFix::CascadeRasterDesc(desc, a_cascade);
			return DrawPipelines::Get().ShadowRasterStateId(desc, PassCapture::kFirstShadowMode + kSunShadowMode);
		};
		std::vector<ShadowViewLayout> placements;
		const auto add = [&](std::uint32_t a_mode, std::uint32_t a_target, std::uint32_t a_slice, std::uint32_t a_y, std::uint32_t a_width, std::uint32_t a_height,
							 std::uint32_t a_state) {
			placements.push_back({ kFirstShadowViewSlot + static_cast<std::uint32_t>(placements.size()), a_mode, a_target, a_slice, 0u, a_y, a_width, a_height, a_state });
		};
		using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
		for (const auto [index, target] : { std::pair{ 0u, T::kSHADOWMAPS_ESRAM }, std::pair{ 1u, T::kSHADOWMAPS }, std::pair{ 2u, T::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM } }) {
			auto* texture = renderer->GetDepthStencilData().depthStencils[target].texture;
			if (!texture)
				continue;
			// Imported now, before any view draws into it.
			(void)ImportShadowDepth(index, target);
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			for (std::uint32_t slice = 0; slice < desc.ArraySize; ++slice) {
				if (index == 0) {
					add(kSunShadowMode, index, slice, 0, desc.Width, desc.Height, cascadeState(slice));
				} else if (index == 2) {
					// The cascades' volumetric copies.
					add(kSunShadowMode, index, slice, 0, desc.Width, desc.Height, plainState);
				} else {
					// Lights' map: a spot light takes a slice, clamped or plain (a light whose near clip leaves casters in front of it:
					// Window Shadows Ultimate's), a point light's paraboloid pair its two halves.
					add(kSunShadowMode, index, slice, 0, desc.Width, desc.Height, plainState);
					add(kPlainShadowMode, index, slice, 0, desc.Width, desc.Height, unclampedState);
					add(kParabolicShadowMode, index, slice, 0, desc.Width, desc.Height / 2, plainState);
					add(kParabolicShadowMode, index, slice, desc.Height / 2, desc.Width, desc.Height / 2, plainState);
				}
			}
		}
		if (placements.empty() || !plainState || !unclampedState)
			return false;
		shadowPlacements = std::move(placements);
		// A revision input: the next revision reserves the latch and slots (ReserveShadowLatch).
		shadowHost.PostRevisionInputs();
		logger::info("[DCLF] shadow view placements: {} (slots {} to {})", shadowPlacements.size(), shadowPlacements.front().slot, shadowPlacements.back().slot);
		return true;
	}

	std::size_t ShadowEngine::PlacementOf(const PendingView& a_view) const
	{
		// The view's mode's placement there first (a light slice has a clamped and a plain one); else any there (a defect).
		std::size_t found = SIZE_MAX;
		for (std::size_t p = 0; p < shadowPlacements.size(); ++p) {
			const auto& placement = shadowPlacements[p];
			if (placement.target == a_view.targetIndex && placement.slice == a_view.slice && placement.x == a_view.x && placement.y == a_view.y &&
				placement.width == a_view.width && placement.height == a_view.height) {
				if (placement.modeIndex == a_view.modeIndex)
					return p;
				if (found == SIZE_MAX)
					found = p;
			}
		}
		return found;
	}

	void ShadowEngine::NoteCapability(bool a_defect, std::string a_cause)
	{
		++capabilityStats.changes;
		if (a_defect) {
			++capabilityStats.defects;
			logger::warn("[DCLF] shadow capability changed: {} <- PHASES", a_cause);
		} else {
			logger::info("[DCLF] shadow capability: {}", a_cause);
		}
		capabilityStats.lastCause = std::move(a_cause);
	}

	void IndirectDraws::UpdateShadowCapability()
	{
		// The scene side's alone (the commit's capability, the scene lane's shadow lookups, the builds ahead): under the newest toggles,
		// which the next publication is committed under; the frame's shadow epochs read the frame's (ActiveToggles) and readyModes.
		auto& i = *impl->shadowEngine;
		std::uint8_t phases = 0;
		if (!failed && SceneToggles().shadows && DrawPipelines::Get().Enabled() && globals::game::utilityShader && i.SetupShadow()) {
			if (!i.shadowCatalogBuilt) {
				i.BuildShadowCatalog();
				if (i.shadowCatalogBuilt) {
					// The pipelines' format is the depth-stencil view's, not the typeless texture's.
					if (auto* renderer = globals::game::renderer)
						if (auto* view = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].views[0]) {
							D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
							view->GetDesc(&desc);
							i.shadowTargetFormat = desc.Format;
						}
					// Caster modes: sun and spot (clamped) and point (paraboloid). No view uses the plain mode.
					i.readyModes[kSunShadowMode] = i.readyModes[kParabolicShadowMode] = true;
					// Both occlusion maps draw into the precipitation occlusion target (an ImportedTexture map's own texture swapped in), in
					// its format.
					if (auto* renderer = globals::game::renderer)
						if (auto* view = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP].views[0]) {
							D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
							view->GetDesc(&desc);
							for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
								i.occlusion[v].dsvFormat = desc.Format;
								i.readyModes[OcclusionModeOf(v)] = true;
							}
							// Imported now, in the maps' import order: a later import would rebuild the graph mid-play.
							using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
							for (const auto map : Extensions::ShadowViewsSet().importOrder) {
								const auto v = static_cast<std::uint32_t>(map);
								const auto& entry = Extensions::OcclusionMapOf(v);
								if (!entry.registered)
									continue;
								if (entry.target == Api::OcclusionTarget::EngineMask) {
									(void)i.ImportShadowDepth(OcclusionDepthTarget(v), T::kPRECIPITATION_OCCLUSION_MAP);
									continue;
								}
								Api::OcclusionTargetImport request;
								entry.importTarget(request);
								if (request.supplied)
									(void)i.ImportDepthTexture(OcclusionDepthTarget(v), request.texture, request.view, T::kPRECIPITATION_OCCLUSION_MAP);
							}
						}
					i.NoteCapability(false, "the casters' and occlusion maps' modes and the catalog set up");
				}
			}
			if (i.shadowCatalogBuilt && i.shadowPlacements.empty() && i.BuildShadowPlacements())
				i.NoteCapability(false, fmt::format("{} shadow view placements set up", i.shadowPlacements.size()));
			if (i.shadowCatalogBuilt && !i.shadowPlacements.empty() && i.shadowTargetFormat != DXGI_FORMAT_UNKNOWN) {
				const auto& phaseOfMode = Extensions::ShadowViewsSet().phaseOfMode;
				for (std::uint32_t m = 0; m < kFirstOcclusionMode; ++m)
					phases |= i.readyModes[m] ? phaseOfMode[m] : 0;
				for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
					phases |= i.readyModes[OcclusionModeOf(v)] && i.occlusion[v].dsvFormat != DXGI_FORMAT_UNKNOWN && Extensions::OcclusionOn(SceneToggles(), v) ?
					              phaseOfMode[OcclusionModeOf(v)] :
					              0;
			}
		}
		// What the builds ahead and the scene lane's shadow lookups are resolved for (PostLookupInputs).
		auto& views = i.lastShadow;
		views.known = phases != 0;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			views.modes[m] = phases && i.readyModes[m] && (!IsOcclusionMode(m) || Extensions::OcclusionOn(SceneToggles(), OcclusionOfMode(m)));
			views.rasterStates[m] = views.modes[m] ? i.readyStates[m] : ModeRasterStates{};
		}
		views.dsvFormat = i.shadowTargetFormat;
		i.shadowCapability.store(phases, std::memory_order_release);
	}

	std::uint8_t IndirectDraws::ShadowCapability() const
	{
		return failed ? 0 : impl->shadowEngine->shadowCapability.load(std::memory_order_acquire);
	}

	bool IndirectDraws::PhaseReady(const void* a_tables, const Lookups& a_lookups, std::uint32_t a_slot, std::uint8_t a_phase, std::uint32_t* a_why) const
	{
		auto no = [a_why](std::uint32_t a_reason) {
			if (a_why)
				*a_why = a_reason;
			return false;
		};
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		const auto& lookups = a_lookups;
		// A LodTechniques view's phase (the reflection's): its slot's forward pipeline.
		if (a_phase & Extensions::Views().lodPhases)
			return impl->LodPhaseReady(a_phase, tables, a_slot) || no(8);
		if (a_slot >= tables.objects.size() || a_slot >= tables.shadowTechnique.size())
			return no(7);
		const auto& object = tables.objects[a_slot];
		if (object.geometryIndex >= tables.geometries.size())
			return no(7);
		// Past the scene buffers' fit there is no input until they grow.
		if (!FitsScene(&tables, a_slot, true))
			return no(12);
		// The modes and states the lane's lookups were resolved for (immutable), never the render thread's readyModes/readyStates,
		// which grow as it sees new ones. None yet: the caster waits.
		const auto* inputs = static_cast<const ShadowLookupInputs*>(lookups.shadowInputs.get());
		if (!inputs)
			return no(7);
		// Fade is culled on the GPU (FadedOutOfOcclusion), so nothing of the engine's cull is waited for. As BuildKeptShadow: every
		// state of the object's class in the phase's modes has its pipeline, and an alpha-tested technique's diffuse is imported.
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (Extensions::ShadowViewsSet().phaseOfMode[m] != a_phase || !inputs->modes[m])
				continue;
			const std::uint32_t technique = ModeTechnique(tables, m, a_slot);
			if (!technique)
				continue;
			const auto& states = inputs->rasterStates[m].Of(VolumetricClass(m, object.flags));
			if (states.empty())
				continue;
			const ShadowPipelineKey key{ BaseTechnique(tables, m, a_slot), (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(tables.geometries[object.geometryIndex].vertexDesc) };
			const auto slot = lookups.shadowSlots.find(key);
			if (slot == lookups.shadowSlots.end())
				return no(7);
			for (const std::uint32_t state : states)
				if (lookups.ShadowMapPipeline(m, state, slot->second) == Lookups::kNone)
					return no(7);
			if (technique & 0x80) {
				auto* diffuse = a_slot < tables.shadowDiffuse.size() ? tables.shadowDiffuse[a_slot] : nullptr;
				const auto texture = diffuse ? lookups.shadowTextures.find(diffuse) : lookups.shadowTextures.end();
				if (texture == lookups.shadowTextures.end() || texture->second == Lookups::kNone)
					return no(13);
			}
		}
		return true;
	}
}
