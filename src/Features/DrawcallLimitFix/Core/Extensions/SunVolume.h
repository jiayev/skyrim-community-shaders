#pragma once

// The sun volume contract's Core side (Api/SunVolume.h): the contribution's entry, resolved once at InstallFeatures (none registered:
// no cascades, no entry planes), and the sinks its callbacks fill: the colour commit's GpuCascade list and the shadow inputs' sun entry
// processes, each written from a camera of Core's camera math (ShadowCameras) exactly as the latch and the inputs read it.

#include "Features/DrawcallLimitFix/Api/SunVolume.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Draws
{
	struct GpuCascade;
	struct SunEntryProcess;
}

namespace DCLF::ShadowCameras
{
	struct Camera;
	struct Planes;
}

namespace DCLF::Api
{
	/**
	 * @brief Where the cascades go: the colour commit's list (IndirectDraws::Impl::sunCascades, cleared before the sample), a GpuCascade
	 * each, in the order put: the planes its cull tests (0-5) and its caster volume (6-11), with their masks. Core's storage: no
	 * allocation per commit once grown.
	 */
	class SunCascadeSink
	{
	public:
		explicit SunCascadeSink(std::vector<Draws::GpuCascade>& a_out) :
			out(a_out) {}

		void Put(const ShadowCameras::Camera& a_cascade);

	private:
		std::vector<Draws::GpuCascade>& out;
	};

	/** @brief Where the full-frustum planes go: the shadow inputs' (ShadowInputs::sunEntryProcesses), a SunEntryProcess each, in order. */
	class SunEntryPlanesSink
	{
	public:
		explicit SunEntryPlanesSink(std::vector<Draws::SunEntryProcess>& a_out) :
			out(a_out) {}

		void Put(const ShadowCameras::Planes& a_planes);

	private:
		std::vector<Draws::SunEntryProcess>& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry (registered false: none, every field its default). */
	struct SunVolumeEntry
	{
		bool registered = false;
		std::string name;
		decltype(Api::SunVolumeContribution::cascades) cascades = nullptr;
		decltype(Api::SunVolumeContribution::entryPlanes) entryPlanes = nullptr;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no name: none). Once. */
	void ResolveSunVolume(const Api::SunVolumeContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (unregistered before): the contribution's entry. */
	const SunVolumeEntry& SunVolume();
	/** @brief Render thread, once per colour commit (CommitShadowInputs): this frame's cascades into a_sink; nothing without one. */
	void SampleSunCascades(Api::SunCascadeSink& a_sink);
	/**
	 * @brief Render thread, where the shadow inputs are prepared: the full-frustum planes of scene frame a_frame or the one before into
	 * a_sink; whether there were (false without one).
	 */
	bool SampleSunEntryPlanes(Api::SunEntryPlanesSink& a_sink, std::uint32_t a_frame);
}
