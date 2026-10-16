#pragma once

// The scene's GPU tables shared by every epoch (SceneBuffers: the vertex stream, the features' scene resources, readback lists, the draw
// families' published pipelines), Core's fade roots' scene resources (FadeScene: registered through the Fades contracts, stage F2) and
// the commits' uploads into them (render thread). Included by Internal.h.

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"

namespace DCLF::Api
{
	class CommitContext;
	class InstallContext;
}

namespace DCLF::Draws
{
	/**
	 * @brief Core's fade roots' scene resources (Api::SceneResources "fade roots", RegisterCoreFadeRender; Features/Fades' in stage F3),
	 * made with the scene's tables when the fade state program loads (SetUpFadeScene): FadeStateCS's buffers by root slot, its frame row
	 * and log, the root programs and animation stamps with their mirrors, the published states, and the write-back ring. Core's cull reads
	 * them at its cull bindings (Api::CullBindingSlot's Root* slots), its draws at the frame bindings t120/t119.
	 */
	struct FadeScene
	{
		// Fade roots (FadeStateCS): static rows by root slot, GPU state rows, the frame's inputs, and CS_DCLF_FADE_PARITY's log.
		Versioned fadeRoots, fadeStates;
		std::shared_ptr<org::Buffer> fadeFrameBuffer, fadeLog;
		Versioned fadeRootPrograms;  // per root slot: its program (Api::MainCullInputContribution::judge)
		Versioned fadeAnimated;    // per root slot: the scene frame whose animation batch updated it
		org::services::FilledVersion fadeAnimatedFilled;
		// CPU mirrors: only differing runs go up.
		std::vector<std::uint32_t> fadeAnimatedMirror;
		std::vector<std::uint32_t> fadeAnimatedCounts;  // per root slot: the batch's updates while it is counted, else 0
		std::vector<std::uint32_t> fadeRootProgramsMirror;
		org::services::FilledContents<> fadeRootProgramsFilled;  // the root and program versions it holds
		// What the programs mirror was judged against: its key (as filled), the roots' content epoch and the input's programsVersion.
		struct ProgramsMirrorOf
		{
			std::uint64_t key = 0, rootsEpoch = 0, programs = 0;
		} fadeRootProgramsMirrorOf;
		// The judge's batch (Api::RootProgramsJudge), kept between commits: the roots judged, the questions (those with a node) and the
		// answers.
		struct RootProgramsBatch
		{
			std::vector<std::uint32_t> roots, asked, programs;
		} fadeRootProgramsBatch;
		// FadeStateCS's published states per frame parity; builds read the previous frame's. Every version is zero-filled (builds
		// then take the static row's state).
		std::array<Versioned, 2> fadeStatesOut;
		/** @brief The published states a frame's builds read (the previous frame's). */
		const Versioned& FadeStatesRead(std::uint32_t a_frame) const { return fadeStatesOut[(a_frame + 1) & 1]; }
		org::services::FilledContents<> fadeRootsFilled;  // the root rows' content epoch (the root states' family's journal)
		/** @brief The root rows' content epoch the fadeRoots version a_sealed names holds (~0: none). */
		std::uint64_t FadeRootsContentEpoch(const NamedVersions& a_sealed) const { return fadeRootsFilled.Since(a_sealed.Of(fadeRoots)).value_or(~0ull); }
		std::uint32_t fadeRootCount = 0;
		std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
		FadeFrame fadeFrame{};
		std::uint32_t fadeLogBase = ~0u;
		std::shared_ptr<const ComputeProgram> fadeState;
		// Fade write-back: event list (count zeroed by every depth commit), each root's last reported generation and milestone
		// (every version zero-filled: no root reported yet, as a zero generation is never a listing's).
		Versioned fadeEvents, fadeReported;
		std::shared_ptr<FadeWriteBack> fadeWriteBack;
	};

	/**
	 * @brief The scene's tables, each a buffer of the scene's set (SceneBufferSet): every capacity a commit, build or pass reads is
	 * its named version's units, which the revision producer requires to hold the tables (RequireSceneTables).
	 */
	struct SceneBuffers
	{
		// The object records, extras and geometry table are the installed payload's (PayloadVersions).
		// The vertex stream's buffer (Api::VertexStreamContribution::select: Features/Faces' face positions), of its scene resources'
		// state, resolved once at setup: the second vertex stream the passes bind. Null without a stream.
		Versioned vertexStream;
		// The index pool every plain indexed draw binds; kept current by every commit that draws from it (UpdateIndexPool).
		std::shared_ptr<IndexPool> pool;
		// The features' scene resources' states (Api::SceneResources::setup, by registration: Extensions::RegisteredSceneResources;
		// Extensions::SceneResourcesOf), made with these tables, the vertex stream's first, and held until they go.
		std::vector<std::shared_ptr<void>> sceneResources;
		// The features' GPU stages' states (Api::GpuStage::setup, by registration: Extensions::RegisteredStages); null: out of the graph.
		std::vector<std::shared_ptr<const void>> stages;
		// Visibility readback (null without its program, or without the root states' rows: Api::CullBindingSlot::RootRows): events
		// (count zeroed by every depth commit), and each root's and object's last reported verdict. Every version is zero-filled; the
		// decoder restarts with the first batch of a version it has not followed (visibilityReportedFollowed: root, object).
		Versioned visibilityEvents, visibilityRootReported, visibilityObjectReported;
		std::array<org::services::FilledVersion, 2> visibilityReportedFollowed;
		std::shared_ptr<const ComputeProgram> visibilityEventsProgram;
		std::shared_ptr<VisibilityReadback> visibilityReadback;
		// The draw families' pipelines on their lane (Api::DrawFamily, by family), published by the frame's decision once built
		// (DrawFamilyFrame::Pipelines); read on the graph host's thread (FamilyPipelinesOf).
		std::array<std::atomic<std::shared_ptr<const FamilyPipelines>>, Extensions::kDrawFamilies> familyPipelines;
	};

	/** @brief Core's fade roots' scene resources in a_scene (null: none made). FadeRenderRegistration.cpp. */
	FadeScene* FadeSceneOf(const SceneBuffers& a_scene);
	/** @brief FadeSceneOf, sharing the state's ownership (a stage's pass holds it). */
	std::shared_ptr<FadeScene> SharedFadeSceneOf(const SceneBuffers& a_scene);

	/**
	 * @brief The root states' family in a_tables (Api::RootStatesContribution::family; null without a producer, or before its first
	 * write): its objectRow is every object's root slot, read as data.
	 */
	inline const Extensions::TableFamilyColumns* RootFamilyOf(const SceneTables& a_tables)
	{
		return a_tables.families.Find(Extensions::RootStatesFamily());
	}

	/**
	 * @brief Cull binding a_slot's descriptor for frame a_frame as a_adopted names it (render thread, a commit's latch): a versioned
	 * buffer's SRV, a plain buffer's, a pooled buffer's index; 0 unbound.
	 */
	inline std::uint32_t CullBindingIndex(Api::CullBindingSlot a_slot, const SceneBuffers& a_scene, std::uint32_t a_frame, const NamedVersions& a_adopted)
	{
		const auto select = Extensions::SelectCullBinding(a_slot, a_scene, a_frame, &a_adopted);
		if (select.versioned && *select.versioned)
			return a_adopted.Of(**select.versioned).srvIndex;
		if (select.buffer && *select.buffer)
			return (*select.buffer)->GetSRVInfo(0).slot.index;
		return select.srv;
	}

	/**
	 * @brief Whether the root states' rows are bound and hold the tables' roots for the commit (Api::CullBindingSlot::RootRows, its rows
	 * answer): Core's cull binds them only then.
	 */
	inline bool RootRowsCurrent(const SceneBuffers& a_scene)
	{
		const auto select = Extensions::SelectCullBinding(Api::CullBindingSlot::RootRows, a_scene);
		return select.versioned && *select.versioned && select.rows;
	}

	// Core's fade roots' registrations' parts (RegisterCoreFadeRender, in its order), each in the source of the code it was:
	/** @brief Scene resources' setup: the fade state program and buffers (null without the program). Resources.cpp. */
	std::shared_ptr<void> SetUpFadeScene(const Api::SceneResourcesSetup& a_setup);
	/** @brief Scene resources' graph registration: every fade buffer under its name. Common.cpp. */
	void RegisterFadeSceneGraph(const Api::SceneResourcesGraph& a_graph);
	/** @brief Scene resources' requirements: the buffers by root slot to the family's rows, the event list to what a frame wanted. Requirements.cpp. */
	void RequireFadeScene(const Api::SceneResourcesRequirements& a_requirements);
	/** @brief The depth commit hook: the root rows and FadeStateCS's frame inputs, the write-back armed, the roots' programs. Uploads.cpp. */
	void CommitFadeRoots(const Api::CommitContext& a_commit);
	/** @brief The GPU stages at DepthSegmentCullState: the fade state, then its events' copy to the host. SceneState.cpp. */
	void RegisterFadeStages(Api::InstallContext& a_context);
	/** @brief The root states' kick and wait (Api::RootStatesContribution): the fade write-back task, and its join. Uploads.cpp. */
	void KickFadeWriteBack();
	void WaitFadeWriteBack();
	/**
	 * @brief Core's fade roots' render parts registered through the Fades contracts (from Scene::RegisterCoreFades, after its family,
	 * component and channels): the seeds pool (the fade roots component's seeds), the scene resources, the depth commit hook, the stages,
	 * the frame bindings t120/t119 and the Root* cull bindings. FadeRenderRegistration.cpp.
	 */
	void RegisterCoreFadeRender(Api::InstallContext& a_context);

	/** @brief Sends sorted word indices as runs a_send(first, count), merging gaps up to kFadeRunGap. */
	constexpr std::uint32_t kFadeRunGap = 32;
	template <class Send>
	void SendWordRuns(const std::vector<std::uint32_t>& a_sorted, Send&& a_send)
	{
		for (std::size_t i = 0; i < a_sorted.size();) {
			const std::uint32_t first = a_sorted[i];
			std::uint32_t lastIndex = first;
			while (++i < a_sorted.size() && a_sorted[i] - lastIndex <= kFadeRunGap)
				lastIndex = a_sorted[i];
			a_send(first, lastIndex - first + 1);
		}
	}

	/**
	 * @brief The depth commit's fade uploads into the versions a_sealed (the adopted set) names: missing static rows, and once a frame
	 * the inputs (the main cull input's programs are its scene resources', sent by its commit hook).
	 */
	template <class Uploads>
	void UploadFadeRoots(const NamedVersions& a_sealed, const SceneTables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs, std::uint32_t a_logBase,
		FadeScene& a_scene, Uploads& a_uploads)
	{
		if (!a_scene.fadeState || !a_scene.fadeRoots)
			return;
		// The roots as the root states' family lists them (its view), and its journal's runs.
		const auto list = Extensions::RootListOf(a_tables);
		const auto* family = RootFamilyOf(a_tables);
		const std::uint64_t current = list.contentEpoch;
		const auto& roots = a_sealed.Of(a_scene.fadeRoots);
		const std::uint64_t contentEpoch = a_scene.fadeRootsFilled.Since(roots).value_or(~0ull);
		if (contentEpoch != current && list.rows.size() <= roots.units) {
			const auto* rows = list.rows.data();
			if (family)
				family->Journal().Take().ForEachRun(contentEpoch, list.rows.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
					a_uploads(a_scene.fadeRoots, rows + a_first, static_cast<std::size_t>(a_count * sizeof(FadeRootStatic)), a_first * sizeof(FadeRootStatic));
				});
			a_scene.fadeRootsFilled.Filled(roots, current);
		}
		a_scene.fadeRootCount = a_scene.FadeRootsContentEpoch(a_sealed) == current ? static_cast<std::uint32_t>(list.rows.size()) : 0u;
		if (a_scene.fadeFrameNumber != a_frame) {
			a_scene.fadeFrameNumber = a_frame;
			a_scene.fadeFrame = a_inputs;
			a_scene.fadeLogBase = a_logBase;
		}
		a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
		a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
		a_scene.fadeFrame.logBase = a_scene.fadeLogBase;
		a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
		// The write-back's header is ArmFadeReadback's.
	}

}
