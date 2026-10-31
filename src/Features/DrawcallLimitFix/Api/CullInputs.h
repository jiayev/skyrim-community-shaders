#pragma once

// The main cull input contract (Api::InstallContext::RegisterMainCullInput): what the main camera's cull reads besides the frustum, from
// the walk a feature makes after Main::Update's own (Features/Portals' port of the portal/occluder walk). Core needs a walk: the primary
// cull's scene lists keep the object root only while the walk reached unbound space, and the depth commits send each fade root's program
// (an index into the feature's programs: an encoding Core does not read, which FadeStateCS and BuildDraws' phase 1 test each root's by).
// One input: a second fails the install. Without one the primary cull stays off (its install finds no walk: Extensions::MainWalkInstalled).
// The programs themselves are the feature's scene resources and commit hook, which Core's cull reads at a cull binding (below).
//
// And the cull binding contract (RegisterCullBinding): the buffers Core's cull (BuildDraws, VisibilityEvents) and the root states' stage
// read at Core's fixed descriptor words, each slot's buffer a feature's (its scene resources or pool), selected per pass or commit.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/CullInputs.h), which the features inside DCLF include for them.
	class RootProgramsJudge;
	class CullBindingSelect;

	/**
	 * @brief The main camera's cull input. Read where Core decides; every callback is per frame, per commit, per event or once, never per
	 * object (the per-root programs are asked in one batch).
	 *
	 * - name: the input's (the install's refusals name it).
	 * - noProgram: the program of a root culled by the frustum alone, which a root without a node takes.
	 * - unboundVisible: render thread, once a frame (the scene lists' keep check): whether this frame's walk reached unbound space.
	 * - structure: any thread, per attach/detach the primary cull's structure events follow (PrimaryCull::NoteListStructure).
	 * - programsVersion: render thread, per depth commit: changes when judge's answers may have.
	 * - judge: render thread, per depth commit when the roots or programsVersion moved: each root's program, the roots asked in one batch.
	 * - test: parity only (the fade log's CPU re-check), any thread a parity observer runs on: the GPU's test on the CPU, of program
	 *   a_program in the programs a_programs (the words a readback saved), for the sphere (a_centre, a_radius) of a node with a_nodeFlags:
	 *   1 in view, 0 culled, -1 no program; a_why names the deciding test.
	 * - programWords: parity only (the fade log, with test), render thread: the programs test reads, as the walk encoded them now (the
	 *   words a log window saves for its re-check).
	 */
	struct MainCullInputContribution
	{
		std::string_view name;
		std::uint32_t noProgram = ~0u;
		bool (*unboundVisible)() = nullptr;
		void (*structure)(const void* a_parent) = nullptr;
		std::uint64_t (*programsVersion)() = nullptr;
		void (*judge)(RootProgramsJudge& a_roots) = nullptr;
		int (*test)(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags,
			std::string& a_why) = nullptr;
		std::span<const std::uint32_t> (*programWords)() = nullptr;
	};

	/**
	 * @brief The cull bindings' slots: the buffers Core's cull reads at its fixed descriptor words (the shaders' constants), by what they are.
	 *
	 * - Programs: the main cull input's programs as the frame's depth commit published them (a pooled backing's SRV index, the depth
	 *   latch's portalProgramsIndex: the member and fade query stages and their batches; Features/Portals' "cs.dclf.portal-programs").
	 * - RootPrograms: each listed root's program (by root slot).
	 * - RootRows: the listed roots' static rows (by root slot).
	 * - RootStates: the roots' published states a frame's cull reads (by root slot; may alternate by frame).
	 * - RootStepStates: the root states' step's own state rows (by root slot): this frame's once the step ran (the visibility events read
	 *   them; RootStates is the published copy a frame's cull reads).
	 * - RootSeeds: the roots' seeds (a frame value pool's: by SRV index).
	 * - RootFrame: the root states' frame inputs.
	 * - MemberQueries/MemberResults: main-camera bound queries and tagged verdicts, indexed by stable object slot.
	 * - MemberClaims: per-object atomic claims during query generation; reused as query ranks after that pass.
	 * - LightContexts: the frame's portal context atlas (the camera's and the selected portal-strict lights' walks: a pooled backing's SRV
	 *   index; Features/LocalLights' "cs.dclf.light-contexts"), which a strict light's views and the colour pass's volumes name an entry of.
	 * - StrictGates: each strict-light gate's verdict per atlas context, this frame's (a pooled backing's SRV index; Portals/StrictGates.h's
	 *   words).
	 * - ObjectGates: each object's strict-light gate (by object slot: a versioned buffer of the feature's scene resources), which the
	 *   StrictGates verdicts are of.
	 */
	enum class CullBindingSlot : std::uint8_t
	{
		Programs,
		RootPrograms,
		RootRows,
		RootStates,
		RootStepStates,
		RootSeeds,
		RootFrame,
		MemberQueries,
		MemberResults,
		MemberClaims,
		LightContexts,
		StrictGates,
		ObjectGates,
		Count
	};

	/**
	 * @brief A cull binding: slot's buffer, one registration a slot.
	 *
	 * - name: the binding's (the install's refusals name it).
	 * - select: render thread (a pass's declaration, revision and preparation; a commit), per pass or commit, never per object: the buffer
	 *   the slot reads, answered on a_select (Core's: the scene's tables, the frame, the adopted set where there is one): Versioned for a
	 *   versioned buffer of the feature's scene resources, Buffer for a plain one, Index for a pooled buffer's SRV index; with Versioned or
	 *   Buffer, Rows for how many of its units its readers may read now. No answer: the slot is unbound (its readers take what they do
	 *   without it). It reads only handles made at the scene's setup, its pool, and the frame.
	 */
	struct CullBinding
	{
		CullBindingSlot slot = CullBindingSlot::Count;
		std::string_view name;
		void (*select)(CullBindingSelect& a_select) = nullptr;
	};
}
