// The registered GPU stages' setup and latch word (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/GpuStages.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	std::vector<std::shared_ptr<const void>> SetUpGpuStages(rhi::Device a_device, const Draws::SceneBuffers& a_scene)
	{
		const auto stages = RegisteredStages();
		std::vector<std::shared_ptr<const void>> states(stages.size());
		const Api::GpuStageSetup setup{ a_device, a_scene };
		for (std::size_t i = 0; i < stages.size(); ++i)
			states[i] = stages[i].setup(setup);
		return states;
	}

	std::uint32_t StageLatchWord(Api::GpuStageSlot a_slot)
	{
		for (const auto& stage : RegisteredStages())
			if (stage.slot == a_slot && stage.latchWord)
				return stage.latchWord();
		return 0;
	}
}
