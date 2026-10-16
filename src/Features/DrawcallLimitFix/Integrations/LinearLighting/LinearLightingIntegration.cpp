// Linear Lighting: its LLPerGeometry (PS b8), a per-draw buffer. Declaration-only.

#include "Features/DrawcallLimitFix/Integrations/LinearLighting/LinearLighting.h"

namespace DCLF::Integrations::LinearLighting
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// The last drawn object's emissive multiplier, never the frame's: DCLF's draws take it from the shading row.
			a_context.DeclarePerDrawBuffer({ .name = "LLPerGeometry", .stage = Api::ShaderStage::Pixel, .constantRegister = kLLPerGeometryRegister });
		}
	}

	Api::Feature Integration()
	{
		return { "linear lighting", &Install };
	}
}
