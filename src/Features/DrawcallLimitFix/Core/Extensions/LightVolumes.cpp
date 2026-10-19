// The light volumes' entry and sink (see the header).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"

#include <cstring>

namespace DCLF::Api
{
	void LightVolumeSink::Put(const LightVolume& a_volume)
	{
		auto& volume = out.emplace_back();
		volume.masks[0] = a_volume.masks[0];
		volume.masks[1] = a_volume.masks[1];
		volume.maskBit = a_volume.maskBit;
		volume.affectsLand = a_volume.affectsLand ? 1u : 0u;
		std::memcpy(volume.sphere, a_volume.sphere, sizeof(volume.sphere));
		std::memcpy(volume.planes, a_volume.planes, sizeof(volume.planes));
		volume.portalContext = a_volume.portalContext;
		volume.padding[0] = volume.padding[1] = volume.padding[2] = 0;
	}
}

namespace DCLF::Extensions
{
	static_assert(sizeof(Api::LightVolume::sphere) == sizeof(Draws::GpuShadowVolume::sphere) &&
				  sizeof(Api::LightVolume::planes) == sizeof(Draws::GpuShadowVolume::planes));

	namespace
	{
		// Written only by ResolveLightVolumes (InstallFeatures, before DCLF's threads exist), read-only after.
		LightVolumesEntry resolved;
	}

	void ResolveLightVolumes(const Api::LightVolumesContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the light volumes were resolved twice");
		if (a_registered.name.empty())
			return;
		resolved = { true, std::string(a_registered.name), a_registered.landscapeFlags, a_registered.sample };
	}

	const LightVolumesEntry& LightVolumes() { return resolved; }

	void SampleLightVolumes(Api::LightVolumeSink& a_sink)
	{
		if (resolved.sample)
			resolved.sample(a_sink);
	}
}
