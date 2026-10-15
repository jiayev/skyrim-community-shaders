#pragma once

// DCLF's extension API: a feature is a name and an install function that registers the extension points it fills. Api/ names only
// Api/, standard and ORG types; the contexts its callbacks receive are Core's (Core/Extensions/), which the features inside DCLF include.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/GpuStages.h"
#include "Features/DrawcallLimitFix/Api/Payload.h"

#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief What a feature registers with (Feature::install), implemented by Core (Extensions::InstallFeatures). Valid only during the
	 * install call: once, on the main thread at PostPostLoad, before DCLF's threads start. What is registered is read-only after, and
	 * runs in registration order (the features in FeatureList's order, a feature's registrations in its call order). The descriptors are
	 * copied; their callbacks are plain functions, called per phase or per frame, never per object.
	 */
	class InstallContext
	{
	public:
		virtual FrameValuePoolId RegisterFrameValuePool(const FrameValuePool& a_pool) = 0;
		virtual void RegisterGpuStage(const GpuStage& a_stage) = 0;
		virtual void RegisterPayloadProduct(const PayloadProduct& a_product) = 0;

	protected:
		~InstallContext() = default;
	};

	/** @brief A feature DCLF installs (Features/FeatureList.cpp). */
	struct Feature
	{
		std::string_view name;
		void (*install)(InstallContext& a_context) = nullptr;
	};
}
