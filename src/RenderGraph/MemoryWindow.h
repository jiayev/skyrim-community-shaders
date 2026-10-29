#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <cstddef>
#	include <string>

/**
 * @brief The VRAM attribution window (ORG's DebugUI/MemoryIntrospectionWidget.h over Vram/VramLedger.h: DXVK's resources and the
 * render graph's, by owner, reconciled against the driver's heap usage), and the render graph's own report line.
 *
 * The report line reads only the graph's memory snapshot provider (ORG's tracking world: every ORG buffer and texture, with the size
 * of its allocation and its name; PersistentGraphHost installs it).
 */
namespace RenderGraphMemory
{
	/** @brief Opens the window. UI thread. */
	void OpenWindow();
	/** @brief Draws the window while it is open (Menu::DrawRenderGraphWindows). UI thread, inside an ImGui frame. */
	void DrawWindow();

	/**
	 * @brief One line (no newline) of the tracked memory: its device-local total (host memory's apart: upload and readback heaps), its families' (a resource name's family: up to its last '.' and the
	 * first '-' after it, digits dropped, so "cs.dclf.grass-instances" is "cs.dclf.grass" and "cs.dclf.shadow.sequences7"
	 * "cs.dclf.shadow.sequences") largest a_families, and the largest a_names by name (a name's versions summed). Empty without a graph.
	 * Any thread (the snapshot takes ORG's tracking world's ownership while it reads).
	 */
	std::string ReportLine(std::size_t a_families, std::size_t a_names);
}

#endif
