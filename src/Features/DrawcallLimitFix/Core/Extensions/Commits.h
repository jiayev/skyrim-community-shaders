#pragma once

// The commit hook contract's Core side (Api/Commits.h): what a registered hook's commit is handed, and the call the commits make.

#include "Features/DrawcallLimitFix/Api/Commits.h"

#include <cstdint>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct SceneBuffers;
		class CommitUploads;
	}
}

namespace DCLF::Api
{
	/** @brief A hook's commit (render thread): what the frame adopted, and the commit's uploads into it. */
	class CommitContext
	{
	public:
		CommitKind kind;
		const org::services::NamedVersions& adopted;  // the set the frame adopted: the versions the commit writes into, and their capacities
		const SceneTables& tables;                    // the frame's tables (FrameView::GetTables)
		std::uint32_t frame;                          // the scene frame (FrameView::GetFrame)
		Draws::SceneBuffers& scene;                   // the scene's tables (a feature's scene resources: Extensions::SceneResourcesOf)
		Draws::CommitUploads& uploads;                // the commit's staged uploads (into a versioned buffer: the version adopted names)
	};
}

namespace DCLF::Extensions
{
	/** @brief Render thread (a commit, with the scene's streams): every registered hook's commit, in order. */
	void RunCommitHooks(const Api::CommitContext& a_commit);
}
