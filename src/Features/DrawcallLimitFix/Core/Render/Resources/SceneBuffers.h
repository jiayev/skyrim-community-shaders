#pragma once

// The scene's GPU tables shared by every epoch (SceneBuffers: the vertex stream, the features' scene resources, fade roots and states,
// readback lists, the draw families' published pipelines), and the commits' uploads into them (render thread). Included by Internal.h.

namespace DCLF::Draws
{
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
		// Fade roots (FadeStateCS): static rows by root slot, GPU state rows, the frame's inputs, and CS_DCLF_FADE_PARITY's log.
		Versioned fadeRoots, fadeStates;
		std::shared_ptr<org::Buffer> fadeFrameBuffer, fadeLog;
		// The main camera's portal programs (PortalViews::Encoded) and a mirror of what was sent into the version filled.
		Versioned portalPrograms;
		org::services::FilledVersion portalProgramsFilled;
		std::vector<std::uint32_t> portalProgramsSent;
		Versioned fadeRootPrograms;  // per root slot: its program (PortalViews::ProgramOf)
		Versioned fadeAnimated;    // per root slot: the scene frame whose animation batch updated it
		org::services::FilledVersion fadeAnimatedFilled;
		// CPU mirrors: only differing runs go up.
		std::vector<std::uint32_t> fadeAnimatedMirror;
		std::vector<std::uint32_t> fadeAnimatedCounts;  // per root slot: the batch's updates while it is counted, else 0
		std::vector<std::uint32_t> fadeRootProgramsMirror;
		org::services::FilledContents<> fadeRootProgramsFilled;  // the root and program versions it holds
		// What the programs mirror was judged against: its key (as filled), the roots' content epoch and PortalViews::ProgramsVersion.
		struct ProgramsMirrorOf
		{
			std::uint64_t key = 0, rootsEpoch = 0, portals = 0;
		} fadeRootProgramsMirrorOf;
		// FadeStateCS's published states per frame parity; builds read the previous frame's. Every version is zero-filled (builds
		// then take the static row's state).
		std::array<Versioned, 2> fadeStatesOut;
		/** @brief The SRV index of the published states a frame's builds read (the previous frame's), as a_sealed names them. */
		std::uint32_t FadeStatesReadIndex(const NamedVersions& a_sealed, std::uint32_t a_frame) const { return a_sealed.Of(fadeStatesOut[(a_frame + 1) & 1]).srvIndex; }
		org::services::FilledContents<> fadeRootsFilled;  // the root rows' content epoch (SceneTables::FadeRootsContentEpoch)
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
		// Visibility readback (null without its program): events (count zeroed by every depth commit), and each root's and
		// object's last reported verdict. Every version is zero-filled; the decoder restarts with the first batch of a version it
		// has not followed (visibilityReportedFollowed: root, object).
		Versioned visibilityEvents, visibilityRootReported, visibilityObjectReported;
		std::array<org::services::FilledVersion, 2> visibilityReportedFollowed;
		std::shared_ptr<const ComputeProgram> visibilityEventsProgram;
		std::shared_ptr<VisibilityReadback> visibilityReadback;
		// The draw families' pipelines on their lane (Api::DrawFamily, by family; the TreeInstances lane's TreeLodPipelines), published
		// by the frame's decision once built (DrawFamilyFrame::Pipelines); read on the graph host's thread (FamilyPipelinesOf).
		std::array<std::atomic<std::shared_ptr<const TreeLodPipelines>>, Extensions::kDrawFamilies> familyPipelines;
	};

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
	 * the inputs and changed portal program words.
	 */
	template <class Uploads>
	void UploadFadeRoots(const NamedVersions& a_sealed, const SceneTables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs, std::uint32_t a_logBase,
		SceneBuffers& a_scene, Uploads& a_uploads, const std::vector<std::uint32_t>& a_programs)
	{
		if (!a_scene.fadeState || !a_scene.fadeRoots)
			return;
		const std::uint64_t current = a_tables.FadeRootsContentEpoch();
		const auto& roots = a_sealed.Of(a_scene.fadeRoots);
		const std::uint64_t contentEpoch = a_scene.fadeRootsFilled.Since(roots).value_or(~0ull);
		if (contentEpoch != current && a_tables.fadeRoots.size() <= roots.units) {
			const auto* rows = a_tables.fadeRoots.data();
			a_tables.fadeRootsJournal.Take().ForEachRun(contentEpoch, a_tables.fadeRoots.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				a_uploads(a_scene.fadeRoots, rows + a_first, static_cast<std::size_t>(a_count * sizeof(FadeRootStatic)), a_first * sizeof(FadeRootStatic));
			});
			a_scene.fadeRootsFilled.Filled(roots, current);
		}
		a_scene.fadeRootCount = a_scene.FadeRootsContentEpoch(a_sealed) == current ? static_cast<std::uint32_t>(a_tables.fadeRoots.size()) : 0u;
		if (a_scene.fadeFrameNumber != a_frame) {
			a_scene.fadeFrameNumber = a_frame;
			a_scene.fadeFrame = a_inputs;
			// Past the named version's words (an encoding newer than the revision's requirement), send an empty header: roots take
			// the frustum alone that frame.
			if (a_scene.portalPrograms) {
				static const std::uint32_t kNone[kPortalHeaderBytes / 4]{};
				const auto& programs = a_sealed.Of(a_scene.portalPrograms);
				const bool fits = a_programs.size() <= programs.units;
				const auto* words = fits ? a_programs.data() : kNone;
				const std::size_t count = fits ? a_programs.size() : std::size(kNone);
				auto& sent = a_scene.portalProgramsSent;
				if (!a_scene.portalProgramsFilled.Holds(programs)) {
					sent.clear();
					a_scene.portalProgramsFilled.Filled(programs);
				}
				std::vector<std::uint32_t> differing;
				for (std::uint32_t w = 0; w < count; ++w)
					if (w >= sent.size() || sent[w] != words[w])
						differing.push_back(w);
				SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					a_uploads(a_scene.portalPrograms, words + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				sent.assign(words, words + count);
			}
			a_scene.fadeLogBase = a_logBase;
		}
		a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
		a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
		a_scene.fadeFrame.logBase = a_scene.fadeLogBase;
		a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
		// The write-back's header is ArmFadeReadback's.
	}

}
