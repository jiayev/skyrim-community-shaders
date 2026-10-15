#pragma once

// Particles (CS_DCLF_PARTICLES; drawcall-limit-fix.md, "Particles"; engine notes, "Particle systems"): the NiParticleSystem geometry with
// a BSEffectShaderProperty DCLF draws in the forward epoch's sort, its quads built from snapshots taken at the engine's particle update.
// The feature registers its switch and live toggle, the particle geometry types' contributions (Api::GeometryTypeContribution: the
// eligibility, the walk's traits and the snapshots' captures read the toggle through them, as data), the snapshot store and its engine
// hooks (ParticleSnapshots.h, ParticleSnapshots::Install), the particle systems' palette rows class (ParticleRows.cpp: the block rule, the
// quads' geometry slot, the frame's cameras, and the snapshots followed and expanded into the palette part FrameValues shares with the
// skins' bone palettes), the parity of the members' engine passes (Diagnostics/ParticlePasses.cpp) and its report lines.
//
// Core keeps what decides an item is a particle system and draws it: the class (Api::FrameValueItemClass::ParticleSystem, named by the
// coordinator's plans and the walk where they see a particle system without a skin), the deferral of the class's items in FrameValues'
// chunks (their rows compared and marked in the run's order), the first-person writes' counters the first person's report reads, and the
// Effect port's particle eligibility and descriptors. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Features/Particles/ParticleSnapshots.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::Particles
{
	// ParticlesFeature.cpp: the feature's entry (Features/FeatureList.cpp), and its toggle (the report's).
	Api::Feature Feature();
	Api::ToggleId Toggle();

	// Diagnostics/ParticlePasses.cpp: CS_DCLF_SET_PARITY's observer of the members' passes the main camera's registration withholds
	// (PassCapture's hooks, on whatever thread the engine registers): <- PARTICLES, a particle member's blended pass, against the
	// snapshots the producer expanded (ParticleSnapshots::Covered).
	void SubscribePasses();
	/** @brief <- PARTICLES since the last Take: the particle members' passes withheld, those without a recent snapshot, the first named. */
	struct PassCounts
	{
		std::uint64_t passes = 0, uncovered = 0;
		std::string first;
	};
	PassCounts TakePasses();

	// Diagnostics/ParticlesReport.cpp: the report lines (the draws' async work).
	void RegisterReport(Api::InstallContext& a_context);

	// ParticleRows.cpp: the particle systems' palette rows class (Api::PaletteRowsContribution).
	void RegisterRows(Api::InstallContext& a_context);
}
