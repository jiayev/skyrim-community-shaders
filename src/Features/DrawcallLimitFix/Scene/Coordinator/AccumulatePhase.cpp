// The coordinator's accumulate phase (scene lane; join evaluations across the pool): the membership joins bound (pipeline and
// material slots, records, the patch), residency kept or dropped, pipeline constants made from the posted pipeline frame, and the
// shadow dependency index refreshed.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"

#include <cassert>

#include "Features/ExtendedTranslucency.h"
#include "Features/SubsurfaceScattering.h"

namespace DCLF::Scene
{
	namespace
	{
		// The record flags a technique's objects carry: a feature's contribution (Api::TechniqueContribution; Features/TreeWind's
		// kObjectTreeAnim for technique 12). Read once, the features installed before any scene work.
		std::uint32_t TechniqueFlags(std::uint32_t a_technique)
		{
			static const auto& techniques = Extensions::RegisteredTechniques();
			return Extensions::TechniqueObjectFlags(techniques, a_technique);
		}

		// The property flags whose objects take only the light volumes that light land (Api::LightVolumesContribution::landscapeFlags:
		// Features/LocalLights' kLandscape and kNoLODLandFade; none without the contribution). Read once, like the techniques.
		std::uint64_t LandscapeLightFlags()
		{
			static const std::uint64_t flags = Extensions::LightVolumes().landscapeFlags;
			return flags;
		}

		/**
		 * @brief Subsurface Scattering's IsBeastRace (BSLightingShader_SetupSkin) for a face property (kFace or kFaceGenRGBTint), from the
		 * node record (BeastOf). Lighting.hlsl reads it only under SKIN.
		 */
		bool BeastRaceFace(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
		{
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			const auto& sss = globals::features::subsurfaceScattering;
			if (!sss.loaded || !sss.isBeastRaceKeyword || !(a_property.flags & (static_cast<std::uint64_t>(Flag::kFace) | static_cast<std::uint64_t>(Flag::kFaceGenRGBTint))))
				return false;
			return a_leaf.node->beast;
		}

		/** @brief ExtendedTranslucency::MaterialModelOf, from the records. */
		std::uint32_t TranslucencyModelOf(const SceneCapture::LeafView& a_leaf)
		{
			using Model = ExtendedTranslucency::MaterialModel;
			const auto* alpha = a_leaf.alpha && a_leaf.alpha->rtti == globals::rtti::NiAlphaPropertyRTTI.get() ? a_leaf.alpha : nullptr;
			const auto* light = a_leaf.property && a_leaf.property->rtti == globals::rtti::BSLightingShaderPropertyRTTI.get() ? a_leaf.property : nullptr;
			// Only blended geometry: an alpha property that blends, or a Lighting property's alpha below one.
			if (!(light && light->alpha < 0.999f) && (!alpha || !(alpha->flags & 1u)))
				return Model::DescriptorDisabled;
			const auto value = a_leaf.geometry->anisotropic;
			if (value == SceneCapture::GeometryRecord::kNoExtra) {
				const auto& feature = globals::features::extendedTranslucency;
				return !feature.settings.SkinnedOnly || a_leaf.geometry->skin ? Model::DescriptorUseDefault : Model::DescriptorDisabled;
			}
			if (value == SceneCapture::GeometryRecord::kWrongExtra)
				return Model::DescriptorDisabled;
			const auto material = static_cast<std::uint32_t>(value) & ExtendedTranslucency::ExtraFeatureDescriptorMask;
			return material == Model::Disabled ? Model::DescriptorDisabled : material;
		}
	}

	const AccumulatedPass* Coordinator::FindAccumulatedPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedPasses.find(a_geometry);
		return it == accumulatedPasses.end() ? nullptr : &it->second;
	}

	const AccumulatedPass* Coordinator::FindAccumulatedLayerPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedLayerPasses.find(a_geometry);
		return it == accumulatedLayerPasses.end() ? nullptr : &it->second;
	}

	void Coordinator::ApplyAccumulatePatch(const AccumulatePatch& a_patch)
	{
		const std::uint32_t objectId = a_patch.object;
		const AccumulateSnapshot before(tables, objectId);
		auto& object = tables.objects[objectId];
		object.materialIndex = a_patch.material;
		if (materialMember.size() <= a_patch.material)
			materialMember.resize(std::size_t(a_patch.material) + 1, 0);
		materialMember[a_patch.material] = 1;
		object.pipelineIndex = a_patch.pipeline;
		object.flags = a_patch.flags;
		tables.fadeDistance[objectId] = a_patch.fadeDistance;
		tables.draws[objectId].pipelineIndex = a_patch.pipeline;
		tables.ancestorValue[objectId] = a_patch.ancestorValue;
		bool extrasWritten = false;
		if (a_patch.projectedUV || a_patch.landBlend) {
			stats.projectedUV += a_patch.projectedUV ? 1 : 0;
			stats.landBlend += a_patch.landBlend ? 1 : 0;
			if (tables.extraOffset[objectId] == kNoExtraRows)
				tables.extraOffset[objectId] = tables.AllocateExtras();
			// The static parts (the draw adds the frame's: ExtrasFrame). Rows rewritten in a kept block change no offset, so they are
			// noted below or the uploads would never send them.
			extrasWritten = WriteObjectExtras(objectId);
		} else {
			tables.FreeExtras(objectId);
		}
		tables.residentSlot[objectId] = 1;
		before.NoteWrite(tables, objectId);
		if (extrasWritten)
			tables.NoteChange(objectId, kChangeExtras);
		// Sampled by the next frame's values (the earliest it can join the set).
		NameShading(objectId, true);
		// The features' components list what they keep by the patch (Features/Decals: the member decals' order and candidates).
		ComponentsAccumulated(objectId, a_patch.decalKey);
	}

	void Coordinator::TakeAccumulateInputs()
	{
		// Latest wins (a replaced post is dropped on the render thread; neither holds engine references).
		if (auto posted = channels.ancestorMapPosted.exchange(nullptr, std::memory_order_acq_rel)) {
			ancestorMap = posted->map;
			ancestorMapGeneration = posted->generation;
			channels.accumulateInputs.ancestorMapsTaken.fetch_add(1, std::memory_order_relaxed);
		}
		if (auto posted = channels.pipelineFramePosted.exchange(nullptr, std::memory_order_acq_rel)) {
			scenePipelineFrame = std::move(posted);
			channels.accumulateInputs.pipelineFramesTaken.fetch_add(1, std::memory_order_relaxed);
		}
		Diagnostics::Notify<Diagnostics::ScenePoint::AccumulateInputsTaken>();
	}

	void Coordinator::MakeNewPipelineConstants()
	{
		// A used pipeline without a current block gets one from the newest posted pipeline frame (PostPipelineFrame; the port is pure).
		// It draws until the render thread's own block supersedes it (RefreshFrameConstants). Without a sample yet the slot waits.
		if (!scenePipelineFrame) {
			for (std::size_t word = 0; word < tables.usedPipelineBits.size(); ++word)
				for (std::uint64_t remaining = tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1)
					if (const std::size_t p = word * 64 + static_cast<std::size_t>(std::countr_zero(remaining)); p < tables.pipelines.size() && !tables.PipelineConstantsCurrent(p))
						channels.accumulateInputs.pipelineBlocksWaited.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		bool wrote = false;
		for (std::size_t word = 0; word < tables.usedPipelineBits.size(); ++word)
			for (std::uint64_t remaining = tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::size_t p = word * 64 + static_cast<std::size_t>(std::countr_zero(remaining));
				if (p >= tables.pipelines.size() || p >= tables.pipelineConstants.size() || p >= tables.pipelineBindingVersion.size() || tables.PipelineConstantsCurrent(p))
					continue;
				GeometryConstants constants{};
				if (!GeometryPort::PipelineGeometryConstants(tables.pipelines[p].passDescriptor, GeometryPort::kMainPassRenderFlags, *scenePipelineFrame, constants))
					continue;
				auto& row = tables.pipelineConstants[p];
				row.constants = constants;
				row.key = tables.pipelines[p];
				row.binding = tables.pipelineBindingVersion[p];
				row.version = tables.NextVersion();
				row.valid = true;
				wrote = true;
				channels.accumulateInputs.pipelineBlocksMade.fetch_add(1, std::memory_order_relaxed);
			}
		if (wrote)
			tables.NoteConstantsWrite();
	}

	Coordinator::AccumulateInputStats Coordinator::TakeAccumulateInputStats()
	{
		auto& c = channels.accumulateInputs;
		AccumulateInputStats s;
		s.ancestorMapsPosted = c.ancestorMapsPosted.exchange(0, std::memory_order_relaxed);
		s.ancestorMapsTaken = c.ancestorMapsTaken.exchange(0, std::memory_order_relaxed);
		s.pipelineFramesPosted = c.pipelineFramesPosted.exchange(0, std::memory_order_relaxed);
		s.pipelineFramesTaken = c.pipelineFramesTaken.exchange(0, std::memory_order_relaxed);
		s.pipelineBlocksMade = c.pipelineBlocksMade.exchange(0, std::memory_order_relaxed);
		s.pipelineBlocksWaited = c.pipelineBlocksWaited.exchange(0, std::memory_order_relaxed);
		return s;
	}

	template <class Result, class Evaluate>
	void Coordinator::EvaluateJoins(std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate)
	{
		// By chunks of kFanoutGrain on the pool or here, each under a ShardScope.
		a_results.clear();
		a_results.resize(a_count);
		auto body = [&](std::size_t a_begin, std::size_t a_end) {
			ShardScope scope(*this, a_pool);
			for (std::size_t i = a_begin; i < a_end; ++i)
				a_evaluate(i, a_results[i]);
		};
		if (a_pool) {
			SceneScheduler::Executor().ParallelFor("CS.DCLF.Accumulate.Joins", a_count, kFanoutGrain, body);
		} else {
			for (std::size_t begin = 0; begin < a_count; begin += kFanoutGrain)
				body(begin, std::min(a_count, begin + kFanoutGrain));
		}
	}

	void Coordinator::BuildAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Tables");
		if (!sceneBuilt)
			return;  // a load screen, or the feature installed mid-frame: nothing to patch
		ShardScope passScope(*this, false);
		PartTimer timer(stats.partMs, &stats.accumulatePartMs);
		std::uint32_t fadingThisFrame = 0;
		TracyCZoneN(captureZone, "CS.DCLF.Accumulate.Capture", true);
		Diagnostics::Notify<Diagnostics::ScenePoint::JoinsBegin>();
		// The pass tables hold only this frame's membership joins.
		accumulatedPasses.clear();
		accumulatedLayerPasses.clear();
		residentJoining.clear();
		residentLayerJoining.clear();
		BindByMembership();
		timer.Add(BuildPart::Walk);

		TracyCZoneEnd(captureZone);

		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8) | (decalBiasMode[3] << 16);
		const bool ancestorValueOn = Extensions::AncestorValueOn();
		// The cached derivation is served and also recomputed, and the two compared.
		const bool derivedProbe = Policy::RecomputeCachedVerdicts(Policy::Verdicts::Derivation, passSerial);

		// Only the joins; every other record keeps its binding, or has none and is only a culling candidate.
		accumulateOrder.clear();
		TracyCZoneN(orderZone, "CS.DCLF.Accumulate.Order", true);
		accumulateOrder.reserve(accumulatedPasses.size());
		for (auto& [passGeometry, pass] : accumulatedPasses) {
			auto* mutableGeometry = const_cast<RE::BSGeometry*>(passGeometry);
			auto trackedIt = tracked.find(mutableGeometry);
			if (trackedIt != tracked.end())
				accumulateOrder.push_back({ mutableGeometry, &trackedIt->second, &pass });
		}
		for (auto& [passGeometry, pass] : accumulatedLayerPasses) {
			auto* mutableGeometry = const_cast<RE::BSGeometry*>(passGeometry);
			auto trackedIt = tracked.find(mutableGeometry);
			if (trackedIt != tracked.end())
				accumulateOrder.push_back({ mutableGeometry, &trackedIt->second, &pass, true });
		}
		timer.Add(BuildPart::PassLookup);
		TracyCZoneEnd(orderZone);
		TracyCZoneN(objectsZone, "CS.DCLF.Accumulate.Objects", true);
		// One join (a base record or its layer's). EvaluateJoin ran before, on the pool for a burst; here, in order, everything that
		// reads or writes slots: the cache's slot half, pipeline and material slots and records, the derivation cache, the patch.
		auto join = [&](const OrderEntry& a_entry, JoinResult& a_result) {
			RE::BSGeometry* geometry = a_entry.geometry;
			Tracked* trackedEntry = a_entry.tracked;
			const AccumulatedPass* accumulated = a_entry.accumulated;
			const bool layer = a_entry.layer;
			timer.Add(BuildPart::LoopTail);
			if (a_result.kind == JoinResult::Kind::Skip)
				return;
			// Only records written this frame with verdict None, never shadow-only.
			const std::uint32_t objectId = layer ? trackedEntry->layerSlot : trackedEntry->objectId;
			auto& object = tables.objects[objectId];
			// The cached derivation is used when its witnesses match and its slots still carry their keys (earlier joins in this loop
			// may have re-keyed them).
			auto& derived = layer ? trackedEntry->layerDerived : trackedEntry->derived;
			bool derivedHit = a_result.derivedWitness && derived.pipelineSlot < tables.pipelines.size() && tables.pipelineSlots.Alive(derived.pipelineSlot) &&
			                  tables.pipelines[derived.pipelineSlot] == derived.key &&
			                  derived.materialSlot < tables.materialSlotKey.size() && tables.materialSlots.Alive(derived.materialSlot) &&
			                  tables.materialSlotKey[derived.materialSlot] == std::pair{ derived.material, derived.descriptors.pass };
			// Witnesses held but slots gone: classify here.
			if (a_result.derivedWitness && !derivedHit && !a_result.classified)
				EvaluateJoin(a_entry, true, a_result);
			const auto& leaf = a_result.leaf;
			auto* property = a_result.property;
			auto* witnessProperty = property;
			const auto* witnessMaterial = a_result.material;
			const std::uint8_t fadeState = a_result.fadeState;
			const bool alphaBelowOne = a_result.alphaBelowOne;
			const std::uint32_t geometrySlot = a_result.geometrySlot;

			LightingDescriptors descriptors;
			const Ineligible reason = a_result.reason;
			if (derivedHit && !derivedProbe) {
				descriptors = derived.descriptors;
				++stats.derivedHits;
			} else {
				descriptors = a_result.descriptors;
			}
			timer.Add(BuildPart::ClassifyStatic);
			// The histogram is the scene phase's; a different verdict here moves the object between buckets.
			if (!layer && reason != trackedEntry->candidateReason) {
				--stats.ineligible[static_cast<std::size_t>(trackedEntry->candidateReason)];
				++stats.ineligible[static_cast<std::size_t>(reason)];
			}
			if (reason != Ineligible::None) {
				if (!layer) {
					trackedEntry->accumulateReason = reason;
					trackedEntry->accumulateReasonFrame = sceneFrame;
					trackedEntry->accumulateReasonPass = passSerial;
				}
				// A record but no bindings: stays native (kObjectNoBindings).
				derived.valid = false;
				return;
			}
			timer.Add(BuildPart::ClassifyFrame);


			std::uint32_t pipelineSlot = Tables::kSlotFree, materialSlot = Tables::kSlotFree, staticFlags = 0;
			PipelineKey key{};
			if (derivedHit && !derivedProbe) {
				pipelineSlot = derived.pipelineSlot;
				materialSlot = derived.materialSlot;
				staticFlags = derived.staticFlags;
				key = derived.key;
				tables.MarkMaterialUsed(materialSlot);
				joinMarkedMaterials.push_back(materialSlot);
				// A pipeline's template is always a member's property (KeepResidentsAlive refreshes it whenever the pipeline's first member or its held property changes), so a slot never
				// points at a freed property. Its constants are evaluated from it at Prepass (RefreshFrameConstants).
				if (!tables.PipelineUsed(pipelineSlot)) {
					tables.MarkPipelineUsed(pipelineSlot);
					joinMarkedPipelines.push_back(pipelineSlot);
					tables.geometryTemplate[pipelineSlot] = property;
				}
				timer.Add(BuildPart::DedupHit);
			} else {
				const auto* alpha = layer ? nullptr : leaf.alpha;
				const bool alphaBlending = alpha && (alpha->flags & 1u);  // GetAlphaBlending
				key = a_result.key;
				auto pipelineIt = pipelineIndex.find(key);
				const bool newPipeline = pipelineIt == pipelineIndex.end();
				if (!newPipeline && !tables.PipelineUsed(pipelineIt->second)) {
					// The slot's first member: its property is the template.
					tables.MarkPipelineUsed(pipelineIt->second);
					joinMarkedPipelines.push_back(pipelineIt->second);
					tables.geometryTemplate[pipelineIt->second] = property;
				}
				if (newPipeline) {
					timer.Add(BuildPart::Dedup);
					const std::uint32_t slot = AllocatePipelineSlot();
					tables.pipelines[slot] = key;
					// PerGeometry values (the engine's SetupGeometry) are evaluated per frame from the template (FrameTables).
					tables.geometryTemplate[slot] = property;

					// A port's key without technique rows reads no Lighting technique row (the Effect port's: Effect.hlsl); it holds the plain
					// technique's (no shadow mask).
					const auto& rules = Extensions::PortRules(descriptors.port);
					tables.pipelineTechnique[slot] = TechniqueRowFor(rules.techniqueRows ? descriptors.pass : 0u);
					stats.shadowMaskPipelines += tables.TechniqueShadowMask(slot) ? 1 : 0;

					PipelinePermutation permutation;
					permutation.vertexShaderDescriptor = descriptors.rawVertex;
					permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
					permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
					// AdditiveLighting (State::UpdateLightingShaderPermutation): destination blend ONE. Only blended decals (group 2) and
					// forward objects (FUN_1414b39d0's render flag 4) apply their alpha property; the key carries the blend mode.
					if ((descriptors.decalGroup == 2 || descriptors.forward) && alphaBlending &&
						static_cast<RE::NiAlphaProperty::AlphaFunction>((alpha->flags >> 5) & 0xF) == RE::NiAlphaProperty::AlphaFunction::kOne)  // GetDestBlendMode
						permutation.extraShaderDescriptor |= static_cast<std::uint32_t>(State::ExtraShaderDescriptors::AdditiveLighting);
					// Extended Translucency's material model, as its SetupGeometry hook sets it (the key carries it).
					permutation.extraFeatureDescriptor = globals::features::extendedTranslucency.loaded ?
					                                         (ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ RasterTranslucency(key.rasterFlags))
					                                             << ExtendedTranslucency::ExtraFeatureDescriptorShift :
					                                         0u;
					// A raw-permutation port's pass, as BSShader_BeginTechnique sets it (the Effect port's): raw vertex descriptor (Effect.hlsl
					// tests SkyObject), the raw pixel bits the lookup dropped (GrayscaleToColor/ToAlpha, IgnoreTexAlpha), InWorld, no feature bits.
					if (rules.rawPermutation) {
						permutation.vertexShaderDescriptor = descriptors.rawVertex;
						permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
						permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
						permutation.extraFeatureDescriptor = 0u;
					}
					tables.permutations[slot] = permutation;
					tables.pipelineBindingVersion[slot] = tables.NextVersion();
					// Its technique mask is looked up with the next bindings update (a slot keyed again may keep its used bit).
					sharedBindings.MarkMask(slot);
					pipelineIt = pipelineIndex.emplace(key, slot).first;
					timer.Add(BuildPart::PipelineEval);
				}
				pipelineSlot = pipelineIt->second;
				tables.MarkPipelineUsed(pipelineSlot);
				joinMarkedPipelines.push_back(pipelineSlot);

				// Material state as the engine's SetupMaterial produces it for this pass descriptor.
				const auto* material = witnessMaterial;
				auto materialIt = materialIndex.find(std::pair{ material, descriptors.pass });
				if (materialIt == materialIndex.end()) {
					timer.Add(BuildPart::Dedup);
					// The material port's record from the material's capture. Without one the render thread is asked once
					// (ServeMaterialRequests) and the join waits, native meanwhile.
					auto& ms = materialSnapshotStats;
					const auto held = materialSnapshots.find(material);
					if (held == materialSnapshots.end() || !held->second.held) {
						if (materialRequested.insert(material).second) {
							MaterialRequest request;
							request.property.reset(property);
							request.material = material;
							request.pass = descriptors.pass;
							channels.materialRequests.Push(std::move(request));
							++ms.requested;
						}
						bindRetry.push_back(objectId);
						++residentStats.materialWaits;
						derived.valid = false;
						return;
					}
					held->second.frame = sceneFrame;
					// Evaluated before the merges (PrepareJoinMaterials), else here (a join classified only in its merge, or no fan-out).
					const MaterialEvaluation* evaluation = nullptr;
					std::optional<MaterialEvaluation> here;
					if (const auto prepared = joinMaterialPrepared.find(std::pair{ material, descriptors.pass }); prepared != joinMaterialPrepared.end()) {
						evaluation = &joinMaterialResults[prepared->second];
						++materialRecordStats.joinPrepared;
					} else {
						EvaluateMaterial(held->second.held->snapshot, descriptors.pass, FrameGlobals::Current().material, kNoMaterialSlot, here.emplace());
						evaluation = &*here;
						++materialRecordStats.joinInline;
					}
					if (evaluation->outcome != MaterialEvaluation::Outcome::Made) {
						// An input the frame lacks yet (e.g. Advanced Skin's textures): retry later. Else a class the port does not cover:
						// native, counted.
						derived.valid = false;
						if (evaluation->outcome == MaterialEvaluation::Outcome::Retry) {
							bindRetry.push_back(objectId);
							++residentStats.materialWaits;
							return;
						}
						++ms.uncovered;
						return;
					}
					++ms.made;
					// As records hold it: the character light's t11 view is the frame's, never the record's (EvaluateMaterial).
					const MaterialRecord& record = evaluation->record;
					const std::uint32_t slot = AllocateMaterialSlot();
					materialOwners.resize(tables.materials.size());
					// A second count on the material (alive: the capture holds one).
					if (materialOwners[slot])
						EngineReleases::PushOwner(std::make_shared<MaterialReference>(std::move(materialOwners[slot])));
					materialOwners[slot].reset(const_cast<RE::BSShaderMaterial*>(material));
					tables.materials[slot] = record;
					tables.materialVersion[slot] = ++materialVersions;
					tables.NoteMaterial(slot);
					tables.materialSlotKey[slot] = std::pair{ material, descriptors.pass };
					materialIt = materialIndex.emplace(std::pair{ material, descriptors.pass }, slot).first;
					ListMaterialDependent(material, slot);
					NoteMaterialRecord(slot, held->second.held->snapshot);
					// Its texture transforms counted as watched (nothing follows them: the frame values patch the rows).
					++materialRecordStats.transformsWatched;
					timer.Add(BuildPart::MaterialEval);
				}
				materialSlot = materialIt->second;
				tables.MarkMaterialUsed(materialSlot);
				joinMarkedMaterials.push_back(materialSlot);
				timer.Add(BuildPart::DedupHit);

				staticFlags = a_result.staticFlags;
				{
					if (derivedHit && derivedProbe) {
						++stats.derivedChecked;
						const bool same = derived.pipelineSlot == pipelineSlot && derived.materialSlot == materialSlot &&
						                  derived.staticFlags == staticFlags && derived.key == key && derived.descriptors.vertex == descriptors.vertex &&
						                  derived.descriptors.pixel == descriptors.pixel && derived.descriptors.pass == descriptors.pass;
						if (!same && stats.derivedDiffers++ == 0)
							logger::warn("[DCLF] derived cache: '{}' differs on recompute (slots {}/{} vs {}/{}, flags {:X} vs {:X})",
								geometry->name.c_str() ? geometry->name.c_str() : "?", derived.pipelineSlot, derived.materialSlot,
								pipelineSlot, materialSlot, derived.staticFlags, staticFlags);
					}
					derived.valid = true;
					derived.generation = tablesGeneration;
					derived.triShape = tables.geometrySlotKey[geometrySlot];
					derived.property = witnessProperty;
					derived.material = witnessMaterial;
					derived.fadeState = fadeState;
					derived.interior = interior;
					derived.alphaBelowOne = alphaBelowOne;
					derived.technique = accumulated->technique;
					derived.subPass = accumulated->subPass;
					derived.hint = accumulated->hint;
					derived.biasWitness = biasWitness;
					derived.descriptors = descriptors;
					derived.staticFlags = staticFlags;
					derived.key = key;
					derived.geometrySlot = geometrySlot;
					derived.pipelineSlot = pipelineSlot;
					derived.materialSlot = materialSlot;
				}
			}

			// The patch into the scene phase's record, applied once and kept.
			// A technique whose objects take the land blend rows (Api::ObjectExtras::LandBlend: Features/Landscape's MTLand, MTLandLODBlend).
			const bool landBlendRecord = Extensions::TechniqueExtras(descriptors.technique) == Api::ObjectExtras::LandBlend;
			AccumulatePatch patch;
			patch.object = objectId;
			patch.material = materialSlot;
			patch.pipeline = pipelineSlot;
			patch.projectedUV = descriptors.projectedUV;
			patch.landBlend = landBlendRecord;
			// Membership is CommitSet's, which keeps a record being bound out of the set (SetStats::patchedMember counts violations).
			patch.flags = (object.flags & kSceneKeptFlags) | staticFlags | (accumulated->sunTest ? kObjectSunTest : 0u) |
			              (accumulated->fadeDistance != 0.0f ? kObjectFadeTest : 0u) | (accumulated->heightTest ? kObjectHeightTest : 0u) |
			              (patch.projectedUV ? kObjectProjectedUV : 0u) | (patch.landBlend ? kObjectLandBlend : 0u) |
			              ((PhasesIn(tables, objectId) & kSetMain) ? kObjectMember : 0u);
			if (PhasesIn(tables, objectId) & kSetMain)
				++setStats.patchedMember;
			patch.fadeDistance = accumulated->fadeDistance;
			timer.Add(BuildPart::Record);
			if (ancestorValueOn) {
				// The evaluation's ancestor value, when the ancestor map moved on.
				if (a_result.ancestorSet) {
					trackedEntry->ancestorValue = a_result.ancestorValue;
					trackedEntry->ancestorMapGeneration = ancestorMapGeneration;
				}
				patch.ancestorValue.value = trackedEntry->ancestorValue;
			}
			timer.Add(BuildPart::CapturePatch);
			if (descriptors.pass & kPassAdditionalAlphaMask) {
				if (fadingThisFrame++ == 0)
					++stats.fadingFrames;
				++stats.fadingDrawn;
			}
			// A member decal's order key (Features/Decals/DecalOrder.cpp): group, pass enum (the descriptor + its kind's base: Lighting's
			// 0x4800002D, the Effect port's 0x4000002C), sub-pass. The engine draws a group's buckets in ascending enum, so effect decals precede
			// Lighting ones.
			if (descriptors.decalGroup) {
				const std::uint32_t passEnum = accumulated->technique + Extensions::PortRules(descriptors.port).passEnumBase;
				patch.decalKey = (std::uint64_t(descriptors.decalGroup) << 60) | (std::uint64_t(passEnum) << 27) | (std::uint64_t(accumulated->subPass & 7) << 24);
			}
			timer.Add(BuildPart::Record);
			ApplyAccumulatePatch(patch);
			timer.Add(BuildPart::ApplyPatch);
			MarkResidentSlot(objectId, { *accumulated, pipelineSlot, materialSlot });
			(layer ? residentLayerJoining : residentJoining).erase(geometry);
			++residentStats.joined;
			timer.Add(BuildPart::Record);
		};
		{
			// The joins resolve no geometry slots: unchanged until the merges are done.
			[[maybe_unused]] const std::size_t geometries = tables.geometries.size();
			[[maybe_unused]] const std::uint32_t generation = tablesGeneration;
			const std::size_t count = accumulateOrder.size();
			auto& f = fanoutStats;
			++f.joinRounds;
			f.joinEntries += count;
			const std::uint32_t mode = FanoutMode(count);
			joinMaterialPrepared.clear();
			if (mode == 0) {
				for (const auto& entry : accumulateOrder) {
					JoinResult result;
					{
						ShardScope scope(*this, false);
						EvaluateJoin(entry, false, result);
					}
					join(entry, result);
				}
			} else {
				f.joinParallelRounds += mode == 2 ? 1 : 0;
				{
					DCLF_SCENE_PART(JoinsFanout, "CS.DCLF.Accumulate.JoinsFanout");
					EvaluateJoins(count, mode == 2, joinResults, [&](std::size_t a_index, JoinResult& a_out) { EvaluateJoin(accumulateOrder[a_index], false, a_out); });
				}
				{
					DCLF_SCENE_PART(JoinsFanout, "CS.DCLF.Accumulate.JoinMaterials");
					PrepareJoinMaterials(count);
				}
				DCLF_SCENE_PART(JoinsMerge, "CS.DCLF.Accumulate.JoinsMerge");
				for (std::size_t i = 0; i < count; ++i)
					join(accumulateOrder[i], joinResults[i]);
			}
			assert(tables.geometries.size() == geometries && tablesGeneration == generation);
		}

		TracyCZoneEnd(objectsZone);
		TracyCZoneN(residentsZone, "CS.DCLF.Accumulate.Residents", true);
		// Unpatched joins (a frame verdict, a material not ready) leave residency.
		for (const auto* geometry : residentJoining) {
			++residentStats.failed;
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			const auto cause = entry == tracked.end() || entry->second.objectStamp != objectStamp ? 1u :                  // no record
			                   entry->second.accumulateReasonPass == passSerial ? 2u :                                           // a verdict of this pass
			                   3u;                                                                                          // material, or extras rows
			++residentStats.failedBy[cause];
			if (entry != tracked.end() && entry->second.slot != kNoObjectSlot && IsResidentSlot(entry->second.slot))
				DropResidentSlot(entry->second.slot, true);
		}
		residentJoining.clear();
		for (const auto* geometry : residentLayerJoining) {
			++residentStats.failed;
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			if (entry != tracked.end() && entry->second.layerSlot != kNoObjectSlot && IsResidentSlot(entry->second.layerSlot))
				DropResidentSlot(entry->second.layerSlot, true);
		}
		residentLayerJoining.clear();
		// A base and its layer are bound together (the set is by geometry): one without the other leaves.
		for (const auto& entry : accumulateOrder) {
			const auto& t = *entry.tracked;
			if (t.layerSlot == kNoObjectSlot || t.slot == kNoObjectSlot || IsResidentSlot(t.slot) == IsResidentSlot(t.layerSlot))
				continue;
			++residentStats.layerUnpaired;
			DropResidentSlot(IsResidentSlot(t.slot) ? t.slot : t.layerSlot, true);
		}
		KeepResidentsAlive();
		++residentStats.frames;
		residentStats.resident += residents.size();
		Diagnostics::Notify<Diagnostics::ScenePoint::ResidentsKept>();
		{
			// Slots are freed only when unreferenced, which the normal path trusts; parity neutralises any object that dangles.
			if (Policy::NeutraliseDanglingSlots(slotsFreedThisFrame, passSerial))
				CheckObjectSlots(frameResolveBuffers);
			slotsFreedThisFrame = false;
		}
		TracyCZoneEnd(residentsZone);
		TracyCZoneN(statsZone, "CS.DCLF.Accumulate.StatsAndDecals", true);
		auto countBits = [](const std::vector<std::uint64_t>& a_bits) {
			std::uint32_t count = 0;
			for (const auto word : a_bits)
				count += static_cast<std::uint32_t>(std::popcount(word));
			return count;
		};
		stats.pipelines = countBits(tables.usedPipelineBits);
		stats.materials = countBits(tables.usedMaterialBits);
		ComponentsJoinsDone();
		stats.geometries = static_cast<std::uint32_t>(tables.geometrySlots.ReferencedCount());
		stats.geometriesAlive = static_cast<std::uint32_t>(tables.geometrySlots.AliveCount());
		stats.pipelinesAlive = static_cast<std::uint32_t>(tables.pipelineSlots.AliveCount());
		stats.materialsAlive = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		stats.materialCacheEntries = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		TracyCZoneEnd(statsZone);
		Diagnostics::Notify<Diagnostics::ScenePoint::JoinsEnd>();
	}

	void Coordinator::PrepareJoinMaterials(std::size_t a_count)
	{
		// Each (material, pass) a classified join keys that no slot holds, once, from its held capture (drained at the pass's start, stable
		// over it): evaluated here, on the pool for a burst, so the merges only allocate and store. Read-only over materialIndex; a merge
		// that keys one first leaves its evaluation unread.
		auto& r = materialRecordStats;
		auto& items = joinMaterialItems;
		items.clear();
		joinMaterialPrepared.clear();
		for (std::size_t i = 0; i < a_count && i < joinResults.size(); ++i) {
			const auto& result = joinResults[i];
			if (result.kind != JoinResult::Kind::Joined || !result.classified || !result.material)
				continue;
			const std::pair key{ result.material, result.descriptors.pass };
			if (materialIndex.contains(key))
				continue;
			const auto held = materialSnapshots.find(result.material);
			if (held == materialSnapshots.end() || !held->second.held)
				continue;  // the merge requests it
			if (joinMaterialPrepared.try_emplace(key, static_cast<std::uint32_t>(items.size())).second)
				items.push_back({ result.material, &held->second.held->snapshot, result.descriptors.pass });
		}
		if (items.empty())
			return;
		auto& results = joinMaterialResults;
		results.resize(items.size());
		const auto& sources = FrameGlobals::Current().material;
		if (PrepareMaterials("CS.DCLF.Accumulate.JoinMaterialsShards", items.size(), [&](std::size_t a_begin, std::size_t a_end) {
				for (std::size_t i = a_begin; i < a_end; ++i)
					EvaluateMaterial(*items[i].snapshot, items[i].pass, sources, kNoMaterialSlot, results[i]);
			})) {
			r.evaluationsPooled += items.size();
			++r.poolRounds;
		}
	}

	void Coordinator::BindByMembership()
	{
		ZoneScopedN("CS.DCLF.Accumulate.BindByMembership");
		// A record being rebound first leaves the set's main phases (a join never patches a member). The commit later in this pass
		// readmits it if ready, keeping its claim (the frame draws the old record until the new publication); otherwise it leaves there.
		auto leaveMain = [&](std::uint32_t a_slot) {
			if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & kSetMain))
				++setStats.rebinding;
			// Every view's member draws with its main bindings (a first-person member too), so it leaves too.
			const auto phases = static_cast<std::uint8_t>(kSetMain | Extensions::Views().phases);
			LeaveSet(a_slot, phases);
			if (PhasesIn(tables, a_slot) & phases) {
				tables.setPhases[a_slot] = 0;
				tables.objects[a_slot].flags &= ~kObjectMember;
				tables.NoteChange(a_slot, kChangeBindings);
			}
		};
		// The frame globals a membership pass reads changed (static sun bits, fade distances): rebind every resident.
		if (const std::uint32_t witness = sceneInputs.membershipWitness; witness != membershipWitness) {
			for (const std::uint32_t slot : residents)
				leaveMain(slot);
			bindQueue.insert(bindQueue.end(), residents.begin(), residents.end());
			EndAllResidency();
			membershipWitness = witness;
		}
		// The frame's material sources know the Lighting shader only from the frame after it was found: wait until then, or every
		// record of the load would fail the port.
		if (!EngineShaders::Lighting() || !FrameGlobals::Current().material.vanilla.sampled || !FrameGlobals::Current().material.vanilla.shaderKnown)
			return;
		bindQueue.insert(bindQueue.end(), bindRetry.begin(), bindRetry.end());
		bindRetry.clear();
		const auto queue = std::exchange(bindQueue, {});
		// Evaluate each element (EvaluateJoinPre), then merge in queue order. A slot may be queued twice; evaluations read the state
		// before the merges, and the merge handles what earlier merges changed.
		auto merge = [&](std::uint32_t a_slot, JoinPre& a_pre) {
			const std::uint32_t slot = a_slot;
			// Kept when evaluated but dropped since (its partner failed): evaluate again.
			if (a_pre.kind == JoinPre::Kind::Kept && !IsResidentSlot(slot))
				EvaluateJoinPre(slot, a_pre);
			if (a_pre.kind == JoinPre::Kind::Skip)
				return;
			// The binding's inputs are unchanged (skins, faces and movers are rewritten every frame for transforms alone).
			if (a_pre.kind == JoinPre::Kind::Kept) {
				++residentStats.membershipKept;
				return;
			}
			auto* geometry = a_pre.geometry;
			const bool layer = a_pre.layer;
			auto& entry = tracked.find(geometry)->second;
			(layer ? entry.membershipLayerDerived : entry.membershipDerived) = a_pre.derived;
			if (a_pre.kind == JoinPre::Kind::Failed) {
				// No pass: unbind, or the commit would keep it with a binding it no longer has. Rebound only when written again.
				if (IsResidentSlot(slot))
					DropResidentSlot(slot, true);
				return;
			}
			(layer ? accumulatedLayerPasses : accumulatedPasses).insert_or_assign(geometry, a_pre.pass);
			(layer ? residentLayerJoining : residentJoining).insert(geometry);
			leaveMain(slot);
			++residentStats.membershipQueued;
		};
		const std::size_t count = queue.size();
		const std::uint32_t mode = FanoutMode(count);
		auto& f = fanoutStats;
		++f.joinRounds;
		f.joinEntries += count;
		if (mode == 0) {
			for (const std::uint32_t slot : queue) {
				JoinPre pre;
				{
					ShardScope scope(*this, false);
					EvaluateJoinPre(slot, pre);
				}
				merge(slot, pre);
			}
			return;
		}
		f.joinParallelRounds += mode == 2 ? 1 : 0;
		{
			DCLF_SCENE_PART(JoinsFanout, "CS.DCLF.Accumulate.BindFanout");
			EvaluateJoins(count, mode == 2, joinPres, [&](std::size_t a_index, JoinPre& a_out) { EvaluateJoinPre(queue[a_index], a_out); });
		}
		{
			DCLF_SCENE_PART(JoinsMerge, "CS.DCLF.Accumulate.BindMerge");
			for (std::size_t i = 0; i < count; ++i)
				merge(queue[i], joinPres[i]);
		}
	}

	void Coordinator::EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const
	{
		// Pure; PrimaryCull's pass reads the frame globals the ShardScope binds.
		const std::uint32_t slot = a_slot;
		auto& r = a_out;
		r = JoinPre{};
		if (slot >= tables.objects.size() || (tables.objects[slot].flags & (kObjectFree | kObjectShadowOnly)))
			return;
		auto* geometry = const_cast<RE::BSGeometry*>(tables.objectGeometry[slot]);
		const auto trackedIt = geometry ? tracked.find(geometry) : tracked.end();
		// A layer slot joins with its own pass (its property's, hint 12).
		const bool layer = tables.IsLayer(slot);
		if (trackedIt == tracked.end() || (layer ? trackedIt->second.layerSlot : trackedIt->second.slot) != slot ||
			trackedIt->second.objectStamp != objectStamp || trackedIt->second.candidateReason != Ineligible::None)
			return;
		const auto& entry = trackedIt->second;
		r.geometry = geometry;
		r.layer = layer;
		if (IsResidentSlot(slot) && MemberBindingStands(slot, entry)) {
			r.kind = JoinPre::Kind::Kept;
			return;
		}
		r.leaf = mirror.Leaf(geometry);
		const auto& cached = layer ? entry.membershipLayerDerived : entry.membershipDerived;
		const auto membership = layer ? PrimaryCull::MembershipLayerPass(r.leaf, cached) : PrimaryCull::MembershipPass(r.leaf, cached);
		r.derived = membership.derived;
		r.fail = membership.fail;
		if (!membership.ok) {
			r.kind = JoinPre::Kind::Failed;
			return;
		}
		r.pass = membership.pass;
		// A fade node's fade-out distance for BuildDraws' fade test (measured from the node's centre); trees add the height test.
		if (const auto* fadeNode = r.leaf.fadeNode) {
			r.pass.fadeDistance = PrimaryCull::MembershipFadeDistance(*fadeNode);
			r.pass.heightTest = (fadeNode->kind & SceneCapture::kKindTree) != 0;
		}
		r.kind = JoinPre::Kind::Pass;
	}

	void Coordinator::EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const
	{
		// Pure (any thread under a ShardScope). Anything touching slots or records is the merge's (BuildAccumulatePhase's join).
		auto* geometry = a_entry.geometry;
		const Tracked& entry = *a_entry.tracked;
		const AccumulatedPass* accumulated = a_entry.accumulated;
		const bool layer = a_entry.layer;
		auto& r = a_out;
		r = JoinResult{};
		const std::uint32_t objectId = layer ? entry.layerSlot : entry.objectId;
		r.geometrySlot = tables.objects[objectId].geometryIndex;
		// The entry's held property reference (the template's, a material request's).
		r.leaf = mirror.Leaf(geometry);
		const auto* propertyRecord = layer ? r.leaf.layer : r.leaf.property;
		r.property = entry.HeldProperty(layer);
		if (!propertyRecord || !r.property || propertyRecord->key != r.property)
			return;
		r.material = static_cast<const RE::BSShaderMaterial*>(propertyRecord->material);
		r.fadeState = FadeStateOf(propertyRecord);
		r.alphaBelowOne = propertyRecord->materialAlpha < 1.0f;  // 1 without a material, or for any but a Lighting property
		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8) | (decalBiasMode[3] << 16);
		// The derivation cache's witnesses; whether its slots still hold is the merge's.
		const auto& derived = layer ? entry.layerDerived : entry.derived;
		r.derivedWitness = derived.valid && derived.generation == tablesGeneration && derived.geometrySlot == r.geometrySlot && derived.property == r.property &&
		                   derived.material == r.material && derived.fadeState == r.fadeState && derived.technique == accumulated->technique &&
		                   derived.subPass == accumulated->subPass && derived.hint == accumulated->hint && derived.interior == interior &&
		                   derived.alphaBelowOne == r.alphaBelowOne && derived.biasWitness == biasWitness;
		const bool derivedProbe = Policy::RecomputeCachedVerdicts(Policy::Verdicts::Derivation, passSerial);
		r.classified = a_classify || !r.derivedWitness || derivedProbe;
		Ineligible reason = Ineligible::None;
		if (r.classified)
			reason = layer ? Eligibility::Layer(r.leaf, &r.descriptors, accumulated) : Eligibility::Static(r.leaf, &r.descriptors, accumulated);
		// A first-person decal draws in the first-person forward part.
		if (r.classified && reason == Ineligible::None && !layer)
			ViewDecalForward(entry, r.descriptors);
		// Every time, cached or not: an object just hidden must lose its bindings now, or DCLF keeps drawing what the engine stopped.
		if (reason == Ineligible::None)
			reason = ClassifyFrame(entry);
		r.reason = reason;
		if (reason != Ineligible::None) {
			r.kind = JoinResult::Kind::Rejected;
			return;
		}
		r.kind = JoinResult::Kind::Joined;
		if (r.classified) {
			const auto& descriptors = r.descriptors;
			using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
			const bool twoSided = (propertyRecord->flags & static_cast<std::uint64_t>(PropertyFlag::kTwoSided)) != 0;
			// A layer's draws apply no alpha property (render flags 0x41, engine notes).
			const auto* alpha = layer ? nullptr : r.leaf.alpha;
			const bool alphaTest = alpha && (alpha->flags & (1u << 9));  // GetAlphaTesting
			const bool alphaBlending = alpha && (alpha->flags & 1u);     // GetAlphaBlending
			// A decal's key carries the engine's fixed-function state indices (Records.h): the frame's depth-bias mode, the alpha
			// property's blend and write modes.
			std::uint32_t rasterFlags = twoSided ? kRasterTwoSided : 0u;
			if (descriptors.decalGroup)
				rasterFlags |= PackDecalRasterFlags(descriptors.decalGroup, descriptors.opaqueInDecalGroup ? 0u : decalBiasMode[descriptors.decalGroup & 3], descriptors.decalBlendMode,
					descriptors.decalWriteMode);  // an opaque effect in decal group 1 has no bias (DeriveEffectDescriptors)
			// As Terrain Blending's replay sets it: blend 1 / write 1 for terrain, blend off / write 1 for the no-blend meshes after it.
			if (descriptors.terrainGroup)
				rasterFlags |= PackTerrainRasterFlags(descriptors.terrainGroup, descriptors.terrainGroup == 1 ? 1u : 0u, 1u);
			// As the engine's blended draw (FUN_1414b39d0) leaves it.
			if (descriptors.forward)
				rasterFlags |= PackForwardRasterFlags(descriptors.forwardBlendMode, kForwardWriteMode, descriptors.forwardDepthTest, descriptors.forwardDepthWrite);
			// A first-person decal's depth bias, in the bias bits a forward key leaves free.
			if (descriptors.forward && descriptors.forwardDecalGroup)
				rasterFlags |= (decalBiasMode[descriptors.forwardDecalGroup & 3] & 15u) << kRasterDepthBiasShift;
			if (descriptors.refraction)
				rasterFlags |= PackRefractionRasterFlags(descriptors.refractionFalloff, descriptors.refractionClamp);
			if (globals::features::extendedTranslucency.loaded && !descriptors.port)
				rasterFlags |= ((ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ TranslucencyModelOf(r.leaf)) & 7u) << kRasterTranslucencyShift;
			// A port's key carries its program family's key bit (the Effect port's kRasterEffect; none for Lighting's).
			const auto& family = Extensions::FamilyRulesOfKind(descriptors.port);
			rasterFlags |= family.keyBit;
			// A billboard leaf's mode + 1 in its family's spare bits, so the vertex stage turns it (ClassifyFrame admits it only with a mode).
			if (family.turnedKeyBits && entry.parentReason == Ineligible::Billboard)
				if (const std::int32_t mode = BillboardModeOf(entry); mode >= 0)
					rasterFlags |= static_cast<std::uint32_t>(mode + 1) << family.spareShift;
			r.key = PipelineKey{ descriptors.vertex, descriptors.pixel, rasterFlags, descriptors.pass,
				VertexLayoutOf(tables.geometries[r.geometrySlot].vertexDesc) };
			// ExternalEmittance::ShouldSuppress: interior, kExternalEmittance, and a reference without an emittance source.
			const bool suppress = interior && (propertyRecord->flags & static_cast<std::uint64_t>(PropertyFlag::kExternalEmittance)) && !r.leaf.node->emittance;
			r.staticFlags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
			                (suppress ? kObjectSuppressExternalEmittance : 0u) |
			                TechniqueFlags(descriptors.technique) |
			                // A Blood effect keeps its threshold too (its family's alphaRefPassBits; AlphaTestRefOf: SetupGeometry's AlphaTestRef.x,
			                // tested or not).
			                ((alphaTest || (alpha && (descriptors.pass & family.records.alphaRefPassBits))) ?
			                        static_cast<std::uint32_t>(alpha->threshold) << kObjectAlphaThresholdShift :
			                        0u) |
			                (alphaTest && alphaBlending ? kObjectAlphaBlended : 0u) |
			                (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u) |
			                (descriptors.terrainGroup << kObjectTerrainGroupShift) |
			                (descriptors.forward ? kObjectForward : 0u) |
			                (!descriptors.port && (propertyRecord->flags & LandscapeLightFlags()) ? kObjectLandscapeLights : 0u) |
			                (!descriptors.port && BeastRaceFace(*propertyRecord, r.leaf) ? kObjectBeastRace : 0u);
		}
		// The ancestor value (Light Limit Fix's room), when the ancestor map moved on (read-only here).
		if (Extensions::AncestorValueOn() && entry.ancestorMapGeneration != ancestorMapGeneration) {
			r.ancestorSet = true;
			r.ancestorValue = Extensions::AncestorValue()->none;
			if (ancestorMap && entry.ancestorKey)
				if (const auto found = ancestorMap->find(entry.ancestorKey); found != ancestorMap->end())
					r.ancestorValue = found->second;
		}
	}

	bool Coordinator::MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const
	{
		const bool layer = tables.IsLayer(a_slot);
		const auto& derived = layer ? a_entry.layerDerived : a_entry.derived;
		const auto leaf = mirror.Leaf(a_entry.geometry.get());
		const auto* record = layer ? leaf.layer : leaf.property;
		const auto* property = record ? static_cast<const RE::BSShaderProperty*>(record->key) : nullptr;
		const auto* material = record ? static_cast<const RE::BSShaderMaterial*>(record->material) : nullptr;
		const bool alphaBelowOne = record && record->materialAlpha < 1.0f;
		return derived.valid && derived.generation == tablesGeneration && derived.geometrySlot == tables.objects[a_slot].geometryIndex &&
		       derived.property == property && derived.material == material && derived.fadeState == FadeStateOf(record) &&
		       derived.alphaBelowOne == alphaBelowOne && derived.interior == frameInterior && derived.pipelineSlot == tables.objects[a_slot].pipelineIndex &&
		       derived.materialSlot == tables.objects[a_slot].materialIndex;
	}

	void Coordinator::ResetAccumulatedHalf(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objects.size() || (tables.objects[a_slot].flags & kObjectFree))
			return;
		const auto columnsBefore = tables.ColumnsOf(a_slot);
		auto& object = tables.objects[a_slot];
		object.flags = tables.sceneFlags[a_slot];
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		tables.draws[a_slot].pipelineIndex = 0;
		tables.ancestorValue[a_slot] = ObjectAncestorValue{};
		tables.FreeExtras(a_slot);
		tables.fadeDistance[a_slot] = 0.0f;
		tables.NoteWrite(a_slot, columnsBefore);
	}

	void Coordinator::RefreshShadowSets(bool a_forceRebuild)
	{
		ZoneScopedN("CS.DCLF.Capture.ShadowSets");
		// ClearFrame rebuilds the public lists every walk; the dependency index holds membership across frames. Each index keeps its keys
		// ordered by their lowest member (the order the sets are emitted in), and the textures that appear or go are noted as they do.
		auto& net = shadowTextureNet;
		net.clear();
		bool rebuild = a_forceRebuild || shadowIndexNeedsRebuild;
		std::vector<std::uint32_t> changed = std::move(shadowDirtySlots);
		shadowDirtySlots.clear();
		auto& counters = shadowSetStats;
		++counters.refreshes;
		counters.unjournaled += !rebuild && changed.empty() ? 1u : 0u;
		// The journal is complete: every input write pushes its slot, and the invalidations no slot names (a table reset, a full walk)
		// set shadowIndexNeedsRebuild. Dirty with nothing journaled (a slotless entry released) changes no membership: the lists are
		// emitted again (the walk dropped them), the index is not made again. The parity rebuild (Policy::RebuildShadowSets) is the check.
		if (!rebuild) {
			stats.shadowCasters = keptShadowCasters;
			stats.shadowRejects = keptShadowRejects;
		}
		// Which emitted lists to make again from their order (texture, key, then one bit per occlusion view): every one after a rebuild.
		std::uint32_t moved = rebuild ? ~0u : 0u;
		constexpr std::uint32_t kTextureOrder = 1u, kKeyOrder = 2u, kOcclusionOrder = 4u;
		if (rebuild) {
			ZoneScopedN("CS.DCLF.Capture.ShadowSets.Rebuild");
			++counters.rebuilds;
			// Every texture held goes, and comes back as the slots are indexed again: only the difference is noted.
			for (const auto& entry : shadowTextureMembers)
				--net[entry.first];
			shadowIndexedInputs.clear();
			shadowIndexedLive.clear();
			shadowTextureMembers.clear();
			shadowKeyMembers.clear();
			for (auto& members : occlusionKeyMembers)
				members.clear();
			shadowTextureOrder.clear();
			shadowKeyOrder.clear();
			for (auto& keyOrder : occlusionKeyOrder)
				keyOrder.clear();
			stats.shadowCasters = 0;
			stats.shadowRejects = {};
			changed.clear();
			changed.reserve(tables.objects.size());
			for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
				changed.push_back(slot);
		}
		shadowIndexNeedsRebuild = false;
		shadowIndexedInputs.resize(tables.objects.size());
		shadowIndexedLive.resize(tables.objects.size(), 0);
		std::sort(changed.begin(), changed.end());
		changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
		counters.journaled += rebuild ? 0u : changed.size();
		// A member in or out of its key's set, the key's place in the order moved with its lowest member; +1 / -1 when the key appears or goes.
		auto member = [&moved](auto& a_index, auto& a_order, const auto& a_key, std::uint32_t a_slot, bool a_add, std::uint32_t a_bit) -> std::int32_t {
			constexpr std::uint32_t kNoMember = ~0u;
			if (a_add) {
				auto [it, fresh] = a_index.try_emplace(a_key);
				auto& slots = it->second;
				const std::uint32_t before = slots.empty() ? kNoMember : *slots.begin();
				slots.insert(a_slot);
				if (const std::uint32_t after = *slots.begin(); after != before) {
					if (before != kNoMember)
						a_order.erase(before);
					a_order.insert_or_assign(after, a_key);
					moved |= a_bit;
				}
				return fresh ? 1 : 0;
			}
			const auto it = a_index.find(a_key);
			if (it == a_index.end())
				return 0;
			auto& slots = it->second;
			const std::uint32_t before = slots.empty() ? kNoMember : *slots.begin();
			slots.erase(a_slot);
			if (slots.empty()) {
				a_order.erase(before);
				a_index.erase(it);
				moved |= a_bit;
				return -1;
			}
			if (const std::uint32_t after = *slots.begin(); after != before) {
				a_order.erase(before);
				a_order.insert_or_assign(after, a_key);
				moved |= a_bit;
			}
			return 0;
		};
		auto apply = [&](std::uint32_t a_slot, const ShadowInputs& a_inputs, bool a_live, bool a_add) {
			if (!a_live)
				return;
			auto& rejects = stats.shadowRejects[std::min<std::size_t>(a_inputs.reject, stats.shadowRejects.size() - 1)];
			a_add ? ++rejects : --rejects;
			const bool caster = !(a_inputs.flags & kObjectNoShadow);
			if (caster)
				a_add ? ++stats.shadowCasters : --stats.shadowCasters;
			const bool occluder = std::any_of(a_inputs.occlusionTechnique.begin(), a_inputs.occlusionTechnique.end(), [](std::uint32_t a_t) { return a_t != 0; });
			if (a_inputs.diffuse && (caster || occluder))
				if (const std::int32_t membership = member(shadowTextureMembers, shadowTextureOrder, a_inputs.diffuse, a_slot, a_add, kTextureOrder))
					net[a_inputs.diffuse] += membership;
			const auto raster = (a_inputs.flags & kObjectTwoSided) ? kRasterTwoSided : 0u;
			const auto vertex = VertexLayoutOf(a_inputs.vertexDesc);
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				if (a_inputs.occlusionTechnique[v])
					member(occlusionKeyMembers[v], occlusionKeyOrder[v], ShadowPipelineKey{ a_inputs.occlusionTechnique[v], raster, vertex }, a_slot, a_add, kOcclusionOrder << v);
			if (caster)
				member(shadowKeyMembers, shadowKeyOrder, ShadowPipelineKey{ a_inputs.technique, raster, vertex }, a_slot, a_add, kKeyOrder);
		};
		TracyCZoneN(indexZone, "CS.DCLF.Capture.ShadowSets.Index", true);
		for (const auto slot : changed) {
			if (slot >= tables.objects.size())
				continue;
			const bool live = !(tables.objects[slot].flags & kObjectFree);
			const auto inputs = ShadowInputsOf(slot);
			if ((shadowIndexedLive[slot] != 0) == live && shadowIndexedInputs[slot] == inputs)
				continue;
			++counters.applied;
			apply(slot, shadowIndexedInputs[slot], shadowIndexedLive[slot] != 0, false);
			apply(slot, inputs, live, true);
			shadowIndexedInputs[slot] = inputs;
			shadowIndexedLive[slot] = live;
		}
		TracyCZoneEnd(indexZone);
		shadowSetsDirty = false;
		TracyCZoneN(emitZone, "CS.DCLF.Capture.ShadowSets.Emit", true);
		// The public lists (the walk's merges started them again), in the kept order: no sort. Each emitted list is made again only when
		// its order moved; the seen set follows the net changes (whole again after a rebuild, which can drop members unnoted). The
		// tables take copies, flat copies of what the sets would rebuild by inserting every texture.
		if (moved & kTextureOrder) {
			shadowEmittedTextures.clear();
			shadowEmittedTextures.reserve(shadowTextureOrder.size());
			for (const auto& [slot, texture] : shadowTextureOrder)
				shadowEmittedTextures.push_back(texture);
			++counters.listsEmitted;
		}
		if (rebuild) {
			shadowEmittedSeen.clear();
			shadowEmittedSeen.insert(shadowEmittedTextures.begin(), shadowEmittedTextures.end());
		} else {
			for (const auto& [texture, change] : net) {
				if (change < 0)
					shadowEmittedSeen.erase(texture);
				else if (change > 0)
					shadowEmittedSeen.insert(texture);
			}
		}
		if (moved & kKeyOrder) {
			shadowEmittedKeys.clear();
			for (const auto& [slot, key] : shadowKeyOrder)
				shadowEmittedKeys.push_back(key);
			++counters.listsEmitted;
		}
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (!(moved & (kOcclusionOrder << v)))
				continue;
			shadowEmittedOcclusionKeys[v].clear();
			for (const auto& [slot, key] : occlusionKeyOrder[v])
				shadowEmittedOcclusionKeys[v].push_back(key);
			++counters.listsEmitted;
		}
		tables.shadowTextureSet = shadowEmittedTextures;
		tables.shadowTextureSeen = shadowEmittedSeen;
		tables.shadowKeysUsed = shadowEmittedKeys;
		tables.occlusionKeysUsed = shadowEmittedOcclusionKeys;
		// Only what appeared or went (a texture that went and came back is no change), the releases first.
		for (const auto& [texture, change] : net)
			if (change < 0)
				tables.shadowTextureChanges.emplace_back(texture, false);
		for (const auto& [texture, change] : net)
			if (change > 0)
				tables.shadowTextureChanges.emplace_back(texture, true);
		TracyCZoneEnd(emitZone);
	}
}
