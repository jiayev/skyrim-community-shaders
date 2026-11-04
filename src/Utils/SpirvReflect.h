#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/// Reads what the game shader path needs from a SPIR-V module, without a reflection library.
namespace Util::Spirv
{
	constexpr uint32_t kNoLocation = 0xffffffffu;

	enum class ComponentType : uint8_t
	{
		Uint = 1,
		Sint = 2,
		Float = 3
	};

	/// One stage input or output.
	struct IoVariable
	{
		std::string semantic;  ///< Without its index.
		uint32_t semanticIndex = 0;
		uint32_t location = kNoLocation;  ///< kNoLocation for a built-in.
		bool builtIn = false;
		uint32_t components = 0;  ///< In one register.
		uint32_t registers = 0;   ///< More than one for an array or a matrix.
		ComponentType type = ComponentType::Float;
		uint32_t interpolation = 0;  ///< The decoration (Flat, Centroid, ...), 0 for none.
	};

	/// A module's inputs and outputs, in declaration order. Needs a module built with -fspv-reflect, for the semantics.
	struct Interface
	{
		std::vector<IoVariable> inputs;
		std::vector<IoVariable> outputs;
	};

	struct ConstantVariable
	{
		std::string name;
		uint32_t offset = 0;
		uint32_t size = 0;
		uint32_t elements = 0;  ///< 0 if it is not an array.
	};

	struct ConstantBuffer
	{
		std::string name;
		uint32_t size = 0;  ///< To the end of its last variable.
		std::vector<ConstantVariable> variables;
	};

	Interface ReflectInterface(std::span<const uint32_t> a_module);
	std::vector<ConstantBuffer> ReflectConstantBuffers(std::span<const uint32_t> a_module);

	/** @brief A stage's place for one variable: FXC's register and first component. */
	struct IoPlacement
	{
		uint32_t reg = 0;  ///< kNoLocation for what has no register, like SV_Depth.
		uint32_t component = 0;
	};

	/**
	 * @brief What FXC assigns to each variable, in a_variables' order. tools/shader-compare/dxc_survey.py checks this rule against FXC's output.
	 *
	 * In declaration order, a variable goes in the first earlier register that it fits and that interpolates the same. Arrays and matrices start
	 * their own registers, and so do system values, which take a whole one. SV_Target is numbered by its index. Vertex inputs are not packed.
	 */
	std::vector<IoPlacement> PlaceLikeFxc(std::span<const IoVariable> a_variables, bool a_packs);

	/**
	 * @brief Moves a module's stage variables to the given registers.
	 * @param a_module The optimized module, whose variables keep the numbering they have in a_interface.
	 * @return The module with its Location and Component decorations rewritten.
	 */
	std::vector<uint32_t> PlaceVariables(std::span<const uint32_t> a_module, const Interface& a_interface, std::span<const IoPlacement> a_inputs, std::span<const IoPlacement> a_outputs);
}
