#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::LodGates
{
	/**
	 * @brief T6b5: the engine's LOD swaps gated on DCLF's readiness (CS_DCLF_LOD_GATES, on by default).
	 *
	 * The object and terrain Upgrade/Downgrade Func2s (AE 1.6.1170; skyrim-engine-notes.md, "LOD swaps and their gates") attach the
	 * incoming blocks and retire the outgoing in one call, on a job thread or the main thread, under the terrain manager's lock. A
	 * gate:
	 * - the incoming blocks are hidden (app-cull) before their attach (when not attached already), and kept hidden by vetoing the
	 *   engine's show stores (HiddenStores' stubs, Vetoes);
	 * - the outgoing blocks' handles are taken from their quadtree nodes inside FUN_140510a10 (its detour, in the swap's retirement
	 *   call), so it skips their detach and release: they stay attached and shown, and DCLF owns the references;
	 * - the gate is captured as a SceneTracker event (CaptureGate) on the swapping thread at the swap's last retirement call, in order
	 *   after the attaches;
	 * - the scene coordinator publishes the post-swap claims once every geometry under the incoming roots is a member or ineligible,
	 *   with a flip for the gate (PublicationDeltas::gateFlips);
	 * - the render thread, adopting that publication (BeginSceneFrame, before the culls), calls Flip: the incoming shown and the
	 *   outgoing hidden through app-cull, with their hidden events, in the frame whose claims are post-swap;
	 * - the next LOD drain, under the manager's lock, detaches and releases the outgoing through the engine's own functions.
	 *
	 * Gates are lock-free: each gate's state is one atomic, the swapping thread and Flip racing by compare-exchange (a forced release
	 * on unload, LOD off, the interior early return, a full rebuild, the map or DCLF stopping wins or loses as one; so does a later swap
	 * that retires a gate's pending blocks, which supersedes it). The engine-side structures are the terrain manager's lock's, as the
	 * quadtree they mirror; DCLF adds no lock. Forced releases bump ForcedGeneration and post Outcome{token, true}.
	 */

	/**
	 * @brief The gate event, implemented by the scene side (SceneTracker, EventType::Gate): called by the swap thunks on the swapping
	 * thread, under the manager's lock, after the incoming blocks' attaches. Pins the incoming roots (the blocks' NiNodes) and names
	 * the outgoing roots (keys; still attached and shown until Flip). One token per gate, from 1.
	 */
	void CaptureGate(std::uint64_t a_token, std::span<RE::NiAVObject* const> a_incoming, std::span<RE::NiAVObject* const> a_outgoing);

	/**
	 * @brief The thunks and detours (CS_DCLF_LOD_GATES, by default on). Once, at DCLF's install. a_running: whether DCLF runs
	 * (DrawcallLimitFix::Running, read on the engine's threads): no gate opens while it does not, and the next LOD drain flushes every
	 * gate (forced) once it stops, as no flip will come. Null: always.
	 */
	void Install(bool (*a_running)() = nullptr);
	/** @brief The switch is on and Install patched every site. */
	bool Enabled();
	/**
	 * @brief The detours on the LOD drain (FUN_140513840: held blocks released at its head) and a node's retirement (FUN_140510a10: the
	 * steal), one each, shared with ImportTimings (which times through them): installed by whichever comes first. Idempotent.
	 */
	void InstallDetours();

	/**
	 * @brief HiddenStores' veto stubs on the engine's LOD show stores (0x510697, 0x509b59, 0x509b69, 0x5117d0), any thread: the gates
	 * open (their fast test, a dword compare in the stub), and whether a_object is a pending incoming node of an open gate (its show is
	 * skipped, and no hidden event pushed). Lock-free: an open-addressing table read without writes.
	 */
	const std::atomic<std::uint32_t>& OpenGates();
	bool Vetoes(const void* a_object);
	/**
	 * @brief HiddenStores (PushHidden), any thread: an engine LOD hide store (0x1405106a4, 0x140509b29, 0x140509b39) just hid a_object.
	 * Under the parity switches it marks a flipped gate's incoming node as hidden again by the engine (the coverage observer's
	 * evidence: LOD under the loaded cells); otherwise nothing.
	 */
	void NoteEngineHide(const void* a_object, std::uint32_t a_siteOffset);

	/**
	 * @brief The render thread, adopting a publication that carries a gate's flip: shows the incoming blocks and hides the outgoing
	 * (app-cull, with kHidden events pushed as HiddenStores' would be), and queues the outgoing's detach and release for the next
	 * LOD drain. A gate already released (forced) or unknown is a no-op that returns false.
	 */
	bool Flip(std::uint64_t a_token);

	/** @brief What became of a gate, for the coordinator (SceneWake::Gate wakes it). */
	struct Outcome
	{
		std::uint64_t token = 0;
		bool forced = false;  // released by a safety path without a flip: the coordinator drops the gate's state
	};
	/** @brief The coordinator: takes every outcome posted since the last call, oldest first (MPSC). */
	void DrainOutcomes(void (*a_visit)(void*, const Outcome&), void* a_context);
	template <class F>
	void DrainOutcomes(F&& a_visit)
	{
		DrainOutcomes([](void* a_context, const Outcome& a_outcome) { (*static_cast<F*>(a_context))(a_outcome); }, &a_visit);
	}

	/**
	 * @brief Generation of forced releases, read by the render thread at the frame's start into the frame inputs: a snapshot built
	 * before a forced release may still claim the detached outgoing blocks, so adoption withdraws until one built after it arrives.
	 */
	std::uint64_t ForcedGeneration();

	/** @brief Counters since the last call: gates opened, flipped, forced, held blocks released, vetoes, oldest open gate age. */
	std::string TakeReport(std::uint32_t a_frames);
}
