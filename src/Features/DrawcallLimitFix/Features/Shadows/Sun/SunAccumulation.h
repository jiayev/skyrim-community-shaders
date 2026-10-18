#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Core/Engine/FrameAddressSet.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"

namespace DCLF::Features::Shadows::Sun
{
	struct SunAccumulationView;

	/**
	 * @brief The engine's work for the sun's cascades, removed where DCLF already draws the result (skyrim-engine-notes.md, "The
	 * sun's accumulation"). AE only. Render thread, plus the registration thunks on job threads.
	 *
	 * Registration-free accumulation (toggle "skip sun accumulation", CS_DCLF_SUN_SKIP): BSShadowDirectionalLight::Accumulate culls each
	 * cascade and hands each geometry to the accumulator's registration (FUN_1414b2140, from FUN_140e28af0), which builds shadow
	 * passes and ORs the cascade's bit into activeLightMask (the main pass reads it). For a member the thunks replicate the early-outs
	 * and mask write and skip the passes; anything else calls the original.
	 *
	 * Entry exclusion (toggle "exclude sun entries", CS_DCLF_SUN_EXCLUDE; dclf-architecture.md, "The sun's cascades without DCLF's objects"):
	 * the cascade culls walk only the full-frustum processes' objectArray, so after the full-frustum cull every entry DCLF draws
	 * entirely is removed from it. DCLF writes the sun's mask bits at the main registration instead: the cascades the bound meets,
	 * against the planes the engine's cascade cull used.
	 *
	 * Whole skip (the same toggle, while DCLF draws every sun cascade this frame: ArmWhole): the full-frustum cull runs over no scene
	 * list, so neither it nor Accumulate culls anything; each full-frustum process keeps one empty node of DCLF's (its cascade culls set
	 * their planes up from it). Every geometry the main camera registers takes DCLF's bits; a caster not yet a member casts no sun shadow
	 * until it joins, never through the engine's cull (as the occlusion maps' joining occluders).
	 */
	class SunAccumulation
	{
	public:
		static SunAccumulation& Get();

		/** @brief AE: the Accumulate vtable slot and the call sites; verified before patching. */
		void Install();
		bool Installed() const { return installed; }
		/** @brief Main thread, at the feature's install (before DCLF's threads start): its toggles, the registration skip's and the entry exclusion's. */
		static void SetToggles(Api::ToggleId a_skip, Api::ToggleId a_exclude);


		/** @brief Whether this frame's full-frustum cull applied the entry exclusion (or the whole skip). */
		bool ExclusionLive() const { return exclusionLive.load(std::memory_order_acquire); }
		/** @brief Render thread, after the full-frustum call site: on a frame it did not run, clears cascades, exclusion and sun bits. */
		void EndFullFrustumWindow();
		/** @brief Whether a_node is an entry this frame's exclusion excludes (render thread, while it is live). */
		bool Excluded(const void* a_node) const
		{
			const auto* exclusion = frameState.exclusion.get();
			if (!exclusion || !exclusion->candidates)
				return false;
			const auto it = exclusion->candidates->entries.find(static_cast<const RE::NiAVObject*>(a_node));
			return it != exclusion->candidates->entries.end() && exclusion->excluded[it->second];
		}


		struct Stats
		{
			std::uint32_t frames = 0;         // Accumulate calls for the sun
			std::uint32_t activeFrames = 0;   // ... with the registration skip active
			std::uint64_t skipped = 0;        // claimed geometries not registered (the mask written)
			std::uint64_t registered = 0;     // sun registrations passed to the engine
			std::uint64_t offThread = 0;      // sun registrations outside Accumulate (passed to the engine)
			std::int64_t fullFrustumTicks = 0, fullFrustumMax = 0;
			std::int64_t accumulateTicks = 0, accumulateMax = 0;
			// The entry exclusion (CS_DCLF_SUN_EXCLUDE).
			std::uint32_t exclusionFrames = 0;        // full-frustum culls that applied one
			std::uint32_t exclusionStale = 0;         // ... built for other candidates
			std::uint32_t exclusionMissing = 0;       // ... with none published
			std::uint32_t exclusionForwarded = 0;     // frame starts that took the builder's translation (SunExclusionSlot)
			std::uint32_t exclusionUnforwarded = 0;   // ... that found none for the frame's candidates
			std::uint64_t entriesSeen = 0;            // objectArray entries the applied culls found
			std::uint64_t entriesRemoved = 0;         // ... and took out
			std::uint64_t candidates = 0;             // candidates, and excluded ones, of the applied exclusions
			std::uint64_t excluded = 0;
			std::int64_t filterTicks = 0;             // the render thread's compaction
			std::uint64_t cascadeRegistrationsUnderRemoved = 0;  // must be 0
			// The whole skip (ArmWhole): frames it applied, and frames with the exclusion on that it could not (a cascade not DCLF's, the
			// lists too many, a parity frame).
			std::uint32_t wholeFrames = 0;
			std::uint32_t wholeDeclined = 0;
			// Its cascade culls (Hooks::Cascade): the full-frustum processes each cascade culled (those holding the extra list's entries) and
			// left out (empty, no cull and no registration).
			std::uint64_t wholeProcessesCulled = 0;
			std::uint64_t wholeProcessesLeft = 0;
		};
		/** @brief Excluded entries the cascade culls still reached through a node above (Hooks::CascadeProcess1). Job threads count. */
		struct SkipStats
		{
			std::atomic<std::uint64_t> skipped{ 0 };
		};
		SkipStats skipStats;

		/** @brief The registration thunks' view of the sun's bits (job threads). */
		struct BitStats
		{
			std::atomic<std::uint64_t> written{ 0 };       // removed-entry registrations that took DCLF's bits
			std::atomic<std::uint64_t> withBits{ 0 };      // ... with at least one cascade's bit
			std::atomic<std::uint64_t> notReady{ 0 };      // ... before the cascades were known (must be 0)
			// The whole skip's: main registrations that took DCLF's bits, with at least one cascade's, and those the per-frame table had no
			// room to stamp (written without the once-a-frame rule; must be 0).
			std::atomic<std::uint64_t> wholeWritten{ 0 };
			std::atomic<std::uint64_t> wholeWithBits{ 0 };
			std::atomic<std::uint64_t> wholeUnstamped{ 0 };
		};

		/** @brief Render thread: the counters since the last call, reset. */
		Stats TakeStats();
		/** @brief The entry exclusion's registration and skip counts since the last call, reset. */
		struct ExclusionCounts
		{
			std::uint64_t written = 0, withBits = 0, notReady = 0, skipped = 0;
			std::uint64_t wholeWritten = 0, wholeWithBits = 0, wholeUnstamped = 0;
		};
		ExclusionCounts TakeExclusionCounts();
		/** @brief Render thread: whether this frame's full-frustum cull took the whole skip (its processes culled nothing: no planes of theirs). */
		bool WholeFrame() const { return frameState.whole; }

	private:
		SunAccumulation() = default;

		struct Hooks;
		friend struct Hooks;
		friend struct SunAccumulationView;

		/**
		 * @brief Render thread, before the full-frustum cull: arms the published exclusion's veto (Hooks::AppendNonAccum), which keeps
		 * its excluded entries out of the full-frustum processes' objectArray as the cull appends them (on the cull's job threads).
		 */
		void ArmExclusion(RE::BSShadowDirectionalLight* a_light);
		/**
		 * @brief Render thread, after the full-frustum cull: disarms the veto and makes its result the frame's exclusion. Each process
		 * keeps at least one entry (the cascade cull sets its planes up from the first): one whose every append was vetoed gets its
		 * first back.
		 */
		void ExcludeEntries();
		/**
		 * @brief Render thread, before the full-frustum cull: whether the frame takes the whole skip (the exclusion's toggle and the
		 * registration skip on, every sun cascade view covered and its casters claimed by the frame's set, not a parity frame), and if
		 * so the frame's state set for it.
		 */
		bool ArmWhole(RE::BSShadowDirectionalLight* a_light);
		/** @brief Render thread, after the whole skip's full-frustum cull: the frame's exclusion is live (its bits taken at registration). */
		void WholeApplied();
		/**
		 * @brief Any thread, the whole skip's main registrations: whether a_geometry's first clearing registration (0xFFFF) this frame
		 * has been seen; a_clears stamps it so (FrameAddressSet, a round per whole frame).
		 */
		bool ClearedThisFrame(const void* a_geometry, bool a_clears);
		/** @brief Any thread: the candidate geometry index if under an entry this frame's cull removed (or would have), else ~0u. */
		std::uint32_t RemovedGeometryIndex(const RE::BSGeometry* a_geometry) const;
		bool UnderRemovedEntry(const RE::BSGeometry* a_geometry) const { return RemovedGeometryIndex(a_geometry) != ~0u; }
		/** @brief Whether a_geometry is under an entry this frame's exclusion excludes (removed or skipped). */
		bool UnderExcludedEntry(const RE::BSGeometry* a_geometry) const
		{
			const auto* exclusion = frameState.exclusion.get();
			if (!exclusion || !exclusion->candidates)
				return false;
			// Any ancestor, as CascadeProcess1 skips: a caster under an excluded entry need not be a candidate geometry.
			const auto& entries = exclusion->candidates->entries;
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (const auto it = entries.find(object); it != entries.end() && exclusion->excluded[it->second])
					return true;
			return false;
		}
		/** @brief Any thread: ORs the bits of the cascades a removed-entry geometry's bound meets into its activeLightMask. */
		void ApplySunBits(RE::BSGeometry* a_geometry, bool a_clears);

		bool installed = false;
		Stats stats;
		std::atomic<std::uint64_t> offThread{ 0 };

		/** @brief One cascade's cull volume and bit, as the engine's cascade cull used them this frame. */
		struct Cascade
		{
			std::array<std::array<float, 4>, 6> planes{};  // normal, constant: outside when n.c - d < -r
			std::uint32_t planeMask = 0;
			std::array<std::array<float, 4>, 6> customPlanes{};
			std::uint32_t customMask = 0;  // 0: no custom planes
			std::uint32_t bit = 0;         // the accumulator's +0x164
			bool captured = false;         // DCLF's volume is known (the Sun board)
		};

		/** @brief The frame's exclusion, as the registration thunks read it. */
		struct FrameState
		{
			std::shared_ptr<Local::SunExclusion> exclusion;  // kept alive until the next full-frustum cull
			std::uint32_t stamp = 0;
			// The cascade culls skip excluded entries they reach anyway (ExcludeSun, SunAccumulation.cpp).
			bool skipExcluded = true;
			// Grown in the sun's Accumulate while bitsReady is clear (no reader sees it move); the first cascadeCount are this frame's.
			std::vector<Cascade> cascades;
			std::uint32_t cascadeCount = 0;
			std::uint32_t cascadesFrame = ~0u;        // the scene frame whose Accumulate captured them
			std::uint32_t sunBits = 0;                // every cascade's bit
			bool whole = false;                       // the whole skip: every main registration takes DCLF's bits
		};
		std::shared_ptr<Local::SunExclusion> previousExclusion;  // the last frame's, kept alive for a straggling registration
		/**
		 * @brief One full-frustum process while the veto is armed. Its appends come from the one job culling it (and, after the jobs,
		 * the render thread's), so its counts have one writer at a time; the render thread reads them after the cull.
		 */
		struct VetoProcess
		{
			const void* process = nullptr;
			std::uint32_t seen = 0, vetoed = 0;
			RE::NiAVObject* firstVetoed = nullptr;  // given back when nothing else was appended
			std::uint32_t firstEntry = 0;
			bool givenBack = false;
		};
		std::vector<VetoProcess> vetoProcesses;          // written on the render thread before the veto is armed
		std::shared_ptr<Local::SunExclusion> armedExclusion;     // render thread: the exclusion the armed veto applies
		std::atomic<const Local::SunExclusion*> vetoExclusion{ nullptr };  // the appends' view of it (null: disarmed)
		std::uint32_t vetoStamp = 0;                      // written before the veto is armed
		FrameState frameState;                           // written on the render thread before bitsReady
		// Set at the end of the sun's Accumulate, cleared when Main::Draw's mask clear returns and at the full-frustum cull: while set,
		// a removed-entry geometry's registration takes DCLF's bits.
		std::atomic<bool> bitsReady{ false };
		std::atomic<bool> exclusionLive{ false };        // frameState.exclusion is set for this frame
		bool fullFrustumRan = false;                     // render thread: since EndFullFrustumWindow
		void ResetFrameSun();
		std::atomic<std::uint32_t> stampCounter{ 0 };
		BitStats bitStats;
		// The whole skip's once-a-frame rule (ClearedThisFrame): the geometries whose clearing registration took DCLF's bits, a round a frame.
		FrameAddressSet wholeCleared;
	};
}
