#include "ImportCallSites.h"

#include <cstring>
#include <vector>

namespace Util
{
	namespace
	{
		void** FindImportSlot(std::byte* a_base, const char* a_dll, const char* a_function)
		{
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(a_base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!dir.VirtualAddress)
				return nullptr;

			for (auto desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(a_base + dir.VirtualAddress); desc->Name; ++desc) {
				if (_stricmp(reinterpret_cast<const char*>(a_base + desc->Name), a_dll) != 0 || !desc->OriginalFirstThunk)
					continue;
				const auto names = reinterpret_cast<const IMAGE_THUNK_DATA*>(a_base + desc->OriginalFirstThunk);
				const auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(a_base + desc->FirstThunk);
				for (std::size_t i = 0; names[i].u1.AddressOfData; ++i) {
					if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
						continue;
					const auto byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(a_base + names[i].u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(byName->Name), a_function) == 0)
						return reinterpret_cast<void**>(&slots[i].u1.Function);
				}
			}
			return nullptr;
		}

		// rip-relative displacements are 32-bit, so the replacement slots must sit within 2 GiB of the code.
		void** AllocateNearbySlot(std::byte* a_base, std::size_t a_imageSize)
		{
			static void** s_page = nullptr;
			static std::size_t s_used = 0;
			constexpr std::size_t kSlotsPerPage = 0x1000 / sizeof(void*);

			if (!s_page || s_used == kSlotsPerPage) {
				SYSTEM_INFO info{};
				::GetSystemInfo(&info);
				const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
				const auto base = reinterpret_cast<std::uintptr_t>(a_base);
				const auto end = base + a_imageSize;
				s_page = nullptr;
				// Just below the image first, then just above: both stay in reach of every byte of it.
				for (std::uintptr_t offset = granularity; offset < 0x70000000 && !s_page; offset += granularity) {
					for (const std::uintptr_t candidate : { (base - offset) & ~(granularity - 1), (end + offset) & ~(granularity - 1) }) {
						if (auto page = ::VirtualAlloc(reinterpret_cast<void*>(candidate), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
							s_page = static_cast<void**>(page);
							break;
						}
					}
				}
				s_used = 0;
				if (!s_page)
					return nullptr;
			}
			return &s_page[s_used++];
		}
	}

	std::size_t RedirectImportCallSites(HMODULE a_module, const char* a_dll, const char* a_function, void* a_replacement)
	{
		const auto base = reinterpret_cast<std::byte*>(a_module);
		void** const iatSlot = FindImportSlot(base, a_dll, a_function);
		if (!iatSlot) {
			logger::warn("[Imports] {}!{} is not imported; call sites left as they are", a_dll, a_function);
			return 0;
		}

		const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
		void** const ourSlot = AllocateNearbySlot(base, nt->OptionalHeader.SizeOfImage);
		if (!ourSlot) {
			logger::warn("[Imports] no memory within 2 GiB of the module for {}!{}; call sites left on the IAT", a_dll, a_function);
			return 0;
		}
		*ourSlot = a_replacement;

		const auto target = reinterpret_cast<std::intptr_t>(iatSlot);
		std::vector<std::byte*> callSites;
		std::size_t otherReferences = 0;

		auto section = IMAGE_FIRST_SECTION(nt);
		for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
			if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;
			std::byte* const begin = base + section->VirtualAddress;
			const std::size_t size = std::min<std::size_t>(section->Misc.VirtualSize, section->SizeOfRawData ? section->SizeOfRawData : section->Misc.VirtualSize);
			for (std::size_t i = 2; i + 4 <= size; ++i) {
				std::int32_t disp;
				std::memcpy(&disp, begin + i, sizeof(disp));
				// A rip-relative operand ending the instruction resolves against the next instruction.
				if (reinterpret_cast<std::intptr_t>(begin + i + 4) + disp != target)
					continue;
				const auto op = std::to_integer<unsigned>(begin[i - 2]);
				const auto modrm = std::to_integer<unsigned>(begin[i - 1]);
				if (op == 0xFF && (modrm == 0x15 || modrm == 0x25))
					callSites.push_back(begin + i);
				else
					++otherReferences;
			}
		}

		for (std::byte* const dispAddress : callSites) {
			const auto newDisp = reinterpret_cast<std::intptr_t>(ourSlot) - reinterpret_cast<std::intptr_t>(dispAddress + 4);
			const auto disp = static_cast<std::int32_t>(newDisp);
			DWORD oldProtect = 0;
			::VirtualProtect(dispAddress, sizeof(disp), PAGE_EXECUTE_READWRITE, &oldProtect);
			std::memcpy(dispAddress, &disp, sizeof(disp));
			::VirtualProtect(dispAddress, sizeof(disp), oldProtect, &oldProtect);
		}
		if (!callSites.empty())
			::FlushInstructionCache(::GetCurrentProcess(), nullptr, 0);

		const auto current = *iatSlot;
		logger::info("[Imports] {}!{}: {} call site(s) redirected off the IAT{}; the IAT holds {}{}",
			a_dll, a_function, callSites.size(),
			otherReferences ? std::format(", {} other reference(s) still read it", otherReferences) : std::string{},
			fmt::ptr(current), current == a_replacement ? " (ours)" : "");
		return callSites.size();
	}
}
