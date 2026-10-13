#include "Engine.h"

#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Registrations.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Engine/Capture/ParticleSnapshots.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EffectConstants.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

namespace DCLF::Reports
{
	std::string ParticleSnapshotsLines()
	{
		// The particle members in the installed tables: objects of an effect key whose pass is a particle system's.
		std::uint32_t members = 0;
		{
			const auto& installedTables = Scene::FrameView::Get().GetTables();
			for (const auto& object : installedTables.objects) {
				if ((object.flags & (kObjectFree | kObjectNoBindings)) || !(object.flags & kObjectMember) || object.pipelineIndex >= installedTables.pipelines.size())
					continue;
				const auto& pipeline = installedTables.pipelines[object.pipelineIndex];
				members += RasterEffect(pipeline.rasterFlags) && (pipeline.passDescriptor & EffectFlag::Particles) ? 1u : 0u;
			}
		}
		const auto c = ParticleSnapshots::Get().Take();
		const bool on = ActiveToggles().particles;
		std::string text;
		if (on || members || c.captures || c.expansions)
			text += fmt::format("[DCLF] particles (T7e, CS_DCLF_PARTICLES {}): {} members bound, {} systems followed; {} snapshots taken at the engine's updates ({} "
								"update passes with nothing new skipped, {} while another update wrote the system); {} expansions of {} particles ({} without a "
								"snapshot yet, {} systems newly frozen: the same snapshot {} runs in a row, {} of them aligned quads); {} tables "
								"published, {} records pruned\n",
				on ? "on" : "off", members, c.followed, c.captures, c.unchanged, c.contended, c.expansions, c.expandedParticles, c.withoutSnapshot, c.frozen,
				ParticleSnapshots::kFrozenRuns, c.aligned, c.published, c.pruned);
		// CS_DCLF_SET_PARITY: the members' passes the engine registered (all withheld), against the snapshots the producer drew them from.
		// Printed whenever the parity is on: a member under a stood-in root (every geometry of its reference DCLF's) is not culled by the engine
		// at all, so 0 registrations is the expected answer there; a non-zero uncovered count is the defect.
		if (const auto p = Diagnostics::Registrations::TakeParticles(); p.passes || (Diagnostics::Observe().claims && on)) {
			const std::string first = p.first.empty() ? std::string() : fmt::format("; first {}", p.first);
			text += fmt::format("[DCLF] particles' engine passes (CS_DCLF_SET_PARITY): {} passes of particle members the engine registered (withheld), {} of "
								"them without a snapshot DCLF expanded in its last {} runs{}{}\n",
				p.passes, p.uncovered, ParticleSnapshots::kCoveredRuns, p.uncovered ? " <- PARTICLES" : " <- OK", first);
		}
		return text;
	}
}
