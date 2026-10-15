#pragma once

// Tree wind (drawcall-limit-fix.md, "Tree wind on the GPU"): the GPU animation of the trees DCLF owns. The scene's tables list the
// trees (Records.h, TreeStatic: SceneTables' tree family); the feature takes their nodes at the frame's start (a frame-value pool, the
// seeds), clocks them on the GPU (TreeWindCS.hlsl: a stage in the depth epoch's next-frame state, its scene resources, its commit's
// frame row) into the wind rows the draws read at t124 (a frame binding), and stands the engine's tree manager down for the trees DCLF
// stands in for (TreeAnimation, TreeList). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"  // TreeStatic, the texture registers

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DCLF::Features::TreeWind
{
	// AE 1.6.1170 (module offsets) of the engine's tree manager this feature alone reads.
	namespace Address
	{
		inline constexpr std::uintptr_t kTreeManager = 0x20f6a18;  // BSTreeManager*
		inline constexpr std::uintptr_t kTreeFirstMultiBound = 0x147d640;  // FUN_14147d640(tree node): its object (+0x118), its first child's AsMultiBoundNode (vfunc 0x30), or null
	}

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
	// TreeWindSeeds.cpp: the seeds' frame-value pool.
	void RegisterSeeds(Api::InstallContext& a_context);
	/** @brief Render thread: the seeds' backing the current frame reads (0 before the first frame): the frame row's treeSeedsIndex. */
	std::uint32_t SeedsIndex();
	/** @brief Render thread (diagnostics): the seed rows as the frame's producer left them, once it is done (a_wait: waits); null before. */
	const std::vector<TreeStatic>* SeedsIfDone(bool a_wait);
	// TreeWindScene.cpp: the scene resources (tree rows, clocks, frame row, wind rows), the commit hook and the frame binding.
	void RegisterScene(Api::InstallContext& a_context);
	// TreeWindPass.cpp: the GPU stage.
	void RegisterStage(Api::InstallContext& a_context);
	// TreeAnimation.cpp: the tree manager stood down for the stood-in trees (the stand-in observer, its engine hooks with TreeList's).
	void RegisterAnimation(Api::InstallContext& a_context);
	// Diagnostics/TreeWindReport.cpp, Diagnostics/TreeWindParity.cpp: the report line and the parity's observers.
	void RegisterReport(Api::InstallContext& a_context);
	void SubscribeParity();
}
