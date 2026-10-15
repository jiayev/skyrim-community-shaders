// The water reflection's faces' capture and frame start: a face's camera and targets once the engine oriented it (FaceOriented), the face
// drawn for the observers (FaceDrawn), the faces' targets, the LOD slots' forward pipelines and the view's resources after tree LOD's
// decision (Prepare), and its coverage (Covered). Render thread.

#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Features::Reflection
{
	namespace
	{
		/** @brief A render target view's format, its face (the array slice it views) and its texture. */
		struct ViewTarget
		{
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t face = ~0u;
			winrt::com_ptr<ID3D11Texture2D> texture;
			std::uint32_t width = 0, height = 0;
		};

		ViewTarget TargetOf(ID3D11RenderTargetView* a_view)
		{
			ViewTarget out;
			if (!a_view)
				return out;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			out.format = desc.Format;
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && desc.Texture2DArray.ArraySize == 1)
				out.face = desc.Texture2DArray.FirstArraySlice;
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(out.texture.put())))) {
				D3D11_TEXTURE2D_DESC textureDesc{};
				out.texture->GetDesc(&textureDesc);
				out.width = textureDesc.Width;
				out.height = textureDesc.Height;
			}
			return out;
		}
	}

	void FaceOriented(const RE::NiCamera& a_camera, std::uint32_t a_face)
	{
		auto& draws = IndirectDraws::Get();
		const auto* state = draws.ViewState(kView);
		if (!state)
			return;
		auto& reflection = FrameState();
		reflection.orientedFace = a_face;
		// The face for this update's epoch (Execute): only a plain face render's, whose LOD the registrations withhold or whose LOD roots
		// were not added (IndirectDraws::ViewLodRootsOwned).
		if (!ReflectionFaces::Plain() || a_face >= kFaces)
			return;
		// The face's colour target, the engine's reflection cube target's face a_face; its depth DCLF's own, cleared each face, at the
		// engine's precision (D24S8, the faces' depth target's R24G8).
		auto* renderer = globals::game::renderer;
		auto target = TargetOf(renderer ? renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].cubeSideRTV[a_face] : nullptr);
		const ForwardTargets targets{ target.format, DXGI_FORMAT_D24_UNORM_S8_UINT };
		if (targets.colour == DXGI_FORMAT_UNKNOWN || !target.texture)
			return;
		if (!(state->targets == targets) && state->targets.colour != DXGI_FORMAT_UNKNOWN)
			logger::info("[DCLF] reflection faces' targets changed: colour {} -> {}, depth {} -> {}", static_cast<int>(state->targets.colour),
				static_cast<int>(targets.colour), static_cast<int>(state->targets.depth), static_cast<int>(targets.depth));
		draws.SetViewTargets(kView, targets);
		++reflection.facesCaptured;
		if (reflection.executedFrame == Scene::FrameView::Get().GetFrame())
			++reflection.lateFaces;
		auto& face = reflection.faces[a_face];
		face = {};
		// VS_PerFrame for the face's camera over the frame's registers as the renderer last uploaded them (as the engine uploads it for
		// a face: no jitter, no previous camera).
		ShadowCameras::Camera view;
		ShadowCameras::CameraMatrices(a_camera, view);
		PerFrameBlock(view, FrameBlockNow(), false, nullptr, face.perFrame.data());
		face.perFrameBytes = ShadowCameras::kBlockBytes;
		const auto* floats = reinterpret_cast<const float*>(face.perFrame.data());
		std::memcpy(face.viewProj.data(), floats + 32, sizeof(float) * 16);
		face.eye = { floats[160], floats[161], floats[162] };
		face.captured = true;
		reflection.cube = std::move(target.texture);
		reflection.width = target.width;
		reflection.height = target.height;
	}

	void FaceDrawn()
	{
		// The face the engine has just drawn, for the observers (its camera against DCLF's: FaceOriented).
		Diagnostics::Notify(Diagnostics::ReflectionFaceDrawn{});
	}

	void Prepare(const Api::ViewPointContext& a_context)
	{
		// The forward pipelines of every LOD pipeline slot the frame's objects use, and tree LOD's, for the faces' targets: asked of the
		// pipeline lane here (each key once, the targets a frame input) and read from the frame's catalog, ready a few
		// frames later. The slots whose pipeline is ready are the reflection phase's readiness (PhaseReady): a change of them is a
		// readiness event for the set. A slot without one is no reflection-phase member meanwhile (the engine draws it in the faces).
		auto& draws = IndirectDraws::Get();
		const auto* state = draws.ViewState(kView);
		if (!state)
			return;
		auto& reflection = FrameState();
		// The faces' targets, known before any face is drawn: the engine's reflection cube target's face views (a face's capture
		// checks them), so the reflection phase is the set's from the start rather than from the first face.
		if (state->targets.colour == DXGI_FORMAT_UNKNOWN)
			if (auto* renderer = globals::game::renderer)
				if (auto* face = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].cubeSideRTV[0]) {
					draws.SetViewTargets(kView, { TargetOf(face).format, DXGI_FORMAT_D24_UNORM_S8_UINT });
					// A revision input: the targets the faces' pipelines are resolved for (from a revision's request's catalog).
					draws.PostViewInputs();
				}
		const bool on = state->targets.colour != DXGI_FORMAT_UNKNOWN && Extensions::ViewOn(ActiveToggles(), kView) && !a_context.failed;
		auto& drawPipelines = DrawPipelines::Get();
		if (on)
			drawPipelines.SetForwardTargets(state->targets);
		// Every slot with a key of the view's (a LOD Lighting technique's), with their programs counted and whether one is still to come.
		IndirectDraws::ViewPipelineCounts pipelines;
		if (on)
			pipelines = draws.ResolveViewPipelines(kView, *a_context.tables, a_context.catalog, {}, true);
		else
			draws.DropViewPipelines(kView);
		// Tree LOD's forward pipeline (its program is built of the DistantTree shader DecideTreeLod posts): the view engine's, under the tree
		// LOD class.
		const bool treeWaited = draws.ResolveViewTreeLod(kView, on, a_context.catalog);
		drawPipelines.NoteFrameWaits(false, pipelines.waited || treeWaited);
		reflection.lodSlots = pipelines.slots;
		reflection.programsReady = pipelines.programs;
		reflection.pipelinesReady = pipelines.ready;
		if (on)
			draws.SetUpView(kView);
		// The faces' tree LOD: DCLF's while the main view's is (DecideTreeLod, before this), the last depth commit uploaded the tables
		// the faces draw from, and the faces are drawn.
		draws.DecideViewTreeLod(kView);
	}

	bool Covered(const Api::ViewCoverage& a_coverage)
	{
		// The faces' withholding (PassCapture), before the engine renders them: the claims stand and the cube the epoch draws into is
		// in the graph as built (its import, at the epoch, is built at the next build point). The faces draw from the frame's scene list
		// and the frame before's colour frame record (Execute): not after a frame whose main epochs were not submitted (no claims), nor
		// once another set was adopted since (this frame's selection) or another payload installed since (the frame record names the
		// payload's objects and extras), which leaves those addresses on versions nothing holds. Whatever the toggle: off, the set has no
		// reflection members to withhold.
		const auto& c = a_coverage;
		return c.main && c.frameBeforeColourHeld && c.epochsCovered && c.targetImported && !c.rebuildRequested;
	}
}
