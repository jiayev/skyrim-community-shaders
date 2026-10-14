#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Runtime/EngineReleases.h"

namespace DCLF::Diagnostics
{
	struct SunAccumulationView;
}

namespace DCLF
{
	struct CandidateChanges;  // Scene/Coordinator/CandidateTable.h

	/**
	 * @brief The sun entries DCLF could take out of the engine's cascade culls (Coordinator::UpdateSunCandidates); immutable once
	 * published. An entry (a static reference's root, or a terrain block's multibound node) is a candidate when every tracked
	 * geometry under it is a table object (its shadow drawn by the shadow epoch or rejected by the caster rule) or one the engine
	 * never draws into a shadow map (non-Lighting, hidden, blended, fading, unselected switch child).
	 *
	 * Indices are stable while an entry is a candidate, as are its geometries'. Every index change takes a session-unique version
	 * (entryVersion), so snapshots differ exactly where versions differ (ChangedEntries), which consumers update by.
	 */
	struct SunCandidates
	{
		SunCandidates() = default;
		SunCandidates(const SunCandidates&) = delete;
		SunCandidates& operator=(const SunCandidates&) = delete;
		// The last owner may be any thread: the entry nodes are released on the render thread.
		~SunCandidates()
		{
			for (auto& node : held)
				EngineReleases::Push(std::move(node));
		}

		std::uint32_t generation = 0;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> entries;     // entry node -> entry index
		std::vector<const RE::NiAVObject*> entryNodes;                                  // entry index -> entry node (null: free)
		std::vector<std::uint32_t> entryVersion;                                        // entry index -> its version
		// The entry nodes, held as long as the snapshot: readers keyed by them may dereference one after the engine drops it.
		std::vector<RE::NiPointer<RE::NiAVObject>> held;
		std::vector<std::vector<std::uint32_t>> entryGeometries;                        // entry index -> its geometry indices
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometries;  // every tracked geometry under one -> geometry index
		std::vector<const RE::BSGeometry*> geometryNodes;                               // geometry index -> geometry (null: free)
		std::vector<std::uint32_t> geometryEntry;                                       // geometry index -> entry index (kNone: free)
		// geometry index -> its object slot at the snapshot's walk (-1: no record); the frame reads this, not the tracked set
		std::vector<std::int32_t> geometrySlot;
		// Per geometry (Eligibility::PrimaryVerdict): 1, a table object PrimaryCull can give a synthetic pass; 2, a hidden static's,
		// drawn by nobody; 0, the engine's (under a left-out entry, handed over as its cull would).
		std::vector<std::uint8_t> primaryGeometry;
		// The table's change log as published with this snapshot: which indices moved since earlier generations
		// (CandidateTable.h, ChangedEntriesByLog). Null for a snapshot no table published.
		std::shared_ptr<const CandidateChanges> changes;

		static constexpr std::uint32_t kNone = ~0u;
		/** @brief The index space: entry indices are below it (free ones among them). */
		std::uint32_t Capacity() const { return static_cast<std::uint32_t>(entryNodes.size()); }
		std::uint32_t GeometryCapacity() const { return static_cast<std::uint32_t>(geometryNodes.size()); }
		/** @brief The live entries. */
		std::uint32_t Count() const { return static_cast<std::uint32_t>(entries.size()); }
	};

	/**
	 * @brief Appends, in order, the entry indices whose versions differ between a_from and a_to (null a_from: every index), by comparing
	 * every index: ChangedEntriesByScan. Readers that update by the change follow ChangedEntriesByLog instead (O(changes)).
	 */
	void ChangedEntries(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out);
	// Defined with the candidate table (Scene/Coordinator/CandidateTable.h, which documents them); declared here for the readers of
	// SunCandidates.
	bool ChangedEntriesByLog(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out);
	void ChangedEntriesByScan(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out);

	/**
	 * @brief One shadow epoch's verdict: the entries whose casters it all drew, which the next frame's full-frustum cull leaves out of
	 * the cascade culls. Built by the shadow build; immutable once published (but for its stamped words and its forward link), and
	 * the same one is published again while nothing it was judged from moved.
	 */
	struct SunExclusion
	{
		SunExclusion() = default;
		SunExclusion(const SunExclusion&) = delete;
		SunExclusion& operator=(const SunExclusion&) = delete;

		std::shared_ptr<const SunCandidates> candidates;
		std::vector<std::uint8_t> excluded;  // per candidate index
		std::uint32_t excludedCount = 0;
		// Changes only when excluded does: the scene lists' filter compares it.
		std::uint64_t version = 0;
		// The stamped words: pooled, shared by the versions of one mode's exclusions (the builder's, and their translations) and never
		// reset. Every word holds the stamp of the frame (or cull) that wrote it, which only that frame's own readers compare with, and
		// one version per mode is stamped a frame (the frame's), so a word another version wrote never reads as this frame's. Grown by
		// replacing them: a version holds what it was made with, at least removedCount and clearedCount long.
		// Per candidate, the stamp of the full-frustum cull that took it out: the registration thunks' test. Written on the render
		// thread, read by the registration jobs.
		std::shared_ptr<std::atomic<std::uint32_t>[]> removed;
		// Per geometry, the stamp of the frame whose main registration cleared its mask (+0x160 = 0xFFFF: reads, then zeroes it), so
		// DCLF's sun bits are not written again that frame.
		std::shared_ptr<std::atomic<std::uint32_t>[]> cleared;
		// Paraboloid mode (LocalLightCull): per candidate, three stamped words (reach kinds) of the point lights whose cull reached and
		// skipped the entry this frame. Null for the sun's.
		std::shared_ptr<std::atomic<std::uint64_t>[]> lightReach;
		std::uint32_t removedCount = 0;  // entries removed and lightReach (times 3) hold
		std::uint32_t clearedCount = 0;  // geometries cleared holds
		// The scene frame of the shadow build it came from, and whether it reused that cache's verdict (diagnostics).
		std::uint32_t builtFrame = 0;
		bool reused = false;

		// The forward link (ForwardExclusion): this verdict translated to the next candidates the builder judged with, made by the
		// builder once it built for them. Written once (forwardOwned, then forward released); read by any thread after forward.
		std::shared_ptr<SunExclusion> forwardOwned;
		std::atomic<const SunExclusion*> forward{ nullptr };
		// A forward translation's: the entries changed between its predecessor's candidates and its own, which it does not exclude.
		std::vector<std::uint32_t> forwardChanged;
	};

	/**
	 * @brief a_exclusion (made for an earlier snapshot) as a verdict on a_to: changed entries are not excluded, the rest keep theirs.
	 * a_newVersion: a new version when the content differs, else the original's. a_lightReach: with the paraboloid reach words.
	 * a_stamps: an exclusion for a_to whose stamped words the translation shares (else a_exclusion's when long enough, else new ones).
	 * The builder makes them (ForwardExclusion follows what it made); a reader translating itself pays O(changes + candidates).
	 */
	std::shared_ptr<SunExclusion> TranslateExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const SunCandidates>& a_to,
		bool a_newVersion, bool a_lightReach, std::vector<std::uint32_t>* a_changed = nullptr, const SunExclusion* a_stamps = nullptr);

	/**
	 * @brief a_exclusion as a verdict on a_to, as the builder translated it (its forward links, made when it built for a_to: the same
	 * verdict TranslateExclusion gives); a_exclusion itself when it is for a_to. Null when the builder made none for a_to (it did not
	 * build that mode's exclusion for those candidates). a_changed: appends each link's changed entries. O(links), any thread.
	 */
	std::shared_ptr<SunExclusion> ForwardExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const SunCandidates>& a_to,
		std::vector<std::uint32_t>* a_changed = nullptr);

	/**
	 * @brief The engine's work for the sun's cascades, removed where DCLF already draws the result (skyrim-engine-notes.md, "The
	 * sun's accumulation"). AE only. Render thread, plus the registration thunks on job threads.
	 *
	 * Registration-free accumulation (`skipSunAccumulation`, CS_DCLF_SUN_SKIP): BSShadowDirectionalLight::Accumulate culls each
	 * cascade and hands each geometry to the accumulator's registration (FUN_1414b2140, from FUN_140e28af0), which builds shadow
	 * passes and ORs the cascade's bit into activeLightMask (the main pass reads it). For a member the thunks replicate the early-outs
	 * and mask write and skip the passes; anything else calls the original.
	 *
	 * Entry exclusion (`excludeSunEntries`, CS_DCLF_SUN_EXCLUDE; drawcall-limit-fix.md, "The sun's cascades without DCLF's objects"):
	 * the cascade culls walk only the full-frustum processes' objectArray, so after the full-frustum cull every entry DCLF draws
	 * entirely is removed from it. DCLF writes the sun's mask bits at the main registration instead: the cascades the bound meets,
	 * against the planes the engine's cascade cull used.
	 */
	class SunAccumulation
	{
	public:
		static SunAccumulation& Get();

		/** @brief AE: the Accumulate vtable slot and the call sites; verified before patching. */
		void Install();
		bool Installed() const { return installed; }


		/** @brief Render thread, after the shadow epoch: the exclusion the next full-frustum cull applies once (null: none). */
		void PublishExclusion(std::shared_ptr<SunExclusion> a_exclusion) { pendingExclusion = std::move(a_exclusion); }
		/** @brief Render thread: the exclusion the next full-frustum cull will apply (null: none published). */
		std::shared_ptr<const SunExclusion> PendingExclusion() const { return pendingExclusion; }
		/**
		 * @brief Render thread, frame start (before the scene lists' filter): the pending exclusion as the builder translated it to
		 * a_candidates (ForwardExclusion: a pointer swap). None made (the builder did not build the mode for them): the pending one stays
		 * as it is, a verdict on other candidates, which the full-frustum cull does not apply (stale).
		 */
		void BeginFrame(const std::shared_ptr<const SunCandidates>& a_candidates);

		/** @brief One cascade as BuildDraws tests it (GpuLayouts.h): plane masks (custom 0: none), 6 planes and 6 custom planes. */
		struct GpuCascade
		{
			std::uint32_t masks[2]{};
			std::uint32_t pad[2]{};
			float planes[12][4]{};
		};
		/** @brief This frame's cascades from DCLF's views (SunViews). Render thread, after the sun's UpdateCamera; empty without views. */
		void GpuCascades(std::vector<GpuCascade>& a_out) const;
		/** @brief Whether this frame's full-frustum cull applied the entry exclusion. */
		bool ExclusionLive() const { return exclusionLive.load(std::memory_order_acquire); }
		/** @brief Render thread, after the full-frustum call site: on a frame it did not run, clears cascades, exclusion and sun bits. */
		void EndFullFrustumWindow();
		/** @brief After the sun's Accumulate: every cascade's activeLightMask bit. */
		std::uint32_t SunBits() const { return frameState.sunBits; }
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
			std::uint32_t exclusionForwarded = 0;     // frame starts that took the builder's translation (BeginFrame)
			std::uint32_t exclusionUnforwarded = 0;   // ... that found none for the frame's candidates
			std::uint64_t entriesSeen = 0;            // objectArray entries the applied culls found
			std::uint64_t entriesRemoved = 0;         // ... and took out
			std::uint64_t candidates = 0;             // candidates, and excluded ones, of the applied exclusions
			std::uint64_t excluded = 0;
			std::int64_t filterTicks = 0;             // the render thread's compaction
			std::uint64_t cascadeRegistrationsUnderRemoved = 0;  // must be 0
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
		};

		/** @brief Render thread: the counters since the last call, reset. */
		Stats TakeStats();
		/** @brief The entry exclusion's registration and skip counts since the last call, reset. */
		struct ExclusionCounts
		{
			std::uint64_t written = 0, withBits = 0, notReady = 0, skipped = 0;
		};
		ExclusionCounts TakeExclusionCounts();

	private:
		SunAccumulation() = default;

		struct Hooks;
		friend struct Hooks;
		friend struct Diagnostics::SunAccumulationView;

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
			bool captured = false;         // DCLF's volume is known (SunViews)
		};

		/** @brief The frame's exclusion, as the registration thunks read it. */
		struct FrameState
		{
			std::shared_ptr<SunExclusion> exclusion;  // kept alive until the next full-frustum cull
			std::uint32_t stamp = 0;
			// The cascade culls skip excluded entries they reach anyway (Policy::ExcludeSun).
			bool skipExcluded = true;
			// Grown in the sun's Accumulate while bitsReady is clear (no reader sees it move); the first cascadeCount are this frame's.
			std::vector<Cascade> cascades;
			std::uint32_t cascadeCount = 0;
			std::uint32_t cascadesFrame = ~0u;        // the scene frame whose Accumulate captured them
			std::uint32_t sunBits = 0;                // every cascade's bit
		};
		std::shared_ptr<SunExclusion> pendingExclusion;  // render thread
		std::shared_ptr<SunExclusion> previousExclusion;  // the last frame's, kept alive for a straggling registration
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
		std::shared_ptr<SunExclusion> armedExclusion;     // render thread: the exclusion the armed veto applies
		std::atomic<const SunExclusion*> vetoExclusion{ nullptr };  // the appends' view of it (null: disarmed)
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
	};
}
