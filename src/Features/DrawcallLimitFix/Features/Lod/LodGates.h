#pragma once

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Features::Lod::LodGates
{
	/**
	 * @brief The engine's LOD swaps gated on DCLF's readiness (CS_DCLF_LOD_GATES, on by default).
	 *
	 * The object and terrain Upgrade/Downgrade Func2s (AE 1.6.1170; skyrim-engine-notes.md, "LOD swaps and their gates") attach the
	 * incoming blocks and retire the outgoing in one call, on a job thread or the main thread, under the terrain manager's lock; the
	 * InitialLoad Func2s attach a node's first level where nothing above it is loaded (a gate with no outgoing; under a load screen
	 * they pass through: the full rebuild flushed every gate and DCLF withdraws). A gate:
	 * - the incoming blocks are hidden (app-cull) before their attach (when not attached already), and kept hidden by vetoing the
	 *   engine's show stores (the gates' show veto, Api::ShowVeto: the hidden-node source's stubs call Vetoes);
	 * - the outgoing blocks' handles are taken from their quadtree nodes inside FUN_140510a10 (its detour, in the swap's retirement
	 *   call), so it skips their detach and release: they stay attached and shown, and DCLF owns the references;
	 * - the gate is captured as an event on the scene's stack (CaptureGate: the gates' SceneStack channel) on the swapping thread at the
	 *   swap's last retirement call, in order after the attaches;
	 * - the scene coordinator publishes the post-swap claims once every geometry under the incoming roots is a member or ineligible,
	 *   with a flip for the gate (the gates' component's deltas: LodGateComponent.h);
	 * - the render thread, adopting that publication (FrameView, at the frame's start before the culls), calls Flip: the incoming shown and the
	 *   outgoing hidden through app-cull, with their hidden events, in the frame whose claims are post-swap;
	 * - the next LOD drain, under the manager's lock, detaches and releases the outgoing through the engine's own functions.
	 *
	 * Gates are lock-free: each gate's state is one atomic, the swapping thread and Flip racing by compare-exchange (a forced release
	 * on unload, LOD off, the interior early return, a full rebuild, the map or DCLF stopping wins or loses as one; so does a later swap
	 * that retires a gate's pending blocks, which supersedes it). The engine-side structures are the terrain manager's lock's, as the
	 * quadtree they mirror; DCLF adds no lock. Forced releases post Outcome{token, true}. A held block the
	 * engine reuses (reattached by a later swap while its gate lives, or referenced by its node again at its release) posts
	 * Outcome{token, false, kept}: Flip leaves a reattached block shown (the two race per block by compare-exchange on its handoff).
	 */

	/**
	 * @brief The gate event (Extensions::PushStacked on the gates' channel): called by the swap thunks on the swapping thread, under the
	 * manager's lock, after the incoming blocks' attaches. Pins the incoming roots (the blocks' NiNodes) and names the outgoing roots
	 * (keys; still attached and shown until Flip). One token per gate, from 1.
	 */
	void CaptureGate(std::uint64_t a_token, std::span<RE::NiAVObject* const> a_incoming, std::span<RE::NiAVObject* const> a_outgoing);
	/**
	 * @brief The feature's registration of the gates (Gates::RegisterGates, its install, while Enabled): the SceneStack channel their
	 * events go on and their outcome posts wake (Extensions::WakeChannel), and the hidden site their own stores carry
	 * (Extensions::PushReservedHidden). Before any gate opens (none does before DCLF runs).
	 */
	void NoteRegistered(Api::EngineChannelId a_channel, std::uint32_t a_hiddenSite);

	/**
	 * @brief The thunks and detours (a_switch: CS_DCLF_LOD_GATES, by default on). Once, at the feature's install. a_running: whether DCLF
	 * runs (ShellState::Running, read on the engine's threads): no gate opens while it does not, and the next LOD drain flushes every
	 * gate (forced) once it stops, as no flip will come. Null: always.
	 */
	void Install(Api::SwitchId a_switch, bool (*a_running)() = nullptr);
	/** @brief The switch is on and Install patched every site. */
	bool Enabled();
	/**
	 * @brief The detours on the LOD drain (FUN_140513840: held blocks released at its head) and a node's retirement (FUN_140510a10: the
	 * steal), one each (Install calls it). Idempotent.
	 */
	void InstallDetours();

	/**
	 * @brief The gates' show veto (Api::ShowVeto: open, vetoes), which the hidden-node source's stubs on the engine's LOD show stores
	 * (0x510697, 0x509b59, 0x509b69, 0x5117d0) call, any thread: the gates open (their fast test, a dword compare in the stub), and
	 * whether a_object is a pending incoming node of an open gate (its show is skipped, and no hidden event pushed). Lock-free: an
	 * open-addressing table read without writes.
	 */
	const std::atomic<std::uint32_t>& OpenGates();
	bool Vetoes(const void* a_object);
	/**
	 * @brief The show veto's engineHid (the hidden-node source's PushHidden), any thread: an engine LOD hide store (kEngineHideSites:
	 * 0x1405106a4, 0x140509b29, 0x140509b39) just hid a_object. Under the parity switches it marks a flipped gate's incoming node as
	 * hidden again by the engine (the coverage observer's evidence: LOD under the loaded cells); otherwise nothing.
	 */
	void NoteEngineHide(const void* a_object, std::uint32_t a_siteOffset);
	/** @brief The engine's LOD hide stores NoteEngineHide is told of (the show veto's hideSites): object update, a terrain block's two nodes. */
	inline constexpr std::array<Engine::Offset, 3> kEngineHideSites{ { { { 31837, 0x2c4 }, { 31050, 0x1f7 } }, { { 31745, 0x9 }, { 30942, 0x9 } }, { { 31745, 0x19 }, { 30942, 0x19 } } } };
	/** @brief kEngineHideSites as the running runtime's offsets (what the show veto's hideSites take). */
	inline std::span<const std::uint32_t> EngineHideSites()
	{
		static const auto sites = [] {
			std::array<std::uint32_t, kEngineHideSites.size()> offsets{};
			for (std::size_t i = 0; i < offsets.size(); ++i)
				offsets[i] = static_cast<std::uint32_t>(kEngineHideSites[i].Get());
			return offsets;
		}();
		return sites;
	}

	/**
	 * @brief The render thread, adopting a publication that carries a gate's flip: shows the incoming blocks and hides the outgoing
	 * (app-cull, with kHidden events pushed as HiddenStores' would be), and queues the outgoing's detach and release for the next
	 * LOD drain. A gate already released (forced) or unknown is a no-op that returns false.
	 */
	bool Flip(std::uint64_t a_token);

	/**
	 * @brief What became of a gate, for the coordinator (its channel's wake, Extensions::WakeChannel), or of one of its held outgoing blocks: kept names a
	 * block the engine reused (a quadtree node references its handle again: reattached while its gate lived, or found so at its release)
	 * by its root keys (the object block's NiNode; the terrain block's land and water nodes), with the gate that held it. The block stays
	 * attached as the node's current one, so the scene stops holding its geometries out (their Hold veto: Api::kVetoHold) and commits them again.
	 */
	struct Outcome
	{
		std::uint64_t token = 0;
		bool forced = false;  // released by a safety path without a flip: the coordinator drops the gate's state
		std::array<const void*, 2> kept{};  // non-null: not the gate's retirement but a held block of it kept by the engine
	};
	/** @brief The coordinator: takes every outcome posted since the last call, oldest first (MPSC). */
	void DrainOutcomes(void (*a_visit)(void*, const Outcome&), void* a_context);
	template <class F>
	void DrainOutcomes(F&& a_visit)
	{
		DrainOutcomes([](void* a_context, const Outcome& a_outcome) { (*static_cast<F*>(a_context))(a_outcome); }, &a_visit);
	}


	/** @brief The engine side's counters since the last Take (gates opened, flipped, forced, held blocks released, vetoes, the oldest open gate's age), reset (the gates' release lines: Gates::ReleaseLines). Empty (installed false) while not installed. */
	inline constexpr std::size_t kFlushKinds = 9;
	struct Totals
	{
		bool installed = false, coverage = false;
		std::uint64_t opened = 0, initialOpened = 0, initialLoadScreen = 0, flipped = 0, flipMisses = 0, forced = 0, superseded = 0, held = 0, released = 0;
		std::uint64_t reused = 0, handedOff = 0, deferred = 0, reattached = 0, kept = 0, reusedHidden = 0, orphansShown = 0, vetoes = 0;
		std::uint64_t flipFrames = 0, releases = 0;
		std::int64_t flipTicks = 0, flipTicksMax = 0, releaseTicks = 0, releaseTicksMax = 0, oldestTicks = 0;
		std::uint32_t open = 0, oldestFrames = 0;
		std::array<std::uint64_t, kFlushKinds> flushes{};
		std::uint64_t coverageBoth = 0, coverageNeither = 0, coverageEngineHid = 0, engineRehides = 0;  // the coverage observer's (coverage)
	};
	Totals Take();
	const char* FlushName(std::size_t a_index);
	double TicksToMilliseconds(std::int64_t a_ticks);
}
