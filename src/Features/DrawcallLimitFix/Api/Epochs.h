#pragma once

// The frame's schedule (Schedule/Frame.cpp): each epoch DCLF adds passes to, and its passes' keys (Api/Passes.h) in order.

#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Which extension's resources an epoch's passes are made from: the main view's, the shadow views', a view's (Api::EngineView). */
	enum class EpochShape : std::uint8_t
	{
		Main,
		Shadow,
		View,
	};

	/**
	 * @brief An epoch: its passes' keys, added in this order. A later phase adds its traits, its trigger and the frame and async orders
	 * here.
	 */
	struct EpochDef
	{
		std::string_view id;                       // "main.depth", "main.colour", "forward", "shadow", "occlusion", "reflection", "fp.depth", "fp.colour"
		std::uint32_t segment = 0;                 // RenderGraphRuntime::Segment, as its value
		EpochShape shape = EpochShape::Main;
		std::uint32_t view = 0;                    // View shape: its Api::EngineView, as its value
		std::span<const std::string_view> passes;  // pass keys, in order
	};

	/** @brief The frame's epochs, in the schedule's order (within an epoch, its passes' order is the graph's; across epochs ORG orders them). */
	struct FrameSchedule
	{
		std::span<const EpochDef> epochs;
	};
}
