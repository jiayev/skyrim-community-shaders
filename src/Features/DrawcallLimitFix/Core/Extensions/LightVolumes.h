#pragma once

// The light volumes contract's Core side (Api/LightVolumes.h): the contribution's entry, resolved once at InstallFeatures (none
// registered: no volumes, no landscape rule), and the sink its sample fills: the colour commit's GpuShadowVolume list.

#include "Features/DrawcallLimitFix/Api/LightVolumes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Draws
{
	struct GpuShadowVolume;
}

namespace DCLF::Api
{
	/**
	 * @brief Where a light volumes sample puts its volumes: the colour commit's list (IndirectDraws::Impl::shadowVolumes, cleared before
	 * the sample), a GpuShadowVolume each, in the order put. The list is Core's storage: no allocation per commit once grown.
	 */
	class LightVolumeSink
	{
	public:
		LightVolumeSink(std::vector<Draws::GpuShadowVolume>& a_out, std::vector<LightShadowMask>& a_masks) :
			out(a_out), masks(a_masks) {}

		void Put(const LightVolume& a_volume);
		/** @brief A light's mask (one per light; a second of the same channel is dropped by the latch). */
		void PutMask(const LightShadowMask& a_mask) { masks.push_back(a_mask); }

	private:
		std::vector<Draws::GpuShadowVolume>& out;
		std::vector<LightShadowMask>& masks;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry (registered false: none, every field its default). */
	struct LightVolumesEntry
	{
		bool registered = false;
		std::string name;
		std::uint64_t landscapeFlags = 0;  // per join, data: the properties taking only affectsLand volumes
		decltype(Api::LightVolumesContribution::sample) sample = nullptr;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no name: none). Once. */
	void ResolveLightVolumes(const Api::LightVolumesContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (unregistered before): the contribution's entry. */
	const LightVolumesEntry& LightVolumes();
	/**
	 * @brief Render thread, once per colour commit (CommitShadowInputs): the frame's volumes into a_sink (the contribution's sample);
	 * nothing without one.
	 */
	void SampleLightVolumes(Api::LightVolumeSink& a_sink);
}
