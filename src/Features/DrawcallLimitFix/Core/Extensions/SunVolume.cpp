// The sun volume's entry and sinks (see the header).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"

#include <cstring>

namespace DCLF::Api
{
	void SunCascadeSink::Put(const ShadowCameras::Camera& a_cascade)
	{
		auto& cascade = out.emplace_back();
		for (std::uint32_t p = 0; p < 6; ++p) {
			std::memcpy(cascade.planes[p], a_cascade.cull.plane[p].data(), sizeof(float) * 4);
			std::memcpy(cascade.planes[6 + p], a_cascade.caster.plane[p].data(), sizeof(float) * 4);
		}
		cascade.masks[0] = a_cascade.cull.mask;
		cascade.masks[1] = a_cascade.caster.mask;
	}

	void SunEntryPlanesSink::Put(const ShadowCameras::Planes& a_planes)
	{
		auto& process = out.emplace_back();
		for (std::uint32_t p = 0; p < 6; ++p)
			for (std::uint32_t i = 0; i < 4; ++i)
				process.planes[p][i] = a_planes.plane[p][i];
		process.mask = a_planes.mask;
	}
}

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveSunVolume (InstallFeatures, before DCLF's threads exist), read-only after.
		SunVolumeEntry resolved;
	}

	void ResolveSunVolume(const Api::SunVolumeContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the sun volume was resolved twice");
		if (a_registered.name.empty())
			return;
		resolved = { true, std::string(a_registered.name), a_registered.cascades, a_registered.entryPlanes };
	}

	const SunVolumeEntry& SunVolume() { return resolved; }

	void SampleSunCascades(Api::SunCascadeSink& a_sink)
	{
		if (resolved.cascades)
			resolved.cascades(a_sink);
	}

	bool SampleSunEntryPlanes(Api::SunEntryPlanesSink& a_sink, std::uint32_t a_frame)
	{
		return resolved.entryPlanes && resolved.entryPlanes(a_sink, a_frame);
	}
}
