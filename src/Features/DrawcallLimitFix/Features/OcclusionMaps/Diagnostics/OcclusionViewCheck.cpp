// The occlusion maps' views (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY), render thread: DCLF's view of each map against where, with
// which camera and state, the engine's RenderMask has just drawn it (the shadow capture's 0x1C capture), reported every 300 maps.

#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"
// ShadowCapture.cpp's: the engine's rasterizer state id, and where and with which block the engine has just drawn a view.
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"

namespace DCLF::Features::OcclusionMaps
{
	namespace
	{
		void CheckOcclusionView(const Diagnostics::OcclusionCaptured& a_captured)
		{
			// Where the engine's RenderMask has just drawn the map (the clear, and whatever SetupMask registered), with which camera and
			// state, against DCLF's view of it this frame (OcclusionView). The renderer's state here is what the view's last pass left, or
			// whatever came before when nothing drew; the cull mode is the Utility shader's per pass, so only the fill, bias and scissor modes
			// are compared.
			const std::uint32_t a_view = a_captured.view;
			if (a_view >= kOcclusionViews || !ActiveToggles().shadows || IndirectDraws::Get().Failed() || !Extensions::OcclusionOn(ActiveToggles(), a_view))
				return;
			const auto& occlusion = Diagnostics::DrawsView::Get().occlusion[a_view];
			if (occlusion.capturedFrame != Scene::Coordinator::Get().GetFrame())
				return;
			auto& s = Diagnostics::DrawsState::Get().occlusionViews[a_view];
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
	}

	void SubscribeOcclusionViewCheck()
	{
		Diagnostics::Subscribe<Diagnostics::OcclusionCaptured>(&CheckOcclusionView);
	}
}
