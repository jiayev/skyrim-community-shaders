// The shading and frame-globals contributions' calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	void DeriveShading(const RE::BSShaderProperty& a_property, std::span<float, Api::kShadingBlockWords> a_block)
	{
		const std::uint64_t flags = a_property.flags.underlying();
		for (const auto& contribution : RegisteredShadingContributions())
			if (flags & contribution.propertyFlags)
				contribution.derive({ a_property, a_block });
	}

	const ShadingBlock& ShadingBlockRule()
	{
		static const ShadingBlock rule = [] {
			ShadingBlock block;
			for (const auto& contribution : RegisteredShadingContributions())
				if (contribution.blockWords) {
					block.objectFlags = contribution.objectFlags;
					block.blockWords = contribution.blockWords;
					block.variables = contribution.vsVariables;
					for (const auto& variable : contribution.vsVariables)
						block.vsMask |= 1ull << variable.variable;
				}
			return block;
		}();
		return rule;
	}

	void CaptureFrameGlobals(std::array<float, kFrameGlobalsFloats>& a_out)
	{
		for (const auto& contribution : RegisteredFrameGlobals())
			contribution.capture(a_out.data() + contribution.first);
	}
}
