// The LOD gates' publication and adoption: their adoption guard, the flips' publication (scene lane) and their release by the frame
// installing them (render thread).

#include "Features/DrawcallLimitFix/Scene/Coordinator/Gates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"
#include "Features/DrawcallLimitFix/Scene/Model/Publication.h"

namespace DCLF::Scene::Gates
{
	namespace
	{
		ReleaseStats releaseStats;  // render thread
	}

	std::uint64_t ForcedGeneration() { return LodGates::ForcedGeneration(); }

	ReleaseStats& Releases() { return releaseStats; }

	void Published([[maybe_unused]] const Api::CoordinatorComponentContext& a_context)
	{
		// Before the deltas are taken: the observers read the flips pending (the component's deltas pending, Flips; none, nothing to check).
		Diagnostics::Notify<Diagnostics::ScenePoint::GateFlipsPublished>();
	}

	void Adopt(const Api::AdoptionContext& a_context)
	{
		auto& rs = releaseStats;
		if (a_context.deltas.empty())
			return;
		rs.any = true;
		// The claims the frame installed (none when withdrawn: the frame is the engine's, and every later publication is post-swap).
		const auto* claims = static_cast<const SetSnapshot*>(a_context.claims);
		const std::uint64_t now = GateNowNs();
		for (const auto& delta : a_context.deltas) {
			const auto& flip = *static_cast<const GateFlip*>(delta.get());
			const bool released = LodGates::Flip(flip.token);
			if (!claims)
				++rs.withdrawn;
			else
				Diagnostics::Notify(Diagnostics::GateReleasing{ *claims, flip.token, flip.incoming, flip.outgoing, released });
			rs.flipToReleaseMs.push_back(GateMs(flip.flipNs, now));
			rs.requestToReleaseMs.push_back(GateMs(flip.requestNs, now));
			rs.flipToReleaseFrames.push_back(a_context.frame >= flip.flipFrame ? a_context.frame - flip.flipFrame : 0u);
			if (released)
				++rs.released;
			else
				++rs.refused;
		}
		// The outcomes the flips posted, for the coordinator (folded into the frame start's wakes: ScenePassWakeBatch).
		Extensions::WakeChannel(channel);
	}
}
