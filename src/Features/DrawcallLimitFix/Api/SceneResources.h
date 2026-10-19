#pragma once

// The scene resources contract (Api::InstallContext::RegisterSceneResources): a feature's GPU state held with the scene's tables
// (Draws::SceneBuffers) - its programs and buffers, the versioned ones registered with the scene's set (SceneBufferSet), which grows
// them to what the feature requires of each revision and names their versions.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/SceneResources.h), which the features inside DCLF include for them.
	class SceneResourcesSetup;
	class SceneResourcesGraph;
	class SceneResourcesRequirements;
	class SceneResourcesFit;
	class SceneResourcesLatch;
	class SceneResourcesSealed;

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
	 *   in registration order (the vertex stream's first: Api/VertexStreams.h) and before the passes' setups (Api::PassDef::setup): makes the feature's programs and buffers (a
	 *   versioned one through SceneResourcesSetup::MakeVersioned, which registers it with the scene's set in this order). Its result is
	 *   the state, held with the scene's tables until they go; null: none, and the other callbacks are not called.
	 * - registerGraph (optional): render thread, when a graph extension registers the scene's buffers (RegisterSceneBuffers), in the same
	 *   order: the buffers its passes declare, each under its name.
	 * - require (optional): the snapshot builder, each revision's requirements (RequireSceneTables), in the same order: the
	 *   units its versioned buffers must hold for the publication's tables (and the words postInputs posted for the revision:
	 *   SceneResourcesRequirements::inputs). A set is sealed only once they hold them. SceneResourcesRequirements::objectRows exposes the already witnessed object-slot count. Of the tables it reads only the table families
	 *   (a family's rows, Api::TableFamily::rows, where it counts them; anything of one that does not, which its stamp follows) and the
	 *   vertex stream's rows' ranges (SceneTables::streamRows: object, first, count): a make whose witnesses of those, of the posted
	 *   words and of the rest of the requirement pass stood keeps the last revision without calling it (Impl::MakeRevision). Anything
	 *   else it would read goes through postInputs.
	 * - fit (optional): the snapshot builder or the render thread, wherever a SceneFit is made from the versions a set names: what its
	 *   buffers hold of the slots the objects read them by. An object past them waits (ObjectFits).
	 * - postInputs (optional, with inputWords): render thread, wherever Core posts the revision's inputs (PostRevisionInputs): the
	 *   feature's inputWords words of them (what its requirements grow by: a mirror's slots), into a_words (zeroed). A change of them
	 *   is a new revision's inputs (RevisionInputs' equality); require reads them as posted. Any state or none: it reads its own.
	 * - latched (optional): render thread, where the main view's latched targets are listed (MainLatchedTargets: the graph's build) and
	 *   the depth shape's latch is laid out (MainLatchedLayout): the buffers the depth commit latches (CommitContext::latched), each with
	 *   its bytes from offset 0, after the frame buffers' and before the draw count's.
	 * - sealed (optional): the snapshot builder, once a revision's set is sealed (Impl::MakeRevision), before any shape of it, in the
	 *   same order: what the feature keeps by the versions the set names (Features/Fades: its event list's host staging for the capacity
	 *   the set names, HostEventRing::AddSet).
	 *
	 * The state is the render thread's (its commits: Api/Commits.h), but for what registerGraph, require, fit and sealed read: the handles
	 * setup made, never replaced.
	 */
	struct SceneResources
	{
		std::string_view name;
		std::shared_ptr<void> (*setup)(const SceneResourcesSetup& a_setup) = nullptr;
		void (*registerGraph)(const SceneResourcesGraph& a_graph) = nullptr;
		void (*require)(const SceneResourcesRequirements& a_requirements) = nullptr;
		void (*fit)(SceneResourcesFit& a_fit) = nullptr;
		void (*postInputs)(std::span<std::uint32_t> a_words) = nullptr;
		std::uint32_t inputWords = 0;
		void (*latched)(SceneResourcesLatch& a_latch) = nullptr;
		void (*sealed)(const SceneResourcesSealed& a_sealed) = nullptr;
	};
}
