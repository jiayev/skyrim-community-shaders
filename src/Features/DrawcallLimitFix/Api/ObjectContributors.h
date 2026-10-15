#pragma once

// The object contributor contracts (Api::InstallContext::RegisterTechnique, RegisterShading, RegisterFrameGlobals): what a feature adds
// to the objects DCLF draws of an engine technique it ports - their eligibility and record flags, the per-object values the Lighting
// port derives from their properties, and the engine globals that derivation reads, captured with the frame's. The records' layouts
// stay compile-time: a feature declares its flag bits in its own header (Features/Manifest.cpp checks them against Core's and each
// other's), and its record words with its table family (Api/TableFamilies.h).

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/ObjectContributors.h), which the features inside DCLF include for it.
	class ShadingDerivation;

	/**
	 * @brief A technique contribution: an engine Lighting technique whose objects the feature ports. Read where the objects are classified
	 * and joined, as data (no call per object).
	 *
	 * - technique: the Lighting technique (SIE::ShaderCache::LightingShaderTechniques; below Extensions::kTechniqueSlots), one feature's.
	 * - toggle: a registered toggle; the technique's objects are DCLF's while it is on (the Lighting port's eligibility).
	 * - objectFlags: the record flags its objects carry (the joins' static flags), the feature's own bits (Records.h, kFeatureObjectFlags).
	 */
	struct TechniqueContribution
	{
		std::uint32_t technique = ~0u;
		ToggleId toggle;
		std::uint32_t objectFlags = 0;
	};

	/**
	 * @brief A shading contribution: the per-object values SetupGeometry derives for a property of the feature's (the shading rows' tree
	 * parameters and wind timers), what a member draws with until the feature's GPU stage has its own.
	 *
	 * - derive: FrameValues' pool (a shading sample: the slots shading events named, inside the engine-read window and the frame's
	 *   FrameGlobals scope) or a parity's sample, for a property with any of propertyFlags: writes ShadingDerivation::out. Called per
	 *   sampled property that has the flags, never per object per frame.
	 */
	struct ShadingContribution
	{
		std::string_view name;
		std::uint64_t propertyFlags = 0;  // BSShaderProperty::EShaderPropertyFlag bits
		void (*derive)(const ShadingDerivation& a_derivation) = nullptr;
	};

	/** @brief A registered frame-globals contribution (RegisterFrameGlobals' result): its first float in FrameGlobals::features. */
	struct FrameGlobalsSlot
	{
		std::uint32_t first = ~0u;
		explicit operator bool() const { return first != ~0u; }
	};

	/**
	 * @brief A frame-globals contribution: engine globals a feature's derivations read, captured with the frame's (FrameGlobals), so the
	 * scene work and FrameValues' pool read them from the capture their frame carries, never from the engine.
	 *
	 * - capture: the render thread at the frame's start (FrameGlobals::Capture, the engine's update done): the frame's floats into
	 *   a_out[0, floats), zero-filled before.
	 */
	struct FrameGlobalsContribution
	{
		std::string_view name;
		std::uint32_t floats = 0;
		void (*capture)(float* a_out) = nullptr;
	};
}
