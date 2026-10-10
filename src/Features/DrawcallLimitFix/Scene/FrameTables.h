#pragma once

#include <cstdint>
#include <vector>

#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Scene/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/Records.h"

namespace DCLF
{
	/**
	 * @brief The per-frame engine values the frame evaluates (RefreshFrameConstants, the render thread), kept out of the tables
	 * (dclf-async-publication.md, "Step 6"): the coordinator owns the tables and publishes them, the frame owns these. Indexed
	 * by the tables' slots; a slot whose key changed (reused for another pipeline) or tables made again is evaluated afresh.
	 * Versions come from the frame's own counter, so they never collide with the tables'.
	 *
	 * Render thread writes (Prepass, and EarlyPrepass for new pipelines); the frame's builds read (kicked after the writes).
	 */
	struct FrameTables
	{
		// Parallel to Tables::pipelines: the PerGeometry block of each pipeline (its template object's lighting pass, evaluated
		// in full once, its frame globals refreshed every frame), whether it holds one, its version, and the key it is for.
		std::vector<GeometryConstants> geometryConstants;
		std::vector<std::uint8_t> geometryConstantsValid;
		std::vector<std::uint32_t> pipelineConstantsVersion;
		std::vector<PipelineKey> pipelineKeys;
		std::vector<std::uint32_t> pipelineBindings;  // the slot's Tables::pipelineBindingVersion its block is for
		// The technique rows are the coordinator's (Tables::techniqueConstants, T6b2c): not the frame's.

		// The material records are the scene work's (Tables::materials, T6b2c step 7: SceneStore::RefreshMaterialRecords), drawn from the
		// installed publication: not the frame's.

		std::uint32_t tablesGeneration = ~0u;
		std::uint32_t versionCounter = 0;

		std::uint32_t NextVersion() { return ++versionCounter; }

		/** @brief Sized and keyed to the tables' pipelines: a slot whose key or binding moved, or tables made again, holds no block. */
		void SyncPipelines(const std::vector<PipelineKey>& a_pipelines, const std::vector<std::uint32_t>& a_bindings, std::uint32_t a_tablesGeneration)
		{
			if (a_tablesGeneration != tablesGeneration) {
				tablesGeneration = a_tablesGeneration;
				geometryConstants.clear();
				geometryConstantsValid.clear();
				pipelineConstantsVersion.clear();
				pipelineKeys.clear();
				pipelineBindings.clear();
			}
			const std::size_t count = a_pipelines.size();
			geometryConstants.resize(count);
			geometryConstantsValid.resize(count, 0);
			pipelineConstantsVersion.resize(count, 0);
			pipelineKeys.resize(count);
			pipelineBindings.resize(count, 0);
			for (std::size_t p = 0; p < count; ++p) {
				const std::uint32_t binding = p < a_bindings.size() ? a_bindings[p] : 0u;
				if (!(pipelineKeys[p] == a_pipelines[p]) || pipelineBindings[p] != binding) {
					pipelineKeys[p] = a_pipelines[p];
					pipelineBindings[p] = binding;
					geometryConstantsValid[p] = 0;
				}
			}
		}
	};
}
