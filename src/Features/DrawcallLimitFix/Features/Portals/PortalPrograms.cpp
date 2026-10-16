// Portals' programs buffer (PortalPrograms.h's encoding, "cs.dclf.portal-programs"): its scene resources (made with the scene's tables,
// required to the words the revision's inputs posted), the depth commit's upload of the frame's encoding (the commit hook), the cull
// binding Core's cull reads it by (Api::CullBindingSlot::Programs), and PortalTest on the CPU (the main cull input's test).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"

#include <array>

namespace DCLF::Features::Portals
{
	namespace
	{
		constexpr const char* kProgramsName = "cs.dclf.portal-programs";
		// The encoding of no programs (zeros): the buffer's small start, and what a depth commit sends past the named version's words.
		constexpr std::array<std::uint32_t, kPortalHeaderBytes / 4> kZeroHeader{};
		constexpr std::uint32_t kProgramWordsInput = 0, kInputWords = 1;

		/** @brief The programs buffer and a mirror of what was sent into the version filled (render thread, but for the handle). */
		struct ProgramsScene
		{
			Draws::Versioned programs;
			org::services::FilledVersion filled;
			std::vector<std::uint32_t> sent;
			std::uint32_t frame = 0;  // the scene frame last sent
		};

		Api::SceneResourcesId sceneId;  // install

		ProgramsScene* SceneOf(const Draws::SceneBuffers& a_scene)
		{
			return Extensions::SceneResourcesOf<ProgramsScene>(a_scene, sceneId);
		}

		// The scene's tables are made (the first frame with a device): the buffer, a header's words at a small start.
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto scene = std::make_shared<ProgramsScene>();
			scene->programs = Draws::MakeVersioned(Draws::CreateWords(a_setup.smallStart ? static_cast<std::uint32_t>(kZeroHeader.size()) : 4096u, false, kProgramsName),
				{ .extent = { 0, 4, 4 } });
			return scene;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			Draws::Register(a_graph.graph, org::ResourceIdentifier(kProgramsName), a_graph.State<ProgramsScene>().programs);
		}

		// To the most a frame's encoding needs (the words posted with the revision's inputs).
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			Draws::SceneBufferSet::Get().Require(a_requirements.build, a_requirements.State<ProgramsScene>().programs, a_requirements.inputs[kProgramWordsInput]);
		}

		// This frame's encoding's words: what the buffer must hold.
		void PostInputs(std::span<std::uint32_t> a_words)
		{
			a_words[kProgramWordsInput] = static_cast<std::uint32_t>(PortalViews::Get().Encoded().size());
		}

		/**
		 * @brief The depth commit's programs, once a frame, into the version a_sealed (the adopted set) names: only the words that differ
		 * from what the version was sent.
		 */
		void UploadPrograms(const Draws::NamedVersions& a_sealed, std::uint32_t a_frame, ProgramsScene& a_scene, Draws::CommitUploads& a_uploads)
		{
			if (a_scene.frame == a_frame)
				return;
			a_scene.frame = a_frame;
			// Past the named version's words (an encoding newer than the revision's requirement), send an empty header: roots take
			// the frustum alone that frame.
			const auto& encoded = PortalViews::Get().Encoded();
			const auto& programs = a_sealed.Of(a_scene.programs);
			const bool fits = encoded.size() <= programs.units;
			const auto* words = fits ? encoded.data() : kZeroHeader.data();
			const std::size_t count = fits ? encoded.size() : kZeroHeader.size();
			auto& sent = a_scene.sent;
			if (!a_scene.filled.Holds(programs)) {
				sent.clear();
				a_scene.filled.Filled(programs);
			}
			std::vector<std::uint32_t> differing;
			for (std::uint32_t w = 0; w < count; ++w)
				if (w >= sent.size() || sent[w] != words[w])
					differing.push_back(w);
			Draws::SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
				a_uploads(a_scene.programs, words + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
			});
			sent.assign(words, words + count);
		}

		// The depth commit only: FadeStateCS (the next frame's state) and BuildDraws' phase 1 read the frame's programs.
		void Commit(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			if (auto* scene = SceneOf(a_commit.scene))
				UploadPrograms(a_commit.adopted, a_commit.frame, *scene, a_commit.uploads);
		}

		void SelectPrograms(Api::CullBindingSelect& a_select)
		{
			if (const auto* scene = SceneOf(a_select.scene))
				a_select.Versioned(scene->programs);
		}
	}

	int PortalTest(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags, std::string& a_why)
	{
		return PortalViews::Visible(a_programs, a_program, a_centre, a_radius, a_nodeFlags, a_why);
	}

	void RegisterPrograms(Api::InstallContext& a_context)
	{
		sceneId = a_context.RegisterSceneResources({ .name = "portal-programs", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require,
			.postInputs = &PostInputs, .inputWords = kInputWords });
		a_context.RegisterCommitHook({ .name = "portal-programs", .commit = &Commit });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::Programs, .name = kProgramsName, .select = &SelectPrograms });
	}
}
