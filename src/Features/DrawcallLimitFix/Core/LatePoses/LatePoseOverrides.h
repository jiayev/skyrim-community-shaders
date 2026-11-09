#pragma once

// The late poses (Api/LatePoses.h): parts of a scene graph a plugin hides after the shadow maps by shrinking nodes. This module finds
// those nodes and turns them into rows for a second copy of the frame's placements and palettes. FrameValues fills that copy in the
// frame's own producer and the epochs from the depth commit on read it. Nothing here waits, and nothing reads the engine off the
// render thread.
//
// Kept apart from the rest of DCLF on purpose: it stands in for an API that sets view masks on geometry, and goes when that comes.
//
// How a frame goes:
// - Depth commit, render thread (ConfirmFrame): each contributed root is searched for shrunk nodes: a world scale far below the
//   parent's. That search is kept for the next frame. It also confirms the frame's plan: the plan is used only when every node it
//   shrinks is shrunk now.
// - Next frame's start, render thread (PrepareFrame): the kept search becomes a plan of plain data. Each hidden node gives a scale about
//   its own world position, by the share of its world scale now that it was hidden to, taken from the full pose. Then the skin bones
//   and rigid geometry under it. A node as small now (shrunk for good) shrinks nothing.
// - The frame's producer (BuildRows): the plan applied to the rows it sampled at the frame's start.
//
// A hide that starts shows one frame late: the plan comes from the frame before. One that stops does not: the frame's check refuses
// the plan.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::LatePoses
{
	/** @brief A frame's late pose, plain data (no engine reference): what the producer applies to its rows. */
	struct FramePlan
	{
		/** @brief A hidden node's shrink in world space: x' = center + factor * (x - center). */
		struct Scale
		{
			float center[3]{};
			float factor = 1.0f;
		};
		/** @brief A skin with bones under a hidden node: per bone, its scale's index + 1 (0: not hidden). */
		struct Skin
		{
			const void* geometry = nullptr;  // a key (the producer's kept movers'), never dereferenced
			std::uint32_t boneRows = 0;      // three a bone, as the palette block's
			std::vector<std::uint16_t> scaleOf;
		};
		/** @brief A geometry without a skin under a hidden node: its placement row shrunk whole. */
		struct Rigid
		{
			const void* geometry = nullptr;
			std::uint16_t scale = 0;
		};
		std::vector<Scale> scales;
		std::vector<Skin> skins;
		std::vector<Rigid> rigid;
	};

	/** @brief Where the producer keeps a geometry's rows (its kept mover's plan item). */
	struct Location
	{
		std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
	};

	/** @brief The late copies' rows that differ from the frame's: whole placement rows and whole palette blocks. */
	struct Rows
	{
		std::vector<BindlessPlacement> placements;
		std::vector<std::uint32_t> placementSlots;  // one per row of placements
		std::vector<FrameValues::PaletteRow> palettes;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> paletteRuns;  // (first row in the frame's palettes, rows), in palettes' order
		void Clear()
		{
			placements.clear();
			placementSlots.clear();
			palettes.clear();
			paletteRuns.clear();
		}
	};

	/**
	 * @brief Render thread, at the frame's start, before FrameValues::Kick: the frame's plan, from the last frame's search (null: no root
	 * named, no hidden node found, or the search is not the last frame's).
	 */
	std::shared_ptr<const FramePlan> PrepareFrame();

	/**
	 * @brief Render thread, at the depth commit, before the depth epoch is submitted: searches the roots (kept for the next frame's plan),
	 * and returns whether the frame's plan holds now: it has one, and every node it shrinks is shrunk now.
	 */
	bool ConfirmFrame();

	/**
	 * @brief Render thread, before the Present (diagnostics): on a frame whose depth commit found no hidden node under a named root,
	 * searches again and counts the frames where it finds one now (the plugin hid them after the depth commit, which the plan cannot
	 * follow).
	 */
	void CheckAtPresent();

	/**
	 * @brief The producer: a_plan applied to the frame's rows (a_rows, a_palettes), into a_out (cleared first). a_find gives a geometry's
	 * rows (false: not kept, or not a plain skin or placement). A skin whose palette block is not the plan's size is left out.
	 */
	void BuildRows(const FramePlan& a_plan, const std::function<bool(const void*, Location&)>& a_find, const std::vector<BindlessPlacement>& a_rows,
		const std::vector<FrameValues::PaletteRow>& a_palettes, Rows& a_out);

	/** @brief The report's text since the last call (empty when no root was named): plans, confirmations, the rows overridden. */
	std::string TakeReport();
}
