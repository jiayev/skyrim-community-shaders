// Policy's answers, each derived from the run's Observation (any thread: Observe() is read once and immutable).
#include "Policy.h"

#include "Observation.h"
#include "Feature/Skylight.h"

namespace DCLF::Policy
{
	bool ScenePassInline()
	{
		// The observers that read the scene state from the engine's hooks while the scene work would run: the persistent and walk parities,
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

	bool WithholdFromEngine()
	{
		// The capture check compares DCLF's draws with the engine's own draws of the same objects.
		return !Diagnostics::Observe().engineDrawsAll;
	}

	bool RecomputeCachedVerdicts(Verdicts a_verdicts, std::uint64_t a_pass)
	{
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_pass, a_verdicts == Verdicts::Classification ? 17 : 13);
	}

	bool SerialFanout()
	{
		// The mirror check's live reads note their leases and differences on the pass's own thread.
		return Diagnostics::Observe().mirror;
	}

	bool HiddenWitness(std::uint64_t a_pass)
	{
		return Diagnostics::Observe().walkOrScene && Diagnostics::ParityDue(a_pass);
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
		// The residue's watch (the reflection report's): every 30th update while the parities observe.
		return Diagnostics::Observe().claimsOrScene && a_updates++ % 30 == 0;
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

	bool SampleEveryMover(std::uint64_t a_serial)
	{
		// The persistent parity's check of the frame values' moved-only sampling.
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_serial, 30);
	}

	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass)
	{
		// The persistent parity's slot check (the normal path trusts the reference counts).
		return Diagnostics::Observe().scene && (a_slotsFreed || Diagnostics::ParityDue(a_pass));
	}

	// ---- W4 (the snapshot builder's main build).
	bool CheckBuilderPairs(std::uint64_t a_build)
	{
		// The persistent parity's check of the region's narrowed pair checks (the build parity builds no region).
		return Diagnostics::Observe().scene && Diagnostics::ParityDue(a_build, 29);
	}

	bool CheckRowPacking(std::uint64_t a_build)
	{
		// The build parity's whole builds check every build; the persistent parity's region builds on a schedule.
		return Diagnostics::Observe().builds || (Diagnostics::Observe().scene && Diagnostics::ParityDue(a_build, 31));
	}

	bool CheckRegionResync()
	{
		// The persistent parity's check of the region's parallel resync.
		return Diagnostics::Observe().scene;
	}

	// ---- W3 (publication pacing).
	bool CheckPacedPublication(std::uint64_t a_publication)
	{
		// The set parity's: its passes run on the lane (the persistent parity's run inline, and every inline pass publishes).
		return Diagnostics::Observe().claims && Diagnostics::ParityDue(a_publication, 7);
	}

	// ---- W1 (placement deltas and walk scheduling).
	bool WitnessRootSchedule(std::uint64_t)
	{
		// The persistent parity's: every pass with a dirty root (they come with attaches and detaches, so a cadence would miss most).
		return Diagnostics::Observe().scene;
	}

	bool CheckGeometryScan()
	{
		// The persistent parity's: stale and refreshed slots are rare, so every scan that has any.
		return Diagnostics::Observe().scene;
	}
}
