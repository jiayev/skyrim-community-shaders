#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Common/EngineReleases.h"

namespace DCLF
{
	/**
	 * @brief The sun entries DCLF could take out of the engine's cascade culls, as the scene store last found them
	 * (SceneStore::UpdateSunCandidates): immutable once published, one for each walk that changed an entry.
	 *
	 * An entry (a static reference's root, or a terrain block's multibound node: SceneStore::ResolveSunEntry) is a
	 * candidate when every tracked geometry under it is either a table object, whose shadow the shadow epoch draws or
	 * the caster rule rejects, or one the engine never draws into a shadow map (not a Lighting geometry, hidden,
	 * alpha-blended, fading, or an unselected switch child).
	 *
	 * Indices are stable: an entry keeps its index while it is a candidate, and so does each of its geometries. A free index has
	 * no node (entryNodes). Every change of an index takes a new version (entryVersion, session-unique), so two snapshots differ
	 * exactly at the indices whose versions differ (ChangedEntries): what every consumer brings itself up to date by, entry by
	 * entry.
	 */
	struct SunCandidates
	{
		SunCandidates() = default;
		SunCandidates(const SunCandidates&) = delete;
		SunCandidates& operator=(const SunCandidates&) = delete;
		// The last owner may be any thread (a shadow build's exclusion): the entry nodes are released on the render thread.
		~SunCandidates()
		{
			for (auto& node : held)
				EngineReleases::Push(std::move(node));
		}

		std::uint32_t generation = 0;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> entries;     // entry node -> entry index
		std::vector<const RE::NiAVObject*> entryNodes;                                  // entry index -> entry node (null: free)
		std::vector<std::uint32_t> entryVersion;                                        // entry index -> its version (see the struct)
		// The entry nodes, held as long as the snapshot is: every reader keyed by them (PrimaryCull's cut and list filter, the
		// exclusions) may dereference one, and the engine may detach and drop it before the next snapshot replaces this one.
		std::vector<RE::NiPointer<RE::NiAVObject>> held;
		std::vector<std::vector<std::uint32_t>> entryGeometries;                        // entry index -> its geometry indices
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometries;  // every tracked geometry under one -> geometry index
		std::vector<const RE::BSGeometry*> geometryNodes;                               // geometry index -> geometry (null: free)
		std::vector<std::uint32_t> geometryEntry;                                       // geometry index -> entry index (kNone: free)
		// geometry index -> its object slot as the walk that made the snapshot had it (-1: no record); the frame reads it, not the
		// coordinator's tracked set (step 6c)
		std::vector<std::int32_t> geometrySlot;
		// Per geometry index: a main-pass table object PrimaryCull can give a synthetic pass (SceneStore::PrimaryEntryAllows);
		// the rest under a left-out entry are registered by the engine, handed over as its cull would (PrimaryCull).
		std::vector<std::uint8_t> primaryGeometry;

		static constexpr std::uint32_t kNone = ~0u;
		/** @brief The index space: entry indices are below it (free ones among them). */
		std::uint32_t Capacity() const { return static_cast<std::uint32_t>(entryNodes.size()); }
		std::uint32_t GeometryCapacity() const { return static_cast<std::uint32_t>(geometryNodes.size()); }
		/** @brief The live entries. */
		std::uint32_t Count() const { return static_cast<std::uint32_t>(entries.size()); }
	};

	/**
	 * @brief The entry indices at which a_to differs from a_from (their versions differ, or one has the index and the other not),
	 * appended to a_out in order. A null a_from: every index of a_to.
	 */
	void ChangedEntries(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out);

	/**
	 * @brief One shadow epoch's verdict on the candidates: the entries whose casters it all drew (the set's), which the
	 * next frame's full-frustum cull leaves out of the cascade culls. Built by the shadow build (IndirectDraws), used once.
	 */
	struct SunExclusion
	{
		std::shared_ptr<const SunCandidates> candidates;
		std::vector<std::uint8_t> excluded;  // per candidate index
		std::uint32_t excludedCount = 0;
		// Changes only when excluded does (a reuse carries its predecessor's): the scene lists' filter compares it.
		std::uint64_t version = 0;
		// Per candidate index, the stamp of the full-frustum cull that took the entry out (SunAccumulation's frame
		// stamp): the registration thunks' test. Written on the render thread, read by the registration jobs.
		std::unique_ptr<std::atomic<std::uint32_t>[]> removed;
		// Per geometry index, the stamp of the frame whose main registration has cleared the geometry's mask (+0x160 =
		// 0xFFFF: it reads the mask, then zeroes it): the engine's mask holds none of the sun's bits after it, so DCLF's
		// are not written again that frame.
		std::unique_ptr<std::atomic<std::uint32_t>[]> cleared;
		// The paraboloid mode's (LocalLightCull): per candidate index, three stamped words (LocalLightCull's reach kinds) of the
		// point lights whose cull reached the entry this frame and skipped it (Process1). Null for the sun's.
		std::unique_ptr<std::atomic<std::uint64_t>[]> lightReach;
		// The scene frame of the shadow build it came from, and whether it reused that cache's verdict (diagnostics).
		std::uint32_t builtFrame = 0;
		bool reused = false;
	};

	/**
	 * @brief a_exclusion, made for an earlier snapshot, as a verdict on a_to: the entries changed between the two (ChangedEntries)
	 * are not excluded (the engine culls them), every other one keeps its verdict (the indices are stable). a_exclusion itself when
	 * it is a_to's. a_newVersion: the copy's version is a new one when its content differs (a consumer keyed on it), else the
	 * original's (one that follows the changed entries itself). a_lightReach: the copy has the paraboloid mode's reach words.
	 * a_changed (optional): the changed indices.
	 */
	std::shared_ptr<SunExclusion> TranslateExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const SunCandidates>& a_to,
		bool a_newVersion, bool a_lightReach, std::vector<std::uint32_t>* a_changed = nullptr);

	/**
	 * @brief The engine's own work for the sun's cascades, taken away where DCLF already draws the result
	 * (docs/development/skyrim-engine-notes.md, "The sun's accumulation"). AE only.
	 *
	 * M1, registration-free accumulation (toggle `skipSunAccumulation`, CS_DCLF_SUN_SKIP): the engine still culls
	 * the sun's cascades, but a geometry in the set's caster phase is not registered. BSShadowDirectionalLight::
	 * Accumulate culls each cascade and hands every geometry it reaches to the accumulator's registration
	 * (FUN_1414b2140, called from FUN_140e28af0). That builds the geometry's shadow passes, which PassCapture then
	 * withholds one by one, and ORs the cascade's bit into the property's activeLightMask, which the main pass reads.
	 * For a member the thunks on those calls replicate the registration's early-outs and its mask write,
	 * and skip the passes. Everything else calls the original, so any other caster is registered and drawn by
	 * the engine, and no frame-wide verdict is needed.
	 *
	 * The entry exclusion (toggle `excludeSunEntries`, CS_DCLF_SUN_EXCLUDE; drawcall-limit-fix.md, "The sun's cascades
	 * without DCLF's objects"): the cascade culls walk only the full-frustum processes' objectArray, so right after the
	 * full-frustum cull every entry (reference root, or a terrain block's multibound node) whose content DCLF draws
	 * entirely is taken out of it (SunCandidates, SunExclusion). Those entries are then never traversed, culled or
	 * registered for the cascades. What the registration would have written, the sun's bits in each geometry's
	 * activeLightMask, DCLF writes when the main camera registers the geometry: the cascades its bound meets, tested
	 * against the planes the engine's own cascade cull used (the Geometric rule).
	 */
	class SunAccumulation
	{
	public:
		static SunAccumulation& Get();

		/** @brief AE: the Accumulate vtable slot and the call sites; verified before patching. */
		void Install();
		bool Installed() const { return installed; }

		/** @brief Every kReportInterval frames: the counters and, with the timing probe, the times. */
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

		/**
		 * @brief Render thread, after the shadow epoch drew: the exclusion the next full-frustum cull
		 * applies, once. Null: nothing is excluded.
		 */
		void PublishExclusion(std::shared_ptr<SunExclusion> a_exclusion) { pendingExclusion = std::move(a_exclusion); }
		/** @brief Render thread: the exclusion the next full-frustum cull will apply (null: none published). */
		std::shared_ptr<const SunExclusion> PendingExclusion() const { return pendingExclusion; }
		/**
		 * @brief Render thread, at the frame's start (before the scene lists' filter reads it): the pending exclusion as a verdict on
		 * the frame's candidates (TranslateExclusion: the entries changed since its snapshot are the engine's, the rest stand).
		 */
		void BeginFrame(const std::shared_ptr<const SunCandidates>& a_candidates);

		/**
		 * @brief Render thread, from the end of the sun's Accumulate to the next full-frustum cull: whether the bound
		 * meets any of this frame's cascades, tested against the planes the engine's own cascade culls used (the
		 * Geometric rule). Unknown (the cascades were not captured this frame): nullopt.
		 */
		std::optional<bool> InSunCascades(const RE::NiBound& a_bound) const;
		/**
		 * @brief One cascade as BuildDraws tests it (the colour latch's cascade region, IndirectDraws: kSunCascadeBytes): the
		 * two active-plane masks (0 custom: none), then 6 planes and 6 custom planes as (normal, constant).
		 */
		struct GpuCascade
		{
			std::uint32_t masks[2]{};
			std::uint32_t pad[2]{};
			float planes[12][4]{};
		};
		/**
		 * @brief This frame's captured cascades as BuildDraws tests them, into a_out (cleared first). Render thread, after the
		 * sun's Accumulate. Empty when none were captured this frame.
		 */
		void GpuCascades(std::vector<GpuCascade>& a_out) const;
		/** @brief Whether this frame's full-frustum cull applied the entry exclusion (its cascades will be captured). */
		bool ExclusionLive() const { return exclusionLive.load(std::memory_order_acquire); }
		/** @brief After the sun's Accumulate: every cascade's activeLightMask bit this frame. */
		std::uint32_t SunBits() const { return frameState.sunBits; }
		/**
		 * @brief CS_DCLF_SET_PARITY (every 30th frame): the set's casters the cascade culls of the frame's Accumulate reached,
		 * each with its cascade's descriptor index - what DCLF's views of those cascades must draw. Render thread.
		 */
		struct CascadeRegistration
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t descriptor = 0;
		};
		const std::vector<CascadeRegistration>& ClaimedRegistrations(std::uint32_t& a_frame) const
		{
			a_frame = claimedRegistrationsFrame;
			return claimedRegistrations;
		}
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
			std::uint32_t activeFrames = 0;   // ... with M1 active
			std::uint64_t skipped = 0;        // claimed geometries not registered (the mask written)
			std::uint64_t registered = 0;     // sun registrations passed to the engine
			std::uint64_t offThread = 0;      // sun registrations outside the Accumulate call (passed to the engine)
			std::int64_t fullFrustumTicks = 0, fullFrustumMax = 0;
			std::int64_t accumulateTicks = 0, accumulateMax = 0;
			// The entry exclusion (CS_DCLF_SUN_EXCLUDE).
			std::uint32_t exclusionFrames = 0;        // full-frustum culls that applied one
			std::uint32_t exclusionStale = 0;         // ... that had one built for other candidates (the scene changed)
			std::uint32_t exclusionMissing = 0;       // ... that had none published (no shadow epoch, or not built)
			std::uint64_t entriesSeen = 0;            // objectArray entries the applied culls found
			std::uint64_t entriesRemoved = 0;         // ... and took out
			std::uint64_t candidates = 0;             // candidates, and excluded ones, of the applied exclusions
			std::uint64_t excluded = 0;
			std::int64_t filterTicks = 0;             // the render thread's compaction
			std::uint64_t cascadeRegistrationsUnderRemoved = 0;  // must be 0: a cascade reached a removed entry anyway
		};
		/**
		 * @brief The cascade culls' skip of excluded entries (Hooks::CascadeProcess1): an excluded entry the objectArray still
		 * reaches through a node above it. Job threads may count.
		 */
		struct SkipStats
		{
			std::atomic<std::uint64_t> skipped{ 0 }, parityFrames{ 0 }, parityLost{ 0 };
			std::string parityLostFirst;  // render thread (the cascades' registrations run on it)
		};
		SkipStats skipStats;

		/** @brief The registration thunks' view of the sun's bits (job threads). */
		struct BitStats
		{
			std::atomic<std::uint64_t> written{ 0 };       // registrations of a geometry under a removed entry that took DCLF's bits
			std::atomic<std::uint64_t> withBits{ 0 };      // ... with at least one cascade's bit
			std::atomic<std::uint64_t> notReady{ 0 };      // ... before the cascades were known (must be 0)
		};

	private:
		SunAccumulation() = default;

		struct Hooks;
		friend struct Hooks;

		/**
		 * @brief After the full-frustum cull, render thread: takes the published exclusion (once) and, when it still
		 * describes the scene, removes its excluded entries from the full-frustum processes' objectArray, which is all
		 * the cascade culls walk. Each process keeps at least one entry: the cascade cull sets its planes up from the
		 * first.
		 */
		void ExcludeEntries(RE::BSShadowDirectionalLight* a_light);
		/**
		 * @brief Any thread: the geometry's index among the candidates when it is under an entry this frame's full-frustum
		 * cull removed (or would have), else ~0u.
		 */
		std::uint32_t RemovedGeometryIndex(const RE::BSGeometry* a_geometry) const;
		bool UnderRemovedEntry(const RE::BSGeometry* a_geometry) const { return RemovedGeometryIndex(a_geometry) != ~0u; }
		/** @brief Whether a_geometry is under an entry this frame's exclusion excludes (removed or skipped). */
		bool UnderExcludedEntry(const RE::BSGeometry* a_geometry) const
		{
			const auto* exclusion = frameState.exclusion.get();
			if (!exclusion || !exclusion->candidates)
				return false;
			// Any ancestor, as the cascades' skip takes it out (CascadeProcess1): a caster under an excluded entry need not be a
			// candidate geometry (an effect under a cell entry, for one).
			const auto& entries = exclusion->candidates->entries;
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (const auto it = entries.find(object); it != entries.end() && exclusion->excluded[it->second])
					return true;
			return false;
		}
		/**
		 * @brief A registration after the sun's Accumulate, any thread: a geometry under a removed entry gets the bits of
		 * the cascades its bound meets, ORed into its activeLightMask before GetRenderPasses reads it.
		 */
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
			bool captured = false;
		};

		/** @brief The frame's exclusion, as the registration thunks read it. */
		struct FrameState
		{
			std::shared_ptr<SunExclusion> exclusion;  // kept alive until the next full-frustum cull
			std::uint32_t stamp = 0;
			// CS_DCLF_PERSISTENT_PARITY's frames: the cascade culls skip nothing, and an unclaimed caster registered under an
			// excluded entry counts as one the skip would lose.
			bool parity = false;
			// One per cascade of the sun's Accumulate (grown there, while bitsReady is clear, so no reader sees it move); the
			// first cascadeCount are this frame's.
			std::vector<Cascade> cascades;
			std::uint32_t cascadeCount = 0;
			std::uint32_t cascadesFrame = ~0u;        // the scene frame whose Accumulate captured them
			std::uint32_t sunBits = 0;                // every cascade's bit
		};
		std::shared_ptr<SunExclusion> pendingExclusion;  // render thread
		std::shared_ptr<SunExclusion> previousExclusion;  // the last frame's, kept alive for a straggling registration
		std::vector<std::uint32_t> removeScratch;         // render thread: per objectArray entry, its candidate index + 1 when removed
		FrameState frameState;                           // written on the render thread before bitsReady
		// Set once the cascades are captured (the end of the sun's Accumulate), cleared when Main::Draw's mask clear
		// returns and at the full-frustum cull: while set, a registration of a geometry under a removed entry takes DCLF's
		// bits, as the engine's cascade registrations would have left them in its mask.
		std::atomic<bool> bitsReady{ false };
		std::atomic<bool> exclusionLive{ false };        // frameState.exclusion is set for this frame
		std::atomic<std::uint32_t> stampCounter{ 0 };
		BitStats bitStats;
		std::vector<CascadeRegistration> claimedRegistrations;  // render thread (ClaimedRegistrations)
		std::uint32_t claimedRegistrationsFrame = ~0u;
	};
}
