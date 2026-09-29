#include "Internal.h"

namespace DCLF
{
	void SceneStore::BuildFrame(Phase a_phase)
	{
		if (a_phase == Phase::Scene)
			BuildScenePhase();
		else
			BuildAccumulatePhase();
	}

	/**
	 * @brief The scene half of the frame, from Main::Draw's early hook.
	 *
	 * Everything that does not depend on the main camera's accumulator: the tracked walk, eligibility, the
	 * geometry slots and their buffer resolve, the transforms and bounds, the bone palettes, and one object
	 * record per eligible object. It runs before the shadow maps are drawn - the scene graph is final from
	 * Main::Draw on, and a shadow epoch reads these records. The early hook is ahead of the main camera's
	 * cull; what moves before BeforeShadowMaps is listed at DrawcallLimitFix::BeginSceneFrame.
	 *
	 * The records leave the accumulator's half unset: no pipeline, no material, kObjectNoBindings, and
	 * kObjectNativeVisible clear. BuildAccumulatePhase patches them in place by object index, which is
	 * fixed for the frame from here on.
	 */
	void SceneStore::BuildScenePhase()
	{
		// Normally joined at BeforeShadowMaps already; a frame that did not get there joins before the walk.
		JoinPlacements();
		{
			DCLF_SCENE_PART(Prologue, "CS.DCLF.Scene.Prologue");
			++frame;
			tables.usedMaterials.clear();
			tables.usedPipelines.clear();
			std::fill(tables.usedMaterialBits.begin(), tables.usedMaterialBits.end(), 0);
			std::fill(tables.usedPipelineBits.begin(), tables.usedPipelineBits.end(), 0);
			tables.usedMaterialsFrame = tables.usedPipelinesFrame = frame;
			// The change log keeps its tail (Tables::changeLog): a reader that has not read past the trimmed half reads every
			// slot again.
			tables.changeLog.Trim(1u << 17);
			tables.geometryLog.Trim(1u << 16);
			CheckChangeLog();
		}
		// Nothing is drawn while a load screen is up, and nothing here may touch the tracked geometry
		// either. The load frees the renderer data and the vertex and index buffers of the cell being
		// unloaded, while the NiPointers in `tracked` keep only the NiAVObjects alive; classifying those
		// entries reads freed BSGraphics::TriShape data and hands the render graph device addresses that no
		// longer belong to anything, which the GPU answers with VK_ERROR_DEVICE_LOST a few frames later.
		//
		// Before ProcessEvents stopped walking the scene graph across loads, this did not arise: the
		// category-node refresh pruned those entries as the cell's nodes vanished, so they never reached
		// this loop. Leaving the tables empty is both the safe and the obviously correct thing to draw
		// during a load screen.
		sceneBuilt = false;
		if (IsLoadingScreenUp()) {
			ResetSlotTables();
			InvalidateObjectIndices();
			accumulatedPasses.clear();
			stats.objects = 0;
			stats.nativeVisible = stats.nativeShadowMasked = stats.derivedDescriptors = 0;
			stats.geometries = 0;
			stats.pipelines = 0;
			stats.materials = 0;
			return;
		}
		PartTimer timer(stats.partMs);
		{
			DCLF_SCENE_PART(Prologue, "CS.DCLF.Scene.Prologue");
			RefreshLodFadeSettings();
			auto& gpu = GpuResources::Get();
			gpu.BeginFrame();
			const bool resolveBuffers = gpu.Enabled();
			frameResolveBuffers = resolveBuffers;
			if (resolveBuffers != graphWasActive) {
				logger::info("[DCLF] tables frame {}: the render graph is {} (slots alive {} geometries / {} pipelines / {} materials)", frame,
					resolveBuffers ? "active from this frame" : "inactive from this frame", stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive);
				graphWasActive = resolveBuffers;
			}
			// Frame-globals that were being read per object. ShouldSuppress in particular is three terms, and
			// only one of them is per object - the interior test is the same answer for every object in the
			// frame. The depth-bias mode of each decal group is frame state (a console toggle and whether sun
			// shadows are off), read once here rather than per decal.
			frameInterior = Util::IsInterior();
			frameDecalBias = { 0u, DecalDepthBiasMode(1), DecalDepthBiasMode(2) };
			auto& evaluator = ConstantEvaluator::Get();
			ConstantEvaluator::ResetFrameAudits();
			if (!evaluator.HasLightingShader())
				FindLightingShader();
			geometryIndex.reserve(tracked.size());
		}
		timer.Add(BuildPart::Walk);
		{
			// Consume object-reference changes from the completed frame. Materials retire
			// on their last-reference event; geometry and pipeline slots retain their grace period.
			DCLF_SCENE_PART(SweepSlots, "CS.DCLF.Scene.SweepSlots");
			SweepSlots();
		}
		{
			DCLF_SCENE_PART(CullHiddenBits, "CS.DCLF.Scene.CullHiddenBits");
			CaptureCullHiddenBits();
		}

		// Only what can have changed (DeltaWalk). CS_DCLF_WALK_PARITY=1: every 60 frames a dense rebuild, classifying
		// every object from scratch, is compared with the slot tables object by object.
		DeltaWalk();
		// The kept records' placements and palettes, on the worker until BeforeShadowMaps (inline on a parity frame).
		KickPlacements();
		const bool walkParityOn = SwitchEnabled(Switch::WalkParity);
		if (walkParityOn && ParityDue(frame)) {
			DCLF_SCENE_PART(WalkParity, "CS.DCLF.Scene.WalkParity");
			CheckWalkParity();
		}
		stats.objects = tables.liveObjects;
		sceneBuilt = true;
	}

	bool SceneStore::ResolveFace(RE::BSGeometry& a_geometry, FaceSnapshots::ShapeView& a_face, std::uint32_t& a_region)
	{
		a_face = {};
		a_region = kNoFaceRegion;
		if (auto* head = a_geometry.parent ? netimmerse_cast<RE::BSFaceGenNiNode*>(a_geometry.parent) : nullptr)
			a_face = FaceSnapshots::Get().Shape(static_cast<RE::BSDynamicTriShape&>(a_geometry), *head);
		if (a_face.positions)
			a_region = FaceRegionOf(&a_geometry, a_face.vertexCount);
		return a_region != kNoFaceRegion;
	}

	void SceneStore::PushFaceStream(RE::BSGeometry& a_geometry, std::uint32_t a_slot, const FaceSnapshots::ShapeView& a_face, std::uint32_t a_region)
	{
		std::shared_ptr<const FaceSnapshots::HeadView> headView;
		if (auto* head = a_geometry.parent ? netimmerse_cast<RE::BSFaceGenNiNode*>(a_geometry.parent) : nullptr) {
			auto [headEntry, inserted] = capturedFaceHeads.try_emplace(head);
			if (inserted) {
				auto snapshot = FaceSnapshots::Get().HeadSnapshot(*head);
				if (snapshot.recordId && snapshot.generation && snapshot.owner)
					headEntry->second = std::make_shared<const FaceSnapshots::HeadView>(std::move(snapshot));
			}
			headView = headEntry->second;
		}
		if (headView && (headView->generation != a_face.generation || headView->owner != a_face.owner))
			headView.reset();
		tables.faceStream[a_slot] = static_cast<std::uint32_t>(tables.faceStreams.size());
		tables.faceStreams.push_back({ a_slot, a_region, a_face.vertexCount, a_face.generation, a_face.positions, a_face.owner, std::move(headView) });
	}

	bool SceneStore::KeepFaceStream(RE::BSGeometry& a_geometry, Tracked& a_tracked)
	{
		// What WriteObject takes for a face shape beyond an actor's record: this walk's snapshot and region, and the
		// slot's entry in this walk's faceStreams. Without a snapshot the record goes, which the full write does.
		if (!FaceSnapshots::Enabled())
			return false;
		FaceSnapshots::ShapeView face{};
		std::uint32_t region = kNoFaceRegion;
		if (!ResolveFace(a_geometry, face, region))
			return false;
		const std::uint32_t before = tables.faceStream[a_tracked.slot];
		PushFaceStream(a_geometry, a_tracked.slot, face, region);
		if (tables.faceStream[a_tracked.slot] != before)
			tables.NoteChange(a_tracked.slot, kChangeGeometry);
		return true;
	}

	std::uint32_t SceneStore::FaceRegionOf(const RE::BSGeometry* a_geometry, std::uint32_t a_vertexCount)
	{
		auto& region = faceRegions[a_geometry];
		if (region.count != a_vertexCount) {
			if (region.count)
				faceRegionFree.push_back({ region.first, region.count });
			region = {};
			// First fit among the freed ranges, else the top of the buffer.
			for (auto it = faceRegionFree.begin(); it != faceRegionFree.end(); ++it) {
				if (it->second < a_vertexCount)
					continue;
				region.first = it->first;
				region.count = a_vertexCount;
				it->first += a_vertexCount;
				it->second -= a_vertexCount;
				if (it->second == 0)
					faceRegionFree.erase(it);
				break;
			}
			if (!region.count) {
				region.first = faceRegionTop;
				region.count = a_vertexCount;
				faceRegionTop += a_vertexCount;
			}
		}
		region.seenWalk = faceWalk;
		return region.first;
	}

	void SceneStore::EndFaceWalk()
	{
		for (auto it = faceRegions.begin(); it != faceRegions.end();) {
			if (it->second.seenWalk == faceWalk) {
				++it;
				continue;
			}
			faceRegionFree.push_back({ it->second.first, it->second.count });
			it = faceRegions.erase(it);
		}
		// Sorted and coalesced, so a run of freed shapes is one range again.
		std::sort(faceRegionFree.begin(), faceRegionFree.end());
		std::size_t out = 0;
		for (std::size_t i = 0; i < faceRegionFree.size(); ++i) {
			if (out && faceRegionFree[out - 1].first + faceRegionFree[out - 1].second == faceRegionFree[i].first)
				faceRegionFree[out - 1].second += faceRegionFree[i].second;
			else
				faceRegionFree[out++] = faceRegionFree[i];
		}
		faceRegionFree.resize(out);
		if (!faceRegionFree.empty() && faceRegionFree.back().first + faceRegionFree.back().second == faceRegionTop) {
			faceRegionTop = faceRegionFree.back().first;
			faceRegionFree.pop_back();
		}
		if (FaceSnapshots::Enabled())
			FaceSnapshots::Get().EndWalk();
	}

	void SceneStore::BeginWalk(bool a_keepIndices)
	{
		capturedFaceHeads.clear();
		const bool keepObjects = !denseWalk;
		tables.ClearFrame(keepObjects);
		if (!a_keepIndices)
			InvalidateObjectIndices();
		++walkSerial;
		++faceWalk;
		if (FaceSnapshots::Enabled())
			FaceSnapshots::Get().BeginWalk();
		stats.ineligible.fill(0);
		stats.ineligibleDrawn.fill(0);
		stats.techniqueRejects.fill(0);
		stats.propertyRejects.clear();
		stats.rejectedBlended = stats.rejectedOpaque = stats.rejectedOpaqueAlphaTest = 0;
		stats.shadowMaskPipelines = 0;
		stats.derivationChecked = stats.derivationDiffers = stats.derivationBits = stats.derivationNative = 0;
		stats.derivationFadeBits = stats.lodFadeChecked = stats.lodMetricDiffers = stats.lodFadeDiffers = 0;
		stats.lodFadeFirst.clear();
		lodFadeSampled = false;
		stats.shadowMaskChecked = stats.shadowMaskEngine = stats.shadowMaskDiffers = stats.shadowMaskOver = stats.shadowMaskUnder = 0;
		stats.shadowMaskFirst.clear();
		stats.syntheticChecked = stats.syntheticNotBuilt = stats.syntheticBitsDiffer = stats.syntheticBits = stats.syntheticSubPass = 0;
		stats.syntheticHint = stats.syntheticLodRow = 0;
		stats.syntheticFirst.clear();
		localShadowsSampled = false;
		stats.derivationRuntimeDiffers = stats.derivationRuntimeBits = 0;
		stats.derivationBitCounts.fill(0);
		stats.materialsEvaluated = 0;
		stats.materialDiffMask = 0;
		stats.materialsValidated = stats.materialCacheStale = 0;
		stats.templateUpgrades = stats.templateDefects = stats.pipelinesCulledOnly = 0;
		stats.nativeVisible = stats.nativeShadowMasked = stats.derivedDescriptors = 0;
		stats.classifyHits = stats.classifyChecked = stats.classifyDiffers = stats.castResolved = 0;
		stats.derivedHits = stats.derivedChecked = stats.derivedDiffers = 0;
		stats.accumulatedWithoutRecord = 0;
		stats.decals = {};
		stats.skinned = stats.boneRows = 0;
		stats.projectedUV = stats.landBlend = 0;
		stats.shadowCasters = 0;
		stats.shadowRejects = {};
		decalOrder.clear();

		skinnedObjects.clear();

		// Every per-object container, not only three of them. The three that were left out reallocated
		// their way back up every frame, and objectIndex - cleared just above - rehashed its way up to
		// ~2900 entries in the Whiterun exterior, which the profile billed to `record`.
		if (!keepObjects) {
			tables.objects.reserve(tracked.size());
			tables.objectGeometry.reserve(tracked.size());
			tables.draws.reserve(tracked.size());
			tables.shading.reserve(tracked.size());
			tables.emissiveMult.reserve(tracked.size());
			tables.lights.reserve(tracked.size());
			tables.treeAnim.reserve(tracked.size());
			tables.skinPartitions.reserve(tracked.size());
		}

	}

	void SceneStore::DenseWalk()
	{
		BeginWalk();
		PartTimer timer(stats.partMs);
		timer.Add(BuildPart::PassLookup);
		for (auto& [geometry, trackedEntry, unused] : order) {
			// Per TRACKED object: for a rejected one this is its `continue` and the iteration itself.
			timer.Add(BuildPart::LoopTail);
			Ineligible bucket = Ineligible::Count;
			WriteObject(geometry, *trackedEntry, timer, bucket);
		}

		SweepObjectSlots();

		EndFaceWalk();
		tables.InvalidateChangeLog();
	}

	bool SceneStore::WriteObject(RE::BSGeometry* geometry, Tracked& a_tracked, PartTimer& timer, Ineligible& a_bucket)
	{
		// CS_DCLF_CLASSIFY_CACHE=probe: the cached verdict is used and *also* recomputed, and the two compared.
		const bool classifyProbe = SwitchValue(Switch::ClassifyCache) == "probe";
		// CS_DCLF_COVERAGE_PROBE=1: which shader the uncovered objects actually use.
		const bool coverageProbe = SwitchEnabled(Switch::CoverageProbe);
		auto* trackedEntry = &a_tracked;
		const auto& entry = a_tracked;
		// A member's record written again (an event, or its per-frame inputs): it stays a member with its binding if the
		// write keeps it eligible in the same slot, and BindByMembership binds it again only if what the binding reads changed.
		const bool wasMember = !denseWalk && a_tracked.slot != kNoObjectSlot && IsResidentSlot(a_tracked.slot);
		if (wasMember)
			++residentStats.rewritten;

		// Eligibility, and nothing else. This phase needs no lighting descriptors - the pipeline and
		// material belong to the accumulator's half - so the verdict is taken from Tracked's own
		// cache (refreshed by the delta walk's events) rather than recomputed per object per frame: that cache is what made the cull-only path cheap, and here it covers every object.
		//
		// A verdict that is stale in the "eligible" direction costs nothing: the accumulate phase
		// classifies again before it hands an object any bindings. A verdict stale the other way
		// would leave an accumulated object without a record, so that phase clears the cache for it
		// and counts it (stats.accumulatedWithoutRecord, the gate: 0 in steady state).
		// An NPC face shape (FaceSnapshots), resolved once: a tracked geometry's type and parent do not change.
		if (!trackedEntry->faceShapeResolved) {
			trackedEntry->faceShapeResolved = true;
			trackedEntry->faceShape = geometry->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape && geometry->parent &&
			                          netimmerse_cast<RE::BSFaceGenNiNode*>(geometry->parent);
		}
		const bool faceShape = trackedEntry->faceShape && FaceSnapshots::Enabled();
		// Owned by an actor, as Skin::GetWetness decides it; resolved once, like the face shape.
		if (!trackedEntry->actorOwnedResolved) {
			trackedEntry->actorOwnedResolved = true;
			const auto* owner = geometry->GetUserData();
			trackedEntry->actorOwned = owner && owner->GetFormType() == RE::FormType::ActorCharacter;
		}
		Ineligible reason;
		bool shadowOnly = false;  // not the main pass's, but a caster the shadow epochs draw (kObjectShadowOnly)
		// A face shape is classified every frame: its record also depends on its head's snapshot, and the
		// verdicts it can take (hidden, fading, a decal group) change as the actor does.
		// The dense rebuild (walk parity's reference) classifies from scratch and leaves the caches alone. With
		// A classification stands until an event takes it again, except an actor's and any other per-frame entry
		// written in full: its inputs are re-read every frame (Tracked::classifyInputs), as its record is.
		const bool rereads = trackedEntry->perFrame && (!trackedEntry->lightTraits || (trackedEntry->lightTraits & kTraitActor));
		bool cached = !denseWalk && !faceShape && trackedEntry->candidateFrame != 0;
		if (cached && rereads && ClassifyInputsOf(*geometry) != trackedEntry->classifyInputs) {
			cached = false;
			++delta.reread;
		}
		if (cached) {
			reason = trackedEntry->candidateReason;
			// Under a switch node the verdict follows the switch's selection, which changes (a harvested
			// plant, a tree's variant) without anything the cache witnesses: the shadow views draw what
			// this phase admits, so it is taken again every frame. Static verdicts other than None stand.
			// A re-read entry's hidden and actor verdicts are the frame's too.
			const bool frameVerdict = reason == Ineligible::None || reason == Ineligible::Switch ||
			                          (rereads && (reason == Ineligible::Hidden || reason == Ineligible::Actor));
			if ((entry.parentReason == Ineligible::Switch || rereads) && frameVerdict) {
				reason = ClassifyFrame(entry);
				// A verdict the frame changed is a classification: EvaluateRound takes the entry's traits again.
				if (reason != trackedEntry->candidateReason)
					trackedEntry->candidateFrame = frame;
				trackedEntry->candidateReason = reason;
			}
			++stats.ineligible[static_cast<std::size_t>(reason)];
			a_bucket = reason;
			shadowOnly = reason != Ineligible::None && !DeferredToAccumulate(reason) && ShadowOnlyCaster(reason, *geometry);
			if (reason != Ineligible::None && !DeferredToAccumulate(reason) && !shadowOnly)
				return false;
		} else {
			LightingDescriptors descriptors;
			auto& verdict = trackedEntry->verdict;
			auto& runtime = geometry->GetGeometryRuntimeData();
			auto* witnessProperty = runtime.shaderProperty.get();
			const auto* witnessMaterial = witnessProperty ? witnessProperty->material : nullptr;
			const std::uint8_t fadeState = FadeStateOf(witnessProperty);
			// Resolve the RTTI cast once per property pointer rather than once per frame.
			if (trackedEntry->castProperty != witnessProperty) {
				trackedEntry->castProperty = witnessProperty;
				trackedEntry->castResult = netimmerse_cast<RE::BSLightingShaderProperty*>(witnessProperty);
				trackedEntry->castRtti = witnessProperty ? witnessProperty->GetRTTI() : nullptr;
				++stats.castResolved;
			}
			RE::BSLightingShaderProperty* castCache = trackedEntry->castResult;
			const bool hit = !denseWalk && verdict.cached && verdict.rendererData == runtime.rendererData &&
			                 verdict.property == witnessProperty && verdict.material == witnessMaterial &&
			                 verdict.fadeState == fadeState;
			if (hit && !classifyProbe) {
				reason = verdict.reason;
				++stats.classifyHits;
			} else {
				reason = ClassifyStatic(*geometry, &descriptors, nullptr, &castCache);
				if (hit) {
					// probe: the cache said one thing and the computation another, which is a defect.
					++stats.classifyChecked;
					if (reason != verdict.reason) {
						++stats.classifyDiffers;
						if (stats.classifyDiffers == 1)
							logger::warn("[DCLF] classify cache: '{}' is cached as {} but recomputes as {}",
								geometry->name.c_str() ? geometry->name.c_str() : "?",
								kIneligibleNames[static_cast<std::size_t>(verdict.reason)], kIneligibleNames[static_cast<std::size_t>(reason)]);
					}
					reason = verdict.reason;  // the cache is what the frame would have used
				} else if (!denseWalk) {
					if (CacheableVerdict(reason))
						verdict = { true, reason, runtime.rendererData, witnessProperty, witnessMaterial, fadeState };
					else
						verdict.cached = false;
				}
			}
			timer.Add(BuildPart::ClassifyStatic);
			if (reason == Ineligible::None)
				reason = ClassifyFrame(entry);
			timer.Add(BuildPart::ClassifyFrame);
			if (denseWalk) {
				referenceReasons[geometry] = reason;
			} else {
				trackedEntry->candidateFrame = frame;
				trackedEntry->candidateReason = reason;
				trackedEntry->classifyInputs = ClassifyInputsOf(*geometry);
			}
			++stats.ineligible[static_cast<std::size_t>(reason)];
			a_bucket = reason;
			if (reason == Ineligible::Technique)
				++stats.techniqueRejects[descriptors.rejectedTechnique & 63];
			if (coverageProbe && reason == Ineligible::NotLightingShader) {
				++stats.propertyRejects[trackedEntry->castRtti];
				const auto* rejectedAlpha = geometry->GetGeometryRuntimeData().alphaProperty.get();
				if (rejectedAlpha && rejectedAlpha->GetAlphaBlending()) {
					++stats.rejectedBlended;
				} else {
					++stats.rejectedOpaque;
					if (rejectedAlpha && rejectedAlpha->GetAlphaTesting())
						++stats.rejectedOpaqueAlphaTest;
				}
			}
			shadowOnly = reason != Ineligible::None && !DeferredToAccumulate(reason) && ShadowOnlyCaster(reason, *geometry);
			if (reason != Ineligible::None && !DeferredToAccumulate(reason) && !shadowOnly)
				return false;
		}

		auto& data = geometry->GetGeometryRuntimeData();
		// A face shape's positions: its head's snapshot (FaceSnapshots), and the region of the positions buffer
		// the epochs upload them to. Every record of a face shape has them, whatever its verdict (a deferred decal
		// included); without a snapshot the shape gets no record, and the engine draws it - and, the snapshot
		// being the head's, every other shape of its head too.
		FaceSnapshots::ShapeView face{};
		std::uint32_t faceRegion = kNoFaceRegion;
		if (faceShape && !ResolveFace(*geometry, face, faceRegion))
			return false;
		// Geometry, shared between every object drawing the same TriShape. A skinned shape draws its
		// skin partitions' own buffers, one draw each (ClassifyStatic has checked every one).
		const auto* skinPartitions = data.skinInstance ? data.skinInstance->skinPartition.get() : nullptr;
		const RE::NiSkinPartition::Partition* skinPartition = skinPartitions ? &skinPartitions->partitions[0] : nullptr;
		// Which of them the engine draws: for the shadow views, from the fade node's LOD level as both of
		// its pass builders read it; the accumulate phase takes the main camera's from its pass. A skin
		// the engine draws no partition of is not drawn at all.
		std::uint32_t partitionMask = 0;
		if (skinPartitions) {
			partitionMask = SkinPartitionMask(*data.skinInstance, LodRowOf(*geometry, data.shaderProperty.get()));
			if (!partitionMask) {
				--stats.ineligible[static_cast<std::size_t>(reason)];
				++stats.ineligible[static_cast<std::size_t>(Ineligible::Hidden)];
				a_bucket = Ineligible::Hidden;
				return false;
			}
		}
		auto* triShape = skinPartition ? skinPartition->buffData : data.rendererData;
		const std::uint32_t geometrySlot = ResolveGeometrySlot(*geometry, triShape, skinPartition, timer);
		if (geometrySlot == Tables::kSlotFree) {
			--stats.ineligible[static_cast<std::size_t>(reason)];
			++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
			a_bucket = Ineligible::UnstableBuffer;
			return false;
		}
		// The other partitions' slots, linked from the first so a draw can walk them. Every partition is
		// resolved and linked whatever this frame's mask, because the main camera's may differ.
		if (skinPartitions && skinPartitions->numPartitions > 1) {
			std::uint32_t previous = geometrySlot;
			bool unstable = false;
			for (std::uint32_t i = 1; i < skinPartitions->numPartitions && !unstable; ++i) {
				const auto& part = skinPartitions->partitions[i];
				const std::uint32_t slot = ResolveGeometrySlot(*geometry, part.buffData, &part, timer);
				if (slot == Tables::kSlotFree) {
					unstable = true;
					break;
				}
				if (tables.geometries[previous].nextPartition != slot) {
					tables.geometries[previous].nextPartition = slot;
					tables.NoteGeometry(previous);
				}
				previous = slot;
			}
			if (unstable) {
				--stats.ineligible[static_cast<std::size_t>(reason)];
				++stats.ineligible[static_cast<std::size_t>(Ineligible::UnstableBuffer)];
			a_bucket = Ineligible::UnstableBuffer;
				return false;
			}
			if (tables.geometries[previous].nextPartition != kNoPartition) {
				tables.geometries[previous].nextPartition = kNoPartition;
				tables.NoteGeometry(previous);
			}
		}

		ObjectRecord object{};
		StoreTransform(geometry->world, object.world);
		StoreTransform(geometry->previousWorld, object.previousWorld);
		object.boundCenter[0] = geometry->worldBound.center.x;
		object.boundCenter[1] = geometry->worldBound.center.y;
		object.boundCenter[2] = geometry->worldBound.center.z;
		object.boundRadius = geometry->worldBound.radius;
		object.geometryIndex = geometrySlot;
		// The accumulator's half is not known yet: no bindings, and not native-visible. Both are
		// patched by BuildAccumulatePhase, and nothing between the two phases reads them -
		// BuildDrawsCS rejects an object with kObjectNoBindings before it looks at the indices.
		object.materialIndex = 0;
		object.pipelineIndex = 0;
		object.flags = kObjectNoBindings | (shadowOnly ? kObjectShadowOnly : 0u);
		// Two-sidedness is the property's, not the pass's: the shadow epochs key their pipelines on it
		// before the accumulate phase has computed the static flags, and that phase derives the same bit.
		if (auto* sceneProperty = data.shaderProperty.get(); sceneProperty && sceneProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided))
			object.flags |= kObjectTwoSided;
		// Likewise the alpha test and its threshold: a shadow caster's alpha reference comes from the
		// object record (Utility.hlsl, DCLFAlphaTestRef), which is packed before the accumulate phase.
		if (const auto* sceneAlpha = data.alphaProperty.get(); sceneAlpha && sceneAlpha->GetAlphaTesting())
			object.flags |= kObjectAlphaTest | (static_cast<std::uint32_t>(sceneAlpha->alphaThreshold) << kObjectAlphaThresholdShift);

		// Skinning: the engine's own palette. Its per-frame update (AE FUN_140e4ff90) is what the bone
		// setter runs from the native draw this object no longer gets; it is idempotent within a frame
		// (frameID), copies the current palette to the previous one first, and writes three float4 rows
		// a bone in absolute world space - which is what the shader indexes, so the rows are copied as
		// they are and made eye-relative by the epoch, like World.
		std::uint32_t objectBoneRows = 0;
		const float* boneCurrent = nullptr;
		const float* bonePrevious = nullptr;
		if (auto* skin = data.skinInstance.get(); skin && ActiveToggles().skinned) {
			timer.Add(BuildPart::Record);
			if (trackedEntry->skinUpdatedFrame != frame) {
				UpdateSkin(skin, geometry->world);
				trackedEntry->skinUpdatedFrame = frame;
			}
			skinnedObjects.push_back(geometry);
			const std::uint32_t rows = skin->numMatrices * 3;
			if (rows && skin->boneMatrices && skin->prevBoneMatrices && rows <= 240) {
				objectBoneRows = rows;
				boneCurrent = static_cast<const float*>(skin->boneMatrices);
				bonePrevious = static_cast<const float*>(skin->prevBoneMatrices);
				object.flags |= kObjectSkinned;
				++stats.skinned;
				stats.boneRows += rows;
			}
			timer.Add(BuildPart::Skinning);
		}
		// The object's slot: past the last `continue`, so a slot is only ever taken by a record that is written.
		const std::uint32_t slotBefore = trackedEntry->slot;
		const std::uint32_t objectId = AcquireObjectSlot(*trackedEntry, geometry);
		// The accumulated half is the accumulate phase's: a record written again for the same object keeps the patch the
		// last phase gave it, which the next renews or lets lapse (LapseAccumulated). An entry written every frame (a face,
		// an actor's part) would otherwise lose its patch here and take it back there, every frame.
		const bool keepMember = wasMember && objectId == slotBefore && a_bucket == Ineligible::None && !shadowOnly &&
		                        !(tables.objects[objectId].flags & kObjectFree);
		if (wasMember && !keepMember)
			DropResidentSlot(slotBefore, true, false);
		const bool keepHalf = keepMember || (!denseWalk && objectId == slotBefore && objectId < patchedFrame.size() && patchedFrame[objectId] + 1 == frame &&
		                                        !(tables.objects[objectId].flags & kObjectFree));
		tables.objectSeen[objectId] = walkSerial;
		if (objectBoneRows) {
			const std::size_t at = std::size_t(tables.PlaceBones(objectId, objectBoneRows)) * 4;
			const std::size_t bytes = std::size_t(objectBoneRows) * 4 * sizeof(float);
			// Noted only when the rows differ (a skin that stands still uploads nothing).
			if (std::memcmp(&tables.bones[at], boneCurrent, bytes) != 0 || std::memcmp(&tables.previousBones[at], bonePrevious, bytes) != 0) {
				std::memcpy(&tables.bones[at], boneCurrent, bytes);
				std::memcpy(&tables.previousBones[at], bonePrevious, bytes);
				tables.NoteChange(objectId, kChangePalette);
			}
		} else {
			tables.FreeBones(objectId);
		}
		// Whether the engine would draw this object into a shadow map, and with which Utility
		// technique. It belongs here and nowhere else: every shadow view is rendered between this
		// phase and the next, so a verdict taken later would arrive after the views that need it.
		const auto* shadowProperty = data.shaderProperty.get();
		const auto shadowReject = ShadowCasterReject(shadowProperty, geometry);
		static_assert(static_cast<std::size_t>(ShadowReject::Count) <= std::tuple_size_v<decltype(stats.shadowRejects)>);
		++stats.shadowRejects[static_cast<std::size_t>(shadowReject)];
		ID3D11ShaderResourceView* shadowDiffuse = nullptr;
		const RE::BSShaderMaterial* shadowMaterial = nullptr;
		// An alpha-tested technique (bit 0x80) samples the diffuse: what the epochs' binding records name, read here so
		// their builds never touch the property. The shadow views and the sky map share the one read.
		auto sampleDiffuse = [&]() {
			if (shadowMaterial)
				return;
			if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(shadowProperty->material)) {
				shadowMaterial = material;
				auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
				shadowDiffuse = texture ? texture->resourceView : nullptr;
				if (shadowDiffuse && tables.shadowTextureSeen.insert(shadowDiffuse).second)
					tables.shadowTextureSet.push_back(shadowDiffuse);
			}
		};
		// The pipeline the technique draws with, listed once per key.
		auto useKey = [&](std::vector<ShadowPipelineKey>& a_used, std::uint32_t a_technique) {
			const ShadowPipelineKey key{ a_technique,
				shadowProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(tables.geometries[geometrySlot].vertexDesc) };
			if (std::find(a_used.begin(), a_used.end(), key) == a_used.end())
				a_used.push_back(key);
		};
		// A volumetric-only caster is one too, for the views of the volumetric lighting copy alone.
		const bool volumetricOnly = shadowReject == ShadowReject::VolumetricOnly;
		if (shadowReject == ShadowReject::None || volumetricOnly) {
			++stats.shadowCasters;
			if (volumetricOnly)
				object.flags |= kObjectVolumetricOnly;
			const std::uint32_t shadowTechnique = ShadowUtilityTechnique(shadowProperty, geometry);
			tables.shadowTechnique[objectId] = shadowTechnique;
			if (shadowTechnique & 0x80)
				sampleDiffuse();
			useKey(tables.shadowKeysUsed, shadowTechnique);
		} else {
			object.flags |= kObjectNoShadow;
			tables.shadowTechnique[objectId] = 0;
		}
		tables.shadowReject[objectId] = static_cast<std::uint8_t>(shadowReject);
		// Skylighting's occlusion map, when DCLF draws it: whether and how this object draws into it.
		std::uint32_t skyTechnique = 0;
		if (SkyOcclusionEnabled())
			if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(shadowProperty))
				skyTechnique = Skylighting::OcclusionTechnique(lighting, geometry, true);
		tables.skyTechnique[objectId] = skyTechnique;
		if (skyTechnique) {
			if (skyTechnique & 0x80)
				sampleDiffuse();
			useKey(tables.skyKeysUsed, skyTechnique);
		}
		tables.sunEntry[objectId] = SunEntryOf(*trackedEntry, *geometry);
		tables.lodFade[objectId] = LodFadeNodeOf(data.shaderProperty.get());
		if (face.positions)
			PushFaceStream(*geometry, objectId, face, faceRegion);
		else
			tables.faceStream[objectId] = kNoFaceStream;
		tables.shadowDiffuse[objectId] = shadowDiffuse;
		tables.shadowMaterial[objectId] = shadowMaterial;
		tables.sceneFlags[objectId] = object.flags;
		tables.objectGeometry[objectId] = geometry;
		tables.objectIdentity[objectId] = trackedEntry->identity;
		tables.objectGroup[objectId] = trackedEntry->groupIdentity;
		const bool retainWetness = trackedEntry->actorOwned &&
			tables.actorWetness.Contains(objectId, trackedEntry->groupIdentity, trackedEntry->identity);
		const std::uint32_t keptPipeline = tables.draws[objectId].pipelineIndex;
		if (keepHalf) {
			// The scene bits the accumulate phase keeps are this write's; everything else is the patch's.
			const auto& kept = tables.objects[objectId];
			object.flags = (object.flags & kSceneKeptFlags) | (kept.flags & ~kSceneKeptFlags);
			object.materialIndex = kept.materialIndex;
			object.pipelineIndex = kept.pipelineIndex;
			tables.objects[objectId] = object;
		} else {
			// The extras rows are allocated by the accumulate phase, which is where the descriptors that
			// decide whether an object needs them are derived.
			tables.FreeExtras(objectId);
			tables.objects[objectId] = object;
			tables.shading[objectId] = ObjectShading{};
			tables.emissiveMult[objectId] = 1.0f;
			tables.lights[objectId] = ObjectLights{};
			tables.treeAnim[objectId] = ObjectTreeAnim{};
			// Wetness is actor-owned frame state, not an output of scene-record
			// construction. Keep it across rewrites of the same member incarnation.
			if (!retainWetness)
				tables.skinWetness[objectId] = {};
			tables.fadeDistance[objectId] = 0.0f;
		}
		tables.actorWetness.Set(objectId, trackedEntry->actorOwned ? trackedEntry->groupIdentity : 0,
			trackedEntry->identity);
		if (trackedEntry->actorOwned)
			tables.actorObjects.push_back(objectId);
		{
			const auto& runtime = geometry->GetGeometryRuntimeData();
			const auto* shaderProperty = runtime.shaderProperty.get();
			const auto* alphaProperty = runtime.alphaProperty.get();
			tables.SetWatch(objectId, Tables::kWatchShading,
				(shaderProperty && shaderProperty->GetControllers()) || (alphaProperty && alphaProperty->GetControllers()));
		}
		tables.skinPartitions[objectId] = static_cast<std::uint8_t>(skinPartitions && skinPartitions->numPartitions > 1 ? partitionMask : 0);
		if (!denseWalk) {
			trackedEntry->objectStamp = objectStamp;
			trackedEntry->objectId = objectId;
			// Scene membership: an eligible record written (entering the scene, or an event rewrote it) is bound again.
			if (a_bucket == Ineligible::None && !shadowOnly)
				bindQueue.push_back(objectId);
		}

		const auto& geometryRecord = tables.geometries[geometrySlot];
		DrawSequence draw{};
		draw.pipelineIndex = keepHalf ? keptPipeline : 0;
		draw.vertexBufferAddress = geometryRecord.vertexAddress;
		draw.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(geometryRecord.vertexBytes, UINT32_MAX));
		draw.vertexStride = geometryRecord.vertexStride;
		// The second stream repeats the first; the epochs replace it with a face shape's positions.
		draw.streamBufferAddress = draw.vertexBufferAddress;
		draw.streamBufferSize = draw.vertexBufferSize;
		draw.streamStride = draw.vertexStride;
		draw.indexBufferAddress = geometryRecord.indexAddress;
		draw.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(geometryRecord.indexBytes, UINT32_MAX));
		draw.indexFormat = kIndexFormatR16;
		draw.indexCount = geometryRecord.indexCount;
		draw.instanceCount = 1;
		draw.firstIndex = geometryRecord.firstIndex;
		draw.vertexOffset = 0;
		draw.firstInstance = 0;
		tables.draws[objectId] = draw;
		timer.Add(BuildPart::Record);
		return true;
	}

	/**
	 * @brief The geometry slot for a TriShape: found, refreshed in place, or newly resolved.
	 *
	 * @return the slot, or Tables::kSlotFree when the buffers cannot be made stable for the render graph.
	 */
	std::uint32_t SceneStore::ResolveGeometrySlot(RE::BSGeometry& a_geometry, const RE::BSGraphics::TriShape* a_triShape,
		const RE::NiSkinPartition::Partition* a_skinPartition, PartTimer& a_timer)
	{
		if (!a_triShape)
			return Tables::kSlotFree;
		auto& gpu = GpuResources::Get();
		const bool resolveBuffers = frameResolveBuffers;
		auto geometryIt = geometryIndex.find(a_triShape);
		const bool newGeometry = geometryIt == geometryIndex.end();
		// A slot found by address but describing other buffers is a TriShape reallocated at the same
		// address: it is resolved again into the same slot. So is one resolved while the render graph was off.
		const bool staleGeometry = !newGeometry &&
		                           ((resolveBuffers && tables.geometries[geometryIt->second].vertexAddress == 0) ||
		                               (resolveBuffers && (!tables.geometryImports[geometryIt->second].vertexOwner || !tables.geometryImports[geometryIt->second].indexOwner)) ||
		                               tables.geometries[geometryIt->second].vertexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer) ||
		                               tables.geometries[geometryIt->second].indexBuffer != reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer));
		if (staleGeometry)
			++stats.geometriesRefreshed;
		if (newGeometry || staleGeometry) {
			// The buffers are resolved once per TRISHAPE, not once per object. The slot's leases keep them resolved
			// (GpuResources) for as long as the slot lives.
			std::optional<GpuResources::LeasedBuffer> vertexLease, indexLease;
			if (resolveBuffers) {
				// The render graph reads the game's buffers in place; they must never move (GpuResources).
				vertexLease = gpu.Acquire(reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer));
				if (vertexLease)
					indexLease = gpu.Acquire(reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer));
				if (!vertexLease || !indexLease) {
					// Nothing was inserted, so the next object sharing this TriShape retries, exactly
					// as it did when every object resolved for itself. A stale slot is freed: its
					// record no longer describes anything.
					if (staleGeometry) {
						const std::uint32_t slot = geometryIt->second;
						FreeGeometrySlot(slot);
						freedGeometry.push_back(slot);
						slotsFreedThisFrame = true;
					}
					a_timer.Add(BuildPart::Resolve);
					return Tables::kSlotFree;
				}
			}
			const auto& shape = static_cast<RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData();
			GeometryRecord record;
			record.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer);
			record.indexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer);
			record.vertexDesc = std::bit_cast<std::uint64_t>(a_triShape->vertexDesc);
			// The stride the engine binds is the desc's low nibble in dwords (engine notes: vertex input).
			record.vertexStride = static_cast<std::uint32_t>(record.vertexDesc & 0xFu) * 4u;
			record.vertexCount = a_skinPartition ? a_skinPartition->vertices : shape.vertexCount;
			record.indexCount = static_cast<std::uint32_t>(a_skinPartition ? a_skinPartition->triangles : shape.triangleCount) * 3;
			record.firstIndex = 0;
			if (vertexLease && indexLease) {
				record.vertexAddress = vertexLease->buffer.address;
				record.vertexBytes = vertexLease->buffer.size;
				record.indexAddress = indexLease->buffer.address;
				record.indexBytes = indexLease->buffer.size;
			}
			const std::uint32_t slot = staleGeometry ? geometryIt->second : AllocateGeometrySlot();
			// Other objects' draws copied the old record: the delta walk re-evaluates the ones it kept.
			if (staleGeometry)
				refreshedGeometry.push_back(slot);
			tables.geometries[slot] = record;
			tables.geometryImports[slot] = vertexLease && indexLease ? Tables::GeometryImport{ vertexLease->generation, indexLease->generation,
				vertexLease->owner, indexLease->owner } : Tables::GeometryImport{};
			tables.NoteGeometry(slot);
			tables.geometrySlotKey[slot] = a_triShape;
			if (!staleGeometry)
				geometryIt = geometryIndex.emplace(a_triShape, slot).first;
			a_timer.Add(BuildPart::Resolve);
		} else if (resolveBuffers && tables.geometryLastUsed[geometryIt->second] != frame) {
			// First use of the slot this frame: the Touch above already kept the references alive.
			a_timer.Add(BuildPart::DedupHit);
		}
		tables.geometryLastUsed[geometryIt->second] = frame;
		return geometryIt->second;
	}

	void SceneStore::BuildFullOrder()
	{
		order.clear();
		order.reserve(tracked.size());
		for (auto& [geometry, entry] : tracked) {
			// In full: the light path keeps a record without writing it, and the full evaluation's sweep
			// (SweepObjectSlots) frees every slot this walk did not write.
			entry.scheduledWalk = walkSerial;
			entry.fullWalk = walkSerial;
			order.push_back({ geometry, &entry, nullptr });
		}
	}

	void SceneStore::Schedule(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_full)
	{
		if (a_full)
			a_tracked.fullWalk = walkSerial;
		// An entry the first round only moved can still be written in full by a later round (its geometry slot).
		if (a_tracked.scheduledWalk == walkSerial && a_tracked.movedWalk != walkSerial)
			return;
		a_tracked.scheduledWalk = walkSerial;
		a_tracked.movedWalk = 0;
		order.push_back({ a_geometry, &a_tracked, nullptr });
	}

	std::uint64_t SceneStore::ShadingInputsOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		Fnv1a hash;
		auto mix = [&hash](std::uint64_t a_value) { hash.Mix(a_value); };
		const auto& data = a_geometry.GetGeometryRuntimeData();
		const auto* property = data.shaderProperty.get();
		mix(reinterpret_cast<std::uintptr_t>(property));
		if (property) {
			mix(property->flags.underlying());
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			mix(reinterpret_cast<std::uintptr_t>(material));
			// Only a lighting property's material is a BSLightingShaderMaterialBase; for any other the record reads
			// nothing more. The cast is the entry's (WriteObject resolves it per property pointer).
			const bool lighting = a_tracked.castProperty == property ? a_tracked.castResult != nullptr :
			                                                           netimmerse_cast<const RE::BSLightingShaderProperty*>(property) != nullptr;
			if (material && lighting) {
				mix(std::bit_cast<std::uint32_t>(material->materialAlpha));
				const auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
				mix(reinterpret_cast<std::uintptr_t>(texture ? texture->resourceView : nullptr));
			}
		}
		const auto* alpha = data.alphaProperty.get();
		mix(reinterpret_cast<std::uintptr_t>(alpha));
		if (alpha)
			mix((std::uint64_t(alpha->alphaFlags) << 8) | alpha->alphaThreshold);
		return hash.value;
	}

	SceneStore::ShadowInputs SceneStore::ShadowInputsOf(std::uint32_t a_slot) const
	{
		if (a_slot == kNoObjectSlot || a_slot >= tables.objects.size() || (tables.objects[a_slot].flags & kObjectFree))
			return {};
		const auto& object = tables.objects[a_slot];
		return { tables.shadowDiffuse[a_slot], object.geometryIndex < tables.geometries.size() ? tables.geometries[object.geometryIndex].vertexDesc : 0,
			tables.shadowTechnique[a_slot], object.flags & (kObjectNoShadow | kObjectTwoSided | kObjectFree), tables.shadowReject[a_slot], tables.skyTechnique[a_slot] };
	}

	bool SceneStore::KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		auto& data = a_geometry->GetGeometryRuntimeData();
		auto* skin = data.skinInstance.get();
		const std::uint32_t slot = a_tracked.slot;
		if (!skin || !ActiveToggles().skinned || !(tables.objects[slot].flags & kObjectSkinned))
			return false;
		const auto* partitions = skin->skinPartition.get();
		std::uint32_t mask = 0;
		if (partitions) {
			mask = SkinPartitionMask(*skin, LodRowOf(*a_geometry, data.shaderProperty.get()));
			if (!mask)
				return false;
		}
		// The size the engine's last palette update gave it; the job checks it again after this frame's.
		const std::uint32_t rows = skin->numMatrices * 3;
		if (!rows || !skin->boneMatrices || !skin->prevBoneMatrices || rows > 240 || rows != tables.boneRows[slot])
			return false;
		skinnedObjects.push_back(a_geometry);
		++stats.lightSkins;
		const auto partitionMask = static_cast<std::uint8_t>(partitions && partitions->numPartitions > 1 ? mask : 0);
		if (tables.skinPartitions[slot] != partitionMask)
			tables.NoteChange(slot, kChangeSkin);
		tables.skinPartitions[slot] = partitionMask;
		++stats.skinned;
		stats.boneRows += rows;
		return true;
	}

	bool SceneStore::RootMoves(const RE::NiAVObject* a_root)
	{
		// Only a reference's root: a multibound's bound is its shape's, and an actor's entry is never tested.
		if (!a_root || !a_root->GetUserData())
			return false;
		// Kept until an event under the root forgets it (ScheduleRoot) or its last dependent leaves (UnlistDependents).
		const auto [motion, inserted] = rootMotion.try_emplace(a_root, false);
		if (inserted) {
			motion->second = RootMovesNow(a_root);
			// Its bound is taken by the root pass (QueueRoots), keyed by its reference's move events.
			if (motion->second)
				movingRoots.try_emplace(a_root, MovingRoot{ a_root->GetUserData(), FindCategoryNode(const_cast<RE::NiAVObject*>(a_root), nullptr) });
		}
		return motion->second;
	}

	bool SceneStore::RootMovesNow(const RE::NiAVObject* a_root)
	{
		if (!a_root || !a_root->GetUserData())
			return false;
		bool moves = false;
		VisitSubtree(const_cast<RE::NiAVObject*>(a_root), [&](RE::NiAVObject& a_object) {
			if (a_object.GetControllers() || NonFixedBody(a_object))
				moves = true;
			else if (auto* geometry = a_object.AsGeometry())
				// A skin's bound follows its bones, which walk parity found moving with no controller or body in sight.
				moves = geometry->GetGeometryRuntimeData().skinInstance != nullptr;
			return !moves;
		});
		return moves;
	}

	std::uint64_t SceneStore::ClassifyInputsOf(const RE::BSGeometry& a_geometry)
	{
		// What ClassifyStatic reads: the renderer data and skin, the property, its flags, material and fade state, the
		// material alpha, and the alpha property.
		Fnv1a hash;
		auto mix = [&hash](std::uint64_t a_value) { hash.Mix(a_value); };
		const auto& data = a_geometry.GetGeometryRuntimeData();
		mix(reinterpret_cast<std::uintptr_t>(data.rendererData));
		mix(reinterpret_cast<std::uintptr_t>(data.skinInstance.get()));
		mix(reinterpret_cast<std::uintptr_t>(data.skinInstance ? data.skinInstance->skinPartition.get() : nullptr));
		// Skylighting::OcclusionTechnique's size test, which a record's sky technique follows.
		mix(a_geometry.worldBound.radius <= 32.0f ? 1u : 0u);
		const auto* property = data.shaderProperty.get();
		mix(reinterpret_cast<std::uintptr_t>(property));
		if (property) {
			mix(property->flags.underlying());
			mix(reinterpret_cast<std::uintptr_t>(property->material));
			mix(FadeStateOf(const_cast<RE::BSShaderProperty*>(property)));
			if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property); lighting && property->material)
				mix(std::bit_cast<std::uint32_t>(static_cast<const RE::BSLightingShaderMaterialBase*>(property->material)->materialAlpha));
		}
		if (const auto* alpha = data.alphaProperty.get()) {
			mix(reinterpret_cast<std::uintptr_t>(alpha));
			mix((std::uint64_t(alpha->alphaFlags) << 8) | alpha->alphaThreshold);
		}
		return hash.value;
	}

	void SceneStore::ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// The properties as this evaluation read them: an event on either classifies the entry again. A property swapped
		// without an event is taken up at the entry's next evaluation, and walk parity is the alarm for one that is not.
		const auto& data = a_geometry->GetGeometryRuntimeData();
		const void* property = data.shaderProperty.get();
		const void* alpha = data.alphaProperty.get();
		if (property != a_tracked.listedProperty) {
			if (a_tracked.listedProperty)
				Unlist(propertyDependents, a_tracked.listedProperty, a_geometry);
			if (property)
				propertyDependents[property].push_back(a_geometry);
			a_tracked.listedProperty = property;
		}
		if (alpha != a_tracked.listedAlpha) {
			if (a_tracked.listedAlpha)
				Unlist(propertyDependents, a_tracked.listedAlpha, a_geometry);
			if (alpha)
				propertyDependents[alpha].push_back(a_geometry);
			a_tracked.listedAlpha = alpha;
		}
	}

	void SceneStore::UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root)
	{
		if (a_tracked.listedProperty)
			Unlist(propertyDependents, a_tracked.listedProperty, a_geometry);
		if (a_tracked.listedAlpha)
			Unlist(propertyDependents, a_tracked.listedAlpha, a_geometry);
		a_tracked.listedProperty = nullptr;
		a_tracked.listedAlpha = nullptr;
		if (a_root)
			UnlistHiddenChain(a_geometry, a_tracked);
		if (a_root && a_tracked.listedRoot) {
			MarkSunEntryDirty(a_tracked.listedRoot);
			// The others under it: its bound takes this one in no more. The node is a key here; it may be gone.
			if (Unlist(rootDependents, a_tracked.listedRoot, a_geometry)) {
				dirtyRoots.push_back(a_tracked.listedRoot);
			} else {
				rootMotion.erase(a_tracked.listedRoot);
				movingRoots.erase(a_tracked.listedRoot);
			}
			a_tracked.listedRoot = nullptr;
		}
	}

	void SceneStore::Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		a_tracked.candidateFrame = 0;
		Schedule(a_geometry, a_tracked);
	}

	bool SceneStore::PlacementMatters(const Tracked& a_tracked)
	{
		const auto reason = a_tracked.candidateReason;
		return a_tracked.slot != kNoObjectSlot || a_tracked.candidateFrame == 0 || reason == Ineligible::None || reason == Ineligible::Hidden ||
		       reason == Ineligible::Switch || reason == Ineligible::Actor;
	}

	void SceneStore::ScheduleRoot(const RE::NiAVObject* a_root)
	{
		rootMotion.erase(a_root);
		movingRoots.erase(a_root);
		const auto it = rootDependents.find(a_root);
		if (it == rootDependents.end())
			return;
		for (auto* geometry : it->second)
			if (const auto entry = tracked.find(geometry); entry != tracked.end() && PlacementMatters(entry->second))
				Reclassify(entry->first, entry->second);
	}

	void SceneStore::MoveBucket(Tracked& a_tracked, Ineligible a_bucket)
	{
		const std::uint8_t bucket = a_bucket == Ineligible::Count ? Tracked::kNoBucket : static_cast<std::uint8_t>(a_bucket);
		if (a_tracked.bucket == bucket)
			return;
		if (a_tracked.bucket != Tracked::kNoBucket && buckets[a_tracked.bucket])
			--buckets[a_tracked.bucket];
		if (bucket != Tracked::kNoBucket)
			++buckets[bucket];
		a_tracked.bucket = bucket;
	}

	void SceneStore::ListFadeDependent(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// Only a kept record needs the event: a per-frame one is written in full anyway, unless it only moves or only
		// follows a switch.
		const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		const RE::BSFadeNode* node = property && (!a_tracked.perFrame || a_tracked.lightTraits) ? property->fadeNode : nullptr;
		if (node == a_tracked.fadeNode)
			return;
		UnlistFadeDependent(a_geometry, a_tracked);
		if (node) {
			fadeDependents[node].push_back(a_geometry);
			a_tracked.fadeNode = node;
		}
	}

	void SceneStore::UnlistFadeDependent(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		if (!a_tracked.fadeNode)
			return;
		Unlist(fadeDependents, a_tracked.fadeNode, a_geometry);
		a_tracked.fadeNode = nullptr;
	}

	std::uint32_t SceneStore::PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		// The movable set (dclf-event-driven-tables.md, "Reverse-engineering results"): a static reference never moves
		// in place, so what moves is owned by an actor, animated by a controller, or simulated by Havok. A skin's
		// palette, a face's snapshot and a switch's selection change without moving anything. A controller on the
		// shader or alpha property animates the material alpha the shadow verdict reads.
		std::uint32_t traits = 0;
		traits |= a_tracked.faceShape ? kTraitFace : 0u;
		traits |= a_tracked.actorOwned ? kTraitActor : 0u;
		// A switch's selection changes by event with the switch events (ApplySwitchEvents), not every frame.
		traits |= a_tracked.parentReason == Ineligible::Switch && !SwitchEventsLive() ? kTraitSwitch : 0u;
		const auto& data = a_geometry.GetGeometryRuntimeData();
		traits |= data.skinInstance ? kTraitSkin : 0u;
		if (const auto* property = data.shaderProperty.get(); property && property->GetControllers())
			traits |= kTraitAnimatedShading;
		if (const auto* alpha = data.alphaProperty.get(); alpha && alpha->GetControllers())
			traits |= kTraitAnimatedShading;
		for (const RE::NiAVObject* object = &a_geometry; object; object = object->parent) {
			if (object->GetControllers() || NonFixedBody(*object)) {
				traits |= kTraitMoves;
				break;
			}
			if (object == a_tracked.categoryNode)
				break;
		}
		return traits;
	}

	std::pair<bool, std::uint32_t> SceneStore::PerFrameOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, Ineligible a_reason, std::uint32_t& a_traits,
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool>* a_freshMotion)
	{
		// An unselected switch child may get a record when its switch selects it: every frame without the switch events,
		// by event with them (ApplySwitchEvents), like any other verdict.
		const bool mayRecord = (a_tracked.parentReason == Ineligible::Switch && !SwitchEventsLive()) || a_reason == Ineligible::None || DeferredToAccumulate(a_reason) ||
		                       ShadowOnlyCaster(a_reason, const_cast<RE::BSGeometry&>(a_geometry));
		std::uint32_t traits = PerFrameTraits(a_tracked, a_geometry);
		if (mayRecord && !(traits & (kTraitFace | kTraitActor)) && a_tracked.sunEntryNode) {
			bool moves = false;
			if (a_freshMotion) {
				const auto [motion, inserted] = a_freshMotion->try_emplace(a_tracked.sunEntryNode, false);
				if (inserted)
					motion->second = RootMovesNow(a_tracked.sunEntryNode);
				moves = motion->second;
			} else {
				moves = RootMoves(a_tracked.sunEntryNode);
			}
			if (moves)
				traits |= kTraitRootMoves;
		}
		a_traits = traits;
		// Per frame when a frame can change its record: its verdict lets it have one (or it is under a switch), and its
		// inputs change every frame. A movable slot's chain is re-read every frame whatever its verdict, as the design has
		// it: an actor's equipment is shown, hidden and swapped with no event of its own (walk parity caught a shield and a
		// chopping axe left hidden), and a visibility controller hides and shows its node.
		// A record whose only motion is its root's bound is not: the root pass takes the bound into its sun entry
		// (QueueRoots), since nothing on its own chain moves.
		const bool frameVerdict = a_reason == Ineligible::Hidden || a_reason == Ineligible::Switch || a_reason == Ineligible::Actor;
		const bool perFrame = a_tracked.faceShape || (mayRecord && traits && traits != kTraitRootMoves) || (traits & kTraitActor) ||
		                      (frameVerdict && (traits & kTraitMoves));
		// The light path takes only a record's placement, palette, switch selection or shading, for an actor's the exact
		// recheck of its verdict inputs (ActorRecordKept), and for a face shape's its head's snapshot (KeepFaceStream);
		// an entry it cannot have a record for is written in full, which re-reads its verdict.
		const std::uint32_t light = perFrame && mayRecord &&
		                                    !(traits & ~(kTraitSwitch | kTraitMoves | kTraitRootMoves | kTraitAnimatedShading | kTraitSkin | kTraitActor | kTraitFace)) ?
		                                traits :
		                                0u;
		return { perFrame, light };
	}

	bool SceneStore::ActorRecordKept(RE::BSGeometry& a_geometry, Tracked& a_tracked)
	{
		// What WriteObject's cached branch reads for an entry it re-reads (`rereads`): the static verdict's inputs, and
		// the frame's verdict when the cached one is a frame verdict. Equal inputs and the same verdict write the same
		// record but for its placement and palette, which the light path takes (walk parity is the check).
		if (ClassifyInputsOf(a_geometry) != a_tracked.classifyInputs) {
			++delta.reread;
			return false;
		}
		const Ineligible cached = a_tracked.candidateReason;
		const bool frameVerdict = cached == Ineligible::None || cached == Ineligible::Switch || cached == Ineligible::Hidden || cached == Ineligible::Actor;
		if (!frameVerdict)
			return true;
		// The verdict's inputs are the hidden bits on its chain, which have events (hiddenDependents), a switch's
		// selection, taken every frame here, and the toggles and fades, which classify it again. Without an event it
		// stands; a parity frame takes it again anyway and counts a change no event announced.
		const bool announced = a_tracked.hiddenEventFrame == frame || a_tracked.parentReason == Ineligible::Switch || !hiddenGating || a_tracked.hiddenChain.empty();
		if (!announced && !hiddenWitness) {
			++stats.verdictsSkipped;
			return true;
		}
		++stats.verdictsChecked;
		const Ineligible now = ClassifyFrame(a_tracked);
		if (now != cached && !announced && !stats.verdictsMissed++)
			stats.firstVerdictMissed = fmt::format("'{}' {} now {}", a_tracked.geometry->name.c_str() ? a_tracked.geometry->name.c_str() : "?",
				kIneligibleNames[static_cast<std::size_t>(cached)], kIneligibleNames[static_cast<std::size_t>(now)]);
		return now == cached;
	}

	void SceneStore::EvaluateRound(PartTimer& a_timer, std::size_t a_first)
	{
		const bool profileKinds = a_first == 0 && ProfileEnabled();
		for (std::size_t i = a_first; i < order.size(); ++i) {
			RE::BSGeometry* geometry = order[i].geometry;
			Tracked& entry = *order[i].tracked;
			a_timer.Add(BuildPart::LoopTail);
			// The entry's time by what the round does with it (EvaluateKind), from here to the end of the iteration.
			EvaluateKind kind = entry.lightTraits && entry.perFrame ? EvaluateKind::Light :
			                    !entry.perFrame                     ? EvaluateKind::Event :
			                    entry.faceShape                     ? EvaluateKind::Face :
			                    entry.actorOwned                    ? (entry.candidateReason == Ineligible::None ? EvaluateKind::Actor : EvaluateKind::ActorNoRecord) :
			                                                          EvaluateKind::PerFrame;
			const auto kindStart = profileKinds ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			struct KindSample
			{
				SceneStore::Stats* stats;
				const EvaluateKind& kind;
				std::chrono::steady_clock::time_point start;
				~KindSample()
				{
					if (!stats)
						return;
					stats->evaluateKindMs[static_cast<std::size_t>(kind)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
					++stats->evaluateKindCount[static_cast<std::size_t>(kind)];
				}
			} kindSample{ profileKinds ? &stats : nullptr, kind, kindStart };
			// A static that only moves or follows a switch, whose classification stands: what the full walk would take
			// again is the switch's verdict (WriteObject's cached branch) and the placement.
			if (a_first == 0 && entry.lightTraits && entry.perFrame && entry.fullWalk != walkSerial && entry.candidateFrame != 0) {
				const bool recorded = entry.slot != kNoObjectSlot && entry.objectStamp == objectStamp;
				bool kept = true;
				if (entry.lightTraits & kTraitActor) {
					kept = recorded && ActorRecordKept(*geometry, entry);
					if (kept && (entry.lightTraits & kTraitFace))
						kept = KeepFaceStream(*geometry, entry);
				} else if (entry.lightTraits & kTraitSwitch) {
					// The selection is what changes (a harvested plant, a tree's LOD); the rest of the chain is a static's,
					// fixed like any kept record's (dclf-event-driven-tables.md).
					const bool verdictKept = (entry.candidateReason == Ineligible::None || entry.candidateReason == Ineligible::Switch) &&
					                         (entry.candidateReason == Ineligible::None) == recorded;
					if (verdictKept && entry.switchNode && ActiveToggles().switchNodes)
						kept = SwitchSelects(*entry.switchNode, entry.switchChild) == (entry.candidateReason == Ineligible::None);
					else
						kept = verdictKept && ClassifyFrame(entry) == entry.candidateReason;
				} else {
					kept = recorded;
				}
				if (kept && (entry.lightTraits & (kTraitAnimatedShading | kTraitActor)))
					kept = ShadingInputsOf(entry, *geometry) == entry.shadingInputs;
				// Last: it lists the skin only when the record is kept (an unselected switch child has none).
				const bool keptSkin = kept && recorded && (entry.lightTraits & kTraitSkin);
				if (keptSkin)
					kept = KeepSkin(geometry, entry);
				if (kept) {
					for (std::uint32_t bit = 0; bit < stats.lightByTrait.size(); ++bit)
						stats.lightByTrait[bit] += (entry.lightTraits >> bit) & 1u;
					// Every walk lists the actors' records anew (Tables::actorObjects: wetness and capture parity read it).
					if (recorded && entry.actorOwned)
						tables.actorObjects.push_back(entry.slot);
					// An actor's skeleton moves without a controller or a body on its chain (Havok's behaviour graph). A mover
					// nothing moved this frame or the last keeps its placement and palette (MoveGated).
					if (recorded && (entry.lightTraits & (kTraitMoves | kTraitRootMoves | kTraitSkin | kTraitActor))) {
						std::uint8_t take = kTakePlacement | (keptSkin ? kTakePalette : 0);
						const MoveReason reason = MoveReasonOf(entry);
						if (reason == kMoveGated) {
							if (!moveWitness) {
								++stats.lightGated;
								entry.movedWalk = walkSerial;
								++delta.kept;
								continue;
							}
							take |= kTakeWitness;
						}
						++stats.lightPlaced;
						QueuePlacement(geometry, entry, take, reason);
					} else {
						entry.movedWalk = walkSerial;  // not written in full: a later round may still write it
						++delta.kept;
					}
					continue;
				}
			}
			if (kind == EvaluateKind::Light)
				kind = EvaluateKind::LightMissed;
			const ShadowInputs shadowBefore = ShadowInputsOf(entry.slot);
			const bool hadSlot = entry.slot != kNoObjectSlot;
			const std::uint32_t slotBefore = entry.slot;
			const Tables::Columns columnsBefore = hadSlot ? tables.ColumnsOf(slotBefore) : Tables::Columns{};
			const Ineligible reasonBefore = entry.candidateReason;
			Ineligible bucket = Ineligible::Count;
			const bool written = WriteObject(geometry, entry, a_timer, bucket);
			MoveBucket(entry, bucket);
			if (!written && entry.slot != kNoObjectSlot)
				ReleaseObjectSlot(entry);
			// What the write changed (Tables::changeLog); a slot taken anew is new in everything.
			if (written && entry.slot != kNoObjectSlot) {
				if (entry.slot == slotBefore)
					tables.NoteWrite(entry.slot, columnsBefore);
				else
					tables.NoteChange(entry.slot, kChangeAll);
			}
			// What its sun entry's candidacy reads (SunEntryAllows).
			if (hadSlot != (entry.slot != kNoObjectSlot) || reasonBefore != entry.candidateReason)
				MarkSunEntryDirty(entry.sunEntryNode);
			if (!(ShadowInputsOf(written ? entry.slot : kNoObjectSlot) == shadowBefore)) {
				shadowSetsDirty = true;
				if (slotBefore != kNoObjectSlot)
					shadowDirtySlots.push_back(slotBefore);
				if (written && entry.slot != kNoObjectSlot)
					shadowDirtySlots.push_back(entry.slot);
			}
			ListDependents(geometry, entry);
			// Classified now: a new entry, or an event took its classification again.
			if (entry.candidateFrame == frame) {
				// Per frame only when a frame can change its record: a face shape (classified every frame), an entry
				// under a switch, or one whose verdict lets it have a record, with inputs that change every frame. An
				// entry left out by its verdict is taken again when the verdict is due, like any other.
				std::uint32_t traits = 0;
				std::tie(entry.perFrame, entry.lightTraits) = PerFrameOf(entry, *geometry, entry.candidateReason, traits);
				entry.moveKey = MoveKeyOf(*geometry, entry.categoryNode);
				if (entry.lightTraits & kTraitActor)
					ListHiddenChain(geometry, entry);
				else if (!entry.hiddenChain.empty())
					UnlistHiddenChain(geometry, entry);
				entry.switchNode = nullptr;
				entry.switchChild = nullptr;
				if (entry.lightTraits & kTraitSwitch) {
					std::uint32_t switches = 0;
					const RE::NiAVObject* child = geometry;
					// ClassifyFrame's walk: every node below the category node.
					for (RE::NiNode* node = geometry->parent; node && node != entry.categoryNode; child = node, node = node->parent) {
						if (auto* switchNode = node->AsSwitchNode()) {
							++switches;
							entry.switchNode = switchNode;
							entry.switchChild = child;
						}
					}
					if (switches != 1)
						entry.switchNode = nullptr;
				}
			}
			if (entry.perFrame && !entry.perFrameListed) {
				entry.perFrameListed = true;
				perFrameSet.push_back({ geometry, &entry });
			}
			if (written) {
				if (entry.lightTraits & (kTraitAnimatedShading | kTraitActor))
					entry.shadingInputs = ShadingInputsOf(entry, *geometry);
				ListFadeDependent(geometry, entry);
				// A static's previous transform is its current one from its second update on; until then it is
				// written again.
				if (!entry.perFrame && !SameTransform(geometry->world, geometry->previousWorld)) {
					pendingEvaluation.push_back(geometry);
					++delta.settling;
				}
			} else {
				UnlistFadeDependent(geometry, entry);
			}
		}
	}

	void SceneStore::DeltaWalk()
	{
		++delta.walks;
		std::optional<ScenePartScope> scheduleScope(std::in_place, stats.scenePartMs[static_cast<std::size_t>(ScenePart::Schedule)],
			stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::Schedule)]);
		TracyCZoneN(scheduleZone, "CS.DCLF.Scene.Schedule", true);
		const bool full = fullEvaluation;
		// Before a full evaluation drops the node events: they name movers too.
		DrainMoveEvents(full);
		DrainHiddenEvents();
		if (full) {
			shadowIndexNeedsRebuild = true;
			shadowDirtySlots.clear();
		}
		// The shadow sets cover every record; BeginWalk clears them, and they are rebuilt only when an input changed.
		auto keptTextureSet = std::move(tables.shadowTextureSet);
		auto keptTextureSeen = std::move(tables.shadowTextureSeen);
		auto keptKeys = std::move(tables.shadowKeysUsed);
		auto keptSkyKeys = std::move(tables.skyKeysUsed);
		shadowSetsDirty = full || shadowSetsDirty;
		BeginWalk(!full);
		PartTimer timer(stats.partMs);
		if (full) {
			// Everything, as the full walk: after a reset, a load or a live toggle nothing kept can be trusted.
			fullEvaluation = false;
			++delta.full;
			perFrameSet.clear();
			pendingEvaluation.clear();
			fadeChanged.clear();
			fadeDependents.clear();
			propertyChanged.clear();
			nodeChanged.clear();
			dirtyRoots.clear();
			propertyDependents.clear();
			accumulatePatched.clear();
			lastPatched.clear();
			rootMotion.clear();
			movingRoots.clear();
			hiddenDependents.clear();
			tables.InvalidateChangeLog();
			buckets = {};
			for (auto& [geometry, entry] : tracked) {
				entry.perFrameListed = false;
				entry.bucket = Tracked::kNoBucket;
				entry.fadeNode = nullptr;
				entry.listedProperty = nullptr;
				entry.listedAlpha = nullptr;
			}
			BuildFullOrder();
		} else {
			order.clear();
			std::size_t counted = 0;
			auto count = [&](std::uint64_t& a_into) {
				a_into += order.size() - counted;
				counted = order.size();
			};
			// The addresses hold while no entry was added or erased since they were taken.
			const bool addressesHold = perFrameLayout == trackedLayout;
			stats.perFrameRelookups += addressesHold ? 0 : 1;
			for (std::size_t i = 0; i < perFrameSet.size();) {
				auto& item = perFrameSet[i];
				if (!addressesHold) {
					const auto it = tracked.find(item.geometry);
					item.tracked = it != tracked.end() ? &it->second : nullptr;
				}
				Tracked* entry = item.tracked;
				if (!entry || !entry->perFrameListed || !entry->perFrame || entry->scheduledWalk == walkSerial) {
					// Erased (or erased and added again, which listed the new entry anew), or no longer per frame
					// (queued for its next classification).
					if (entry && !entry->perFrame && entry->scheduledWalk != walkSerial)
						entry->perFrameListed = false;
					item = perFrameSet.back();
					perFrameSet.pop_back();
					continue;
				}
				Schedule(item.geometry, *entry, false);
				++i;
			}
			perFrameLayout = trackedLayout;
			count(delta.perFrame);
			for (auto* geometry : pendingEvaluation)
				if (const auto it = tracked.find(geometry); it != tracked.end())
					Schedule(it->first, it->second);
			pendingEvaluation.clear();
			count(delta.pending);
			for (const auto* node : fadeChanged) {
				const auto dependents = fadeDependents.find(node);
				if (dependents == fadeDependents.end())
					continue;
				for (auto* geometry : dependents->second)
					if (const auto it = tracked.find(geometry); it != tracked.end())
						Reclassify(it->first, it->second);
			}
			fadeChanged.clear();
			count(delta.fade);
			delta.propertyEvents += propertyChanged.size();
			delta.nodeEvents += nodeChanged.size();
			for (const void* key : propertyChanged) {
				const auto dependents = propertyDependents.find(key);
				if (dependents == propertyDependents.end())
					continue;
				for (auto* geometry : dependents->second)
					if (const auto it = tracked.find(geometry); it != tracked.end())
						Reclassify(it->first, it->second);
			}
			propertyChanged.clear();
			count(delta.property);
			std::sort(nodeChanged.begin(), nodeChanged.end(), [](const auto& a_left, const auto& a_right) { return a_left.get() < a_right.get(); });
			nodeChanged.erase(std::unique(nodeChanged.begin(), nodeChanged.end()), nodeChanged.end());
			for (auto& node : nodeChanged)
				ApplyNodeEvent(node.get());
			nodeChanged.clear();
			count(delta.node);
			std::sort(dirtyRoots.begin(), dirtyRoots.end());
			dirtyRoots.erase(std::unique(dirtyRoots.begin(), dirtyRoots.end()), dirtyRoots.end());
			for (const auto* root : dirtyRoots)
				ScheduleRoot(root);
			if (!residents.empty())
				residentRootEvents.insert(residentRootEvents.end(), dirtyRoots.begin(), dirtyRoots.end());
			dirtyRoots.clear();
			count(delta.roots);
		}
		TracyCZoneEnd(scheduleZone);
		scheduleScope.reset();
		{
			DCLF_SCENE_PART(SwitchEvents, "CS.DCLF.Scene.SwitchEvents");
			ApplySwitchEvents(full);
		}
		{
			DCLF_SCENE_PART(Evaluate, "CS.DCLF.Scene.Evaluate");
			EvaluateRound(timer, 0);
		}
		if (!shadowSetsDirty) {
			tables.shadowTextureSet = std::move(keptTextureSet);
			tables.shadowTextureSeen = std::move(keptTextureSeen);
			tables.shadowKeysUsed = std::move(keptKeys);
			tables.skyKeysUsed = std::move(keptSkyKeys);
		}
		FinishDeltaWalk(timer);
		if (full) {
			DCLF_SCENE_PART(ObjectSweep, "CS.DCLF.Scene.ObjectSweep");
			SweepObjectSlots();
		}
		{
			DCLF_SCENE_PART(SunCandidates, "CS.DCLF.Scene.SunCandidates");
			UpdateSunCandidates(full);
		}
		{
			DCLF_SCENE_PART(FaceWalk, "CS.DCLF.Scene.FaceWalk");
			EndFaceWalk();
		}
		stats.ineligible = buckets;
		delta.evaluated += order.size();
		delta.evaluatedMax = std::max(delta.evaluatedMax, static_cast<std::uint32_t>(order.size()));
		delta.live += tables.liveObjects;
	}

	void SceneStore::FinishDeltaWalk(PartTimer& a_timer)
	{
		// The kept slots' geometry slots: a referenced one lives (Tables::geometrySlots), and its leases keep its buffers
		// resolved (GpuResources). Every unresolved slot once buffers start resolving (the render graph came back), and a slot
		// ResolveGeometrySlot freed, are stale: the objects drawing them are written again, which resolves them.
		std::optional<ScenePartScope> scanScope(std::in_place, stats.scenePartMs[static_cast<std::size_t>(ScenePart::GeometryScan)],
			stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::GeometryScan)]);
		TracyCZoneN(scanZone, "CS.DCLF.Scene.GeometryScan", true);
		const bool resolveBuffers = frameResolveBuffers;
		const std::size_t second = order.size();
		staleGeometrySlots.clear();
		if (resolveBuffers && !geometryResolvedLastWalk) {
			for (std::uint32_t g = 0; g < tables.geometries.size(); ++g) {
				if (!tables.geometrySlots.Alive(g) || tables.geometryLastUsed[g] == frame)
					continue;  // free, or written this frame (resolved by the write)
				if (!tables.geometries[g].vertexAddress)
					staleGeometrySlots.push_back(g);
			}
		}
		geometryResolvedLastWalk = resolveBuffers;
		staleGeometrySlots.insert(staleGeometrySlots.end(), freedGeometry.begin(), freedGeometry.end());
		freedGeometry.clear();
		if (!staleGeometrySlots.empty()) {
			std::sort(staleGeometrySlots.begin(), staleGeometrySlots.end());
			staleGeometrySlots.erase(std::unique(staleGeometrySlots.begin(), staleGeometrySlots.end()), staleGeometrySlots.end());
			for (std::uint32_t s = 0; s < tables.objects.size(); ++s) {
				const auto& object = tables.objects[s];
				if ((object.flags & kObjectFree) || tables.objectSeen[s] == walkSerial)
					continue;
				// A skin of several partitions draws the chain of slots linked from its first (WriteObject).
				bool stale = false;
				std::uint32_t g = object.geometryIndex;
				for (std::uint32_t link = 0; !stale && link < kMaxSkinPartitions && g != kNoPartition; ++link) {
					stale = g >= tables.geometries.size() || std::binary_search(staleGeometrySlots.begin(), staleGeometrySlots.end(), g);
					if (!stale && !tables.skinPartitions[s] && !(object.flags & kObjectSkinned))
						break;
					g = stale ? g : tables.geometries[g].nextPartition;
				}
				if (!stale)
					continue;
				++delta.geometryDirty;
				if (const auto it = tracked.find(tables.objectGeometry[s]); it != tracked.end()) {
					Schedule(it->first, it->second);
				} else {
					tables.ResetObject(s);
					tables.objectFree.push_back(s);
					shadowSetsDirty = true;
					shadowDirtySlots.push_back(s);
				}
			}
		}
		TracyCZoneEnd(scanZone);
		scanScope.reset();
		std::optional<ScenePartScope> roundsScope(std::in_place, stats.scenePartMs[static_cast<std::size_t>(ScenePart::LaterRounds)],
			stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::LaterRounds)]);
		TracyCZoneN(roundsZone, "CS.DCLF.Scene.LaterRounds", true);
		EvaluateRound(a_timer, second);
		// A geometry slot re-resolved in place for one object: every kept object drawing it copied the old record.
		if (!refreshedGeometry.empty()) {
			std::sort(refreshedGeometry.begin(), refreshedGeometry.end());
			const std::size_t third = order.size();
			for (std::uint32_t s = 0; s < tables.objects.size(); ++s) {
				const auto& object = tables.objects[s];
				if ((object.flags & kObjectFree) || tables.objectSeen[s] == walkSerial ||
					!std::binary_search(refreshedGeometry.begin(), refreshedGeometry.end(), object.geometryIndex))
					continue;
				if (const auto it = tracked.find(tables.objectGeometry[s]); it != tracked.end())
					Schedule(it->first, it->second);
			}
			delta.geometryDirty += order.size() - third;
			EvaluateRound(a_timer, third);
			refreshedGeometry.clear();
		}
		TracyCZoneEnd(roundsZone);
		roundsScope.reset();
		DCLF_SCENE_PART(ShadowSets, "CS.DCLF.Scene.ShadowSets");
		// The shadow dependency index reads changed object slots from the same journal as the other kept tables.
		std::sort(tables.actorObjects.begin(), tables.actorObjects.end());
		tables.liveObjects = static_cast<std::uint32_t>(tables.objects.size() - tables.objectFree.size());
		if (!shadowSetsDirty) {
			stats.shadowCasters = keptShadowCasters;
			stats.shadowRejects = keptShadowRejects;
			a_timer.Add(BuildPart::Record);
			return;
		}
		RefreshShadowSets(false);
		const bool shadowIndexParity = SwitchEnabled(Switch::PersistentParity);
		if (shadowIndexParity && ParityDue(frame, 15)) {
			const auto textures = tables.shadowTextureSet;
			const auto shadowKeys = tables.shadowKeysUsed;
			const auto skyKeys = tables.skyKeysUsed;
			const auto casters = stats.shadowCasters;
			const auto rejects = stats.shadowRejects;
			std::uint32_t staleInputs = 0;
			for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
				if (slot >= shadowIndexedInputs.size() || shadowIndexedInputs[slot] != ShadowInputsOf(slot) ||
					(slot < shadowIndexedLive.size() && (shadowIndexedLive[slot] != 0) != !(tables.objects[slot].flags & kObjectFree)))
					++staleInputs;
			RefreshShadowSets(true);
			if (textures != tables.shadowTextureSet || shadowKeys != tables.shadowKeysUsed || skyKeys != tables.skyKeysUsed ||
				casters != stats.shadowCasters || rejects != stats.shadowRejects)
				logger::error("[DCLF] incremental shadow dependencies disagreed with same-table full reconstruction (frame {}, stale inputs {}, textures {}->{}, shadow keys {}->{}, sky keys {}->{}, casters {}->{}, rejects {}->{})",
					frame, staleInputs, textures.size(), tables.shadowTextureSet.size(), shadowKeys.size(), tables.shadowKeysUsed.size(), skyKeys.size(), tables.skyKeysUsed.size(),
					casters, stats.shadowCasters, rejects[0], stats.shadowRejects[0]);
		}
		keptShadowCasters = stats.shadowCasters;
		keptShadowRejects = stats.shadowRejects;
		a_timer.Add(BuildPart::Record);
	}
}
