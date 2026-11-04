#include "Utils/SpirvReflect.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Util::Spirv
{
	namespace
	{
		constexpr uint32_t kHeaderWords = 5;

		// The opcodes, storage classes and decorations this file reads, from the SPIR-V specification.
		enum Op : uint32_t
		{
			OpName = 5,
			OpMemberName = 6,
			OpTypeBool = 20,
			OpTypeInt = 21,
			OpTypeFloat = 22,
			OpTypeVector = 23,
			OpTypeMatrix = 24,
			OpTypeArray = 28,
			OpTypeStruct = 30,
			OpTypePointer = 32,
			OpConstant = 43,
			OpFunction = 54,
			OpVariable = 59,
			OpDecorate = 71,
			OpMemberDecorate = 72,
			OpDecorateString = 5632
		};
		constexpr uint32_t kStorageInput = 1;
		constexpr uint32_t kStorageUniform = 2;
		constexpr uint32_t kStorageOutput = 3;
		constexpr uint32_t kDecorationRowMajor = 4;
		constexpr uint32_t kDecorationArrayStride = 6;
		constexpr uint32_t kDecorationBuiltIn = 11;
		constexpr uint32_t kDecorationNoPerspective = 13;
		constexpr uint32_t kDecorationFlat = 14;
		constexpr uint32_t kDecorationCentroid = 16;
		constexpr uint32_t kDecorationSample = 17;
		constexpr uint32_t kDecorationLocation = 30;
		constexpr uint32_t kDecorationComponent = 31;
		constexpr uint32_t kDecorationOffset = 35;

		struct VectorType
		{
			uint32_t element = 0;
			uint32_t count = 0;
			bool matrix = false;
		};

		struct Variable
		{
			uint32_t id = 0;
			uint32_t pointee = 0;
			uint32_t storage = 0;
		};

		/** @brief The declarations of a module, read once. */
		struct Module
		{
			std::unordered_map<uint32_t, std::string> names;
			std::unordered_map<uint64_t, std::string> memberNames;
			std::unordered_map<uint64_t, uint32_t> memberOffsets;
			std::unordered_set<uint64_t> rowMajorMembers;
			std::unordered_map<uint32_t, std::string> semantics;
			std::unordered_map<uint32_t, uint32_t> builtIns;
			std::unordered_map<uint32_t, uint32_t> locations;
			std::unordered_map<uint32_t, uint32_t> interpolation;
			std::unordered_map<uint32_t, uint32_t> strides;
			std::unordered_map<uint32_t, uint32_t> scalarBytes;
			std::unordered_map<uint32_t, ComponentType> scalarTypes;
			std::unordered_map<uint32_t, VectorType> vectors;
			std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> arrays;
			std::unordered_map<uint32_t, uint32_t> constants;
			std::unordered_map<uint32_t, std::vector<uint32_t>> structs;
			std::unordered_map<uint32_t, uint32_t> pointees;
			std::vector<Variable> variables;
		};

		uint64_t MemberKey(uint32_t a_struct, uint32_t a_member)
		{
			return (static_cast<uint64_t>(a_struct) << 32) | a_member;
		}

		std::string WordsToString(std::span<const uint32_t> a_words)
		{
			const auto* text = reinterpret_cast<const char*>(a_words.data());
			return std::string(text, strnlen(text, a_words.size_bytes()));
		}

		Module Read(std::span<const uint32_t> a_words)
		{
			Module module;
			for (size_t i = kHeaderWords; i < a_words.size();) {
				const uint32_t length = a_words[i] >> 16;
				const uint32_t op = a_words[i] & 0xffff;
				if (!length || i + length > a_words.size())
					break;
				const auto args = a_words.subspan(i + 1, length - 1);
				switch (op) {
				case OpName:
					module.names[args[0]] = WordsToString(args.subspan(1));
					break;
				case OpMemberName:
					module.memberNames[MemberKey(args[0], args[1])] = WordsToString(args.subspan(2));
					break;
				case OpDecorate:
					switch (args[1]) {
					case kDecorationBuiltIn:
						module.builtIns[args[0]] = args[2];
						break;
					case kDecorationLocation:
						module.locations[args[0]] = args[2];
						break;
					case kDecorationArrayStride:
						module.strides[args[0]] = args[2];
						break;
					case kDecorationNoPerspective:
					case kDecorationFlat:
					case kDecorationCentroid:
					case kDecorationSample:
						module.interpolation[args[0]] = args[1];
						break;
					default:
						break;
					}
					break;
				case OpMemberDecorate:
					if (args[2] == kDecorationOffset)
						module.memberOffsets[MemberKey(args[0], args[1])] = args[3];
					else if (args[2] == kDecorationRowMajor)
						module.rowMajorMembers.insert(MemberKey(args[0], args[1]));
					break;
				case OpDecorateString:
					module.semantics[args[0]] = WordsToString(args.subspan(2));
					break;
				case OpTypeBool:
					module.scalarBytes[args[0]] = 4;
					module.scalarTypes[args[0]] = ComponentType::Uint;
					break;
				case OpTypeInt:
					module.scalarBytes[args[0]] = args[1] / 8;
					module.scalarTypes[args[0]] = args[2] ? ComponentType::Sint : ComponentType::Uint;
					break;
				case OpTypeFloat:
					module.scalarBytes[args[0]] = args[1] / 8;
					module.scalarTypes[args[0]] = ComponentType::Float;
					break;
				case OpTypeVector:
				case OpTypeMatrix:
					module.vectors[args[0]] = { args[1], args[2], op == OpTypeMatrix };
					break;
				case OpTypeArray:
					module.arrays[args[0]] = { args[1], args[2] };
					break;
				case OpTypeStruct:
					module.structs[args[0]] = std::vector<uint32_t>(args.begin() + 1, args.end());
					break;
				case OpTypePointer:
					module.pointees[args[0]] = args[2];
					break;
				case OpConstant:
					if (length >= 4)
						module.constants[args[1]] = args[2];
					break;
				case OpVariable:
					module.variables.push_back({ args[1], module.pointees[args[0]], args[2] });
					break;
				default:
					break;
				}
				if (op == OpFunction)
					break;
				i += length;
			}
			return module;
		}

		struct Shape
		{
			uint32_t components = 1;
			uint32_t registers = 1;
			ComponentType type = ComponentType::Float;
		};

		Shape ShapeOf(const Module& a_module, uint32_t a_type)
		{
			if (const auto array = a_module.arrays.find(a_type); array != a_module.arrays.end()) {
				auto shape = ShapeOf(a_module, array->second.first);
				shape.registers *= a_module.constants.at(array->second.second);
				return shape;
			}
			if (const auto vector = a_module.vectors.find(a_type); vector != a_module.vectors.end()) {
				auto shape = ShapeOf(a_module, vector->second.element);
				if (vector->second.matrix)
					shape.registers = vector->second.count;
				else
					shape.components = vector->second.count;
				return shape;
			}
			const auto scalar = a_module.scalarTypes.find(a_type);
			return { 1, 1, scalar != a_module.scalarTypes.end() ? scalar->second : ComponentType::Float };
		}

		// A constant buffer member's size in the dx layout: the last row or element is not padded.
		uint32_t SizeOf(const Module& a_module, uint32_t a_type, bool a_rowMajor)
		{
			if (const auto array = a_module.arrays.find(a_type); array != a_module.arrays.end()) {
				const auto stride = a_module.strides.find(a_type);
				return (stride != a_module.strides.end() ? stride->second : 16) * (a_module.constants.at(array->second.second) - 1) + SizeOf(a_module, array->second.first, a_rowMajor);
			}
			if (const auto vector = a_module.vectors.find(a_type); vector != a_module.vectors.end()) {
				if (!vector->second.matrix)
					return vector->second.count * SizeOf(a_module, vector->second.element, false);
				const uint32_t columnSize = ShapeOf(a_module, vector->second.element).components;
				// A row major matrix is stored as its rows: one vector per component of a column.
				const uint32_t stored = a_rowMajor ? columnSize : vector->second.count;
				const uint32_t vectorSize = a_rowMajor ? vector->second.count : columnSize;
				return 16 * (stored - 1) + 4 * vectorSize;
			}
			if (const auto members = a_module.structs.find(a_type); members != a_module.structs.end()) {
				if (members->second.empty())
					return 0;
				const auto last = static_cast<uint32_t>(members->second.size() - 1);
				const auto key = MemberKey(a_type, last);
				const auto offset = a_module.memberOffsets.find(key);
				return (offset != a_module.memberOffsets.end() ? offset->second : 0) + SizeOf(a_module, members->second[last], a_module.rowMajorMembers.contains(key));
			}
			const auto scalar = a_module.scalarBytes.find(a_type);
			return scalar != a_module.scalarBytes.end() ? scalar->second : 4;
		}

		// "TEXCOORD12" to ("TEXCOORD", 12); "SV_IsFrontFace" to itself and 0.
		std::pair<std::string, uint32_t> SplitSemantic(std::string_view a_semantic)
		{
			size_t digits = a_semantic.size();
			while (digits > 0 && std::isdigit(static_cast<unsigned char>(a_semantic[digits - 1])))
				--digits;
			uint32_t index = 0;
			for (size_t i = digits; i < a_semantic.size(); ++i)
				index = index * 10 + (a_semantic[i] - '0');
			return { std::string(a_semantic.substr(0, digits)), index };
		}

		std::string Upper(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return a_text;
		}
	}

	Interface ReflectInterface(std::span<const uint32_t> a_module)
	{
		const auto module = Read(a_module);
		Interface result;
		for (const auto& variable : module.variables) {
			if (variable.storage != kStorageInput && variable.storage != kStorageOutput)
				continue;
			IoVariable io;
			const auto semantic = module.semantics.find(variable.id);
			const auto name = module.names.find(variable.id);
			std::tie(io.semantic, io.semanticIndex) = SplitSemantic(semantic != module.semantics.end() ? std::string_view(semantic->second) : name != module.names.end() ? std::string_view(name->second) :
			                                                                                                                                                              std::string_view());
			if (const auto location = module.locations.find(variable.id); location != module.locations.end())
				io.location = location->second;
			io.builtIn = module.builtIns.contains(variable.id);
			const auto shape = ShapeOf(module, variable.pointee);
			io.components = shape.components;
			io.registers = shape.registers;
			io.type = shape.type;
			if (const auto interpolation = module.interpolation.find(variable.id); interpolation != module.interpolation.end())
				io.interpolation = interpolation->second;
			(variable.storage == kStorageInput ? result.inputs : result.outputs).push_back(std::move(io));
		}
		return result;
	}

	std::vector<ConstantBuffer> ReflectConstantBuffers(std::span<const uint32_t> a_module)
	{
		const auto module = Read(a_module);
		std::vector<ConstantBuffer> buffers;
		for (const auto& variable : module.variables) {
			if (variable.storage != kStorageUniform)
				continue;
			const auto members = module.structs.find(variable.pointee);
			if (members == module.structs.end())
				continue;
			ConstantBuffer buffer;
			if (const auto name = module.names.find(variable.pointee); name != module.names.end())
				buffer.name = name->second;
			if (buffer.name.starts_with("type."))
				buffer.name.erase(0, 5);
			buffer.size = SizeOf(module, variable.pointee, false);
			for (uint32_t m = 0; m < members->second.size(); ++m) {
				const auto key = MemberKey(variable.pointee, m);
				ConstantVariable member;
				if (const auto name = module.memberNames.find(key); name != module.memberNames.end())
					member.name = name->second;
				if (const auto offset = module.memberOffsets.find(key); offset != module.memberOffsets.end())
					member.offset = offset->second;
				member.size = SizeOf(module, members->second[m], module.rowMajorMembers.contains(key));
				if (const auto array = module.arrays.find(members->second[m]); array != module.arrays.end())
					member.elements = module.constants.at(array->second.second);
				buffer.variables.push_back(std::move(member));
			}
			buffers.push_back(std::move(buffer));
		}
		return buffers;
	}

	std::vector<IoPlacement> PlaceLikeFxc(std::span<const IoVariable> a_variables, bool a_packs)
	{
		struct Register
		{
			uint32_t used = 0;
			uint32_t mode = 0;
		};
		constexpr uint32_t kFlat = kDecorationFlat;

		std::vector<IoPlacement> placements;
		placements.reserve(a_variables.size());
		std::vector<Register> registers;
		for (const auto& variable : a_variables) {
			const auto name = Upper(variable.semantic);
			if (name == "SV_DEPTH" || name == "SV_COVERAGE" || name == "SV_DEPTHGREATEREQUAL" || name == "SV_DEPTHLESSEQUAL") {
				placements.push_back({ kNoLocation, 0 });
				continue;
			}
			if (name == "SV_TARGET") {
				placements.push_back({ variable.semanticIndex, 0 });
				continue;
			}
			const uint32_t mode = variable.type == ComponentType::Float ? variable.interpolation : kFlat;
			const bool system = variable.builtIn || name.starts_with("SV_");
			if (a_packs && variable.registers == 1 && !system) {
				const auto fits = std::ranges::find_if(registers, [&](const Register& a_register) { return a_register.mode == mode && a_register.used + variable.components <= 4; });
				if (fits != registers.end()) {
					placements.push_back({ static_cast<uint32_t>(fits - registers.begin()), fits->used });
					fits->used += variable.components;
					continue;
				}
			}
			placements.push_back({ static_cast<uint32_t>(registers.size()), 0 });
			for (uint32_t r = 0; r < variable.registers; ++r)
				registers.push_back({ variable.registers > 1 || system ? 4 : variable.components, mode });
		}
		return placements;
	}

	std::vector<uint32_t> PlaceVariables(std::span<const uint32_t> a_module, const Interface& a_interface, std::span<const IoPlacement> a_inputs, std::span<const IoPlacement> a_outputs)
	{
		// Interface locations to placements, per direction. A built-in has no location, so it has no entry.
		auto placementsByLocation = [](std::span<const IoVariable> a_variables, std::span<const IoPlacement> a_placements) {
			std::unordered_map<uint32_t, IoPlacement> map;
			for (size_t i = 0; i < a_variables.size(); ++i)
				if (a_variables[i].location != kNoLocation)
					map[a_variables[i].location] = a_placements[i];
			return map;
		};
		const auto inputs = placementsByLocation(a_interface.inputs, a_inputs);
		const auto outputs = placementsByLocation(a_interface.outputs, a_outputs);

		const auto module = Read(a_module);
		std::unordered_map<uint32_t, uint32_t> storageOf;
		for (const auto& variable : module.variables)
			storageOf[variable.id] = variable.storage;

		std::vector<uint32_t> result(a_module.begin(), a_module.begin() + kHeaderWords);
		result.reserve(a_module.size() + 32);
		for (size_t i = kHeaderWords; i < a_module.size();) {
			const uint32_t length = a_module[i] >> 16;
			if (!length || i + length > a_module.size())
				break;
			const auto instruction = a_module.subspan(i, length);
			i += length;
			if ((instruction[0] & 0xffff) == OpDecorate && instruction[2] == kDecorationLocation) {
				const auto storage = storageOf.find(instruction[1]);
				const auto& map = storage != storageOf.end() && storage->second == kStorageInput ? inputs : outputs;
				if (const auto placement = map.find(instruction[3]); placement != map.end() && placement->second.reg != kNoLocation) {
					result.insert(result.end(), { instruction[0], instruction[1], kDecorationLocation, placement->second.reg });
					if (placement->second.component)
						result.insert(result.end(), { (4u << 16) | OpDecorate, instruction[1], kDecorationComponent, placement->second.component });
					continue;
				}
			}
			result.insert(result.end(), instruction.begin(), instruction.end());
		}
		return result;
	}
}
