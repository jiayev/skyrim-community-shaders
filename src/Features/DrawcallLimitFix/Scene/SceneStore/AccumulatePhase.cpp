#include "Internal.h"

#include "Features/SubsurfaceScattering.h"

namespace DCLF
{
	namespace
	{
		/**
		 * @brief Subsurface Scattering's IsBeastRace for the object (SubsurfaceScattering::BSLightingShader_SetupSkin): a face's
		 * (kFace or kFaceGenRGBTint), from its actor's race keyword, and set without an actor or a race. Lighting.hlsl reads it
		 * under SKIN alone, so it is nothing for any other property.
		 */
		bool BeastRaceFace(const RE::BSShaderProperty& a_property, RE::BSGeometry& a_geometry)
		{
			using enum RE::BSShaderProperty::EShaderPropertyFlag;
			const auto& sss = globals::features::subsurfaceScattering;
			if (!sss.loaded || !sss.isBeastRaceKeyword || !a_property.flags.any(kFace, kFaceGenRGBTint))
				return false;
			if (auto* userData = a_geometry.GetUserData())
				if (auto* actor = userData->As<RE::Actor>())
					if (auto* race = actor->GetRace())
						return race->HasKeyword(sss.isBeastRaceKeyword);
			return true;
		}
	}

	void SceneStore::LatchAccumulator()
	{
		auto* accumulator = *globals::game::currentAccumulator.get();
		if (!accumulator || accumulator == latchedAccumulator)
			return;
		if (latchedAccumulator) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::warn("[DCLF] the main camera's accumulator changed ({} -> {}); the tables follow it",
					static_cast<const void*>(latchedAccumulator), static_cast<const void*>(accumulator));
			}
		}
		latchedAccumulator = accumulator;
	}

	bool SceneStore::RefreshMainBatchRenderers()
	{
		auto* accumulator = latchedAccumulator ? latchedAccumulator : *globals::game::currentAccumulator.get();
		auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
		if (!batch)
			return false;  // before the first latch, i.e. the first frame only
		mainBatchRenderers.clear();
		mainBatchRenderers.insert(batch);
		for (auto* group : batch->geometryGroups) {
			if (group && group->batchRenderer)
				mainBatchRenderers.insert(group->batchRenderer);
		}
		return true;
	}

	const RE::BSRenderPass* SceneStore::TemplatePassOf(RE::BSShaderProperty* a_property) const
	{
		const auto* pass = a_property ? FindLightingPass(a_property) : nullptr;
		return pass ? pass : frameLightingPass;
	}

	void SceneStore::DrainCapture()
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained: the capture buffer is fixed-capacity and a frame that does not drain it overflows. The main
		// camera's registrations are no source of bindings (scene membership is); the diagnostics read them.
		const auto entries = capture.Drain();
		// The BSLightingShader instance, from any Lighting pass the frame registered (step 6e F2: the render thread's, not the walk's).
		if (auto& evaluator = ConstantEvaluator::Get(); !evaluator.HasLightingShader())
			for (const auto& entry : entries)
				if (entry.pass && entry.pass->shader && entry.pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					evaluator.SetLightingShader(entry.pass->shader);
					break;
				}
		frameLightingPass = nullptr;
		for (const auto& entry : entries)
			if (entry.pass && mainBatchRenderers.contains(entry.batch) && entry.pass->shader &&
				entry.pass->shader->shaderType.get() == RE::BSShader::Type::Lighting && entry.pass->numLights > 0 && entry.pass->sceneLights) {
				frameLightingPass = entry.pass;
				break;
			}
		if (SwitchValue(Switch::DecalOrderProbe) == "1")
			Scene::ProbeDecalOrder(entries, mainBatchRenderers);
		// Eligible objects DCLF has not bound that the engine registered: a scene event DCLF missed (a record not written
		// again, a verdict not taken again). The tracked set is the coordinator's: checked by the accumulate work (CheckRegistrations).
		capturedRegistrations.clear();
		for (const auto& entry : entries)
			if (entry.geometry && mainBatchRenderers.contains(entry.batch) && !entry.fading)
				capturedRegistrations.push_back({ entry.geometry, entry.hint });
	}

	void SceneStore::CheckRegistrations()
	{
		for (const auto& entry : capturedRegistrations) {
			const auto it = tracked.find(const_cast<RE::BSGeometry*>(entry.geometry));
			if (it == tracked.end() || it->second.candidateReason != Ineligible::None || ResidentObject(FindObject(entry.geometry)))
				continue;
			++residentStats.registeredUnbound;
			if (residentStats.registeredUnboundFirst.empty())
				residentStats.registeredUnboundFirst = fmt::format("'{}' under '{}' (hint {})", entry.geometry->name.c_str() ? entry.geometry->name.c_str() : "",
					entry.geometry->parent && entry.geometry->parent->name.c_str() ? entry.geometry->parent->name.c_str() : "", entry.hint);
		}
	}

	const AccumulatedPass* SceneStore::FindAccumulatedPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedPasses.find(a_geometry);
		return it == accumulatedPasses.end() ? nullptr : &it->second;
	}

	const AccumulatedPass* SceneStore::FindAccumulatedLayerPass(const RE::BSGeometry* a_geometry) const
	{
		auto it = accumulatedLayerPasses.find(a_geometry);
		return it == accumulatedLayerPasses.end() ? nullptr : &it->second;
	}

	void SceneStore::ApplyAccumulatePatch(const AccumulatePatch& a_patch)
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
		tables.lights[objectId] = a_patch.lights;
		tables.treeAnim[objectId] = a_patch.tree;
		if (a_patch.projectedUV || a_patch.landBlend) {
			stats.projectedUV += a_patch.projectedUV ? 1 : 0;
			stats.landBlend += a_patch.landBlend ? 1 : 0;
			if (tables.extraOffset[objectId] == kNoExtraRows)
				tables.extraOffset[objectId] = tables.AllocateExtras();
			// Its static parts (the frame's are the draw's: ExtrasFrame), noted with the patch's other columns.
			WriteObjectExtras(objectId);
		} else {
			tables.FreeExtras(objectId);
		}
		// A membership join: the column goes into the same before/after journal as the other value-only writes.
		tables.residentSlot[objectId] = 1;
		before.NoteWrite(tables, objectId);
		// Its shading, sampled by the next frame's values (the record joins the set then at the earliest).
		NameShading(objectId, true);
		if (a_patch.decalKey) {
			++stats.decals[static_cast<std::uint32_t>(a_patch.decalKey >> 60) - 1];
			memberDecals[objectId] = a_patch.decalKey;
			NoteDecalChanged(objectId);
		}
	}

	/**
	 * @brief The accumulator half of the frame, at EarlyPrepass.
	 *
	 * The main camera's passes are complete only once Main_RenderShadowMaps returns, so everything that
	 * depends on them is here: the capture drain, the membership joins' pipeline and material slots, the
	 * per-frame lighting template, the shading and light lists and the decal order. It patches the records
	 * BuildScenePhase appended, in place and by object index.
	 */
	void SceneStore::PrepareAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Prepare");
		// The engine's registrations are drained for the diagnostics (and the frame's lighting pass, TemplatePassOf's fallback).
		RefreshMainBatchRenderers();
		DrainCapture();
		if (!sceneBuilt)
			return;  // a load screen, or the feature installed mid-frame: nothing to patch
		SyncFrameMaterials();
		PrimaryCull::Get().CheckLightMasks();
		// LightLimitFix's room map, for the joins (a copy: the render thread swaps the map itself).
		if (auto& lightFix = globals::features::lightLimitFix; lightFix.loaded && lightFix.GetRoomMapGeneration() != roomMapGeneration) {
			auto copy = std::make_shared<ankerl::unordered_dense::map<const RE::NiNode*, int>>();
			copy->reserve(lightFix.roomNodes.size());
			for (const auto& [node, index] : lightFix.roomNodes)
				copy->emplace(node, index);
			roomMap = std::move(copy);
			roomMapGeneration = lightFix.GetRoomMapGeneration();
		}
		// What only the render thread may run, ahead of the joins: the engine's SetupMaterial for the materials the last joins
		// asked for, and the material tail (writer events, texture transforms, the validation slice), which evaluate materials too.
		ServeMaterialRequests();
		// The frame's own work on its snapshot (step 6c): the new pipelines' blocks and technique rows (the last accumulate phase's,
		// now in the snapshot), then the material tail.
		RefreshNewPipelineConstants();
		ProcessMaterialWrites();
		RefreshTextureTransforms();
		ValidateMaterialSlice();
	}

	void SceneStore::ServeMaterialRequests()
	{
		// A served record no join took within two frames (its object left, or bound another way): let go.
		for (auto it = materialsServed.begin(); it != materialsServed.end();) {
			if (frame - it->second.frame > 2) {
				materialsHandedBack.push_back(std::move(it->second.owner));
				it = materialsServed.erase(it);
			} else {
				++it;
			}
		}
		for (auto& request : std::exchange(materialRequests, {})) {
			const auto key = std::pair{ request.material, request.pass };
			materialRequested.erase(key);
			MaterialServed served;
			served.valid = EvaluateMaterialForSlot(request.material, request.pass, served.record);
			served.owner = std::move(request.owner);
			served.frame = frame;
			++residentStats.materialsServed;
			if (const auto it = materialsServed.find(key); it != materialsServed.end())
				materialsHandedBack.push_back(std::move(it->second.owner));
			materialsServed.insert_or_assign(key, std::move(served));
		}
	}

	void SceneStore::RunAccumulateWork(bool a_task)
	{
		inSceneTask = a_task;
		holdPrimaryNotes = true;
		holdLostMembers = true;
		RecycleRetired();
		DropWrittenMaterials();
		BuildAccumulatePhase();
		// The set applied and the tables published with it (step 6e E3), for the next frame's start to install.
		PublishScene();
		holdLostMembers = false;
		holdPrimaryNotes = false;
		inSceneTask = false;
		accumulateWorkPending = true;
	}

	void SceneStore::FinishAccumulateWork()
	{
		auto& primary = PrimaryCull::Get();
		if (std::exchange(allMembersLostHeld, false))
			primary.NoteAllMembersLost();
		for (const auto* geometry : lostMembersHeld)
			primary.NoteMemberLost(geometry);
		lostMembersHeld.clear();
	}

	void SceneStore::RefreshNewPipelineConstants()
	{
		const Tables& view = FrameView();
		frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, tablesGeneration);
		frameTables.SyncTechniques(view.techniqueKeys.size());
		auto& evaluator = ConstantEvaluator::Get();
		for (std::size_t word = 0; word < view.usedPipelineBits.size(); ++word)
			for (std::uint64_t remaining = view.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t slot = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (slot >= view.pipelines.size())
					continue;
				// Its technique row, new: evaluated once (RefreshFrameConstants keeps it a frame's).
				if (auto& row = frameTables.techniques[view.pipelineTechnique[slot]]; !row.valid) {
					EvaluateTechnique(view.pipelines[slot].passDescriptor, row.value);
					row.evaluated = frame;
					row.constantsVersion = frameTables.NextVersion();
					row.bindingVersion = frameTables.NextVersion();
					row.valid = true;
					PostTechniqueConstants(view.pipelineTechnique[slot]);
				}
				if (frameTables.geometryConstantsValid[slot])
					continue;
				GeometryConstants constants{};
				const auto* templatePass = TemplatePassOf(view.geometryTemplate[slot]);
				if (!templatePass || !evaluator.EvaluateGeometry(*templatePass, view.pipelines[slot].passDescriptor, kMainPassRenderFlags, constants))
					continue;
				frameTables.geometryConstants[slot] = constants;
				frameTables.geometryConstantsValid[slot] = 1;
				lightingSeeds.push_back(constants.ps);
				frameTables.pipelineConstantsVersion[slot] = frameTables.NextVersion();
				PostPipelineConstants(slot);
			}
	}

	void SceneStore::BuildAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Tables");
		if (!sceneBuilt)
			return;  // a load screen, or the feature installed mid-frame: nothing to patch
		PartTimer timer(stats.partMs, &stats.accumulatePartMs);
		std::uint32_t fadingThisFrame = 0;
		TracyCZoneN(captureZone, "CS.DCLF.Accumulate.Capture", true);
		CheckRegistrations();
		// The pass table holds the frame's membership joins alone (BindByMembership).
		accumulatedPasses.clear();
		accumulatedLayerPasses.clear();
		// Scene membership: the records written since, bound (patched once below, then kept).
		residentJoining.clear();
		residentLayerJoining.clear();
		BindByMembership();
		timer.Add(BuildPart::Walk);

		TracyCZoneEnd(captureZone);

		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8) | (decalBiasMode[3] << 16);
		const bool lightLimitFixLoaded = globals::features::lightLimitFix.loaded;
		// CS_DCLF_PERSISTENT_PARITY's frames: the cached derivation is served and also recomputed, and the two compared.
		const bool derivedProbe = SwitchEnabled(Switch::PersistentParity) && ParityDue(frame, 13);
		const bool derivationStats = SwitchEnabled(Switch::DeriveProbe);

		// What this phase has anything to do with: the frame's membership joins (BindByMembership). Every other
		// record keeps its binding, or has none and is a culling candidate only.
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
		// One membership join: an entry's base record, or its layer's (Tracked::layerSlot, drawn from the layer property).
		auto join = [&](RE::BSGeometry* geometry, Tracked* trackedEntry, const AccumulatedPass* accumulated, bool layer) {
			timer.Add(BuildPart::LoopTail);
			// BindByMembership joins only records written this frame with a verdict of None, never a shadow-only one.
			const std::uint32_t objectId = layer ? trackedEntry->layerSlot : trackedEntry->objectId;
			auto& object = tables.objects[objectId];
			const std::uint32_t geometrySlot = object.geometryIndex;

			auto& data = geometry->GetGeometryRuntimeData();
			auto* property = layer ? LayerPropertyOf(*geometry) : data.shaderProperty.get();
			if (!property)
				return;
			auto* witnessProperty = property;
			const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
			const std::uint8_t fadeState = FadeStateOf(witnessProperty);
			RE::BSLightingShaderProperty* castCache = layer ? netimmerse_cast<RE::BSLightingShaderProperty*>(property) :
			                                          trackedEntry->castProperty == witnessProperty ? trackedEntry->castResult : nullptr;
			const bool alphaBelowOne = witnessMaterial && static_cast<const RE::BSLightingShaderMaterialBase*>(witnessMaterial)->materialAlpha < 1.0f;

			// The positive derivation, cached (Tracked::Derived): for an accumulated object whose
			// witnesses all match and whose slots still carry the keys they were derived for, the
			// classification and the whole derived section are skipped.
			auto& derived = layer ? trackedEntry->layerDerived : trackedEntry->derived;
			bool derivedHit = derived.valid && derived.generation == tablesGeneration &&
			                  derived.geometrySlot == geometrySlot && derived.property == witnessProperty &&
			                  derived.material == witnessMaterial && derived.fadeState == fadeState && derived.technique == accumulated->technique &&
			                  derived.subPass == accumulated->subPass && derived.hint == accumulated->hint && derived.interior == interior &&
			                  derived.alphaBelowOne == alphaBelowOne && derived.biasWitness == biasWitness;
			if (derivedHit) {
				derivedHit = derived.pipelineSlot < tables.pipelines.size() && tables.pipelineSlots.Alive(derived.pipelineSlot) &&
				             tables.pipelines[derived.pipelineSlot] == derived.key &&
				             derived.materialSlot < tables.materialSlotKey.size() && tables.materialSlots.Alive(derived.materialSlot) &&
				             tables.materialSlotKey[derived.materialSlot] == std::pair{ derived.material, derived.descriptors.pass };
			}

			LightingDescriptors descriptors;
			Ineligible reason = Ineligible::None;
			if (derivedHit && !derivedProbe) {
				descriptors = derived.descriptors;
				// The LOD fades are this frame's GetRenderPasses' (the property's fields, as ClassifyStatic reads them for an
				// accumulated object), not the cached derivation's.
				const auto& lightingProperty = *static_cast<const RE::BSLightingShaderProperty*>(property);
				descriptors.specularLODFade = lightingProperty.specularLODFade;
				descriptors.envmapLODFade = lightingProperty.envmapLODFade;
				++stats.derivedHits;
			} else {
				reason = layer ? ClassifyLayer(*geometry, &descriptors, accumulated) : ClassifyStatic(*geometry, &descriptors, accumulated, &castCache);
			}
			timer.Add(BuildPart::ClassifyStatic);
			// Per frame whether or not the derivation was cached: hidden, part of an actor and fading are
			// states of this frame, and the scene phase's verdict for them is the last classification's
			// (for a static, the last event's). An object that has just been hidden must lose its bindings now,
			// or DCLF keeps drawing what the engine has stopped drawing.
			if (reason == Ineligible::None)
				reason = ClassifyFrame(*trackedEntry, accumulated);
			// The skin partitions are the walk's (Tables::skinPartitions): GetRenderPasses gives the main pass the same row, the
			// fade node's LOD level.
			// The histogram is the scene phase's, taken over the whole tracked set; where this phase reaches a
			// different verdict, the object is moved between the buckets.
			if (!layer && reason != trackedEntry->candidateReason) {
				--stats.ineligible[static_cast<std::size_t>(trackedEntry->candidateReason)];
				++stats.ineligible[static_cast<std::size_t>(reason)];
			}
			if (reason != Ineligible::None) {
				if (!layer) {
					trackedEntry->accumulateReason = reason;
					trackedEntry->accumulateReasonFrame = frame;
				}
				// Eligible for a record but not for bindings: it stays native, which is what its scene record
				// already says (kObjectNoBindings).
				derived.valid = false;
				return;
			}
			timer.Add(BuildPart::ClassifyFrame);

			// Only computed when something will report them: this whole block exists to feed one log line.
			if (!layer && derivationStats && descriptors.derivedPass != kNotDerived) {
				++stats.derivationChecked;
				// Against the pass the engine registered. Where its LOD fades ran out it dropped Specular or the Envmap
				// technique, which the derivation keeps (the draw fades them): those bits are the fades' and counted apart,
				// and the draw's fades are checked against the engine's own (LodMetricOf, LodFadeAt).
				std::uint32_t differing = descriptors.derivedPass ^ accumulated->technique;
				if (!lodFadeSampled) {
					lodFadeSample = SampleLodFadeFrame();
					lodFadeSampled = true;
				}
				if (const auto node = LodFadeNodeOf(property); LodFadesApply(node) && lodFadeSample.fadesOn != 0.0f) {
					++stats.lodFadeChecked;
					const float metric = LodMetricOf(lodFadeSample, node);
					const float engineMetric = std::bit_cast<float>(property->fadeNode->GetRuntimeData().unk144);
					const bool metricSame = std::abs(metric - engineMetric) <= 1e-4f * std::max(1.0f, std::abs(engineMetric));
					if (!metricSame)
						++stats.lodMetricDiffers;
					const auto& lighting = *static_cast<const RE::BSLightingShaderProperty*>(property);
					const std::uint32_t registered = accumulated->technique;
					std::uint32_t fadeBits = 0;
					bool fadeDiffers = false;
					if (descriptors.derivedPass & kSpecularBit) {
						const float drawn = LodFadeAt(metric, lodFadeSample.specularStart, lodFadeSample.specularEnd);
						const float engine = (registered & kSpecularBit) ? lighting.specularLODFade : 0.0f;
						fadeDiffers |= std::abs(drawn - engine) > 1e-3f;
						if (!(registered & kSpecularBit))
							fadeBits |= kSpecularBit;
					}
					if (((descriptors.derivedPass >> 24) & 0x3f) == kTechniqueEnvmap) {
						const float drawn = LodFadeAt(metric, lodFadeSample.envmapStart, lodFadeSample.envmapEnd);
						const bool engineOn = ((registered >> 24) & 0x3f) == kTechniqueEnvmap;
						const float engine = engineOn ? lighting.envmapLODFade : 0.0f;
						fadeDiffers |= std::abs(drawn - engine) > 1e-3f;
						if (!engineOn)
							fadeBits |= 0x3fu << 24;
					}
					if (fadeDiffers) {
						++stats.lodFadeDiffers;
						if (stats.lodFadeFirst.empty())
							stats.lodFadeFirst = fmt::format("'{}' metric {} (engine {}), specular {} (engine {}), envmap {} (engine {}), registered {:08X}",
								geometry->name.c_str() ? geometry->name.c_str() : "?", metric, engineMetric,
								LodFadeAt(metric, lodFadeSample.specularStart, lodFadeSample.specularEnd), lighting.specularLODFade,
								LodFadeAt(metric, lodFadeSample.envmapStart, lodFadeSample.envmapEnd), lighting.envmapLODFade, registered);
					}
					if (differing & fadeBits) {
						++stats.derivationFadeBits;
						differing &= ~fadeBits;
					}
				}
				if (const std::uint32_t bits = differing & ~kRuntimePassBits) {
					++stats.derivationDiffers;
					stats.derivationBits |= bits;
				}
				if (const std::uint32_t bits = differing & kRuntimePassBits) {
					++stats.derivationRuntimeDiffers;
					stats.derivationRuntimeBits |= bits;
				}
				for (std::uint32_t remaining = differing; remaining;) {
					const std::uint32_t bit = std::countr_zero(remaining);
					++stats.derivationBitCounts[bit];
					remaining &= remaining - 1;
				}
			} else if (!layer && derivationStats && descriptors.derivedPass == kNotDerived) {
				++stats.derivationNative;
			}
			timer.Add(BuildPart::Diagnostics);

			std::uint32_t pipelineSlot = Tables::kSlotFree, materialSlot = Tables::kSlotFree, staticFlags = 0;
			PipelineKey key{};
			if (derivedHit && !derivedProbe) {
				pipelineSlot = derived.pipelineSlot;
				materialSlot = derived.materialSlot;
				staticFlags = derived.staticFlags;
				key = derived.key;
				tables.MarkMaterialUsed(materialSlot);
				joinMarkedMaterials.push_back(materialSlot);
				// A pipeline's template is always a member's property (the joiner's while the slot has no member; KeepResidentsAlive
				// takes a resident's every frame), so a persistent slot never points at a property the game has since freed. The
				// constants are evaluated from it at Prepass (RefreshFrameConstants).
				if (!tables.PipelineUsed(pipelineSlot)) {
					tables.MarkPipelineUsed(pipelineSlot);
					joinMarkedPipelines.push_back(pipelineSlot);
					tables.geometryTemplate[pipelineSlot] = property;
				}
				timer.Add(BuildPart::DedupHit);
			} else {
				const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
				// A layer's draws apply no alpha property (render flags 0x41, engine notes).
				const auto* alpha = layer ? nullptr : data.alphaProperty.get();
				const bool alphaTest = alpha && alpha->GetAlphaTesting();

				// A decal's key carries the engine's fixed-function state indices as well (Records.h): the
				// depth-bias mode from the frame, blend and write modes from the alpha property (derived with
				// the descriptors). Zero for everything else, so an opaque key is exactly what it was.
				std::uint32_t rasterFlags = twoSided ? kRasterTwoSided : 0u;
				if (descriptors.decalGroup)
					rasterFlags |= PackDecalRasterFlags(descriptors.decalGroup, decalBiasMode[descriptors.decalGroup & 3], descriptors.decalBlendMode, descriptors.decalWriteMode);
				if (globals::features::extendedTranslucency.loaded)
					rasterFlags |= ((ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ ExtendedTranslucency::MaterialModelOf(geometry)) & 7u) << kRasterTranslucencyShift;
				key = PipelineKey{ descriptors.vertex, descriptors.pixel, rasterFlags, descriptors.pass,
					VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
				auto pipelineIt = pipelineIndex.find(key);
				const bool newPipeline = pipelineIt == pipelineIndex.end();
				if (!newPipeline && !tables.PipelineUsed(pipelineIt->second)) {
					// The slot's first member: this object's property is the template (see the cached path).
					tables.MarkPipelineUsed(pipelineIt->second);
					joinMarkedPipelines.push_back(pipelineIt->second);
					tables.geometryTemplate[pipelineIt->second] = property;
				}
				if (newPipeline) {
					timer.Add(BuildPart::Dedup);
					const std::uint32_t slot = AllocatePipelineSlot();
					tables.pipelines[slot] = key;
					// Its PerGeometry values (the engine's SetupGeometry from the template's lighting pass) are the frame's
					// (FrameTables): evaluated by the render thread for a slot whose key or binding is new.
					tables.geometryTemplate[slot] = property;

					tables.pipelineTechnique[slot] = TechniqueRowFor(descriptors.pass);
					stats.shadowMaskPipelines += tables.TechniqueShadowMask(slot) ? 1 : 0;

					PipelinePermutation permutation;
					permutation.vertexShaderDescriptor = descriptors.rawVertex;
					permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
					permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
					// AdditiveLighting (State::UpdateLightingShaderPermutation): a pass whose alpha property blends onto the target
					// (destination ONE). Only a blended decal (group 2) applies its alpha property, and its key carries the blend
					// mode, which the blend functions decide: the same for every object of the key.
					if (descriptors.decalGroup == 2 && alpha && alpha->GetAlphaBlending() && alpha->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kOne)
						permutation.extraShaderDescriptor |= static_cast<std::uint32_t>(State::ExtraShaderDescriptors::AdditiveLighting);
					// Extended Translucency's material model, as its SetupGeometry hook sets it (the key carries it):
					// disabled for opaque geometry, the default or the mesh's own for blended geometry.
					permutation.extraFeatureDescriptor = globals::features::extendedTranslucency.loaded ?
					                                         (ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ RasterTranslucency(key.rasterFlags))
					                                             << ExtendedTranslucency::ExtraFeatureDescriptorShift :
					                                         0u;
					tables.permutations[slot] = permutation;
					tables.pipelineBindingVersion[slot] = tables.NextVersion();
					pipelineIt = pipelineIndex.emplace(key, slot).first;
					timer.Add(BuildPart::PipelineEval);
				}
				pipelineSlot = pipelineIt->second;
				tables.MarkPipelineUsed(pipelineSlot);
				joinMarkedPipelines.push_back(pipelineSlot);

				// Material state as the engine's SetupMaterial produces it for this pass descriptor.
				const auto* material = property->material;
				auto materialIt = materialIndex.find(std::pair{ material, descriptors.pass });
				if (materialIt == materialIndex.end()) {
					timer.Add(BuildPart::Dedup);
					// The record is the engine's SetupMaterial, which only the render thread runs (ServeMaterialRequests): asked for, and
					// the join waits for it, staying native meanwhile. No shader instance yet (nothing drawn so far) asks again.
					const auto materialKey = std::pair{ material, descriptors.pass };
					const auto served = materialsServed.find(materialKey);
					if (served == materialsServed.end() || !served->second.valid) {
						if (served != materialsServed.end()) {
							materialsHandedBack.push_back(std::move(served->second.owner));
							materialsServed.erase(served);
						}
						if (materialRequested.insert(materialKey).second) {
							MaterialRequest request;
							request.owner.reset(const_cast<RE::BSShaderMaterial*>(material));
							request.material = material;
							request.pass = descriptors.pass;
							materialRequests.push_back(std::move(request));
						}
						bindRetry.push_back(objectId);
						++residentStats.materialWaits;
						derived.valid = false;
						return;
					}
					MaterialRecord record = served->second.record;
					const std::uint32_t slot = AllocateMaterialSlot();
					materialOwners.resize(tables.materials.size());
					materialOwners[slot] = std::move(served->second.owner);
					materialsServed.erase(served);
					tables.materials[slot] = record;
					tables.materialVersion[slot] = ++materialVersions;
					tables.NoteMaterial(slot);
					tables.materialSlotKey[slot] = std::pair{ material, descriptors.pass };
					materialIt = materialIndex.emplace(std::pair{ material, descriptors.pass }, slot).first;
					ListMaterialDependent(material, slot);
					timer.Add(BuildPart::MaterialEval);
				}
				materialSlot = materialIt->second;
				tables.MarkMaterialUsed(materialSlot);
				joinMarkedMaterials.push_back(materialSlot);
				timer.Add(BuildPart::DedupHit);

				staticFlags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
				              (ExternalEmittance::ShouldSuppress(interior, property, geometry) ? kObjectSuppressExternalEmittance : 0u) |
				              (descriptors.technique == kTechniqueTreeAnim ? kObjectTreeAnim : 0u) |
				              (alphaTest ? static_cast<std::uint32_t>(alpha->alphaThreshold) << kObjectAlphaThresholdShift : 0u) |
				              (alphaTest && alpha->GetAlphaBlending() ? kObjectAlphaBlended : 0u) |
				              (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u) |
				              ((property->flags.underlying() & ((1ull << 14) | (1ull << 46))) ? kObjectLandscapeLights : 0u) |
				              (BeastRaceFace(*property, *geometry) ? kObjectBeastRace : 0u);
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

			// The patch. Everything above decided what this object draws with; here it goes into the
			// record the scene phase appended, at the index that phase fixed.
			// A member's pass is patched once and kept; its extras rows (projected UV, land blend) are its static parts, which the
			// draw completes from the frame's (ExtrasFrame).
			const bool landBlendRecord = descriptors.technique == 8 || descriptors.technique == 19;
			AccumulatePatch patch;
			patch.object = objectId;
			patch.material = materialSlot;
			patch.pipeline = pipelineSlot;
			patch.projectedUV = descriptors.projectedUV;
			patch.landBlend = landBlendRecord;
			// The sun's bits are decided by the draw (the synthetic pass's kObjectSunTest). Membership of the set is CommitSet's, which
			// keeps a record being bound out of it: a join never patches a member (SetStats::patchedMember).
			patch.flags = (object.flags & kSceneKeptFlags) | staticFlags | (accumulated->sunTest ? kObjectSunTest : 0u) |
			              (accumulated->fadeDistance != 0.0f ? kObjectFadeTest : 0u) | (accumulated->heightTest ? kObjectHeightTest : 0u) |
			              (patch.projectedUV ? kObjectProjectedUV : 0u) | (patch.landBlend ? kObjectLandBlend : 0u) |
			              ((PhasesIn(tables, objectId) & kSetMain) ? kObjectMember : 0u);
			if (PhasesIn(tables, objectId) & kSetMain)
				++setStats.patchedMember;
			patch.fadeDistance = accumulated->fadeDistance;
			timer.Add(BuildPart::Record);
			if (lightLimitFixLoaded) {
				if (trackedEntry->roomMapGeneration != roomMapGeneration) {
					trackedEntry->roomIndex = -1;
					if (roomMap && trackedEntry->roomNode)
						if (const auto room = roomMap->find(trackedEntry->roomNode); room != roomMap->end())
							trackedEntry->roomIndex = room->second;
					trackedEntry->roomMapGeneration = roomMapGeneration;
				}
				patch.lights.roomIndex = trackedEntry->roomIndex;
			}
			if (patch.flags & kObjectTreeAnim)
				DeriveTreeAnim(*property, patch.tree);
			timer.Add(BuildPart::CapturePatch);
			if (descriptors.pass & kPassAdditionalAlphaMask) {
				if (fadingThisFrame++ == 0)
					++stats.fadingFrames;
				++stats.fadingDrawn;
			}
			// A member decal's chain (DecalOrder.cpp): its group, technique and sub-pass.
			if (descriptors.decalGroup) {
				patch.decalKey = (std::uint64_t(descriptors.decalGroup) << 60) | (std::uint64_t(accumulated->technique & 0x3FFFFFFF) << 28) |
				                 (std::uint64_t(accumulated->subPass & 7) << 24);
			}
			timer.Add(BuildPart::Record);
			ApplyAccumulatePatch(patch);
			timer.Add(BuildPart::ApplyPatch);
			MarkResidentSlot(objectId, { *accumulated, pipelineSlot, materialSlot });
			(layer ? residentLayerJoining : residentJoining).erase(geometry);
			++residentStats.joined;
			timer.Add(BuildPart::Record);
		};
		for (const auto& entry : accumulateOrder)
			join(entry.geometry, entry.tracked, entry.accumulated, entry.layer);

		TracyCZoneEnd(objectsZone);
		TracyCZoneN(residentsZone, "CS.DCLF.Accumulate.Residents", true);
		// Membership joins that were not patched (a verdict of the frame, a material not ready): their entries leave
		// residency.
		for (const auto* geometry : residentJoining) {
			++residentStats.failed;
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			const auto cause = entry == tracked.end() || entry->second.objectStamp != objectStamp ? 1u :                  // no record
			                   entry->second.accumulateReasonFrame == frame ? 2u :                                          // a verdict of the frame
			                   3u;                                                                                          // material, or extras rows
			++residentStats.failedBy[cause];
			// A member whose binding could not be taken again is not bound any more.
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
		// A base and its layer are bound together: the engine's passes of both are its geometry's (the set is by geometry), so a
		// base whose layer did not join leaves again, and a layer whose base did not.
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
		if (ResidentParityEnabled() && ParityDue(frame) && !residents.empty())
			CheckResidentParity();
		{
			// CS_DCLF_PERSISTENT_PARITY: no bound object references a slot that is not live. Slots are freed only when nothing references
			// them (their reference counts), which this checks; the normal path trusts them (invariant 5).
			if (SwitchEnabled(Switch::PersistentParity) && (slotsFreedThisFrame || ParityDue(frame)))
				CheckObjectSlots(frameResolveBuffers);
			slotsFreedThisFrame = false;
		}
		TracyCZoneEnd(residentsZone);
		TracyCZoneN(statsZone, "CS.DCLF.Accumulate.StatsAndDecals", true);
		const bool slotProbe = SwitchValue(Switch::SlotProbe) == "1";
		if (slotProbe)
			ProbeSlots(frameResolveBuffers);
		// The slot counts, for the reports: the used sets' sizes, and the slot tables' live and referenced counts.
		auto countBits = [](const std::vector<std::uint64_t>& a_bits) {
			std::uint32_t count = 0;
			for (const auto word : a_bits)
				count += static_cast<std::uint32_t>(std::popcount(word));
			return count;
		};
		stats.pipelines = countBits(tables.usedPipelineBits);
		stats.materials = countBits(tables.usedMaterialBits);
		// Decal draw order (DecalOrder.cpp).
		OrderDecals();
		stats.geometries = static_cast<std::uint32_t>(tables.geometrySlots.ReferencedCount());
		stats.geometriesAlive = static_cast<std::uint32_t>(tables.geometrySlots.AliveCount());
		stats.pipelinesAlive = static_cast<std::uint32_t>(tables.pipelineSlots.AliveCount());
		stats.materialsAlive = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		stats.materialCacheEntries = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		TracyCZoneEnd(statsZone);
	}

	void SceneStore::BindByMembership()
	{
		ZoneScopedN("CS.DCLF.Accumulate.BindByMembership");
		// The frame globals a membership pass reads changed (the static sun bits, the fade distances): every resident is bound
		// again from this frame's.
		if (const std::uint32_t witness = frameMembershipWitness; witness != membershipWitness) {
			bindQueue.insert(bindQueue.end(), residents.begin(), residents.end());
			EndAllResidency();
			membershipWitness = witness;
		}
		// Material records are the engine's SetupMaterial through its Lighting shader, which exists only once the engine has drawn
		// with it: until then the queue waits (the records at load are the whole scene).
		if (!ConstantEvaluator::Get().HasLightingShader())
			return;
		auto& primary = PrimaryCull::Get();
		// The joins that waited for a material record, again.
		bindQueue.insert(bindQueue.end(), bindRetry.begin(), bindRetry.end());
		bindRetry.clear();
		for (const std::uint32_t slot : std::exchange(bindQueue, {})) {
			if (slot >= tables.objects.size() || (tables.objects[slot].flags & (kObjectFree | kObjectShadowOnly)))
				continue;
			auto* geometry = const_cast<RE::BSGeometry*>(tables.objectGeometry[slot]);
			const auto trackedIt = geometry ? tracked.find(geometry) : tracked.end();
			// A layer slot (WriteLayer) joins with its own pass: its property's, with hint 12.
			const bool layer = tables.IsLayer(slot);
			// A record written every frame (a mover, an actor's part, animated shading) is bound again every frame: its slots are
			// referenced again in the same batch, so nothing about it is retired.
			if (trackedIt == tracked.end() || (layer ? trackedIt->second.layerSlot : trackedIt->second.slot) != slot ||
				trackedIt->second.objectStamp != objectStamp || trackedIt->second.candidateReason != Ineligible::None)
				continue;
			// A member written again keeps its binding while what the binding reads is the same (the derivation cache's
			// witnesses): a skin, a face or a mover is written every frame for its transform, bones or stream alone.
			const auto& data = geometry->GetGeometryRuntimeData();
			if (IsResidentSlot(slot) && MemberBindingStands(slot, *geometry, trackedIt->second)) {
				++residentStats.membershipKept;
				continue;
			}
			AccumulatedPass pass;
			const auto* layerProperty = layer ? netimmerse_cast<const RE::BSLightingShaderProperty*>(LayerPropertyOf(*geometry)) : nullptr;
			if (layer ? !layerProperty || !primary.MembershipLayerPass(geometry, *layerProperty, pass) : !primary.MembershipPass(geometry, pass))
				continue;
			// A fade node's objects carry its fade-out distance for BuildDraws' fade test, which measures from the node's centre
			// (Tables::lodFade, SetFadeRow). A tree's also take its height test.
			if (const auto* fadeNode = data.shaderProperty ? data.shaderProperty->fadeNode : nullptr) {
				pass.fadeDistance = PrimaryCull::MembershipFadeDistance(fadeNode);
				const auto* rtti = fadeNode->GetRTTI();
				pass.heightTest = rtti && rtti->name && std::strcmp(rtti->name, "BSTreeNode") == 0;
			}
			(layer ? accumulatedLayerPasses : accumulatedPasses).insert_or_assign(geometry, pass);
			(layer ? residentLayerJoining : residentJoining).insert(geometry);
			// A record bound again leaves the main phases of the coordinator's set first (a join never patches a member): the frame
			// draws its old record from the snapshot meanwhile, and the next frame's start takes the claim back (RevokeClaims), as the
			// walk's join used to before the frame read a snapshot (step 6c).
			// The commit's decision too, applied or waiting for its application (LeaveSet).
			LeaveSet(slot, kSetMain | kSetReflection);
			if (PhasesIn(tables, slot) & (kSetMain | kSetReflection)) {
				tables.setPhases[slot] &= static_cast<std::uint8_t>(~(kSetMain | kSetReflection));
				if (tables.objects[slot].flags & kObjectMember) {
					tables.objects[slot].flags &= ~kObjectMember;
					tables.NoteChange(slot, kChangeBindings);
				}
			}
			++residentStats.membershipQueued;
		}
	}

	bool SceneStore::MemberBindingStands(std::uint32_t a_slot, const RE::BSGeometry& a_geometry, const Tracked& a_entry) const
	{
		const bool layer = tables.IsLayer(a_slot);
		const auto& derived = layer ? a_entry.layerDerived : a_entry.derived;
		const auto* property = layer ? LayerPropertyOf(a_geometry) : a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* material = property ? property->material : nullptr;
		const bool alphaBelowOne = material && static_cast<const RE::BSLightingShaderMaterialBase*>(material)->materialAlpha < 1.0f;
		return derived.valid && derived.generation == tablesGeneration && derived.geometrySlot == tables.objects[a_slot].geometryIndex &&
		       derived.property == property && derived.material == material && derived.fadeState == FadeStateOf(property) &&
		       derived.alphaBelowOne == alphaBelowOne && derived.interior == frameInterior && derived.pipelineSlot == tables.objects[a_slot].pipelineIndex &&
		       derived.materialSlot == tables.objects[a_slot].materialIndex;
	}

	void SceneStore::ResetAccumulatedHalf(std::uint32_t a_slot)
	{
		if (a_slot >= tables.objects.size() || (tables.objects[a_slot].flags & kObjectFree))
			return;
		const auto columnsBefore = tables.ColumnsOf(a_slot);
		auto& object = tables.objects[a_slot];
		object.flags = tables.sceneFlags[a_slot];
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		tables.draws[a_slot].pipelineIndex = 0;
		tables.lights[a_slot] = ObjectLights{};
		tables.treeAnim[a_slot] = ObjectTreeAnim{};
		tables.FreeExtras(a_slot);
		tables.fadeDistance[a_slot] = 0.0f;
		tables.NoteWrite(a_slot, columnsBefore);
	}

	void SceneStore::RefreshShadowSets(bool a_forceRebuild)
	{
		ZoneScopedN("CS.DCLF.Capture.ShadowSets");
		// ClearFrame reconstructs the public list every walk; the dependency
		// index, not that list, holds previous membership across frames.
		ankerl::unordered_dense::set<ID3D11ShaderResourceView*> previousTextures;
		previousTextures.reserve(shadowTextureMembers.size());
		for (const auto& entry : shadowTextureMembers)
			previousTextures.insert(entry.first);
		bool rebuild = a_forceRebuild || shadowIndexNeedsRebuild;
		std::vector<std::uint32_t> changed = std::move(shadowDirtySlots);
		shadowDirtySlots.clear();
		if (!rebuild) {
			stats.shadowCasters = keptShadowCasters;
			stats.shadowRejects = keptShadowRejects;
			if (changed.empty())
				rebuild = true;  // an unjournaled invalidation must still rebuild the index
		}
		if (rebuild) {
			shadowIndexedInputs.clear();
			shadowIndexedLive.clear();
			shadowTextureMembers.clear();
			shadowKeyMembers.clear();
			for (auto& members : occlusionKeyMembers)
				members.clear();
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
		auto member = [](auto& a_index, const auto& a_key, std::uint32_t a_slot, bool a_add) {
			if (a_add) {
				a_index[a_key].insert(a_slot);
			} else if (auto it = a_index.find(a_key); it != a_index.end()) {
				it->second.erase(a_slot);
				if (it->second.empty())
					a_index.erase(it);
			}
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
				member(shadowTextureMembers, a_inputs.diffuse, a_slot, a_add);
			const auto raster = (a_inputs.flags & kObjectTwoSided) ? kRasterTwoSided : 0u;
			const auto vertex = VertexLayoutOf(a_inputs.vertexDesc);
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				if (a_inputs.occlusionTechnique[v])
					member(occlusionKeyMembers[v], ShadowPipelineKey{ a_inputs.occlusionTechnique[v], raster, vertex }, a_slot, a_add);
			if (caster)
				member(shadowKeyMembers, ShadowPipelineKey{ a_inputs.technique, raster, vertex }, a_slot, a_add);
		};
		for (const auto slot : changed) {
			if (slot >= tables.objects.size())
				continue;
			const bool live = !(tables.objects[slot].flags & kObjectFree);
			const auto inputs = ShadowInputsOf(slot);
			if ((shadowIndexedLive[slot] != 0) == live && shadowIndexedInputs[slot] == inputs)
				continue;
			apply(slot, shadowIndexedInputs[slot], shadowIndexedLive[slot] != 0, false);
			apply(slot, inputs, live, true);
			shadowIndexedInputs[slot] = inputs;
			shadowIndexedLive[slot] = live;
		}
		shadowSetsDirty = false;
		tables.shadowTextureSet.clear();
		tables.shadowTextureSeen.clear();
		tables.shadowKeysUsed.clear();
		for (auto& keys : tables.occlusionKeysUsed)
			keys.clear();
		auto emit = [](const auto& a_index, auto& a_out) {
			using Key = typename std::decay_t<decltype(a_index)>::key_type;
			std::vector<std::pair<std::uint32_t, Key>> ordered;
			ordered.reserve(a_index.size());
			for (const auto& [key, slots] : a_index)
				ordered.emplace_back(*slots.begin(), key);
			std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (const auto& [slot, key] : ordered)
				a_out.push_back(key);
		};
		emit(shadowTextureMembers, tables.shadowTextureSet);
		for (auto* texture : tables.shadowTextureSet)
			tables.shadowTextureSeen.insert(texture);
		for (auto* texture : previousTextures)
			if (!shadowTextureMembers.contains(texture))
				tables.shadowTextureChanges.emplace_back(texture, false);
		for (const auto& entry : shadowTextureMembers)
			if (!previousTextures.contains(entry.first))
				tables.shadowTextureChanges.emplace_back(entry.first, true);
		emit(shadowKeyMembers, tables.shadowKeysUsed);
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			emit(occlusionKeyMembers[v], tables.occlusionKeysUsed[v]);
	}
}
