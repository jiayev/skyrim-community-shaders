#pragma once

// The object contributor contracts' Core side (Api/ObjectContributors.h): the technique contributions by technique (the Lighting port's
// eligibility and the joins' record flags read them as data), what a shading contribution is handed, and the calls the Lighting port's
// shading sample and the frame globals' capture make.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <array>
#include <cstdint>

namespace RE
{
	class BSShaderProperty;
}

namespace DCLF
{
	struct ObjectTreeAnim;
}

namespace DCLF::Api
{
	/**
	 * @brief A shading contribution's derive: the property sampled, and the shading row's per-object PerGeometry pair it writes
	 * (BindlessShading's TreeParams and WindTimers; the sample keeps xy of the timers, zero-filled before).
	 */
	class ShadingDerivation
	{
	public:
		const RE::BSShaderProperty& property;
		ObjectTreeAnim& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The Lighting techniques a contribution may name (SIE::ShaderCache::LightingShaderTechniques' values below it). */
	inline constexpr std::uint32_t kTechniqueSlots = 32;
	/** @brief The floats the frame globals hold for the features' contributions (FrameGlobals::features): grow it before more are registered. */
	inline constexpr std::uint32_t kFrameGlobalsFloats = 8;

	/** @brief A technique's contribution (none: no toggle, no flags). */
	struct TechniqueEntry
	{
		Api::ToggleId toggle;
		std::uint32_t objectFlags = 0;
	};
	using TechniqueEntries = std::array<TechniqueEntry, kTechniqueSlots>;

	/** @brief Any thread, after InstallFeatures (all none before): the technique contributions, by technique. */
	const TechniqueEntries& RegisteredTechniques();

	/** @brief The record flags a_technique's objects carry (its contribution's; 0 for none). */
	inline std::uint32_t TechniqueObjectFlags(const TechniqueEntries& a_techniques, std::uint32_t a_technique)
	{
		return a_technique < a_techniques.size() ? a_techniques[a_technique].objectFlags : 0u;
	}

	/**
	 * @brief The shading sample (SampleShading): every shading contribution whose property flags a_property has, in registration order,
	 * deriving a_out (left as it is by none).
	 */
	void DeriveShading(const RE::BSShaderProperty& a_property, ObjectTreeAnim& a_out);
	/** @brief Render thread, FrameGlobals::Capture: every frame-globals contribution's floats into a_out (kFrameGlobalsFloats, zeroed). */
	void CaptureFrameGlobals(std::array<float, kFrameGlobalsFloats>& a_out);
}
