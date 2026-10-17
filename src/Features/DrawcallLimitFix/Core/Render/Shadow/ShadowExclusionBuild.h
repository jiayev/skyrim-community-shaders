#pragma once

// What the shadow exclusion contribution's build is handed (Api::ShadowExclusionContribution, Api/ShadowViews.h): one mode's exclusion
// build out of the shadow participant's payload (ShadowEngine::BuildPayload), the candidates it judges and the mode's inputs as the
// payload has them, and the exclusion it answers (opaque to Core). Self-contained: the contribution's feature includes it.

#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Api
{
	/**
	 * @brief One mode's exclusion build: the Sun source's (the cascades' mode, over the sun candidates) or the Point source's (the
	 * paraboloid mode, over the light candidates, with the reach words), from the build's tables and the payload's list. The snapshot
	 * builder's thread (a pool task each when both build).
	 */
	class ShadowExclusionBuild
	{
	public:
		ShadowExclusionBuild(ShadowSource a_source, const std::shared_ptr<const EntrySpace>& a_candidates, const SceneTables& a_tables,
			const KeptView<Draws::DrawInput>& a_region, const std::vector<Draws::DrawInput>& a_list) :
			source(a_source),
			candidates(a_candidates),
			tables(a_tables),
			region(a_region),
			list(a_list)
		{}

		ShadowSource source;
		const std::shared_ptr<const EntrySpace>& candidates;
		const SceneTables& tables;
		std::uint32_t modeBits = 0;             // the mode's views' bits (ShadowPayload::modeBits): what selects its inputs
		std::uint64_t membership = 0;           // ShadowPayload::membership of the mode (0 without the kept state)
		std::uint32_t frameNumber = 0;          // the payload's inputs' frame
		std::uint32_t tablesGeneration = 0;     // and tables generation
		// The objects that joined and left the mode's region inputs in the kept build keptBuild (ShadowPayload::keptBuild, joined, left;
		// 0: none): what a cache that read the inputs as of the build before follows. movesValid false: the build retook the mode whole.
		std::uint64_t keptBuild = 0;
		bool movesValid = false;
		std::span<const std::uint32_t> joined, left;
		std::shared_ptr<void> result;           // the build's answer, the contribution's own type (null: none)

		/** @brief The paraboloid mode's (the Point source's culls' reach words). */
		bool Paraboloid() const { return source == ShadowSource::Point; }
		/** @brief The mode's inputs, as ShadowPayload::ForEachInput(mode) visits them: the region's, then the frame's own. */
		template <class F>
		void ForEachInput(F&& a_visit) const
		{
			if (region.elements)
				for (const auto& input : *region.elements)
					if (input.view.mask & modeBits)
						a_visit(input);
			for (const auto& input : list)
				if (input.view.mask & modeBits)
					a_visit(input);
		}
		/** @brief ForEachInput's first part: the mode's inputs of the region. */
		template <class F>
		void ForEachRegionInput(F&& a_visit) const
		{
			if (region.elements)
				for (const auto& input : *region.elements)
					if (input.view.mask & modeBits)
						a_visit(input);
		}
		/** @brief ForEachInput's second part: the mode's inputs of the frame's own list (face shapes; a few). */
		template <class F>
		void ForEachListInput(F&& a_visit) const
		{
			for (const auto& input : list)
				if (input.view.mask & modeBits)
					a_visit(input);
		}

	private:
		const KeptView<Draws::DrawInput>& region;
		const std::vector<Draws::DrawInput>& list;
	};
}
