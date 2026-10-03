#include "EngineStates.h"

#include "Deferred.h"

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

	ID3D11BlendState* DeferredBlendState(std::uint32_t a_blendMode, std::uint32_t a_alphaToCoverage, std::uint32_t a_writeMode, std::uint32_t a_extra)
	{
		auto* deferred = globals::deferred;
		if (!deferred || a_blendMode >= 7 || a_alphaToCoverage >= 2 || a_writeMode >= 13 || a_extra >= 2)
			return nullptr;
		return deferred->deferredBlendStates[a_blendMode][a_alphaToCoverage][a_writeMode][a_extra];
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
		case 3:
			// A multi-index shape's layer, geometry group 2 (FUN_1414b3bb0 at 1414b3c7a).
			return *depthBiasEnabled ? 8u + variant : 0u;
		default:
			return 0;
		}
	}
}
