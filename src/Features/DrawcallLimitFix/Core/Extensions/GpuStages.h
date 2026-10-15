#pragma once

// The GPU stage contract's Core side (Api/GpuStages.h): what a registered stage's setup and pass factory are handed, and the calls
// Core makes where the stages run (the scene's GPU setup, the main extension's passes, the depth commit's latch).

#include "Features/DrawcallLimitFix/Api/GpuStages.h"

#include <rhi.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct Resources;
}

namespace DCLF::Api
{
	/** @brief A stage's setup (render thread, EnsureSceneBuffers): the device its programs are made on. */
	class GpuStageSetup
	{
	public:
		rhi::Device device;
	};

	/** @brief A stage's pass factory (render thread, the graph's build): the main view's resources and the state its setup made. */
	class GpuStageContext
	{
	public:
		const std::shared_ptr<Draws::Resources>& resources;
		const std::shared_ptr<const void>& state;

		/** @brief The setup's state as the stage's type. */
		template <class T>
		std::shared_ptr<const T> State() const
		{
			return std::static_pointer_cast<const T>(state);
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief Render thread (EnsureSceneBuffers): every registered stage's setup, in order: its state by stage (null: out of the graph). */
	std::vector<std::shared_ptr<const void>> SetUpGpuStages(rhi::Device a_device);
	/** @brief Render thread (the depth commit): a_slot's latch word (GpuStage::latchWord of its stage that has one; 0: none). */
	std::uint32_t StageLatchWord(Api::GpuStageSlot a_slot);
}
