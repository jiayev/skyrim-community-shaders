#pragma once

// The sun volume contract (Api::InstallContext::RegisterSunVolume), the light volumes' sibling (Api/LightVolumes.h): the sun's cull
// volumes as DCLF's own views have them this frame, where Core's work reads the sun's cameras: every colour commit's cascades (BuildDraws'
// sun test, InSunCascades: the latch's sun region) and the shadow inputs' full-frustum planes (the sun's entry rule, kCullSunEntry:
// ShadowInputs::sunEntryProcesses). The test, the latch region and its clamp, and the inputs are Core's (they mirror BuildDrawsCS); the
// views are the feature's (Features/Shadows/Sun: the Sun board its UpdateCamera hook writes). One contribution: a second fails the
// install. Without one no cascade is latched (every pass with the sun's bits then draws unshadowed) and the entry rule has no planes, as
// on a frame the sun's UpdateCamera did not run.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/SunVolume.h), which the features inside DCLF include for them.
	class SunCascadeSink;
	class SunEntryPlanesSink;

	/**
	 * @brief The sun volume contribution.
	 *
	 * - name: the contribution's (the install's failures).
	 * - cascades: render thread, once per colour commit (CommitShadowInputs), after the sun's UpdateCamera: this frame's cascades into
	 *   a_sink, in descriptor order (none when the sun did not set its views up this frame). Core keeps the list unclamped and latches at
	 *   most the revision's layout's cascades.
	 * - entryPlanes: render thread, where the shadow inputs are prepared (the shadow epoch's, and the frame start's for the build ahead):
	 *   the planes each process of the full-frustum cull tests, by process, when they are scene frame a_frame's or the one before (the
	 *   build ahead's, posted before this frame's UpdateCamera); whether it put them (false: none, nothing put).
	 */
	struct SunVolumeContribution
	{
		std::string_view name;
		void (*cascades)(SunCascadeSink& a_sink) = nullptr;
		bool (*entryPlanes)(SunEntryPlanesSink& a_sink, std::uint32_t a_frame) = nullptr;
	};
}
