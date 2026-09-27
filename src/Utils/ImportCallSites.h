#pragma once

#include <Windows.h>
#include <cstddef>

namespace Util
{
	/**
	 * @brief Point every `call`/`jmp qword ptr [rip+X]` in a module's code that reads the IAT slot of
	 * `a_dll!a_function` at a slot owned by CS instead, holding `a_replacement`.
	 *
	 * An IAT patch is only as durable as the IAT. Capture tools that hook the same import rewrite it:
	 * RenderDoc, loaded in-process by the RenderDoc feature, re-hooks every loaded module after each
	 * LoadLibrary and overwrites any d3d11.dll/dxgi.dll slot that does not already point at its own hook.
	 * The game then calls RenderDoc, RenderDoc calls the *system* runtime, and DXVK is loaded but never
	 * used. Redirected call sites no longer read the IAT, so nothing that rewrites it can take them back.
	 *
	 * The slot is found from the module's import descriptors and its references by scanning executable
	 * sections for rip-relative displacements that resolve to it, so no per-version address is needed.
	 * Every reference that is not a call or jmp (a `mov reg, [slot]`, say) is left alone and counted in
	 * the log, because it still reads whatever the IAT holds.
	 *
	 * @return the number of call sites redirected; 0 when the import or its call sites are missing.
	 */
	std::size_t RedirectImportCallSites(HMODULE a_module, const char* a_dll, const char* a_function, void* a_replacement);
}
