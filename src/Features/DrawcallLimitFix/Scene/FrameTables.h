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
		// Parallel to Tables::techniqueKeys: each technique row's constants (EvaluateTechnique: fog, settings, the shadow mask's
		// view), evaluated once a frame for every pipeline of its key and versioned (floats, bindings) only where they differ.
		struct TechniqueRow
		{
			TechniqueConstants value;
			std::uint32_t constantsVersion = 0, bindingVersion = 0;
			std::uint32_t evaluated = ~0u;  // the frame
			bool valid = false;
		};
		std::vector<TechniqueRow> techniques;

		// Parallel to Tables::materials: each slot's record as the frame draws it - the coordinator's record (copied when the slot's key
		// or Tables::materialVersion moves), with the frame's writes on it: the writer events' re-evaluations, the frame-sourced
		// components, the texture transform. Versions (the record, its frame floats) and the log the builds follow are the frame's.
		std::vector<MaterialRecord> materials;
		std::vector<std::uint32_t> materialVersion, materialFrameVersion;
		std::vector<std::pair<const RE::BSShaderMaterial*, std::uint32_t>> materialKeys;  // the key the slot's record is for
		std::vector<std::uint32_t> materialSource;                                        // the Tables::materialVersion copied
		EventLog<std::uint32_t> materialLog;
		std::uint32_t materialLogGeneration = 0;
		std::uint32_t materialsGeneration = ~0u;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::vector<std::uint32_t>> materialDependents;
		// The frame-sourced components (MaterialSources): one live sample per signature, applied to its slots when it differs from
		// the last; the slots keyed or rewritten since (materialFramePending) take it regardless.
		struct FrameSignature
		{
			std::vector<std::uint32_t> slots;
			std::uint32_t representative = ~0u;
			MaterialRecord applied;
			bool appliedValid = false;
		};
		ankerl::unordered_dense::map<std::uint32_t, FrameSignature> frameSignatures;
		std::vector<std::uint32_t> materialSignatureListed;  // signature + 1, 0 when unlisted
		std::vector<std::uint32_t> materialFramePending;
		// TexcoordOffset's watch (RefreshTextureTransforms): a slot is watched from its keying or its material's write until two
		// frames have passed and both buffers agree. transformWatchFrame: the frame it was keyed or written, 0 when unwatched.
		std::vector<std::uint32_t> transformWatch;
		std::vector<std::uint32_t> transformWatchFrame;
		// Slots a write could not be evaluated for yet: asked again, as if written again, until they are.
		std::vector<std::uint32_t> materialEvaluationsPending;

		void ResetMaterials()
		{
			materials.clear();
			materialVersion.clear();
			materialFrameVersion.clear();
			materialKeys.clear();
			materialSource.clear();
			materialDependents.clear();
			frameSignatures.clear();
			materialSignatureListed.clear();
			materialFramePending.clear();
			transformWatch.clear();
			transformWatchFrame.clear();
			materialEvaluationsPending.clear();
			materialLog.Invalidate();
			++materialLogGeneration;
		}
		void ResizeMaterials(std::size_t a_count)
		{
			materials.resize(a_count);
			materialVersion.resize(a_count, 0);
			materialFrameVersion.resize(a_count, 0);
			materialKeys.resize(a_count, { nullptr, 0u });
			materialSource.resize(a_count, ~0u);
			materialSignatureListed.resize(a_count, 0);
			transformWatchFrame.resize(a_count, 0);
		}
		void UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
		{
			if (const auto it = materialDependents.find(a_material); it != materialDependents.end()) {
				std::erase(it->second, a_slot);
				if (it->second.empty())
					materialDependents.erase(it);
			}
		}
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
				techniques.clear();
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

		/** @brief Sized to the tables' technique rows (never freed but with the tables): a new row holds no constants. */
		void SyncTechniques(std::size_t a_rows) { techniques.resize(a_rows); }
	};
}
