#pragma once

namespace FrameAnnotations
{
	void OnPostPostLoad();
	void OnDataLoaded();

	/**
	 * @brief Whether the per-draw geometry events (BSShader SetupGeometry opens, RestoreGeometry closes) are on.
	 *
	 * Their hooks exist only if Frame Annotations was on at startup. Off while the GPU idle trace runs: one
	 * formatted event per draw would dominate the render thread's timeline being measured. A caller that runs
	 * SetupGeometry without RestoreGeometry closes the event itself when this is true.
	 */
	bool GeometryEventsEnabled();
}
