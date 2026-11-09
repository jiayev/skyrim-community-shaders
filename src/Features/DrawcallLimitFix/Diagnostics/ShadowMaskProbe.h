#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	struct GpuShadowVolume;
}

namespace DCLF::Diagnostics
{
	/**
	 * @brief The actor probe's shadow lights (dclf:actor-probe): for the objects it names, the next CPU colour cull (Cull.cpp's
	 * ObjectWordOf) logs each local shadow light's verdict - taken, or which test dropped it - with the object's bound. A shadow light
	 * missing from an object's mask is one Light Limit Fix ignores for its draw (IsLightIgnored), so the object goes unlit by it.
	 */
	namespace ShadowMaskProbe
	{
		/** @brief Scene lane: the objects (slot, description) to report on the next colour cull that meets them. */
		void Arm(std::vector<std::pair<std::uint32_t, std::string>> a_objects);
		/** @brief Any thread: an armed probe is waiting (a relaxed read, for the cull's hot loop). */
		bool Armed();
		/**
		 * @brief A cull's thread: a_object's mask was decided; logs it once if the probe names it. a_rejects: per volume, 0 taken, 1 the
		 * sphere, 2 a plane, 3 the strict gate, 4 landscape, 5 its bit already taken.
		 */
		void Note(std::uint32_t a_object, const float a_bound[4], float a_margin, std::uint32_t a_mask, const Draws::GpuShadowVolume* a_volumes, std::uint32_t a_count,
			const std::uint8_t* a_rejects);
	}
}
