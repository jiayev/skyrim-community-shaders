// CS_DCLF_FADE_PARITY, the port's side (the primary cull's hook points; Diagnostics/Engine/Events.h):
// - <- FADE PORT: every 30th frame, up to 64 fade roots, the port against the engine's functions on node copies;
// - the fade roots' nodes at the list jobs' end every 30th frame, for the fade state's comparison (NodeSnapshot: the fade log's).

#include "Features/DrawcallLimitFix/Features/Fades/Diagnostics/FadeDiagnostics.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/PrimaryCullView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include <utility>

namespace DCLF::Features::Fades
{
	namespace
	{
		using Diagnostics::PrimaryCullView;

		std::uint64_t frames = 0;   // PrepareFrame's frames (the fade port's every 30th)
		std::uint32_t fadePortCursor = 0;
		FadeState::PortCheck fadePort;  // render thread, since the last report (TakePortCheck)
		std::vector<FadeNodeState> nodeSnapshot;
		std::uint32_t nodeSnapshotFrame = ~0u;

		void CountFrame(const Diagnostics::CutFrameBegin&)
		{
			++frames;
		}

		/** @brief The port against the engine's functions on copies of the snapshot's fade roots (nothing writes them until the list jobs). */
		void CheckFadePort(const Diagnostics::On<Diagnostics::EnginePoint::FullFrustumDone>&)
		{
			if (!PrimaryCullView::StandInLive() || (frames % 30) != 0)
				return;
			const auto& cut = PrimaryCullView::Cut();
			auto** processes = Engine::Global<RE::NiCullingProcess**>(Engine::Address::kListProcesses);
			const auto* camera = processes && cut.processCount ? processes[0]->camera : nullptr;
			if (!camera)
				return;
			std::uint32_t checked = 0;
			const auto entries = static_cast<std::uint32_t>(cut.roots.size());
			for (std::uint32_t n = 0; n < entries && checked < 64; ++n) {
				const std::uint32_t e = (fadePortCursor + n) % entries;
				if (!PrimaryCullView::FadePlan(e))
					continue;
				FadeState::CheckPort(*cut.roots[e], *camera, fadePort);
				++checked;
			}
			fadePortCursor = entries ? (fadePortCursor + 64) % entries : 0;
		}

		/** @brief The fade roots' nodes now, after this cull's OnVisible and before the next animation job's updates. */
		void SnapshotNodes(const Diagnostics::On<Diagnostics::EnginePoint::ListJobsDone>&)
		{
			const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
			if ((frame % 30) != 0)
				return;
			const auto roots = Extensions::RootListOf(Scene::FrameView::Get().GetTables()).nodes;
			nodeSnapshot.resize(roots.size());
			for (std::size_t r = 0; r < roots.size(); ++r)
				nodeSnapshot[r] = roots[r] ? FadeState::ReadNode(*static_cast<const RE::NiAVObject*>(roots[r])) : FadeNodeState{};
			nodeSnapshotFrame = frame;
		}
	}

	void SubscribePortParity()
	{
		Diagnostics::Subscribe<Diagnostics::CutFrameBegin>(CountFrame);
		Diagnostics::Subscribe<Diagnostics::EnginePoint::FullFrustumDone>(CheckFadePort);
		Diagnostics::Subscribe<Diagnostics::EnginePoint::ListJobsDone>(SnapshotNodes);
	}

	const std::vector<FadeNodeState>* NodeSnapshot(std::uint32_t a_frame)
	{
		return nodeSnapshotFrame == a_frame ? &nodeSnapshot : nullptr;
	}

	FadeState::PortCheck TakePortCheck()
	{
		return std::exchange(fadePort, {});
	}
}
