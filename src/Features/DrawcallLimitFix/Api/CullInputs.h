#pragma once

// The main cull input contract (Api::InstallContext::RegisterMainCullInput): what the main camera's cull reads besides the frustum, from
// the walk a feature makes after Main::Update's own (Features/Portals' port of the portal/occluder walk). Core needs a walk: the primary
// cull's scene lists keep the object root only while the walk reached unbound space, and the depth commits send its programs (an encoding
// Core does not read: FadeStateCS and BuildDraws' phase 1 test each root's) and each fade root's program. One input: a second fails the
// install. Without one the primary cull stays off (its install finds no walk: Extensions::MainWalkInstalled) and no programs are sent.

#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/CullInputs.h), which the features inside DCLF include for it.
	class RootProgramsJudge;

	/**
	 * @brief The main camera's cull input. Read where Core decides; every callback is per frame, per commit, per event or once, never per
	 * object (the per-root programs are asked in one batch).
	 *
	 * - name: the input's (the install's refusals name it).
	 * - programsName: the programs buffer's graph and debug name ("cs.dclf.portal-programs"), which Core makes with its fade buffers.
	 * - headerWords: the encoding of no programs, in words (zeros): the buffer's small start, and what a depth commit sends when a frame's
	 *   encoding outgrows the version the revision named (the roots take the frustum alone that frame).
	 * - noProgram: the program of a root culled by the frustum alone, which a root without a node takes.
	 * - unboundVisible: render thread, once a frame (the scene lists' keep check): whether this frame's walk reached unbound space.
	 * - structure: any thread, per attach/detach the primary cull's structure events follow (PrimaryCull::NoteListStructure).
	 * - programs: render thread, per revision's inputs (their words size the buffer) and per depth commit (the words sent, once a frame).
	 * - programsVersion: render thread, per depth commit: changes when judge's answers may have.
	 * - judge: render thread, per depth commit when the roots or programsVersion moved: each root's program, the roots asked in one batch.
	 */
	struct MainCullInputContribution
	{
		std::string_view name, programsName;
		std::uint32_t headerWords = 0;
		std::uint32_t noProgram = ~0u;
		bool (*unboundVisible)() = nullptr;
		void (*structure)(const void* a_parent) = nullptr;
		std::span<const std::uint32_t> (*programs)() = nullptr;
		std::uint64_t (*programsVersion)() = nullptr;
		void (*judge)(RootProgramsJudge& a_roots) = nullptr;
	};
}
