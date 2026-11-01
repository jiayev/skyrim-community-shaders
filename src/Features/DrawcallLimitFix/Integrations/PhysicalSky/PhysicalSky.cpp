#include "Features/DrawcallLimitFix/Integrations/PhysicalSky/PhysicalSky.h"
#include "Features/PhysicalSky.h"
#include "Globals.h"

namespace DCLF::Integrations::PhysicalSky
{
	namespace
	{
		template <auto Member>
		void* View()
		{
			const auto& sky = globals::features::physicalSky;
			const auto& texture = sky.*Member;
			return sky.loaded && sky.cbData.enabled && texture ? texture->srv.get() : nullptr;
		}

		void Install(Api::InstallContext& context)
		{
			context.RegisterFrameTexture({ .name = "atmospheric transmittance", .textureRegister = kTransmittance, .view = &View<&::PhysicalSky::texTrLut> });
			context.RegisterFrameTexture({ .name = "mean cloud shadow", .textureRegister = kCloudShadowMean, .view = &View<&::PhysicalSky::texCloudShadowMean> });
			context.RegisterFrameTexture({ .name = "cloud shadow volume", .textureRegister = kCloudShadowVolume, .view = &View<&::PhysicalSky::texShadowVolume> });
		}
	}

	Api::Feature Integration() { return { "physical sky", &Install }; }
}
