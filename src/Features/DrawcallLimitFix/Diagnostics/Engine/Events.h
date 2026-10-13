#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <ankerl/unordered_dense.h>

#include "RegistrationCensus.h"

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
	class BSRenderPass;
	class NiAVObject;
	class NiCullingProcess;
}

namespace DCLF::Diagnostics
{
	/**
	 * @brief The engine area's hook points (Hooks.h: Notify, Armed, Subscribe). Each event names where the main path announces it, on
	 * which thread; the observers subscribe in Diagnostics/Engine/Install.cpp.
	 */

	/** @brief PrimaryCull::Install, its call sites patched (render thread, PostPostLoad): the observers' own engine hooks go in here. */
	struct PrimaryCullInstalled
	{
	};

	/** @brief PrimaryCull::PrepareFrame, render thread, after the full-frustum cull: the frame's list processes (before the stand-in's preconditions). */
	struct CutFrameBegin
	{
		RE::NiCullingProcess* const* processes;
		std::uint32_t count;
	};

	/** @brief PrimaryCull::AfterListJobs, render thread, after the list jobs' Finish (the animated batch taken): the cull is done. */
	struct ListJobsDone
	{
		std::uint32_t frame;
	};

	/** @brief PrimaryCull::PrepareFrame, render thread: the cut followed the frame's snapshot (before the switch selections and admission). */
	struct CutSynced
	{
		std::uint32_t frame;
	};

	/** @brief PrimaryCull::PrepareFrame, render thread: the list jobs' outputs reset for this frame's a_count processes. */
	struct CutSlotsReset
	{
		std::uint32_t count;
	};

	/** @brief A list job (its slot's thread) reached eligible entry a_entry, its root a_root, before the stand-in's verdict. */
	struct StandInEntry
	{
		int slot;
		std::uint32_t entry;
		const RE::NiAVObject* root;
	};

	/** @brief A list job's stand-in handed member a_member (a_geometry) of entry a_entry to the engine's registration. */
	struct StandInHandedOver
	{
		int slot;
		std::uint32_t entry;
		std::uint32_t member;
		const RE::BSGeometry* geometry;
	};

	/**
	 * @brief A list job: the engine culled an admitted entry with engine-drawn parts (its OnVisible ran on a_root); a_metricBefore and
	 * a_visibleBefore are the fade root's metric (+0x144) and last visible frame (+0x13C) before it (a_fadeRoot: the plan's fade root).
	 */
	struct MixedEntryCulled
	{
		int slot;
		RE::NiAVObject* root;
		bool fadeRoot;
		float metricBefore;
		std::int32_t visibleBefore;
	};

	/** @brief PrimaryCull::AfterListJobs, render thread, the stand-in ran this frame: the jobs' outputs gathered. */
	struct ListJobsMerged
	{
		std::uint32_t frame;
	};

	/** @brief PrimaryCull::CheckLightMasks, render thread, after the registration jobs (the scene phase's frame inputs). */
	struct MainRegistrationsDone
	{
	};

	/** @brief PrimaryCull::AfterFullFrustum, render thread, after the frame's preparation (the fade roots' nodes stand until the list jobs). */
	struct FullFrustumDone
	{
	};

	/** @brief PrimaryCull::PublishListFilter, render thread, first thing: this frame's scene lists are decided next. */
	struct SceneListsPublishing
	{
	};

	/**
	 * @brief ... a frame the engine builds its lists whole (Policy::FilterSceneLists false): the roots the filter would have left out
	 * (a_roots, held by a_hold; null: no filter), and whether the build is the object root's exterior branch (a_keepable).
	 */
	struct SceneListsWhole
	{
		std::shared_ptr<const void> hold;
		const ankerl::unordered_dense::set<const RE::NiAVObject*>* roots;
		bool keepable;
	};

	/** @brief DrawWorld_BuildSceneLists' job: the engine built the lists (ListMode::Engine), before DCLF's filter. */
	struct EngineSceneListsBuilt
	{
	};

	/** @brief PrimaryCull::AtUpdateEntry, main thread at Main::Update's entry, after the visibility and fade writes. */
	struct UpdateEntry
	{
		bool running;
	};

	/** @brief PortalViews::Build, render thread, right after Main::Update's walk: this frame's programs built (before they are encoded). */
	struct PortalWalkBuilt
	{
	};

	/** @brief LightSelection's detour of CalculateActiveShadowCasterLights, render thread: before the engine's function runs. */
	struct LightSelectionBegin
	{
	};

	/** @brief ... after the engine's function and DCLF's selection (LightSelection::Select). */
	struct LightSelectionMade
	{
	};

	// The sun's accumulation (SunAccumulation's hooks; render thread unless said).

	/** @brief ExcludeEntries applied this frame's exclusion: a_skipExcluded is Policy::ExcludeSun's answer (false: the cascades skip nothing). */
	struct SunExclusionApplied
	{
		bool skipExcluded;
	};

	/** @brief The sun's Accumulate, before the engine's (its cascades captured from DCLF's views). */
	struct SunAccumulateBegin
	{
		std::uint32_t frame;
	};

	/** @brief ... after the engine's Accumulate returned. */
	struct SunAccumulateEnd
	{
		std::uint32_t frame;
	};

	/** @brief A cascade of the sun's Accumulate without DCLF's view (SunViews has none for its descriptor this frame). */
	struct SunCascadeMissing
	{
		std::uint32_t frame;
		std::uint32_t descriptor;
	};

	/** @brief A set caster the cascade registration skipped (claimed), with its cascade's descriptor index. */
	struct SunCasterClaimed
	{
		const RE::BSGeometry* geometry;
		std::uint32_t descriptor;
	};

	/** @brief An unclaimed caster the engine's cascade registration built passes for. */
	struct SunCasterRegistered
	{
		const RE::BSGeometry* geometry;
	};

	/** @brief A cascade cull returned (FUN_140e305c0): its process has the cascade's planes; a_cascade indexes the Accumulate's cascades. */
	struct SunCascadeCulled
	{
		const void* fullProcess;
		const RE::NiCullingProcess* process;
		std::uint32_t cascade;
	};

	// The point lights' culls (LocalLightCull's hooks).

	/** @brief LocalLightCull::SelectFrame, render thread: this frame's exclusion selected; a_whole: the culls skip nothing (Policy::FilterLightCulls). */
	struct LightCullsSelected
	{
		bool whole;
	};

	/** @brief PassCapture's registration hooks (any registration thread): a pass handed to a batch renderer, withheld or not (a_source: the insertion's hint, 0 RegisterPass). */
	struct PointLightPass
	{
		const RE::BSBatchRenderer* batch;
		const RE::BSRenderPass* pass;
		bool withheld;
		std::uint32_t source;
	};

	/**
	 * @brief LocalLightCull::NoteMainRegistration on a frame the culls keep whole (any registration thread): a geometry's main
	 * registration with the bits DCLF would give it (a_entry: its excluded entry, ~0u none; a_reach: the lights by reach kind) and the
	 * point lights' bits the engine's culls left in its mask (a_engine).
	 */
	struct PointLightBits
	{
		const RE::BSGeometry* geometry;
		std::uint32_t entry;
		std::uint32_t bits;
		const std::array<std::uint32_t, 3>& reach;
		std::uint32_t engine;
	};

	// The shadow lights' views (render thread).

	/** @brief SunViews::Update made the sun's views from the cameras UpdateCamera set up. */
	struct SunViewsUpdated
	{
	};

	/** @brief LightViews::Update, the first light of a new frame, when the last frame had local views. */
	struct LightViewsFrame
	{
	};

	/** @brief FocusViews::Update, the first host of a new frame, when the last frame had focus hosts. */
	struct FocusViewsFrame
	{
	};

	/** @brief PassCapture's registration hook (any registration thread): a pass into a covered focus view's renderer. */
	struct FocusRegistration
	{
		std::uint32_t descriptor;
		const void* geometry;
	};

	// The registrations (PassCapture's hooks, on whatever thread the engine registers).

	/** @brief The main modes' hint 1 list insertion withheld a member's pass from the main camera's views. */
	struct MemberListPassWithheld
	{
		const RE::BSRenderPass* pass;
	};

	/** @brief The main modes' insertion into geometry group 5 withheld a member's pass from the main camera's views. */
	struct MemberGroup5PassWithheld
	{
		const RE::BSRenderPass* pass;
	};

	/** @brief A registration hook handed a pass on to the engine's batch renderer (a_face: the reflection faces withhold now). */
	struct PassKept
	{
		const RE::BSBatchRenderer* batch;
		const RE::BSRenderPass* pass;
		RegistrationCensus::Site site;
		bool face;
		std::uint8_t occlusionPhase;
	};
}
