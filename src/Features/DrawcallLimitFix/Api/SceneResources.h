#pragma once

// The scene resources contract (Api::InstallContext::RegisterSceneResources): a feature's GPU state held with the scene's tables
// (Draws::SceneBuffers) - its programs and buffers, the versioned ones registered with the scene's set (SceneBufferSet), which grows
// them to what the feature requires of each revision and names their versions.

#include <cstdint>
#include <memory>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/SceneResources.h), which the features inside DCLF include for them.
	class SceneResourcesSetup;
	class SceneResourcesGraph;
	class SceneResourcesRequirements;
	class SceneResourcesFit;

	/** @brief Registered scene resources (RegisterSceneResources' result): Extensions::SceneResourcesOf finds their state by it. */
	struct SceneResourcesId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A feature's scene resources.
	 *
	 * - setup: render thread, once, where the scene's tables are made (the first frame with a device; IndirectDraws' EnsureSceneBuffers),
	 *   in registration order, after the face positions and before the GPU stages' setups: makes the feature's programs and buffers (a
	 *   versioned one through SceneResourcesSetup::MakeVersioned, which registers it with the scene's set in this order). Its result is
	 *   the state, held with the scene's tables until they go; null: none, and the other callbacks are not called.
	 * - registerGraph (optional): render thread, when a graph extension registers the scene's buffers (RegisterSceneBuffers), after the
	 *   face positions: the buffers its passes declare, each under its name.
	 * - require (optional): the snapshot builder, each revision's requirements (RequireSceneTables), after the face positions: the
	 *   units its versioned buffers must hold for the publication's tables. A set is sealed only once they hold them.
	 * - fit (optional): the snapshot builder or the render thread, wherever a SceneFit is made from the versions a set names: what its
	 *   buffers hold of the slots the objects read them by. An object past them waits (ObjectFits).
	 *
	 * The state is the render thread's (its commits: Api/Commits.h), but for what registerGraph, require and fit read: the handles setup
	 * made, never replaced.
	 */
	struct SceneResources
	{
		std::string_view name;
		std::shared_ptr<void> (*setup)(const SceneResourcesSetup& a_setup) = nullptr;
		void (*registerGraph)(const SceneResourcesGraph& a_graph) = nullptr;
		void (*require)(const SceneResourcesRequirements& a_requirements) = nullptr;
		void (*fit)(SceneResourcesFit& a_fit) = nullptr;
	};
}
