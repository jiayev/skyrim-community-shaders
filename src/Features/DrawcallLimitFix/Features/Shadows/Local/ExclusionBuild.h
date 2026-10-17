#pragma once

// The shadow payload's entry exclusions (Api::ShadowExclusionContribution: this feature's, for both modes, since the build is one): the
// next frame's sun exclusion (the cascades' mode, over the sun candidates) and the paraboloids' (the Point source's culls, over the light
// candidates), built by the shadow participant's payload on the snapshot builder, each with its own cache kept up to date by what moved.
// The exclusions are this feature's type (SunExclusion.h), opaque to Core, whose shadow epoch hands them back through the contribution's
// publish into their slots for the full-frustum cull (Features/Shadows/Sun's accumulation) and the point lights' culls (LocalLightCull).

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowExclusionBuild.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Features::Shadows::Local
{
	struct SunExclusionIndex;  // ExclusionBuild.cpp

	/**
	 * @brief The last sun exclusion's inputs and verdicts, kept up to date by what moved (BuildSunExclusion): the candidates (the
	 * entries whose versions moved), the change log (an object's flags, shadow reject or geometry) and the mode's inputs (the objects
	 * that joined or left them, while ShadowKept::Mode::membership moves: the kept build's moves, read whole only when the cache missed
	 * a build). Judged whole only for a new cache or tables generation, or behind the log's trim.
	 */
	struct SunExclusionCache
	{
		std::shared_ptr<const EntrySpace> candidates;
		LogCursor cursor;
		std::uint64_t membership = 0;
		std::uint64_t keptBuild = 0;  // the kept shadow build its inputs are as of (Api::ShadowExclusionBuild::keptBuild)
		std::vector<std::uint8_t> excluded;
		std::uint32_t excludedCount = 0;
		std::uint64_t version = 0;  // SunExclusion::version of excluded
		bool valid = false;
		// What the verdicts are updated by: the objects by geometry, the inputs, the published exclusion, the stamped words and the
		// forward chains (shared_ptr: the type is the builder's own).
		std::shared_ptr<SunExclusionIndex> index;
		// Since the last report: builds; of them kept (updated by what moved; translated: for newer candidates) and, of those, the
		// same exclusion published again (nothing moved); the rest judged whole. And the builds' time.
		std::uint64_t builds = 0, reused = 0, translated = 0, same = 0;
		double buildMs = 0.0, buildMaxMs = 0.0;
	};

	/**
	 * @brief The next frame's entry exclusion of a_build's mode, from its candidates and the mode's inputs, which are the set's
	 * casters (SceneSet.h): a candidate stays in the mode's culls when one of its table objects casts (no kObjectNoShadow) and is
	 * not an input, since the engine must then still draw it. Null without candidates. The sun's (the cascades' mode) and the
	 * paraboloids' (with the reach words) alike.
	 */
	std::shared_ptr<SunExclusion> BuildSunExclusion(const Api::ShadowExclusionBuild& a_build, SunExclusionCache* a_cache = nullptr);

	/**
	 * @brief Each mode's cache, by its source (Sun: the cascades', Point: the paraboloids'; another: none): the snapshot builder's, the
	 * builds' in frame order; the report reads and resets its counters there (Diagnostics/Reports/DrawsProducer.cpp).
	 */
	SunExclusionCache& ExclusionCacheOf(Api::ShadowSource a_source);

	/** @brief The shadow exclusion contribution's build (Api::ShadowExclusionContribution::build): a_build's exclusion, with its source's cache. */
	void BuildExclusion(Api::ShadowExclusionBuild& a_build);
}
