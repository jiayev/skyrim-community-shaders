// The features' shader manifest: the compile-time constants their shaders share with Core's (the frame bindings' texture registers),
// checked here not to overlap Core's or each other's. A feature with a frame binding (Api::FrameBinding) lists its register; the install
// checks the registrations the same way (InstallContext::RegisterFrameBinding).

#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace DCLF::Features
{
	namespace
	{
		// Every feature's frame-binding register (its HLSL declares the same: TreeWind's DCLFTreeWind, Common/DCLFObjects.hlsli).
		constexpr std::uint32_t kFrameBindingRegisters[] = {
			TreeWind::kWindRegister,
		};

		constexpr bool FrameBindingRegistersValid()
		{
			for (std::size_t i = 0; i < std::size(kFrameBindingRegisters); ++i) {
				const std::uint32_t r = kFrameBindingRegisters[i];
				if (r >= kTextureRegisters || Extensions::CoreFrameRegister(r))
					return false;
				for (std::size_t j = 0; j < i; ++j)
					if (kFrameBindingRegisters[j] == r)
						return false;
			}
			return true;
		}
	}

	static_assert(FrameBindingRegistersValid(), "a feature's frame-binding register is Core's, another feature's, or past t127");
	static_assert(std::size(kFrameBindingRegisters) <= Extensions::kFrameBindingSlots, "more frame bindings than ResourceAddresses embeds: grow kFrameBindingSlots");
}
