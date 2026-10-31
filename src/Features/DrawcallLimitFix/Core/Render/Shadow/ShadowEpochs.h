#pragma once

// Helpers shared by the shadow epochs' files (ShadowEpochs.cpp, ShadowCapture.cpp, ShadowShape.cpp, ShadowCapability.cpp).
// Internal to them, to Diagnostics/Draws/ShadowChecks.cpp and to Features/OcclusionMaps/Diagnostics/OcclusionViewCheck.cpp; included after Internal.h.

namespace DCLF::Draws
{
	// ShadowCapture.cpp.
	std::uint32_t EngineRasterStateId(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_bias, std::uint32_t a_scissor, std::uint32_t a_renderMode);
	void CaptureViewTarget(PendingView& a_view, std::uint32_t a_target);
	void CapturePerFrame(PendingView& a_view);

	// ShadowShape.cpp.
	ShadowViewLayout LayoutOf(const PendingView& a_view, std::uint32_t a_slot, std::uint32_t a_mode, std::uint32_t a_target);
}
