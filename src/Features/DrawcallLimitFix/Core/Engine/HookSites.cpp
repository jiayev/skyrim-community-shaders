#include "HookSites.h"

#include <cstring>
#include <vector>

namespace DCLF::HookSites
{
	namespace
	{
		struct State
		{
			std::vector<std::string> conflicts;
			std::size_t checked = 0;
		};
		State& Get()
		{
			static State state;
			return state;
		}

		void Conflict(std::string_view a_name, std::string a_why)
		{
			Get().conflicts.push_back(fmt::format("{}: {}", a_name, a_why));
		}

		std::uintptr_t BranchTarget(std::uintptr_t a_site)
		{
			std::int32_t displacement = 0;
			std::memcpy(&displacement, reinterpret_cast<const void*>(a_site + 1), sizeof(displacement));
			return a_site + 5 + static_cast<std::intptr_t>(displacement);
		}

		// In the game's code (its .text): an engine function, not another module's hook.
		bool InGame(std::uintptr_t a_address)
		{
			const auto text = REL::Module::get().segment(REL::Segment::textx);
			return a_address >= text.address() && a_address < text.address() + text.size();
		}

		HMODULE ModuleAt(std::uintptr_t a_address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a_address), &module))
				return nullptr;
			return module;
		}

		// Community Shaders' own code: its hooks (SceneWriterFramework's, another feature's) are not another plugin's.
		bool Own(std::uintptr_t a_address)
		{
			static const HMODULE self = ModuleAt(reinterpret_cast<std::uintptr_t>(&ModuleAt));
			return self && ModuleAt(Resolve(a_address)) == self;
		}

		// A rel32 branch with opcode a_opcode at a_site (0xE8 CALL, 0xE9 JMP); empty when it is one, else why not.
		std::string Branch(std::uintptr_t a_site, std::uint8_t a_opcode)
		{
			if (!a_site)
				return "its address is not known on this runtime";
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(a_site);
			if (opcode == a_opcode)
				return {};
			return fmt::format("not a {} (opcode {:02X}: patched inline, by {})", a_opcode == 0xE8 ? "CALL rel32" : "JMP rel32", opcode,
				opcode == 0xE8 || opcode == 0xE9 ? BranchTargetModule(a_site) : "an unknown module");
		}

		void Target(std::string_view a_name, std::uintptr_t a_site, std::uint8_t a_opcode, std::uintptr_t a_target)
		{
			++Get().checked;
			if (auto why = Branch(a_site, a_opcode); !why.empty())
				Conflict(a_name, std::move(why));
			else if (a_target ? BranchTarget(a_site) != a_target && !Own(BranchTarget(a_site)) : !InGame(BranchTarget(a_site)) && !Own(BranchTarget(a_site)))
				Conflict(a_name, fmt::format("already hooked by {} (this hook does not chain)", BranchTargetModule(a_site)));
		}
	}

	void RequireCall(std::string_view a_name, std::uintptr_t a_site)
	{
		++Get().checked;
		if (auto why = Branch(a_site, 0xE8); !why.empty())
			Conflict(a_name, std::move(why));
	}

	void RequireJump(std::string_view a_name, std::uintptr_t a_site)
	{
		++Get().checked;
		if (auto why = Branch(a_site, 0xE9); !why.empty())
			Conflict(a_name, std::move(why));
	}

	void RequireCallTo(std::string_view a_name, std::uintptr_t a_site, std::uintptr_t a_target) { Target(a_name, a_site, 0xE8, a_target); }

	void RequireJumpTo(std::string_view a_name, std::uintptr_t a_site, std::uintptr_t a_target) { Target(a_name, a_site, 0xE9, a_target); }

	void RequireSlot(std::string_view a_name, std::uintptr_t a_vtable, std::size_t a_index, std::uintptr_t a_engine)
	{
		++Get().checked;
		if (!a_vtable) {
			Conflict(a_name, "the vtable's address is not known on this runtime");
			return;
		}
		const auto held = reinterpret_cast<const std::uintptr_t*>(a_vtable)[a_index];
		if (a_engine ? held != a_engine && !Own(held) : !InGame(held) && !Own(held))
			Conflict(a_name, fmt::format("vtable slot {:#x} already hooked by {} (this hook does not chain)", a_index, ModuleOf(held)));
	}

	void RequireEntry(std::string_view a_name, std::uintptr_t a_function)
	{
		++Get().checked;
		if (!a_function) {
			Conflict(a_name, "its address is not known on this runtime");
			return;
		}
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_function);
		if ((bytes[0] == 0xE9 || (bytes[0] == 0xFF && bytes[1] == 0x25)) && !Own(a_function))
			Conflict(a_name, fmt::format("its entry already jumps to {} (this detour does not chain)", ModuleOf(a_function)));
	}

	void RequireBytes(std::string_view a_name, std::uintptr_t a_site, std::span<const std::uint8_t> a_bytes)
	{
		++Get().checked;
		if (!a_site)
			Conflict(a_name, "its address is not known on this runtime");
		else if (std::memcmp(reinterpret_cast<const void*>(a_site), a_bytes.data(), a_bytes.size()) != 0) {
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(a_site);
			Conflict(a_name, fmt::format("its code is not the engine's (patched{})",
								 opcode == 0xE8 || opcode == 0xE9 ? fmt::format(" by {}", BranchTargetModule(a_site)) : std::string{}));
		}
	}

	void RequireKnown(std::string_view a_name, bool a_known)
	{
		++Get().checked;
		if (!a_known)
			Conflict(a_name, "its address is not known on this runtime");
	}

	void NoteFailed(std::string_view a_name, std::string_view a_why) { Conflict(a_name, std::string(a_why)); }

	std::string Conflicts()
	{
		std::string out;
		for (const auto& conflict : Get().conflicts)
			out += (out.empty() ? "" : "; ") + conflict;
		return out;
	}

	std::size_t Checked() { return Get().checked; }

	std::uintptr_t Resolve(std::uintptr_t a_address)
	{
		// A few hops at most: a JMP rel32 into a branch stub, the stub's jmp [rip+disp] to the hook.
		for (int hop = 0; hop < 4 && a_address; ++hop) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			if (bytes[0] == 0xE9)
				a_address = BranchTarget(a_address);
			else if (bytes[0] == 0xFF && bytes[1] == 0x25) {
				std::int32_t displacement = 0;
				std::memcpy(&displacement, bytes + 2, sizeof(displacement));
				std::memcpy(&a_address, reinterpret_cast<const void*>(a_address + 6 + static_cast<std::intptr_t>(displacement)), sizeof(a_address));
			} else
				break;
		}
		return a_address;
	}

	std::string ModuleOf(std::uintptr_t a_address)
	{
		a_address = Resolve(a_address);
		HMODULE module = ModuleAt(a_address);
		char path[MAX_PATH]{};
		if (module && GetModuleFileNameA(module, path, MAX_PATH)) {
			const std::string_view name(path);
			const auto cut = name.find_last_of("\\/");
			return std::string(cut == std::string_view::npos ? name : name.substr(cut + 1));
		}
		return fmt::format("{:#x} (no module)", a_address);
	}

	std::string BranchTargetModule(std::uintptr_t a_site) { return ModuleOf(BranchTarget(a_site)); }
}
