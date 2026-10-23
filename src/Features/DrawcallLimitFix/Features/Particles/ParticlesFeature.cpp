// Particles: the effect-shader particle systems DCLF draws from the snapshots of the engine's particle update. Its switch and the
// particles toggle, the particle geometry types' contributions, the snapshots' engine hooks, their palette rows, the report lines and the
// parity.

#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Particles
{
	namespace
	{
		Api::ToggleId particlesToggle;  // install

		void Install(Api::InstallContext& a_context)
		{
			// Where Core's were listed: after the refraction (before Features/Billboards', which registers after this feature).
			const auto particles = a_context.RegisterSwitch({ .name = "CS_DCLF_PARTICLES", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "1: particle systems with an effect shader (NiParticleSystem: smoke, flames, glows) drawn by DCLF's forward epoch from snapshots taken at the engine's particle update, sorted with the blended objects (T7e; needs CS_DCLF_EFFECTS); 0 they stay native (live toggle's seed)",
				.after = "CS_DCLF_REFRACTION" });
			// NiParticleSystem geometry with a BSEffectShaderProperty, its quads built by DCLF from snapshots taken at the engine's particle
			// update (ParticleSnapshots) and drawn in the forward epoch's sort (needs effects; on by default: CS_DCLF_PARTICLES=0 seeds it
			// off): an object class.
			particlesToggle = a_context.RegisterToggle({ .name = "particles", .label = "Particle systems: smoke, flames, glows",
				.tooltip = "T7e: DCLF draws effect-shader particle systems in the forward epoch, sorted with the other blended objects, their quads built from snapshots taken at the engine's particle update. On by default (validated fp6).",
				.seed = particles, .entersClassification = true, .after = "refraction", .needs = { "effects" } });
			// The particle geometry types (ParticleGeometryType): DCLF's while the toggle is on. A particle system (kParticles) without a skin
			// is the particle systems' item class (its palette block its quads: ParticleRows.cpp), and a mover: its bound and quads change with
			// each update, invisibly to the walk (FrameValues places it every frame; its update's snapshot capture pushes its move event).
			a_context.RegisterGeometryType({ .geometryType = static_cast<std::uint8_t>(RE::BSGeometry::Type::kParticles), .toggle = particlesToggle,
				.itemClass = Api::FrameValueItemClass::ParticleSystem, .moves = true });
			a_context.RegisterGeometryType({ .geometryType = static_cast<std::uint8_t>(RE::BSGeometry::Type::kStripParticles), .toggle = particlesToggle });
			// The particle systems' snapshots, taken after the engine's update of each (ParticleSnapshots; the systems DCLF follows alone).
			ParticleSnapshots::Get().Install(a_context);
			// Their palette rows: the members' blocks and placements, from the snapshots, every frame.
			RegisterRows(a_context);
			RegisterReport(a_context);
			// CS_DCLF_SET_PARITY: the members' withheld particle passes.
			if (Diagnostics::Observe().claims)
				SubscribePasses();
		}
	}

	Api::Feature Feature()
	{
		return { "particles", &Install };
	}

	Api::ToggleId Toggle()
	{
		return particlesToggle;
	}
}
