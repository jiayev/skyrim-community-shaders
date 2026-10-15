#pragma once

// The commit hook contract's Core side (Api/Commits.h): what a registered hook's commit is handed, and the call the commits make. And the
// frame constant rows' (Api::FrameConstantRow): Core's feature rows of PS b13 they take, and Core's built-ins (the main commits latch them
// in ascending row order, Extensions::RegisteredFrameConstantRows: LatchFrameSlots and MainLatchedLayout).

#include "Features/DrawcallLimitFix/Api/Commits.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
		class LatchedUploads;
	}
}

namespace DCLF::Api
{
	/**
	 * @brief A hook's commit (render thread): what the frame adopted, the commit's uploads into it, its latched uploads (the values its
	 * epoch's first pass copies: a scene resources' latched buffers, SceneResources::latched) and what its execution holds.
	 */
	class CommitContext
	{
	public:
		CommitKind kind;
		const org::services::NamedVersions& adopted;  // the set the frame adopted: the versions the commit writes into, and their capacities
		const SceneTables& tables;                    // the frame's tables (FrameView::GetTables)
		std::uint32_t frame;                          // the scene frame (FrameView::GetFrame)
		Draws::SceneBuffers& scene;                   // the scene's tables (a feature's scene resources: Extensions::SceneResourcesOf)
		Draws::CommitUploads& uploads;                // the commit's staged uploads (into a versioned buffer: the version adopted names)
		Draws::LatchedUploads* latched;               // the main commits' latched uploads (a value no copy is laid out for is staged); null: the shadow's
		std::vector<std::shared_ptr<const void>>& bindingOwners;  // held until the commit's execution retires (a texture a row binds)
	};
}

namespace DCLF::Extensions
{
	/** @brief Render thread (a commit, with the scene's streams): every registered hook's commit, in order. */
	void RunCommitHooks(const Api::CommitContext& a_commit);

	/**
	 * @brief Core's feature rows of the lighting slot (PS b13): c17 up to c19, after the extras' pixel row (c16). A frame constant row
	 * takes one of them (Features/Manifest.cpp checks the features' rows, the install every registration's).
	 */
	inline constexpr std::uint32_t kFrameConstantRowFirst = 17;
	inline constexpr std::uint32_t kFrameConstantRowsEnd = 20;
	/**
	 * @brief Core's own frame constant rows, registered ahead of the features' (Registry's InstallFeatures): the Effect port's camera
	 * planes, light colour and blood (c17-c19), until Features/Effects registers them (Scene/Ports/EffectFrame.cpp).
	 */
	std::span<const Api::FrameConstantRow> CoreFrameConstantRows();
}
