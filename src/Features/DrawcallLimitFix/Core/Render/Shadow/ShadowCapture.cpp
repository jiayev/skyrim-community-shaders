// The shadow and occlusion views' captures (render thread): at the engine's shadow and occlusion hooks, each view's target,
// viewport, camera and constants, DCLF's own where it has them; the views' coverage and the occlusion maps' predicted layout.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	namespace Draws
	{
		/** @brief DCLF's id for the engine rasterizer state at (fill, cull, bias, scissor); 0 when out of the table or empty. */
		std::uint32_t EngineRasterStateId(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_bias, std::uint32_t a_scissor, std::uint32_t a_renderMode)
		{
			if (a_fill >= 2 || a_cull >= 3 || a_bias >= 12 || a_scissor >= 2)
				return 0;
			auto* engineState = EngineRasterStates()[a_fill][a_cull][a_bias][a_scissor];
			if (!engineState)
				return 0;
			D3D11_RASTERIZER_DESC desc{};
			engineState->GetDesc(&desc);
			return DrawPipelines::Get().ShadowRasterStateId(desc, a_renderMode);
		}

		/** @brief Reads the depth format, viewport and eye the engine just drew a view with from the renderer's state. */
		void CaptureViewTarget(PendingView& a_view, std::uint32_t a_target)
		{
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsv->GetDesc(&dsvDesc);
				a_view.dsvFormat = dsvDesc.Format;
			}
			a_view.x = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftX));
			a_view.y = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftY));
			a_view.width = static_cast<std::uint32_t>(shadowState.viewPort.Width);
			a_view.height = static_cast<std::uint32_t>(shadowState.viewPort.Height);
			a_view.minDepth = shadowState.viewPort.MinDepth;
			a_view.maxDepth = shadowState.viewPort.MaxDepth;
			a_view.eye = shadowState.posAdjust.getEye();
		}

		/**
		 * @brief Replaces the captured viewport and eye with DCLF's own (its source's camera, from the light's
		 * UpdateCamera): port times target size. False without DCLF's view this frame (captured values stand).
		 */
		bool ApplyOwnViewport(PendingView& a_view, const ShadowCameras::Camera* a_cascade, std::uint32_t a_descriptor, std::uint32_t a_target, Diagnostics::ShadowView a_kind)
		{
			Diagnostics::Notify(Diagnostics::OwnShadowViewport{ a_view, a_cascade, a_descriptor, a_target, a_kind });
			auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].texture;
			if (!a_cascade || !texture)
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const auto port = ShadowCameras::Viewport(a_cascade->port, desc.Width, desc.Height);
			const RE::NiPoint3 eye{ a_cascade->eye[0], a_cascade->eye[1], a_cascade->eye[2] };
			a_view.x = port[0];
			a_view.y = port[1];
			a_view.width = port[2];
			a_view.height = port[3];
			a_view.minDepth = 0.0f;
			a_view.maxDepth = 1.0f;
			a_view.eye = eye;
			return true;
		}

		/**
		 * @brief Sets the view's VS_PerFrame block and caster planes from DCLF's own camera (ShadowCameras::Block). a_engineBlock: the
		 * engine's block was captured too, for the observers.
		 */
		void ApplyOwnCamera(PendingView& a_view, const ShadowCameras::Camera& a_cascade, std::uint32_t a_descriptor, Diagnostics::ShadowView a_kind, bool a_engineBlock)
		{
			if (a_engineBlock)
				Diagnostics::Notify(Diagnostics::OwnShadowCamera{ a_view, a_cascade, a_descriptor, a_kind });
			std::array<std::byte, ShadowCameras::kBlockBytes> block{};
			ShadowCameras::Block(a_cascade, block.data());
			const auto* mine = reinterpret_cast<const float*>(block.data());
			std::memcpy(a_view.perFrame.data(), block.data(), block.size());
			a_view.perFrameBytes = static_cast<std::uint32_t>(block.size());
			std::memcpy(a_view.viewProj.data(), mine + 32, sizeof(float) * 16);
			a_view.hasViewProj = true;
			for (std::uint32_t q = 0; q < 6; ++q)
				for (std::uint32_t i = 0; i < 4; ++i)
					a_view.cullPlanes[q][i] = a_cascade.caster.plane[q][i];
			a_view.cullPlaneMask = a_cascade.caster.mask;
		}

		/**
		 * @brief Draws a focus view over its whole slice. The engine draws it into a sub-rectangle sized from the target's distance
		 * (BSShadowDirectionalLight::sub), new most frames, while DCLF's placements are per slice. The sub-rectangle's viewport
		 * transform moves into the projection (x' = x * sw/W + (sw + 2 sx)/W - 1, y' = y * sh/H + 1 - (sh + 2 sy)/H on c4, c8, c12
		 * and the culling view-projection), so casters land on the same pixels; the rest of the slice is sampled by no receiver.
		 */
		void FocusToSlice(PendingView& a_view, const ShadowCameras::Camera& a_cascade, std::uint32_t a_target)
		{
			auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].texture;
			if (!texture)
				return;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const float w = static_cast<float>(desc.Width), h = static_cast<float>(desc.Height);
			// Renderer::UpdateViewPort's rectangle, unrounded.
			const auto& port = a_cascade.port;
			const float sx = port[0] * w, sy = (1.0f - port[2]) * h, sw = (port[1] - port[0]) * w, sh = (port[2] - port[3]) * h;
			const float ax = sw / w, bx = (sw + 2.0f * sx) / w - 1.0f;
			const float ay = sh / h, by = 1.0f - (sh + 2.0f * sy) / h;
			auto* block = reinterpret_cast<float*>(a_view.perFrame.data());
			for (const std::uint32_t c : { 4u, 8u, 12u }) {
				float* row0 = block + c * 4;
				float* row1 = row0 + 4;
				const float* row3 = row0 + 12;
				for (std::uint32_t i = 0; i < 4; ++i) {
					row0[i] = ax * row0[i] + bx * row3[i];
					row1[i] = ay * row1[i] + by * row3[i];
				}
			}
			std::memcpy(a_view.viewProj.data(), block + 32, sizeof(float) * 16);
		}

		/**
		 * @brief DCLF's own camera for a view, from its source (Api::ShadowSourceContribution::camera, by its kind:
		 * Extensions::ShadowViewSet::sourceOf: the sun's cascades, the kept local lights' views, the focus views), with the observers' kind
		 * and a focus view's target. False for a view of no source (its captured values stand).
		 */
		bool OwnViewOf(const ShadowViews::View& a_view, std::uint32_t a_sceneFrame, PendingView& a_pending, const ShadowCameras::Camera*& a_own,
			Diagnostics::ShadowView& a_kind)
		{
			const auto source = Extensions::ShadowViewsSet().sourceOf[static_cast<std::size_t>(a_view.kind)][a_view.focus ? 1 : 0];
			if (source >= Api::ShadowSource::Count)
				return false;
			Api::ShadowSourceCamera request{ a_view.light, a_view.descriptor, a_sceneFrame };
			a_own = Extensions::SourceCamera(source, request);
			a_pending.portalContext = a_own && a_own->valid ? a_own->portalContext : 0u;
			switch (source) {
			case Api::ShadowSource::Spot:
			case Api::ShadowSource::Point:
				a_kind = Diagnostics::ShadowView::Light;
				break;
			case Api::ShadowSource::Focus:
				a_kind = Diagnostics::ShadowView::Focus;
				a_pending.focusTarget = request.focusTarget;
				a_pending.focus = true;
				break;
			default:
				break;
			}
			return true;
		}

		/**
		 * @brief The engine's caster volume: BSShadowDirectionalLight::UpdateCamera builds it from the main frustum's corners and the
		 * light direction into the descriptor's culling process, and the accumulation's cull (FUN_1414f0920) tests it on top of the shadow
		 * frustum. Much tighter than the orthographic box, it keeps casters no visible receiver can see out of the far cascade. A focus
		 * view has no culling process (its descriptor indexes the focus array).
		 */
		void CaptureCasterVolume(PendingView& a_view, const ShadowViews::View& a_shadowView)
		{
			if (auto& lightData = const_cast<RE::BSShadowLight*>(a_shadowView.light)->GetRuntimeData();
				!a_shadowView.focus && a_shadowView.descriptor < lightData.shadowmapDescriptors.size()) {
				const auto* process = lightData.shadowmapDescriptors[a_shadowView.descriptor].cullingProcess;
				if (process && process->doCustomCullPlanes) {
					const auto& planes = process->customCullPlanes;
					for (std::uint32_t p = 0; p < RE::NiFrustumPlanes::Planes::kTotal; ++p) {
						a_view.cullPlanes[p][0] = planes.cullingPlanes[p].normal.x;
						a_view.cullPlanes[p][1] = planes.cullingPlanes[p].normal.y;
						a_view.cullPlanes[p][2] = planes.cullingPlanes[p].normal.z;
						a_view.cullPlanes[p][3] = planes.cullingPlanes[p].constant;
					}
					a_view.cullPlaneMask = planes.activePlanes.underlying() & 0x3Fu;
				}
			}
		}

		/** @brief Capture check: the block's CameraPosAdjust (c40) against the renderer's eye for the view, reported every 1000 views. */
		void CheckCapturedEye(const PendingView& a_view, std::uint32_t a_viewId, std::uint32_t a_renderMode)
		{
			static std::uint64_t views = 0, fallback = 0, differ = 0;
			static float largest = 0.0f;
			static std::string first;
			++views;
			const auto* block = reinterpret_cast<const float*>(a_view.perFrame.data());
			if (a_view.perFrameBytes >= 164 * sizeof(float)) {
				const float d = std::max({ std::abs(block[160] - a_view.eye.x), std::abs(block[161] - a_view.eye.y), std::abs(block[162] - a_view.eye.z) });
				if (d > 0.01f) {
					++differ;
					if (first.empty())
						first = fmt::format("view {} mode {:#x}: block ({:.1f} {:.1f} {:.1f}), renderer ({:.1f} {:.1f} {:.1f})", a_viewId, a_renderMode, block[160], block[161],
							block[162], a_view.eye.x, a_view.eye.y, a_view.eye.z);
				}
				largest = std::max(largest, d);
			}
			if (!ConstantMirror::Get().Contents(*globals::game::perFrame.get()).size())
				++fallback;
			if (views == 1000) {
				logger::info("[DCLF] shadow view capture: {} views, {} from the cached copy, {} whose b12 eye is not the renderer's (largest {:.2f}){}{}", views, fallback, differ,
					largest, differ ? " <- CAPTURE" : " <- OK", first.empty() ? "" : "; first " + first);
				views = fallback = differ = 0;
				largest = 0.0f;
				first.clear();
			}
		}

		/**
		 * @brief Captures VS_PerFrame (b12) now, before the next view rewrites it: from the mirror, or CS's cached copy
		 * (CacheFramebuffer) until the mirror has seen a write. Its view-projection is the draws'.
		 */
		void CapturePerFrame(PendingView& a_view)
		{
			auto& mirror = ConstantMirror::Get();
			if (auto* perFrame = *globals::game::perFrame.get()) {
				mirror.Watch(perFrame);
				const auto contents = mirror.Contents(perFrame);
				if (contents.size() >= 48 * sizeof(float)) {
					a_view.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), a_view.perFrame.size()));
					std::memcpy(a_view.perFrame.data(), contents.data(), a_view.perFrameBytes);
				}
			}
			if (!a_view.perFrameBytes) {
				const auto& cached = globals::game::frameBufferCached.data;
				static_assert(sizeof(cached) >= 48 * sizeof(float) && sizeof(cached) <= 1024);
				a_view.perFrameBytes = sizeof(cached);
				std::memcpy(a_view.perFrame.data(), &cached, sizeof(cached));
			}
			std::memcpy(a_view.viewProj.data(), reinterpret_cast<const float*>(a_view.perFrame.data()) + 32, sizeof(float) * 16);
			a_view.hasViewProj = true;
		}
	}

	std::uint32_t IndirectDraws::ShadowViewCapacity()
	{
		// None when DCLF draws no shadows: the engine draws every view.
		auto& engine = *impl->shadowEngine;
		if (failed || !engine.SetupShadow() || engine.shadowPlacements.empty())
			return 0;
		// Re-import shadow maps the engine replaced; a new import is built at the next build point.
		if (auto* renderer = globals::game::renderer) {
			using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
			for (const auto [index, target] : { std::pair{ 0u, T::kSHADOWMAPS_ESRAM }, std::pair{ 1u, T::kSHADOWMAPS }, std::pair{ 2u, T::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM } })
				if (renderer->GetDepthStencilData().depthStencils[target].texture)
					(void)engine.ImportShadowDepth(index, target);
		}
		// None while the built graph lacks a new import, or the frame has no claims.
		if (auto* host = RenderGraphRuntime::Get().Host(); !host || host->RebuildRequested() || Scene::FrameView::Get().NotDrawingYet())
			return 0;
		// Every view: each draws at its placement's slot.
		return UINT32_MAX;
	}

	void IndirectDraws::DecideShadowCoverage()
	{
		// Before the engine keeps its first shadow light (ShadowViews::BeginCasters), so that every registration and the sun's claims
		// test this frame's verdict. Nothing it reads changes before the shadow draw: the frame's start adopted the revision and installed
		// the publication.
		if (failed || !impl->shadowEngine->shadow || impl->shadowEngine->shadowPlacements.empty())
			return;
		// The views are DCLF's once the selected revision has the shadow epoch's recording (shaped for all placements) on the built
		// graph and the installed publication has a shadow payload; until then (startup, a toggle) the engine's. Both are trusted.
		if (!impl->EpochCovered(EpochOfShape(ShapeKind::Shadow, kShadowShape)) || !impl->installedDraws || !impl->installedDraws->shadow)
			ShadowViews::Get().SetDrawable(false);
	}

	void IndirectDraws::BeginShadowFrame()
	{
		// A frame whose listed views DCLF would draw but does not (DecideShadowCoverage).
		if (ShadowViews::Get().Unrecorded())
			++impl->shadowUnrecorded;
		impl->shadowEngine->pendingViews.clear();
	}

	void IndirectDraws::CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode)
	{
		// Captures where the engine just drew the view and its constants, for ExecuteShadowFrame. Nothing is drawn here.
		if (!ActiveToggles().shadows || failed)
			return;
		ScopedPerfEvent event("CS DCLF: shadow view capture");
		const auto start = std::chrono::steady_clock::now();
		auto& engine = *impl->shadowEngine;
		++shadowStats.views;
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		auto notReady = [&](ShadowNotReady a_reason) {
			++shadowStats.notReady;
			++shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)];
			if (a_renderMode >= PassCapture::kFirstShadowMode)
				engine.ShadowsNotDrawn(shadowStats);
		};
		if (!pipelines.Enabled() || !utility || !engine.SetupShadow())
			return notReady(ShadowNotReady::Setup);
		// The tables are not read while the engine draws the shadow maps: ExecuteShadowFrame checks them.
		const auto* shadowView = ShadowViews::Get().At(a_viewId);
		if (!shadowView)
			return notReady(ShadowNotReady::Tables);
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + kShadowModeCount)
			return notReady(ShadowNotReady::Tables);
		// Not DCLF's this frame (no revision shape for the placements yet): the engine drew it whole.
		if (!shadowView->covered) {
			++shadowStats.uncovered;
			return;
		}
		// The target is DCLF's prediction (ShadowViews::PredictTargets); a cascade's first capture of the frame is its volumetric copy.
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t occurrence = ShadowViews::Get().NextCapture(a_viewId);
		const bool copyDraw = shadowView->volumetricCopy && occurrence == 0;
		const std::uint32_t target = copyDraw ? static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM) : shadowView->drawTarget;
		const std::uint32_t slice = shadowView->drawSlice;
		Diagnostics::Notify(Diagnostics::ShadowTargetPredicted{ a_viewId, shadowView, occurrence, target, slice });
		const std::uint32_t targetIndex = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM                     ? 0u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS                           ? 1u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM ? 2u :
		                                                                                                                     ~0u;
		// The volumetric copy holds only volumetric-only casters (batch group 15, all the engine's flag-0x100 draw renders): casterClass 1.
		const bool volumetricCopy = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;
		if (targetIndex == ~0u || !engine.ImportShadowDepth(targetIndex, target)) {
			// Logged once per target: a view the design has not met.
			if (target < 32 && !((engine.shadowLoggedTargets >> target) & 1)) {
				engine.shadowLoggedTargets |= 1u << target;
				logger::info("[DCLF] shadow view {} ({}, light {} descriptor {}, mode {:#x}) draws into depth target {} slice {}; it stays native",
					a_viewId, ShadowViews::KindName(shadowView->kind), shadowView->lightIndex, shadowView->descriptor, a_renderMode, target, slice);
			}
			return notReady(ShadowNotReady::Depth);
		}
		// The engine's rasterizer table entry for this view, read now: CS's ShadowmapCascadeRasterizerFix swaps per-cascade depth-bias
		// copies into the table for exactly this window. The cull mode is taken at 1, not the renderer's: the Utility shader sets it
		// per pass (0 for a two-sided property, 1 otherwise), so the renderer's is whatever drew last. Two-sided keys draw without
		// culling (kRasterTwoSided) as the engine's pass does, so the state changes only with the view. A view with no entry at 1
		// (the volumetric copy, culling off) takes its entry at 0.
		std::uint32_t rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 1, shadowState.rasterStateDepthBiasMode,
			shadowState.rasterStateScissorMode, a_renderMode);
		if (rasterState == 0)
			rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 0, shadowState.rasterStateDepthBiasMode, shadowState.rasterStateScissorMode,
				a_renderMode);
		if (rasterState == 0)
			return notReady(ShadowNotReady::Pipelines);

		auto& view = engine.pendingViews.emplace_back();
		view.viewId = a_viewId;
		view.renderMode = a_renderMode;
		view.modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		view.targetIndex = targetIndex;
		view.slice = slice;
		view.rasterState = rasterState;
		view.casterClass = volumetricCopy ? 1u : 0u;
		view.sunView = shadowView->kind == ShadowViews::Kind::Directional && !shadowView->focus;
		CaptureViewTarget(view, target);
		// The sun's, local lights' and focus views' viewport and eye are DCLF's; the renderer's are the observers'.
		const std::uint32_t sceneFrame = Scene::FrameView::Get().GetFrame();
		const ShadowCameras::Camera* ownView = nullptr;
		auto ownKind = Diagnostics::ShadowView::Sun;
		const bool owned = OwnViewOf(*shadowView, sceneFrame, view, ownView, ownKind) && ApplyOwnViewport(view, ownView, shadowView->descriptor, target, ownKind);
		// A focus view draws over its whole slice (FocusToSlice).
		if (owned && shadowView->focus)
			if (auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[target].texture) {
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				view.x = 0;
				view.y = 0;
				view.width = desc.Width;
				view.height = desc.Height;
			}
		// The placement slot: foreseen by setup, under its state, the frame's only view there; else a defect (a hole).
		{
			const std::size_t placement = engine.PlacementOf(view);
			std::string defect;
			if (placement == SIZE_MAX)
				defect = "at no placement";
			else if (const auto& p = engine.shadowPlacements[placement]; p.rasterState != rasterState || p.modeIndex != view.modeIndex)
				defect = fmt::format("at placement {} (mode {}, state {}) with mode {}, state {}", placement, p.modeIndex, p.rasterState, view.modeIndex, rasterState);
			else if (std::any_of(engine.pendingViews.begin(), engine.pendingViews.end() - 1, [&](const PendingView& a_other) { return a_other.slot == p.slot; }))
				defect = fmt::format("at placement {}, which a view of the frame already drew at", placement);
			if (!defect.empty()) {
				if (engine.placementDefectsLogged++ < 16)
					logger::warn("[DCLF] shadow view {} ({}, mode {:#x}) drew into target {} slice {} at ({} {}) {}x{} {} <- PLACEMENT", a_viewId,
						ShadowViews::KindName(shadowView->kind), a_renderMode, targetIndex, slice, view.x, view.y, view.width, view.height, defect);
				engine.pendingViews.pop_back();
				return notReady(ShadowNotReady::Placement);
			}
			view.slot = engine.shadowPlacements[placement].slot;
		}
		CaptureCasterVolume(view, *shadowView);
		// PerTechnique (b0): HighDetailRange (LOD landscape only; zero until owned) and ParabolaParam from the engine's two globals as
		// SetupTechnique reads them.
		{
			static const REL::Relocation<float*> parabolaRadius{ REL::Offset(Engine::Address::kParabolaRadius) };
			static const REL::Relocation<float*> parabolaSide{ REL::Offset(Engine::Address::kParabolaSide) };
			const float radius = *parabolaRadius.get();
			view.viewBlock[4] = radius != 0.0f ? 1.0f / radius : 0.0f;
			view.viewBlock[5] = *parabolaSide.get();
			// c2 (DCLFEyeDelta) unused: records are absolute and Utility.hlsl subtracts the view's CameraPosAdjust.
			view.viewBlock[8] = view.viewBlock[9] = view.viewBlock[10] = 0.0f;
		}
		// DCLF's view-projection; the engine's block is captured only for the observers.
		const bool engineBlock = Policy::CaptureEngineShadowBlock();
		const bool capturePerFrame = !owned || engineBlock;
		if (capturePerFrame)
			CapturePerFrame(view);
		if (owned)
			ApplyOwnCamera(view, *ownView, shadowView->descriptor, ownKind, engineBlock);
		// After the observers compared the engine's block.
		if (owned && shadowView->focus)
			FocusToSlice(view, *ownView, target);
		CheckCapturedEye(view, a_viewId, a_renderMode);
		shadowStats.captureMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void IndirectDraws::OcclusionView(std::uint32_t a_view, const RE::NiCamera& a_camera)
	{
		// DCLF's view from the camera Skylighting just set up for the map (Precipitation's occlusion camera; for the sky map a quarter
		// of its square, SetViewFrustum), before RenderMask draws it: ShadowCameras' matrices and eye, port times map size; depth target 10
		// (Skylighting swaps its own texture in for the sky map), slice 0; back-face culling, solid fill, no bias or scissor (the
		// Utility shader's state but for two-sided keys). Computed every frame the map renders, drawn or not: next frame's build
		// prepares pipelines for its state and format.
		auto& engine = *impl->shadowEngine;
		if (a_view >= kOcclusionViews || !ActiveToggles().shadows || failed || !Extensions::OcclusionOn(ActiveToggles(), a_view) || !engine.SetupShadow())
			return;
		constexpr auto target = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP);
		const std::uint32_t rasterState = EngineRasterStateId(0, 1, 0, 0, kOcclusionRenderMode);
		if (!rasterState || !engine.ImportShadowDepth(OcclusionDepthTarget(a_view), target))
			return;
		auto& occlusion = engine.occlusion[a_view];
		const std::uint32_t mode = OcclusionModeOf(a_view);
		if (!engine.readyModes[mode]) {
			engine.readyModes[mode] = true;
			engine.NoteCapability(engine.shadowCatalogBuilt, fmt::format("occlusion map {} set up before its setup", a_view));
		}
		if (const auto& states = engine.readyStates[mode].Of(false); !std::binary_search(states.begin(), states.end(), rasterState)) {
			engine.readyStates[mode].Add(rasterState, false);
			engine.NoteCapability(engine.shadowCatalogBuilt, fmt::format("occlusion map {} draws with rasterizer state {}, not in the catalog", a_view, rasterState));
		}
		const auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[target];
		if (!depth.texture || !depth.views[0])
			return;
		auto& view = occlusion.view;
		view = {};
		view.viewId = ~0u;
		view.renderMode = kOcclusionRenderMode;
		view.modeIndex = mode;
		view.targetIndex = OcclusionDepthTarget(a_view);
		view.slice = 0;
		view.rasterState = rasterState;
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		depth.views[0]->GetDesc(&dsvDesc);
		view.dsvFormat = dsvDesc.Format;
		D3D11_TEXTURE2D_DESC desc{};
		depth.texture->GetDesc(&desc);
		ShadowCameras::Camera camera;
		ShadowCameras::CameraMatrices(a_camera, camera);
		const auto port = ShadowCameras::Viewport(camera.port, desc.Width, desc.Height);
		view.x = port[0];
		view.y = port[1];
		view.width = port[2];
		view.height = port[3];
		view.minDepth = 0.0f;
		view.maxDepth = 1.0f;
		view.eye = { camera.eye[0], camera.eye[1], camera.eye[2] };
		ShadowCameras::Block(camera, view.perFrame.data());
		view.perFrameBytes = ShadowCameras::kBlockBytes;
		std::memcpy(view.viewProj.data(), reinterpret_cast<const float*>(view.perFrame.data()) + 32, sizeof(float) * 16);
		view.hasViewProj = true;
		occlusion.rasterState = rasterState;
		if (view.dsvFormat != occlusion.dsvFormat) {
			engine.NoteCapability(engine.shadowCatalogBuilt && occlusion.dsvFormat != DXGI_FORMAT_UNKNOWN,
				fmt::format("occlusion map {} draws into format {}, not {}", a_view, static_cast<int>(view.dsvFormat), static_cast<int>(occlusion.dsvFormat)));
			occlusion.dsvFormat = view.dsvFormat;
		}
		occlusion.capturedFrame = Scene::FrameView::Get().GetFrame();
		// A revision input (PredictedOcclusion).
		impl->PostRevisionInputs();
	}

	void IndirectDraws::CaptureOcclusion(std::uint32_t a_view)
	{
		// The engine's RenderMask just drew the map: for the observers only.
		Diagnostics::Notify(Diagnostics::OcclusionCaptured{ a_view });
	}

	std::vector<ShadowViewLayout> ShadowEngine::PredictedOcclusion(const ToggleSet* a_toggles) const
	{
		std::vector<ShadowViewLayout> out;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			const auto& o = occlusion[v];
			if ((a_toggles && !Extensions::OcclusionOn(*a_toggles, v)) || !o.rasterState || o.dsvFormat == DXGI_FORMAT_UNKNOWN)
				continue;
			out.push_back(LayoutOf(o.view, OcclusionSlot(v), OcclusionModeOf(v), OcclusionDepthTarget(v)));
		}
		return out;
	}
}
