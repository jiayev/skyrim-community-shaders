// The first-person view's two epochs (their passes are Core's, ViewEpochRecord.cpp's): the depth epoch at RenderDepth's first-person draw
// and the colour epoch at Main::RenderFirstPersonView's accumulator render, on the view engine's inputs and submission; and the view's
// report lines. Render thread (the report: wherever the draws' async work is composed).

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// The engine's first-person globals (AE 1.6.1170; skyrim-engine-notes.md, "The first-person view").
		// DrawWorld (FUN_1414cafd0), filled by FUN_1406452c0 (Main::Draw's first call, before BeginSceneFrame): +0x00 the camera position
		// (PlayerCamera::GetFirstPersonNodePosition), in world space.
		// The first-person camera (an NiCamera*), set up by FUN_1414cb020 (FUN_140d2cda0: FOV firstPersonFOV + 0x14338c628, near the float at
		// 0x14202a928, far the world scene graph's GetFarDistance, the screen's aspect; the world camera's rotation), translated into the
		// root's space (its position minus the root's).
		constexpr std::uintptr_t kFirstPersonCamera = 0x338c820;
		// The viewport depth range both first-person passes set (FUN_140e57cb0(state, 0, max)): f1stPersonFarDepthRange, or
		// f1stPersonFarDepthRangeControlDriven with RenderDepth's and FUN_1414cb6d0's second argument (the movement controller's vfunc 0x78).
		constexpr std::uintptr_t kFarDepthRange = 0x202b2d0, kFarDepthRangeControlDriven = 0x202b2e8;

		using ViewSubmit = IndirectDraws::ViewSubmit;

		/**
		 * @brief VS_PerFrame (b12) for the first-person camera as FUN_1414cb020 set it up: its frustum and rotation, and its position in
		 * world space (DrawWorld +0x00: its translation is in the root's space here). False without the camera.
		 */
		bool CaptureCamera(FirstPersonFrame& a_f)
		{
			auto& f = a_f;
			auto* camera = *REL::Relocation<RE::NiCamera**>{ REL::Offset(kFirstPersonCamera) };
			if (!camera)
				return false;
			const auto* eyeWorld = REL::Relocation<const float*>{ REL::Offset(Engine::Address::kDrawWorldEye) }.get();
			ShadowCameras::Camera view;
			ShadowCameras::CameraMatrices(*camera, view);
			const RE::NiPoint3 eye{ eyeWorld[0], eyeWorld[1], eyeWorld[2] };
			// The rows are world space (FrameValues samples them before the first-person shift): CameraPosAdjust is the world eye, not the
			// first-person camera's root-space translate.
			view.eye = { eye.x, eye.y, eye.z };
			// VS_PerFrame (b12) for the camera, as the engine's SetCameraData builds it for the first-person draws (the accumulator renders'
			// flags 3 and 0x43: jittered), over the renderer's last block; the motion vectors' previous camera last frame's, its own on the first.
			const Draws::PreviousCamera current{ view.viewProj, eye };
			f.perFrame.fill(std::byte{ 0 });
			Draws::PerFrameBlock(view, Draws::FrameBlockNow(), true, f.havePrevious ? &f.previous : &current, f.perFrame.data());
			f.viewProj = view.viewProj;
			f.eye = eye;
			f.previous = current;
			f.havePrevious = true;
			return true;
		}
	}

	void ExecuteDepth(bool a_controlDriven)
	{
		ZoneScopedN("CS.DCLF.ExecuteFirstPersonDepth");
		auto& draws = IndirectDraws::Get();
		if (!draws.ViewState(kView))
			return;
		auto& f = FrameState();
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		// The engine's depth range for the frame (a revision input: the shapes' viewport).
		draws.SetViewDepthRange(kView, *REL::Relocation<float*>{ REL::Offset(a_controlDriven ? kFarDepthRangeControlDriven : kFarDepthRange) });
		if (!Extensions::ViewOn(ActiveToggles(), kView) || !f.latch || f.latchFrame != frameNumber)
			return skip(0);
		const auto set = Scene::FrameView::Get().FrameClaims();
		if (!set || !(set->drawn & kSetFirstPerson))
			return skip(0);
		if (!draws.ViewCovered(kView))
			return skip(1);
		if (!CaptureCamera(f))
			return skip(2);
		// What the epoch draws from: the frame's scene list (the installed payload) with this frame's depth commit's index pool, inputs, object
		// records and geometry rows - the frame's publication whole.
		IndirectDraws::ViewListed listed;
		switch (draws.TakeViewInputs(kView, IndirectDraws::ViewInputsFrom::DepthThisFrame, listed)) {
		case IndirectDraws::ViewInputsTaken::Stale:
			return skip(3);
		case IndirectDraws::ViewInputsTaken::NoTarget:
			return skip(4);
		case IndirectDraws::ViewInputsTaken::Taken:
			break;
		}
		const std::array<Draws::ViewFace, 1> faces{ Draws::ViewFace{ true, std::span<const std::byte>(f.perFrame.data(), f.perFrame.size()), f.viewProj, f.eye } };
		switch (draws.SubmitViewEpoch(kView, 0, listed, faces)) {
		case ViewSubmit::NoRevision:
			return skip(1);
		case ViewSubmit::NoOwners:
			return skip(4);
		case ViewSubmit::Failed:
			logger::error("[DCLF] the first-person depth epoch failed");
			return skip(5);
		case ViewSubmit::Submitted:
			break;
		}
		++f.depthEpochs;
		f.depthFrame = frameNumber;
	}

	void ExecuteColour()
	{
		ZoneScopedN("CS.DCLF.ExecuteFirstPersonColour");
		auto& draws = IndirectDraws::Get();
		if (!draws.ViewState(kView))
			return;
		auto& f = FrameState();
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto skip = [&](std::uint32_t a_cause) { ++f.skipped[a_cause]; };
		if (!Extensions::ViewOn(ActiveToggles(), kView) || !f.latch || f.latchFrame != frameNumber || !draws.ViewCovered(kView))
			return;
		// What it draws is what this frame's depth epoch culled (its sequences and bucket counts), with this frame's colour commit's frame
		// constants (the frame record, the lighting at PS b13).
		if (f.depthFrame != frameNumber)
			return skip(6);
		IndirectDraws::ViewListed listed;
		if (draws.TakeViewInputs(kView, IndirectDraws::ViewInputsFrom::ColourThisFrame, listed) != IndirectDraws::ViewInputsTaken::Taken)
			return skip(3);
		switch (draws.SubmitViewEpoch(kView, 1, listed, {})) {
		case ViewSubmit::NoRevision:
			return skip(1);
		case ViewSubmit::NoOwners:
			return skip(4);
		case ViewSubmit::Failed:
			logger::error("[DCLF] the first-person colour epoch failed");
			return skip(5);
		case ViewSubmit::Submitted:
			break;
		}
		++f.colourEpochs;
	}

	std::string Report()
	{
		auto& draws = IndirectDraws::Get();
		const auto* state = draws.ViewState(kView);
		if (!state)
			return {};
		auto& f = FrameState();
		const auto counts = PassCapture::Get().TakeViewCounts(kView);
		const bool on = Extensions::ViewOn(ActiveToggles(), kView);
		if (!f.frames && !on && !counts.registered)
			return {};
		const auto& s = f.skipped;
		std::string text = fmt::format("[DCLF] first person (T7e, CS_DCLF_FIRST_PERSON {}): {} frames, {} the engine drew first person (latch); epochs: {} depth, {} colour; not drawn: "
									   "{} not the view's frame, {} not covered, {} without the camera, {} stale inputs, {} without resources, {} failed{}, {} colour without the "
									   "frame's depth epoch{}; targets colour {} motion {} depth {}, {} first-person pipeline slots, {} pipelines ready, depth range {:.4f}\n",
			on ? "on" : "off", f.frames, f.latched, f.depthEpochs, f.colourEpochs, s[0], s[1], s[2], s[3], s[4], s[5], s[5] ? " <- EPOCH FAILED" : "", s[6],
			s[6] ? " <- NO DEPTH EPOCH" : "", static_cast<int>(state->targets.colour), static_cast<int>(state->targets.colour2), static_cast<int>(state->targets.depth), f.slots,
			f.pipelinesReady, draws.ViewDepthRange(kView));
		// The palette rows classes' first-person writes (Features/Particles' particle members), counted by Core's FrameValues.
		const auto particles = FrameValues::TakeFirstPersonPaletteStats();
		text += fmt::format("[DCLF] first person's forward part (T7e): {:.1f} forward members a frame (blended Lighting, effects, particles; the main set's pipelines "
							"through their state classes' colour signatures, sorted back to front); particle members: {} expansions of {} particles faced to the first-person eye, "
							"their snapshots' bound at most {:.0f} units from it{}{}\n",
			f.frames ? static_cast<double>(f.forwardMembers) / static_cast<double>(f.frames) : 0.0, particles.writes, particles.units, particles.farthest,
			particles.farthest > 2000.0f ? " <- FIRST-PERSON PARTICLE SPACE" : "",
			state->forwardCompatible.load(std::memory_order_acquire) ? "" : "; the main depth format is not kPOST_ZPREPASS_COPY's: forward members stay the engine's");
		// The <- FIRST PERSON observer (CS_DCLF_SET_PARITY; PassCapture): the engine's first-person registrations of members, withheld, and
		// those it was handed (a member registered while its epochs did not draw: 0 on every covered frame).
		if (Diagnostics::Observe().claims || counts.memberKept)
			text += fmt::format("[DCLF] first person's registrations (T7e, the first-person accumulator and RenderDepth's block): {} passes registered into the "
								"first-person accumulator, {} member passes withheld, {} member passes the engine drew{}\n",
				counts.registered, counts.withheld, counts.memberKept,
				counts.memberKept && f.depthEpochs ? " <- FIRST PERSON" : (counts.withheld || counts.registered ? " <- OK" : ""));
		const std::uint32_t depthFrame = f.depthFrame;
		f.frames = f.latched = f.depthEpochs = f.colourEpochs = f.forwardMembers = 0;
		f.skipped = {};
		f.depthFrame = depthFrame;
		return text;
	}
}
