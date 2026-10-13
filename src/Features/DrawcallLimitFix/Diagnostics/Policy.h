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

	/** @brief The sun's cascades skip the entries DCLF excludes on frame a_frame (else the engine accumulates them). Normal path: true. */
	bool ExcludeSun(std::uint32_t a_frame);

	/** @brief The engine's scene lists are kept as DCLF's filter leaves them on frame a_frame (else the engine builds them whole). Normal path: true. */
	bool FilterSceneLists(std::uint32_t a_frame);

	/** @brief The engine renders occlusion map a_map (0 Skylighting's, 1 the precipitation mask) whole this frame, before DCLF's, as a reference. Normal path: false. */
	bool RenderOcclusionReference(std::uint32_t a_map);

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

	/** @brief This reflection update keeps the face roots DCLF owns (their members' registrations watched for the residue); a_updates counts the calls. Normal path: false. */
	bool KeepReflectionRoots(std::uint32_t& a_updates);

	/** @brief Pass a_pass's walk makes the shadow sets again whole after their incremental update (the two compared). Normal path: false. */
	bool RebuildShadowSets(std::uint64_t a_pass);

	/** @brief The scene pass checks a slice of the tracked set against the live scene graph and drops the entries whose detach the events missed. Normal path: false. */
	bool ValidateTrackedSlice();

	/** @brief The accumulate phase checks every bound record's slots after a pass that freed one (a_slotsFreed) or on pass a_pass, and neutralise a record naming a slot no member holds. Normal path: false. */
	bool NeutraliseDanglingSlots(bool a_slotsFreed, std::uint64_t a_pass);
}
