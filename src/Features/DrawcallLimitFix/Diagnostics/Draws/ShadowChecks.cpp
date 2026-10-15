// The shadow epochs' checks (render thread): DCLF's views against the engine's (the viewport, eye, matrices and caster volume at each
// view's draw, the predicted target, the occlusion maps' views, the sun's full-frustum processes), the sun entry test on the GPU against
// the CPU's, and the index pool's ranges against the tables.

#include "DrawsState.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Events.h"
#include "Observers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
// ShadowCapture.cpp's: the engine's rasterizer state id, and where and with which block the engine has just drawn a view.
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"

namespace DCLF::Diagnostics
{
	void CheckIndexPool(const IndexPoolUpdated& a_updated)
	{
		// Every range against its slot's buffer as the tables have it now (a slot whose buffer changed with no log entry would keep
		// drawing the old buffer's indices).
		auto& checks = DrawsState::Get().indexPoolChecks;
		if ((++checks % 300) != 0)
			return;
		const auto& p = a_updated.pool;
		const auto& a_tables = a_updated.tables;
		const std::uint32_t count = a_updated.count;
		std::uint32_t held = 0, stale = 0, unheld = 0, firstStale = IndexPool::kNoRange;
		for (std::uint32_t g = 0; g < count; ++g) {
			const auto& geometry = a_tables.geometries[g];
			const auto& range = p.slots[g];
			const bool wants = geometry.indexAddress && geometry.indexBytes;
			if (range.first == IndexPool::kNoRange) {
				unheld += wants ? 1 : 0;
				continue;
			}
			++held;
			if (!wants || range.address != geometry.indexAddress || range.bytes != geometry.indexBytes) {
				if (stale++ == 0)
					firstStale = g;
			}
		}
		logger::info("[DCLF] index pool parity: {} slots, {} ranges ({} indices held of {}), {} stale, {} without a range ({} waiting for the pool's growth){}{}",
			count, held, p.indicesHeld, NamedUnits(DrawsView::Get().Adopted(), p.indices), stale, unheld, p.waiting.size(), stale || unheld > p.waiting.size() ? " <- INDEX POOL" : " <- OK",
			firstStale != IndexPool::kNoRange ? fmt::format("; first stale: slot {}", firstStale) : std::string());
	}

	void CheckShadowTarget(const ShadowTargetPredicted& a_predicted)
	{
		// DCLF's predicted depth target and slice against the renderer's at the draw.
		auto& s = DrawsState::Get().shadowTargets;
		const auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const auto* shadowView = a_predicted.shadowView;
		++s.checked;
		const std::uint32_t engineTarget = shadowState.depthStencil, engineSlice = shadowState.depthStencilSlice;
		if (engineTarget != a_predicted.target || engineSlice != a_predicted.slice) {
			if (s.differ++ == 0)
				s.first = fmt::format("view {} ({}{}, light {} descriptor {}, capture {}): target {} slice {}, the engine's {} slice {}", a_predicted.viewId,
					ShadowViews::KindName(shadowView->kind), shadowView->focus ? " focus" : "", shadowView->lightIndex, shadowView->descriptor, a_predicted.occurrence,
					a_predicted.target, a_predicted.slice, engineTarget, engineSlice);
		}
		if (s.checked == 3000) {
			logger::info("[DCLF] shadow targets (T3d: DCLF's predicted depth target and slice against the renderer's at the draw): {} views, {} differ{}{}", s.checked,
				s.differ, s.differ ? " <- SHADOW TARGET" : " <- OK", s.first.empty() ? "" : "; first: " + s.first);
			s = {};
		}
	}

	void CheckOwnViewport(const OwnShadowViewport& a_own)
	{
		// The viewport and eye CaptureViewTarget read from the renderer's state, against DCLF's (its port times the depth target's size).
		auto& p = ShadowViewTally(a_own.kind);
		++p.views;
		auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_own.target].texture;
		if (!a_own.cascade || !texture) {
			++p.missing;
			return;
		}
		const auto& a_view = a_own.view;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		const auto port = ShadowCameras::Viewport(a_own.cascade->port, desc.Width, desc.Height);
		const RE::NiPoint3 eye{ a_own.cascade->eye[0], a_own.cascade->eye[1], a_own.cascade->eye[2] };
		// The depth range against the camera state's (0x14202b130, FUN_140e57e10), which every draw's state flush starts from
		// (FUN_140e4b5b0): the renderer's viewport holds the last draw's, less its depth bias's step (0x143286a80[bias]) when it had
		// one - a per-draw state, not the view's.
		static const REL::Relocation<const float*> cameraDepthRange{ REL::Offset(Engine::Address::kCameraDepthRange) };
		const float engineMin = cameraDepthRange.get()[0], engineMax = cameraDepthRange.get()[1];
		if (port[0] != a_view.x || port[1] != a_view.y || port[2] != a_view.width || port[3] != a_view.height || engineMin != 0.0f || engineMax != 1.0f) {
			++p.viewport;
			p.Note("viewport", 1.0f, fmt::format("descriptor {}: ({} {}) {}x{}, the engine's ({} {}) {}x{} camera depth range {}-{}", a_own.descriptor, port[0], port[1],
				port[2], port[3], a_view.x, a_view.y, a_view.width, a_view.height, engineMin, engineMax));
		}
		if (!ShadowCameras::Close(eye.x, a_view.eye.x) || !ShadowCameras::Close(eye.y, a_view.eye.y) || !ShadowCameras::Close(eye.z, a_view.eye.z)) {
			++p.eye;
			p.Note("eye", (eye - a_view.eye).Length(),
				fmt::format("descriptor {}: ({} {} {}), the engine's ({} {} {})", a_own.descriptor, eye.x, eye.y, eye.z, a_view.eye.x, a_view.eye.y, a_view.eye.z));
		}
	}

	void CheckOwnCamera(const OwnShadowCamera& a_own)
	{
		// The matrices the draws use: CameraView, CameraProj and CameraViewProj (c0-c11; Utility.hlsl's paraboloid modes read the view
		// too), and the engine's caster volume, against DCLF's block (the engine's taken for this: Policy::CaptureEngineShadowBlock).
		auto& p = ShadowViewTally(a_own.kind);
		const auto& a_view = a_own.view;
		const auto& a_cascade = a_own.cascade;
		std::array<std::byte, ShadowCameras::kBlockBytes> block{};
		ShadowCameras::Block(a_cascade, block.data());
		const auto* mine = reinterpret_cast<const float*>(block.data());
		const auto* theirs = reinterpret_cast<const float*>(a_view.perFrame.data());
		float largest = 0.0f;
		std::int32_t first = -1;
		for (std::uint32_t i = 0; i < 48; ++i) {
			if (!ShadowCameras::Close(mine[i], theirs[i]) && first < 0)
				first = static_cast<std::int32_t>(i);
			largest = std::max(largest, std::abs(mine[i] - theirs[i]));
		}
		if (first >= 0) {
			++p.viewProj;
			const std::uint32_t row = static_cast<std::uint32_t>(first) / 4 * 4;
			p.Note("matrices", largest, fmt::format("descriptor {}: c{} ({} {} {} {}), the engine's ({} {} {} {})", a_own.descriptor, row / 4, mine[row], mine[row + 1],
				mine[row + 2], mine[row + 3], theirs[row], theirs[row + 1], theirs[row + 2], theirs[row + 3]));
		}
		ShadowCameras::Planes captured;
		for (std::uint32_t q = 0; q < 6; ++q)
			for (std::uint32_t i = 0; i < 4; ++i)
				captured.plane[q][i] = a_view.cullPlanes[q][i];
		captured.mask = a_view.cullPlaneMask;
		if (!ShadowCameras::SamePlanes(a_cascade.caster, captured)) {
			++p.casters;
			p.Note("caster volume", 1.0f, fmt::format("descriptor {}: mask {:#x}, the engine's {:#x}", a_own.descriptor, a_cascade.caster.mask, captured.mask));
		}
	}

	void CheckSunEntryPlanes(const SunEntryPlanesTaken& a_taken)
	{
		// The processes' own planes after the full-frustum cull against DCLF's (SunViews), which the entry rule tests.
		auto& tally = ShadowViewTally(ShadowView::Sun);
		const auto& sunViews = ShadowCameras::Sun();
		if (!a_taken.taken) {
			++tally.missing;
			tally.Note("missing full frustum", 0.0f, fmt::format("inputs of frame {}, DCLF's views of frame {} (valid {}, {} processes)", a_taken.frame,
				sunViews.sceneFrame, sunViews.valid, sunViews.fullFrusta.size()));
			return;
		}
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		auto* sun = node ? node->GetRuntimeData().sunShadowDirLight : nullptr;
		if (!sun)
			return;
		std::size_t index = 0;
		for (const auto& process : sun->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray) {
			if (!process)
				continue;
			ShadowCameras::Planes engine;
			for (std::uint32_t p = 0; p < 6; ++p)
				engine.plane[p] = { process->planes.cullingPlanes[p].normal.x, process->planes.cullingPlanes[p].normal.y, process->planes.cullingPlanes[p].normal.z,
					process->planes.cullingPlanes[p].constant };
			engine.mask = process->planes.activePlanes.underlying() & 0x3Fu;
			++tally.fullFrusta;
			if (index >= sunViews.fullFrusta.size()) {
				++tally.fullFrustumPlanes;
				tally.Note("full-frustum planes", 1.0f, fmt::format("process {}: DCLF has {} processes", index, sunViews.fullFrusta.size()));
			} else if (!ShadowCameras::SamePlanes(sunViews.fullFrusta[index], engine)) {
				++tally.fullFrustumPlanes;
				tally.Note("full-frustum planes", 1.0f, fmt::format("process {}: {}", index, ShadowCameras::DescribePlanes(sunViews.fullFrusta[index], engine)));
			}
			++index;
		}
	}

	void CheckOcclusionView(const OcclusionCaptured& a_captured)
	{
		// Where the engine's RenderMask has just drawn the map (the clear, and whatever SetupMask registered), with which camera and
		// state, against DCLF's view of it this frame (OcclusionView). The renderer's state here is what the view's last pass left, or
		// whatever came before when nothing drew; the cull mode is the Utility shader's per pass, so only the fill, bias and scissor modes
		// are compared.
		const std::uint32_t a_view = a_captured.view;
		if (a_view >= kOcclusionViews || !ActiveToggles().shadows || IndirectDraws::Get().Failed() || !Extensions::OcclusionOn(ActiveToggles(), a_view))
			return;
		const auto& occlusion = DrawsView::Get().occlusion[a_view];
		if (occlusion.capturedFrame != Scene::Coordinator::Get().GetFrame())
			return;
		auto& s = DrawsState::Get().occlusionViews[a_view];
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const auto& mine = occlusion.view;
		PendingView theirs;
		const std::uint32_t target = shadowState.depthStencil;
		CaptureViewTarget(theirs, target);
		CapturePerFrame(theirs);
		const std::uint32_t rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 1, shadowState.rasterStateDepthBiasMode, shadowState.rasterStateScissorMode,
			kOcclusionRenderMode);
		++s.maps;
		// Every field compared; what differs is listed.
		std::string what;
		auto note = [&](std::string a_text) { what += (what.empty() ? "" : "; ") + a_text; };
		float largest = 0.0f;
		if (target != static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP) || shadowState.depthStencilSlice != mine.slice)
			note(fmt::format("target {} slice {}", target, shadowState.depthStencilSlice));
		if (rasterState != mine.rasterState)
			note(fmt::format("rasterizer state {} (fill {}, bias {}, scissor {}), DCLF's {}", rasterState, shadowState.rasterStateFillMode, shadowState.rasterStateDepthBiasMode,
				shadowState.rasterStateScissorMode, mine.rasterState));
		// The depth range against the camera state's (0x14202b130), as the shadow views' check: the renderer's viewport holds the last
		// draw's, less its depth bias's step when it had one - a per-draw state, not the view's.
		static const REL::Relocation<const float*> cameraDepthRange{ REL::Offset(Engine::Address::kCameraDepthRange) };
		const float engineMin = cameraDepthRange.get()[0], engineMax = cameraDepthRange.get()[1];
		if (theirs.dsvFormat != mine.dsvFormat || theirs.x != mine.x || theirs.y != mine.y || theirs.width != mine.width || theirs.height != mine.height ||
			engineMin != mine.minDepth || engineMax != mine.maxDepth)
			note(fmt::format("viewport ({} {}) {}x{} {}-{} format {}, DCLF's ({} {}) {}x{} {}-{} format {}", theirs.x, theirs.y, theirs.width, theirs.height, engineMin, engineMax,
				static_cast<int>(theirs.dsvFormat), mine.x, mine.y, mine.width, mine.height, mine.minDepth, mine.maxDepth, static_cast<int>(mine.dsvFormat)));
		{
			// The registers the depth draws read: CameraView, CameraProj, CameraViewProj (c0-c11), and the eye.
			const auto* a = reinterpret_cast<const float*>(mine.perFrame.data());
			const auto* b = reinterpret_cast<const float*>(theirs.perFrame.data());
			std::int32_t first = -1;
			for (std::uint32_t i = 0; i < 48; ++i) {
				if (!ShadowCameras::Close(a[i], b[i]) && first < 0)
					first = static_cast<std::int32_t>(i);
				largest = std::max(largest, std::abs(a[i] - b[i]));
			}
			for (std::uint32_t i = 160; i < 163; ++i) {
				if (!ShadowCameras::Close(a[i], b[i]) && first < 0)
					first = static_cast<std::int32_t>(i);
				largest = std::max(largest, std::abs(a[i] - b[i]));
			}
			if (first >= 0) {
				const std::uint32_t row = static_cast<std::uint32_t>(first) / 4 * 4;
				note(fmt::format("c{} ({} {} {} {}), the engine's ({} {} {} {})", row / 4, a[row], a[row + 1], a[row + 2], a[row + 3], b[row], b[row + 1], b[row + 2], b[row + 3]));
			}
		}
		if (!what.empty()) {
			s.largest = std::max(s.largest, largest);
			if (s.differ++ == 0)
				s.first = std::move(what);
		}
		if (s.maps == 300) {
			logger::info("[DCLF] occlusion views (T4: DCLF's view of map {} from its camera against the engine's at RenderMask): {} maps, {} differ (largest {:.3g}){}{}", a_view,
				s.maps, s.differ, s.largest, s.differ ? " <- OCCLUSION VIEW" : " <- OK", s.first.empty() ? "" : "; first: " + s.first);
			s = {};
		}
	}

	void CheckSunEntry(const ShadowViewLatched& a_latched)
	{
		// The test BuildDraws makes on the placement row's entry sphere (the kept rows the epoch uploads), against the CPU's verdict from
		// the tables' entry, per input of a sun view.
		if (!a_latched.view.sunView || !ParityDue(a_latched.frame))
			return;
		const auto& payload = a_latched.payload;
		const auto& tablesNow = Scene::FrameView::Get().GetTables();
		static const std::vector<BindlessPlacement> kNone;
		const auto* frameRows = FrameValues::Get().RowsIfDone();
		const auto& placements = frameRows ? *frameRows : kNone;
		for (const auto& input : payload.Flat(a_latched.view.modeIndex)) {
			if (input.objectIndex >= placements.size())
				continue;
			const bool cpu = OutsideSunEntry(payload.inputs, tablesNow, input.objectIndex);
			++a_latched.stats.sunEntryChecks;
			a_latched.stats.sunEntryMismatches += cpu != OutsideSunEntryProcesses(payload.inputs.sunEntryProcesses, placements[input.objectIndex].sunEntry) ? 1 : 0;
		}
	}
}
