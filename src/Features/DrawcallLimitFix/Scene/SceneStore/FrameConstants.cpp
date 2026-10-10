#include "Internal.h"

#include "Features/DrawcallLimitFix/Draws/FrameValues.h"

#include <cmath>

namespace DCLF
{
	namespace
	{
		/**
		 * @brief T6b2c: whether two technique samples feed the technique rows the same (bitwise): everything EvaluateTechnique reads but
		 * the fog, which a row keeps from its making (KeepTechniqueFog; the draws read the frame's, FrameCapture::fog).
		 */
		bool SameTechniqueWitness(const TechniqueInputs& a_a, const TechniqueInputs& a_b)
		{
			auto same = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
			return same(a_a.highDetailRange, a_b.highDetailRange) && a_a.shadowMask == a_b.shadowMask && a_a.shadowMaskFilter == a_b.shadowMaskFilter &&
			       a_a.shadowMaskSized == a_b.shadowMaskSized && same(a_a.shadowMaskInverseSize, a_b.shadowMaskInverseSize) && same(a_a.colourClamp, a_b.colourClamp);
		}

		bool SameTechniqueFloats(const TechniqueConstants& a_a, const TechniqueConstants& a_b) { return a_a.vs.SameBits(a_b.vs) && a_a.ps.SameBits(a_b.ps); }
		bool SameTechniqueBindings(const TechniqueConstants& a_a, const TechniqueConstants& a_b)
		{
			return a_a.filterModes == a_b.filterModes && a_a.shadowMask == a_b.shadowMask && a_a.shadowMaskTexture == a_b.shadowMaskTexture;
		}
	}

	void SceneStore::RefreshMaterialRecords()
	{
		// T6b2c step 7: the scene work, its frame's FrameGlobals bound, the captures drained (ApplyEvents): the written materials' records,
		// the frame components, the texture transforms, in that order (a rewrite keeps the slot's frame parts, which the two after bring
		// to this frame's sources). No engine read: the captures and the frame's sample are the inputs.
		ZoneScopedN("CS.DCLF.Scene.MaterialRecords");
		++materialRecordStats.passes;
		stats.materialWrites = stats.materialsRewritten = stats.materialsHeld = stats.frameMaterialSamples = 0;
		RewriteCapturedMaterials();
		RefreshMaterialSignatures();
		RefreshMaterialTransforms();
	}

	void SceneStore::RewriteCapturedMaterials()
	{
		auto& r = materialRecordStats;
		std::vector<const RE::BSShaderMaterial*> materials = std::exchange(materialsCaptured, {});
		const std::size_t captured = materials.size();
		materials.insert(materials.end(), materialRewritesPending.begin(), materialRewritesPending.end());
		materialRewritesPending.clear();
		if (materials.empty())
			return;
		std::sort(materials.begin(), materials.end());
		materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
		r.captured += captured;
		stats.materialWrites = static_cast<std::uint32_t>(captured);
		const auto& sources = FrameGlobals::Current().material;
		for (const auto* material : materials) {
			const auto dependents = materialDependents.find(material);
			if (dependents == materialDependents.end())
				continue;  // no slot holds it: a join makes its record from the capture
			const auto held = materialSnapshots.find(material);
			if (held == materialSnapshots.end() || !held->second.held)
				continue;  // let go: no slot has used it for kMaterialSnapshotFrames, and its next capture comes with its next write
			held->second.frame = sceneFrame;
			const auto& snapshot = held->second.held->snapshot;
			WatchMaterialTransforms(material, snapshot);
			bool retry = false;
			for (const std::uint32_t slot : dependents->second) {
				if (slot >= tables.materials.size() || !tables.materialSlots.Alive(slot) || tables.materialSlotKey[slot].first != material)
					continue;
				const std::uint32_t pass = tables.materialSlotKey[slot].second;
				MaterialRecord record;
				if (!MaterialPort::Evaluate(snapshot, pass, sources, record)) {
					// An input the frame's sources do not have yet (Advanced Skin's textures for a key it has not set up): again with a
					// later pass's, the slot keeping its record meanwhile. Else a class the port does not cover (counted).
					if (!MaterialPort::FeatureHooksCovered(snapshot, sources.feature))
						retry = true;
					else
						++r.uncovered;
					continue;
				}
				// A record holds no view of the character light's t11; its frame parts (the frame components, TexcoordOffset) are kept
				// by the signature's and the transform watch's writes: only the material's own values are compared and rewritten.
				MaterialSources::StripFrameViews(record, pass);
				MaterialSources::CopyFrameComponents(tables.materials[slot], record, pass);
				MaterialSources::KeepUnreadFloats(tables.materials[slot], record);
				NoteMaterialRecord(slot, snapshot);
				if (record == tables.materials[slot])
					continue;
				tables.materials[slot] = record;
				tables.materialVersion[slot] = ++materialVersions;
				tables.NoteMaterial(slot);
				++r.rewritten;
				++stats.materialsRewritten;
			}
			if (retry) {
				materialRewritesPending.push_back(material);
				++r.retried;
				++stats.materialsHeld;
			}
		}
	}

	void SceneStore::RefreshMaterialSignatures()
	{
		// The frame-sourced components (MaterialSources): the same for every record of a signature, so one port evaluation of the
		// signature's probe against the frame's sources gives them; written into every slot of the signature only when they moved
		// since the last application. A slot keyed since was made from the same sources (the joins evaluate with them).
		auto& r = materialRecordStats;
		const auto& sources = FrameGlobals::Current().material;
		auto keyed = [&](std::uint32_t a_slot, std::uint32_t a_signature) {
			return a_slot < tables.materials.size() && tables.materialSlots.Alive(a_slot) && tables.materialSlotKey[a_slot].first &&
			       MaterialSources::Signature(tables.materialSlotKey[a_slot].second) == a_signature;
		};
		for (auto& [signature, entry] : materialSignatures) {
			if (!entry.probeValid)
				continue;
			MaterialRecord live;
			if (!MaterialPort::Evaluate(entry.probe, entry.probePass, sources, live)) {
				// Not evaluable with these sources (an Advanced Skin key not set up yet): tried again next pass, unless a slot noted
				// meanwhile gives another probe.
				entry.probeFailed = true;
				continue;
			}
			entry.probeFailed = false;
			++r.samples;
			++stats.frameMaterialSamples;
			if (entry.appliedValid) {
				MaterialRecord probe = entry.applied;
				bool floatsChanged = false;
				if (!MaterialSources::ApplyFrameComponents(live, probe, entry.probePass, &floatsChanged) && !floatsChanged)
					continue;
			}
			entry.applied = live;
			entry.appliedValid = true;
			++r.applications;
			auto& slots = entry.slots;
			for (std::size_t i = 0; i < slots.size();) {
				const std::uint32_t slot = slots[i];
				if (!keyed(slot, signature)) {
					if (slot < materialSignatureListed.size() && materialSignatureListed[slot] == signature + 1)
						materialSignatureListed[slot] = 0;
					slots[i] = slots.back();
					slots.pop_back();
					continue;
				}
				bool floatsChanged = false;
				const bool recordChanged = MaterialSources::ApplyFrameComponents(live, tables.materials[slot], tables.materialSlotKey[slot].second, &floatsChanged);
				if (recordChanged)
					tables.materialVersion[slot] = ++materialVersions;
				if (floatsChanged)
					tables.materialFrameVersion[slot] = ++materialVersions;
				if (recordChanged || floatsChanged)
					tables.NoteMaterial(slot);
				++r.slotsApplied;
				++i;
			}
		}
	}

	void SceneStore::RefreshMaterialTransforms()
	{
		// TexcoordOffset: the frame reads one of a material's two buffers (FrameGlobals' textureTransformBuffer, flipped every frame by
		// Main::Update), and a controller's write captures it (MaterialSources). A material is watched from its capture or keying until two
		// passes have gone and both buffers agree; meanwhile its slots take the frame's buffer.
		auto& r = materialRecordStats;
		const std::uint32_t buffer = FrameGlobals::Current().material.vanilla.textureTransformBuffer & 1;
		r.transformsWatched += transformWatch.size();
		for (auto it = transformWatch.begin(); it != transformWatch.end();) {
			const auto* material = it->first;
			const auto& watch = it->second;
			const auto dependents = materialDependents.find(material);
			if (dependents == materialDependents.end()) {
				it = transformWatch.erase(it);
				continue;
			}
			for (const std::uint32_t slot : dependents->second) {
				if (slot >= tables.materials.size() || !tables.materialSlots.Alive(slot) || tables.materialSlotKey[slot].first != material)
					continue;
				if (MaterialSources::ApplyTextureTransform(watch.transforms, buffer, tables.materials[slot])) {
					tables.materialFrameVersion[slot] = ++materialVersions;
					tables.NoteMaterial(slot);
					++r.transformsWritten;
				}
			}
			if (sceneFrame - watch.frame > 2 && watch.transforms.Agree())
				it = transformWatch.erase(it);
			else
				++it;
		}
	}

	void SceneStore::NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot)
	{
		if (a_slot >= tables.materialSlotKey.size())
			return;
		const auto [material, pass] = tables.materialSlotKey[a_slot];
		if (!material)
			return;
		const std::uint32_t signature = MaterialSources::Signature(pass);
		auto& entry = materialSignatures[signature];
		if (materialSignatureListed.size() <= a_slot)
			materialSignatureListed.resize(std::size_t(a_slot) + 1, 0);
		if (materialSignatureListed[a_slot] != signature + 1) {
			materialSignatureListed[a_slot] = signature + 1;
			entry.slots.push_back(a_slot);
		}
		if (!entry.probeValid || entry.probeFailed) {
			entry.probe = a_snapshot;
			entry.probePass = pass;
			entry.probeValid = true;
			entry.probeFailed = false;
		}
		WatchMaterialTransforms(material, a_snapshot);
	}

	void SceneStore::WatchMaterialTransforms(const RE::BSShaderMaterial* a_material, const MaterialPort::MaterialSnapshot& a_snapshot)
	{
		auto& watch = transformWatch[a_material];
		watch.transforms = MaterialSources::TextureTransformsOf(a_snapshot);
		watch.frame = sceneFrame;
	}

	void SceneStore::ResetMaterialRecords()
	{
		for (auto& item : materialSignatures) {
			item.second.slots.clear();
			item.second.appliedValid = false;
		}
		materialSignatureListed.clear();
		transformWatch.clear();
		materialRewritesPending.clear();
	}

	void SceneStore::RefreshCharacterLightView()
	{
		// The frame record's t11 (kCharacterLightRegister) for character-light passes: the frame's sample (no engine read); the last one
		// kept while the sample has none (its target is -1 while a cell loads).
		if (auto* view = MaterialSources::FrameCharacterLightView(FrameGlobals::Current().material))
			frameCapture.characterLightView = view;
	}

	void SceneStore::CheckMaterialFrame()
	{
		const Tables& view = FrameView();
		// CS_DCLF_PERSISTENT_PARITY: the installed tables' used sets against the slots their bound records name. The records' parts
		// against the engine's evaluation are ValidateMaterialSlice's.
		const bool enabled = SwitchEnabled(Switch::PersistentParity);
		if (!enabled || !ParityDue(frame, 45))
			return;
		// The used sets are kept by membership changes. On a parity frame compare them against the slots the bound records
		// name, before trusting their absence to skip descriptor/import work.
		{
			std::vector<std::uint64_t> pipelines(view.usedPipelineBits.size()), materials(view.usedMaterialBits.size());
			auto set = [](std::vector<std::uint64_t>& a_bits, std::uint32_t a_slot) {
				if (a_bits.size() <= a_slot / 64)
					a_bits.resize(a_slot / 64 + 1, 0);
				a_bits[a_slot / 64] |= 1ull << (a_slot % 64);
			};
			for (std::uint32_t o = 0; o < view.objects.size(); ++o)
				if (!(view.objects[o].flags & (kObjectNoBindings | kObjectFree))) {
					set(pipelines, view.objects[o].pipelineIndex);
					set(materials, view.objects[o].materialIndex);
				}
			auto compare = [&](std::vector<std::uint64_t> a_named, const std::vector<std::uint64_t>& a_used, const char* a_name) {
				a_named.resize(std::max(a_named.size(), a_used.size()), 0);
				std::uint32_t missing = 0, extra = 0;
				for (std::size_t word = 0; word < a_named.size(); ++word) {
					const std::uint64_t used = word < a_used.size() ? a_used[word] : 0;
					missing += static_cast<std::uint32_t>(std::popcount(a_named[word] & ~used));
					extra += static_cast<std::uint32_t>(std::popcount(used & ~a_named[word]));
				}
				if (missing || extra)
					logger::warn("[DCLF] {} used set differs from the bound records at frame {}: {} named but not used, {} used but not named", a_name, frame,
						missing, extra);
			};
			compare(std::move(pipelines), view.usedPipelineBits, "pipeline");
			compare(std::move(materials), view.usedMaterialBits, "material");
		}
	}

	void SceneStore::ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (a_material)
			materialDependents[a_material].push_back(a_slot);
	}

	void SceneStore::UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (auto it = materialDependents.find(a_material); it != materialDependents.end()) {
			auto& slots = it->second;
			std::erase(slots, a_slot);
			if (slots.empty())
				materialDependents.erase(it);
		}
	}

	void SceneStore::RefreshFrameConstants()
	{
		const Tables& view = FrameView();
		ZoneNamedN(frameConstantsZone, "CS.DCLF.FrameConstants", true);
		// The material records' frame components are the scene work's (T6b2c step 7: RefreshMaterialSignatures); the frame keeps the
		// character light's view, from its sample.
		RefreshCharacterLightView();
		CheckMaterialFrame();
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		// T6b2b: the frame's pipeline inputs (GeometryPort::PipelineFrame), sampled once, here: the eye is the current accumulator's
		// less the shadow state's posAdjust, the main camera's only from Prepass (LatchAccumulator's point; at EarlyPrepass both are
		// still the shadow-map camera's), and the engine's evaluation the port replaces ran here. The held blocks were made from
		// geometryPortFrame (the last sample with a sun), so the pipeline inputs moved when this sample's differ from it. A sample
		// without a sun makes nothing and is not kept. A sample with one is posted to the coordinator too (T6b2c step 8), which makes
		// its new pipelines' blocks from the newest (MakeNewPipelineConstants); a slot new to the frame's tables is made whole below.
		const GeometryPort::PipelineFrame portFrame = GeometryPort::SamplePipelineFrame();
		++geometryStats.samples;
		const bool pipelineInputsMoved = portFrame.sun && (!geometryPortFrame || !portFrame.SamePipelineInputs(*geometryPortFrame));
		if (portFrame.sun) {
			geometryPortFrame = portFrame;
			geometryPortFrameNumber = frame;
			PostPipelineFrame(portFrame);
		}
		// The frame lighting, the frame's (GeometryPort::MergeFrameLighting: the rows every pipeline that writes them shares) over what
		// was last published (nothing without a sun: it keeps its value); versioned only when it differs. On a parity frame each
		// pipeline's engine evaluation is kept to compare with what was published.
		FrameLighting frameLighting;
		std::memcpy(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting));
		std::uint32_t lightingWritten = 0;
		GeometryPort::MergeFrameLighting(portFrame, frameLighting, lightingWritten);
		// The frame's fog (FrameFog, what the DCLF_BINDLESS vertex stage reads from VS b13): the frame's sample's, as every technique
		// writes it alike (EvaluateTechnique's fog block depends on no pass descriptor), over the last frame's where it writes none.
		// The technique rows keep the fog they were made with, so the time of day versions none of them. No engine read (T6b2c).
		FrameFog frameFog = frameCapture.fog;
		std::uint32_t fogWritten = 0;
		{
			TechniqueConstants fog;
			EvaluateTechnique(0, fog);
			MergeFrameFog(fog, frameFog, fogWritten);
		}
		std::vector<std::pair<std::uint32_t, GeometryConstants>> lightingReferences;
		const bool geometryParityEnabled = SwitchEnabled(Switch::PersistentParity);
		const bool geometryParityFrame = geometryParityEnabled && ParityDue(frame, 30);
		geometryStats.checks += geometryParityFrame ? 1u : 0u;
		frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, frameTablesGeneration);
		// CS_DCLF_PERSISTENT_PARITY: each technique row the frame draws (the frame's tables: the coordinator's rows, T6b2c), once, against
		// an evaluation from the engine's values now. A row differs late when the live sample's witness moved since the row's write (a
		// change reaches the draws with the next publication); otherwise it differs.
		std::optional<TechniqueInputs> techniqueLive;
		std::vector<std::uint8_t> techniqueChecked;
		if (geometryParityFrame) {
			techniqueLive = SampleTechniqueInputs();
			techniqueChecked.assign(view.techniqueConstants.size(), 0);
		}
		TracyCZoneN(pipelinesZone, "CS.DCLF.FrameConstants.Pipelines", true);
		std::int64_t pipelinesVisited = 0;
		for (std::size_t word = 0; word < view.usedPipelineBits.size(); ++word) {
			for (std::uint64_t remaining = view.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t i = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (i >= view.pipelines.size())
					continue;
				++pipelinesVisited;
				if (const std::uint32_t r = i < view.pipelineTechnique.size() ? view.pipelineTechnique[i] : ~0u;
					techniqueLive && r < techniqueChecked.size() && !techniqueChecked[r] && view.techniqueConstants[r].valid) {
					techniqueChecked[r] = 1;
					const auto& row = view.techniqueConstants[r];
					TechniqueConstants reference;
					EvaluateTechnique(view.pipelines[i].passDescriptor, *techniqueLive, reference);
					KeepTechniqueFog(row.value, reference);
					++geometryStats.techniquesChecked;
					if (!SameTechniqueFloats(reference, row.value) || !SameTechniqueBindings(reference, row.value)) {
						if (!SameTechniqueWitness(row.inputs, *techniqueLive)) {
							++geometryStats.techniquesLate;
						} else if (geometryStats.techniquesDiffer++ == 0) {
							geometryStats.techniqueFirst = fmt::format("row {} (key {:#x}, pass {:08X}): {}{}", r, view.techniqueKeys[r], view.pipelines[i].passDescriptor,
								SameTechniqueFloats(reference, row.value) ? "" : "floats ", SameTechniqueBindings(reference, row.value) ? "" : "bindings");
						}
					}
				}
				// T6b2b: a pipeline's PerGeometry block is the port's (GeometryPort::PipelineGeometryConstants) from the frame's sample,
				// no engine code run. Made whole when the slot is new (or keyed again), at the first refresh, or when the pipeline inputs
				// moved (PipelineFrame::SamePipelineInputs: SSRParams.xyz, the world map rows); otherwise what moves every frame is
				// written in place (GeometryPort::RefreshFrameValues: the sun, the ambient, the ambient specular, and EyePosition by the
				// port's rule, True PBR's passes included). Those are kept in the block for the constant-buffer path and the checks, but
				// no DCLF_BINDLESS draw reads them there (DCLFFrameLighting; kPSBindlessGeometryUnread), so only what such a draw reads
				// versions and posts the pipeline (SameBindlessGeometry). No template member or pass is needed: a pipeline whose
				// template pass is missing is covered too. Without a sun nothing is made (the engine had no template pass then either):
				// the held block is kept.
				if (!portFrame.sun)
					continue;
				const std::uint32_t passDescriptor = view.pipelines[i].passDescriptor;
				auto& held = frameTables.geometryConstants[i];
				bool ownChanged = false;
				if (!constantsRefreshed || !frameTables.geometryConstantsValid[i] || pipelineInputsMoved) {
					GeometryConstants constants;
					if (!GeometryPort::PipelineGeometryConstants(passDescriptor, kMainPassRenderFlags, portFrame, constants))
						continue;
					++geometryStats.full;
					// Its own values: what a bindless draw reads from the block (PackGeometryTemplate's mask).
					ownChanged = !frameTables.geometryConstantsValid[i] || !SameBindlessGeometry(constants, held);
					held = constants;
				} else {
					GeometryPort::RefreshFrameValues(passDescriptor, portFrame, held);
				}
				if (ownChanged) {
					frameTables.pipelineConstantsVersion[i] = frameTables.NextVersion();
					++geometryStats.changed;
					PostPipelineConstants(i);
				}
				frameTables.geometryConstantsValid[i] = 1;
				// CS_DCLF_PERSISTENT_PARITY: the engine's evaluation (BSLightingShader::SetupGeometry on the synthetic template pass,
				// TemplatePassOf: the parity's only use of it) as the reference. The held block against it, in what no object overrides
				// (CheckFrameGeometry: the producer's refresh rules); the port made now against it (Differences: the port itself); the
				// frame lighting against each reference after the loop, where the reference writes it. A pipeline with no template
				// member has no reference (made by the port all the same).
				if (geometryParityFrame) {
					GeometryConstants reference;
					const auto* templatePass = TemplatePassOf(view, i);
					if (templatePass && evaluator.EvaluateGeometry(*templatePass, view.pipelines[i].passDescriptor, kMainPassRenderFlags, reference)) {
						CheckFrameGeometry(static_cast<std::uint32_t>(i), reference, held);
						// T6b2b: the port from the frame's sample (taken above, at this point, where the reference runs), against the reference.
						auto& gp = geometryPortParity;
						GeometryConstants port;
						if (!GeometryPort::PipelineGeometryConstants(view.pipelines[i].passDescriptor, kMainPassRenderFlags, portFrame, port)) {
							if (gp.uncovered++ == 0 && gp.first.empty())
								gp.first = fmt::format("uncovered: pipeline {} (pass {:08X})", i, view.pipelines[i].passDescriptor);
						} else {
							++gp.checked;
							if (auto difference = GeometryPort::Differences(port, reference); !difference.empty())
								if (gp.differ++ == 0 || gp.first.starts_with("uncovered"))
									gp.first = fmt::format("pipeline {} (pass {:08X}): {}", i, view.pipelines[i].passDescriptor, difference);
						}
						lightingReferences.emplace_back(static_cast<std::uint32_t>(i), reference);
						// T6: against the evaluation from a pass the engine registered for the template's property (or the frame's).
						if (const auto* registered = RegisteredTemplatePassOf(i < view.geometryTemplate.size() ? view.geometryTemplate[i] : nullptr)) {
							GeometryConstants engine;
							if (evaluator.EvaluateGeometry(*registered, view.pipelines[i].passDescriptor, kMainPassRenderFlags, engine)) {
								++geometryStats.templateChecked;
								// PS NumLightNumShadowLight.x is the registered pass's own point lights (its object's, the sun not counted):
								// per object, and unread with Light Limit Fix (only .y, the shadow lights, is).
								const std::uint32_t lightCount = LightingPSLayout().offset[kPSNumLights];
								engine.ps.floats[lightCount] = reference.ps.floats[lightCount];
								const bool own = SameBindlessGeometry(engine, reference);
								FrameLighting engineLighting{};
								std::uint32_t engineWritten = 0;
								MergeFrameLighting(engine.ps, engineLighting, engineWritten);
								const bool lighting = MatchesFrameLighting(reference.ps, engineLighting, nullptr);
								if (!own || !lighting) {
									++(own ? geometryStats.templateLightingDiffer : geometryStats.templateDiffer);
									if (geometryStats.templateFirst.empty())
										geometryStats.templateFirst = fmt::format("pipeline {} (descriptor {:#x}): {}", i, view.pipelines[i].passDescriptor,
											own ? std::string("frame lighting") : BindlessGeometryDifferences(engine, reference));
								}
							}
						} else {
							++geometryStats.templateMissing;
						}
					}
				}
			}
		}
		TracyCZoneEnd(pipelinesZone);
		TracyPlot("CS.DCLF.FrameConstants.Pipelines", pipelinesVisited);
		// The pipeline inputs moved: a slot no member names this frame (not visited) holds a block of the older inputs; it is made
		// again (and posted) at the first Prepass that visits it, so every held block is of the inputs it is refreshed with.
		if (pipelineInputsMoved)
			for (std::size_t p = 0; p < frameTables.geometryConstantsValid.size(); ++p)
				if (p / 64 >= view.usedPipelineBits.size() || !((view.usedPipelineBits[p / 64] >> (p % 64)) & 1))
					frameTables.geometryConstantsValid[p] = 0;
		if (fogWritten)
			frameCapture.fog = frameFog;
		if (lightingWritten && std::memcmp(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting)) != 0) {
			std::memcpy(frameCapture.lighting.data(), frameLighting.data(), sizeof(frameLighting));
			++geometryStats.lightingVersions;
		}
		for (const auto& [pipeline, reference] : lightingReferences) {
			++geometryStats.lightingChecked;
			std::string first;
			if (MatchesFrameLighting(reference.ps, frameLighting, geometryStats.lightingFirst.empty() ? &first : nullptr))
				continue;
			++geometryStats.lightingDiffer;
			if (!first.empty())
				geometryStats.lightingFirst = fmt::format("pipeline {} (pass {:X}) {}", pipeline, view.pipelines[pipeline].passDescriptor, first);
		}
		++geometryStats.frames;

		// The objects' shading is FrameValues' (BindlessShading), sampled at the frame's start for the slots the walk names by their
		// events (NameShadingEvents), and their extras' static rows are written by the same events: nothing of either is sampled here.
		// Every pipeline was made whole once: from the next frame, the frame values in place unless the pipeline inputs move.
		constantsRefreshed = true;
		++shadingParity.frames;
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(frame) && lodFadeEventsInstalled) {
			auto& ep = extrasParity;
			// The engine's globals and routines under leases, an item each (T6b1d: the scene work may run beside the engine's update).
			const auto frameInputs = [] {
				const EngineReadWindow::Lease lease;
				return lease ? SampleExtrasFrame() : ExtrasFrame{};
			}();
			std::array<float, kExtraRows * 4> reference{}, completed{};
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !(tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) ||
					!tables.objectGeometry[o] || o >= tables.extraOffset.size() || tables.extraOffset[o] == kNoExtraRows)
					continue;
				float* held = &tables.extraRows[std::size_t(tables.extraOffset[o]) * 4];
				const std::array<float, kExtraRows * 4> before = [&] { std::array<float, kExtraRows * 4> copy; std::memcpy(copy.data(), held, sizeof(copy)); return copy; }();
				if (WriteObjectExtras(o)) {
					++ep.staleStatic;
					std::memcpy(held, before.data(), sizeof(before));
				}
				const EngineReadWindow::Lease lease;
				if (!lease || !ReferenceExtras(o, reference.data()))
					continue;
				BindlessPlacement placement;
				if (!FrameValues::SampleSlot(tables, o, placement))
					continue;
				CompleteExtras(held, tables.objects[o].flags, placement.world, frameInputs, completed.data());
				++ep.objects;
				float worst = 0.0f;
				for (std::size_t c = 0; c < completed.size(); ++c)
					worst = std::max(worst, std::abs(completed[c] - reference[c]));
				ep.maxDifference = std::max(ep.maxDifference, worst);
				// TextureProj's translation column: the engine subtracts the eye and adds it back, the draw does not (an ulp of the
				// world position).
				if (worst > 0.05f && ep.differ++ == 0) {
					const auto* geometry = tables.objectGeometry[o];
					ep.first = fmt::format("slot {} '{}' (flags {:#x}): differs by {}", o, geometry->name.c_str() ? geometry->name.c_str() : "?", tables.objects[o].flags, worst);
				}
			}
		}
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(frame))
			CheckShadingParity();
	}

	void SceneStore::CheckFrameGeometry(std::uint32_t a_pipeline, const GeometryConstants& a_reference, const GeometryConstants& a_held)
	{
		auto& g = geometryStats;
		++g.pipelinesChecked;
		// The frame's snapshot, whose slots RefreshFrameConstants visits (the coordinator's tables may have moved on).
		const std::uint32_t passDescriptor = FrameView().pipelines[a_pipeline].passDescriptor;
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& layout = stage ? LightingPSLayout() : LightingVSLayout();
			const auto& a = stage ? a_reference.ps : a_reference.vs;
			const auto& b = stage ? a_held.ps : a_held.vs;
			// T6b2b: the port's rule, True PBR's passes included (the engine writes their eye: they gain AmbientSpecular in True PBR's
			// hook; LightingConstants.h's WritesEyePosition misses them, so the check skipped them, and the held eye was never refreshed).
			const bool writesEye = GeometryPort::PassWritesEyePosition(passDescriptor);
			const std::uint64_t skip = stage ? kObjectGeometryPS : (kObjectGeometryVS | (writesEye ? 0ull : 1ull << kVSEyePosition));
			const std::uint64_t perGeometry = stage ? kPSGroups[kPerGeometry] : kVSGroups[kPerGeometry];
			for (std::uint32_t v = 0; v < layout.count; ++v) {
				if ((skip & (1ull << v)) || !(perGeometry & (1ull << v)))
					continue;
				if (std::memcmp(&a.floats[layout.offset[v]], &b.floats[layout.offset[v]], layout.size[v] * sizeof(float)) == 0)
					continue;
				++g.differ[stage][v];
				if (g.first.empty())
					g.first = fmt::format("pipeline {} (pass {:X}) {}{}: {} against {}", a_pipeline, passDescriptor, stage ? "PS" : "VS", v,
						a.floats[layout.offset[v]], b.floats[layout.offset[v]]);
			}
		}
	}

	void SceneStore::NameShading(std::uint32_t a_slot, bool a_member)
	{
		if (a_slot >= tables.objects.size() || a_slot >= tables.objectGeometry.size() || (tables.objects[a_slot].flags & (kObjectNoBindings | kObjectFree)))
			return;
		auto* property = SlotProperty(a_slot);
		if (!property)
			return;
		auto& item = shadingNamed.emplace_back();
		item.property.reset(property);
		item.slot = a_slot;
		item.pass = tables.pipelines[tables.objects[a_slot].pipelineIndex].passDescriptor;
		item.member = a_member;
		item.actor = tables.actorWetness.Grouped(a_slot);
	}

	void SceneStore::NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots)
	{
		for (const void* key : a_keys) {
			const auto dependents = propertyDependents.find(key);
			if (dependents == propertyDependents.end())
				continue;
			for (auto* geometry : dependents->second) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.objectStamp != objectStamp)
					continue;
				for (const std::uint32_t slot : { it->second.slot, it->second.layerSlot }) {
					if (slot == kNoObjectSlot || slot >= tables.objects.size())
						continue;
					NameShading(slot, IsResidentSlot(slot));
					// The extras' static rows read the same property fields (ProjectedUV's parameters and colour).
					if ((tables.objects[slot].flags & (kObjectProjectedUV | kObjectLandBlend)) && WriteObjectExtras(slot))
						tables.NoteChange(slot, kChangeExtras);
					++shadingParity.resampled;
					if (a_slots)
						a_slots->push_back(slot);
				}
			}
		}
	}

	void SceneStore::NameShadingEvents(std::vector<std::uint32_t>* a_slots)
	{
		// The events: the controllers' writes of the emissive colour and multiplier and of material fields (MaterialSources), the
		// LOD fades and the alpha GetRenderPasses leaves on the property (an actor's fade among them; lodFadeEvents), and external
		// emittance's shared colour (emittanceEvents). Their dependents are sampled at the next frame's start: a value that changes
		// during this frame (a controller on the animation job, a cull's GetRenderPasses) is drawn from the next frame on, a frame
		// after the engine's own draw would show it (dclf-async-publication.md, "Accepted differences").
		lodFadeChanged.clear();
		DrainLodFadeEvents(lodFadeChanged);
		const std::size_t lodFades = lodFadeChanged.size();
		DrainEmittanceEvents(lodFadeChanged);
		shadingParity.emittanceEvents += lodFadeChanged.size() - lodFades;
		MaterialSources::DrainShadingChanges(lodFadeChanged);
		if (!shadingNamedAll || !lodFadeEventsInstalled) {
			// Every slot (the events are not installed: every walk).
			shadingNamedAll = true;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				NameShading(o, IsResidentSlot(o));
				if ((tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) && WriteObjectExtras(o))
					tables.NoteChange(o, kChangeExtras);
			}
			shadingParity.resampled += tables.objects.size();
			return;
		}
		std::sort(lodFadeChanged.begin(), lodFadeChanged.end());
		lodFadeChanged.erase(std::unique(lodFadeChanged.begin(), lodFadeChanged.end()), lodFadeChanged.end());
		shadingParity.lodFadeEvents += lodFadeChanged.size();
		NameShadingKeys(lodFadeChanged, a_slots);
	}

	std::vector<SceneStore::WetnessValue> SceneStore::CaptureWetness()
	{
		// Advanced Skin's wetness, per actor-owned object. Skin::GetWetness keeps each actor's fading state and computes it once a
		// frame (the first call; later ones, including its own SetupGeometry hook for the draws the engine still makes this frame,
		// return the same value), so calling it here for every actor in the tables advances every actor's fade once a frame,
		// whether or not the engine draws it. Its cache is not thread-safe: the render thread's, at the frame's start.
		ZoneScopedN("CS.DCLF.Capture.Wetness");
		std::vector<WetnessValue> values;
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		// T6b3a: the actors' membership is the render thread's own index (the log's changes, HandOverAtFrameStart), their geometries the
		// newest publication's (held by it), never the coordinator's tables.
		const auto latest = Newest();
		const Tables* source = latest ? latest->tables.get() : nullptr;
		const auto wetnessStats = frameActorWetness.Update([&](std::uint32_t o) {
			auto* geometry = source && o < source->objectGeometry.size() ? source->objectGeometry[o] : nullptr;
			const float4 value = wetness && geometry ? skin.GetWetness(geometry) : float4{};
			return ActorValueIndex::Value{ value.x, value.y, value.z, value.w };
		}, [&](std::uint32_t o, const auto& row) { values.push_back({ o, row }); });
		TracyPlot("CS.DCLF.Wetness.Actors", static_cast<std::int64_t>(wetnessStats.actors));
		TracyPlot("CS.DCLF.Wetness.ChangedActors", static_cast<std::int64_t>(wetnessStats.changed));
		TracyPlot("CS.DCLF.Wetness.VisitedMeshes", static_cast<std::int64_t>(wetnessStats.visited));
		return values;
	}

	void SceneStore::CheckShadingParity()
	{
		auto& sp = shadingParity;
		// A diagnostics frame: waits for the frame's values.
		const auto* rows = FrameValues::Get().ShadingIfDone(true);
		if (!rows || !lodFadeEventsInstalled)
			return;
		++sp.checks;
		// Named for the next frame: by the walk (its events) and the accumulate phase, this frame.
		std::vector<std::uint8_t> named(tables.objects.size(), 0);
		for (const auto& item : PeekShadingItems())
			if (item.slot < named.size())
				named[item.slot] = 1;
		std::vector<std::uint32_t> changed;
		for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size() && o < rows->size(); ++o) {
			if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !tables.objectGeometry[o])
				continue;
			const auto* property = static_cast<const RE::BSLightingShaderProperty*>(SlotProperty(o));
			if (!property)
				continue;
			++sp.slots;
			BindlessShading now = (*rows)[o];
			SampleShading(*property, tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor, IsResidentSlot(o), now);
			// The tree's wind is the row's as the slot was named (a member's until its tree's entry: T6b1a), not kept current: not compared.
			if (std::memcmp(&now, &(*rows)[o], offsetof(BindlessShading, treeParams)) != 0)
				changed.push_back(o);
		}
		// A write after the walk took the events (the animation job runs the controllers alongside the render thread; the culls'
		// GetRenderPasses) is not missed: its event is queued. Those events are taken now and their dependents named for the next
		// frame, so each changed slot is named, queued or missed.
		std::vector<const void*> late;
		DrainLodFadeEvents(late);
		DrainEmittanceEvents(late);
		MaterialSources::DrainShadingChanges(late);
		std::sort(late.begin(), late.end());
		late.erase(std::unique(late.begin(), late.end()), late.end());
		std::vector<std::uint32_t> covered;
		NameShadingKeys(late, &covered);
		std::sort(covered.begin(), covered.end());
		for (const std::uint32_t o : changed) {
			if (named[o]) {
				++sp.named;
				continue;
			}
			if (std::binary_search(covered.begin(), covered.end(), o)) {
				++sp.late;
				continue;
			}
			if (sp.missing++ == 0) {
				const auto* geometry = tables.objectGeometry[o];
				const auto& held = (*rows)[o];
				BindlessShading now = held;
				SampleShading(*static_cast<const RE::BSLightingShaderProperty*>(SlotProperty(o)), tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor,
					IsResidentSlot(o), now);
				sp.first = fmt::format("slot {} '{}' (flags {:#x}): material data ({} {} {}) against ({} {} {}), emit ({} {} {}) against ({} {} {}), mult {} against {}",
					o, geometry->name.c_str() ? geometry->name.c_str() : "", tables.objects[o].flags, now.shading.materialData[0], now.shading.materialData[1],
					now.shading.materialData[2], held.shading.materialData[0], held.shading.materialData[1], held.shading.materialData[2], now.shading.emitColor[0],
					now.shading.emitColor[1], now.shading.emitColor[2], held.shading.emitColor[0], held.shading.emitColor[1], held.shading.emitColor[2], now.emissiveMult,
					held.emissiveMult);
			}
		}
		// The wetness, as the frame's start captured it (Skin::GetWetness's same-frame value: no second advance): a mesh named this
		// frame (joined, or its record rewritten) takes its value with the next frame's capture.
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		for (const auto o : tables.actorObjects) {
			if (o >= rows->size() || o >= named.size() || named[o] || !tables.objectGeometry[o])
				continue;
			++sp.wetness;
			const float4 value = wetness ? skin.GetWetness(tables.objectGeometry[o]) : float4{};
			const float expected[4] = { value.x, value.y, value.z, value.w };
			sp.wetnessDiffer += std::memcmp(expected, (*rows)[o].skinPerGeometry, sizeof(expected)) != 0;
		}
	}

	bool SceneStore::WriteObjectExtras(std::uint32_t a_object)
	{
		if (a_object >= tables.extraOffset.size() || tables.extraOffset[a_object] == kNoExtraRows || a_object >= tables.objectGeometry.size())
			return false;
		const auto& object = tables.objects[a_object];
		const auto* geometry = tables.objectGeometry[a_object];
		// The records (T6b1b): the slot's property's (its layer's for a layer slot) and the geometry's.
		const auto leaf = geometry ? mirror.Leaf(geometry) : SceneCapture::LeafView{};
		const auto* property = tables.IsLayer(a_object) ? leaf.layer : leaf.property;
		if (!geometry || !leaf || !property || (object.flags & kObjectNoBindings))
			return false;
		// What of the rows is the object's alone (CompleteExtras adds the frame's): the land blend's material offset; how its
		// TextureProj is made, and a multi-index shape's own; the ProjectedUV parameters.
		std::array<float, kExtraRows * 4> rows{};
		if (object.flags & kObjectLandBlend) {
			rows[kExtraRowLandBlend * 4 + 0] = property->landBlend[0];  // the landscape material's landBlendParams
			rows[kExtraRowLandBlend * 4 + 1] = property->landBlend[1];
		}
		if (object.flags & kObjectProjectedUV) {
			// A multi-index shape's parameters (GeometryRecord::multiParams: materialProjection, materialParams, materialScale,
			// normalDampener).
			const bool multiIndex = leaf.Type() == static_cast<std::uint8_t>(RE::BSGeometry::Type::kMultiIndexTriShape);
			const auto& m = leaf.geometry->multiParams;
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (!(object.flags & kObjectLandBlend))
				rows[kExtraRowLandBlend * 4] = multiIndex ? kTextureProjShape : technique == 1 ? kTextureProjProjection : kTextureProjWorld;
			// A multi-index shape's passes take the shape's own projection (SetupGeometry's ProjectedUV block, engine notes):
			// materialProjection's columns, as stored.
			if (multiIndex) {
				float* proj = rows.data() + kExtraRowTextureProj * 4;
				for (std::uint32_t r = 0; r < 3; ++r) {
					proj[r * 4 + 0] = m[0 * 4 + r];  // materialProjection.m[column][row], as stored
					proj[r * 4 + 1] = m[1 * 4 + r];
					proj[r * 4 + 2] = m[2 * 4 + r];
					proj[r * 4 + 3] = m[3 * 4 + r];
				}
			}
			// The pixel parameters (FUN_1414e00c0): the property's projectedUVParams folded by its w, its projectedUVColor (a
			// multi-index shape's materialParams, normalDampener and materialScale); the globals are the frame's (ExtrasFrame).
			const std::array<float, 4> params = multiIndex ? std::array<float, 4>{ m[16], m[17], m[18], m[19] } :
			                                                 std::array<float, 4>{ property->projected[0], property->projected[1], property->projected[2], property->projected[3] };
			float* out = rows.data() + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params[3];
			out[0] = fade * params[0];
			out[1] = 0.0f;  // never written by the engine
			out[2] = params[2];
			out[3] = fade * params[1] + params[3];
			if (multiIndex) {
				out[4] = m[21];  // normalDampener
				out[5] = m[20];  // materialScale
			} else {
				out[4] = property->projected[4];  // projectedUVColor
				out[5] = property->projected[5];
				out[6] = property->projected[6];
				out[7] = property->projected[7];
			}
		}
		float* held = &tables.extraRows[std::size_t(tables.extraOffset[a_object]) * 4];
		if (std::memcmp(held, rows.data(), sizeof(rows)) == 0)
			return false;
		std::memcpy(held, rows.data(), sizeof(rows));
		tables.NoteExtrasBlock(tables.extraOffset[a_object]);
		return true;
	}

	bool SceneStore::ReferenceExtras(std::uint32_t a_object, float* a_out)
	{
		EngineReadWindow::Touch("SceneStore::ReferenceExtras");
		const auto& object = tables.objects[a_object];
		const auto* geometry = a_object < tables.objectGeometry.size() ? tables.objectGeometry[a_object] : nullptr;
		const auto* property = static_cast<const RE::BSLightingShaderProperty*>(SlotProperty(a_object));
		if (!geometry || !property || (object.flags & kObjectNoBindings))
			return false;
		std::fill_n(a_out, kExtraRows * 4, 0.0f);
		const auto eye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
		const auto frameInputs = SampleExtrasFrame();
		if (object.flags & kObjectLandBlend) {
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(property->material);
			float* land = a_out + kExtraRowLandBlend * 4;
			land[0] = material ? material->landBlendParams.red : 0.0f;
			land[1] = material ? material->landBlendParams.green : 0.0f;
			land[2] = frameInputs.landBlend[0] - geometry->world.translate.x;
			land[3] = frameInputs.landBlend[1] - geometry->world.translate.y;
		}
		if (object.flags & kObjectProjectedUV) {
			// The texture matrix as SetupGeometry builds it for a ProjectedUV pass (engine notes), through the engine's own routines:
			// for every technique but Envmap the projection multiplied onto the geometry's world matrix, converted the same way and with
			// posAdjust added back.
			using ToMatrix = void (*)(float*, const RE::NiTransform*);
			using Multiply = void* (*)(float*, const float*, const float*);
			static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(0x14aaf10) };
			static const REL::Relocation<Multiply> multiply{ REL::Offset(0x153d3c8) };
			const float* p = frameInputs.projection;
			float m[16];
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (technique == 1) {
				std::memcpy(m, p, sizeof(m));
			} else {
				float w[16];
				toMatrix(w, &geometry->world);
				w[12] += eye.x;
				w[13] += eye.y;
				w[14] += eye.z;
				multiply(m, w, p);
			}
			float* proj = a_out + kExtraRowTextureProj * 4;
			const auto* multiIndex = const_cast<RE::BSGeometry*>(geometry)->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape ?
			                             &static_cast<const RE::BSMultiIndexTriShape*>(geometry)->GetMultiIndexTrishapeRuntimeData() :
			                             nullptr;
			for (std::uint32_t r = 0; r < 3; ++r)
				for (std::uint32_t c = 0; c < 4; ++c)
					proj[r * 4 + c] = multiIndex ? multiIndex->materialProjection.m[c][r] : m[c * 4 + r];
			const auto& params = multiIndex ? multiIndex->materialParams : property->projectedUVParams;
			const auto& colour = property->projectedUVColor;
			float* out = a_out + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params.alpha;
			out[0] = fade * params.red;
			out[2] = params.blue;
			out[3] = fade * params.green + params.alpha;
			if (multiIndex) {
				out[4] = multiIndex->normalDampener;
				out[5] = multiIndex->materialScale;
			} else {
				out[4] = colour.red;
				out[5] = colour.green;
				out[6] = colour.blue;
				out[7] = colour.alpha;
			}
			std::memcpy(out + 8, frameInputs.projectedGlobals, sizeof(frameInputs.projectedGlobals));
		}
		return true;
	}

	void SceneStore::NoteProjectedTextures()
	{
		auto& state = globals::game::shadowState->GetRuntimeData();
		ProjectedTextures seen{};
		for (std::size_t i = 0; i < ProjectedTextures::kSlots.size(); ++i) {
			seen.views[i] = reinterpret_cast<ID3D11ShaderResourceView*>(state.PSTexture[ProjectedTextures::kSlots[i]]);
			if (!seen.views[i])
				return;
		}
		seen.valid = true;
		if (projectedTextures.valid && projectedTextures.views == seen.views)
			return;
		projectedTextures = seen;
		// To the scene work, which asks for their bindings (T6b2c: SharedBindings): each view referenced here, where the engine has it
		// bound, so it outlives the scene work's request. The newest capture wins.
		auto capture = std::make_shared<SharedBindings::ProjectedCapture>();
		for (std::size_t i = 0; i < seen.views.size(); ++i)
			capture->views[i].copy_from(seen.views[i]);
		sharedBindings.projectedPosted.store(std::move(capture), std::memory_order_release);
	}

	void SceneStore::MaterialReference::reset(RE::BSShaderMaterial* a_material)
	{
		// BSIntrusiveRefCounted's count (+0x8), incremented as the engine takes a reference on a property's material
		// (FUN_1414ac820); a material being read off a live property has one already, so it cannot be at zero here.
		if (a_material)
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<std::byte*>(a_material) + 0x8));
		if (material) {
			using Release = void(void*, RE::BSShaderMaterial*);
			static REL::Relocation<Release*> release{ REL::Offset(0x14f7a40) };  // AE ID 107720
			static REL::Relocation<void**> manager{ REL::Offset(0x3187758) };     // AE ID 403555
			release(*manager, material);
		}
		material = a_material;
	}

	bool SceneStore::ProfileEnabled()
	{
		return SwitchEnabled(Switch::Profile);
	}

	bool SceneStore::PortMaterial(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass, MaterialRecord& a_out)
	{
		if (!a_material)
			return false;
		// The frame's sources, as the scene work's records take them (FrameGlobals, sampled at the frame's start).
		MaterialPort::MaterialSnapshot snapshot;
		if (!MaterialPort::Capture(*a_material, snapshot) || !MaterialPort::Evaluate(snapshot, a_pass, FrameGlobals::Current().material, a_out)) {
			auto& p = materialPortParity;
			if (p.uncovered++ == 0 && p.first.empty())
				p.first = fmt::format("uncovered: material {} (feature {}, {}), pass {:08X}", fmt::ptr(a_material), snapshot.feature, snapshot.pbr ? "PBR" : "vanilla", a_pass);
			return false;
		}
		if (SwitchEnabled(Switch::PersistentParity))
			CheckMaterialPort(a_material, a_pass, a_out, snapshot);
		return true;
	}

	void SceneStore::CheckMaterialPort(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass, const MaterialRecord& a_port, const MaterialPort::MaterialSnapshot& a_snapshot,
		const MaterialRecord* a_engine)
	{
		// The observer: the engine's SetupMaterial (the stand-in evaluation), at the same point, against the port's record (the caller's
		// evaluation when it has one: ValidateMaterialSlice's).
		auto& p = materialPortParity;
		MaterialRecord engine;
		if (a_engine)
			engine = *a_engine;
		else if (!ConstantEvaluator::Get().EvaluateMaterial(a_material, a_pass, engine))
			return;
		++p.checked;
		// IBLParams (PS 29): the shader object's, which moves within a frame (the port's is the frame's first record's), and which no
		// Lighting stage reads (MaterialSources): counted apart, not compared.
		constexpr std::uint32_t kPSIBLParams = 29;
		const auto& ps = LightingPSLayout();
		MaterialRecord port = a_port;
		// The character light's t11 is the frame's whole (its target flips within a frame; records take it from the frame:
		// ApplyFrameComponents): not compared.
		if (MaterialSources::FrameCharacterLight(a_pass)) {
			constexpr std::uint32_t kT11 = 11;
			port.textures[kT11] = engine.textures[kT11];
			port.addressModes[kT11] = engine.addressModes[kT11];
			port.filterModes[kT11] = engine.filterModes[kT11];
			port.textureWritten = (port.textureWritten & ~(1u << kT11)) | (engine.textureWritten & (1u << kT11));
		}
		for (std::uint32_t c = 0; c < ps.size[kPSIBLParams]; ++c) {
			const std::uint32_t f = ps.offset[kPSIBLParams] + c;
			if (!port.ps.SameBits(engine.ps, f)) {
				++p.iblDrift;
				port.ps.floats[f] = engine.ps.floats[f];
			}
		}
		if (auto difference = MaterialPort::DescribeDifference(port, engine, &p.kinds); !difference.empty())
			if (p.differ++ == 0 || p.first.starts_with("uncovered"))
				p.first = fmt::format("material {} (feature {}, {}), pass {:08X}: {}", fmt::ptr(a_material), a_snapshot.feature, a_snapshot.pbr ? "PBR" : "vanilla", a_pass,
					difference);
	}

	void SceneStore::NoteStaleMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key,
		const MaterialRecord& a_served, const MaterialRecord& a_live)
	{
		++stats.materialCacheStale;
		if (!a_served.vs.SameBits(a_live.vs))
			stats.materialDiffMask |= 1u << 0;
		if (!a_served.ps.SameBits(a_live.ps))
			stats.materialDiffMask |= 1u << 1;
		if (a_served.textures != a_live.textures)
			stats.materialDiffMask |= 1u << 2;
		if (a_served.addressModes != a_live.addressModes)
			stats.materialDiffMask |= 1u << 3;
		if (a_served.filterModes != a_live.filterModes)
			stats.materialDiffMask |= 1u << 4;
		if (a_served.textureWritten != a_live.textureWritten)
			stats.materialDiffMask |= 1u << 5;
		static std::uint32_t logged = 0;
		if (logged++ >= 16)
			return;
		std::string what;
		for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f) {
			if (!a_served.vs.SameBits(a_live.vs, f))
				what += fmt::format(" vs[{}] {}->{}", f, a_served.vs.floats[f], a_live.vs.floats[f]);
			if (!a_served.ps.SameBits(a_live.ps, f))
				what += fmt::format(" ps[{}] {}->{}", f, a_served.ps.floats[f], a_live.ps.floats[f]);
		}
		for (std::size_t t = 0; t < a_served.textures.size(); ++t)
			if (a_served.textures[t] != a_live.textures[t] || a_served.addressModes[t] != a_live.addressModes[t] || a_served.filterModes[t] != a_live.filterModes[t])
				what += fmt::format(" texture[{}] {}/{}/{} -> {}/{}/{}", t, static_cast<const void*>(a_served.textures[t]), a_served.addressModes[t],
					a_served.filterModes[t], static_cast<const void*>(a_live.textures[t]), a_live.addressModes[t], a_live.filterModes[t]);
		if (a_served.textureWritten != a_live.textureWritten)
			what += fmt::format(" written {:X}->{:X}", a_served.textureWritten, a_live.textureWritten);
		logger::warn("[DCLF] STALE material record{} (material {}, pass {:X}): a writer the material events do not cover:{}",
			a_slot == ~0u ? std::string() : fmt::format(" in slot {}", a_slot), fmt::ptr(a_key.first), a_key.second, what.substr(0, 600));
	}

	std::uint32_t SceneStore::TechniqueRowFor(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t key = TechniqueKey(a_passDescriptor);
		const auto [it, fresh] = tables.techniqueRow.try_emplace(key, static_cast<std::uint32_t>(tables.techniqueKeys.size()));
		if (fresh) {
			tables.techniqueKeys.push_back(key);
			tables.techniqueConstants.emplace_back().passDescriptor = a_passDescriptor;
			// Evaluated now, from the frame's sample (the witness RefreshTechniqueRows took this pass): drawable with this publication.
			EvaluateTechniqueRow(it->second);
			tables.NoteConstantsWrite();
		}
		return it->second;
	}

	void SceneStore::RefreshTechniqueRows()
	{
		// T6b2c: the coordinator's technique rows follow its frame's sample (FrameGlobals, bound to the scene work). The witness is what
		// the rows read of it but the fog (SameTechniqueWitness) and SetupTechniqueDescriptor's bytes; while it holds, every row is
		// already this sample's. It moves with the loaded grid (terrain LOD's HighDetailRange), the shadow mask target, the INI clamps.
		const auto& g = FrameGlobals::Current();
		auto& w = techniqueWitness;
		if (w.valid && w.byte12 == g.techniqueByte12 && w.byte7 == g.techniqueByte7 && SameTechniqueWitness(w.inputs, g.technique))
			return;
		w.inputs = g.technique;
		w.byte12 = g.techniqueByte12;
		w.byte7 = g.techniqueByte7;
		w.valid = true;
		++constantsPostStats.techniqueInputsMoved;
		for (std::uint32_t r = 0; r < tables.techniqueConstants.size(); ++r)
			EvaluateTechniqueRow(r);
	}

	void SceneStore::EvaluateTechniqueRow(std::uint32_t a_row)
	{
		if (a_row >= tables.techniqueConstants.size())
			return;
		auto& row = tables.techniqueConstants[a_row];
		const auto& inputs = FrameGlobals::Current().technique;
		TechniqueConstants now;
		EvaluateTechnique(row.passDescriptor, inputs, now);
		++constantsPostStats.techniquesEvaluated;
		// The row keeps the fog it was made with (the draws read the frame's, FrameCapture::fog): the time of day versions no row.
		if (row.valid)
			KeepTechniqueFog(row.value, now);
		const bool floats = !row.valid || !SameTechniqueFloats(now, row.value);
		const bool binding = !row.valid || !SameTechniqueBindings(now, row.value);
		if (!floats && !binding)
			return;
		row.value = now;
		row.inputs = inputs;
		if (floats)
			row.constantsVersion = tables.NextVersion();
		if (binding)
			row.bindingVersion = tables.NextVersion();
		row.valid = true;
		tables.NoteConstantsWrite();
		++constantsPostStats.techniquesWritten;
	}

	void SceneStore::PostPipelineConstants(std::uint32_t a_slot)
	{
		// To the coordinator through a lock-free queue (T6b3a: no hand-over at the frame's start): its next scene pass applies it.
		auto post = std::make_unique<PipelineConstantsPost>();
		post->slot = a_slot;
		post->key = frameTables.pipelineKeys[a_slot];
		post->binding = frameTables.pipelineBindings[a_slot];
		post->generation = frameTables.tablesGeneration;
		post->constants = frameTables.geometryConstants[a_slot];
		pipelineConstantsPosted.Push(std::move(post));
		++constantsPostStats.pipelinesPosted;
	}

	void SceneStore::ApplyConstantsPosts()
	{
		// The coordinator's tables: a post whose slot holds another key now (a slot reused, the tables made again) is dropped; the
		// render thread evaluates the new tenant when the frame's snapshot shows it.
		bool wrote = false;
		pipelineConstantsPosted.Drain([&](std::unique_ptr<PipelineConstantsPost>&& a_post) {
			if (!a_post)
				return;
			const auto& post = *a_post;
			const std::uint32_t p = post.slot;
			if (post.generation != tablesGeneration || p >= tables.pipelines.size() || p >= tables.pipelineConstants.size() || !(tables.pipelines[p] == post.key) ||
				tables.pipelineBindingVersion[p] != post.binding) {
				++constantsPostStats.stale;
				return;
			}
			auto& row = tables.pipelineConstants[p];
			row.constants = post.constants;
			row.key = post.key;
			row.binding = post.binding;
			row.version = tables.NextVersion();
			row.valid = true;
			wrote = true;
			++constantsPostStats.pipelinesApplied;
		});
		if (wrote)
			tables.NoteConstantsWrite();
	}

	bool SceneStore::MaterialPartDiffers(MaterialPart a_part, const MaterialRecord& a_record, const MaterialRecord& a_live, std::uint32_t a_pass)
	{
		switch (a_part) {
		case MaterialPart::Own:
			{
				// Outside the frame parts (the frame components, TexcoordOffset, t11 whole) and IBLParams (kept from the first evaluation).
				MaterialRecord probe = a_record;
				MaterialSources::CopyFrameComponents(a_live, probe, a_pass);
				MaterialSources::KeepUnreadFloats(a_live, probe);
				return !(probe == a_live);
			}
		case MaterialPart::Frame:
			{
				// The frame components and t11's modes (its view is the frame's: no record holds one).
				MaterialRecord probe = a_record;
				MaterialSources::StripFrameViews(probe, a_pass);
				bool floatsChanged = false;
				return MaterialSources::ApplyFrameComponents(a_live, probe, a_pass, &floatsChanged) || floatsChanged;
			}
		case MaterialPart::Transform:
			for (const auto position : MaterialSources::FrameVSFloats())
				if (a_record.vs.Written(position) && a_live.vs.Written(position) && !a_record.vs.SameBits(a_live.vs, position))
					return true;
			return false;
		}
		return false;
	}

	std::string SceneStore::DescribePartDifference(MaterialPart a_part, const MaterialRecord& a_record, const MaterialRecord& a_live, std::uint32_t a_pass)
	{
		// The record as the part compares it, then its first float (by variable) or texture that differs.
		MaterialRecord probe = a_record;
		if (a_part == MaterialPart::Own) {
			MaterialSources::CopyFrameComponents(a_live, probe, a_pass);
			MaterialSources::KeepUnreadFloats(a_live, probe);
		} else {
			MaterialSources::StripFrameViews(probe, a_pass);
		}
		auto variableOf = [](const StageLayout& a_layout, std::uint32_t a_float) {
			for (std::uint32_t v = 0; v < a_layout.count; ++v)
				if (a_float >= a_layout.offset[v] && a_float < std::uint32_t(a_layout.offset[v]) + a_layout.size[v])
					return fmt::format("{}.{}", v, "xyzw"[(a_float - a_layout.offset[v]) & 3]);
			return fmt::format("float {}", a_float);
		};
		const auto& vsFrame = MaterialSources::FrameVSFloats();
		const auto& psFrame = MaterialSources::FramePSFloats();
		auto inPart = [&](bool a_ps, std::uint32_t a_float) {
			const auto& frameFloats = a_ps ? psFrame : vsFrame;
			const bool frameFloat = std::find(frameFloats.begin(), frameFloats.end(), a_float) != frameFloats.end();
			if (a_part == MaterialPart::Own)
				return !frameFloat;
			return frameFloat && (a_part == MaterialPart::Transform) == !a_ps;
		};
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& a = stage ? probe.ps : probe.vs;
			const auto& b = stage ? a_live.ps : a_live.vs;
			for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f)
				if (inPart(stage != 0, f) && a.Written(f) && b.Written(f) && !a.SameBits(b, f))
					return fmt::format("{} {} {} -> {}", stage ? "PS" : "VS", variableOf(stage ? LightingPSLayout() : LightingVSLayout(), f), a.floats[f], b.floats[f]);
		}
		if (a_part != MaterialPart::Transform)
			for (std::size_t t = 0; t < probe.textures.size(); ++t)
				if (probe.textures[t] != a_live.textures[t] || probe.addressModes[t] != a_live.addressModes[t] || probe.filterModes[t] != a_live.filterModes[t])
					return fmt::format("t{}", t);
		if (probe.textureWritten != a_live.textureWritten)
			return fmt::format("written {:X} -> {:X}", probe.textureWritten, a_live.textureWritten);
		return "?";
	}

	void SceneStore::JudgeMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key, const MaterialRecord& a_record,
		const MaterialRecord& a_live)
	{
		auto& m = materialFrameStats;
		++stats.materialsValidated;
		++m.slotsChecked;
		if (materialSuspects.contains(a_slot))
			return;  // already in its window (SampleMaterialSuspects judges it)
		std::uint8_t parts = 0;
		for (std::uint32_t p = 0; p < kMaterialParts; ++p)
			if (MaterialPartDiffers(static_cast<MaterialPart>(p), a_record, a_live, a_key.second)) {
				parts |= static_cast<std::uint8_t>(1u << p);
				++m.suspected[p];
			}
		if (!parts)
			return;
		// First seen differing: the publication may not carry the last frames' writes yet. Its window starts with this evaluation.
		auto& suspect = materialSuspects[a_slot];
		suspect.key = a_key;
		suspect.frame = frame;
		suspect.parts = parts;
		const Tables& view = FrameView();
		suspect.version = a_slot < view.materialVersion.size() ? view.materialVersion[a_slot] : 0u;
		suspect.history.assign(1, a_live);
	}

	void SceneStore::SampleMaterialSuspects()
	{
		auto& m = materialFrameStats;
		auto& evaluator = ConstantEvaluator::Get();
		const Tables& view = FrameView();
		for (auto it = materialSuspects.begin(); it != materialSuspects.end();) {
			const std::uint32_t slot = it->first;
			auto& suspect = it->second;
			// The slot still the same key's in the installed tables (its material held by them); else let go.
			if (slot >= view.materials.size() || !view.materialSlots.Alive(slot) || view.materialSlotKey[slot] != suspect.key) {
				it = materialSuspects.erase(it);
				continue;
			}
			if (suspect.history.empty() || suspect.frame + suspect.history.size() <= frame) {
				MaterialRecord live;
				if (evaluator.EvaluateMaterial(suspect.key.first, suspect.key.second, live))
					suspect.history.push_back(live);
			}
			if (frame - suspect.frame < kMaterialLateFrames || suspect.history.empty()) {
				++it;
				continue;
			}
			// Due: each part the installed record differed in, against the window's evaluations.
			const auto& record = view.materials[slot];
			const auto pass = suspect.key.second;
			for (std::uint32_t p = 0; p < kMaterialParts; ++p) {
				if (!((suspect.parts >> p) & 1))
					continue;
				const auto part = static_cast<MaterialPart>(p);
				const bool seen = std::any_of(suspect.history.begin(), suspect.history.end(),
					[&](const MaterialRecord& a_live) { return !MaterialPartDiffers(part, record, a_live, pass); });
				if (seen) {
					// Late: a value the material had within the window. The first of each part named against the window's first
					// evaluation, the frame the record had not reached yet (what moved).
					++m.late[p];
					if (m.lateFirst[p].empty() && MaterialPartDiffers(part, record, suspect.history.front(), pass))
						m.lateFirst[p] = fmt::format("slot {} (pass {:X}): {}", slot, pass, DescribePartDifference(part, record, suspect.history.front(), pass));
					continue;
				}
				// None of the window's values: a write or a frame source the producers missed.
				const auto& live = suspect.history.back();
				if (part == MaterialPart::Own && slot < view.materialVersion.size() && view.materialVersion[slot] != suspect.version) {
					// The producers rewrote the record within the window (a covered writer's capture made it again), yet it holds none of
					// the window's values: the material moves faster than the publication follows it, not a write the events missed.
					if (m.lagging++ == 0)
						m.laggingFirst = fmt::format("slot {} (pass {:X}): {}", slot, pass, DescribePartDifference(part, record, live, pass));
				} else if (part == MaterialPart::Own) {
					MaterialRecord served = record;
					MaterialSources::CopyFrameComponents(live, served, pass);
					MaterialSources::KeepUnreadFloats(live, served);
					NoteStaleMaterial(slot, suspect.key, served, live);
				} else if (part == MaterialPart::Frame) {
					if (m.componentsDiffer++ == 0 && m.first.empty())
						m.first = fmt::format("slot {} (pass {:X}): frame components: {}", slot, pass, DescribePartDifference(part, record, live, pass));
				} else if (m.transformsDiffer++ == 0 && m.first.empty()) {
					m.first = fmt::format("slot {} (pass {:X}): texture transform: {}", slot, pass, DescribePartDifference(part, record, live, pass));
				}
			}
			it = materialSuspects.erase(it);
		}
	}

	void SceneStore::CheckSlotPort(const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key, const MaterialRecord& a_engine)
	{
		// T6b2a's gate: the port of a capture taken now, on the render thread, with the frame's sources (FrameGlobals), against the engine's
		// evaluation at the same point (CheckMaterialPort). An Advanced Skin key not set up yet is the join's wait, not a miss.
		const auto& sources = FrameGlobals::Current().material;
		MaterialPort::MaterialSnapshot snapshot;
		MaterialRecord port;
		if (!MaterialPort::Capture(*a_key.first, snapshot) ||
			(!MaterialPort::Evaluate(snapshot, a_key.second, sources, port) && MaterialPort::FeatureHooksCovered(snapshot, sources.feature))) {
			auto& p = materialPortParity;
			if (p.uncovered++ == 0 && p.first.empty())
				p.first = fmt::format("uncovered: material {} (feature {}, {}), pass {:08X}", fmt::ptr(a_key.first), snapshot.feature, snapshot.pbr ? "PBR" : "vanilla",
					a_key.second);
			return;
		}
		if (!MaterialPort::FeatureHooksCovered(snapshot, sources.feature))
			return;
		CheckMaterialPort(a_key.first, a_key.second, port, snapshot, &a_engine);
	}

	void SceneStore::ValidateMaterialSlice()
	{
		// CS_DCLF_CAPTURE_PARITY's diagnostics: the materials written since the last frame (queued only under that switch).
		if (SwitchEnabled(Switch::CaptureParity)) {
			writtenMaterials.clear();
			MaterialSources::Drain(writtenMaterials);
		}
		// CS_DCLF_PERSISTENT_PARITY, the observers (reported, never repaired). The installed publication's records (the frame's tables,
		// whose slots hold their materials until no publication names them) against the engine's evaluation now: the slots its material
		// log names since the last frame (the records the scene work wrote), a few more a frame by a rotating cursor, every used one on a
		// parity frame; each in every part (JudgeMaterial, the suspects' windows), and the port of a live capture against the same
		// evaluation (CheckSlotPort, T6b2a's material port parity).
		const Tables& view = FrameView();
		if (!SwitchEnabled(Switch::PersistentParity) || view.materials.empty()) {
			materialSuspects.clear();
			return;
		}
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		SampleMaterialSuspects();
		ankerl::unordered_dense::set<std::uint32_t> validated;
		auto validate = [&](std::uint32_t a_slot) {
			if (a_slot >= view.materials.size() || !view.materialSlots.Alive(a_slot) || !validated.insert(a_slot).second)
				return;
			const auto key = view.materialSlotKey[a_slot];
			MaterialRecord live;
			if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
				return;
			JudgeMaterial(a_slot, key, view.materials[a_slot], live);
			CheckSlotPort(key, live);
		};
		// The records written since the last frame's tables (a new tables generation or a trimmed log: from the end on).
		auto& cursor = materialParityCursor;
		if (cursor.Continues(view.materialLog, 0)) {
			for (const std::uint32_t slot : cursor.Unread(view.materialLog))
				if (view.MaterialUsed(slot)) {
					++materialFrameStats.written;
					validate(slot);
				}
		} else {
			cursor.Restart(0);
		}
		cursor.Advance(view.materialLog);
		if (ParityDue(frame, 23)) {
			++materialFrameStats.checks;
			Tables::ForEachBit(view.usedMaterialBits, [&](const std::uint32_t a_slot) { validate(a_slot); });
			return;
		}
		std::uint32_t looked = 0;
		for (std::uint32_t n = 0; n < kMaterialValidationsPerFrame * kMaterialValidationStride && looked < kMaterialValidationsPerFrame; ++n) {
			const std::uint32_t slot = materialValidationCursor++ % static_cast<std::uint32_t>(view.materials.size());
			if (!view.MaterialUsed(slot))
				continue;
			++looked;
			validate(slot);
		}
	}
}
