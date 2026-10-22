#pragma once

// Tree LOD's scene resources (Api::SceneResources): the TreeInstances family's tables (its layout, TreeInstancesLayout.h), its cull
// program and counts' readbacks, held with the scene's tables, and what the depth commit's uploads keep. Included by the feature's
// sources that use them.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeInstancesLayout.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Features::TreeLod
{
	// Tree LOD's cull (TreeLodCullCS.hlsl) in the depth segment's two phases and a trees view's faces: a thread per instance (phase 1)
	// or retest entry (2), kTreeLodCullGroup a group (the faces report it: their cullGroup, which sizes a trees view's per-face dispatch).
	inline constexpr const char* kTreeLodCullShader = "DrawcallLimitFix/Features/TreeLod/TreeLodCullCS.hlsl";
	inline constexpr std::uint32_t kTreeLodCullGroup = 64;
	struct TreeLodCullConstants
	{
		std::uint32_t shapesIndex = 0, instancesIndex = 0, visibleIndex = 0, latchIndex = 0, latchOffset = 0, drawIndex = 0;
		std::uint32_t phase = 0;
		std::uint32_t retestOffset = 0;  // bytes into the visible list
		std::uint32_t hzbIndex = 0, hzbSizePacked = 0, hzbMips = 0, padding = 0;
	};
	inline constexpr std::uint32_t kTreeLodCullConstantWords = sizeof(TreeLodCullConstants) / 4;

	/**
	 * @brief Tree LOD cull counts (TreeLodReadbackPass), read back per frame slot on the graph host's thread.
	 */
	struct TreeLodCounts
	{
		std::vector<std::shared_ptr<org::Buffer>> readback;
		std::vector<std::uint8_t> filled;
		std::atomic<std::uint64_t> frames{ 0 }, phaseOne{ 0 }, retests{ 0 }, phaseTwo{ 0 };
		// CS_DCLF_TREE_LOD_STAGES: the newest frame judged, frames judged and those a stage missed.
		std::uint32_t stagesJudged = 0;
		std::uint64_t stageFrames = 0, stageMisses = 0, stageLogged = 0, clipDiffers = 0, clipLogged = 0;
	};

	/**
	 * @brief Tree LOD's tables (dclf-architecture.md, "Tree LOD: the draws"; Api::SceneResources' state, render thread but for the handles, made once at setup): shape
	 * rows, instance records (75 per shape slot), mesh rows, draw row and visible list, read by address in the vertex stage (the draw
	 * row's names the adopted set's versions). TreeLodCullPass fills the list in the depth epoch. ready: the row names its slots.
	 *
	 * Without the cull program, or without the tables' device addresses, there is none (setup gives null): tree LOD stays the engine's.
	 */
	struct TreeLodTables
	{
		Draws::Versioned shapes, instances, meshes, visible;
		std::array<org::services::FilledVersion, 3> filled;  // shapes, instances, meshes
		std::shared_ptr<org::Buffer> draw;
		std::uint64_t drawAddress = 0;
		std::shared_ptr<const ComputeProgram> cull;
		std::atomic<bool> ready{ false };
		std::uint64_t uploads = 0, slotsSent = 0;  // since the last report
		std::shared_ptr<TreeLodCounts> counts;
		// CS_DCLF_TREE_LOD_STAGES' stamps (null without it) and their device address.
		std::shared_ptr<org::Buffer> stages;
		std::uint64_t stagesAddress = 0;
		// The last depth commit's draw row; a trees view's faces reuse the tables with their own lists.
		TreeInstances::DrawRow row{};
	};

	// The words the tables post with the revision's inputs (Api::SceneResources::postInputs): what their requirements grow by, the mirror's
	// shape and mesh slots (a trees view's faces' lists are required at the first: TreeLodFaces.cpp).
	inline constexpr std::uint32_t kShapeSlotsInput = 0, kMeshSlotsInput = 1, kInputWords = 2;

	/** @brief The tables in a_scene (null before the scene's tables are made, or without them). */
	TreeLodTables* TablesOf(const Draws::SceneBuffers& a_scene);
	/** @brief TablesOf, sharing their ownership (a pass's state). */
	std::shared_ptr<TreeLodTables> SharedTablesOf(const Draws::SceneBuffers& a_scene);
	/** @brief The tables' registration (RegisterTables). */
	Api::SceneResourcesId TablesId();

	/**
	 * @brief Tree LOD's texture and sampler into a_row: the engine's tree LOD atlas (BSDistantTreeShader::SetupTechnique's static at
	 * 0x1433dcd18) and slot 0's sampler as its draws leave it (address mode 0, filter 2). Binding owners go to the execution.
	 * False when there is no texture.
	 */
	bool TreeLodTextureBinding(TreeInstances::DrawRow& a_row, std::vector<std::shared_ptr<const void>>& a_owners);

	/** @brief Whether the engine's tree LOD atlas is there to draw with (the frame's decision; the depth commit binds it). */
	bool TreeLodAtlasReady();
}
