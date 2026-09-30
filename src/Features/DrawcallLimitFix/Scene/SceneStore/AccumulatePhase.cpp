#include "Internal.h"

namespace DCLF
{
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
		// again, a verdict not taken again) shows up here.
		for (const auto& entry : entries) {
			if (!entry.geometry || !mainBatchRenderers.contains(entry.batch) || entry.fading)
				continue;
			const auto it = tracked.find(const_cast<RE::BSGeometry*>(entry.geometry));
			if (it == tracked.end() || it->second.candidateReason != Ineligible::None || IsMember(FindObject(entry.geometry)))
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

	void SceneStore::ApplyAccumulatePatch(const AccumulatePatch& a_patch)
	{
		const std::uint32_t objectId = a_patch.object;
		const AccumulateSnapshot before(tables, objectId);
		auto& object = tables.objects[objectId];
		object.materialIndex = a_patch.material;
		if (a_patch.resident) {
			if (materialMember.size() <= a_patch.material)
				materialMember.resize(std::size_t(a_patch.material) + 1, 0);
			materialMember[a_patch.material] = 1;
		}
		object.pipelineIndex = a_patch.pipeline;
		object.flags = a_patch.flags | FadedOutBit(objectId);
		tables.fadeDistance[objectId] = a_patch.fadeDistance;
		tables.SetWatch(objectId, Tables::kWatchExtras, a_patch.projectedUV || a_patch.landBlend);
		tables.draws[objectId].pipelineIndex = a_patch.pipeline;
		tables.shading[objectId] = a_patch.shading;
		tables.emissiveMult[objectId] = a_patch.emissiveMult;
		tables.lights[objectId] = a_patch.lights;
		tables.treeAnim[objectId] = a_patch.tree;
		if (a_patch.projectedUV || a_patch.landBlend) {
			stats.projectedUV += a_patch.projectedUV ? 1 : 0;
			stats.landBlend += a_patch.landBlend ? 1 : 0;
			if (tables.extraOffset[objectId] == kNoExtraRows)
				tables.extraOffset[objectId] = tables.AllocateExtras();
		} else {
			tables.FreeExtras(objectId);
		}
		// This object reached the patch only after the IsResidentSlot fast path
		// rejected it, so resident means a new membership. Include that column
		// in the same before/after journal as the other value-only writes.
		if (a_patch.resident)
			tables.residentSlot[objectId] = 1;
		before.NoteWrite(tables, objectId);
		stats.nativeVisible += a_patch.nativeVisible ? 1 : 0;
		stats.nativeShadowMasked += a_patch.nativeShadowMasked ? 1 : 0;
		stats.derivedDescriptors += a_patch.derivedDescriptor ? 1 : 0;
		if (a_patch.decalKey) {
			++stats.decals[(static_cast<std::uint32_t>(a_patch.decalKey >> 60) - 1) & 1];
			if (a_patch.resident) {
				memberDecals[objectId] = a_patch.decalKey & ~std::uint64_t(0xFFFFFF);
				memberDecalsChanged = true;
			} else {
				decalOrder.push_back({ a_patch.decalKey, objectId });
			}
		}
	}

	/**
	 * @brief The accumulator half of the frame, at EarlyPrepass.
	 *
	 * The main camera's passes are complete only once Main_RenderShadowMaps returns, so everything that
	 * depends on them is here: the capture drain, the pipeline and material slots, the per-frame lighting
	 * template, the shading and light lists, the decal order and kObjectNativeVisible. It patches the
	 * records BuildScenePhase appended, in place and by object index.
	 */
	void SceneStore::BuildAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Tables");
		if (!sceneBuilt) {
			// A load screen, or the feature installed mid-frame: nothing to patch.
			DrainCapture();
			return;
		}
		PartTimer timer(stats.partMs, &stats.accumulatePartMs);
		std::uint32_t fadingThisFrame = 0;
		TracyCZoneN(captureZone, "CS.DCLF.Accumulate.Capture", true);
		// The pass table holds the frame's membership joins alone (BindByMembership); the engine's registrations are drained
		// for the diagnostics.
		RefreshMainBatchRenderers();
		accumulatedPasses.clear();
		DrainCapture();
		// The roots the last decode found faded out or back in (kObjectFadedOut); then the frames of visibility feedback that
		// completed, decoded on the worker (the stood-in roots' fade, LOD and tree state).
		PrimaryCull::Get().ApplyFadeChanges();
		PrimaryCull::Get().KickFeedbackDecode();
		// Scene membership: the records written since, bound (patched once below, then kept).
		residentJoining.clear();
		BindByMembership();
		timer.Add(BuildPart::Walk);

		TracyCZoneEnd(captureZone);

		auto& evaluator = ConstantEvaluator::Get();
		const bool interior = frameInterior;
		const auto& decalBiasMode = frameDecalBias;
		const std::uint32_t biasWitness = decalBiasMode[1] | (decalBiasMode[2] << 8);
		const bool lightLimitFixLoaded = globals::features::lightLimitFix.loaded;
		// CS_DCLF_DERIVED_CACHE=probe: the cached derivation is served and also recomputed, and the two compared.
		const bool derivedProbe = SwitchValue(Switch::DerivedCache) == "probe";
		const bool derivationStats = SwitchEnabled(Switch::DeriveProbe);
		// CS_DCLF_PRIMARY_EXCLUDE=probe: what the objects under the primary's candidate entries take from their
		// registration, against what DCLF derives (PrimaryCull::NoteDerived).
		const bool primaryProbe = PrimaryCull::Probe() && PrimaryCull::Get().Installed();

		// What this phase has anything to do with: the engine's accumulated passes alone - ~1,700 of the exterior's
		// 10,000 tracked objects. An object the engine did not accumulate cannot be drawn (the draws are gated on the
		// engine's visibility): it is a culling candidate and nothing more, and its scene record already carries
		// everything the culling reads.
		accumulateOrder.clear();
		TracyCZoneN(orderZone, "CS.DCLF.Accumulate.Order", true);
		accumulateOrder.reserve(accumulatedPasses.size());
		for (auto& [passGeometry, pass] : accumulatedPasses) {
			auto* mutableGeometry = const_cast<RE::BSGeometry*>(passGeometry);
			auto trackedIt = tracked.find(mutableGeometry);
			if (trackedIt != tracked.end())
				accumulateOrder.push_back({ mutableGeometry, &trackedIt->second, &pass });
		}
		timer.Add(BuildPart::PassLookup);
		TracyCZoneEnd(orderZone);
		TracyCZoneN(objectsZone, "CS.DCLF.Accumulate.Objects", true);
		for (auto& [geometry, trackedEntry, accumulated] : accumulateOrder) {
			timer.Add(BuildPart::LoopTail);
			if (!accumulated)
				continue;
			if (trackedEntry->objectStamp != objectStamp) {
				// No record: the scene phase found it ineligible, which is where the histogram's "drawn"
				// column comes from. A verdict of None here means the record itself failed (an unstable
				// buffer) or the cached verdict was stale, so that one is cleared and reported.
				if (accumulated) {
					++stats.ineligibleDrawn[static_cast<std::size_t>(trackedEntry->candidateReason)];
					if (trackedEntry->candidateReason == Ineligible::None) {
						++stats.accumulatedWithoutRecord;
						trackedEntry->candidateFrame = 0;
						pendingEvaluation.push_back(geometry);
					} else if (trackedEntry->candidateReason == Ineligible::Hidden || trackedEntry->candidateReason == Ineligible::Switch) {
						// The engine drew what the kept verdict calls hidden or unselected: shown since. A static's hidden
						// bit has no event of its own, and this is the engine's cull saying so.
						trackedEntry->candidateFrame = 0;
						pendingEvaluation.push_back(geometry);
					}
				}
				continue;
			}
			const std::uint32_t objectId = trackedEntry->objectId;
			// Resident: patched once, kept; a pass the engine registered for it anyway (its root was entry 0 of a list,
			// which Process2 culls) is withheld by static ownership and changes nothing here. A member bound again
			// (BindByMembership) is patched.
			if (IsResidentSlot(objectId) && !residentJoining.contains(geometry)) {
				++residentStats.registered;
				continue;
			}
			auto& object = tables.objects[objectId];
			const std::uint32_t geometrySlot = object.geometryIndex;
			// A shadow-only record: the main pass cannot take it, which the scene phase has already decided.
			if (object.flags & kObjectShadowOnly) {
				if (accumulated)
					++stats.ineligibleDrawn[static_cast<std::size_t>(trackedEntry->candidateReason)];
				continue;
			}

			auto& data = geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			auto* witnessProperty = property;
			const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
			const std::uint8_t fadeState = FadeStateOf(witnessProperty);
			RE::BSLightingShaderProperty* castCache = trackedEntry->castProperty == witnessProperty ? trackedEntry->castResult : nullptr;
			const bool alphaBelowOne = witnessMaterial && static_cast<const RE::BSLightingShaderMaterialBase*>(witnessMaterial)->materialAlpha < 1.0f;

			// The positive derivation, cached (Tracked::Derived): for an accumulated object whose
			// witnesses all match and whose slots still carry the keys they were derived for, the
			// classification and the whole derived section are skipped.
			auto& derived = trackedEntry->derived;
			bool derivedHit = accumulated && derived.valid && derived.generation == tablesGeneration &&
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
				reason = ClassifyStatic(*geometry, &descriptors, accumulated, &castCache);
			}
			timer.Add(BuildPart::ClassifyStatic);
			const bool primaryCandidate = primaryProbe && accumulated && PrimaryCull::Get().UnderListedCandidate(geometry);
			// Per frame whether or not the derivation was cached: hidden, part of an actor and fading are
			// states of this frame, and the scene phase's verdict for them is the last classification's
			// (for a static, the last event's). An object that has just been hidden must lose its bindings now,
			// or DCLF keeps drawing what the engine has stopped drawing.
			if (reason == Ineligible::None)
				reason = ClassifyFrame(*trackedEntry, accumulated);
			// The skin partitions are the walk's (Tables::skinPartitions): GetRenderPasses gives the main pass the same row, the
			// fade node's LOD level.
			// A pass in an alpha-test list is drawn with DoAlphaTest whatever it was registered with
			// (DrawnPassDescriptor, applied where the passes are taken).
			// The histogram is the scene phase's, taken over the whole tracked set; where this phase -
			// which has the accumulated pass, and so the decal group - reaches a different verdict, the
			// object is moved between the buckets so the report reads as it did before the split.
			if (primaryCandidate)
				PrimaryCull::Get().NoteDerived(*geometry, descriptors, *accumulated, reason, LodRowOf(*geometry, property));
			if (reason != trackedEntry->candidateReason) {
				--stats.ineligible[static_cast<std::size_t>(trackedEntry->candidateReason)];
				++stats.ineligible[static_cast<std::size_t>(reason)];
			}
			if (reason != Ineligible::None) {
				trackedEntry->accumulateReason = reason;
				trackedEntry->accumulateReasonFrame = frame;
				// Eligible for a record but not for bindings: it stays native this frame, which is what
				// its scene record already says (kObjectNoBindings, not native-visible).
				if (accumulated)
					++stats.ineligibleDrawn[static_cast<std::size_t>(reason)];
				derived.valid = false;
				continue;
			}
			timer.Add(BuildPart::ClassifyFrame);

			// Only computed when something will report them: this whole block exists to feed one log line.
			if (derivationStats && accumulated && descriptors.derivedPass != kNotDerived) {
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
				// What a pass built from the object alone (PrimaryCull::FreshSyntheticPass) would lose against the registered one:
				// the bits the registration still gives (kRegisteredPassBits), the sub-pass, the hint and a skin's LOD row.
				if (accumulated->pass) {
					++stats.syntheticChecked;
					AccumulatedPass fresh;
					if (!PrimaryCull::FreshSyntheticPass(*geometry, fresh)) {
						++stats.syntheticNotBuilt;
					} else {
						const std::uint32_t bits = (fresh.technique ^ accumulated->technique) & kRegisteredPassBits;
						const bool subPass = fresh.subPass != accumulated->subPass, hint = fresh.hint != accumulated->hint;
						const bool lodRow = data.skinInstance && fresh.lodRow != accumulated->lodRow;
						stats.syntheticBits |= bits;
						stats.syntheticBitsDiffer += bits ? 1u : 0u;
						stats.syntheticSubPass += subPass ? 1u : 0u;
						stats.syntheticHint += hint ? 1u : 0u;
						stats.syntheticLodRow += lodRow ? 1u : 0u;
						if ((bits || subPass || hint || lodRow) && stats.syntheticFirst.empty())
							stats.syntheticFirst = fmt::format("'{}' technique {:08X} (registered {:08X}), sub-pass {} ({}), hint {} ({}), LOD row {} ({})",
								geometry->name.c_str() ? geometry->name.c_str() : "?", fresh.technique, accumulated->technique, fresh.subPass,
								accumulated->subPass, fresh.hint, accumulated->hint, fresh.lodRow, accumulated->lodRow);
					}
				}
				// The local shadow lights' selection (LocalShadowLights) against the engine's: the LLF mask of the pass it registered.
				if (lightLimitFixLoaded && accumulated->pass) {
					if (!localShadowsSampled) {
						localShadowsSample = LocalShadowLights::Sample();
						localShadowsSampled = true;
					}
					const auto& bound = geometry->worldBound;
					const float center[3]{ bound.center.x, bound.center.y, bound.center.z };
					const std::uint32_t predicted = localShadowsSample.MaskOf(property, center, bound.radius);
					const std::uint32_t engine = LightLimitFix::GetShadowBitMask(accumulated->pass);
					++stats.shadowMaskChecked;
					stats.shadowMaskEngine += engine ? 1u : 0u;
					if (predicted != engine) {
						++stats.shadowMaskDiffers;
						stats.shadowMaskOver += (predicted & ~engine) ? 1u : 0u;
						stats.shadowMaskUnder += (engine & ~predicted) ? 1u : 0u;
						if (stats.shadowMaskFirst.empty())
						{
							std::string lights;
							for (const auto& light : localShadowsSample.lights) {
								const float dx = center[0] - light.center[0], dy = center[1] - light.center[1], dz = center[2] - light.center[2];
								lights += fmt::format(" [bit {:X} r {:.0f} at {:.0f}, {} volumes]", light.maskBit, light.radius, std::sqrt(dx * dx + dy * dy + dz * dz), light.volumes.size());
							}
							stats.shadowMaskFirst = fmt::format("'{}' predicted {:X} engine {:X} (bound {:.1f} {:.1f} {:.1f} r {:.1f}, pass shadow lights {}; lights:{})",
								geometry->name.c_str() ? geometry->name.c_str() : "?", predicted, engine, center[0], center[1], center[2], bound.radius,
								accumulated->pass->numShadowLights, lights);
						}
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
			} else if (derivationStats && descriptors.derivedPass == kNotDerived) {
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
				tables.MarkMaterialUsed(materialSlot, frame);
				// The per-frame template: a pipeline's template is always a property of a member of this frame (the first
				// to use the slot; KeepResidentsAlive takes one for the rest), so a persistent slot never points at a
				// property the game has since freed. The constants are evaluated from it at Prepass (RefreshFrameConstants).
				if (tables.pipelineLastUsed[pipelineSlot] != frame) {
					tables.MarkPipelineUsed(pipelineSlot, frame);
					tables.geometryTemplate[pipelineSlot] = property;
				}
				timer.Add(BuildPart::DedupHit);
			} else {
				const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
				const auto* alpha = data.alphaProperty.get();
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
				if (!newPipeline && tables.pipelineLastUsed[pipelineIt->second] != frame) {
					// The slot's first use this frame: this object's property is the template (see the cached path).
					tables.MarkPipelineUsed(pipelineIt->second, frame);
					tables.geometryTemplate[pipelineIt->second] = property;
				}
				if (newPipeline) {
					timer.Add(BuildPart::Dedup);
					const std::uint32_t slot = AllocatePipelineSlot();
					tables.pipelines[slot] = key;
					// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
					// (it supplies the scene light list the engine reads the sun from).
					GeometryConstants constants{};
					const auto* templatePass = TemplatePassOf(property);
					const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, kMainPassRenderFlags, constants);
					tables.geometryConstants[slot] = constants;
					tables.geometryConstantsValid[slot] = valid ? 1 : 0;
					tables.geometryTemplate[slot] = property;

					tables.pipelineTechnique[slot] = TechniqueRowFor(descriptors.pass);
					stats.shadowMaskPipelines += tables.TechniqueOf(slot).shadowMask ? 1 : 0;

					PipelinePermutation permutation;
					permutation.vertexShaderDescriptor = descriptors.rawVertex;
					permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
					permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
					// Extended Translucency's material model, as its SetupGeometry hook sets it (the key carries it):
					// disabled for opaque geometry, the default or the mesh's own for blended geometry.
					permutation.extraFeatureDescriptor = globals::features::extendedTranslucency.loaded ?
					                                         (ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ RasterTranslucency(key.rasterFlags))
					                                             << ExtendedTranslucency::ExtraFeatureDescriptorShift :
					                                         0u;
					tables.permutations[slot] = permutation;
					tables.pipelineConstantsVersion[slot] = tables.NextVersion();
					tables.pipelineBindingVersion[slot] = tables.NextVersion();
					pipelineIt = pipelineIndex.emplace(key, slot).first;
					timer.Add(BuildPart::PipelineEval);
				}
				pipelineSlot = pipelineIt->second;
				tables.MarkPipelineUsed(pipelineSlot, frame);

				// Material state as the engine's SetupMaterial produces it for this pass descriptor.
				const auto* material = property->material;
				auto materialIt = materialIndex.find(std::pair{ material, descriptors.pass });
				if (materialIt == materialIndex.end()) {
					timer.Add(BuildPart::Dedup);
					MaterialRecord record;
					if (!EvaluateMaterialForSlot(material, descriptors.pass, record)) {
						// No shader instance yet (nothing drawn so far): stay native this frame.
						derived.valid = false;
						continue;
					}
					const std::uint32_t slot = AllocateMaterialSlot();
					materialOwners.resize(tables.materials.size());
					materialOwners[slot].reset(const_cast<RE::BSShaderMaterial*>(material));
					tables.materials[slot] = record;
					tables.materialVersion[slot] = ++materialVersions;
					tables.MarkMaterialTextureChanged(slot, frame);
					tables.materialSlotKey[slot] = std::pair{ material, descriptors.pass };
					tables.ListMaterialSlot(slot, frame);
					materialIt = materialIndex.emplace(std::pair{ material, descriptors.pass }, slot).first;
					ListMaterialDependent(material, slot);
					timer.Add(BuildPart::MaterialEval);
				}
				materialSlot = materialIt->second;
				tables.MarkMaterialUsed(materialSlot, frame);
				timer.Add(BuildPart::DedupHit);

				staticFlags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
				              (ExternalEmittance::ShouldSuppress(interior, property, geometry) ? kObjectSuppressExternalEmittance : 0u) |
				              (descriptors.technique == kTechniqueTreeAnim ? kObjectTreeAnim : 0u) |
				              (alphaTest ? static_cast<std::uint32_t>(alpha->alphaThreshold) << kObjectAlphaThresholdShift : 0u) |
				              (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u) |
				              ((property->flags.underlying() & ((1ull << 14) | (1ull << 46))) ? kObjectLandscapeLights : 0u);
				if (accumulated) {
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
				} else {
					derived.valid = false;
				}
			}

			// The patch. Everything above decided what this object draws with; here it goes into the
			// record the scene phase appended, at the index that phase fixed.
			// A member's pass is patched once and kept; its extras rows (projected UV, land blend) follow the eye and a clock
			// through the watch (kWatchExtras, RefreshFrameConstants).
			const bool landBlendRecord = descriptors.technique == 8 || descriptors.technique == 19;
			const bool resident = accumulated && accumulated->resident && residentJoining.contains(geometry);
			AccumulatePatch patch;
			patch.object = objectId;
			patch.material = materialSlot;
			patch.pipeline = pipelineSlot;
			patch.resident = resident;
			patch.nativeVisible = accumulated && !resident;
			patch.nativeShadowMasked = accumulated && (descriptors.pass & 0x6000u) == 0x6000u;
			patch.derivedDescriptor = !accumulated;
			patch.projectedUV = descriptors.projectedUV;
			patch.landBlend = landBlendRecord;
			patch.flags = (object.flags & kSceneKeptFlags) | staticFlags | (accumulated ? kObjectNativeVisible : 0u) |
			              // The sun's bits are decided by the draw for every pass: a registered one's come from StaticShadowBits.
			              (accumulated && (accumulated->pass ? (descriptors.pass & 0x2000u) != 0 : accumulated->sunTest) ? kObjectSunTest : 0u) |
			              (resident && accumulated->fadeDistance != 0.0f ? kObjectFadeTest : 0u) |
			              (resident && accumulated->heightTest ? kObjectHeightTest : 0u) |
			              (patch.projectedUV ? kObjectProjectedUV : 0u) | (patch.landBlend ? kObjectLandBlend : 0u) | (resident ? kObjectMember : 0u);
			patch.fadeDistance = resident ? accumulated->fadeDistance : 0.0f;
			timer.Add(BuildPart::Record);
			float emissiveMult = 1.0f;
			patch.shading = MakeShading(*static_cast<RE::BSLightingShaderProperty*>(property), descriptors, kMainPassRenderFlags, emissiveMult, resident);
			patch.emissiveMult = emissiveMult;
			if (lightLimitFixLoaded) {
				auto& lightFix = globals::features::lightLimitFix;
				if (trackedEntry->roomMapGeneration != lightFix.GetRoomMapGeneration()) {
					trackedEntry->roomIndex = lightFix.GetRoomIndexForRoom(trackedEntry->roomNode);
					trackedEntry->roomMapGeneration = lightFix.GetRoomMapGeneration();
				}
				patch.lights.roomIndex = trackedEntry->roomIndex;
				if (accumulated)
					patch.lights.shadowBitMask = accumulated->pass ? LightLimitFix::GetShadowBitMask(accumulated->pass) : 0u;
			}
			if (patch.flags & kObjectTreeAnim)
				DeriveTreeAnim(*property, patch.tree);
			timer.Add(BuildPart::CapturePatch);
			if (descriptors.pass & kPassAdditionalAlphaMask) {
				if (fadingThisFrame++ == 0)
					++stats.fadingFrames;
				++stats.fadingDrawn;
			}
			if (descriptors.decalGroup && accumulated) {
				patch.decalKey = (std::uint64_t(descriptors.decalGroup) << 60) | (std::uint64_t(accumulated->technique & 0x3FFFFFFF) << 28) |
				                 (std::uint64_t(accumulated->subPass & 7) << 24) | (accumulated->chainIndex & 0xFFFFFF);
			}
			timer.Add(BuildPart::Record);
			ApplyAccumulatePatch(patch);
			timer.Add(BuildPart::ApplyPatch);
			if (resident) {
				MarkResidentSlot(objectId, { *accumulated, pipelineSlot, materialSlot });
				residentJoining.erase(geometry);
				++residentStats.joined;
			}
			timer.Add(BuildPart::Record);
		}

		TracyCZoneEnd(objectsZone);
		TracyCZoneN(residentsZone, "CS.DCLF.Accumulate.Residents", true);
		// Resident passes that were not patched (no record, a verdict of the frame, a material not ready, extras rows, or
		// the engine's own pass for the object): their entries leave residency.
		for (const auto* geometry : residentJoining) {
			++residentStats.failed;
			const auto* pass = FindAccumulatedPass(geometry);
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			const auto cause = pass && !pass->resident ? 0u :                                                             // the engine's pass took the object
			                   entry == tracked.end() || entry->second.objectStamp != objectStamp ? 1u :                  // no record
			                   entry->second.accumulateReasonFrame == frame ? 2u :                                          // a verdict of the frame
			                   3u;                                                                                          // material, or extras rows
			++residentStats.failedBy[cause];
			// A member whose binding could not be taken again is not bound any more.
			if (entry != tracked.end() && entry->second.slot != kNoObjectSlot && IsResidentSlot(entry->second.slot))
				DropResidentSlot(entry->second.slot, true);
		}
		residentJoining.clear();
		KeepResidentsAlive();
		++residentStats.frames;
		residentStats.resident += residents.size();
		if (ResidentParityEnabled() && ParityDue(frame) && !residents.empty())
			CheckResidentParity();
		{
			// A bound object can reference a slot that is not live only after a slot was freed (the sweep, a material
			// drop, a geometry slot that could not be resolved again).
			const bool slotParity = SwitchEnabled(Switch::PersistentParity);
			if (slotsFreedThisFrame || slotParity)
				CheckObjectSlots(frameResolveBuffers);
			slotsFreedThisFrame = false;
		}
		TracyCZoneEnd(residentsZone);
		TracyCZoneN(statsZone, "CS.DCLF.Accumulate.StatsAndDecals", true);
		const bool slotProbe = SwitchValue(Switch::SlotProbe) == "1";
		if (slotProbe)
			ProbeSlots(frameResolveBuffers);
		// The slot counts, for the reports: the frame's users every 16th frame, held in between; the live and referenced
		// counts are the slot tables'.
		const bool countSlots = frame % 16 == 0;
		if (countSlots) {
			stats.pipelines = 0;
			stats.pipelines = static_cast<std::uint32_t>(tables.usedPipelines.size());
		}
		// Decal draw order (DecalOrder.cpp).
		OrderDecals();
		if (countSlots) {
			stats.materials = 0;
			for (const auto used : tables.materialLastUsed)
				stats.materials += used == frame ? 1u : 0u;
		}
		stats.geometries = static_cast<std::uint32_t>(tables.geometrySlots.ReferencedCount());
		stats.geometriesAlive = static_cast<std::uint32_t>(tables.geometrySlots.AliveCount());
		stats.pipelinesAlive = static_cast<std::uint32_t>(tables.pipelineSlots.AliveCount());
		stats.materialsAlive = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		stats.materialCacheEntries = static_cast<std::uint32_t>(tables.materialSlots.AliveCount());
		TracyCZoneEnd(statsZone);
		ZoneNamedN(materialTailZone, "CS.DCLF.Accumulate.MaterialTail", true);
		ProcessMaterialWrites();
		RefreshTextureTransforms();
		ValidateMaterialSlice();
	}

	void SceneStore::BindByMembership()
	{
		ZoneScopedN("CS.DCLF.Accumulate.BindByMembership");
		// The frame globals a membership pass reads changed (the static sun bits, the fade distances): every resident is bound
		// again from this frame's.
		if (const std::uint32_t witness = PrimaryCull::MembershipWitness(); witness != membershipWitness) {
			bindQueue.insert(bindQueue.end(), residents.begin(), residents.end());
			EndAllResidency();
			membershipWitness = witness;
		}
		// Material records are the engine's SetupMaterial through its Lighting shader, which exists only once the engine has drawn
		// with it: until then the queue waits (the records at load are the whole scene).
		if (!ConstantEvaluator::Get().HasLightingShader())
			return;
		auto& primary = PrimaryCull::Get();
		for (const std::uint32_t slot : std::exchange(bindQueue, {})) {
			if (slot >= tables.objects.size() || (tables.objects[slot].flags & (kObjectFree | kObjectShadowOnly)))
				continue;
			auto* geometry = const_cast<RE::BSGeometry*>(tables.objectGeometry[slot]);
			const auto trackedIt = geometry ? tracked.find(geometry) : tracked.end();
			// A record written every frame (a mover, an actor's part, animated shading) is bound again every frame: its slots are
			// referenced again in the same batch, so nothing about it is retired.
			if (trackedIt == tracked.end() || trackedIt->second.slot != slot || trackedIt->second.objectStamp != objectStamp ||
				trackedIt->second.candidateReason != Ineligible::None)
				continue;
			// A member written again keeps its binding while what the binding reads is the same (the derivation cache's
			// witnesses): a skin, a face or a mover is written every frame for its transform, bones or stream alone.
			const auto& data = geometry->GetGeometryRuntimeData();
			if (IsResidentSlot(slot) && MemberBindingStands(slot, *geometry, trackedIt->second)) {
				++residentStats.membershipKept;
				continue;
			}
			AccumulatedPass pass;
			if (!primary.MembershipPass(geometry, pass))
				continue;
			// A fade node's objects carry its fade-out distance for BuildDraws' fade test, which measures from the node's centre
			// (Tables::lodFade, SetFadeRow). A tree's also take its height test.
			if (const auto* fadeNode = data.shaderProperty ? data.shaderProperty->fadeNode : nullptr) {
				pass.fadeDistance = PrimaryCull::MembershipFadeDistance(fadeNode);
				const auto* rtti = fadeNode->GetRTTI();
				pass.heightTest = rtti && rtti->name && std::strcmp(rtti->name, "BSTreeNode") == 0;
			}
			accumulatedPasses.insert_or_assign(geometry, pass);
			residentJoining.insert(geometry);
			++residentStats.membershipQueued;
		}
	}

	void SceneStore::SetFadedOut(std::int32_t a_object, bool a_fadedOut)
	{
		if (a_object < 0 || static_cast<std::size_t>(a_object) >= tables.objects.size() || static_cast<std::size_t>(a_object) >= tables.fadedOut.size())
			return;
		const auto o = static_cast<std::uint32_t>(a_object);
		if (tables.fadedOut[o] == (a_fadedOut ? 1 : 0))
			return;
		tables.fadedOut[o] = a_fadedOut ? 1 : 0;
		if (tables.objects[o].flags & kObjectFree)
			return;
		tables.objects[o].flags = (tables.objects[o].flags & ~kObjectFadedOut) | (a_fadedOut ? kObjectFadedOut : 0u);
		tables.NoteChange(o, kChangeBindings);
	}

	bool SceneStore::MemberBindingStands(std::uint32_t a_slot, const RE::BSGeometry& a_geometry, const Tracked& a_entry) const
	{
		const auto& derived = a_entry.derived;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
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
		object.flags = tables.sceneFlags[a_slot] | FadedOutBit(a_slot);
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		tables.draws[a_slot].pipelineIndex = 0;
		tables.shading[a_slot] = ObjectShading{};
		tables.emissiveMult[a_slot] = 1.0f;
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
			skyKeyMembers.clear();
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
			if (a_inputs.diffuse && (caster || a_inputs.skyTechnique))
				member(shadowTextureMembers, a_inputs.diffuse, a_slot, a_add);
			const auto raster = (a_inputs.flags & kObjectTwoSided) ? kRasterTwoSided : 0u;
			const auto vertex = VertexLayoutOf(a_inputs.vertexDesc);
			if (a_inputs.skyTechnique)
				member(skyKeyMembers, ShadowPipelineKey{ a_inputs.skyTechnique, raster, vertex }, a_slot, a_add);
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
		tables.skyKeysUsed.clear();
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
		emit(skyKeyMembers, tables.skyKeysUsed);
	}
}
