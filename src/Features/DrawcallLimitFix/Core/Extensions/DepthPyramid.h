#pragma once

// The depth pyramid contract's Core side (Api/DepthPyramid.h): what the producer's setup is handed, its entry, resolved once at
// InstallFeatures (none registered: no pyramid, frustum culling only), the call Core makes where the main view's resources are made
// (Impl::Setup), and its state for the producer's pass. The main extension registers its resources; the depth commit zeroes its scratch.

#include "Features/DrawcallLimitFix/Api/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"

#include <rhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace DCLF::Api
{
	/** @brief The pyramid's setup (render thread, Impl::Setup): the device its program is made on, and the main view's extent. */
	class DepthPyramidSetup
	{
	public:
		rhi::Device device;
		std::uint32_t width = 0, height = 0;
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry (its names owned). */
	struct DepthPyramidEntry
	{
		std::string resource, scratchResource;
		decltype(Api::DepthPyramidContribution::setup) setup = nullptr;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no setup: none). Once. */
	void ResolveDepthPyramid(const Api::DepthPyramidContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): the contribution's entry; null when none is registered. */
	const DepthPyramidEntry* DepthPyramidOf();
	/**
	 * @brief Render thread (Impl::Setup, per main resource set): the contribution's setup into a_out, its result the producer's state.
	 * Without one, a_out is left empty (no pyramid) and the state null.
	 */
	std::shared_ptr<const void> SetUpDepthPyramid(rhi::Device a_device, std::uint32_t a_width, std::uint32_t a_height, Api::DepthPyramid& a_out);
	/**
	 * @brief Render thread, the producer's pass's make (the graph's build, a Main epoch): the state its setup made for a_context's main
	 * resources (Resources::hzbState), as the producer's type. Those resources also hold the pyramid as Core keeps it (hzb, hzbScratch,
	 * hzbWidth, hzbHeight, hzbMips), the main depth (nativeDepth) and the frames (CurrentFrame).
	 */
	const std::shared_ptr<const void>& DepthPyramidState(const Api::PassContext& a_context);
	template <class T>
	std::shared_ptr<const T> DepthPyramidState(const Api::PassContext& a_context)
	{
		return std::static_pointer_cast<const T>(DepthPyramidState(a_context));
	}
}
