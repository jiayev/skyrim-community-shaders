#pragma once

// Tree wind (dclf-architecture.md, "Tree wind on the GPU"): the GPU animation of the trees DCLF owns. The feature lists the trees
// its members draw under in the scene's tables (TreeStatic: its table family, written by its coordinator component as members join and
// leave: TreeWindTables), ports their technique (technique 12: its eligibility, record flag, shading derivation and frame globals:
// TreeWindObjects), takes their nodes at the frame's start (a frame-value pool, the seeds), clocks them on the GPU (TreeWindCS.hlsl: a
// pass in the depth epoch's next-frame state, its scene resources, its commit's frame row) into the wind rows the draws read at t124 (a
// frame binding), and stands the engine's tree manager down for the trees DCLF stands in for (TreeAnimation, TreeList). Included by the
// feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"  // the texture registers, the record flags

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DCLF::Features::TreeWind
{
	// AE 1.6.1170 and SE 1.5.97 (module offsets) of the engine's tree manager and globals this feature alone reads.
	namespace Address
	{
		inline constexpr Engine::Offset kTreeManager{ 0x20f6a18, 0x1ebeb38 };  // BSTreeManager*
		inline constexpr Engine::Offset kTreeFirstMultiBound{ 0x147d640, 0x1293440 };  // FUN_14147d640(tree node): its object (+0x118), its first child's AsMultiBoundNode (vfunc 0x30), or null
		inline constexpr Engine::Offset kTreeWindFadeStart{ 0x2033100, 0x1e0df70 };  // float: the tree wind fade start
		inline constexpr Engine::Offset kTreeWindFadeEnd{ 0x2033104, 0x1e0df74 };  // float: the tree wind fade end
		inline constexpr Engine::Offset kTreeWindTimerScale{ 0x1ad28bc, 0x164dd68 };  // float: the tree wind timer scale
		inline constexpr Engine::Offset kFrameSeconds{ 0x31cc288, 0x2f6b948 };  // float: the frame's seconds (GetSecondsPassed)
	}

	// The Lighting technique the feature ports (SIE::ShaderCache::LightingShaderTechniques::TreeAnim), and the record flag its objects
	// carry (Api::TechniqueContribution; Records.h, kFeatureObjectFlags): their TreeParams and WindTimers are per object, not the
	// pipeline template's, which a zeroed per-object copy would clobber.
	inline constexpr std::uint32_t kTechniqueTreeAnim = 12;
	inline constexpr std::uint32_t kObjectTreeAnim = 1u << 6;

	/**
	 * @brief A tree's static row: what tree wind (TreeWindCS.hlsl; dclf-architecture.md, "Tree wind on the GPU") clocks a listed
	 * BSTreeNode from. The scene's tables list the trees (the feature's table family, its component's ListTree); the feature owns the
	 * rest of the wind (its clock rows, seeds, pass and wind rows).
	 *
	 * The engine's tree manager (FUN_1404381e0, from Main::Update) advances a BSTreeNode's timer (+0x164) by the frame's
	 * seconds, measures its squared distance to the camera (+0x158), and within its range (manager +0x80) takes the gust
	 * amplitude (+0x15C) from the timer: 0.25 * sum over k in {1,3,5,7} of sin(k * pi * manager+0x78 * timer), times the
	 * model's scale (model +0xB0). SetupGeometry reads them and keeps the timer as the previous one (+0x168). For members
	 * TreeWindCS does it all: per node a TreeStatic row (CPU, when listed) and a clock row (GPU, reseeded on a new
	 * generation); per member a TreeObject, whose record's TreeParams and WindTimers the pass writes.
	 */
	struct TreeStatic
	{
		float position[3]{};       // the node's world translate (+0xA0)
		float leafFrequency = 1.0f;  // +0x160
		float modelAmplitude = 0.0f; // the model's +0xB0
		float timer = 0.0f;          // the node's clock when listed: +0x164, +0x168, +0x15C
		float previousTimer = 0.0f;
		float amplitude = 1.0f;
		std::uint32_t generation = 0;  // new for every listing: the clock row takes the values above again
		std::uint32_t animated = 0;    // a model is attached (the manager advances only such a node)
		// Which of the slot's two seed rows (FrameValues' tree seeds, 2 * slot + this) holds its generation's: the values above
		// are the seed's (the node at frame start); the coordinator's row holds the generation and this.
		std::uint32_t seedOdd = 0;
		std::uint32_t padding = 0;
	};
	static_assert(sizeof(TreeStatic) == 48);

	// A member drawing under a tree node: its object slot and the node's tree slot (kNodelessTree: the engine's defaults).
	struct TreeObject
	{
		std::uint32_t object, tree;
	};
	// An object's tree slot (its reference in the trees' family: Extensions::TableFamilyColumns::objectRow): none, or a member whose
	// property names no listed tree node (the record's and the nodeless wind entry's defaults).
	inline constexpr std::uint32_t kNoTree = ~0u;
	inline constexpr std::uint32_t kNodelessTree = ~0u - 1;
	// The record word the tree slot is carried in, its generation in the next (DCLFObjectRecord's DCLFWindTimers.zw: DCLFTreeWind).
	inline constexpr std::uint32_t kTreeRecordWord = 10;

	// The feature block the shading contribution claims (Api::ShadingContribution::blockWords; checked by Features/Manifest.cpp): its first
	// word in the record (BindlessObject::featureBlock: DCLFObjectRecord's DCLFTreeParams, kTreeRecordWord inside) and in the shading row
	// (BindlessShading::featureBlock: DCLFShadingRow's DCLFShadingTreeParams). TreeParams in words 0-3, WindTimers in 4-7 (the row keeps
	// xy: kBlockWords; zw the record's tree slot and generation).
	inline constexpr std::uint32_t kRecordBlockWord = 4;
	inline constexpr std::uint32_t kShadingBlockWord = 16;
	inline constexpr std::uint32_t kBlockTreeParams = 0;
	inline constexpr std::uint32_t kBlockWindTimers = 4;
	inline constexpr std::uint32_t kBlockWords = 6;
	// Their Lighting VS variables (ShaderConstants::LightingVS: Core/Shading/LightingConstants.h's kVSTreeParams, kVSWindTimers).
	inline constexpr std::uint32_t kTreeParamsVariable = 4;
	inline constexpr std::uint32_t kWindTimersVariable = 5;

	/** @brief t124: the trees' wind (DCLFTreeWind in Common/DCLFObjects.hlsli; TreeWindCS), vertex stage. It alternates by frame. */
	inline constexpr std::uint32_t kWindRegister = kTextureRegisters - 4;

	constexpr const char* kShader = "DrawcallLimitFix/Features/TreeWind/TreeWindCS.hlsl";
	// TreeWindCS.hlsl's constants: buffer indices only; per-frame values are the frame row every commit uploads.
	struct TreeWindConstants
	{
		std::uint32_t treesIndex = 0, clocksIndex = 0, frameIndex = 0, padding0 = 0;
		std::uint32_t windIndices[2]{};
		std::uint32_t padding1[2]{};
	};
	// The wind buffers' rows per entry (TreeWindCS.hlsl): TreeParams, WindTimers, the listing's generation.
	constexpr std::uint32_t kTreeWindEntryRows = 3;
	static_assert(sizeof(TreeWindConstants) == 32);
	constexpr std::uint32_t kTreeWindConstantWords = sizeof(TreeWindConstants) / 4;
	constexpr std::uint32_t kTreeWindGroup = 64;
	// The tree buffers' first capacity (doubled as needed).
	constexpr std::uint32_t kInitialTrees = 1024;

	/** @brief A tree's clock on the GPU (TreeWindCS.hlsl's TreeClock; must match): per tree slot, reseeded on a new generation. */
	struct TreeClock
	{
		float timer, previousTimer, amplitude;
		std::uint32_t generation, frame;
		std::uint32_t padding[3];
	};
	static_assert(sizeof(TreeClock) == 32);

	/** @brief The frame's inputs to the tree clocks (TreeWindCS.hlsl's constants after its indices). */
	struct TreeWindFrame
	{
		float deltaTime = 0.0f;  // the frame's seconds (GetSecondsPassed's global)
		float camera[3]{};       // the main camera's world position
		float windSpeed = 0.0f;  // the tree manager's +0x78
		float maxDistance2 = 0.0f;  // its +0x80
		float windMagnitude = 0.0f;
		float fadeStart = 0.0f, fadeEnd = 0.0f;
		float timerScale = 0.0f;
	};

	// TreeWindCS.hlsl's frame row (StructuredBuffer, one row).
	struct TreeWindFrameRow
	{
		std::uint32_t treeCount = 0, objectCount = 0, frame = 0;
		std::uint32_t treeSeedsIndex = 0;  // the seeds pool's backing the frame reads (SeedsIndex)
		TreeWindFrame inputs{};
		std::uint32_t padding2[2]{};
	};
	static_assert(sizeof(TreeWindFrameRow) == 64);

	/** @brief The row TreeWindCS takes a tree from: its seed, if one of the row's generation is there (must match TreeWindCS.hlsl). */
	inline bool MergeTreeSeed(const TreeStatic& a_row, const TreeStatic* a_seeds, std::size_t a_seedCount, std::uint32_t a_slot, TreeStatic& a_out)
	{
		const std::size_t at = 2ull * a_slot + (a_row.seedOdd & 1u);
		if (!a_seeds || at >= a_seedCount || a_seeds[at].generation != a_row.generation)
			return false;
		TreeStatic merged = a_seeds[at];  // a_out may be a_row
		merged.seedOdd = a_row.seedOdd;
		a_out = merged;
		return true;
	}

	// TreeWindFeature.cpp: the feature's entry (Features/FeatureList.cpp), and its switch (CS_DCLF_TREE_LIST_ALL).
	Api::Feature Feature();
	bool ListAll();
	// TreeWindTables.cpp: the trees' table family and the coordinator component that lists them.
	void RegisterTables(Api::InstallContext& a_context);
	/** @brief The component's registration: the seeds its listings request (FrameView::TakeComponentSeeds). */
	Api::CoordinatorComponentId ComponentId();
	// TreeWindObjects.cpp: technique 12's eligibility (a_trees: the trees toggle) and record flag, its shading derivation and frame globals.
	void RegisterObjects(Api::InstallContext& a_context, Api::ToggleId a_trees);
	// TreeWindSeeds.cpp: the seeds' frame-value pool.
	void RegisterSeeds(Api::InstallContext& a_context);
	/** @brief Render thread: the seeds' backing the current frame reads (0 before the first frame): the frame row's treeSeedsIndex. */
	std::uint32_t SeedsIndex();
	/** @brief Render thread (diagnostics): the seed rows as the frame's producer left them, once it is done (a_wait: waits); null before. */
	const std::vector<TreeStatic>* SeedsIfDone(bool a_wait);
	// TreeWindScene.cpp: the scene resources (tree rows, clocks, frame row, wind rows), the commit hook and the frame binding.
	void RegisterScene(Api::InstallContext& a_context);
	// TreeWindPass.cpp: the pass (tree-wind.next-frame).
	void RegisterPass(Api::InstallContext& a_context);
	// TreeAnimation.cpp: the tree manager stood down for the stood-in trees (the stand-in observer, its engine hooks with TreeList's).
	void RegisterAnimation(Api::InstallContext& a_context);
	// Diagnostics/TreeWindReport.cpp, Diagnostics/TreeWindParity.cpp: the report line and the parity's observers.
	void RegisterReport(Api::InstallContext& a_context);
	void SubscribeParity();
}
