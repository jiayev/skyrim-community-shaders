#include "HookSites.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <vector>

#include <tlhelp32.h>

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

		constexpr DWORD kExecutable = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		constexpr DWORD kReadable = kExecutable | PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY;

		// Whether a_size bytes at a_address are committed with one of a_protections. The report reads wherever a byte pattern points, so it
		// asks first: a pattern found by chance must not crash the game.
		bool Accessible(std::uintptr_t a_address, std::size_t a_size, DWORD a_protections)
		{
			const auto end = a_address + a_size;
			while (a_address < end) {
				MEMORY_BASIC_INFORMATION info{};
				if (!VirtualQuery(reinterpret_cast<LPCVOID>(a_address), &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || !(info.Protect & a_protections))
					return false;
				a_address = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
			}
			return true;
		}

		// As Resolve, asking before each read. 0 when the path leaves code, or does not end in a few hops.
		std::uintptr_t ResolveChecked(std::uintptr_t a_address)
		{
			for (int hop = 0; hop < 4; ++hop) {
				if (!Accessible(a_address, 6, kExecutable))
					return 0;
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
				if (bytes[0] == 0xE9)
					a_address = BranchTarget(a_address);
				else if (bytes[0] == 0xFF && bytes[1] == 0x25) {
					std::int32_t displacement = 0;
					std::memcpy(&displacement, bytes + 2, sizeof(displacement));
					const auto slot = a_address + 6 + static_cast<std::intptr_t>(displacement);
					if (!Accessible(slot, sizeof(a_address), kReadable))
						return 0;
					std::memcpy(&a_address, reinterpret_cast<const void*>(slot), sizeof(a_address));
				} else
					return a_address;
			}
			return 0;
		}

		std::string HexBytes(std::uintptr_t a_begin, std::uintptr_t a_end, std::uintptr_t a_marked = 0)
		{
			std::string out;
			for (auto address = a_begin; address < a_end; ++address) {
				if (!out.empty())
					out += ' ';
				const auto text = Accessible(address, 1, kReadable) ? fmt::format("{:02X}", *reinterpret_cast<const std::uint8_t*>(address)) : std::string("??");
				out += address == a_marked ? fmt::format("[{}]", text) : text;
			}
			return out;
		}

		struct NearbyJump
		{
			const char* kind = nullptr;
			std::size_t length = 0;
			std::uintptr_t target = 0;
		};

		// The call or jump whose encoding starts at a_address, if there is one. A patch's: an engine call's target is in the game.
		NearbyJump JumpAt(std::uintptr_t a_address)
		{
			constexpr std::size_t kLongest = 13;
			if (!Accessible(a_address, kLongest, kReadable))
				return {};
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			if (bytes[0] == 0xE8 || bytes[0] == 0xE9)
				return { bytes[0] == 0xE8 ? "CALL rel32" : "JMP rel32", 5, BranchTarget(a_address) };
			if (bytes[0] == 0xFF && (bytes[1] == 0x15 || bytes[1] == 0x25)) {
				std::int32_t displacement = 0;
				std::memcpy(&displacement, bytes + 2, sizeof(displacement));
				const auto slot = a_address + 6 + static_cast<std::intptr_t>(displacement);
				if (!Accessible(slot, sizeof(std::uintptr_t), kReadable))
					return {};
				std::uintptr_t target = 0;
				std::memcpy(&target, reinterpret_cast<const void*>(slot), sizeof(target));
				// Displacement 0: the target's 8 bytes follow inline, and are part of the patch.
				return { bytes[1] == 0x15 ? "CALL [rip]" : "JMP [rip]", displacement == 0 ? 14u : 6u, target };
			}
			// mov reg, imm64, then call or jmp reg: 48 B8+r imm64 FF D0+r / E0+r, or 49 B8+r imm64 41 FF D0+r / E0+r for r8-r15.
			if ((bytes[0] == 0x48 || bytes[0] == 0x49) && bytes[1] >= 0xB8 && bytes[1] <= 0xBF) {
				const std::uint8_t reg = bytes[1] - 0xB8;
				const std::size_t branch = bytes[0] == 0x49 ? 11 : 10;
				if ((bytes[0] == 0x48 || bytes[10] == 0x41) && bytes[branch] == 0xFF && (bytes[branch + 1] == 0xD0 + reg || bytes[branch + 1] == 0xE0 + reg)) {
					std::uintptr_t target = 0;
					std::memcpy(&target, bytes + 2, sizeof(target));
					return { bytes[branch + 1] >= 0xE0 ? "JMP imm64" : "CALL imm64", branch + 2, target };
				}
			}
			return {};
		}

		// How a plugin that patches a place in the engine usually names it: the function's Address Library id with the offset inside it, or
		// the address itself. A patch can start before the site (a site inside a longer patch), so these allow kNameReach either way. Not
		// the module offset: 32 bits in that wide a range turn up by chance in a large module.
		constexpr std::int64_t kNameReach = 32;
		struct SiteNames
		{
			std::uint32_t id = 0;  // 0: no function near the site in the Address Library
			std::int64_t offset = 0;
			std::uint32_t moduleOffset = 0;
			std::uint64_t preferredAddress = 0;
		};
		struct NameHits
		{
			std::uint32_t ids = 0, idsWithOffset = 0, addresses = 0;
			std::int64_t offsetSeen = -1;
		};

		SiteNames NamesOf(std::uintptr_t a_site)
		{
			constexpr std::uint64_t kImagePreferredBase = 0x140000000;
			constexpr std::int64_t kLongestFunction = 0x4000;
			SiteNames names;
			const auto moduleOffset = a_site - REL::Module::get().base();
			names.moduleOffset = static_cast<std::uint32_t>(moduleOffset);
			names.preferredAddress = kImagePreferredBase + moduleOffset;
			static const REL::Offset2ID table;
			auto after = std::upper_bound(table.begin(), table.end(), moduleOffset, [](std::uint64_t a_offset, const auto& a_entry) { return a_offset < a_entry.offset; });
			if (after != table.begin()) {
				--after;
				const auto offset = static_cast<std::int64_t>(moduleOffset - after->offset);
				if (offset < kLongestFunction && after->id < (1ull << 32)) {
					names.id = static_cast<std::uint32_t>(after->id);
					names.offset = offset;
				}
			}
			return names;
		}

		void CountNames(const std::uint8_t* a_bytes, std::size_t a_size, const SiteNames& a_names, NameHits& a_hits)
		{
			if (a_size < sizeof(std::uint64_t))
				return;
			constexpr std::size_t kPairReach = 48;
			const auto lowPreferred = a_names.preferredAddress - static_cast<std::uint64_t>(kNameReach);
			for (std::size_t at = 0; at + sizeof(std::uint64_t) <= a_size; ++at) {
				std::uint32_t value = 0;
				std::memcpy(&value, a_bytes + at, sizeof(value));
				if (value == a_names.moduleOffset) {
					++a_hits.addresses;
					continue;
				}
				std::uint64_t wide = 0;
				std::memcpy(&wide, a_bytes + at, sizeof(wide));
				if (wide - lowPreferred <= 2 * kNameReach) {
					++a_hits.addresses;
					continue;
				}
				if (!a_names.id || value != a_names.id)
					continue;
				++a_hits.ids;
				const auto first = at > kPairReach ? at - kPairReach : 0;
				const auto last = (std::min)(a_size - sizeof(std::uint16_t), at + kPairReach);
				for (auto probe = first; probe <= last; ++probe) {
					if (probe + sizeof(std::uint16_t) > at && probe < at + sizeof(value))
						continue;
					std::uint16_t offset = 0;
					std::memcpy(&offset, a_bytes + probe, sizeof(offset));
					if (std::abs(static_cast<std::int64_t>(offset) - a_names.offset) <= kNameReach) {
						++a_hits.idsWithOffset;
						a_hits.offsetSeen = offset;
						break;
					}
				}
			}
		}

		// A module can be unloaded under the scan, and the scan only exists to explain a failure: reading it must not add a crash.
		void CountNamesGuarded(const std::uint8_t* a_bytes, std::size_t a_size, const SiteNames& a_names, NameHits& a_hits)
		{
			__try {
				CountNames(a_bytes, a_size, a_names, a_hits);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		NameHits ScanImage(std::uintptr_t a_base, std::size_t a_size, const SiteNames& a_names)
		{
			NameHits hits;
			for (auto address = a_base; address < a_base + a_size;) {
				MEMORY_BASIC_INFORMATION info{};
				if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)))
					break;
				const auto regionEnd = (std::min)(a_base + a_size, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
				if (info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) && (info.Protect & kReadable))
					CountNamesGuarded(reinterpret_cast<const std::uint8_t*>(address), regionEnd - address, a_names, hits);
				address = regionEnd;
			}
			return hits;
		}

		// The loaded plugins whose image names the site's code. A lead, not proof: a plugin can name a place and not patch it, and one that
		// finds its place by a byte signature names nothing.
		void LogModulesNaming(std::uintptr_t a_site)
		{
			constexpr int kMostScans = 4;
			constexpr std::size_t kMostLines = 8;
			constexpr std::uint32_t kNoisyIds = 16;  // a bare id this common is just a small number in the module's data
			static int scans = 0;
			if (++scans > kMostScans) {
				if (scans == kMostScans + 1)
					logger::error("[DCLF]   the loaded modules were scanned for the first {} conflicts only", kMostScans);
				return;
			}
			const auto names = NamesOf(a_site);
			if (names.id)
				logger::error("[DCLF]   the site is Address Library id {} + {:#x}", names.id, names.offset);
			const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (snapshot == INVALID_HANDLE_VALUE) {
				logger::error("[DCLF]   the loaded modules could not be listed (error {})", GetLastError());
				return;
			}
			wchar_t system[MAX_PATH]{};
			const auto systemLength = GetWindowsDirectoryW(system, MAX_PATH);
			const auto self = ModuleAt(reinterpret_cast<std::uintptr_t>(&ModuleAt));
			const auto game = reinterpret_cast<HMODULE>(REL::Module::get().base());
			struct Lead
			{
				HMODULE module;
				NameHits hits;
			};
			std::vector<Lead> leads;
			MODULEENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			if (Module32FirstW(snapshot, &entry)) {
				do {
					if (entry.hModule == self || entry.hModule == game || (systemLength && _wcsnicmp(entry.szExePath, system, systemLength) == 0))
						continue;
					const auto hits = ScanImage(reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize, names);
					if (hits.idsWithOffset || hits.addresses || (hits.ids && hits.ids <= kNoisyIds))
						leads.push_back({ entry.hModule, hits });
				} while (Module32NextW(snapshot, &entry));
			}
			CloseHandle(snapshot);
			std::ranges::stable_sort(leads, [](const Lead& a_left, const Lead& a_right) {
				return std::tuple(a_left.hits.idsWithOffset, a_left.hits.addresses) > std::tuple(a_right.hits.idsWithOffset, a_right.hits.addresses);
			});
			if (leads.empty())
				logger::error("[DCLF]   no other loaded module names this code (one that finds it by a byte signature would not show here)");
			else
				logger::error("[DCLF]   loaded modules that name this code (leads, not proof):");
			for (std::size_t i = 0; i < leads.size() && i < kMostLines; ++i) {
				const auto& hits = leads[i].hits;
				logger::error("[DCLF]     {}: id x{}, id beside an offset x{}{}, address x{}", ModuleOf(reinterpret_cast<std::uintptr_t>(leads[i].module)), hits.ids, hits.idsWithOffset,
					hits.offsetSeen >= 0 ? fmt::format(" ({:#x})", hits.offsetSeen) : std::string{}, hits.addresses);
			}
			if (leads.size() > kMostLines)
				logger::error("[DCLF]     and {} more", leads.size() - kMostLines);
		}

		// What a person needs to tell a patch from a wrong address, in the log, as a conflict's text has to stay short: the game's version,
		// the site's address and bytes, and each call or jump around it that leaves the game's code. A first byte alone says little: the site
		// can sit inside a patch that starts before it.
		void LogSite(std::string_view a_name, std::uintptr_t a_site)
		{
			constexpr std::uintptr_t kReach = 32;
			constexpr int kMostJumps = 8;
			const auto& game = REL::Module::get();
			logger::error("[DCLF] hook conflict at {}: game {}, address {:#x} (module offset {:#x})", a_name, game.version().string("."), a_site, a_site - game.base());
			logger::error("[DCLF]   bytes from 32 before the site, the site's in brackets: {}", HexBytes(a_site - kReach, a_site + kReach, a_site));
			int jumps = 0;
			for (auto address = a_site - kReach; address < a_site + kReach && jumps < kMostJumps; ++address) {
				const auto jump = JumpAt(address);
				const auto leads = jump.kind ? ResolveChecked(jump.target) : 0;
				if (!leads || reinterpret_cast<std::uintptr_t>(ModuleAt(leads)) == game.base())
					continue;
				++jumps;
				const bool covers = address <= a_site && a_site < address + jump.length;
				const auto offset = static_cast<std::intptr_t>(address) - static_cast<std::intptr_t>(a_site);
				logger::error("[DCLF]   at site{:+d}: {} to {}{}{}", offset, jump.kind, ModuleOf(leads), covers ? ", covering the site" : "",
					ModuleAt(leads) ? std::string{} : fmt::format(", which starts {}", HexBytes(leads, leads + 32)));
			}
			if (!jumps)
				logger::error("[DCLF]   no call or jump near the site leaves the game's code");
			LogModulesNaming(a_site);
		}

		void SiteConflict(std::string_view a_name, std::uintptr_t a_site, std::string a_why)
		{
			Conflict(a_name, std::move(a_why));
			if (a_site)
				LogSite(a_name, a_site);
		}

		// A rel32 branch with opcode a_opcode at a_site (0xE8 CALL, 0xE9 JMP); empty when it is one, else why not.
		std::string Branch(std::uintptr_t a_site, std::uint8_t a_opcode)
		{
			if (!a_site)
				return "its address is not known on this runtime";
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(a_site);
			if (opcode == a_opcode)
				return {};
			const auto* expected = a_opcode == 0xE8 ? "CALL rel32" : "JMP rel32";
			if (opcode == 0xE8 || opcode == 0xE9)
				return fmt::format("not a {} (opcode {:02X}: patched inline, by {})", expected, opcode, BranchTargetModule(a_site));
			return fmt::format("not a {} (opcode {:02X}: unexpected code, the log's next lines have the site's bytes)", expected, opcode);
		}

		void Target(std::string_view a_name, std::uintptr_t a_site, std::uint8_t a_opcode, std::uintptr_t a_target)
		{
			++Get().checked;
			if (auto why = Branch(a_site, a_opcode); !why.empty())
				SiteConflict(a_name, a_site, std::move(why));
			else if (a_target ? BranchTarget(a_site) != a_target && !Own(BranchTarget(a_site)) : !InGame(BranchTarget(a_site)) && !Own(BranchTarget(a_site)))
				SiteConflict(a_name, a_site, fmt::format("already hooked by {} (this hook does not chain)", BranchTargetModule(a_site)));
		}
	}

	void RequireCall(std::string_view a_name, std::uintptr_t a_site)
	{
		++Get().checked;
		if (auto why = Branch(a_site, 0xE8); !why.empty())
			SiteConflict(a_name, a_site, std::move(why));
	}

	void RequireJump(std::string_view a_name, std::uintptr_t a_site)
	{
		++Get().checked;
		if (auto why = Branch(a_site, 0xE9); !why.empty())
			SiteConflict(a_name, a_site, std::move(why));
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
			SiteConflict(a_name, a_function, fmt::format("its entry already jumps to {} (this detour does not chain)", ModuleOf(a_function)));
	}

	void RequireBytes(std::string_view a_name, std::uintptr_t a_site, std::span<const std::uint8_t> a_bytes)
	{
		++Get().checked;
		if (!a_site)
			Conflict(a_name, "its address is not known on this runtime");
		else if (std::memcmp(reinterpret_cast<const void*>(a_site), a_bytes.data(), a_bytes.size()) != 0) {
			const auto opcode = *reinterpret_cast<const std::uint8_t*>(a_site);
			SiteConflict(a_name, a_site, fmt::format("its code is not the engine's (patched{})",
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
