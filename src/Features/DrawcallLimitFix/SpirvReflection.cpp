#include "SpirvReflection.h"

#include <cstring>
#include <string_view>

namespace DCLF
{
	namespace
	{
		constexpr std::uint32_t kMagic = 0x07230203;
		constexpr std::uint32_t kHeaderWords = 5;

		// Opcodes, decorations and storage classes from the SPIR-V specification.
		constexpr std::uint16_t kOpName = 5;
		constexpr std::uint16_t kOpExtInstImport = 11;
		constexpr std::uint16_t kOpExtInst = 12;
		constexpr std::uint16_t kOpFunction = 54;
		constexpr std::uint16_t kOpFunctionEnd = 56;
		constexpr std::uint16_t kOpTypeImage = 25;
		constexpr std::uint16_t kOpTypeSampler = 26;
		constexpr std::uint16_t kOpTypeSampledImage = 27;
		constexpr std::uint16_t kOpTypeArray = 28;
		constexpr std::uint16_t kOpTypeRuntimeArray = 29;
		constexpr std::uint16_t kOpTypePointer = 32;
		constexpr std::uint16_t kOpVariable = 59;
		constexpr std::uint16_t kOpDecorate = 71;

		constexpr std::uint32_t kDecorationBuiltIn = 11;
		constexpr std::uint32_t kDecorationLocation = 30;
		constexpr std::uint32_t kDecorationBinding = 33;
		constexpr std::uint32_t kDecorationDescriptorSet = 34;

		constexpr std::uint32_t kStorageUniformConstant = 0;
		constexpr std::uint32_t kStorageInput = 1;
		constexpr std::uint32_t kStorageUniform = 2;
		constexpr std::uint32_t kStorageStorageBuffer = 12;

		constexpr std::uint32_t kNone = ~0u;

		struct Id
		{
			std::uint16_t opcode = 0;
			std::uint32_t storage = kNone;  // variables and pointers
			std::uint32_t type = kNone;     // pointee (pointers), element (arrays), result type (variables)
			std::uint32_t location = kNone;
			std::uint32_t binding = kNone;
			std::uint32_t set = 0;
			bool builtIn = false;
			bool referenced = false;  // named by an instruction in a function body
			bool nonSemantic = false;  // an OpExtInstImport of a NonSemantic.* set
			std::string name;
		};
	}

	bool SpirvReflection::Parse(std::span<const std::byte> a_spirv)
	{
		inputs.clear();
		bindings.clear();
		if (a_spirv.size() < kHeaderWords * 4 || a_spirv.size() % 4 != 0)
			return false;
		std::vector<std::uint32_t> words(a_spirv.size() / 4);
		std::memcpy(words.data(), a_spirv.data(), a_spirv.size());
		if (words[0] != kMagic)
			return false;
		std::vector<Id> ids(words[3]);  // header: id bound
		auto at = [&](std::uint32_t a_id) -> Id* { return a_id < ids.size() ? &ids[a_id] : nullptr; };
		bool inFunction = false;

		for (std::size_t offset = kHeaderWords; offset < words.size();) {
			const std::uint16_t count = static_cast<std::uint16_t>(words[offset] >> 16);
			const std::uint16_t opcode = static_cast<std::uint16_t>(words[offset] & 0xFFFF);
			if (count == 0 || offset + count > words.size())
				return false;
			const std::uint32_t* operands = &words[offset + 1];
			// A binding counts only if the code uses it. Unoptimized builds (-Od) keep every declared
			// resource in the module and the entry point's interface, used or not; debug instructions
			// name variables without accessing them. A literal operand that happens to equal a
			// variable's id can only keep a binding, which is the conservative direction.
			if (opcode == kOpFunction)
				inFunction = true;
			else if (opcode == kOpFunctionEnd)
				inFunction = false;
			else if (inFunction && !(opcode == kOpExtInst && count >= 4 && at(operands[2]) && at(operands[2])->nonSemantic)) {
				// An extended instruction's number is a literal (GLSL.std.450's NClamp is 80, a plausible id).
				for (std::uint16_t i = opcode == kOpExtInst ? 4 : 0; i + 1 < count; ++i)
					if (auto* id = at(operands[i]))
						id->referenced = true;
			}
			switch (opcode) {
			case kOpExtInstImport:
				if (auto* id = count >= 3 ? at(operands[0]) : nullptr)
					id->nonSemantic = std::string_view(reinterpret_cast<const char*>(operands + 1), strnlen(reinterpret_cast<const char*>(operands + 1), (count - 2) * 4)).starts_with("NonSemantic.");
				break;
			case kOpName:
				if (auto* id = count >= 3 ? at(operands[0]) : nullptr)
					id->name.assign(reinterpret_cast<const char*>(operands + 1), strnlen(reinterpret_cast<const char*>(operands + 1), (count - 2) * 4));
				break;
			case kOpDecorate:
				if (auto* id = count >= 3 ? at(operands[0]) : nullptr) {
					const std::uint32_t value = count >= 4 ? operands[2] : 0;
					switch (operands[1]) {
					case kDecorationBuiltIn:
						id->builtIn = true;
						break;
					case kDecorationLocation:
						id->location = value;
						break;
					case kDecorationBinding:
						id->binding = value;
						break;
					case kDecorationDescriptorSet:
						id->set = value;
						break;
					default:
						break;
					}
				}
				break;
			case kOpTypeImage:
			case kOpTypeSampler:
			case kOpTypeSampledImage:
				if (auto* id = at(operands[0]))
					id->opcode = opcode;
				break;
			case kOpTypeArray:
			case kOpTypeRuntimeArray:
				if (auto* id = at(operands[0])) {
					id->opcode = opcode;
					id->type = operands[1];
				}
				break;
			case kOpTypePointer:
				if (auto* id = at(operands[0])) {
					id->opcode = opcode;
					id->storage = operands[1];
					id->type = operands[2];
				}
				break;
			case kOpVariable:
				if (auto* id = at(operands[1])) {
					id->opcode = opcode;
					id->type = operands[0];
					id->storage = operands[2];
				}
				break;
			default:
				break;
			}
			offset += count;
		}

		// The type a variable's pointer points at, through resource arrays.
		auto elementOpcode = [&](std::uint32_t a_pointer) -> std::uint16_t {
			const Id* pointer = at(a_pointer);
			const Id* type = pointer ? at(pointer->type) : nullptr;
			while (type && (type->opcode == kOpTypeArray || type->opcode == kOpTypeRuntimeArray))
				type = at(type->type);
			return type ? type->opcode : 0;
		};

		for (const auto& id : ids) {
			if (id.opcode != kOpVariable)
				continue;
			if (id.storage == kStorageInput) {
				constexpr std::string_view kPrefix = "in.var.";
				if (id.builtIn || id.location == kNone || !id.name.starts_with(kPrefix))
					continue;
				std::string semantic = id.name.substr(kPrefix.size());
				std::size_t digits = semantic.size();
				while (digits > 0 && std::isdigit(static_cast<unsigned char>(semantic[digits - 1])))
					--digits;
				const std::uint32_t index = digits < semantic.size() ? static_cast<std::uint32_t>(std::stoul(semantic.substr(digits))) : 0;
				semantic.resize(digits);
				inputs.push_back({ std::move(semantic), index, id.location });
				continue;
			}
			if (id.binding == kNone || !id.referenced)
				continue;
			BindingKind kind = BindingKind::Other;
			if (id.storage == kStorageUniform)
				kind = BindingKind::ConstantBuffer;
			else if (id.storage == kStorageStorageBuffer)
				kind = BindingKind::StorageBuffer;
			else if (id.storage == kStorageUniformConstant) {
				const auto opcode = elementOpcode(id.type);
				kind = opcode == kOpTypeSampler ? BindingKind::Sampler : (opcode == kOpTypeImage ? BindingKind::Texture : BindingKind::Other);
			}
			bindings.push_back({ id.set, id.binding, kind });
		}
		return true;
	}
}
