#pragma once

// The depth pyramid contract's Core side (Api/DepthPyramid.h): what the producer's setup and pass factory are handed, its entry, resolved
// once at InstallFeatures (none registered: no pyramid, frustum culling only), and the call Core makes where the main view's resources
// are made (Impl::Setup). The main extension registers its resources and adds its pass; the depth commit zeroes its scratch.

#include "Features/DrawcallLimitFix/Api/DepthPyramid.h"

#include <rhi.h>

#include <cstdint>
#include <memory>
#include <string>

namespace DCLF::Draws
{
	struct Resources;
}

namespace DCLF::Api
{
	/** @brief The pyramid's setup (render thread, Impl::Setup): the device its program is made on, and the main view's extent. */
	class DepthPyramidSetup
	{
	public:
		rhi::Device device;
		std::uint32_t width = 0, height = 0;
	};

	/**
	 * @brief The pyramid's pass factory (render thread, the graph's build): the main view's resources (the main depth, nativeDepth; the
	 * pyramid as Core holds it, hzb, hzbScratch, hzbWidth, hzbHeight, hzbMips; the frames, CurrentFrame) and the state its setup made.
	 */
	class DepthPyramidContext
	{
	public:
		const std::shared_ptr<Draws::Resources>& resources;
		const std::shared_ptr<const void>& state;

		/** @brief The setup's state as the producer's type. */
		template <class T>
		std::shared_ptr<const T> State() const
		{
			return std::static_pointer_cast<const T>(state);
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief The contribution's entry (its names owned). */
	struct DepthPyramidEntry
	{
		std::string resource, scratchResource, passName;
		decltype(Api::DepthPyramidContribution::setup) setup = nullptr;
		decltype(Api::DepthPyramidContribution::makePass) makePass = nullptr;
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
}
