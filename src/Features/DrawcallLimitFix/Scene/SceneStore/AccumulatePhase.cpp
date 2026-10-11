#include "Internal.h"
#include "Features/DrawcallLimitFix/Common/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"

#include <cassert>

#include "Features/ExtendedTranslucency.h"
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
		bool BeastRaceFace(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
		{
			// The records' (T6b1b): the geometry's reference's answer is its node record's (CaptureNode: BeastOf).
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			const auto& sss = globals::features::subsurfaceScattering;
			if (!sss.loaded || !sss.isBeastRaceKeyword || !(a_property.flags & (static_cast<std::uint64_t>(Flag::kFace) | static_cast<std::uint64_t>(Flag::kFaceGenRGBTint))))
				return false;
			return a_leaf.node->beast;
		}

		/** @brief ExtendedTranslucency::MaterialModelOf, from the records (T6b1b). */
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

	const RE::BSRenderPass* SceneStore::RegisteredTemplatePassOf(RE::BSShaderProperty* a_property) const
	{
		const auto* pass = a_property ? FindLightingPass(a_property) : nullptr;
		return pass ? pass : frameLightingPass;
	}

	// T6b2b: the parity's alone (RefreshFrameConstants' persistent-parity reference); the blocks are GeometryPort's. With it, outside
	// parity nothing reads Tables::geometryTemplateObject, syntheticTemplate or templateLights (droppable once the parity is retired;
	// geometryTemplate also feeds RegisteredTemplatePassOf, the T6 check, and the slot check's "pipeline without a template").
	const RE::BSRenderPass* SceneStore::TemplatePassOf(const Tables& a_view, std::uint32_t a_pipeline)
	{
		auto* property = a_pipeline < a_view.geometryTemplate.size() ? a_view.geometryTemplate[a_pipeline] : nullptr;
		const std::uint32_t object = a_pipeline < a_view.geometryTemplateObject.size() ? a_view.geometryTemplateObject[a_pipeline] : kNoObjectSlot;
		const auto* geometry = object < a_view.objectGeometry.size() && object < a_view.objects.size() && !(a_view.objects[object].flags & kObjectFree) ?
		                           a_view.objectGeometry[object] :
		                           nullptr;
		auto* shader = ConstantEvaluator::Get().GetLightingShader();
		auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		if (!property || !geometry || !shader || !sceneNode)
			return nullptr;
		auto& runtime = sceneNode->GetRuntimeData();
		RE::BSLight* sun = runtime.sunLight;
		RE::BSLight* shadow = runtime.sunShadowDirLight;
		if (!shadow)
			for (const auto& light : runtime.activeShadowLights)
				if ((shadow = light.get()))
					break;
		if (!sun || !sun->light)
			return nullptr;
		templateLights.fill(shadow && shadow->light ? shadow : sun);
		templateLights[0] = sun;
		syntheticTemplate = {};
		syntheticTemplate.shader = shader;
		syntheticTemplate.shaderProperty = property;
		syntheticTemplate.geometry = const_cast<RE::BSGeometry*>(geometry);
		// The sun alone, as the engine's main passes carry it (PS NumLightNumShadowLight is the pass's light count less the sun,
		// with Light Limit Fix's lights the GPU's); the rest of the array only keeps SetupGeometry's descriptor-count reads in bounds.
		syntheticTemplate.numLights = 1;
		syntheticTemplate.numShadowLights = 0;
		syntheticTemplate.sceneLights = templateLights.data();
		return &syntheticTemplate;
	}

	void SceneStore::CaptureLightingShader()
	{
		// The BSLightingShader instance (T6): the engine's own (0x14338ca00, written by its constructor; the shader GetRenderPasses gives
		// a Lighting pass), not learned from a registration. Read at the frame's start with the coordinator idle, before the frame's
		// globals (MaterialPort's sources read it) and the scene work's kick, so the scene lane reads only what was set before its kick.
		auto& evaluator = ConstantEvaluator::Get();
		if (evaluator.HasLightingShader())
			return;
		static const REL::Relocation<RE::BSShader**> lightingShader{ REL::Offset(0x338ca00) };
		if (auto* shader = *lightingShader.get(); shader && shader->shaderType.get() == RE::BSShader::Type::Lighting)
			evaluator.SetLightingShader(shader);
	}

	void SceneStore::DrainCapture(RegistrationDrain& a_drain)
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained: the capture buffer is fixed-capacity and a frame that does not drain it overflows, and the drain takes the
		// frame's withholding counters (the report's). The main camera's registrations are no source of anything DCLF draws (scene
		// membership and the mirror are; T6b2c step 8): what follows is the parity's (CS_DCLF_PERSISTENT_PARITY) and the decal order
		// probe's, and nothing else reads them.
		const auto entries = capture.Drain();
		// The reflection residue's geometries (T6), classified by the next scene pass (ClassifyResidue). Its watch is its own gate (the
		// sun views' parity or the timeline: IndirectDraws::ReflectionRootsOwned); taken every frame so it never accumulates. Their names
		// are read here, where the frame registered them (T6b3c: the pass reading them runs past this frame's Present).
		const auto residue = capture.TakeReflectionResidueGeometries();
		a_drain.residue.reserve(residue.size());
		for (const auto* geometry : residue) {
			const auto* parent = geometry ? geometry->parent : nullptr;
			a_drain.residue.push_back({ geometry, geometry ? fmt::format("'{}' under '{}' ({})", geometry->name.c_str() ? geometry->name.c_str() : "",
															 parent && parent->name.c_str() ? parent->name.c_str() : "",
															 geometry->GetRTTI() && geometry->GetRTTI()->name ? geometry->GetRTTI()->name : "?") :
														 std::string() });
		}
		frameLightingPass = nullptr;
		a_drain.observed = SwitchEnabled(Switch::PersistentParity);
		const bool probe = SwitchValue(Switch::DecalOrderProbe) == "1";
		if ((!a_drain.observed && !probe) || !RefreshMainBatchRenderers()) {
			a_drain.observed = false;
			return;
		}
		if (probe)
			Scene::ProbeDecalOrder(entries, mainBatchRenderers);
		if (!a_drain.observed)
			return;
		// The parity's: any Lighting pass the frame registered names the engine's instance DCLF evaluates with (CaptureLightingShader).
		auto& evaluator = ConstantEvaluator::Get();
		if (evaluator.HasLightingShader())
			for (const auto& entry : entries)
				if (entry.pass && entry.pass->shader && entry.pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					if (entry.pass->shader != evaluator.GetLightingShader() && residentStats.lightingShaderDiffers++ == 0)
						logger::warn("[DCLF] the Lighting shader the registrations use ({}) is not the engine's instance DCLF evaluates with ({}) <- LIGHTING SHADER",
							static_cast<const void*>(entry.pass->shader), static_cast<const void*>(evaluator.GetLightingShader()));
					break;
				}
		// The frame's registered lighting pass: the template parity's fallback reference (RegisteredTemplatePassOf, persistent-parity
		// frames at Prepass).
		for (const auto& entry : entries)
			if (entry.pass && mainBatchRenderers.contains(entry.batch) && entry.pass->shader &&
				entry.pass->shader->shaderType.get() == RE::BSShader::Type::Lighting && entry.pass->numLights > 0 && entry.pass->sceneLights) {
				frameLightingPass = entry.pass;
				break;
			}
		// Eligible objects DCLF has not bound that the engine registered: a scene event DCLF missed (a record not written
		// again, a verdict not taken again). The tracked set is the coordinator's: checked by the next scene pass (CheckRegistrations).
		for (const auto& entry : entries)
			if (entry.geometry && mainBatchRenderers.contains(entry.batch) && !entry.fading)
				a_drain.registrations.push_back({ entry.geometry, entry.hint });
	}

	void SceneStore::PostRegistrationDrain(std::shared_ptr<RegistrationDrain> a_drain)
	{
		// Latest wins for the registrations (the newest frame's alone are checked); a post no scene pass took gives its residue to this
		// one, so none is lost or classified twice. The take between this exchange and the store finds nothing and takes it next pass.
		if (const auto unread = registrationDrainPosted.exchange(nullptr, std::memory_order_acq_rel))
			a_drain->residue.insert(a_drain->residue.begin(), unread->residue.begin(), unread->residue.end());
		registrationDrainPosted.store(std::shared_ptr<const RegistrationDrain>(std::move(a_drain)), std::memory_order_release);
		// T6b3d: no wake: the frame's next pass takes it (a mid-frame post the pass does not need sooner).
	}

	void SceneStore::ClassifyResidue()
	{
		// What the engine registered into the reflection faces under the LOD land and objects roots that is no reflection member:
		// untracked, tracked but ineligible (by reason), eligible but not bound, or bound but not in the reflection phase. The drain's
		// (TakeAccumulateInputs: the frames since the last pass took one), by pointer and the names the render thread read (T6b3c: a
		// geometry the engine released since is untracked here, and nothing of it is dereferenced).
		if (!registrationDrain)
			return;
		std::scoped_lock lock(residueClassesLock);
		for (const auto& [geometry, names] : registrationDrain->residue) {
			const auto it = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			std::size_t kind;
			if (it == tracked.end())
				kind = kResidueUntracked;
			else if (it->second.candidateReason != Ineligible::None)
				kind = static_cast<std::size_t>(it->second.candidateReason);
			else if (const std::int32_t slot = FindObject(geometry); !ResidentObject(slot))
				kind = kResidueUnbound;
			else
				kind = (PhasesIn(tables, static_cast<std::uint32_t>(slot)) & kSetReflection) ? kResidueReflection : kResidueNotReflection;
			++residueClasses.counts[kind];
			// How long a geometry stays in the residue (distinct geometries, and those seen on 10 frames or more this window).
			if (auto& seen = residueClasses.seen[geometry]; seen.frame != sceneFrame) {
				seen.frame = sceneFrame;
				if (++seen.frames == 10)
					++residueClasses.persistent;
			}
			if (TimelineEnabled())
				StageResidue(geometry, names, it == tracked.end() ? nullptr : &it->second);
			if (residueClasses.first[kind].empty() && it != tracked.end())
				residueClasses.first[kind] = names;
		}
	}

	void SceneStore::StageResidue(const RE::BSGeometry* a_geometry, const std::string& a_names, const Tracked* a_entry)
	{
		// Under residueClassesLock (ClassifyResidue's).
		auto& classes = residueClasses;
		std::size_t stage = kStageUntracked;
		if (a_entry) {
			if (a_entry->candidateReason == Ineligible::Hidden) {
				bool hiddenNow = false;
				const auto [shown, site] = LastShowOnChain(*a_geometry, *a_entry, hiddenNow);
				stage = hiddenNow ? kStageHiddenNow : kStageHiddenStale;
				if (!hiddenNow) {
					++classes.staleSites[site];
					if (shown)
						++classes.sinceShow[AgeBucket(sceneFrame - shown)];
				}
			} else if (a_entry->candidateReason != Ineligible::None) {
				stage = kStageIneligible;
			} else if (const std::int32_t slot = FindObject(a_geometry); slot < 0) {
				stage = kStageNoRecord;
			} else if (!ResidentObject(slot)) {
				stage = kStageUnbound;
				++classes.unboundBy[a_entry->bindFailFrame >= a_entry->writtenFrame ? a_entry->bindFail : 0];
				++classes.unboundJoin[a_entry->accumulateReasonFrame >= a_entry->writtenFrame ? static_cast<std::size_t>(a_entry->accumulateReason) : 0];
			} else if (const auto s = static_cast<std::uint32_t>(slot); PhasesIn(tables, s) & kSetReflection) {
				stage = kStageApplied;
			} else if (s < setWaitCause.size() && setWaitCause[s]) {
				stage = kStageWaiting;
				++classes.waitingBy[s < setWaitWhy.size() ? std::min<std::size_t>(setWaitWhy[s], classes.waitingBy.size() - 1) : 0];
			} else {
				stage = kStageNotReflection;
			}
			++classes.ages[stage][AgeBucket(sceneFrame - a_entry->trackedFrame)];
		}
		++classes.stages[stage];
		if (classes.stageFirst[stage].empty()) {
			classes.stageFirst[stage] = fmt::format("{}{}", a_names,
				a_entry ? fmt::format(" (tracked {}, written {}, bound {}, joined {}, frame {})", a_entry->trackedFrame, a_entry->writtenFrame, a_entry->boundFrame,
							  a_entry->memberFrame, sceneFrame) :
						  std::string());
		}
	}

	SceneStore::ResidueClasses SceneStore::TakeResidueClasses()
	{
		std::scoped_lock lock(residueClassesLock);
		return std::exchange(residueClasses, {});
	}

	void SceneStore::CheckRegistrations()
	{
		ClassifyResidue();
		// The registration parity (T6b2c step 8): only while the render thread observed the frame's registrations (DrainCapture); the
		// normal path reads none of them. T6b3c: the newest frame's drain posted since the last pass (the frame before this pass's), judged
		// against the residency this pass's walk left, before its joins: a record bound by the last pass counts as bound.
		if (!registrationDrain || !registrationDrain->observed)
			return;
		++residentStats.registrationFrames;
		residentStats.registrationsChecked += registrationDrain->registrations.size();
		for (const auto& entry : registrationDrain->registrations) {
			const auto it = tracked.find(const_cast<RE::BSGeometry*>(entry.geometry));
			if (it == tracked.end() || it->second.candidateReason != Ineligible::None || ResidentObject(FindObject(entry.geometry)))
				continue;
			++residentStats.registeredUnbound;
			// The names are the engine's: read under a lease (the scene lane), skipped without one.
			if (residentStats.registeredUnboundFirst.empty())
				if (const EngineReadWindow::Lease lease; lease)
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
		bool extrasWritten = false;
		if (a_patch.projectedUV || a_patch.landBlend) {
			stats.projectedUV += a_patch.projectedUV ? 1 : 0;
			stats.landBlend += a_patch.landBlend ? 1 : 0;
			if (tables.extraOffset[objectId] == kNoExtraRows)
				tables.extraOffset[objectId] = tables.AllocateExtras();
			// Its static parts (the frame's are the draw's: ExtrasFrame). A kept block's rows rewritten in place (a re-bind that moved
			// kObjectProjectedUV/LandBlend or the technique) change no offset: noted below, or the uploads would never send them.
			extrasWritten = WriteObjectExtras(objectId);
		} else {
			tables.FreeExtras(objectId);
		}
		// A membership join: the column goes into the same before/after journal as the other value-only writes.
		tables.residentSlot[objectId] = 1;
		before.NoteWrite(tables, objectId);
		if (extrasWritten)
			tables.NoteChange(objectId, kChangeExtras);
		// Its shading, sampled by the next frame's values (the record joins the set then at the earliest).
		NameShading(objectId, true);
		if (a_patch.decalKey) {
			++stats.decals[static_cast<std::uint32_t>(a_patch.decalKey >> 60) - 1];
			memberDecals[objectId] = a_patch.decalKey;
			NoteDecalChanged(objectId);
		}
	}

	void SceneStore::PostAccumulateInputs()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Inputs");
		// The capture drained (always: its buffer is fixed-capacity, and the drain takes the frame's withholding counters); what the
		// registrations say is the parity's alone (DrainCapture). Posted for the next scene pass (T6b3c: the pass runs from the frame's
		// start, so this frame's drain is the next pass's).
		auto drain = std::make_shared<RegistrationDrain>();
		DrainCapture(*drain);
		PostRegistrationDrain(std::move(drain));
		// CS_DCLF_PERSISTENT_PARITY: the owned members' light masks, read off the engine's light data after the registration jobs.
		if (SwitchEnabled(Switch::PersistentParity))
			PrimaryCull::Get().CheckLightMasks();
		// The joins' frame input: Light Limit Fix's room map, posted when its generation moves.
		PostRoomMap();
	}

	void SceneStore::PostRoomMap()
	{
		// Light Limit Fix's map is the render thread's (it swaps it at its own point), so it is read here and only the copy is posted.
		auto& lightFix = globals::features::lightLimitFix;
		if (!lightFix.loaded)
			return;
		const std::uint64_t generation = lightFix.GetRoomMapGeneration();
		if (generation == roomMapPostedGeneration)
			return;
		auto map = std::make_shared<ankerl::unordered_dense::map<const RE::NiNode*, int>>();
		map->reserve(lightFix.roomNodes.size());
		for (const auto& [node, index] : lightFix.roomNodes)
			map->emplace(node, index);
		auto input = std::make_shared<RoomMapInput>();
		input->map = std::move(map);
		input->generation = generation;
		roomMapPosted.store(std::shared_ptr<const RoomMapInput>(std::move(input)), std::memory_order_release);
		roomMapPostedGeneration = generation;
		accumulateInputStats.roomMapsPosted.fetch_add(1, std::memory_order_relaxed);
		// T6b3d: no wake: the frame's next pass takes it (a mid-frame post the pass does not need sooner).
	}

	void SceneStore::PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame)
	{
		pipelineFramePosted.store(std::make_shared<const GeometryPort::PipelineFrame>(a_frame), std::memory_order_release);
		accumulateInputStats.pipelineFramesPosted.fetch_add(1, std::memory_order_relaxed);
		// T6b3d: no wake: the frame's next pass takes it (a mid-frame post the pass does not need sooner).
	}

	void SceneStore::TakeAccumulateInputs()
	{
		// Latest wins: a post the render thread replaced before this pass took it is dropped there (nothing in either holds the engine's).
		if (auto posted = roomMapPosted.exchange(nullptr, std::memory_order_acq_rel)) {
			roomMap = posted->map;
			roomMapGeneration = posted->generation;
			accumulateInputStats.roomMapsTaken.fetch_add(1, std::memory_order_relaxed);
		}
		if (auto posted = pipelineFramePosted.exchange(nullptr, std::memory_order_acq_rel)) {
			scenePipelineFrame = std::move(posted);
			accumulateInputStats.pipelineFramesTaken.fetch_add(1, std::memory_order_relaxed);
		}
		// The capture's drain (T6b3c): none posted since the last pass leaves none, so no drain is checked twice.
		registrationDrain = registrationDrainPosted.exchange(nullptr, std::memory_order_acq_rel);
	}

	void SceneStore::MakeNewPipelineConstants()
	{
		// T6b2c step 8: a used pipeline without a current block (a new slot, a slot keyed again, a post dropped) gets one here, on the
		// coordinator, from the newest pipeline frame the render thread posted (Prepass's sample with a sun, PostPipelineFrame): the
		// port is pure. The render thread makes the slot's block again from its own sample once its frame shows the slot (Prepass:
		// RefreshFrameConstants, a slot new to the frame's tables made whole and posted), which supersedes this one; until then this
		// one draws. Without a sample yet (the session's first frames, or no sun since) nothing is made and the slot waits as before.
		if (!scenePipelineFrame) {
			for (std::size_t word = 0; word < tables.usedPipelineBits.size(); ++word)
				for (std::uint64_t remaining = tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1)
					if (const std::size_t p = word * 64 + static_cast<std::size_t>(std::countr_zero(remaining)); p < tables.pipelines.size() && !tables.PipelineConstantsCurrent(p))
						accumulateInputStats.pipelineBlocksWaited.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		bool wrote = false;
		for (std::size_t word = 0; word < tables.usedPipelineBits.size(); ++word)
			for (std::uint64_t remaining = tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::size_t p = word * 64 + static_cast<std::size_t>(std::countr_zero(remaining));
				if (p >= tables.pipelines.size() || p >= tables.pipelineConstants.size() || p >= tables.pipelineBindingVersion.size() || tables.PipelineConstantsCurrent(p))
					continue;
				GeometryConstants constants{};
				if (!GeometryPort::PipelineGeometryConstants(tables.pipelines[p].passDescriptor, kMainPassRenderFlags, *scenePipelineFrame, constants))
					continue;
				auto& row = tables.pipelineConstants[p];
				row.constants = constants;
				row.key = tables.pipelines[p];
				row.binding = tables.pipelineBindingVersion[p];
				row.version = tables.NextVersion();
				row.valid = true;
				wrote = true;
				accumulateInputStats.pipelineBlocksMade.fetch_add(1, std::memory_order_relaxed);
			}
		if (wrote)
			tables.NoteConstantsWrite();
	}

	SceneStore::AccumulateInputStats SceneStore::TakeAccumulateInputStats()
	{
		auto& c = accumulateInputStats;
		AccumulateInputStats s;
		s.roomMapsPosted = c.roomMapsPosted.exchange(0, std::memory_order_relaxed);
		s.roomMapsTaken = c.roomMapsTaken.exchange(0, std::memory_order_relaxed);
		s.pipelineFramesPosted = c.pipelineFramesPosted.exchange(0, std::memory_order_relaxed);
		s.pipelineFramesTaken = c.pipelineFramesTaken.exchange(0, std::memory_order_relaxed);
		s.pipelineBlocksMade = c.pipelineBlocksMade.exchange(0, std::memory_order_relaxed);
		s.pipelineBlocksWaited = c.pipelineBlocksWaited.exchange(0, std::memory_order_relaxed);
		return s;
	}

	/**
	 * @brief The accumulator half of the frame's render-thread work, at EarlyPrepass: the material part, a frame input and a parity
	 * observer (T6b2c step 7). The records are the scene work's (RefreshMaterialRecords: the writers' captures, the frame components,
	 * the texture transforms); the frame inputs and the other observers are PostAccumulateInputs' (step 8); the joins are the scene
	 * pass's (T6b3c), on the scene lane.
	 */
	void SceneStore::PrepareAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Prepare");
		// T6b3c: no sceneBuilt test (the coordinator's, and the scene pass may be running): a request is answered whether or not a scene
		// is built, and the queue is empty when no join ran.
		// The frame input: the captures the joins asked for (a material no writer or attach captured lately), read where the engine
		// reads its materials. The parity observer: the installed records against the engine's evaluation (CS_DCLF_PERSISTENT_PARITY).
		ServeMaterialRequests();
		ValidateMaterialSlice();
	}

	void SceneStore::ServeMaterialRequests()
	{
		materialRequestQueue.Drain([&](MaterialRequest&& a_request) {
			// A material the scene work holds no capture of (T6b2a): captured here, off the property the join read (it holds the
			// property, the property its material), for the scene work's next pass. A property with another material now was swapped
			// after the join's batch: the request is stale (the join asks again after the swap's). Answered either way, so the join may
			// ask again; the property's reference is dropped here, on the render thread.
			const bool current = a_request.property && a_request.property->material == a_request.material;
			if (current)
				MaterialPort::PushCapture(a_request.material);
			materialRequestsAnswered.Push(MaterialAnswer{ a_request.material, current });
		});
	}

	template <class Result, class Evaluate, class Difference>
	void SceneStore::EvaluateJoins(const char* a_what, std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate, Difference&& a_difference)
	{
		// T6b3e: the joins' evaluations (no counters: they count nothing), by chunks of kFanoutGrain on the pool or here; each under a
		// ShardScope (on the pool also the frame's globals and the scene work's flags).
		a_results.clear();
		a_results.resize(a_count);
		auto evaluate = [&](std::vector<Result>& a_into, bool a_onPool) {
			auto body = [&](std::size_t a_begin, std::size_t a_end) {
				ShardScope scope(*this, a_onPool);
				for (std::size_t i = a_begin; i < a_end; ++i)
					a_evaluate(i, a_into[i]);
			};
			if (a_onPool) {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Accumulate.Joins", a_count, kFanoutGrain, body);
			} else {
				for (std::size_t begin = 0; begin < a_count; begin += kFanoutGrain)
					body(begin, std::min(a_count, begin + kFanoutGrain));
			}
		};
		evaluate(a_results, a_pool);
		// CS_DCLF_FANOUT_PARITY: evaluated again here, one after another, and compared field by field.
		if (a_pool && SwitchEnabled(Switch::FanoutParity) && PassParityDue(11)) {
			std::vector<Result> serial(a_count);
			evaluate(serial, false);
			auto& f = fanoutStats;
			++f.parityRounds;
			f.parityEntries += a_count;
			for (std::size_t i = 0; i < a_count; ++i)
				if (const char* field = a_difference(a_results[i], serial[i]); field && f.parityDiffers++ == 0)
					f.parityFirst = fmt::format("{}, element {}: {}", a_what, i, field);
		}
	}

	void SceneStore::BuildAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Tables");
		if (!sceneBuilt)
			return;  // a load screen, or the feature installed mid-frame: nothing to patch
		// T6b3e: the pass's snapshot of the toggles and Terrain Blending (BuildScenePhase took it), as the walk read them.
		ShardScope passScope(*this, false);
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
		const bool derivedProbe = SwitchEnabled(Switch::PersistentParity) && PassParityDue(13);
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
		// One membership join: an entry's base record, or its layer's (Tracked::layerSlot, drawn from the layer property). T6b3e: its
		// evaluation (EvaluateJoin: the held property's check, the cache's witnesses, the classification, the key) before, on the pool
		// for a burst; here, in order, what reads or writes the slots: the cache's slot half, the pipeline and material slots and records,
		// the derivation's cache, the patch and the membership.
		auto join = [&](const OrderEntry& a_entry, JoinResult& a_result) {
			RE::BSGeometry* geometry = a_entry.geometry;
			Tracked* trackedEntry = a_entry.tracked;
			const AccumulatedPass* accumulated = a_entry.accumulated;
			const bool layer = a_entry.layer;
			timer.Add(BuildPart::LoopTail);
			if (a_result.kind == JoinResult::Kind::Skip)
				return;
			// BindByMembership joins only records written this frame with a verdict of None, never a shadow-only one.
			const std::uint32_t objectId = layer ? trackedEntry->layerSlot : trackedEntry->objectId;
			auto& object = tables.objects[objectId];
			// The positive derivation, cached (Tracked::Derived): for an accumulated object whose witnesses all match (the evaluation's)
			// and whose slots still carry the keys they were derived for (here: the joins before this one allocate and key slots), the
			// classification and the whole derived section are skipped.
			auto& derived = layer ? trackedEntry->layerDerived : trackedEntry->derived;
			bool derivedHit = a_result.derivedWitness && derived.pipelineSlot < tables.pipelines.size() && tables.pipelineSlots.Alive(derived.pipelineSlot) &&
			                  tables.pipelines[derived.pipelineSlot] == derived.key &&
			                  derived.materialSlot < tables.materialSlotKey.size() && tables.materialSlots.Alive(derived.materialSlot) &&
			                  tables.materialSlotKey[derived.materialSlot] == std::pair{ derived.material, derived.descriptors.pass };
			// Its witnesses held but its slots are gone: classified as the join would have been (the evaluation again, here).
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
					trackedEntry->accumulateReasonFrame = sceneFrame;
					trackedEntry->accumulateReasonPass = passSerial;
				}
				// Eligible for a record but not for bindings: it stays native, which is what its scene record
				// already says (kObjectNoBindings).
				derived.valid = false;
				return;
			}
			timer.Add(BuildPart::ClassifyFrame);

			// Only computed when something will report them: this whole block exists to feed one log line (CS_DCLF_DERIVE_PROBE: the
			// joins run interleaved, FanoutMode, so its lazily sampled frame and its leases are the lane's).
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
				// The live property and fade node under a lease (T6b1d: the scene work may run beside the engine's update).
				const EngineReadWindow::Lease lease;
				if (const auto node = lease ? LodFadeNodeOf(property) : decltype(LodFadeNodeOf(property)){}; lease && LodFadesApply(node) && lodFadeSample.fadesOn != 0.0f) {
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
					tables.geometryTemplateObject[pipelineSlot] = objectId;
				}
				timer.Add(BuildPart::DedupHit);
			} else {
				// The key and static flags are the evaluation's (a classified join's: EvaluateJoin); the alpha property's blend for the
				// permutation, the record's.
				const auto* alpha = layer ? nullptr : leaf.alpha;
				const bool alphaBlending = alpha && (alpha->flags & 1u);  // GetAlphaBlending
				key = a_result.key;
				auto pipelineIt = pipelineIndex.find(key);
				const bool newPipeline = pipelineIt == pipelineIndex.end();
				if (!newPipeline && !tables.PipelineUsed(pipelineIt->second)) {
					// The slot's first member: this object's property is the template (see the cached path).
					tables.MarkPipelineUsed(pipelineIt->second);
					joinMarkedPipelines.push_back(pipelineIt->second);
					tables.geometryTemplate[pipelineIt->second] = property;
					tables.geometryTemplateObject[pipelineIt->second] = objectId;
				}
				if (newPipeline) {
					timer.Add(BuildPart::Dedup);
					const std::uint32_t slot = AllocatePipelineSlot();
					tables.pipelines[slot] = key;
					// Its PerGeometry values (the engine's SetupGeometry from the template's lighting pass) are the frame's
					// (FrameTables): evaluated by the render thread for a slot whose key or binding is new.
					tables.geometryTemplate[slot] = property;
					tables.geometryTemplateObject[slot] = objectId;

					tables.pipelineTechnique[slot] = TechniqueRowFor(descriptors.pass);
					stats.shadowMaskPipelines += tables.TechniqueShadowMask(slot) ? 1 : 0;

					PipelinePermutation permutation;
					permutation.vertexShaderDescriptor = descriptors.rawVertex;
					permutation.pixelShaderDescriptor = descriptors.rawPixel & ~descriptors.pixel;
					permutation.extraShaderDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::InWorld);
					// AdditiveLighting (State::UpdateLightingShaderPermutation): a pass whose alpha property blends onto the target
					// (destination ONE). Only a blended decal (group 2) applies its alpha property, and its key carries the blend
					// mode, which the blend functions decide: the same for every object of the key.
					if (descriptors.decalGroup == 2 && alphaBlending &&
						static_cast<RE::NiAlphaProperty::AlphaFunction>((alpha->flags >> 5) & 0xF) == RE::NiAlphaProperty::AlphaFunction::kOne)  // GetDestBlendMode
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
				const auto* material = witnessMaterial;
				auto materialIt = materialIndex.find(std::pair{ material, descriptors.pass });
				if (materialIt == materialIndex.end()) {
					timer.Add(BuildPart::Dedup);
					// The record is the material port's (T6b2a), from the material's capture (MaterialPort::captures: its writer's, its
					// attach's) and the frame's sources. A material with no capture is asked of the render thread (materialRequestQueue:
					// ServeMaterialRequests captures it), once until it answers, and the join waits for it, staying native meanwhile.
					auto& ms = materialSnapshotStats;
					const auto held = materialSnapshots.find(material);
					if (held == materialSnapshots.end() || !held->second.held) {
						if (materialRequested.insert(material).second) {
							MaterialRequest request;
							request.property.reset(property);
							request.material = material;
							request.pass = descriptors.pass;
							materialRequestQueue.Push(std::move(request));
							++ms.requested;
						}
						bindRetry.push_back(objectId);
						++residentStats.materialWaits;
						derived.valid = false;
						return;
					}
					held->second.frame = sceneFrame;
					MaterialRecord record;
					if (!MaterialPort::Evaluate(held->second.held->snapshot, descriptors.pass, FrameGlobals::Current().material, record)) {
						// An input the frame's sources do not have yet (Advanced Skin's textures for a key it has not set up): again with a
						// later frame's. Else a class the port does not cover: native, by its reason (counted).
						derived.valid = false;
						if (!MaterialPort::FeatureHooksCovered(held->second.held->snapshot, FrameGlobals::Current().material.feature)) {
							bindRetry.push_back(objectId);
							++residentStats.materialWaits;
							return;
						}
						++ms.uncovered;
						return;
					}
					++ms.made;
					// A record holds no view of the character light's t11 (the frame's: MaterialSources::ApplyFrameComponents).
					MaterialSources::StripFrameViews(record, descriptors.pass);
					const std::uint32_t slot = AllocateMaterialSlot();
					materialOwners.resize(tables.materials.size());
					// Its own reference, a second count on the capture's material (alive: the capture holds one).
					if (materialOwners[slot])
						materialsReleased.Push(std::move(materialOwners[slot]));
					materialOwners[slot].reset(const_cast<RE::BSShaderMaterial*>(material));
					tables.materials[slot] = record;
					tables.materialVersion[slot] = ++materialVersions;
					tables.NoteMaterial(slot);
					tables.materialSlotKey[slot] = std::pair{ material, descriptors.pass };
					materialIt = materialIndex.emplace(std::pair{ material, descriptors.pass }, slot).first;
					ListMaterialDependent(material, slot);
					// Its signature's frame components and its texture transforms follow from here (T6b2c step 7).
					NoteMaterialRecord(slot, held->second.held->snapshot);
					timer.Add(BuildPart::MaterialEval);
				}
				materialSlot = materialIt->second;
				tables.MarkMaterialUsed(materialSlot);
				joinMarkedMaterials.push_back(materialSlot);
				timer.Add(BuildPart::DedupHit);

				// ExternalEmittance::ShouldSuppress and the rest of the static flags: the evaluation's (EvaluateJoin).
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
				// The room the evaluation found when the room map moved on since the entry's (EvaluateJoin; the map is read-only here).
				if (a_result.roomSet) {
					trackedEntry->roomIndex = a_result.roomIndex;
					trackedEntry->roomMapGeneration = roomMapGeneration;
				}
				patch.lights.roomIndex = trackedEntry->roomIndex;
			}
			// A tree's wind: TreeWindCS's from its listing, and until its entry the shading row's (FrameValues, T6b1a): none in the record.
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
		{
			// The geometry slots the evaluations read (the joins resolve none): unchanged until the merges are done.
			[[maybe_unused]] const std::size_t geometries = tables.geometries.size();
			[[maybe_unused]] const std::uint32_t generation = tablesGeneration;
			const std::size_t count = accumulateOrder.size();
			auto& f = fanoutStats;
			++f.joinRounds;
			f.joinEntries += count;
			const std::uint32_t mode = FanoutMode(count, true);
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
					EvaluateJoins("joins", count, mode == 2, joinResults, [&](std::size_t a_index, JoinResult& a_out) { EvaluateJoin(accumulateOrder[a_index], false, a_out); },
						&JoinDifference);
				}
				DCLF_SCENE_PART(JoinsMerge, "CS.DCLF.Accumulate.JoinsMerge");
				for (std::size_t i = 0; i < count; ++i)
					join(accumulateOrder[i], joinResults[i]);
			}
			assert(tables.geometries.size() == geometries && tablesGeneration == generation);
		}

		TracyCZoneEnd(objectsZone);
		TracyCZoneN(residentsZone, "CS.DCLF.Accumulate.Residents", true);
		// Membership joins that were not patched (a verdict of the frame, a material not ready): their entries leave
		// residency.
		for (const auto* geometry : residentJoining) {
			++residentStats.failed;
			const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
			const auto cause = entry == tracked.end() || entry->second.objectStamp != objectStamp ? 1u :                  // no record
			                   entry->second.accumulateReasonPass == passSerial ? 2u :                                           // a verdict of this pass
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
		if (ResidentParityEnabled() && PassParityDue() && !residents.empty())
			CheckResidentParity();
		{
			// CS_DCLF_PERSISTENT_PARITY: no bound object references a slot that is not live. Slots are freed only when nothing references
			// them (their reference counts), which this checks; the normal path trusts them (invariant 5).
			if (SwitchEnabled(Switch::PersistentParity) && (slotsFreedThisFrame || PassParityDue()))
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
		// A record bound again leaves the main phases of the coordinator's set first (a join never patches a member), the commit's
		// decision too, applied or waiting for its application (LeaveSet), the whole object (U5: a claim is the whole object). T6b3c:
		// the commit follows the joins in the same pass, so one bound again and ready joins again at it, its claim kept (the frame draws
		// its old record from the installed snapshot until the publication with the new one); one that is not leaves there (ApplySet's
		// notes: LeaveSet marks it for the application). This was CommitSet's rebinding rule while the commit ran before the joins.
		auto leaveMain = [&](std::uint32_t a_slot) {
			if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & kSetMain))
				++setStats.rebinding;
			LeaveSet(a_slot, kSetMain | kSetReflection);
			if (PhasesIn(tables, a_slot) & (kSetMain | kSetReflection)) {
				tables.setPhases[a_slot] = 0;
				tables.objects[a_slot].flags &= ~kObjectMember;
				tables.NoteChange(a_slot, kChangeBindings);
			}
		};
		// The frame globals a membership pass reads changed (the static sun bits, the fade distances): every resident is bound
		// again from this frame's, out of the set until the commit (T6b3c: the witness is the joins' alone; CommitSet's rebindAll is gone).
		if (const std::uint32_t witness = sceneInputs.membershipWitness; witness != membershipWitness) {
			for (const std::uint32_t slot : residents)
				leaveMain(slot);
			bindQueue.insert(bindQueue.end(), residents.begin(), residents.end());
			EndAllResidency();
			membershipWitness = witness;
		}
		// Material records are the engine's SetupMaterial through its Lighting shader, which exists only once the engine has drawn
		// with it: until then the queue waits (the records at load are the whole scene).
		// T6b2a: the material records come from the frame's sources (FrameGlobals), which know the Lighting shader only from the frame
		// after it was found: the joins wait for that frame (else every record of the load would fail the port).
		if (!ConstantEvaluator::Get().HasLightingShader() || !FrameGlobals::Current().material.vanilla.sampled || !FrameGlobals::Current().material.vanilla.shaderKnown)
			return;
		// The joins that waited for a material record, again.
		bindQueue.insert(bindQueue.end(), bindRetry.begin(), bindRetry.end());
		bindRetry.clear();
		const auto queue = std::exchange(bindQueue, {});
		// T6b3e: each element evaluated (EvaluateJoinPre: the skip, the binding standing, the membership pass), then merged here in queue
		// order. A slot can sit in the queue twice (a retry and a resident queued again): each evaluation reads the state before the
		// merges, and the merge decides what depends on the merges before it (a member dropped since, with its partner).
		auto merge = [&](std::uint32_t a_slot, JoinPre& a_pre) {
			const std::uint32_t slot = a_slot;
			// A member whose binding stood when evaluated, dropped since by an earlier element's failure (a layer and its base leave
			// together): evaluated again, as the serial walk found it.
			if (a_pre.kind == JoinPre::Kind::Kept && !IsResidentSlot(slot))
				EvaluateJoinPre(slot, a_pre);
			if (a_pre.kind == JoinPre::Kind::Skip)
				return;
			// A member written again keeps its binding while what the binding reads is the same (the derivation cache's
			// witnesses): a skin, a face or a mover is written every frame for its transform, bones or stream alone.
			if (a_pre.kind == JoinPre::Kind::Kept) {
				++residentStats.membershipKept;
				return;
			}
			auto* geometry = a_pre.geometry;
			const bool layer = a_pre.layer;
			auto& entry = tracked.find(geometry)->second;
			// The membership pass's derivation, kept in the entry (T6b3e: PrimaryCull's per-geometry maps before).
			(layer ? entry.membershipLayerDerived : entry.membershipDerived) = a_pre.derived;
			if (a_pre.kind == JoinPre::Kind::Failed) {
				// A member whose binding does not stand and gives no pass is not bound any more (the failed joins' rule, below): T6b3c,
				// the commit after the joins would otherwise keep it with the binding it no longer has (the commit before them kept it
				// out by its rebinding rule).
				if (IsResidentSlot(slot))
					DropResidentSlot(slot, true);
				// T6b0: dropped from the queue, bound only when written again.
				if (TimelineEnabled() && !layer) {
					const std::uint8_t why = a_pre.fail;
					const auto& leaf = a_pre.leaf;
					entry.bindFail = why;
					entry.bindFailFrame = sceneFrame;
					std::scoped_lock lock(residueClassesLock);
					++timelineStats.bindFailures[why];
					if (timelineStats.bindFailFirst.empty()) {
						timelineStats.bindFailFirst = fmt::format("'{}' under '{}' ({}, material alpha {:.3f}, alpha property {})", leaf.node && leaf.node->name ? leaf.node->name : "",
							leaf.parent && leaf.parent->name ? leaf.parent->name : "", PrimaryCull::kSyntheticFailNames[why],
							leaf.property && leaf.property->material ? leaf.property->materialAlpha : -1.0f, leaf.alpha ? leaf.alpha->flags : 0xFFFF);
					}
				}
				return;
			}
			(layer ? accumulatedLayerPasses : accumulatedPasses).insert_or_assign(geometry, a_pre.pass);
			(layer ? residentLayerJoining : residentJoining).insert(geometry);
			leaveMain(slot);
			++residentStats.membershipQueued;
		};
		const std::size_t count = queue.size();
		const std::uint32_t mode = FanoutMode(count, true);
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
			EvaluateJoins("bind queue", count, mode == 2, joinPres, [&](std::size_t a_index, JoinPre& a_out) { EvaluateJoinPre(queue[a_index], a_out); }, &JoinPreDifference);
		}
		{
			DCLF_SCENE_PART(JoinsMerge, "CS.DCLF.Accumulate.BindMerge");
			for (std::size_t i = 0; i < count; ++i)
				merge(queue[i], joinPres[i]);
		}
	}

	void SceneStore::EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const
	{
		// T6b3e: BindByMembership's element, evaluated (pure: the tables as the joins found them, the tracked set, the mirror, the entry's
		// membership cache; PrimaryCull's pass reads the frame's globals the ShardScope binds).
		const std::uint32_t slot = a_slot;
		auto& r = a_out;
		r = JoinPre{};
		if (slot >= tables.objects.size() || (tables.objects[slot].flags & (kObjectFree | kObjectShadowOnly)))
			return;
		auto* geometry = const_cast<RE::BSGeometry*>(tables.objectGeometry[slot]);
		const auto trackedIt = geometry ? tracked.find(geometry) : tracked.end();
		// A layer slot (WriteLayer) joins with its own pass: its property's, with hint 12.
		const bool layer = tables.IsLayer(slot);
		// A record written every frame (a mover, an actor's part, animated shading) is bound again every frame: its slots are
		// referenced again in the same batch, so nothing about it is retired.
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
		// The mirror's records (T6b1b), and the entry's derivation cache (T6b3e: the merge stores what the pass leaves).
		r.leaf = mirror.Leaf(geometry);
		const auto& cached = layer ? entry.membershipLayerDerived : entry.membershipDerived;
		const auto membership = layer ? PrimaryCull::MembershipLayerPass(geometry, r.leaf, cached) : PrimaryCull::MembershipPass(geometry, r.leaf, cached);
		r.derived = membership.derived;
		r.fail = membership.fail;
		if (!membership.ok) {
			r.kind = JoinPre::Kind::Failed;
			return;
		}
		r.pass = membership.pass;
		// A fade node's objects carry its fade-out distance for BuildDraws' fade test, which measures from the node's centre
		// (Tables::lodFade, SetFadeRow). A tree's also take its height test.
		if (const auto* fadeNode = r.leaf.fadeNode) {
			r.pass.fadeDistance = PrimaryCull::MembershipFadeDistance(*fadeNode);
			r.pass.heightTest = (fadeNode->kind & SceneCapture::kKindTree) != 0;
		}
		r.kind = JoinPre::Kind::Pass;
	}

	void SceneStore::EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const
	{
		// T6b3e: a membership join's evaluation (pure, any thread under a ShardScope). Whether the derivation cache's slots still hold,
		// the pipeline and material slots, the material records and the patch are the merge's (BuildAccumulatePhase's join).
		auto* geometry = a_entry.geometry;
		const Tracked& entry = *a_entry.tracked;
		const AccumulatedPass* accumulated = a_entry.accumulated;
		const bool layer = a_entry.layer;
		auto& r = a_out;
		r = JoinResult{};
		// BindByMembership joins only records written this frame with a verdict of None, never a shadow-only one.
		const std::uint32_t objectId = layer ? entry.layerSlot : entry.objectId;
		r.geometrySlot = tables.objects[objectId].geometryIndex;
		// The mirror's records (T6b1b), and the entry's held property (its reference: the template's, a material request's).
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
		// The positive derivation, cached (Tracked::Derived): its witnesses. Whether its slots still carry the keys they were derived for
		// is the merge's: the merges before it allocate and key slots.
		const auto& derived = layer ? entry.layerDerived : entry.derived;
		r.derivedWitness = derived.valid && derived.generation == tablesGeneration && derived.geometrySlot == r.geometrySlot && derived.property == r.property &&
		                   derived.material == r.material && derived.fadeState == r.fadeState && derived.technique == accumulated->technique &&
		                   derived.subPass == accumulated->subPass && derived.hint == accumulated->hint && derived.interior == interior &&
		                   derived.alphaBelowOne == r.alphaBelowOne && derived.biasWitness == biasWitness;
		// CS_DCLF_PERSISTENT_PARITY's frames: the cached derivation is served and also recomputed, and the two compared.
		const bool derivedProbe = SwitchEnabled(Switch::PersistentParity) && PassParityDue(13);
		r.classified = a_classify || !r.derivedWitness || derivedProbe;
		Ineligible reason = Ineligible::None;
		if (r.classified)
			reason = layer ? ClassifyLayer(r.leaf, &r.descriptors, accumulated) : ClassifyStatic(r.leaf, &r.descriptors, accumulated);
		// Per frame whether or not the derivation was cached: hidden, part of an actor and fading are
		// states of this frame, and the scene phase's verdict for them is the last classification's
		// (for a static, the last event's). An object that has just been hidden must lose its bindings now,
		// or DCLF keeps drawing what the engine has stopped drawing.
		if (reason == Ineligible::None)
			reason = ClassifyFrame(entry, accumulated);
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
			// A decal's key carries the engine's fixed-function state indices as well (Records.h): the
			// depth-bias mode from the frame, blend and write modes from the alpha property (derived with
			// the descriptors). Zero for everything else, so an opaque key is exactly what it was.
			std::uint32_t rasterFlags = twoSided ? kRasterTwoSided : 0u;
			if (descriptors.decalGroup)
				rasterFlags |= PackDecalRasterFlags(descriptors.decalGroup, decalBiasMode[descriptors.decalGroup & 3], descriptors.decalBlendMode, descriptors.decalWriteMode);
			if (globals::features::extendedTranslucency.loaded)
				rasterFlags |= ((ExtendedTranslucency::MaterialModel::DescriptorDisabled ^ TranslucencyModelOf(r.leaf)) & 7u) << kRasterTranslucencyShift;
			// The geometry slots are the walk's (the joins resolve none: frozen until the merges are done).
			r.key = PipelineKey{ descriptors.vertex, descriptors.pixel, rasterFlags, descriptors.pass,
				VertexLayoutOf(tables.geometries[r.geometrySlot].vertexDesc) };
			// ExternalEmittance::ShouldSuppress: interior, the property's kExternalEmittance, its reference without an emittance source.
			const bool suppress = interior && (propertyRecord->flags & static_cast<std::uint64_t>(PropertyFlag::kExternalEmittance)) && !r.leaf.node->emittance;
			r.staticFlags = (alphaTest ? kObjectAlphaTest : 0u) | (twoSided ? kObjectTwoSided : 0u) |
			                (suppress ? kObjectSuppressExternalEmittance : 0u) |
			                (descriptors.technique == kTechniqueTreeAnim ? kObjectTreeAnim : 0u) |
			                (alphaTest ? static_cast<std::uint32_t>(alpha->threshold) << kObjectAlphaThresholdShift : 0u) |
			                (alphaTest && alphaBlending ? kObjectAlphaBlended : 0u) |
			                (descriptors.decalGroup ? kObjectDecal | (descriptors.decalGroup << kObjectDecalGroupShift) : 0u) |
			                ((propertyRecord->flags & ((1ull << 14) | (1ull << 46))) ? kObjectLandscapeLights : 0u) |
			                (BeastRaceFace(*propertyRecord, r.leaf) ? kObjectBeastRace : 0u);
		}
		// Light Limit Fix's room, when the room map moved on since the entry's (the map the pass took: read-only here).
		if (globals::features::lightLimitFix.loaded && entry.roomMapGeneration != roomMapGeneration) {
			r.roomSet = true;
			r.roomIndex = -1;
			if (roomMap && entry.roomNode)
				if (const auto room = roomMap->find(entry.roomNode); room != roomMap->end())
					r.roomIndex = room->second;
		}
	}

	const char* SceneStore::JoinPreDifference(const JoinPre& a_left, const JoinPre& a_right)
	{
		const auto& l = a_left;
		const auto& r = a_right;
		const std::pair<const char*, bool> fields[] = {
			{ "kind", l.kind == r.kind },
			{ "geometry", l.geometry == r.geometry },
			{ "layer", l.layer == r.layer },
			{ "leaf", l.leaf.node == r.leaf.node && l.leaf.geometry == r.leaf.geometry && l.leaf.property == r.leaf.property && l.leaf.layer == r.leaf.layer &&
			              l.leaf.alpha == r.leaf.alpha && l.leaf.fadeNode == r.leaf.fadeNode },
			{ "derived", l.derived == r.derived },
			{ "pass", l.pass.technique == r.pass.technique && l.pass.subPass == r.pass.subPass && l.pass.passEnum == r.pass.passEnum && l.pass.hint == r.pass.hint &&
			              l.pass.lodRow == r.pass.lodRow && l.pass.sunTest == r.pass.sunTest && l.pass.resident == r.pass.resident &&
			              l.pass.fadeDistance == r.pass.fadeDistance && l.pass.heightTest == r.pass.heightTest },
			{ "fail", l.fail == r.fail },
		};
		for (const auto& [name, same] : fields)
			if (!same)
				return name;
		return nullptr;
	}

	const char* SceneStore::JoinDifference(const JoinResult& a_left, const JoinResult& a_right)
	{
		const auto& l = a_left;
		const auto& r = a_right;
		const auto& ld = l.descriptors;
		const auto& rd = r.descriptors;
		const std::pair<const char*, bool> fields[] = {
			{ "kind", l.kind == r.kind },
			{ "leaf", l.leaf.node == r.leaf.node && l.leaf.geometry == r.leaf.geometry && l.leaf.property == r.leaf.property && l.leaf.layer == r.leaf.layer &&
			              l.leaf.alpha == r.leaf.alpha && l.leaf.fadeNode == r.leaf.fadeNode },
			{ "property", l.property == r.property },
			{ "material", l.material == r.material },
			{ "fadeState", l.fadeState == r.fadeState },
			{ "alphaBelowOne", l.alphaBelowOne == r.alphaBelowOne },
			{ "geometrySlot", l.geometrySlot == r.geometrySlot },
			{ "derivedWitness", l.derivedWitness == r.derivedWitness },
			{ "classified", l.classified == r.classified },
			{ "reason", l.reason == r.reason },
			{ "descriptors", ld.technique == rd.technique && ld.pass == rd.pass && ld.vertex == rd.vertex && ld.pixel == rd.pixel && ld.rawVertex == rd.rawVertex &&
			                     ld.rawPixel == rd.rawPixel && ld.derivedPass == rd.derivedPass && ld.rejectedTechnique == rd.rejectedTechnique &&
			                     ld.decalGroup == rd.decalGroup && ld.decalBlendMode == rd.decalBlendMode && ld.decalWriteMode == rd.decalWriteMode &&
			                     ld.projectedUV == rd.projectedUV },
			{ "key", l.key == r.key },
			{ "staticFlags", l.staticFlags == r.staticFlags },
			{ "room", l.roomSet == r.roomSet && l.roomIndex == r.roomIndex },
		};
		for (const auto& [name, same] : fields)
			if (!same)
				return name;
		return nullptr;
	}

	bool SceneStore::MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const
	{
		const bool layer = tables.IsLayer(a_slot);
		const auto& derived = layer ? a_entry.layerDerived : a_entry.derived;
		// The mirror's records (T6b1b).
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
