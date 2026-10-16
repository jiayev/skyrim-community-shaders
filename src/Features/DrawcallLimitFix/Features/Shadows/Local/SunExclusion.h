#pragma once

// The shadow payload's entry exclusions (the sun's cascades', the paraboloids'; built by ExclusionBuild.h), their translations to later
// candidates, and the slots they wait in: the sun's for the next full-frustum cull (Features/Shadows/Sun's accumulation), the
// paraboloids' for the next frame's point-light culls (LocalLightCull). Core holds them only opaquely (the shadow payload's, published
// through the exclusion contribution's publish) and reads the sun's pending one through its listExclusion (Api/ShadowViews.h).

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

namespace DCLF::Features::Shadows::Local
{
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

		std::shared_ptr<const EntrySpace> candidates;
		std::vector<std::uint8_t> excluded;  // per candidate index
		std::uint32_t excludedCount = 0;
		// Changes only when excluded does (Api::ShadowListExclusion::version).
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
		// Paraboloid mode (the Point source's culls, LocalLightCull): per candidate, three stamped words (reach kinds) of the point lights whose cull reached and
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
	std::shared_ptr<SunExclusion> TranslateExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const EntrySpace>& a_to,
		bool a_newVersion, bool a_lightReach, std::vector<std::uint32_t>* a_changed = nullptr, const SunExclusion* a_stamps = nullptr);

	/**
	 * @brief a_exclusion as a verdict on a_to, as the builder translated it (its forward links, made when it built for a_to: the same
	 * verdict TranslateExclusion gives); a_exclusion itself when it is for a_to. Null when the builder made none for a_to (it did not
	 * build that mode's exclusion for those candidates). a_changed: appends each link's changed entries. O(links), any thread.
	 */
	std::shared_ptr<SunExclusion> ForwardExclusion(const std::shared_ptr<SunExclusion>& a_exclusion, const std::shared_ptr<const EntrySpace>& a_to,
		std::vector<std::uint32_t>* a_changed = nullptr);

	/**
	 * @brief The sun's exclusion between the shadow epoch that published it and the full-frustum cull that applies it (once). Render
	 * thread only.
	 */
	class SunExclusionSlot
	{
	public:
		static SunExclusionSlot& Get();

		/** @brief After the shadow epoch (the exclusion contribution's publish): the exclusion the next full-frustum cull applies once (null: none). */
		void Publish(std::shared_ptr<SunExclusion> a_exclusion) { pending = std::move(a_exclusion); }
		/** @brief The exclusion the next full-frustum cull will apply (null: none published). */
		std::shared_ptr<const SunExclusion> Pending() const { return pending; }
		/** @brief The full-frustum cull: the pending exclusion, the slot left empty. */
		std::shared_ptr<SunExclusion> Take() { return std::move(pending); }
		/**
		 * @brief Frame start (the exclusion contribution's frameStart, before the scene lists' filter): the pending exclusion as the builder translated it to a_candidates
		 * (ForwardExclusion: a pointer swap). None made (the builder did not build the mode for them): the pending one stays as it is,
		 * a verdict on other candidates, which the full-frustum cull does not apply (stale).
		 */
		void BeginFrame(const std::shared_ptr<const EntrySpace>& a_candidates);

		/** @brief Frame starts that took the builder's translation (BeginFrame), and those that found none for the frame's candidates. */
		struct ForwardCounts
		{
			std::uint32_t forwarded = 0;
			std::uint32_t unforwarded = 0;
		};
		/** @brief The counts since the last call, reset. */
		ForwardCounts TakeForwardCounts() { return std::exchange(counts, {}); }

	private:
		SunExclusionSlot() = default;

		std::shared_ptr<SunExclusion> pending;
		ForwardCounts counts;
	};

	/**
	 * @brief The paraboloid mode's exclusion between the shadow epoch that published it and the next frame's point-light culls (the Point
	 * source's, LocalLightCull), whose frameStart takes it (Api::ShadowSourceContribution). Published only while the Point source's
	 * culls are installed (Extensions::SourceCullsInstalled). Render thread only.
	 */
	class ParabolicExclusionSlot
	{
	public:
		static ParabolicExclusionSlot& Get();

		/** @brief After the shadow epoch (the exclusion contribution's publish): next frame's exclusion (null: none). */
		void Publish(std::shared_ptr<SunExclusion> a_exclusion) { pending = std::move(a_exclusion); }
		/** @brief The exclusion published last (null: none); it stays until the next Publish. */
		std::shared_ptr<SunExclusion> Pending() const { return pending; }

	private:
		ParabolicExclusionSlot() = default;

		std::shared_ptr<SunExclusion> pending;
	};
}
