#pragma once

#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::RegistrationCensus
{
	/**
	 * @brief The registration census (CS_DCLF_CENSUS; dclf-validation.md, "The registration census"): every pass a
	 * registration hook hands on to a batch renderer (PassCapture: RegisterPass and the direct group insertions), withheld ones
	 * not, by view and class, reported every report interval. An observer: it decides nothing and changes nothing.
	 *
	 * The hooks run wherever the engine registers (the scene list jobs, the render thread): Note pushes the pass's geometry (a
	 * reference, so it outlives the frame's registrations), renderer and the per-pass values that are gone by the frame's end
	 * onto a lock-free queue. Collect, on the render thread at Present (the report's), drains it and classifies each pass there:
	 * the view by its batch renderer, the class by DCLF's own state as the render thread may read it - the frame's set
	 * (Coordinator::FrameClaims), the installed tables' records, the category nodes of the newest publication, and the live leaf's
	 * static verdict. A Lighting geometry that is no member of its view's phase gets the coordinator's verdict (Coordinator's tracked
	 * entries: Ineligible's reasons with the per-frame ones, a record, bound): read directly while the scene work runs inline (the
	 * parities); otherwise asked of the coordinator - Collect posts the distinct geometries seen since its last request, pinned,
	 * the next scene pass answers each (Diagnostics::CensusVerdictOf) and hands the pins back with the reply, and a later Collect takes it -
	 * with the live approximation (from the frame's copies, the per-frame verdicts not told apart) only until a reply arrives, its
	 * classes marked so. Nothing is read of the engine on DCLF's threads, and no engine reference is let go of there.
	 */
	inline bool Enabled()
	{
		static const bool on = SwitchEnabled(Switch::Census);
		return on;
	}

	/** @brief Where the hook saw the pass. */
	enum class Site : std::uint8_t
	{
		RegisterPass,   // BSBatchRenderer::RegisterPass (vfunc 0x02)
		MainGroup,      // the main modes' direct group insertions (FUN_1414b2330)
		MainList,       // the main modes' hint 1 list insertion
		ShadowDirect,   // the shadow modes' direct insertions of hints 11, 7 and 3 (FUN_1414b2a60)
		Volumetric,     // the shadow modes' volumetric-only insertion into group 15
		Occlusion,      // the occlusion maps' insertion into group 14 (render mode 0x1C)
	};

	enum NoteFlags : std::uint8_t
	{
		kReflectionFace = 1u << 1,  // inside a face render in the plain mode (PassCapture::SetViewWithholdActive): the faces withhold then
	};

	/**
	 * @brief Any thread (the registration hooks), only while Enabled: a pass the engine keeps. a_occlusionPhase: the occlusion map's
	 * phase (PassCapture::SetOcclusionPhase) at an occlusion insertion.
	 */
	void Note(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, Site a_site, std::uint8_t a_flags, std::uint8_t a_occlusionPhase = 0);

	/** @brief Install, while Enabled: Note subscribed to the registration hooks' kept passes (Diagnostics::PassKept). */
	void Install();

	/** @brief Render thread, at Present (Reports::RenderThread), every frame: the passes noted since, classified and counted. */
	void Collect();

	/** @brief Render thread, after Collect: the window's report lines (empty when nothing was noted), and the window restarted. */
	std::string TakeReport();
}
