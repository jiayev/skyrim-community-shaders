// The LOD gates' publication and adoption: the flips' publication (scene lane) and their release by the frame installing them (render
// thread).

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Features/Lod/Diagnostics/LodGateEvents.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"

namespace DCLF::Features::Lod::Gates
{
	namespace
	{
		ReleaseStats releaseStats;  // render thread
	}

	ReleaseStats& Releases() { return releaseStats; }

	void Published([[maybe_unused]] const Api::CoordinatorComponentContext& a_context)
	{
		// Before the deltas are taken: the observers read the flips pending (the component's deltas pending, Flips; none, nothing to check).
		Diagnostics::Notify(GateFlipsPublished{});
	}

	void Adopt(const Api::AdoptionContext& a_context)
	{
		auto& rs = releaseStats;
		if (a_context.deltas.empty())
			return;
		rs.any = true;
		// The claims the frame installed (none before DCLF's first covered frame: the frame is the engine's, and every later publication
		// is post-swap).
		const void* claims = a_context.claims;
		const std::uint64_t now = GateNowNs();
		for (const auto& delta : a_context.deltas) {
			const auto& flip = *static_cast<const GateFlip*>(delta.get());
			const bool released = LodGates::Flip(flip.token);
			if (!claims)
				++rs.beforeDrawing;
			else
				Diagnostics::Notify(GateReleasing{ claims, flip.token, flip.incoming, flip.outgoing, released });
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
