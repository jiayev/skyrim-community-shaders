#include "MaterialPortParity.h"

#include <bit>

namespace DCLF::MaterialPort
{
	namespace
	{
		/** @brief The variable a float of a ConstantBlock belongs to (its StageLayout index), and the component in it; -1 when none. */
		std::pair<int, std::uint32_t> VariableOf(const StageLayout& a_layout, std::uint32_t a_float)
		{
			for (std::uint32_t v = 0; v < a_layout.count; ++v)
				if (a_float >= a_layout.offset[v] && a_float < std::uint32_t(a_layout.offset[v]) + a_layout.size[v])
					return { static_cast<int>(v), a_float - a_layout.offset[v] };
			return { -1, 0 };
		}

		std::string Value(float a_value)
		{
			const auto bits = std::bit_cast<std::uint32_t>(a_value);
			return bits == kUnwrittenBits ? std::string("unwritten") : fmt::format("{} ({:08X})", a_value, bits);
		}
	}

	std::string DescribeDifference(const MaterialRecord& a_port, const MaterialRecord& a_engine, std::array<std::uint64_t, 7>* a_kinds)
	{
		std::string first;
		auto note = [&](std::size_t a_kind, auto&& a_describe) {
			if (a_kinds)
				++(*a_kinds)[a_kind];
			if (first.empty())
				first = a_describe();
		};
		auto blocks = [&](std::size_t a_kind, const char* a_stage, const ConstantBlock& a_p, const ConstantBlock& a_e, const StageLayout& a_layout) {
			for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f)
				if (!a_p.SameBits(a_e, f)) {
					note(a_kind, [&] {
						const auto [variable, component] = VariableOf(a_layout, f);
						return fmt::format("{} variable {} component {} (float {}): port {}, engine {}", a_stage, variable, component, f, Value(a_p.floats[f]),
							Value(a_e.floats[f]));
					});
					return;
				}
		};
		blocks(0, "VS", a_port.vs, a_engine.vs, LightingVSLayout());
		blocks(1, "PS", a_port.ps, a_engine.ps, LightingPSLayout());
		auto slots = [&](std::size_t a_kind, const char* a_what, const auto& a_p, const auto& a_e) {
			for (std::size_t s = 0; s < a_p.size(); ++s)
				if (a_p[s] != a_e[s]) {
					note(a_kind, [&] { return fmt::format("{} t{}: port {}, engine {}", a_what, s, fmt::ptr(reinterpret_cast<const void*>(std::uintptr_t(a_p[s]))),
										 fmt::ptr(reinterpret_cast<const void*>(std::uintptr_t(a_e[s])))); });
					return;
				}
		};
		slots(2, "texture", a_port.textures, a_engine.textures);
		slots(3, "address mode", a_port.addressModes, a_engine.addressModes);
		slots(4, "filter mode", a_port.filterModes, a_engine.filterModes);
		if (a_port.textureWritten != a_engine.textureWritten)
			note(5, [&] { return fmt::format("textures written: port {:04X}, engine {:04X}", a_port.textureWritten, a_engine.textureWritten); });
		for (std::uint32_t i = 0; i < kFeatureMaterialTextures; ++i)
			if (a_port.featureTextures[i] != a_engine.featureTextures[i]) {
				note(6, [&] { return fmt::format("feature texture t{}: port {}, engine {}", kFeatureMaterialRegisters[i], fmt::ptr(a_port.featureTextures[i]),
									fmt::ptr(a_engine.featureTextures[i])); });
				break;
			}
		return first;
	}
}
