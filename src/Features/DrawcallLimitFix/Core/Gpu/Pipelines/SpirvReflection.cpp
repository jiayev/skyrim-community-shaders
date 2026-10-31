// SpirvReflection::Parse: one pass over a DXC module's words. Pure; any thread.

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"

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
		constexpr std::uint16_t kOpMemberName = 6;
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
		constexpr std::uint16_t kOpMemberDecorate = 72;

		constexpr std::uint32_t kDecorationBuiltIn = 11;
		constexpr std::uint32_t kDecorationLocation = 30;
		constexpr std::uint32_t kDecorationOffset = 35;
		constexpr std::uint32_t kDecorationBinding = 33;
		constexpr std::uint32_t kDecorationDescriptorSet = 34;

		constexpr std::uint32_t kStorageUniformConstant = 0;
		constexpr std::uint32_t kStorageInput = 1;
		constexpr std::uint32_t kStorageUniform = 2;
		constexpr std::uint32_t kStorageOutput = 3;
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
			// A struct type's members, by index: their names (OpMemberName) and byte offsets (Offset; kNone where it has none).
			std::vector<std::string> memberNames;
			std::vector<std::uint32_t> memberOffsets;
		};

		std::string LiteralString(const std::uint32_t* a_words, std::size_t a_wordCount)
		{
			return std::string(reinterpret_cast<const char*>(a_words), strnlen(reinterpret_cast<const char*>(a_words), a_wordCount * 4));
		}

		Id* At(std::vector<Id>& a_ids, std::uint32_t a_id) { return a_id < a_ids.size() ? &a_ids[a_id] : nullptr; }
		const Id* At(const std::vector<Id>& a_ids, std::uint32_t a_id) { return a_id < a_ids.size() ? &a_ids[a_id] : nullptr; }

		// A binding counts only if the code uses it. Unoptimized builds (-Od) keep every declared
		// resource in the module and the entry point's interface, used or not; debug instructions
		// name variables without accessing them. A literal operand that happens to equal a
		// variable's id can only keep a binding, which is the conservative direction.
		void MarkReferenced(std::vector<Id>& a_ids, std::uint16_t a_opcode, std::uint16_t a_count, const std::uint32_t* a_operands, bool& a_inFunction)
		{
			const auto at = [&](std::uint32_t a_id) { return At(a_ids, a_id); };
			if (a_opcode == kOpFunction)
				a_inFunction = true;
			else if (a_opcode == kOpFunctionEnd)
				a_inFunction = false;
			else if (a_inFunction && !(a_opcode == kOpExtInst && a_count >= 4 && at(a_operands[2]) && at(a_operands[2])->nonSemantic)) {
				// An extended instruction's number is a literal (GLSL.std.450's NClamp is 80, a plausible id).
				for (std::uint16_t i = a_opcode == kOpExtInst ? 4 : 0; i + 1 < a_count; ++i)
					if (auto* id = at(a_operands[i]))
						id->referenced = true;
			}
		}

		// OpExtInstImport, OpName, OpMemberName.
		void ReadName(std::vector<Id>& a_ids, std::uint16_t a_opcode, std::uint16_t a_count, const std::uint32_t* a_operands)
		{
			const auto at = [&](std::uint32_t a_id) { return At(a_ids, a_id); };
			switch (a_opcode) {
			case kOpExtInstImport:
				if (auto* id = a_count >= 3 ? at(a_operands[0]) : nullptr)
					id->nonSemantic = std::string_view(reinterpret_cast<const char*>(a_operands + 1), strnlen(reinterpret_cast<const char*>(a_operands + 1), (a_count - 2) * 4)).starts_with("NonSemantic.");
				break;
			case kOpName:
				if (auto* id = a_count >= 3 ? at(a_operands[0]) : nullptr)
					id->name.assign(reinterpret_cast<const char*>(a_operands + 1), strnlen(reinterpret_cast<const char*>(a_operands + 1), (a_count - 2) * 4));
				break;
			case kOpMemberName:
				if (auto* id = a_count >= 4 ? at(a_operands[0]) : nullptr) {
					if (id->memberNames.size() <= a_operands[1])
						id->memberNames.resize(std::size_t(a_operands[1]) + 1);
					id->memberNames[a_operands[1]] = LiteralString(a_operands + 2, a_count - 3);
				}
				break;
			default:
				break;
			}
		}

		// OpMemberDecorate (Offset), OpDecorate.
		void ReadDecoration(std::vector<Id>& a_ids, std::uint16_t a_opcode, std::uint16_t a_count, const std::uint32_t* a_operands)
		{
			const auto at = [&](std::uint32_t a_id) { return At(a_ids, a_id); };
			if (a_opcode == kOpMemberDecorate) {
				if (auto* id = a_count >= 5 && a_operands[2] == kDecorationOffset ? at(a_operands[0]) : nullptr) {
					if (id->memberOffsets.size() <= a_operands[1])
						id->memberOffsets.resize(std::size_t(a_operands[1]) + 1, kNone);
					id->memberOffsets[a_operands[1]] = a_operands[3];
				}
				return;
			}
			if (auto* id = a_count >= 3 ? at(a_operands[0]) : nullptr) {
				const std::uint32_t value = a_count >= 4 ? a_operands[2] : 0;
				switch (a_operands[1]) {
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
		}

		// The resource, array and pointer types, and variables.
		void ReadType(std::vector<Id>& a_ids, std::uint16_t a_opcode, const std::uint32_t* a_operands)
		{
			const auto at = [&](std::uint32_t a_id) { return At(a_ids, a_id); };
			switch (a_opcode) {
			case kOpTypeImage:
			case kOpTypeSampler:
			case kOpTypeSampledImage:
				if (auto* id = at(a_operands[0]))
					id->opcode = a_opcode;
				break;
			case kOpTypeArray:
			case kOpTypeRuntimeArray:
				if (auto* id = at(a_operands[0])) {
					id->opcode = a_opcode;
					id->type = a_operands[1];
				}
				break;
			case kOpTypePointer:
				if (auto* id = at(a_operands[0])) {
					id->opcode = a_opcode;
					id->storage = a_operands[1];
					id->type = a_operands[2];
				}
				break;
			case kOpVariable:
				if (auto* id = at(a_operands[1])) {
					id->opcode = a_opcode;
					id->type = a_operands[0];
					id->storage = a_operands[2];
				}
				break;
			default:
				break;
			}
		}

		void ReadInstruction(std::vector<Id>& a_ids, std::uint16_t a_opcode, std::uint16_t a_count, const std::uint32_t* a_operands)
		{
			switch (a_opcode) {
			case kOpExtInstImport:
			case kOpName:
			case kOpMemberName:
				ReadName(a_ids, a_opcode, a_count, a_operands);
				break;
			case kOpMemberDecorate:
			case kOpDecorate:
				ReadDecoration(a_ids, a_opcode, a_count, a_operands);
				break;
			case kOpTypeImage:
			case kOpTypeSampler:
			case kOpTypeSampledImage:
			case kOpTypeArray:
			case kOpTypeRuntimeArray:
			case kOpTypePointer:
			case kOpVariable:
				ReadType(a_ids, a_opcode, a_operands);
				break;
			default:
				break;
			}
		}

		// A stage input: its semantic and index from DXC's name ("in.var.TEXCOORD1").
		void ReadInput(const Id& a_variable, std::vector<SpirvReflection::Input>& a_inputs)
		{
			constexpr std::string_view kPrefix = "in.var.";
			if (a_variable.builtIn || a_variable.location == kNone || !a_variable.name.starts_with(kPrefix))
				return;
			std::string semantic = a_variable.name.substr(kPrefix.size());
			std::size_t digits = semantic.size();
			while (digits > 0 && std::isdigit(static_cast<unsigned char>(semantic[digits - 1])))
				--digits;
			const std::uint32_t index = digits < semantic.size() ? static_cast<std::uint32_t>(std::stoul(semantic.substr(digits))) : 0;
			semantic.resize(digits);
			a_inputs.push_back({ std::move(semantic), index, a_variable.location });
		}

		// A constant buffer's members, whether the code reads them or not: the layout is the declaration's.
		void ReadBlockMembers(const std::vector<Id>& a_ids, const Id& a_variable, std::vector<SpirvReflection::BlockMember>& a_blockMembers)
		{
			const auto at = [&](std::uint32_t a_id) { return At(a_ids, a_id); };
			const Id* pointer = at(a_variable.type);
			if (const Id* block = pointer ? at(pointer->type) : nullptr) {
				for (std::size_t m = 0; m < block->memberNames.size(); ++m)
					if (!block->memberNames[m].empty() && m < block->memberOffsets.size() && block->memberOffsets[m] != kNone)
						a_blockMembers.push_back({ a_variable.name, block->memberNames[m], block->memberOffsets[m] });
			}
		}

		// The kind of a used binding.
		SpirvReflection::BindingKind KindOf(const std::vector<Id>& a_ids, const Id& a_variable)
		{
			using BindingKind = SpirvReflection::BindingKind;
			const auto at = [&](std::uint32_t a_id) { return At(a_ids, a_id); };
			// The type a variable's pointer points at, through resource arrays.
			auto elementOpcode = [&](std::uint32_t a_pointer) -> std::uint16_t {
				const Id* pointer = at(a_pointer);
				const Id* type = pointer ? at(pointer->type) : nullptr;
				while (type && (type->opcode == kOpTypeArray || type->opcode == kOpTypeRuntimeArray))
					type = at(type->type);
				return type ? type->opcode : 0;
			};
			BindingKind kind = BindingKind::Other;
			if (a_variable.storage == kStorageUniform)
				kind = BindingKind::ConstantBuffer;
			else if (a_variable.storage == kStorageStorageBuffer)
				kind = BindingKind::StorageBuffer;
			else if (a_variable.storage == kStorageUniformConstant) {
				const auto opcode = elementOpcode(a_variable.type);
				kind = opcode == kOpTypeSampler ? BindingKind::Sampler : (opcode == kOpTypeImage ? BindingKind::Texture : BindingKind::Other);
			}
			return kind;
		}
	}

	bool SpirvReflection::Parse(std::span<const std::byte> a_spirv)
	{
		inputs.clear();
		bindings.clear();
		blockMembers.clear();
		outputLocations = 0;
		if (a_spirv.size() < kHeaderWords * 4 || a_spirv.size() % 4 != 0)
			return false;
		std::vector<std::uint32_t> words(a_spirv.size() / 4);
		std::memcpy(words.data(), a_spirv.data(), a_spirv.size());
		if (words[0] != kMagic)
			return false;
		std::vector<Id> ids(words[3]);  // header: id bound
		bool inFunction = false;

		for (std::size_t offset = kHeaderWords; offset < words.size();) {
			const std::uint16_t count = static_cast<std::uint16_t>(words[offset] >> 16);
			const std::uint16_t opcode = static_cast<std::uint16_t>(words[offset] & 0xFFFF);
			if (count == 0 || offset + count > words.size())
				return false;
			const std::uint32_t* operands = &words[offset + 1];
			MarkReferenced(ids, opcode, count, operands, inFunction);
			ReadInstruction(ids, opcode, count, operands);
			offset += count;
		}

		for (const auto& id : ids) {
			if (id.opcode != kOpVariable)
				continue;
			if (id.storage == kStorageInput) {
				ReadInput(id, inputs);
				continue;
			}
			if (id.storage == kStorageOutput) {
				if (!id.builtIn && id.location < 32)
					outputLocations |= 1u << id.location;
				continue;
			}
			if (id.storage == kStorageUniform && !id.name.empty())
				ReadBlockMembers(ids, id, blockMembers);
			if (id.binding == kNone || !id.referenced)
				continue;
			bindings.push_back({ id.set, id.binding, KindOf(ids, id) });
		}
		return true;
	}
}
