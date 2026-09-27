#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace DCLF
{
	/**
	 * @brief The sun entries DCLF could take out of the engine's cascade culls, as the scene store last found them
	 * (SceneStore::UpdateSunCandidates): immutable, and replaced whole when any entry's status changes.
	 *
	 * An entry (a static reference's root, or a terrain block's multibound node: SceneStore::SunEntryOf) is a
	 * candidate when every tracked geometry under it is either a table object, whose shadow the shadow epoch draws or
	 * the caster rule rejects, or one the engine never draws into a shadow map (not a Lighting geometry, hidden,
	 * alpha-blended, fading, or an unselected switch child).
	 */
	struct SunCandidates
	{
		std::uint32_t generation = 0;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> entries;     // entry node -> entry index
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometries;  // every tracked geometry under one -> geometry index
		std::vector<std::uint32_t> geometryEntry;                                       // geometry index -> entry index
		// Per geometry index: a main-pass table object PrimaryCull can give a synthetic pass (SceneStore::PrimaryEntryAllows);
		// the rest under a left-out entry are registered by the engine, handed over as its cull would (PrimaryCull).
		std::vector<std::uint8_t> primaryGeometry;
	};

	/**
	 * @brief One shadow epoch's verdict on the candidates: the entries whose casters it all drew (claimed), which the
	 * next frame's full-frustum cull leaves out of the cascade culls. Built with the claims (IndirectDraws: the shadow
	 * build), used once.
	 */
	struct SunExclusion
	{
		std::shared_ptr<const SunCandidates> candidates;
		std::vector<std::uint8_t> excluded;  // per candidate index
		std::uint32_t excludedCount = 0;
		// Per candidate index, the stamp of the full-frustum cull that took the entry out (SunAccumulation's frame
		// stamp): the registration thunks' test. Written on the render thread, read by the registration jobs.
		std::unique_ptr<std::atomic<std::uint32_t>[]> removed;
		// Per geometry index, the stamp of the frame whose main registration has cleared the geometry's mask (+0x160 =
		// 0xFFFF: it reads the mask, then zeroes it): the engine's mask holds none of the sun's bits after it, so DCLF's
		// are not written again that frame.
		std::unique_ptr<std::atomic<std::uint32_t>[]> cleared;
	};

	/**
	 * @brief The engine's own work for the sun's cascades, taken away where DCLF already draws the result
	 * (docs/development/skyrim-engine-notes.md, "The sun's accumulation"). AE only.
	 *
	 * M1, registration-free accumulation (toggle `skipSunAccumulation`, CS_DCLF_SUN_SKIP): the engine still culls
	 * the sun's cascades, but a geometry DCLF claims for the sun's views is not registered. BSShadowDirectionalLight::
	 * Accumulate culls each cascade and hands every geometry it reaches to the accumulator's registration
	 * (FUN_1414b2140, called from FUN_140e28af0). That builds the geometry's shadow passes, which PassCapture then
	 * withholds one by one, and ORs the cascade's bit into the property's activeLightMask, which the main pass reads.
	 * For a claimed geometry the thunks on those calls replicate the registration's early-outs and its mask write,
	 * and skip the passes. Everything else calls the original, so an unclaimed caster is registered and drawn by
	 * the engine as before, and no frame-wide verdict is needed.
	 *
	 * The entry exclusion (toggle `excludeSunEntries`, CS_DCLF_SUN_EXCLUDE; drawcall-limit-fix.md, "The sun's cascades
	 * without DCLF's objects"): the cascade culls walk only the full-frustum processes' objectArray, so right after the
	 * full-frustum cull every entry (reference root, or a terrain block's multibound node) whose content DCLF draws
	 * entirely is taken out of it (SunCandidates, SunExclusion). Those entries are then never traversed, culled or
	 * registered for the cascades. What the registration would have written, the sun's bits in each geometry's
	 * activeLightMask, DCLF writes when the main camera registers the geometry: the cascades its bound meets, tested
	 * against the planes the engine's own cascade cull used (the Geometric rule; CS_DCLF_SUN_EXCLUDE=probe compares
	 * them with the engine's bits, and the casters the exclusion would lose, on a dry run).
	 *
	 * The timing probe (TEMP, CS_DCLF_SUN_TIMING=1) measures the render thread in the full-frustum cull
	 * (FUN_141511f30), in Accumulate, and in the sun's registrations.
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
		 * @brief Render thread, at the shadow epoch's claim publication: the exclusion the next full-frustum cull
		 * applies, once. Null: nothing is excluded.
		 */
		void PublishExclusion(std::shared_ptr<SunExclusion> a_exclusion) { pendingExclusion = std::move(a_exclusion); }

		/**
		 * @brief Render thread, from the end of the sun's Accumulate to the next full-frustum cull: whether the bound
		 * meets any of this frame's cascades, tested against the planes the engine's own cascade culls used (the
		 * Geometric rule). Unknown (the cascades were not captured this frame): nullopt.
		 */
		std::optional<bool> InSunCascades(const RE::NiBound& a_bound) const;
		/**
		 * @brief This frame's captured cascades as BuildDraws tests them (BuildDrawsLatch): per cascade, 6 planes and
		 * then 6 custom planes as (normal, constant), and the two active-plane masks (0 custom: none). Render thread,
		 * after the sun's Accumulate. Returns the number of cascades, 0 when none were captured this frame.
		 */
		std::uint32_t GpuCascades(std::uint32_t (&a_masks)[4][2], float (&a_planes)[4][12][4]) const;
		/** @brief Whether this frame's full-frustum cull applied the entry exclusion (its cascades will be captured). */
		bool ExclusionLive() const { return exclusionLive.load(std::memory_order_acquire); }
		/** @brief After the sun's Accumulate: every cascade's activeLightMask bit this frame. */
		std::uint32_t SunBits() const { return frameState.sunBits; }

		/** @brief CS_DCLF_SUN_EXCLUDE=probe: the exclusion runs dry, and DCLF's sun bits are compared with the engine's. */
		static bool ExclusionProbe();

		/**
		 * @brief [TEMP] CS_DCLF_SKYLIGHT_PARITY: the geometries the engine registered with a pass into Skylighting's
		 * occlusion map since the last call (render thread).
		 */
		std::vector<const RE::BSGeometry*> TakeSkyRegistrations() { return std::exchange(skyRegistrations, {}); }
		std::vector<const RE::BSGeometry*> skyRegistrations;

		struct Stats
		{
			std::uint32_t frames = 0;         // Accumulate calls for the sun
			std::uint32_t activeFrames = 0;   // ... with M1 active
			std::uint64_t skipped = 0;        // claimed geometries not registered (the mask written)
			std::uint64_t registered = 0;     // sun registrations passed to the engine
			std::uint64_t offThread = 0;      // sun registrations outside the Accumulate call (passed to the engine)
			std::int64_t fullFrustumTicks = 0, fullFrustumMax = 0;
			std::int64_t accumulateTicks = 0, accumulateMax = 0;
			std::int64_t registrationTicks = 0;  // inside Accumulate, the registrations (timing probe only)
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

		/** @brief The registration thunks' view of the sun's bits (job threads). */
		struct BitStats
		{
			std::atomic<std::uint64_t> written{ 0 };       // registrations of a geometry under a removed entry that took DCLF's bits
			std::atomic<std::uint64_t> withBits{ 0 };      // ... with at least one cascade's bit
			std::atomic<std::uint64_t> notReady{ 0 };      // ... before the cascades were known (must be 0)
			// CS_DCLF_SUN_EXCLUDE=probe, the main registrations of geometries under would-be-removed entries.
			std::atomic<std::uint64_t> compared{ 0 };
			std::atomic<std::uint64_t> agree{ 0 };
			std::atomic<std::uint64_t> engineOnly{ 0 };    // the engine set a cascade's bit, DCLF's test would not
			std::atomic<std::uint64_t> dclfOnly{ 0 };      // DCLF's test would set one the engine did not
			std::atomic<std::uint64_t> probeUnclaimed{ 0 };  // cascade registrations the engine made under would-be-removed entries that built a pass
			std::atomic<std::uint64_t> probeNoPass{ 0 };     // ... that built none (the mask only)
		};

	private:
		SunAccumulation() = default;

		struct Hooks;
		friend struct Hooks;

		/**
		 * @brief After the full-frustum cull, render thread: takes the published exclusion (once) and, when it still
		 * describes the scene, removes its excluded entries from the full-frustum processes' objectArray, which is all
		 * the cascade culls walk. Each process keeps at least one entry: the cascade cull sets its planes up from the
		 * first. Under the probe nothing is removed, but the entries are marked all the same.
		 */
		void ExcludeEntries(RE::BSShadowDirectionalLight* a_light);
		/**
		 * @brief Any thread: the geometry's index among the candidates when it is under an entry this frame's full-frustum
		 * cull removed (or would have), else ~0u.
		 */
		std::uint32_t RemovedGeometryIndex(const RE::BSGeometry* a_geometry) const;
		bool UnderRemovedEntry(const RE::BSGeometry* a_geometry) const { return RemovedGeometryIndex(a_geometry) != ~0u; }
		/**
		 * @brief A registration after the sun's Accumulate, any thread: a geometry under a removed entry gets the bits of
		 * the cascades its bound meets, ORed into its activeLightMask before GetRenderPasses reads it. Under the probe
		 * the bits are compared with the engine's instead.
		 */
		void ApplySunBits(RE::BSGeometry* a_geometry, bool a_clears);
		/** @brief The probe, render thread: a cascade registration the engine made for a would-be-removed entry. */
		void NoteProbeUnclaimed(RE::BSGeometry* a_geometry, std::uint32_t a_passes);

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
			bool probe = false;                       // dry: nothing removed, the bits compared instead
			std::array<Cascade, 4> cascades{};
			std::uint32_t cascadeCount = 0;
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
	};
}
