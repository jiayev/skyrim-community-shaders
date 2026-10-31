// The main rows' frame variable places (MainRows.h, PlaceFrameVariables): where a packed material row holds what the frame owns.

#include "Features/DrawcallLimitFix/Core/Render/Payload/MainRows.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

namespace DCLF::Draws
{
	namespace
	{
		struct Placed
		{
			std::uint32_t dword = ~0u;
			std::uint8_t mask = 0;
		};

		// a_variable's packed dword in a_stage's block and the components the record wrote that follow it there, inside the block's a_bytes
		// (mask 0: not placed).
		Placed Place(const ConstantBlock& a_block, const MaterialRowStage& a_stage, std::uint32_t a_variable, std::size_t a_bytes)
		{
			const auto& layout = a_stage.layout;
			if (a_variable >= layout.count || !((a_stage.group >> a_variable) & 1))
				return {};
			const std::uint32_t first = layout.offset[a_variable];
			const std::uint32_t dword = PackedPositionOf(layout, a_stage.table, a_stage.group, a_stage.first, first);
			std::uint8_t mask = 0;
			for (std::uint32_t c = 0; dword != ~0u && c < layout.size[a_variable] && c < 4; ++c)
				if (a_block.Written(first + c) && PackedPositionOf(layout, a_stage.table, a_stage.group, a_stage.first, first + c) == dword + c &&
					(dword + c) * 4 + 4 <= a_bytes)
					mask |= static_cast<std::uint8_t>(1u << c);
			return mask ? Placed{ dword, mask } : Placed{};
		}
	}

	void PlaceFrameVariables(const MaterialRecord& a_material, std::uint8_t a_kind, const MaterialRowStage& a_vs, const MaterialRowStage& a_ps,
		MainRows::MaterialState& a_state)
	{
		// TexcoordOffset's place: the row holds the record's (capture-time) value, which a feature's frame patch overwrites with the
		// buffer the frame's shaders read (a payload product names the place: texcoordDword, texcoordMask).
		if (const auto texcoord = Place(a_material.vs, a_vs, Extensions::PortRules(a_kind).frameVSVariable, kMaterialRowVSBytes); texcoord.mask) {
			a_state.texcoordDword = texcoord.dword;
			a_state.texcoordMask = texcoord.mask;
		}
		// The declared frame variables' places likewise: the row holds the record's value until the frame's patch writes the live one.
		const auto variables = Extensions::FrameVariablesOf(a_kind);
		for (std::size_t i = 0; i < variables.size() && i < a_state.frameVariables.size(); ++i) {
			const auto& variable = variables[i];
			const auto placed = variable.pixel ? Place(a_material.ps, a_ps, variable.variable, kMaterialRowPSBytes) :
			                                     Place(a_material.vs, a_vs, variable.variable, kMaterialRowVSBytes);
			if (placed.mask)
				a_state.frameVariables[i] = { static_cast<std::uint16_t>((variable.pixel ? kMaterialRowPS : kMaterialRowVS) + placed.dword * 4), placed.mask };
		}
	}
}
