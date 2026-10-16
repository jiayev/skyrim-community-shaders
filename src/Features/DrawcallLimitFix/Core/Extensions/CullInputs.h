#pragma once

// The main cull input contract's Core side (Api/CullInputs.h): the input's entry, resolved once at InstallFeatures (none registered: no
// walk, no programs buffer); the batch its judge answers (RootProgramsJudge); and whether its walk's hook is in, which the primary cull's
// install requires (noted from the feature's early hooks, before InstallFeatures).

#include "Features/DrawcallLimitFix/Api/CullInputs.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Api
{
	/**
	 * @brief The fade roots a depth commit asks the judge about (render thread, UploadFadeRootPrograms): Count of them, each one's node
	 * (Root: non-null, a root without one takes noProgram unasked), and the answer Put back by the same index. Core keeps the batch's
	 * storage beside its programs mirror (SceneBuffers::fadeRootProgramsBatch): no allocation per commit once grown.
	 */
	class RootProgramsJudge
	{
	public:
		std::size_t Count() const { return asked.size(); }
		const void* Root(std::size_t a_index) const { return nodes[roots[asked[a_index]]]; }
		void Put(std::size_t a_index, std::uint32_t a_program) const { programs[asked[a_index]] = a_program; }

		std::span<const void* const> nodes;     // by root slot (SceneTables::fadeRootNode)
		std::span<const std::uint32_t> roots;   // the roots judged, in Core's order
		std::span<const std::uint32_t> asked;   // by question: its place in roots (those with a node)
		std::span<std::uint32_t> programs;      // by place in roots: the answers
	};
}

namespace DCLF::Extensions
{
	/** @brief The input's entry: its registration (copied, its names owned) and the encoding of no programs (headerWords zeros). */
	struct MainCullInputEntry
	{
		std::string name, programsName;
		std::vector<std::uint32_t> zeroHeader;
		Api::MainCullInputContribution contribution;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no name: none). */
	void ResolveMainCullInput(const Api::MainCullInputContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): the input's entry, or null without one. */
	const MainCullInputEntry* MainCullInput();
	/** @brief Render thread: the input's programs this frame (Api::MainCullInputContribution::programs); empty without one. */
	std::span<const std::uint32_t> MainCullPrograms();

	/** @brief Main thread at PostPostLoad, from a feature's early hooks: the main walk's hook is in. */
	void NoteMainWalkInstalled();
	/** @brief Main thread at PostPostLoad (PrimaryCull::Install): whether NoteMainWalkInstalled came; the primary cull requires it. */
	bool MainWalkInstalled();
}
