#include "EngineStates.h"

namespace DCLF
{
	RasterStateArray& EngineRasterStates()
	{
		static auto* states = reinterpret_cast<RasterStateArray*>(REL::RelocationID(524748, 411363).address());
		return *states;
	}

	BlendStateArray& EngineBlendStates()
	{
		static auto* states = reinterpret_cast<BlendStateArray*>(REL::RelocationID(524749, 411364).address());
		return *states;
	}

	std::uint32_t DecalDepthBiasMode(std::uint32_t a_decalGroup)
	{
		// The byte the ToggleDepthBias console command flips (Ghidra: read by FUN_1414b3bb0 at
		// 1414b3c0a and 1414b3c7a, written by ConsoleFunc::handler::ToggleDepthBias). AE 1.6.1170.
		static const REL::Relocation<std::uint8_t*> depthBiasEnabled{ REL::Offset(0x2032ff6) };
		const std::uint32_t variant = RE::DrawWorld::GetSingleton().disableSunShadows ? 1u : 0u;
		switch (a_decalGroup) {
		case 1:
			return *depthBiasEnabled ? 6u + variant : 0u;
		case 2:
			return 10u + variant;
		default:
			return 0;
		}
	}
}
