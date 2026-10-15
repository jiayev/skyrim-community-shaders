// The Effect port's frame inputs, Core's built-ins until Features/Effects registers them: its three frame constant rows
// (Extensions::CoreFrameConstantRows: LightingConstants' SampleEffectCamera, SampleEffectLight and SampleEffectBlood) and the soft effects'
// depth (Extensions::CoreFrameTextures).

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"

#include "Features/DrawcallLimitFix/Scene/Ports/EffectConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"

#include <cstring>

namespace DCLF::Extensions
{
	namespace
	{
		template <std::array<float, 4> (*Sample)()>
		void SampleRow(float (&a_out)[4])
		{
			const auto value = Sample();
			static_assert(sizeof(value) == sizeof(a_out));
			std::memcpy(a_out, value.data(), sizeof(a_out));
		}

		// Soft effects' depth: the kPOST_ZPREPASS_COPY SetupTechnique binds for a Soft pass, which CS fills from the main depth after the
		// deferred pass, before the forward epoch.
		void* SoftDepthView()
		{
			return globals::game::renderer ? static_cast<void*>(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
			                                 nullptr;
		}
	}

	std::span<const Api::FrameConstantRow> CoreFrameConstantRows()
	{
		// The camera planes soft effects read (DCLFEffectCamera), the sun's effect colour (DCLFEffectDLightColor), and Blood's
		// AlphaTestRef.yz: the weapon blood's two globals (DCLFEffectBlood).
		static const Api::FrameConstantRow rows[] = {
			{ "effect camera", kEffectCameraFrameRow, &SampleRow<&SampleEffectCamera> },
			{ "effect light", kEffectLightFrameRow, &SampleRow<&SampleEffectLight> },
			{ "effect blood", kEffectBloodFrameRow, &SampleRow<&SampleEffectBlood> },
		};
		return rows;
	}

	std::span<const Api::FrameTexture> CoreFrameTextures()
	{
		static const Api::FrameTexture textures[] = {
			{ "effect soft depth", kEffectSoftDepthRegister, &SoftDepthView },
		};
		return textures;
	}
}
