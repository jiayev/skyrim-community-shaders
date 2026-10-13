#include "Policy.h"

#include "Observation.h"
#include "Feature/Skylight.h"

namespace DCLF::Policy
{
	bool ScenePassInline()
	{
		// The observers that read the store from the engine's hooks while the scene work would run: the persistent and walk parities,
		// and the capture parity's face draws.
		const auto& o = Diagnostics::Observe();
		return o.scene || o.walk || o.capture;
	}

	bool WalkEveryEntry()
	{
		// The stand-in walk check visits every stood-in entry.
		return Diagnostics::Observe().scene;
	}

	bool FilterLightCulls(std::uint32_t a_frame)
	{
		// The light culls' check needs the engine's own cull of every candidate on its frames.
		return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
	}

	bool ExcludeSun(std::uint32_t a_frame)
	{
		// The sun exclusion's check watches the excluded entries' registrations on its frames.
		return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
	}

	bool KeepSceneListsWhole()
	{
		// The decal order probe compares the decal order with the engine's own lists.
		return Diagnostics::Observe().decalOrderProbe;
	}

	bool FilterSceneLists(std::uint32_t a_frame)
	{
		// The scene lists' check compares the engine's whole lists against DCLF's enumeration on its frames.
		return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
	}

	bool RenderOcclusionReference(std::uint32_t a_map)
	{
		// The occlusion maps' check compares the engine's render with DCLF's, texel by texel.
		return Diagnostics::Skylight::ReferenceDue(a_map);
	}

	bool CaptureEngineShadowBlock()
	{
		// The shadow views' check compares the engine's block with DCLF's camera.
		return Diagnostics::Observe().claimsOrScene;
	}

	bool BuildAtEpoch()
	{
		// The bindless check's builds read the eye the Z-prepass epoch captures.
		return Diagnostics::Observe().bindless;
	}

	bool WithholdFromEngine()
	{
		// The capture check compares DCLF's draws with the engine's own draws of the same objects.
		return !Diagnostics::Observe().engineDrawsAll;
	}

	bool RecomputeCachedVerdicts(Verdicts a_verdicts, std::uint64_t a_pass)
	{
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_pass, a_verdicts == Verdicts::Classification ? 17 : 13);
	}

	bool SerialFanout(bool a_joins)
	{
		// The mirror check's live reads note their leases and differences on the pass's own thread, and the derive probe samples the LOD
		// fade frame lazily and reads live properties under a lease.
		const auto& o = Diagnostics::Observe();
		return o.mirror || (a_joins && o.deriveProbe);
	}

	bool HiddenWitness(std::uint64_t a_pass)
	{
		return Diagnostics::Observe().walkOrScene && Diagnostics::ParityDue(a_pass);
	}

	bool RereadCachedInputs()
	{
		return Diagnostics::Observe().inputWatch;
	}

	bool ReuseResidentRegion()
	{
		// The build check compares BuildDraws' output with whole builds.
		return !Diagnostics::Observe().builds;
	}

	bool ReuseShadowExclusion(std::uint32_t a_frame)
	{
		// The kept shadow check builds a reuse in full on its frames and compares.
		return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
	}

	bool RecordFoliagePixels()
	{
		// The foliage parity compares the recorded pixels with the frame before's.
		return Diagnostics::Observe().foliage;
	}

	bool FreezeTreeWind()
	{
		// The foliage parity compares each frame's pixels with the frame before's (1 or land).
		return Diagnostics::Observe().foliageFrozen;
	}

	bool KeepReflectionRoots(std::uint32_t& a_updates)
	{
		// The residue's watch (the reflection report's): every 30th update while the parities or the timeline observe.
		const auto& o = Diagnostics::Observe();
		return (o.claimsOrScene || o.timeline) && a_updates++ % 30 == 0;
	}

	bool RebuildShadowSets(std::uint64_t a_pass)
	{
		// The persistent parity's shadow dependency check.
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_pass, 15);
	}

	bool ValidateTrackedSlice()
	{
		// The persistent parity's safety net for missed detaches.
		return Diagnostics::Observe().scene;
	}

	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass)
	{
		// The persistent parity's slot check (the normal path trusts the reference counts).
		return Diagnostics::Observe().scene && (a_slotsFreed || Diagnostics::ParityDue(a_pass));
	}
}
