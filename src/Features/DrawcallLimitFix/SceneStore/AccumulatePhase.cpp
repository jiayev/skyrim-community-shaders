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
		// Published for the registration hook, which runs before this and so uses the previous frame's
		// set. These pointers are stable across frames, and an empty set on the first frame simply means
		// nothing is withheld yet.
		PassCapture::Get().SetMainBatchRenderers(
			std::make_shared<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>>(mainBatchRenderers));
		return true;
	}

	void SceneStore::CollectAccumulatedPasses()
	{
		accumulatedPasses.clear();
		// The latched accumulator, not `currentAccumulator`. BuildFrame now runs at EarlyPrepass, before
		// the depth pass, where `currentAccumulator` is still null because nothing is being rendered yet -
		// but the accumulator has held its passes since the cull job finished, which `Main::Draw` does
		// before the shadow maps. Measured: 612 passes at EarlyPrepass, at the end of the depth pass and
		// at Prepass alike, against 0 from `currentAccumulator` at the first two.
		auto* accumulator = latchedAccumulator ? latchedAccumulator : *globals::game::currentAccumulator.get();
		auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
		if (!batch)
			return;  // before the first latch, i.e. the first frame only

		// BSBatchRenderer::renderPass holds PassGroup structs inline (the engine indexes it as
		// data + (pass + group * 6) * 8), not the PassGroup pointers CommonLib declares; each of the five
		// entries heads a list chained through passGroupNext. renderPassMap maps each group's technique
		// (what SetupTechnique receives) to its index; the engine reads it as buckets of
		// { key, value, next } at +0x48, bucket count at +0x2C (engine notes: batch renderer).
		struct MapEntry
		{
			std::uint32_t key;
			std::uint32_t value;
			const MapEntry* next;
		};
		auto addBatch = [&](const RE::BSBatchRenderer* a_batch) {
			const auto* base = reinterpret_cast<const std::uint8_t*>(a_batch);
			const auto* buckets = *reinterpret_cast<const MapEntry* const*>(base + 0x48);
			const std::uint32_t bucketCount = *reinterpret_cast<const std::uint32_t*>(base + 0x2c);
			const auto* groups = reinterpret_cast<const RE::BSBatchRenderer::PassGroup*>(a_batch->renderPass.data());
			const std::uint32_t groupCount = a_batch->renderPass.size();
			for (std::uint32_t b = 0; buckets && groups && b < bucketCount; ++b) {
				const auto& entry = buckets[b];
				if (!entry.next || entry.value >= groupCount)
					continue;  // empty bucket
				const std::uint32_t technique = entry.key;
				const auto& group = groups[entry.value];
				for (std::uint32_t subPass = 0; subPass < 5; ++subPass) {
					std::uint32_t chainIndex = 0;
					for (auto* pass = group.passes[subPass]; pass; pass = pass->passGroupNext, ++chainIndex) {
						if (pass->geometry && pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting)
							AddAccumulatedPass(pass->geometry, AccumulatedPass{ pass, DrawnPassDescriptor(PassDescriptorOf(technique), subPass), subPass,
																   pass->passEnum, pass->accumulationHint, chainIndex, PassCapture::FadingAtRegistration(pass), LodRowOf(*pass) });
					}
				}
			}
		};
		addBatch(batch);
		// Geometry groups sort their passes in batch renderers of their own.
		for (auto* group : batch->geometryGroups) {
			if (group && group->batchRenderer)
				addBatch(group->batchRenderer);
		}
	}

	void SceneStore::CompareCapturedPasses(bool a_compare)
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained, whether or not anything is compared: the capture buffer is fixed-capacity and a
		// frame that does not drain it overflows.
		const auto entries = capture.Drain();
		auto& captureStats = capture.MutableStats();
		captureStats.compared = captureStats.missing = captureStats.extra = captureStats.techniqueDiffers = captureStats.subPassDiffers = 0;
		if (!a_compare) {
			// The normal path needs only the first registration per geometry. Insert directly into the
			// retained table instead of allocating a second map and hashing every geometry twice.
			bool foundMain = false;
			for (const auto& entry : entries) {
				if (!mainBatchRenderers.contains(entry.batch))
					continue;
				if (!foundMain) {
					accumulatedPasses.clear();
					foundMain = true;
				}
				// Preserve the capture's first-registration rule, including duplicate hint-10 passes.
				accumulatedPasses.try_emplace(entry.geometry,
					AccumulatedPass{ entry.pass, DrawnPassDescriptor(PassDescriptorOf(entry.technique), entry.subPass), entry.subPass, entry.passEnum,
						entry.pass ? static_cast<std::uint32_t>(entry.pass->accumulationHint) : 0u,
						0xFFFFFFu - static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(&entry - entries.data()), 0xFFFFFFu)),
						entry.fading, entry.pass ? LodRowOf(*entry.pass) : 3u });
			}
			return;  // An empty capture preserves the caller's accumulator fallback.
		}

		// Only the main camera's registrations; the shadow cameras register into their own renderers.
		ankerl::unordered_dense::map<const RE::BSGeometry*, const PassCapture::Entry*> captured;
		for (const auto& entry : entries) {
			if (mainBatchRenderers.contains(entry.batch))
				captured.try_emplace(entry.geometry, &entry);
		}

		for (const auto& [geometry, accumulated] : a_compare ? accumulatedPasses : decltype(accumulatedPasses){}) {
			++captureStats.compared;
			const auto it = captured.find(geometry);
			if (it == captured.end()) {
				++captureStats.missing;
				continue;
			}
			if (DrawnPassDescriptor(PassDescriptorOf(it->second->technique), it->second->subPass) != accumulated.technique)
				++captureStats.techniqueDiffers;
			if (it->second->subPass != accumulated.subPass)
				++captureStats.subPassDiffers;
		}
		if (a_compare) {
			for (const auto& [geometry, entry] : captured) {
				if (!accumulatedPasses.contains(geometry))
					++captureStats.extra;
			}
		}

		// The tables are built from the capture rather than the accumulator walk once the two agree. The
		// walk stops working the moment a pass is withheld from the batch renderer, which is the whole
		// point of static ownership; the capture sees the registration regardless of what happens to it
		// afterwards. Falling back when the capture is empty keeps the first frame and any unexpected
		// path working.
		if (captured.empty())
			return;
		accumulatedPasses.clear();
		for (const auto& [geometry, entry] : captured) {
			// The hint is read off the pass now, on the render thread, while the pass is alive for the
			// frame. RegisterPass PREPENDS to its list (Ghidra: passGroupNext = head; head = pass), so the
			// engine draws a bucket in reverse registration order; the chain position is reversed here so
			// that an ascending sort on it is the draw order, as it is for the accumulator walk.
			AddAccumulatedPass(geometry,
				AccumulatedPass{ entry->pass, DrawnPassDescriptor(PassDescriptorOf(entry->technique), entry->subPass), entry->subPass, entry->passEnum,
					entry->pass ? static_cast<std::uint32_t>(entry->pass->accumulationHint) : 0u,
					0xFFFFFFu - static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(entry - entries.data()), 0xFFFFFFu)),
					entry->fading, entry->pass ? LodRowOf(*entry->pass) : 3u });
		}
	}

	void SceneStore::AddAccumulatedPass(const RE::BSGeometry* a_geometry, const AccumulatedPass& a_pass)
	{
		// One pass per object, the first registered - except that a hint-10 pass (a LOD cross-fade's copy of the
		// old level, the native loop's) never stands for an object that also has a pass of its own.
		const auto [it, inserted] = accumulatedPasses.try_emplace(a_geometry, a_pass);
		if (!inserted && it->second.hint == 10 && a_pass.hint != 10)
			it->second = a_pass;
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
		if (!a_patch.resident) {
			accumulatePatched.push_back(objectId);
			if (patchedFrame.size() < tables.objects.size())
				patchedFrame.resize(tables.objects.size(), 0);
			patchedFrame[objectId] = frame;
		}
		object.materialIndex = a_patch.material;
		object.pipelineIndex = a_patch.pipeline;
		object.flags = a_patch.flags;
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
			decalOrder.push_back({ a_patch.decalKey, objectId });
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
			CompareCapturedPasses(false);
			return;
		}
		PartTimer timer(stats.partMs, &stats.accumulatePartMs);
		std::uint32_t fadingThisFrame = 0;
		TracyCZoneN(captureZone, "CS.DCLF.Accumulate.Capture", true);
		// The pass table is filled from the capture, which is the source that keeps working once passes
		// are withheld from the batch renderer. The accumulator walk is the cross-check.
		// CS_DCLF_PASS_PARITY=1: the accumulator walk every frame, and the capture compared with it.
		const bool passParity = SwitchEnabled(Switch::PassParity);
		const bool haveAccumulator = RefreshMainBatchRenderers();
		if (passParity)
			CollectAccumulatedPasses();
		else
			accumulatedPasses.clear();
		CompareCapturedPasses(passParity);
		// The capture had nothing and the walk was skipped: take the walk after all, so the first frame
		// after a latch is not empty.
		if (accumulatedPasses.empty() && haveAccumulator && !passParity)
			CollectAccumulatedPasses();
		// The objects the primary's cull left out this frame (PrimaryCull): their main passes, built without a
		// registration, stand where the engine's would have.
		for (const auto& [geometry, pass] : PrimaryCull::Get().BuildSyntheticPasses())
			AddAccumulatedPass(geometry, pass);
		// Resident records (PrimaryCull): a frame the primary's cut did not keep them, every one ends; the entries joining
		// this frame have their passes patched once below.
		const bool residentsLive = PrimaryCull::Get().TakeResidentsLive();
		if (!residentsLive && (!residents.empty() || PrimaryCull::Get().HasResidents()))
			PrimaryCull::Get().EndAllResidents();
		residentJoining.clear();
		if (residentsLive)
			for (const auto& [geometry, pass] : PrimaryCull::Get().ResidentPasses()) {
				residentJoining.insert(geometry);
				AddAccumulatedPass(geometry, pass);
			}
		// The engine's passes for what PrimaryCull draws synthetically take the same sun bits, so both sources of an
		// object's pass need one pipeline (and the probe still compares against the engine's own bits).
		if (!PrimaryCull::Probe())
			for (auto& [geometry, pass] : accumulatedPasses)
				if (pass.pass)
					PrimaryCull::Get().UnifySunBits(geometry, pass);
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
			// which Process2 culls) is withheld by static ownership and changes nothing here.
			if (IsResidentSlot(objectId)) {
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
				reason = ClassifyStatic(*geometry, &descriptors, accumulated, derivationStats || primaryProbe, &castCache);
			}
			timer.Add(BuildPart::ClassifyStatic);
			const bool primaryCandidate = primaryProbe && accumulated && PrimaryCull::Get().UnderListedCandidate(geometry);
			// Per frame whether or not the derivation was cached: hidden, part of an actor and fading are
			// states of this frame, and the scene phase's verdict for them is the last classification's
			// (for a static, the last event's). An object that has just been hidden must lose its bindings now,
			// or DCLF keeps drawing what the engine has stopped drawing.
			if (reason == Ineligible::None)
				reason = ClassifyFrame(*trackedEntry, accumulated);
			// The skin partitions the main camera draws, from its registered pass's LODMode rather than the fade
			// node the scene phase read for the shadow views. None is not drawn at all.
			if (reason == Ineligible::None && accumulated && data.skinInstance && data.skinInstance->skinPartition) {
				const std::uint32_t mask = SkinPartitionMask(*data.skinInstance, accumulated->lodRow);
				if (!mask)
					reason = Ineligible::Hidden;
				else if (objectId < tables.skinPartitions.size()) {
					// The walk writes the scene's mask again (every skin is written every walk); a resident's is the kept skin's,
					// from the same LOD row.
					const auto partitionMask = static_cast<std::uint8_t>(data.skinInstance->skinPartition->numPartitions > 1 ? mask : 0);
					if (tables.skinPartitions[objectId] != partitionMask) {
						tables.skinPartitions[objectId] = partitionMask;
						tables.NoteChange(objectId, kChangeSkin);
					}
				}
			}
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
				const std::uint32_t differing = descriptors.derivedPass ^ descriptors.pass;
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
				// The per-frame template: a pipeline's template is always a property of an object of this
				// frame (the first to use the slot, upgraded to a native-visible one by the election), so a
				// persistent slot never points at a property the game has since freed. The constants are
				// evaluated from it at Prepass (RefreshFrameConstants).
				if (tables.pipelineLastUsed[pipelineSlot] != frame) {
					tables.MarkPipelineUsed(pipelineSlot, frame);
					tables.geometryTemplate[pipelineSlot] = property;
					tables.geometryTemplateNative[pipelineSlot] = accumulated ? 1 : 0;
				} else if (accumulated && !tables.geometryTemplateNative[pipelineSlot]) {
					tables.geometryTemplate[pipelineSlot] = property;
					tables.geometryTemplateNative[pipelineSlot] = 1;
					++stats.templateUpgrades;
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
					// The slot's first use this frame: this object's property is the template until the
					// election finds a native-visible one (see the cached path).
					tables.MarkPipelineUsed(pipelineIt->second, frame);
					tables.geometryTemplate[pipelineIt->second] = property;
					tables.geometryTemplateNative[pipelineIt->second] = accumulated ? 1 : 0;
				}
				if (newPipeline) {
					timer.Add(BuildPart::Dedup);
					const std::uint32_t slot = AllocatePipelineSlot();
					tables.pipelines[slot] = key;
					// Per-frame PerGeometry values for this pass descriptor, from any object's lighting pass
					// (it supplies the scene light list the engine reads the sun from).
					GeometryConstants constants{};
					const auto* templatePass = FindLightingPass(property);
					const bool valid = templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, kMainPassRenderFlags, constants);
					tables.geometryConstants[slot] = constants;
					tables.geometryConstantsValid[slot] = valid ? 1 : 0;
					tables.geometryTemplate[slot] = property;
					tables.geometryTemplateNative[slot] = accumulated ? 1 : 0;

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
				} else if (accumulated && !tables.geometryTemplateNative[pipelineIt->second]) {
					// The election. This pipeline's per-frame lighting template belongs to an object the
					// engine culled, and here is one it kept: take the template over. An object the engine
					// kept is by definition in the lighting situation being drawn, so it is the correct
					// template, and this is the ordering guarantee stated as a rule about the objects rather
					// than as a rule about the order they are visited in.
					const auto slot = pipelineIt->second;
					GeometryConstants constants;
					const auto* templatePass = FindLightingPass(property);
					if (templatePass && evaluator.EvaluateGeometry(*templatePass, descriptors.pass, kMainPassRenderFlags, constants)) {
						tables.geometryConstants[slot] = constants;
						tables.geometryConstantsValid[slot] = 1;
						tables.pipelineConstantsVersion[slot] = tables.NextVersion();
					}
					tables.geometryTemplate[slot] = property;
					tables.geometryTemplateNative[slot] = 1;
					++stats.templateUpgrades;
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
				              (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u);
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
			// A resident pass is patched once and kept, unless the record needs this frame's extras rows.
			const bool landBlendRecord = descriptors.technique == 8 || descriptors.technique == 19;
			const bool resident = accumulated && accumulated->resident && !descriptors.projectedUV && !landBlendRecord && residentJoining.contains(geometry);
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
			              (accumulated && accumulated->sunTest ? kObjectSunTest : 0u) | (resident && accumulated->fadeDistance != 0.0f ? kObjectFadeTest : 0u) |
			              (resident && accumulated->heightTest ? kObjectHeightTest : 0u) |
			              (patch.projectedUV ? kObjectProjectedUV : 0u) | (patch.landBlend ? kObjectLandBlend : 0u);
			patch.fadeDistance = resident ? accumulated->fadeDistance : 0.0f;
			timer.Add(BuildPart::Record);
			float emissiveMult = 1.0f;
			patch.shading = MakeShading(*static_cast<RE::BSLightingShaderProperty*>(property), descriptors, kMainPassRenderFlags, emissiveMult);
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
			residentEvictions.push_back(geometry);
			++residentStats.failed;
			const auto* pass = FindAccumulatedPass(geometry);
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			const auto cause = pass && !pass->resident ? 0u :                                                             // the engine's pass took the object
			                   entry == tracked.end() || entry->second.objectStamp != objectStamp ? 1u :                  // no record
			                   entry->second.accumulateReasonFrame == frame ? 2u :                                          // a verdict of the frame
			                   3u;                                                                                          // material, or extras rows
			++residentStats.failedBy[cause];
		}
		residentJoining.clear();
		LapseAccumulated();
		KeepResidentsAlive();
		++residentStats.frames;
		residentStats.resident += residents.size();
		if (ResidentParityEnabled() && frame % 60 == 0 && !residents.empty())
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
		// Decal draw order: sort the frame's decals by the engine's key and hand each its slot in its
		// group. Tens to a few hundred entries; the sort is the whole cost.
		// Last frame's ordinals are reset, not the whole column.
		if (tables.decalOrdinal.size() != tables.objects.size())
			tables.decalOrdinal.resize(tables.objects.size(), ~0u);
		for (const std::uint32_t o : decalOrdered)
			if (o < tables.decalOrdinal.size())
				tables.decalOrdinal[o] = ~0u;
		decalOrdered.clear();
		tables.decalCount = {};
		if (!decalOrder.empty()) {
			std::sort(decalOrder.begin(), decalOrder.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
			for (const auto& entry : decalOrder) {
				const std::uint32_t group = static_cast<std::uint32_t>(entry.key >> 60) - 1;
				tables.decalOrdinal[entry.object] = tables.decalCount[group & 1]++;
				decalOrdered.push_back(entry.object);
			}
		}
		// The 4c gate, checked over the finished tables rather than asserted from the election: no
		// native-visible object may draw on a pipeline whose lighting template came from a culled one.
		stats.templateDefects = 0;
		stats.pipelinesCulledOnly = 0;
		for (std::size_t p = 0; p < tables.geometryTemplateNative.size(); ++p)
			stats.pipelinesCulledOnly += (tables.PipelineUsed(p, frame) && !tables.geometryTemplateNative[p]) ? 1u : 0u;
		if (stats.pipelinesCulledOnly) {
			for (const auto& object : tables.objects) {
				if (!(object.flags & kObjectNativeVisible) || (object.flags & kObjectNoBindings))
					continue;
				if (object.pipelineIndex < tables.geometryTemplateNative.size() && !tables.geometryTemplateNative[object.pipelineIndex])
					++stats.templateDefects;
			}
		}
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

	void SceneStore::LapseAccumulated()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Lapse");
		// A patch this phase did not renew: the engine no longer registers the object (culled, hidden, faded out), or it
		// has no bindings this frame. Its record goes back to the scene half; a resident's is kept by its residency.
		std::uint32_t lapsed = 0;
		for (const std::uint32_t slot : lastPatched) {
			if (slot < patchedFrame.size() && patchedFrame[slot] == frame)
				continue;
			if (IsResidentSlot(slot))
				continue;
			ResetAccumulatedHalf(slot);
			++lapsed;
		}
		delta.restored += lapsed;
		lastPatched.swap(accumulatePatched);
		accumulatePatched.clear();
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
