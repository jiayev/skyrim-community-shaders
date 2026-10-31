// The main pass's points (Api::PrimaryViewContribution), called by Core's FrameDriver from the shell's hooks: the main pass's capture where
// its opaque batches start, and the colour epoch (Core's IndirectDraws::ExecuteColour) where they end, under the opaque.after trigger.

#include "Features/DrawcallLimitFix/Features/MainView/MainView.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Globals.h"

#include <d3d11.h>

#include <cstdint>

namespace DCLF::Features::MainView
{
	namespace
	{
		// Render thread, after the install: the colour epoch's trigger (opaque.after), and whether the capture's failure was logged.
		Api::TriggerId opaqueAfter;
		bool loggedCaptureFailure = false;

		TargetFormats CurrentTargetFormats()
		{
			TargetFormats formats;
			ID3D11RenderTargetView* views[8] = {};
			ID3D11DepthStencilView* depth = nullptr;
			globals::d3d::context->OMGetRenderTargets(8, views, &depth);
			for (std::uint32_t i = 0; i < 8; ++i) {
				if (!views[i])
					continue;
				D3D11_RENDER_TARGET_VIEW_DESC desc;
				views[i]->GetDesc(&desc);
				formats.colors[i] = desc.Format;
				formats.colorCount = i + 1;
				views[i]->Release();
			}
			if (depth) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc;
				depth->GetDesc(&desc);
				formats.depth = desc.Format;
				depth->Release();
			}
			return formats;
		}

		/** @brief The main pass's targets and bindings for this frame's colour epoch; false when its targets are not bound. */
		bool CaptureMainPass()
		{
			const auto formats = CurrentTargetFormats();
			if (formats.colorCount == 0 || formats.depth == DXGI_FORMAT_UNKNOWN)
				return false;
			DrawPipelines::Get().SetTargetFormats(formats);
			// What the main pass binds, for this frame's indirect draws (run before the composite).
			IndirectDraws::Get().CaptureMainPass();
			// The engine's state objects the pipeline lane asked for, read here because this is inside the deferred pass, where the blend table
			// holds the deferred variants.
			DrawPipelines::Get().CaptureEngineStates();
			return true;
		}

		// The main pass's bindings for this frame's colour epoch, where the opaque batches start (FrameDriver applied the engine's pending
		// state first: the engine binds the G-buffer only when a draw applies its state).
		bool BeforeOpaque()
		{
			const bool captured = CaptureMainPass();
			if (!captured && !loggedCaptureFailure) {
				loggedCaptureFailure = true;
				logger::warn("[DCLF] the main pass's targets are not bound where its opaque batches start; the colour epoch is skipped");
			}
			return captured;
		}

		void AfterOpaque()
		{
			// DCLF's objects must be in the G-buffer and the depth before Terrain Blending draws its held terrain (alpha blended onto the
			// G-buffer, its alpha the distance to the object beneath, with depth writes): drawn after that terrain, a buried object would be
			// missing from what the terrain blended onto, and the terrain's depth writes would fail DCLF's EQUAL test where it overlapped.
			auto& draws = IndirectDraws::Get();
			globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);  // the epoch writes the G-buffer the context has bound
			draws.ProbeTargets("before colour");
			Extensions::TriggerScope trigger(opaqueAfter);
			draws.ExecuteColour();  // the colour pass, against the depth written at the first draw
			draws.ProbeTargets("after colour");
			// DCLF's epoch leaves the context's bindings to the engine's state tracking: its targets rebound for the rest of the pass.
			globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
	}

	void RegisterPrimaryView(Api::InstallContext& a_context)
	{
		opaqueAfter = a_context.FindTrigger("opaque.after");
		a_context.RegisterPrimaryView({ .beforeOpaque = &BeforeOpaque, .afterOpaque = &AfterOpaque });
	}
}
