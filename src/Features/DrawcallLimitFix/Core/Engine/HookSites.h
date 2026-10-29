#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace DCLF::HookSites
{
	/**
	 * @brief DCLF's engine hook sites, checked before any hook is written (DrawcallLimitFix::PostPostLoad's preflight).
	 *
	 * Another plugin may already have hooked a site DCLF hooks. A hook that chains (its thunk always calls what the site held, with the
	 * same arguments) takes whatever is there: only a site that is no longer a CALL rel32 (an inline patch) stops it. A hook that does not
	 * chain (it skips or replaces the original for some calls, or reads more of the engine's callee than its signature) needs the engine's
	 * own code there: a conflict at one leaves DCLF off, with an error naming the site and the module that holds it, rather than half
	 * installed (a producer whose consumer never went in: the scene tables' write lists that stalled the retirement chain behind
	 * ShadowBoost's Main::Update hook).
	 *
	 * A site held by Community Shaders' own code (SceneWriterFramework's detours, another CS feature's hook) is not a conflict: DCLF's
	 * layering over those is reviewed in-tree (a hook that must yield to one, as the point lights' category filter to Grass
	 * Optimizations' NiNode::OnVisible, checks it at its own install).
	 *
	 * The Require calls record conflicts; Conflicts() reads them. Main thread, at PostPostLoad.
	 */

	/** @brief A chained CALL rel32: any call target is taken (another plugin's hook runs, and through it the engine's). */
	void RequireCall(std::string_view a_name, std::uintptr_t a_site);
	/** @brief A chained JMP rel32 (a tail call), likewise. */
	void RequireJump(std::string_view a_name, std::uintptr_t a_site);
	/**
	 * @brief A hook that does not chain on a CALL rel32: the call must still reach the engine's a_target (0: any function in the game's
	 * image, where the callee has no address of its own here).
	 */
	void RequireCallTo(std::string_view a_name, std::uintptr_t a_site, std::uintptr_t a_target = 0);
	/** @brief A hook that does not chain on a JMP rel32: the jump must still reach the engine's a_target (0: the game's image). */
	void RequireJumpTo(std::string_view a_name, std::uintptr_t a_site, std::uintptr_t a_target = 0);
	/**
	 * @brief A vtable slot a hook that does not chain takes: it must still hold the engine's function (a_engine; 0: any function in the
	 * game's image).
	 */
	void RequireSlot(std::string_view a_name, std::uintptr_t a_vtable, std::size_t a_index, std::uintptr_t a_engine = 0);
	/** @brief A function a detour that does not chain takes: its entry must not already jump elsewhere (another plugin's detour). */
	void RequireEntry(std::string_view a_name, std::uintptr_t a_function);
	/** @brief Engine bytes a hook writes over (an inline patch): they must be as reverse engineered. */
	void RequireBytes(std::string_view a_name, std::uintptr_t a_site, std::span<const std::uint8_t> a_bytes);
	/** @brief A site whose address this runtime lacks, where the hook is required. */
	void RequireKnown(std::string_view a_name, bool a_known);

	/** @brief After install: a hook that failed to go in although its site passed (a detour's attach). */
	void NoteFailed(std::string_view a_name, std::string_view a_why);
	/** @brief A detour's install result: its func is 0 when the attach failed (stl::detour_thunk). */
	inline void NoteDetour(std::string_view a_name, std::uintptr_t a_func)
	{
		if (!a_func)
			NoteFailed(a_name, "the detour did not attach");
	}

	/** @brief The conflicts recorded so far, one line each ("site: why"); empty when none. */
	std::string Conflicts();
	/** @brief How many sites were checked (the log's line). */
	std::size_t Checked();

	/** @brief Where a_address leads: through JMP rel32s and absolute-jump trampolines (FF 25: a plugin's or Detours' branch stub). */
	std::uintptr_t Resolve(std::uintptr_t a_address);
	/** @brief The module Resolve(a_address) lies in. */
	std::string ModuleOf(std::uintptr_t a_address);
	/** @brief The module the CALL or JMP rel32 at a_site leads to (ModuleOf its target). */
	std::string BranchTargetModule(std::uintptr_t a_site);
}
