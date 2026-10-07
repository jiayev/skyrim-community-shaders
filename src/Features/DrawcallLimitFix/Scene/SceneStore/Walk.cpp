#include "Internal.h"
#include "Features/DrawcallLimitFix/Common/FrameTrace.h"
#include "Features/DrawcallLimitFix/Diagnostics/HiddenWatch.h"

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
	 * no bindings (kObjectNoBindings). BuildAccumulatePhase patches them in place by object index, which is
	 * fixed for the frame from here on.
	 */
	void SceneStore::BuildScenePhase()
	{
		DCLF_FRAME_TRACE("BuildScenePhase");  // TEMP frame trace
		{
			DCLF_SCENE_PART(Prologue, "CS.DCLF.Scene.Prologue");
			// The frame number moved at BeginFrame, before the work was kicked: what the engine's hooks read in the window.
			// The change log keeps its tail (Tables::changeLog): a reader that has not read past the trimmed half reads every
			// slot again.
			tables.changeLog.Trim(1u << 17);
			tables.materialLog.Trim(1u << 16);
			tables.geometryLog.Trim(1u << 16);
			tables.extrasBlockLog.Trim(1u << 16);
			CheckChangeLog();
		}
		// Nothing is drawn while a load screen is up, and nothing here may touch the tracked geometry
		// either. The load frees the renderer data and the vertex and index buffers of the cell being
		// unloaded, while the NiPointers in `tracked` keep only the NiAVObjects alive; classifying those
		// entries reads freed BSGraphics::TriShape data and hands the render graph device addresses that no
		// longer belong to anything, which the GPU answers with VK_ERROR_DEVICE_LOST a few frames later.
		//
		// Before the events stopped being applied the scene graph across loads, this did not arise: the
		// category-node refresh pruned those entries as the cell's nodes vanished, so they never reached
		// this loop. Leaving the tables empty is both the safe and the obviously correct thing to draw
		// during a load screen.
		sceneBuilt = false;
		// The frame's globals (FrameGlobals: the render thread's capture at the frame's start; step 6e F2).
		const auto& globals = FrameGlobals::Current();
		if (globals.loading) {
			ResetSlotTables();
			InvalidateObjectIndices();
			accumulatedPasses.clear();
			stats.objects = 0;
				stats.geometries = 0;
			stats.pipelines = 0;
			stats.materials = 0;
			return;
		}
		PartTimer timer(stats.partMs);
		{
			DCLF_SCENE_PART(Prologue, "CS.DCLF.Scene.Prologue");
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
			frameInterior = globals.interior;
			frameDecalBias = globals.decalBias;
			ConstantEvaluator::ResetFrameAudits();
			geometryIndex.reserve(tracked.size());
		}
		timer.Add(BuildPart::Walk);
		{
			// Consume object-reference changes from the completed frame. Materials retire
			// on their last-reference event; geometry and pipeline slots retain their grace period.
			DCLF_SCENE_PART(SweepSlots, "CS.DCLF.Scene.SweepSlots");
			SweepSlots();
		}

		// Only what can have changed (DeltaWalk). CS_DCLF_WALK_PARITY=1: every 60 frames a dense rebuild, classifying
		// every object from scratch, is compared with the slot tables object by object.
		DeltaWalk();
		// What the next frame's values sample (FrameValues): the slots the shading events name, the walk's movers, the slots it wrote,
		// the roots it takes.
		{
			DCLF_SCENE_PART(Shading, "CS.DCLF.Scene.Shading");
			NameShadingEvents();
		}
		PublishPlacementPlan();
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
		const auto* head = a_geometry.parent ? netimmerse_cast<RE::BSFaceGenNiNode*>(a_geometry.parent) : nullptr;
		tables.SetFaceStream(a_slot, { a_slot, a_region, a_face.vertexCount, a_face.generation, a_face.positions, a_face.owner, std::move(headView), &a_geometry, head });
	}

	void SceneStore::ListFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		auto* head = a_geometry->parent ? netimmerse_cast<RE::BSFaceGenNiNode*>(a_geometry->parent) : nullptr;
		if (!head || a_tracked.faceHead == head)
			return;
		UnlistFaceShape(a_geometry, a_tracked);
		faceHeads[head].push_back(a_geometry);
		a_tracked.faceHead = head;
	}

	void SceneStore::UnlistFaceShape(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		const auto* head = std::exchange(a_tracked.faceHead, nullptr);
		if (!head)
			return;
		const auto it = faceHeads.find(head);
		if (it == faceHeads.end())
			return;
		std::erase(it->second, a_geometry);
		if (!it->second.empty()) {
			// Its siblings are written again, which rebuilds the head's record without it.
			faceStale.push_back(head);
			return;
		}
		faceHeads.erase(it);
		if (FaceSnapshots::Enabled())
			FaceSnapshots::Get().Release(head);
	}

	void SceneStore::ClearFaceShapes()
	{
		faceHeads.clear();
		facePublished.clear();
		faceStale.clear();
		if (FaceSnapshots::Enabled())
			FaceSnapshots::Get().ReleaseAll();
	}

	void SceneStore::ApplyFacePublications()
	{
		// A head's record changed shape (a writer found a shape's vertex count changed, or a shape left): every shape of it is
		// written again, the first of which rebuilds the record.
		for (const auto* head : faceStale) {
			const auto it = faceHeads.find(head);
			if (it == faceHeads.end())
				continue;
			for (auto* geometry : it->second)
				if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
					Schedule(entry->first, entry->second, true);
					++delta.faceWritten;
				}
		}
		faceStale.clear();
		// A new snapshot of a head: its kept streams take it in place; a shape that waits for one is written.
		auto& snapshots = FaceSnapshots::Get();
		delta.facePublished += facePublished.size();
		for (const auto* head : facePublished) {
			const auto it = faceHeads.find(head);
			if (it == faceHeads.end())
				continue;
			std::shared_ptr<const FaceSnapshots::HeadView> headView;
			bool headTaken = false;
			for (auto* geometry : it->second) {
				const auto entry = tracked.find(geometry);
				if (entry == tracked.end())
					continue;
				auto& tracked_ = entry->second;
				const std::uint32_t slot = tracked_.slot;
				const std::uint32_t index = slot != kNoObjectSlot && slot < tables.faceStream.size() ? tables.faceStream[slot] : kNoFaceStream;
				if (index == kNoFaceStream) {
					if (tracked_.faceWaiting) {
						Schedule(entry->first, tracked_, true);
						++delta.faceWritten;
					}
					continue;
				}
				auto& stream = tables.faceStreams[index];
				auto view = snapshots.View(geometry, head);
				if (!view.positions || view.vertexCount != stream.vertexCount) {
					Schedule(entry->first, tracked_, true);
					++delta.faceWritten;
					continue;
				}
				if (!headTaken) {
					headTaken = true;
					if (auto snapshot = snapshots.HeadSnapshot(*head); snapshot.recordId && snapshot.generation && snapshot.owner)
						headView = std::make_shared<const FaceSnapshots::HeadView>(std::move(snapshot));
				}
				stream.positions = view.positions;
				stream.generation = view.generation;
				stream.headView = headView && headView->generation == view.generation && headView->owner == view.owner ? headView : nullptr;
				stream.owner = std::move(view.owner);
				++delta.faceUpdated;
			}
		}
		facePublished.clear();
	}

	std::uint32_t SceneStore::FaceRegionOf(const RE::BSGeometry* a_geometry, std::uint32_t a_vertexCount)
	{
		auto& region = faceRegions[a_geometry];
		if (region.count != a_vertexCount) {
			// The old region retires: an installed publication's streams may still name it (the reflection's, a frame behind).
			if (region.count)
				tables.Retire(Tables::kRetiredFaceRegion, region.first, region.count);
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
		return region.first;
	}

	void SceneStore::ClearFaceRegions()
	{
		faceRegions.clear();
		faceRegionFree.clear();
		faceRegionTop = 0;
		tables.faceStreamsReleased.clear();
	}

	void SceneStore::EndFaceWalk()
	{
		// The regions of the streams freed since: free when no stream of the shape holds the region any more (a shape written
		// to another slot holds it through its new stream). Not the dense walk's (walk parity), whose tables are put back.
		for (const auto& [geometry, first] : denseWalk ? decltype(tables.faceStreamsReleased){} : tables.faceStreamsReleased) {
			const auto it = faceRegions.find(geometry);
			if (it == faceRegions.end() || it->second.first != first)
				continue;
			if (const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry)); entry != tracked.end()) {
				const std::uint32_t slot = entry->second.slot;
				if (slot != kNoObjectSlot && slot < tables.faceStream.size() && tables.faceStream[slot] != kNoFaceStream &&
					tables.faceStreams[tables.faceStream[slot]].region == first)
					continue;
			}
			tables.Retire(Tables::kRetiredFaceRegion, it->second.first, it->second.count);
			faceRegions.erase(it);
		}
		tables.faceStreamsReleased.clear();
		// Sorted and coalesced, so a run of freed shapes is one range again (the retirement chain gives them back: RecycleRetired).
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
		if (FaceSnapshots::Enabled())
			FaceSnapshots::Get().BeginWalk(facePublished, faceStale);
		stats.ineligible.fill(0);
		stats.techniqueRejects.fill(0);
		stats.propertyRejects.clear();
		stats.rejectedBlended = stats.rejectedOpaque = stats.rejectedOpaqueAlphaTest = 0;
		stats.shadowMaskPipelines = 0;
		stats.derivationChecked = stats.derivationDiffers = stats.derivationBits = stats.derivationNative = 0;
		stats.derivationFadeBits = stats.lodFadeChecked = stats.lodMetricDiffers = stats.lodFadeDiffers = 0;
		stats.lodFadeFirst.clear();
		lodFadeSampled = false;
		stats.derivationRuntimeDiffers = stats.derivationRuntimeBits = 0;
		stats.derivationBitCounts.fill(0);
		stats.materialsEvaluated = 0;
		stats.materialDiffMask = 0;
		stats.materialsValidated = stats.materialCacheStale = 0;
		stats.classifyHits = stats.classifyChecked = stats.classifyDiffers = stats.castResolved = 0;
		stats.derivedHits = stats.derivedChecked = stats.derivedDiffers = 0;
		stats.accumulatedWithoutRecord = 0;
		stats.decals = {};
		stats.skinned = stats.boneRows = 0;
		stats.projectedUV = stats.landBlend = 0;
		stats.shadowCasters = 0;
		stats.shadowRejects = {};

		skinnedObjects.clear();

		// Every per-object container, not only three of them. The three that were left out reallocated
		// their way back up every frame, and objectIndex - cleared just above - rehashed its way up to
		// ~2900 entries in the Whiterun exterior, which the profile billed to `record`.
		if (!keepObjects) {
			tables.objects.reserve(tracked.size());
			tables.objectGeometry.reserve(tracked.size());
			tables.draws.reserve(tracked.size());
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
		for (auto& [geometry, trackedEntry, unused, layer] : order) {
			// Per TRACKED object: for a rejected one this is its `continue` and the iteration itself.
			timer.Add(BuildPart::LoopTail);
			Ineligible bucket = Ineligible::Count;
			WriteObject(geometry, *trackedEntry, timer, bucket);
		}

		SweepObjectSlots();

		EndFaceWalk();
		tables.InvalidateChangeLog();
	}

	namespace
	{
		/** @brief An object's draw template (Tables::draws) for its geometry slot's record. */
		DrawSequence DrawTemplateOf(const GeometryRecord& a_geometry, std::uint32_t a_pipeline)
		{
			DrawSequence draw{};
			draw.pipelineIndex = a_pipeline;
			draw.vertexBufferAddress = a_geometry.vertexAddress;
			draw.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
			draw.vertexStride = a_geometry.vertexStride;
			// The second stream repeats the first; the epochs replace it with a face shape's positions.
			draw.streamBufferAddress = draw.vertexBufferAddress;
			draw.streamBufferSize = draw.vertexBufferSize;
			draw.streamStride = draw.vertexStride;
			draw.indexBufferAddress = a_geometry.indexAddress;
			draw.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.indexBytes, UINT32_MAX));
			draw.indexFormat = kIndexFormatR16;
			draw.indexCount = a_geometry.indexCount;
			draw.instanceCount = 1;
			draw.firstIndex = a_geometry.firstIndex;
			draw.vertexOffset = 0;
			draw.firstInstance = 0;
			return draw;
		}
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
		if (trackedEntry->faceShape && !trackedEntry->faceHead)
			ListFaceShape(geometry, *trackedEntry);
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
		// What a classification reads has events (properties, fades, hidden bits, attach and detach), each of which writes the
		// entry in full; CS_DCLF_INPUT_WATCH reads the inputs again regardless, and counts a change no event announced.
		if (cached && rereads && SwitchEnabled(Switch::InputWatch)) {
			const bool changed = ClassifyInputsOf(*geometry) != trackedEntry->classifyInputs;
			NoteInputReread(*trackedEntry, *geometry, 0, changed);
			if (changed) {
				cached = false;
				++delta.reread;
			}
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
				StoreInputComponents(*trackedEntry, *geometry);
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
		// Without a snapshot it waits for its head's publication (ApplyFacePublications), which writes it again.
		trackedEntry->faceWaiting = faceShape && !ResolveFace(*geometry, face, faceRegion);
		if (trackedEntry->faceWaiting)
			return false;
		// Geometry, shared between every object drawing the same TriShape. A skinned shape draws its
		// skin partitions' own buffers, one draw each (ClassifyStatic has checked every one).
		const auto* skinPartitions = data.skinInstance ? data.skinInstance->skinPartition.get() : nullptr;
		const RE::NiSkinPartition::Partition* skinPartition = skinPartitions ? &skinPartitions->partitions[0] : nullptr;
		// Which of them the engine draws, from the fade node's LOD level as both of its pass builders read it. A skin the
		// engine draws no partition of at this level stays a member that draws nothing (kNoPartitions).
		std::uint16_t partitionMask = SkinPartitionsOf(*geometry);
		auto* triShape = skinPartition ? skinPartition->buffData : data.rendererData;
		// Object LOD (dclf-lod.md): the ranges its hidden cells leave. Whole, the TriShape's own slot; none, a member that draws
		// nothing (kNoPartitions); else a chain of range slots (kPartitionChain), resolved again at each segment event.
		const auto* shapeRanges = geometry->GetType().get() == RE::BSGeometry::Type::kSubIndexTriShape ? LodRangesOf(geometry) : nullptr;
		const bool lodChain = shapeRanges && !shapeRanges->empty() && !LodSegments::Whole(geometry, *shapeRanges);
		if (shapeRanges && shapeRanges->empty())
			partitionMask = static_cast<std::uint16_t>(kNoPartitions);
		else if (lodChain)
			partitionMask = static_cast<std::uint16_t>(kPartitionChain | std::min<std::size_t>(shapeRanges->size(), kPartitionChainCount));
		const std::uint32_t geometrySlot = lodChain ? ResolveLodRangeSlots(*geometry, *shapeRanges, timer) : ResolveGeometrySlot(*geometry, triShape, skinPartition, timer);
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
			object.flags |= kObjectAlphaTest | (static_cast<std::uint32_t>(sceneAlpha->alphaThreshold) << kObjectAlphaThresholdShift) |
			                (sceneAlpha->GetAlphaBlending() ? kObjectAlphaBlended : 0u);

		// Skinning: the engine's own palette, whose rows FrameValues samples after running the engine's per-frame update (AE
		// FUN_140e4ff90) itself - what the bone setter runs from the native draw this object no longer gets. That update sizes the
		// palette from the skin data's bone count (numMatrices = skinData +0x58), so the block is placed by the bone count here
		// (step 6e F1: the scene work runs no engine code). A palette that is not that size is FrameValues' defect, counted.
		std::uint32_t objectBoneRows = 0;
		if (auto* skin = data.skinInstance.get(); skin && ActiveToggles().skinned) {
			timer.Add(BuildPart::Record);
			skinnedObjects.push_back(geometry);
			const std::uint32_t rows = SkinRowsOf(*skin);
			if (rows && rows <= 240) {
				objectBoneRows = rows;
				object.flags |= kObjectSkinned;
				++stats.skinned;
				stats.boneRows += rows;
			}
			timer.Add(BuildPart::Skinning);
		}
		// The object's slot: past the last `continue`, so a slot is only ever taken by a record that is written.
		const std::uint32_t slotBefore = trackedEntry->slot;
		const std::uint32_t objectId = AcquireObjectSlot(*trackedEntry, geometry);
		// The accumulated half is the membership's: a member written again for the same object keeps its binding (checked
		// against its witnesses when bound again). An entry written every frame (a face, an actor's part) would otherwise
		// lose its binding here and take it back there, every frame.
		const bool keepMember = wasMember && objectId == slotBefore && a_bucket == Ineligible::None && !shadowOnly &&
		                        !(tables.objects[objectId].flags & kObjectFree);
		if (wasMember && !keepMember)
			DropResidentSlot(slotBefore, false);
		const bool keepHalf = keepMember;
		tables.objectSeen[objectId] = walkSerial;
		if (objectBoneRows)
			tables.PlaceBones(objectId, objectBoneRows);
		else
			tables.FreeBones(objectId);
		// Whether the engine would draw this object into a shadow map, and with which Utility
		// technique. It belongs here and nowhere else: every shadow view is rendered between this
		// phase and the next, so a verdict taken later would arrive after the views that need it.
		const auto* shadowProperty = data.shaderProperty.get();
		const auto shadowReject = ShadowCasterReject(shadowProperty, geometry, FadeOnGpu(shadowProperty ? shadowProperty->fadeNode : nullptr));
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
		// The occlusion maps DCLF draws (Skylighting's sky map, the precipitation mask): whether and how this object draws into
		// each, by Skylighting's rule for that map. Its size test (a bound radius of 32 or less draws nothing) is the view's
		// BuildDraws' (kCullMinRadius), against the record's bound, so an animated actor's bound crossing it is no
		// classification input.
		// LOD (the land and object blocks under the LOD root) is no occluder of either map: Precipitation::SetupMask culls the scene
		// lists, which do not hold the LOD root, so the engine never draws it there.
		using LodFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		const bool lod = shadowProperty && shadowProperty->flags.any(LodFlag::kLODLandscape, LodFlag::kLODObjects, LodFlag::kHDLODObjects);
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			std::uint32_t occlusion = 0;
			if (OcclusionEnabled(v) && !lod)
				if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(shadowProperty))
					occlusion = Skylighting::OcclusionTechnique(lighting, geometry, v == kOcclusionSky, true);
			tables.occlusionTechnique[v][objectId] = occlusion;
			if (occlusion) {
				if (occlusion & 0x80)
					sampleDiffuse();
				useKey(tables.occlusionKeysUsed[v], occlusion);
			}
		}
		ResolveSunEntry(*trackedEntry, *geometry);
		tables.sunEntryNode[objectId] = trackedEntry->sunEntryNode;
		tables.hasFadeNode[objectId] = data.shaderProperty && data.shaderProperty->fadeNode ? 1 : 0;
		if (face.positions)
			PushFaceStream(*geometry, objectId, face, faceRegion);
		else
			tables.ClearFaceStream(objectId);
		tables.shadowDiffuse[objectId] = shadowDiffuse;
		tables.shadowMaterial[objectId] = shadowMaterial;
		tables.sceneFlags[objectId] = object.flags;
		tables.objectGeometry[objectId] = geometry;
		tables.objectIdentity[objectId] = trackedEntry->identity;
		tables.objectGroup[objectId] = trackedEntry->groupIdentity;
		// A member decal written again (an actor's part every frame, a subtree attached again) may have moved in the scene graph:
		// its place in the decal order is taken again (OrderDecals).
		if (memberDecals.contains(objectId))
			NoteDecalChanged(objectId);
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
			tables.lights[objectId] = ObjectLights{};
			tables.treeAnim[objectId] = ObjectTreeAnim{};
			tables.fadeDistance[objectId] = 0.0f;
		}
		tables.actorWetness.Set(objectId, trackedEntry->actorOwned ? trackedEntry->groupIdentity : 0,
			trackedEntry->identity);
		if (trackedEntry->actorOwned)
			tables.actorObjects.push_back(objectId);
		tables.skinPartitions[objectId] = partitionMask;
		if (!denseWalk) {
			trackedEntry->objectStamp = objectStamp;
			trackedEntry->objectId = objectId;
			// Scene membership: an eligible record written (entering the scene, or an event rewrote it) is bound again.
			if (a_bucket == Ineligible::None && !shadowOnly)
				bindQueue.push_back(objectId);
		}

		tables.draws[objectId] = DrawTemplateOf(tables.geometries[geometrySlot], keepHalf ? keptPipeline : 0);
		timer.Add(BuildPart::Record);
		WriteLayer(geometry, *trackedEntry, objectId, a_bucket == Ineligible::None && !shadowOnly, keepHalf, timer);
		return true;
	}

	void SceneStore::WriteLayer(RE::BSGeometry* a_geometry, Tracked& a_tracked, std::uint32_t a_base, bool a_member, bool a_keepMember, PartTimer& a_timer)
	{
		auto* property = a_member ? netimmerse_cast<RE::BSLightingShaderProperty*>(LayerPropertyOf(*a_geometry)) : nullptr;
		const std::uint32_t geometrySlot = property ? ResolveLayerGeometrySlot(*a_geometry, a_timer) : Tables::kSlotFree;
		if (geometrySlot == Tables::kSlotFree) {
			if (!denseWalk)
				ReleaseLayerSlot(a_tracked);
			return;
		}
		const std::uint32_t slotBefore = a_tracked.layerSlot;
		const std::uint32_t slot = AcquireLayerSlot(a_tracked, a_geometry, a_base);
		const bool same = !denseWalk && slot == slotBefore;
		const Tables::Columns columnsBefore = same ? tables.ColumnsOf(slot) : Tables::Columns{};
		// A layer joins and leaves with its base (BindByMembership, DropResidentSlot): its binding stands while the base's does.
		const bool keepMember = same && a_keepMember && IsResidentSlot(slot) && !(tables.objects[slot].flags & kObjectFree);
		if (!denseWalk && !keepMember && IsResidentSlot(slot))
			DropResidentSlot(slot, false);
		ObjectRecord object{};
		object.geometryIndex = geometrySlot;
		// The layer's draws apply no alpha property (render flags 0x41) and cast no shadow; two-sidedness is its property's.
		object.flags = kObjectNoBindings | kObjectNoShadow |
		               (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided) ? kObjectTwoSided : 0u);
		tables.objectSeen[slot] = walkSerial;
		tables.FreeBones(slot);
		tables.shadowTechnique[slot] = 0;
		tables.shadowReject[slot] = static_cast<std::uint8_t>(ShadowReject::Layer);
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			tables.occlusionTechnique[v][slot] = 0;
		tables.sunEntryNode[slot] = tables.sunEntryNode[a_base];
		tables.hasFadeNode[slot] = property->fadeNode ? 1 : 0;
		tables.ClearFaceStream(slot);
		tables.shadowDiffuse[slot] = nullptr;
		tables.shadowMaterial[slot] = nullptr;
		tables.sceneFlags[slot] = object.flags;
		tables.objectGeometry[slot] = a_geometry;
		tables.objectIdentity[slot] = a_tracked.identity;
		tables.objectGroup[slot] = a_tracked.groupIdentity;
		if (memberDecals.contains(slot))
			NoteDecalChanged(slot);
		tables.layerBase[slot] = a_base;
		tables.layerOf[a_base] = slot;
		const std::uint32_t keptPipeline = tables.draws[slot].pipelineIndex;
		if (keepMember) {
			const auto& kept = tables.objects[slot];
			object.flags = (object.flags & kSceneKeptFlags) | (kept.flags & ~kSceneKeptFlags);
			object.materialIndex = kept.materialIndex;
			object.pipelineIndex = kept.pipelineIndex;
			tables.objects[slot] = object;
		} else {
			tables.FreeExtras(slot);
			tables.objects[slot] = object;
			tables.lights[slot] = ObjectLights{};
			tables.treeAnim[slot] = ObjectTreeAnim{};
			tables.fadeDistance[slot] = 0.0f;
		}
		tables.actorWetness.Set(slot, 0, a_tracked.identity);
		tables.skinPartitions[slot] = 0;
		tables.draws[slot] = DrawTemplateOf(tables.geometries[geometrySlot], keepMember ? keptPipeline : 0);
		if (denseWalk)
			return;
		if (same)
			tables.NoteWrite(slot, columnsBefore);
		else
			tables.NoteChange(slot, kChangeAll);
		shadowSetsDirty = true;
		shadowDirtySlots.push_back(slot);
		bindQueue.push_back(slot);
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
		const auto& shape = static_cast<RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData();
		GeometrySource source;
		source.key = a_triShape;
		source.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->vertexBuffer);
		source.indexBuffer = reinterpret_cast<ID3D11Buffer*>(a_triShape->indexBuffer);
		source.vertexDesc = std::bit_cast<std::uint64_t>(a_triShape->vertexDesc);
		source.vertexCount = a_skinPartition ? a_skinPartition->vertices : shape.vertexCount;
		source.indexCount = static_cast<std::uint32_t>(a_skinPartition ? a_skinPartition->triangles : shape.triangleCount) * 3;
		return ResolveGeometrySource(source, a_timer);
	}

	std::uint32_t SceneStore::ResolveLodRangeSlots(RE::BSGeometry& a_geometry, const std::vector<LodSegments::Range>& a_ranges, PartTimer& a_timer)
	{
		// The draw (FUN_1414f2ad0, type 8): the renderer data's buffers, DrawIndexed(count, first, 0) per range.
		const auto* triShape = a_geometry.GetGeometryRuntimeData().rendererData;
		if (!triShape || a_ranges.empty())
			return Tables::kSlotFree;
		std::uint32_t first = Tables::kSlotFree, previous = Tables::kSlotFree;
		for (const auto& range : a_ranges) {
			GeometrySource source;
			source.key = reinterpret_cast<const RE::BSGraphics::TriShape*>(range.key);
			source.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer);
			source.indexBuffer = reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer);
			source.vertexDesc = std::bit_cast<std::uint64_t>(triShape->vertexDesc);
			source.vertexCount = static_cast<RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData().vertexCount;
			source.indexCount = range.indexCount;
			source.firstIndex = range.firstIndex;
			const std::uint32_t slot = range.key ? ResolveGeometrySource(source, a_timer) : Tables::kSlotFree;
			if (slot == Tables::kSlotFree)
				return Tables::kSlotFree;
			if (previous == Tables::kSlotFree) {
				first = slot;
			} else if (tables.geometries[previous].nextPartition != slot) {
				tables.geometries[previous].nextPartition = slot;
				tables.NoteGeometry(previous);
			}
			previous = slot;
		}
		if (tables.geometries[previous].nextPartition != kNoPartition) {
			tables.geometries[previous].nextPartition = kNoPartition;
			tables.NoteGeometry(previous);
		}
		return first;
	}

	std::uint32_t SceneStore::ResolveLayerGeometrySlot(RE::BSGeometry& a_geometry, PartTimer& a_timer)
	{
		// The layer's draw (FUN_140e465d0 for a hint-12 pass): the renderer data's vertex buffer, the second index list
		// (altIndexBuffer, whose first qword is its ID3D11Buffer) as R16, altPrimCount triangles from index 0.
		const auto* triShape = a_geometry.GetGeometryRuntimeData().rendererData;
		const auto& data = static_cast<RE::BSMultiIndexTriShape&>(a_geometry).GetMultiIndexTrishapeRuntimeData();
		auto* indexBuffer = data.altIndexBuffer ? *reinterpret_cast<ID3D11Buffer* const*>(data.altIndexBuffer) : nullptr;
		if (!triShape || !indexBuffer || !data.altPrimCount)
			return Tables::kSlotFree;
		GeometrySource source;
		source.key = reinterpret_cast<const RE::BSGraphics::TriShape*>(data.altIndexBuffer);
		source.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer);
		source.indexBuffer = indexBuffer;
		source.vertexDesc = std::bit_cast<std::uint64_t>(triShape->vertexDesc);
		source.vertexCount = static_cast<RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData().vertexCount;
		source.indexCount = data.altPrimCount * 3;
		source.layer = true;
		return ResolveGeometrySource(source, a_timer);
	}

	void SceneStore::PrefetchGeometryBuffers(std::size_t a_first)
	{
		// The buffers the round's geometry slots will resolve (ResolveGeometrySource), named to GpuResources at once: one
		// synchronization with DXVK's worker thread for the round rather than one per new buffer (the cell loads' cost). An entry
		// the light path keeps resolves nothing; one this misses still resolves, alone.
		if (!frameResolveBuffers)
			return;
		ZoneScopedN("CS.DCLF.Scene.PrefetchBuffers");
		std::vector<ID3D11Buffer*> buffers;
		auto add = [&](const RE::BSGraphics::TriShape* a_shape) {
			if (!a_shape)
				return;
			buffers.push_back(reinterpret_cast<ID3D11Buffer*>(a_shape->vertexBuffer));
			buffers.push_back(reinterpret_cast<ID3D11Buffer*>(a_shape->indexBuffer));
		};
		for (std::size_t i = a_first; i < order.size(); ++i) {
			// A per-frame entry is placed, not written (a selected switch child or an actor's record resolves alone), and one the
			// last classification left without a record has nothing to resolve unless an event changed it, which is rare.
			const Tracked& entry = *order[i].tracked;
			if (entry.perFrame || (entry.slot == kNoObjectSlot && entry.candidateFrame != 0 && entry.candidateReason != Ineligible::None))
				continue;
			auto* geometry = order[i].geometry;
			const auto& data = geometry->GetGeometryRuntimeData();
			if (const auto* partitions = data.skinInstance ? data.skinInstance->skinPartition.get() : nullptr) {
				for (std::uint32_t p = 0; p < partitions->numPartitions; ++p)
					add(partitions->partitions[p].buffData);
			} else {
				add(data.rendererData);
			}
			if (geometry->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape) {
				const auto& multi = static_cast<RE::BSMultiIndexTriShape*>(geometry)->GetMultiIndexTrishapeRuntimeData();
				if (multi.altIndexBuffer)
					buffers.push_back(*reinterpret_cast<ID3D11Buffer* const*>(multi.altIndexBuffer));
			}
		}
		GpuResources::Get().Prefetch(buffers);
	}

	std::uint32_t SceneStore::ResolveGeometrySource(const GeometrySource& a_source, PartTimer& a_timer)
	{
		auto& gpu = GpuResources::Get();
		const bool resolveBuffers = frameResolveBuffers;
		auto geometryIt = geometryIndex.find(a_source.key);
		const bool newGeometry = geometryIt == geometryIndex.end();
		// A slot found by address but describing other buffers is a TriShape reallocated at the same
		// address: it is resolved again into the same slot. So is one resolved while the render graph was off.
		const bool staleGeometry = !newGeometry &&
		                           ((resolveBuffers && tables.geometries[geometryIt->second].vertexAddress == 0) ||
		                               tables.geometries[geometryIt->second].firstIndex != a_source.firstIndex ||
		                               tables.geometries[geometryIt->second].indexCount != a_source.indexCount ||
		                               (resolveBuffers && (!tables.geometryImports[geometryIt->second].vertexOwner || !tables.geometryImports[geometryIt->second].indexOwner)) ||
		                               tables.geometries[geometryIt->second].vertexBuffer != a_source.vertexBuffer ||
		                               tables.geometries[geometryIt->second].indexBuffer != a_source.indexBuffer);
		if (staleGeometry)
			++stats.geometriesRefreshed;
		if (newGeometry || staleGeometry) {
			// The buffers are resolved once per TRISHAPE, not once per object. The slot's leases keep them resolved
			// (GpuResources) for as long as the slot lives.
			std::optional<GpuResources::LeasedBuffer> vertexLease, indexLease;
			if (resolveBuffers) {
				// The render graph reads the game's buffers in place; they must never move (GpuResources).
				vertexLease = gpu.Acquire(a_source.vertexBuffer);
				if (vertexLease)
					indexLease = gpu.Acquire(a_source.indexBuffer);
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
			GeometryRecord record;
			record.vertexBuffer = a_source.vertexBuffer;
			record.indexBuffer = a_source.indexBuffer;
			record.vertexDesc = a_source.vertexDesc;
			// The stride the engine binds is the desc's low nibble in dwords (engine notes: vertex input).
			record.vertexStride = static_cast<std::uint32_t>(record.vertexDesc & 0xFu) * 4u;
			record.vertexCount = a_source.vertexCount;
			record.indexCount = a_source.indexCount;
			record.firstIndex = a_source.firstIndex;
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
			// A stale slot's old leases outlive the frames in flight (TakeRetiredImports).
			for (auto* owner : { &tables.geometryImports[slot].vertexOwner, &tables.geometryImports[slot].indexOwner })
				if (*owner)
					retiredImports.push_back(std::move(*owner));
			tables.geometryImports[slot] = vertexLease && indexLease ? Tables::GeometryImport{ vertexLease->generation, indexLease->generation,
				vertexLease->owner, indexLease->owner } : Tables::GeometryImport{};
			tables.NoteGeometry(slot);
			tables.geometrySlotKey[slot] = a_source.key;
			tables.geometryLayerKey[slot] = a_source.layer ? 1 : 0;
			if (!staleGeometry)
				geometryIt = geometryIndex.emplace(a_source.key, slot).first;
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
			tables.shadowTechnique[a_slot], object.flags & (kObjectNoShadow | kObjectTwoSided | kObjectFree), tables.shadowReject[a_slot],
			{ tables.occlusionTechnique[kOcclusionSky][a_slot], tables.occlusionTechnique[kOcclusionPrecipitation][a_slot] } };
	}

	bool SceneStore::KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		auto& data = a_geometry->GetGeometryRuntimeData();
		auto* skin = data.skinInstance.get();
		const std::uint32_t slot = a_tracked.slot;
		if (!skin || !ActiveToggles().skinned || !(tables.objects[slot].flags & kObjectSkinned))
			return false;
		// The size the engine's palette update gives it (the skin data's bone count); FrameValues checks it after this frame's.
		const std::uint32_t rows = SkinRowsOf(*skin);
		if (!rows || rows > 240 || rows != tables.boneRows[slot])
			return false;
		skinnedObjects.push_back(a_geometry);
		++stats.lightSkins;
		const std::uint16_t partitionMask = SkinPartitionsOf(*a_geometry);
		if (tables.skinPartitions[slot] != partitionMask)
			tables.NoteChange(slot, kChangeSkin | kChangeStructure);
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

	std::array<std::uint64_t, 12> SceneStore::InputComponentsOf(const RE::BSGeometry& a_geometry)
	{
		std::array<std::uint64_t, 12> c{};
		const auto& data = a_geometry.GetGeometryRuntimeData();
		c[0] = reinterpret_cast<std::uintptr_t>(data.rendererData);
		c[1] = reinterpret_cast<std::uintptr_t>(data.skinInstance.get());
		c[2] = reinterpret_cast<std::uintptr_t>(data.skinInstance ? data.skinInstance->skinPartition.get() : nullptr);
		c[3] = a_geometry.worldBound.radius <= 32.0f ? 1u : 0u;
		const auto* property = data.shaderProperty.get();
		c[4] = reinterpret_cast<std::uintptr_t>(property);
		if (property) {
			c[5] = property->flags.underlying();
			c[6] = reinterpret_cast<std::uintptr_t>(property->material);
			c[7] = FadeStateOf(const_cast<RE::BSShaderProperty*>(property));
			if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property); lighting && property->material) {
				const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
				c[8] = std::bit_cast<std::uint32_t>(material->materialAlpha);
				const auto* texture = material->diffuseTexture ? material->diffuseTexture->rendererTexture : nullptr;
				c[11] = reinterpret_cast<std::uintptr_t>(texture ? texture->resourceView : nullptr);
			}
		}
		if (const auto* alpha = data.alphaProperty.get()) {
			c[9] = reinterpret_cast<std::uintptr_t>(alpha);
			c[10] = (std::uint64_t(alpha->alphaFlags) << 8) | alpha->alphaThreshold;
		}
		return c;
	}

	void SceneStore::StoreInputComponents(Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		if (!SwitchEnabled(Switch::InputWatch))
			return;
		if (!a_tracked.inputComponents)
			a_tracked.inputComponents = std::make_unique<std::array<std::uint64_t, 12>>();
		*a_tracked.inputComponents = InputComponentsOf(a_geometry);
	}

	void SceneStore::NoteInputReread(Tracked& a_tracked, const RE::BSGeometry& a_geometry, std::uint32_t a_kind, bool a_changed)
	{
		if (!SwitchEnabled(Switch::InputWatch))
			return;
		++stats.inputRereads[a_kind];
		if (!a_changed)
			return;
		++stats.inputRereadsChanged[a_kind];
		const auto now = InputComponentsOf(a_geometry);
		if (!a_tracked.inputComponents)
			return;
		std::string changed;
		for (std::size_t i = 0; i < now.size(); ++i)
			if (now[i] != (*a_tracked.inputComponents)[i]) {
				++stats.inputChanged[i];
				if (stats.firstInputChange.empty())
					changed += fmt::format("{}{} {:#x} -> {:#x}", changed.empty() ? "" : ", ", kInputComponentNames[i], (*a_tracked.inputComponents)[i], now[i]);
			}
		if (stats.firstInputChange.empty() && !changed.empty())
			stats.firstInputChange = fmt::format("'{}' ({}): {}", a_geometry.name.c_str() ? a_geometry.name.c_str() : "?", a_kind ? "shading" : "classify", changed);
		*a_tracked.inputComponents = now;
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
		// Not the bound: Skylighting's size test is the GPU's (kCullMinRadius).
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
		// External emittance: the shared colour the property reads (a light's, a region's or the sky's), which the weather
		// rewrites in place (emittanceEvents). Its re-point is a property event (SetExternalEmittance).
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(data.shaderProperty.get());
		const void* emittance = lighting && lighting->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance) ? lighting->emissiveColor : nullptr;
		if (emittance != a_tracked.listedEmittance) {
			if (a_tracked.listedEmittance)
				Unlist(propertyDependents, a_tracked.listedEmittance, a_geometry);
			if (emittance)
				propertyDependents[emittance].push_back(a_geometry);
			a_tracked.listedEmittance = emittance;
		}
		// A multi-index shape's layer property (LayerPropertyOf): its events classify the entry again too.
		const void* layer = LayerPropertyOf(*a_geometry);
		if (layer != a_tracked.listedLayerProperty) {
			if (a_tracked.listedLayerProperty)
				Unlist(propertyDependents, a_tracked.listedLayerProperty, a_geometry);
			if (layer)
				propertyDependents[layer].push_back(a_geometry);
			a_tracked.listedLayerProperty = layer;
		}
	}

	void SceneStore::UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root)
	{
		if (a_tracked.listedProperty)
			Unlist(propertyDependents, a_tracked.listedProperty, a_geometry);
		if (a_tracked.listedAlpha)
			Unlist(propertyDependents, a_tracked.listedAlpha, a_geometry);
		if (a_tracked.listedEmittance)
			Unlist(propertyDependents, a_tracked.listedEmittance, a_geometry);
		if (a_tracked.listedLayerProperty)
			Unlist(propertyDependents, a_tracked.listedLayerProperty, a_geometry);
		a_tracked.listedProperty = nullptr;
		a_tracked.listedAlpha = nullptr;
		a_tracked.listedEmittance = nullptr;
		a_tracked.listedLayerProperty = nullptr;
		if (a_root)
			UnlistHiddenChain(a_geometry, a_tracked);
		if (a_root && a_tracked.lightRoot) {
			MarkLightEntryDirty(a_tracked.lightRoot);
			// A candidate losing a geometry may be on its way out of the scene (a cell unloading frees the entry after its
			// geometries): the snapshots that name it are stale now, not at the next walk, since the point lights' selection
			// (LocalLightCull::SelectFrame) reads their entries' nodes.
			if (lightCandidateSet.contains(a_tracked.lightRoot))
				++lightCandidatesGeneration;
			Unlist(lightDependents, a_tracked.lightRoot, a_geometry);
			ReleaseRootOwner(a_tracked.lightRoot);
			a_tracked.lightRoot = nullptr;
		}
		if (a_root && a_tracked.listedRoot) {
			MarkSunEntryDirty(a_tracked.listedRoot);
			// The others under it: its bound takes this one in no more. The node is a key here; it may be gone.
			if (Unlist(rootDependents, a_tracked.listedRoot, a_geometry)) {
				dirtyRoots.push_back(a_tracked.listedRoot);
			} else {
				rootMotion.erase(a_tracked.listedRoot);
				movingRoots.erase(a_tracked.listedRoot);
				if (const auto reference = rootReference.find(a_tracked.listedRoot); reference != rootReference.end()) {
					if (const auto back = referenceRoot.find(reference->second); back != referenceRoot.end() && back->second == a_tracked.listedRoot)
						referenceRoot.erase(back);
					rootReference.erase(reference);
				}
			}
			ReleaseRootOwner(a_tracked.listedRoot);
			a_tracked.listedRoot = nullptr;
		}
	}

	void SceneStore::OwnRoot(const RE::NiAVObject* a_root)
	{
		if (auto& owner = rootOwners[a_root]; !owner)
			owner.reset(const_cast<RE::NiAVObject*>(a_root));
	}

	void SceneStore::ReleaseRootOwner(const RE::NiAVObject* a_root)
	{
		if (!a_root || rootDependents.contains(a_root) || lightDependents.contains(a_root))
			return;
		if (const auto it = rootOwners.find(a_root); it != rootOwners.end()) {
			HandBack(std::move(it->second));
			rootOwners.erase(it);
		}
	}

	void SceneStore::ReleaseRootOwners()
	{
		for (auto& [root, owner] : rootOwners)
			HandBack(std::move(owner));
		rootOwners.clear();
	}

	RE::NiPointer<RE::NiAVObject> SceneStore::OwnedRoot(const RE::NiAVObject* a_root)
	{
		if (!a_root)
			return nullptr;
		if (const auto it = rootOwners.find(a_root); it != rootOwners.end())
			return it->second;
		if (unownedRoots++ == 0)
			logger::error("[DCLF] a root no dependents list names was handed out (step 6e F1: its reference would be made from a key); left out");
		return nullptr;
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
		const bool perFrame = (mayRecord && traits && traits != kTraitRootMoves && traits != kTraitFace) || (traits & kTraitActor) ||
		                      (frameVerdict && (traits & kTraitMoves));
		// The light path takes only a record's placement, palette, switch selection or shading, and for an actor's the exact
		// recheck of its verdict inputs (ActorRecordKept); a face shape's positions follow its head's publications
		// (ApplyFacePublications), not the walk. An entry it cannot have a record for is written in full, which re-reads its
		// verdict.
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
		if (SwitchEnabled(Switch::InputWatch)) {
			const bool changed = ClassifyInputsOf(a_geometry) != a_tracked.classifyInputs;
			NoteInputReread(a_tracked, a_geometry, 0, changed);
			if (changed) {
				++delta.reread;
				return false;
			}
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
		// CS_DCLF_HIDDEN_WATCH: the chain's nearest four nodes, for the store that flips them next.
		if (now != cached && !announced && HiddenWatch::Enabled()) {
			std::array<const RE::NiAVObject*, 4> nodes{};
			for (std::size_t i = 0; i < nodes.size() && i < a_tracked.hiddenChain.size(); ++i)
				nodes[i] = static_cast<const RE::NiAVObject*>(a_tracked.hiddenChain[i]);
			HiddenWatch::Arm(nodes);
		}
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
				if (kept && (entry.lightTraits & (kTraitAnimatedShading | kTraitActor)) && SwitchEnabled(Switch::InputWatch)) {
					kept = ShadingInputsOf(entry, *geometry) == entry.shadingInputs;
					NoteInputReread(entry, *geometry, 1, !kept);
				}
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
					// An actor's skeleton moves without a controller or a body on its chain (Havok's behaviour graph). Every
					// recorded mover is FrameValues' each frame (its placement and palette).
					if (recorded && (entry.lightTraits & (kTraitMoves | kTraitRootMoves | kTraitSkin | kTraitActor))) {
						WalkPlan().movers.push_back(PlanItemOf(geometry, entry));
						++stats.lightPlaced;
						++delta.moved;
					} else {
						++delta.kept;
					}
					entry.movedWalk = walkSerial;  // not written in full: a later round may still write it
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
			// What the write changed (Tables::changeLog); a slot taken anew is new in everything. Its row is FrameValues' next frame.
			if (written && entry.slot != kNoObjectSlot) {
				WalkPlan().written.push_back(PlanItemOf(geometry, entry));
				if (entry.slot == slotBefore)
					tables.NoteWrite(entry.slot, columnsBefore);
				else
					tables.NoteChange(entry.slot, kChangeAll);
			}
			// What its sun entry's candidacy reads (SunEntryAllows).
			if (hadSlot != (entry.slot != kNoObjectSlot) || reasonBefore != entry.candidateReason) {
				MarkSunEntryDirty(entry.sunEntryNode);
				MarkLightEntryDirty(entry.lightRoot);
			}
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
				if (entry.lightTraits & (kTraitAnimatedShading | kTraitActor)) {
					entry.shadingInputs = ShadingInputsOf(entry, *geometry);
					StoreInputComponents(entry, *geometry);
				}
				ListFadeDependent(geometry, entry);
				// A static's previous transform is its current one from its second update on; until then it is
				// written again.
				if (!entry.perFrame && !SameTransform(geometry->world, DrawnPreviousWorld(*geometry))) {
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
		auto keptOcclusionKeys = std::move(tables.occlusionKeysUsed);
		shadowSetsDirty = full || shadowSetsDirty;
		BeginWalk(!full);
		PartTimer timer(stats.partMs);
		if (full) {
			// Everything, as the full walk: after a reset, a load or a live toggle nothing kept can be trusted.
			fullEvaluation = false;
			facePublished.clear();
			faceStale.clear();
			++delta.full;
			perFrameSet.clear();
			pendingEvaluation.clear();
			fadeChanged.clear();
			fadeDependents.clear();
			propertyChanged.clear();
			HandBack(nodeChanged);
			dirtyRoots.clear();
			propertyDependents.clear();
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
			ApplyFacePublications();
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
			// Their references are the render thread's to drop (ReleaseHandedBack).
			HandBack(nodeChanged);
			count(delta.node);
			std::sort(dirtyRoots.begin(), dirtyRoots.end());
			dirtyRoots.erase(std::unique(dirtyRoots.begin(), dirtyRoots.end()), dirtyRoots.end());
			for (const auto* root : dirtyRoots)
				ScheduleRoot(root);
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
			PrefetchGeometryBuffers(0);
			EvaluateRound(timer, 0);
		}
		if (!shadowSetsDirty) {
			tables.shadowTextureSet = std::move(keptTextureSet);
			tables.shadowTextureSeen = std::move(keptTextureSeen);
			tables.shadowKeysUsed = std::move(keptKeys);
			tables.occlusionKeysUsed = std::move(keptOcclusionKeys);
		}
		FinishDeltaWalk(timer);
		if (full) {
			DCLF_SCENE_PART(ObjectSweep, "CS.DCLF.Scene.ObjectSweep");
			SweepObjectSlots();
		}
		{
			DCLF_SCENE_PART(SunCandidates, "CS.DCLF.Scene.SunCandidates");
			UpdateSunCandidates(full);
			UpdateLightCandidates(full);
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
				const std::uint32_t links = (tables.skinPartitions[s] & kPartitionChain) ? (tables.skinPartitions[s] & kPartitionChainCount) : kMaxSkinPartitions;
				for (std::uint32_t link = 0; !stale && link < links && g != kNoPartition; ++link) {
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
					tables.RetireObject(s);
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
		tables.liveObjects = tables.CountLive();
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
			const auto occlusionKeys = tables.occlusionKeysUsed;
			const auto casters = stats.shadowCasters;
			const auto rejects = stats.shadowRejects;
			std::uint32_t staleInputs = 0;
			for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
				if (slot >= shadowIndexedInputs.size() || shadowIndexedInputs[slot] != ShadowInputsOf(slot) ||
					(slot < shadowIndexedLive.size() && (shadowIndexedLive[slot] != 0) != !(tables.objects[slot].flags & kObjectFree)))
					++staleInputs;
			RefreshShadowSets(true);
			if (textures != tables.shadowTextureSet || shadowKeys != tables.shadowKeysUsed || occlusionKeys != tables.occlusionKeysUsed ||
				casters != stats.shadowCasters || rejects != stats.shadowRejects)
				logger::error("[DCLF] incremental shadow dependencies disagreed with same-table full reconstruction (frame {}, stale inputs {}, textures {}->{}, shadow keys {}->{}, sky keys {}->{}, precipitation keys {}->{}, casters {}->{}, rejects {}->{})",
					frame, staleInputs, textures.size(), tables.shadowTextureSet.size(), shadowKeys.size(), tables.shadowKeysUsed.size(), occlusionKeys[kOcclusionSky].size(),
					tables.occlusionKeysUsed[kOcclusionSky].size(), occlusionKeys[kOcclusionPrecipitation].size(), tables.occlusionKeysUsed[kOcclusionPrecipitation].size(),
					casters, stats.shadowCasters, rejects[0], stats.shadowRejects[0]);
		}
		keptShadowCasters = stats.shadowCasters;
		keptShadowRejects = stats.shadowRejects;
		a_timer.Add(BuildPart::Record);
	}
}
