#pragma once

#include <cstdint>

namespace DCLF::Policy
{
	/**
	 * @brief The questions on which an observer changes what the main path does: a parity that needs the engine's own result on its
	 * frames, a reference built in full, or the scene work kept where its observers read it.
	 *
	 * Every answer here with the observers off (no `CS_DCLF_*` parity or probe switch) is the normal path's, and the main path asks
	 * nothing else of the diagnostics to decide its work (it never reads a parity switch itself). Policy.cpp derives each answer from
	 * Observation.
	 */

	/** @brief The scene pass runs inline at the frame's start on the render thread (the pump's wakes dropped), where the observers that read the scene state from the engine's hooks keep their placement. Normal path: false. */
	bool ScenePassInline();

	/** @brief The primary cull's stand-in walks every entry each frame, not only the entries it refreshes. Normal path: false. */
	bool WalkEveryEntry();

	/** @brief The local light culls are filtered by DCLF's exclusion on frame a_frame (else the engine culls every candidate). Normal path: true. */
	bool FilterLightCulls(std::uint32_t a_frame);

	/** @brief The engine's scene lists are kept as DCLF's filter leaves them on frame a_frame (else the engine builds them whole). Normal path: true. */
	bool FilterSceneLists(std::uint32_t a_frame);

	/** @brief The shadow views copy the engine's per-frame block even where DCLF owns the view's camera. Normal path: false. */
	bool CaptureEngineShadowBlock();

	/** @brief The set's members are withheld from the engine's views (else the engine draws everything while DCLF draws its set too). Normal path: true. */
	bool WithholdFromEngine();

	/** @brief Which cached verdict RecomputeCachedVerdicts asks about. */
	enum class Verdicts : std::uint8_t
	{
		Classification,  // the walk's cached classification
		Derivation,      // the accumulate phase's cached derivation
	};

	/** @brief Pass a_pass recomputes the cached a_verdicts as well as serving them (the two compared). Normal path: false. */
	bool RecomputeCachedVerdicts(Verdicts a_verdicts, std::uint64_t a_pass);

	/** @brief The scene pass's evaluations run interleaved on the pass's own thread (fan-out 0) whatever CS_DCLF_FANOUT asks: the mirror reads' live checks are the pass's. Normal path: false. */
	bool SerialFanout();

	/** @brief The hidden witness reads every gated entry's hidden chain on pass a_pass. Normal path: false. */
	bool HiddenWitness(std::uint64_t a_pass);

	/** @brief The main builds reuse the resident region's cache (else every build is whole). Normal path: true. */
	bool ReuseResidentRegion();

	/** @brief A shadow build reuses the exclusion it made before when nothing moved, on frame a_frame (else it builds in full). Normal path: true. */
	bool ReuseShadowExclusion(std::uint32_t a_frame);

	/** @brief The colour pass's alpha-tested draws record each pixel they shade (the foliage record: buffers, frame block words, the pixel stage's writes). Normal path: false. */
	bool RecordFoliagePixels();

	/** @brief The trees' wind clocks stand still (each frame's tree wind is the frame before's). Normal path: false. */
	bool FreezeTreeWind();

	/** @brief Pass a_pass's walk makes the shadow sets again whole after their incremental update (the two compared). Normal path: false. */
	bool RebuildShadowSets(std::uint64_t a_pass);

	/** @brief The scene pass checks a slice of the tracked set against the live scene graph and drops the entries whose detach the events missed. Normal path: false. */
	bool ValidateTrackedSlice();

	/** @brief The frame values' producer a_serial also samples every mover and root of its plan, against the rows its moved-only sampling left (nothing written). Normal path: false. */
	bool SampleEveryMover(std::uint64_t a_serial);

	/** @brief The accumulate phase checks every bound record's slots after a pass that freed one (a_slotsFreed) or on pass a_pass, and neutralise a record naming a slot no member holds. Normal path: false. */
	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass);

	// ---- W4 (the snapshot builder's main build).
	/** @brief Main build a_build checks its narrowed pair checks (and their resolution on the pool) against every pair resolved again serially, and a parallel region resync against a serial one. Normal path: false. */
	bool CheckBuilderPairs(std::uint64_t a_build);

	/** @brief Main build a_build checks that every pipeline of a material packs its one row alike (MainRows' row per material). Normal path: false. */
	bool CheckRowPacking(std::uint64_t a_build);

	/** @brief A main build's parallel region resync is checked slot by slot against the serial path's verdicts (resyncs are rare: each one). Normal path: false. */
	bool CheckRegionResync();

	// ---- W3 (publication pacing).
	/** @brief Publication a_publication, made after deferred passes, checks its tables and claims against the coordinator's whole (an every-pass publication's). Normal path: false. */
	bool CheckPacedPublication(std::uint64_t a_publication);

	// ---- W1 (placement deltas and walk scheduling).
	/** @brief Pass a_pass also schedules a dirty root's dependents that kept its motion verdict, each merge checked to change nothing (ScheduleRoot's witnesses). Normal path: false. */
	bool WitnessRootSchedule(std::uint64_t a_pass);

	/** @brief A geometry scan with stale or refreshed slots also scans every object slot, against the index's (GeometryScan). Normal path: false. */
	bool CheckGeometryScan();

	// ---- W8 (the attach burst: mirror, subtrees, category nodes).
	/** @brief Pass a_pass counts the mirror's property and alpha records no geometry counts (a capture drops only its own keys' uncounted records, not a walk over the maps). Normal path: false. */
	bool CheckMirrorUses(std::uint64_t a_pass);

	/** @brief The category members' index is checked against the tracked entries: on every refresh that drops a cell or portal root (a_removal; the dropped set against the whole scan too) and on pass a_pass. Normal path: false. */
	bool CheckCategoryMembers(bool a_removal, std::uint64_t a_pass);
	/** @brief The persistent parity's: the tracked entries' ancestor keys against the mirror's chains (Coordinator::CheckAncestorKeys). */
	bool CheckAncestorKeys(std::uint64_t a_pass);

	/** @brief The mirror checks its own apply every batch: skipped replays walked anyway, prepared captures against their records, the update log against its chains. Normal path: false. */
	bool CheckMirrorApply();
}
