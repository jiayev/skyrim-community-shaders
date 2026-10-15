#pragma once

// The first-person view's own frame state (FirstPersonFrame): the latch, the skeleton's slots, the camera and the report's counters, beside
// what Core keeps of the view's slot (IndirectDraws::ViewState: its targets, resources, coverage, depth range). Render thread, and the
// report. Included by the feature's view and epoch sources.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"

namespace DCLF::Features::FirstPerson
{
	inline constexpr Api::EngineView kView = Api::EngineView::FirstPerson;

	/** @brief The view's frame state: its colour targets are kMAIN's format, the motion vectors' (colour2) and kPOST_ZPREPASS_COPY's (Core's). */
	struct FirstPersonFrame
	{
		std::uint32_t slots = 0, pipelinesReady = 0;
		// The slots the skeleton's geometry draws with (BeginFrame's walk), and the frame they were found.
		std::vector<std::uint8_t> slotsWanted;
		std::uint32_t slotsFrame = 0;
		// Main::Draw's cVar23 recomputed, its frame.
		bool latch = false;
		std::uint32_t latchFrame = ~0u;
		// The first-person camera as the engine set it up (FUN_1414cb020), in world space; last frame's for the motion vectors.
		std::array<std::byte, kViewBlockBytes> perFrame{};
		std::array<float, 16> viewProj{};
		RE::NiPoint3 eye;
		PreviousCamera previous;
		bool havePrevious = false;
		// The frame whose depth epoch built the sequences the colour epoch draws.
		std::uint32_t depthFrame = ~0u;
		// Since the last report.
		std::uint64_t frames = 0, latched = 0, depthEpochs = 0, colourEpochs = 0;
		std::array<std::uint64_t, 7> skipped{};  // not drawn (latch, claims), not covered, no camera, stale inputs, no resources, failed, no depth epoch
		// The installed tables' forward members, summed since the last report.
		std::uint64_t forwardMembers = 0;
		std::uint32_t forwardMembersNow = 0;  // the last walk's (every 16 frames)
	};

	/** @brief The view's frame state (render thread; the report reads and resets its counters). */
	inline FirstPersonFrame& FrameState()
	{
		static FirstPersonFrame frame;
		return frame;
	}
}
