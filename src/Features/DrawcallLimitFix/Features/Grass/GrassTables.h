#pragma once

// Grass's scene resources (Api::SceneResources): the Grass family's tables (its layout, GrassLayout.h), its cull and arguments
// programs and counts' readbacks, held with the scene's tables, and what the depth commit's uploads keep. Included by the feature's
// sources that use them.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassLayout.h"
#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"

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

namespace DCLF::Features::Grass
{
	// Grass's cull (GrassCullCS.hlsl): a thread per instance slot (phase 1) or retest entry (2), kGrassCullGroup a group.
	inline constexpr const char* kGrassCullShader = "DrawcallLimitFix/Features/Grass/GrassCullCS.hlsl";
	inline constexpr std::uint32_t kGrassCullGroup = 64;
	struct GrassCullConstants
	{
		std::uint32_t drawIndex = 0, instancesIndex = 0, ownersIndex = 0, shapesIndex = 0, bucketsIndex = 0, frameIndex = 0, visibleIndex = 0, drawsIndex = 0;
		std::uint32_t latchIndex = 0, latchOffset = 0;
		std::uint32_t phase = 0;
		std::uint32_t retestOffset = 0;  // bytes into the visible list
		std::uint32_t hzbIndex = 0, hzbSizePacked = 0, hzbMips = 0, padding = 0;
	};
	inline constexpr std::uint32_t kGrassCullConstantWords = sizeof(GrassCullConstants) / 4;
	static_assert(kGrassCullConstantWords == 16);

	// Grass's draws' arguments (GrassArgsCS.hlsl): a thread per draw slot (bucket * 3 + tier), kGrassArgsGroup a group, in one of three
	// modes (phase 1's depth draws, phase 2's, the colour draws, which also zero the counts).
	inline constexpr const char* kGrassArgsShader = "DrawcallLimitFix/Features/Grass/GrassArgsCS.hlsl";
	inline constexpr std::uint32_t kGrassArgsGroup = 64;
	inline constexpr std::uint32_t kArgsPhaseOne = 1, kArgsPhaseTwo = 2, kArgsColour = 3;
	struct GrassArgsConstants
	{
		std::uint32_t drawIndex = 0, bucketsIndex = 0, meshesIndex = 0, drawsIndex = 0;
		std::uint32_t mode = 0, drawSlots = 0, padding[2]{};
	};
	inline constexpr std::uint32_t kGrassArgsConstantWords = sizeof(GrassArgsConstants) / 4;
	static_assert(kGrassArgsConstantWords == 8);

	/**
	 * @brief Grass's cull counts (GrassReadbackPass: the draws buffer's header), read back per frame slot on the graph host's thread.
	 */
	struct GrassCounts
	{
		std::vector<std::shared_ptr<org::Buffer>> readback;
		std::vector<std::uint8_t> filled;
		std::atomic<std::uint64_t> frames{ 0 }, live{ 0 }, outside{ 0 }, thinned{ 0 }, faded{ 0 }, tested{ 0 }, phaseOne{ 0 }, retests{ 0 }, phaseTwo{ 0 };
		std::atomic<std::uint64_t> phaseOneDraws{ 0 }, phaseTwoDraws{ 0 }, colourDraws{ 0 }, pbrDraws{ 0 };
	};

	/**
	 * @brief Grass's tables (dclf-architecture.md, "Grass: the draws"; Api::SceneResources' state, render thread but for the handles,
	 * made once at setup): by instance slot the records, their shape slots (owners), their frame values and the visible list (each tier's
	 * entries, then the retest list); shape, bucket and mesh rows; the draw row; the draws buffer (the arguments' arrays and counts, the
	 * family's indirect-argument resource). GrassCullPass and GrassArgsPass fill the list and the arrays in the depth epoch. ready: the
	 * row names its slots.
	 *
	 * Without the programs, or without the tables' device addresses, there is none (setup gives null): grass stays native.
	 */
	struct GrassTables
	{
		Draws::Versioned instances, owners, frame, visible, shapes, buckets, meshes;
		std::array<org::services::FilledVersion, 5> filled;  // instances, owners, shapes, buckets, meshes
		std::shared_ptr<org::Buffer> draw, draws;
		std::uint64_t drawAddress = 0;
		std::shared_ptr<const ComputeProgram> cull, args;
		std::atomic<bool> ready{ false };
		std::shared_ptr<GrassCounts> counts;
		// The depth commits' state: the previous commit's wind timer, whether the counts were zeroed since the tables were made or the
		// family became DCLF's, the bindings' generation (GpuTextures), the buckets waiting for a texture.
		float prevTimeBase = 0.0f;
		bool countsZeroed = false, owned = false;
		std::uint32_t bindingGeneration = ~0u;
		std::vector<std::uint32_t> unbound;
		// The mirror's slots as the tables hold them (the last commit that sent its changes): what a commit the mirror has outgrown
		// draws until the growth is adopted.
		std::uint32_t sentInstanceSlots = 0, sentShapeSlots = 0, sentBucketSlots = 0;
		// Since the last report.
		std::uint64_t uploads = 0, shapesSent = 0, bucketsSent = 0, unboundBuckets = 0, overBuckets = 0, outgrown = 0;
		// The last depth commit's draw row.
		GrassInstances::DrawRow row{};
	};

	// The words the tables post with the revision's inputs (Api::SceneResources::postInputs): what their requirements grow by, the mirror's
	// instance, shape, bucket and mesh slots.
	inline constexpr std::uint32_t kInstanceSlotsInput = 0, kShapeSlotsInput = 1, kBucketSlotsInput = 2, kMeshSlotsInput = 3, kInputWords = 4;

	/** @brief The tables in a_scene (null before the scene's tables are made, or without them). */
	GrassTables* TablesOf(const Draws::SceneBuffers& a_scene);
	/** @brief TablesOf, sharing their ownership (a pass's state). */
	std::shared_ptr<GrassTables> SharedTablesOf(const Draws::SceneBuffers& a_scene);
	/** @brief The tables' registration (RegisterTables). */
	Api::SceneResourcesId TablesId();
}
