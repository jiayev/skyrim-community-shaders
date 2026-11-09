// StartupHold: the session's first DCLF frames held black until its views are covered (StartupHold.h).

#include "Features/DrawcallLimitFix/Core/Render/StartupHold.h"

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

namespace DCLF::StartupHold
{
	namespace
	{
		// Two frames in a row without a held view end it; a frame limit ends it whatever (about 10 s at 60 fps: a view DCLF never covers
		// is then the engine's again, and its whole culls are reported as any other).
		constexpr std::uint32_t kCoveredFrames = 2;
		constexpr std::uint32_t kMaxFrames = 600;

		bool done = false;
		bool heldThisFrame = false;
		std::uint32_t frames = 0, covered = 0, held = 0, heldSun = 0, heldOcclusion = 0;
	}

	bool Active() { return !done; }

	void NoteHeld(View a_view)
	{
		if (done)
			return;
		++(a_view == View::Sun ? heldSun : heldOcclusion);
		if (!heldThisFrame)
			++held;
		heldThisFrame = true;
	}

	void OnFrameEnd(bool a_running)
	{
		if (done || !a_running || Scene::FrameView::IsLoadingScreenUp())
			return;
		// Only the frames that drew the world count (the scene frame advanced): the main menu's and the first load's do not.
		static std::uint32_t lastScene = 0;
		const std::uint32_t scene = Scene::FrameView::Get().GetFrame();
		if (scene == 0 || scene == std::exchange(lastScene, scene)) {
			heldThisFrame = false;
			return;
		}
		++frames;
		covered = heldThisFrame ? 0 : covered + 1;
		heldThisFrame = false;
		if (covered < kCoveredFrames && frames < kMaxFrames)
			return;
		done = true;
		if (frames >= kMaxFrames && covered < kCoveredFrames)
			logger::warn("[DCLF] startup hold: views still not covered after {} frames ({} held); released, the engine culls them whole until they are", frames, held);
		else
			logger::info("[DCLF] startup hold: {} frames black, {} of them with a view DCLF did not cover yet (its engine cull skipped: {} sun, {} occlusion maps)",
				frames, held, heldSun, heldOcclusion);
	}
}
