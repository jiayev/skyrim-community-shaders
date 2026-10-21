#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <Render/RenderGraph/ScheduleTrace.h>

#	include <cstdint>
#	include <memory>

/**
 * @brief The render graph's persistent schedule, explained (docs/development/render-graph-schedule.md): the menu's "Render graph
 * schedule" window (ORG's DebugUI/ScheduleInspector.h) and the DevBench tool communityshaders.schedule.
 *
 * The graph traces its schedule (RenderGraph::SetScheduleTraceEnabled) while either wants it: the window while open, the tool from
 * its enable until its disable.
 */
namespace RenderGraphSchedule
{
	enum class Requester : std::uint8_t
	{
		Window,
		DevBench,
	};

	/** @brief Whether a_requester wants the schedule traced; the graph traces while any does. Any thread. */
	void SetTracing(Requester a_requester, bool a_enabled);
	/** @brief Whether the graph traces its schedule now (false without a graph). Any thread. */
	bool TracingEnabled();

	/** @brief The schedules the graph executes now; null without a graph, while tracing is off, or before a traced frame. Any thread. */
	std::shared_ptr<const org::PersistentScheduleSnapshot> Snapshot();

	/** @brief Opens the window (it starts tracing). UI thread. */
	void OpenWindow();
	/** @brief Draws the window while it is open (Menu::DrawSettings). UI thread, inside an ImGui frame. */
	void DrawWindow();
}

#endif
