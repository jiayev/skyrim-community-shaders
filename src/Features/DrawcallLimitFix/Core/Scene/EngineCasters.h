#pragma once

// The shadow casters only the engine's cull draws: tracked geometries with no record whose settled verdict keeps them out of DCLF's
// shadow views (a skin the skinned path refuses, a buffer DCLF cannot import: Eligibility::EngineCasterReason). Fading and hidden parts
// are not counted: they settle within a few frames and their rules already cover them.
//
// The sun's whole skip (SunAccumulation::ArmWhole) and the point lights' empty culls (LocalLightCull's CullEmpty) skip the engine's cull,
// which would leave such a caster with no shadow at all. The coordinator keeps them as each geometry is classified and untracked
// (NoteEngineCaster) and publishes their roots with each publication (the frame reads them from the installed one): the actor's root
// for an actor's part (its light entry), else its sun entry. The sun's whole skip culls only those roots, so the engine registers their
// casters (the members under them are withheld as always); the point lights' empty culls decline while there are any.

#include <cstdint>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Scene
{
	/** @brief One publication's engine-only casters: their roots, deduplicated. Immutable once published. */
	struct EngineCasterRoots
	{
		// Roots the coordinator owns at the publication (rootOwners): they live while it is installed (the retirement chain). Never
		// dereferenced off the render thread's shadow culls.
		std::vector<const RE::NiAVObject*> roots;
		std::uint32_t geometries = 0;  // the casters under them
		std::uint32_t untargeted = 0;  // casters with no owned root: the engine culls the whole scene for them
	};
}
