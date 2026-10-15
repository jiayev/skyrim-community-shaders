// The shading and frame-globals contributions' calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	void DeriveShading(const RE::BSShaderProperty& a_property, ObjectTreeAnim& a_out)
	{
		const std::uint64_t flags = a_property.flags.underlying();
		for (const auto& contribution : RegisteredShadingContributions())
			if (flags & contribution.propertyFlags)
				contribution.derive({ a_property, a_out });
	}

	void CaptureFrameGlobals(std::array<float, kFrameGlobalsFloats>& a_out)
	{
		for (const auto& contribution : RegisteredFrameGlobals())
			contribution.capture(a_out.data() + contribution.first);
	}
}
