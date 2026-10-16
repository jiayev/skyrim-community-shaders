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

	std::uint32_t ReferenceFactsOf(const RE::TESObjectREFR* a_reference)
	{
		const auto& rules = RegisteredLightingJoinRules();
		if (!a_reference)
			return rules.factsNone;
		std::uint32_t facts = 0;
		for (std::uint32_t f = 0; f < rules.factCount; ++f)
			facts |= rules.facts[f].sample(*a_reference) ? 1u << f : 0u;
		return facts;
	}

	std::uint8_t CapturePropertyBits(const RE::BSShaderProperty& a_property)
	{
		const auto& rules = RegisteredLightingJoinRules();
		std::uint8_t bits = 0;
		for (std::uint32_t i = 0; i < rules.propertyBitsCount; ++i)
			bits = static_cast<std::uint8_t>(bits | (rules.propertyBits[i].capture(a_property) & rules.propertyBits[i].bits));
		return bits;
	}

	std::uint32_t LightingObjectFlagsOf(const Api::LightingJoinView& a_view)
	{
		const auto& rules = RegisteredLightingJoinRules();
		std::uint32_t flags = 0;
		for (std::uint32_t i = 0; i < rules.objectFlagCount; ++i)
			flags |= rules.objectFlags[i].applies(a_view) ? rules.objectFlags[i].objectFlag : 0u;
		return flags;
	}

	std::uint32_t LightingKeyBitsOf(const Api::LightingJoinView& a_view)
	{
		const auto keyBits = RegisteredLightingJoinRules().keyBits.keyBits;
		return keyBits ? keyBits(a_view) & ((1u << kLightingKeySpareBits) - 1) : 0u;
	}

	std::uint32_t LightingKeyPermutation(std::uint32_t a_keyBits)
	{
		const auto permutation = RegisteredLightingJoinRules().keyBits.permutation;
		return permutation ? permutation(a_keyBits) : 0u;
	}

	std::uint32_t DeriveLightingPass(std::uint32_t a_pass, const Api::LightingJoinView& a_view)
	{
		const auto& rules = RegisteredLightingJoinRules();
		for (std::uint32_t i = 0; i < rules.deriveCount; ++i)
			a_pass = rules.derives[i](a_pass, a_view);
		return a_pass;
	}
}
