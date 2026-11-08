#pragma once

// The geometry only the engine draws in a view DCLF otherwise draws, as the one subset every such view's engine cull is limited to.
//
// Today it holds the shadow casters: tracked geometries with no record whose settled verdict keeps them out of DCLF's shadow views (a
// skin the skinned path refuses, a buffer DCLF cannot import: Eligibility::EngineCasterReason). Fading and hidden parts are not counted:
// they settle within a few frames and their rules already cover them.
//
// The coordinator keeps them as each geometry is classified and untracked (NoteEngineCaster) and publishes the subset with each
// publication (PublishEngineCasters; the frame reads the installed one's, FrameView::GetEngineCasters). A view never culls the whole
// scene for them: the sun's whole skip hands its full-frustum cull the roots (roots), and the light culls walk down to them alone
// (Admit, through EngineOnlyCull's scope). The members under a root are withheld at registration as always.

#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Scene
{
	/** @brief One publication's engine-only geometry, as the views' engine culls take it. Immutable once published. */
	struct EngineOnlySubset
	{
		// What a cull does with an object it reaches: cull it (a root: the engine's own cull of it and all below), descend (an ancestor of a
		// root: the engine's own cull, so its portal, room and bound tests stay on the path), or skip it.
		enum class Admit : std::uint8_t
		{
			Skip,
			Descend,
			Cull
		};

		// The roots: each caster's light entry (an actor's root) or sun entry while the coordinator owns it, else the geometry itself (a
		// tracked geometry, held until the retirement chain frees it). Either lives while the publication does. Deduplicated.
		std::vector<const RE::NiAVObject*> roots;
		// Keys only, never dereferenced: the roots, and every node from each one's parent up to the scene root (the mirror's chain).
		ankerl::unordered_dense::set<const void*> rootSet, ancestors;
		std::uint32_t geometries = 0;     // the casters under the roots
		std::uint32_t geometryRoots = 0;  // of the roots, geometries culled alone (no owned root above them)

		Admit AdmitObject(const void* a_object) const
		{
			if (rootSet.contains(a_object))
				return Admit::Cull;
			return ancestors.contains(a_object) ? Admit::Descend : Admit::Skip;
		}
	};
}
