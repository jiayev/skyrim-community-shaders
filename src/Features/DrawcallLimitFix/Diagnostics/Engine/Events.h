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
	 * which thread; the observers subscribe in Diagnostics/Engine/EngineInstall.cpp.
	 */

	/** @brief The engine area's points that carry nothing (On<EnginePoint::...>, Hooks.h); render thread unless said. */
	enum class EnginePoint : std::uint8_t
	{
		PrimaryCullInstalled,   // PrimaryCull::Install, its call sites patched (PostPostLoad): the observers' own engine hooks go in here
		CutSynced,              // PrimaryCull::PrepareFrame: the cut followed the frame's snapshot (before the switch selections and admission)
		FullFrustumDone,        // PrimaryCull::AfterFullFrustum, after the frame's preparation (the fade roots' nodes stand until the list jobs)
		ListJobsDone,           // PrimaryCull::AfterListJobs, after the list jobs' Finish (the animated batch taken): the cull is done
		ListJobsMerged,         // ... the stand-in ran this frame: the jobs' outputs gathered
		SceneListsPublishing,   // PrimaryCull::PublishListFilter, first thing: this frame's scene lists are decided next
		EngineSceneListsBuilt,  // DrawWorld_BuildSceneLists' job: the engine built the lists (ListMode::Engine), before DCLF's filter
		PortalWalkBuilt,        // PortalViews::Build, right after Main::Update's walk: this frame's programs built (before they are encoded)
		LightSelectionBegin,    // LightSelection's detour of CalculateActiveShadowCasterLights: before the engine's function runs
		LightSelectionMade,     // ... after the engine's function and DCLF's selection (LightSelection::Select)
		SunAccumulateBegin,     // the sun's Accumulate, before the engine's (its cascades captured from DCLF's views)
		SunViewsUpdated,        // SunViews::Update made the sun's views from the cameras UpdateCamera set up
		LightViewsFrame,        // LightViews::Update, the first light of a new frame, when the last frame had local views
		FocusViewsFrame,        // FocusViews::Update, the first host of a new frame, when the last frame had focus hosts
	};

	/** @brief PrimaryCull::PrepareFrame, render thread, after the full-frustum cull: the frame's list processes (before the stand-in's preconditions). */
	struct CutFrameBegin
	{
		RE::NiCullingProcess* const* processes;
		std::uint32_t count;
	};

	/** @brief PrimaryCull::PrepareFrame, render thread: the list jobs' outputs reset for this frame's `count` processes. */
	struct CutSlotsReset
	{
		std::uint32_t count;
	};

	/** @brief A list job (its slot's thread) reached eligible entry `entry`, its root `root`, before the stand-in's verdict. */
	struct StandInEntry
	{
		int slot;
		std::uint32_t entry;
		const RE::NiAVObject* root;
	};

	/** @brief A list job's stand-in handed member `member` (`geometry`) of entry `entry` to the engine's registration. */
	struct StandInHandedOver
	{
		int slot;
		std::uint32_t entry;
		std::uint32_t member;
		const RE::BSGeometry* geometry;
	};

	/**
	 * @brief ... a frame the engine builds its lists whole (Policy::FilterSceneLists false): the roots the filter would have left out
	 * (`roots`, held by `hold`; null: no filter), and whether the build is the object root's exterior branch (`keepable`).
	 */
	struct SceneListsWhole
	{
		std::shared_ptr<const void> hold;
		const ankerl::unordered_dense::set<const RE::NiAVObject*>* roots;
		bool keepable;
	};

	/** @brief PrimaryCull::AtUpdateEntry, main thread at Main::Update's entry, after the visibility and fade writes. */
	struct UpdateEntry
	{
		bool running;
	};

	// The sun's accumulation (SunAccumulation's hooks; render thread unless said).

	/** @brief ExcludeEntries applied this frame's exclusion: `skipExcluded` is Policy::ExcludeSun's answer (false: the cascades skip nothing). */
	struct SunExclusionApplied
	{
		bool skipExcluded;
	};

	/** @brief A cascade of the sun's Accumulate without DCLF's view (SunViews has none for its descriptor this frame). */
	struct SunCascadeMissing
	{
		std::uint32_t frame;
		std::uint32_t descriptor;
	};

	/** @brief An unclaimed caster the engine's cascade registration built passes for. */
	struct SunCasterRegistered
	{
		const RE::BSGeometry* geometry;
	};

	/** @brief A cascade cull returned (FUN_140e305c0): its process has the cascade's planes; `cascade` indexes the Accumulate's cascades. */
	struct SunCascadeCulled
	{
		const void* fullProcess;
		const RE::NiCullingProcess* process;
		std::uint32_t cascade;
	};

	// The point lights' culls (LocalLightCull's hooks).

	/** @brief LocalLightCull::SelectFrame, render thread: this frame's exclusion selected; `whole`: the culls skip nothing (Policy::FilterLightCulls). */
	struct LightCullsSelected
	{
		bool whole;
	};

	/** @brief PassCapture's registration hooks (any registration thread): a pass handed to a batch renderer, withheld or not (`source`: the insertion's hint, 0 RegisterPass). */
	struct PointLightPass
	{
		const RE::BSBatchRenderer* batch;
		const RE::BSRenderPass* pass;
		bool withheld;
		std::uint32_t source;
	};

	/**
	 * @brief LocalLightCull::NoteMainRegistration on a frame the culls keep whole (any registration thread): a geometry's main
	 * registration with the bits DCLF would give it (`entry`: its excluded entry, ~0u none; `reach`: the lights by reach kind) and the
	 * point lights' bits the engine's culls left in its mask (`engine`).
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

	/** @brief A registration hook handed a pass on to the engine's batch renderer (`face`: the reflection faces withhold now). */
	struct PassKept
	{
		const RE::BSBatchRenderer* batch;
		const RE::BSRenderPass* pass;
		RegistrationCensus::Site site;
		bool face;
		std::uint8_t occlusionPhase;
	};
}
