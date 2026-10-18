// The coordinator's frame-dependent constants (scene lane, its frame's FrameGlobals bound): material records refreshed from the
// captures, frame-sourced components and texture transforms, object extras, technique rows, and the pipeline constants the render
// thread posts back.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"

#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include <cmath>
#include <optional>

namespace DCLF::Scene
{
	namespace
	{
		/**
		 * @brief RefreshMaterialSignatures' witness (scene lane only; the coordinator is one, Coordinator::Get): the frame's sources every
		 * applied signature was last evaluated against, and what it saved. The parity (CS_DCLF_PERSISTENT_PARITY, every 60 passes while it
		 * holds) evaluates every held signature as every pass did, and counts a component the witness would have missed.
		 */
		struct SignatureWitness
		{
			std::optional<MaterialPort::FrameComponentsWitness> applied;
			std::uint64_t passes = 0, moved = 0, held = 0;
			Diagnostics::ParityCounter parity;
		} signatureWitness;
		constexpr std::uint32_t kSignatureParityOffset = 31;
		constexpr std::uint64_t kSignatureReportPasses = 600;

		/**
		 * @brief DrainMaterialCaptures' deduplication's parity (CS_DCLF_PERSISTENT_PARITY, on its passes, kMaterialDedupParityOffset): the
		 * slots of every material whose capture was deduplicated, evaluated as a rewrite would; a record that rewrite would change is a
		 * capture the deduplication lost. Diagnostics only (scene lane).
		 */
		Diagnostics::ParityCounter materialDedupParity;
	}

	bool Coordinator::PrepareMaterials(std::string_view a_zone, std::size_t a_count, const std::function<void(std::size_t, std::size_t)>& a_body)
	{
		// Pure chunks over frozen inputs (the held captures, the pass's sources passed in, the records read-only or each chunk's own
		// slots); the lane applies their results after, in order.
		const bool pool = FanoutMode(a_count) == 2;
		auto chunk = [&](std::size_t a_begin, std::size_t a_end, bool a_worker) {
			ShardScope scope(*this, a_worker);
			a_body(a_begin, a_end);
		};
		if (pool) {
			SceneScheduler::Executor().ParallelFor(a_zone, a_count, kFanoutGrain, [&](std::size_t a_begin, std::size_t a_end) { chunk(a_begin, a_end, true); });
			return true;
		}
		for (std::size_t begin = 0; begin < a_count; begin += kFanoutGrain)
			chunk(begin, std::min(a_count, begin + kFanoutGrain), false);
		return false;
	}

	void Coordinator::EvaluateMaterial(const MaterialPort::MaterialSnapshot& a_snapshot, std::uint32_t a_pass, const MaterialPort::MaterialFrame& a_sources,
		std::uint32_t a_slot, MaterialEvaluation& a_out) const
	{
		using Outcome = MaterialEvaluation::Outcome;
		a_out.record = MaterialRecord{};
		a_out.same = false;
		if (!MaterialPort::Evaluate(a_snapshot, a_pass, a_sources, a_out.record)) {
			// An input the frame's sources do not have yet (Advanced Skin's textures for a key it has not set up): again with a later
			// pass's (an After contributor's covers false: Extensions::MaterialRetryable). Else a class the port does not cover.
			a_out.outcome = Extensions::MaterialRetryable({ a_snapshot, a_snapshot.materialClass, a_pass, a_sources.hook, a_sources.contributors }) ?
			                    Outcome::Retry :
			                    Outcome::Uncovered;
			return;
		}
		a_out.outcome = Outcome::Made;
		// The character light's t11 view is the frame's, never the record's.
		MaterialSources::StripFrameViews(a_out.record, a_pass);
		if (a_slot == kNoMaterialSlot)
			return;
		// A rewrite: its frame parts are kept (the frame components are the signature's writes; TexcoordOffset stays the slot's: the frame
		// values patch the rows with the frame's), so only the material's own values are compared and rewritten.
		const auto& held = tables.materials[a_slot];
		MaterialSources::CopyFrameComponents(held, a_out.record, a_pass);
		MaterialSources::KeepUnreadFloats(held, a_out.record);
		a_out.same = a_out.record == held;
	}

	void Coordinator::RefreshMaterialRecords()
	{
		// The scene work, its frame's FrameGlobals bound, the captures drained (ApplyEvents): the written materials' records, then the
		// frame components (a rewrite keeps the slot's frame parts, which the signatures bring to this frame's sources). The texture
		// transforms are no record's (TextureTransformList). No engine read: the captures and the frame's sample are the inputs.
		ZoneScopedN("CS.DCLF.Scene.MaterialRecords");
		++materialRecordStats.passes;
		stats.materialWrites = stats.materialsRewritten = stats.materialsHeld = stats.frameMaterialSamples = 0;
		RewriteCapturedMaterials();
		RefreshMaterialSignatures();
		// The captures' deduplication and the pool's share (CS_DCLF_STATS), with the parities where they ran: once per report window (the
		// pass's frame in a later one; the coordinator's clock may skip frame numbers), whatever the passes a frame.
		auto& r = materialRecordStats;
		auto& s = materialSnapshotStats;
		const std::uint32_t window = sceneFrame / Reports::kInterval;
		const bool parityRan = materialDedupParity.checks || sharedBindings.work.parity.checks || lookupsResolve.pipelineParity.checks;
		if (window != materialReportWindow && (Diagnostics::Observe().stats || parityRan)) {
			materialReportWindow = window;
			logger::info("[DCLF] material captures: {} deduplicated ({} transforms only), {} relisted; evaluations pooled {} in {} rounds, joins' {} prepared "
						 "{} inline; transform captures left to the frame values {}; applications pooled {}; dedup parity {} checks, {} differ{}",
				s.deduped, s.transformsOnly, s.relisted, r.evaluationsPooled, r.poolRounds, r.joinPrepared, r.joinInline, r.transformsWatched, r.appliedPooled,
				materialDedupParity.checks, materialDedupParity.mismatches, materialDedupParity.Verdict(true));
			// The bindings' and lookups' change sets: owner blocks sealed or shared, waiters deduplicated, masks looked up of those checked
			// (and full sweeps), Lighting pipeline entries resolved of those witnessed, shadow resolutions skipped; their parities.
			auto& mw = materialBindings.work;
			auto& sw = sharedBindings.work;
			auto& lr = lookupsResolve;
			logger::info("[DCLF] material bindings: owner blocks sealed {} shared {}, waiters deduplicated {}; masks resolved {} of {} checked, {} sweeps, parity {} "
						 "checks, {} differ{}; pipeline lookups resolved {} of {} witnessed, {} sweeps, parity {} checks, {} differ{}; shadow lookups resolved {} skipped {}",
				mw.sealed, mw.sealShared, mw.waitersDeduped, sw.masksResolved, sw.masksChecked, sw.maskSweeps, sw.parity.checks, sw.parity.mismatches,
				sw.parity.Verdict(true), lr.pipelinesResolved, lr.pipelinesChecked, lr.pipelineSweeps, lr.pipelineParity.checks, lr.pipelineParity.mismatches,
				lr.pipelineParity.Verdict(true), lr.shadowResolved, lr.shadowSkipped);
			s.deduped = s.transformsOnly = s.relisted = 0;
			r.evaluationsPooled = r.poolRounds = r.joinPrepared = r.joinInline = r.watchesSkipped = r.watchesDropped = r.appliedPooled = 0;
			materialDedupParity.Reset();
			mw = {};
			sw.masksChecked = sw.masksResolved = sw.maskSweeps = 0;
			sw.parity.Reset();
			lr.pipelinesChecked = lr.pipelinesResolved = lr.pipelineSweeps = lr.shadowResolved = lr.shadowSkipped = 0;
			lr.pipelineParity.Reset();
		}
	}

	void Coordinator::RewriteCapturedMaterials()
	{
		// The lane gathers (slot, held capture) for the materials captured since the last pass; the pool evaluates each slot's record and
		// compares it with the slot's (read-only); the lane stores the changed ones in slot order.
		auto& r = materialRecordStats;
		std::vector<const RE::BSShaderMaterial*> materials = std::exchange(materialsCaptured, {});
		const std::size_t captured = materials.size();
		materials.insert(materials.end(), materialRewritesPending.begin(), materialRewritesPending.end());
		materialRewritesPending.clear();
		std::vector<const RE::BSShaderMaterial*> deduped = std::exchange(materialsDeduped, {});
		if (materials.empty() && deduped.empty())
			return;
		std::sort(materials.begin(), materials.end());
		materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
		r.captured += captured;
		stats.materialWrites = static_cast<std::uint32_t>(captured);
		const auto& sources = FrameGlobals::Current().material;
		auto& items = materialRewriteItems;
		items.clear();
		auto gather = [&](const RE::BSShaderMaterial* a_material, bool a_parity) {
			const auto held = materialSnapshots.find(a_material);
			if (held != materialSnapshots.end())
				held->second.listed = false;
			const auto dependents = materialDependents.find(a_material);
			if (dependents == materialDependents.end())
				return;  // no slot holds it: a join makes its record from the capture
			if (held == materialSnapshots.end() || !held->second.held)
				return;  // let go: no slot has used it for kMaterialSnapshotFrames, and its next capture comes with its next write
			const auto& snapshot = held->second.held->snapshot;
			if (!a_parity)
				held->second.frame = sceneFrame;
			for (const std::uint32_t slot : dependents->second)
				if (slot < tables.materials.size() && tables.materialSlots.Alive(slot) && tables.materialSlotKey[slot].first == a_material)
					items.push_back({ a_material, &snapshot, slot, a_parity });
		};
		for (const auto* material : materials)
			gather(material, false);
		// The parity's: the deduplicated materials no rewrite covers, evaluated as one would (nothing stored).
		std::sort(deduped.begin(), deduped.end());
		deduped.erase(std::unique(deduped.begin(), deduped.end()), deduped.end());
		for (const auto* material : deduped)
			if (!std::binary_search(materials.begin(), materials.end(), material))
				gather(material, true);
		if (items.empty())
			return;
		auto& results = materialRewriteResults;
		results.resize(items.size());
		// Each slot once (a slot is one material's), so every result reads a record no other chunk writes, and none is written before the apply.
		if (PrepareMaterials("CS.DCLF.Scene.MaterialRewritesShards", items.size(), [&](std::size_t a_begin, std::size_t a_end) {
				for (std::size_t i = a_begin; i < a_end; ++i)
					EvaluateMaterial(*items[i].snapshot, tables.materialSlotKey[items[i].slot].second, sources, items[i].slot, results[i]);
			})) {
			r.evaluationsPooled += items.size();
			++r.poolRounds;
		}
		using Outcome = MaterialEvaluation::Outcome;
		const RE::BSShaderMaterial* retried = nullptr;  // a material's items are contiguous: retried once
		for (std::size_t i = 0; i < items.size(); ++i) {
			const auto& item = items[i];
			auto& result = results[i];
			if (item.parity) {
				if (result.outcome == Outcome::Made)
					materialDedupParity.Check(result.same, [&] {
						return fmt::format("material {} slot {} (pass {:#x}): a deduplicated capture's rewrite would change the record", static_cast<const void*>(item.material),
							item.slot, tables.materialSlotKey[item.slot].second);
					});
				continue;
			}
			if (result.outcome == Outcome::Retry) {
				// The slot keeps its record meanwhile; the material is evaluated again with a later pass's sources.
				if (retried != item.material) {
					retried = item.material;
					materialRewritesPending.push_back(item.material);
					++r.retried;
					++stats.materialsHeld;
				}
				continue;
			}
			if (result.outcome == Outcome::Uncovered) {
				++r.uncovered;
				continue;
			}
			NoteMaterialRecord(item.slot, *item.snapshot);
			if (result.same)
				continue;
			tables.materials[item.slot] = std::move(result.record);
			tables.materialVersion[item.slot] = ++materialVersions;
			tables.NoteMaterial(item.slot);
			++r.rewritten;
			++stats.materialsRewritten;
		}
	}

	void Coordinator::RefreshMaterialSignatures()
	{
		// The frame-sourced components (MaterialSources): the same for every record of a signature, so one port evaluation of the
		// signature's probe against the frame's sources gives them; written into every slot of the signature only when they moved
		// since the last application. A slot keyed since was made from the same sources (the joins evaluate with them).
		// The frame's witness (MaterialPort::FrameComponentsWitness, what of the sources the components read): while it is the one the
		// signatures were last evaluated against, every applied signature already holds this frame's components, and only a signature
		// not applied yet or whose probe failed (a new probe, or the same one again) is evaluated.
		ZoneScopedN("CS.DCLF.Scene.MaterialSignatures");
		auto& r = materialRecordStats;
		const auto& sources = FrameGlobals::Current().material;
		auto& w = signatureWitness;
		const bool moved = !w.applied || !(*w.applied == sources.components);
		if (moved)
			w.applied = sources.components;
		++w.passes;
		w.moved += moved ? 1 : 0;
		const bool parity = !moved && Diagnostics::Observe().scene && Diagnostics::ParityDue(passSerial, kSignatureParityOffset);
		auto keyed = [&](std::uint32_t a_slot, std::uint32_t a_signature) {
			return a_slot < tables.materials.size() && tables.materialSlots.Alive(a_slot) && tables.materialSlotKey[a_slot].first &&
			       MaterialSources::Signature(tables.materialSlotKey[a_slot].second) == a_signature;
		};
		for (auto& [signature, entry] : materialSignatures) {
			if (!entry.probeValid)
				continue;
			const bool held = !moved && entry.appliedValid && !entry.probeFailed;
			if (held && !parity) {
				++w.held;
				continue;
			}
			MaterialRecord live;
			if (!MaterialPort::Evaluate(entry.probe, entry.probePass, sources, live)) {
				if (held) {
					// The parity's: a held probe these sources no longer evaluate (a gate the witness leaves out).
					w.parity.Check(false, [&] { return fmt::format("signature {:#x}: its probe no longer evaluates", signature); });
					continue;
				}
				// Not evaluable with these sources (an Advanced Skin key not set up yet): tried again next pass, unless a slot noted
				// meanwhile gives another probe. Its slots keep components of an older witness: not applied at this one, so the probe
				// that replaces it is evaluated under any witness (NoteMaterialRecord clears probeFailed).
				entry.probeFailed = true;
				entry.appliedValid = false;
				continue;
			}
			if (held) {
				// The parity's: the components every pass's evaluation would apply against those the witness kept. Changes nothing.
				MaterialRecord probe = entry.applied;
				bool floatsChanged = false;
				const bool texturesChanged = MaterialSources::ApplyFrameComponents(live, probe, entry.probePass, &floatsChanged);
				w.parity.Check(!texturesChanged && !floatsChanged, [&] {
					return fmt::format("signature {:#x} (pass {:#x}): {} moved under an unchanged witness", signature, entry.probePass,
						texturesChanged ? "t11" : "a frame float");
				});
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
			// Kept as records hold it (no t11 view: the frame's), so the next comparison sees only what a record would take.
			entry.applied = live;
			MaterialSources::StripFrameViews(entry.applied, entry.probePass);
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
				++i;
			}
			// Written in place by chunks (a slot is on one signature's list once), versioned after in list order.
			auto& items = materialApplyItems;
			items.resize(slots.size());
			if (PrepareMaterials("CS.DCLF.Scene.MaterialSignaturesShards", slots.size(), [&](std::size_t a_begin, std::size_t a_end) {
					for (std::size_t i = a_begin; i < a_end; ++i) {
						const std::uint32_t slot = slots[i];
						bool floatsChanged = false;
						const bool recordChanged = MaterialSources::ApplyFrameComponents(live, tables.materials[slot], tables.materialSlotKey[slot].second, &floatsChanged);
						items[i] = { slot, 0, static_cast<std::uint8_t>((recordChanged ? 1u : 0u) | (floatsChanged ? 2u : 0u)) };
					}
				}))
				r.appliedPooled += slots.size();
			for (const auto& item : items) {
				if (item.changed & 1)
					tables.materialVersion[item.slot] = ++materialVersions;
				if (item.changed & 2)
					tables.materialFrameVersion[item.slot] = ++materialVersions;
				if (item.changed)
					tables.NoteMaterial(item.slot);
			}
			r.slotsApplied += slots.size();
		}
		// The witness's line (CS_DCLF_STATS), with its parity's verdict where it ran.
		if (w.passes >= kSignatureReportPasses && Diagnostics::Observe().stats) {
			logger::info("[DCLF] material signature witness: moved on {} of {} passes, {} signature evaluations held; parity {} checks, {} differ{}", w.moved,
				w.passes, w.held, w.parity.checks, w.parity.mismatches, w.parity.Verdict(true));
			w.passes = w.moved = w.held = 0;
			w.parity.Reset();
		}
	}

	void Coordinator::NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot)
	{
		// The material's texture transforms are no record's (the frame values patch the rows: TextureTransformList).
		if (a_slot >= tables.materialSlotKey.size())
			return;
		const auto [material, pass] = tables.materialSlotKey[a_slot];
		if (!material)
			return;
		// A port's record without signature components (the Effect port's: its pass descriptor's bits are not Lighting's); its texture
		// transforms are the frame values'.
		if (a_slot < tables.materials.size() && !Extensions::PortRules(tables.materials[a_slot].kind).signatureComponents)
			return;
		const std::uint32_t signature = MaterialSources::Signature(pass);
		auto& entry = materialSignatures[signature];
		if (materialSignatureListed.size() <= a_slot)
			materialSignatureListed.resize(std::size_t(a_slot) + 1, 0);
		if (materialSignatureListed[a_slot] != signature + 1) {
			// Listed under another signature (the slot keyed again): out of that list first, as a list is walked only when its signature
			// applies (the witness moved), so a slot cycling between two would otherwise gather duplicates.
			if (const std::uint32_t was = materialSignatureListed[a_slot]; was != 0)
				if (const auto old = materialSignatures.find(was - 1); old != materialSignatures.end()) {
					auto& slots = old->second.slots;
					if (const auto at = std::find(slots.begin(), slots.end(), a_slot); at != slots.end()) {
						*at = slots.back();
						slots.pop_back();
					}
				}
			materialSignatureListed[a_slot] = signature + 1;
			entry.slots.push_back(a_slot);
		}
		if (!entry.probeValid || entry.probeFailed) {
			entry.probe = a_snapshot;
			entry.probePass = pass;
			entry.probeValid = true;
			entry.probeFailed = false;
		}
	}

	void Coordinator::ResetMaterialRecords()
	{
		for (auto& item : materialSignatures) {
			item.second.slots.clear();
			item.second.appliedValid = false;
		}
		materialSignatureListed.clear();
		materialRewritesPending.clear();
	}

	void Coordinator::ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (a_material)
			materialDependents[a_material].push_back(a_slot);
	}

	void Coordinator::UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (auto it = materialDependents.find(a_material); it != materialDependents.end()) {
			auto& slots = it->second;
			std::erase(slots, a_slot);
			if (slots.empty())
				materialDependents.erase(it);
		}
	}

	void Coordinator::NameShading(std::uint32_t a_slot, bool a_member)
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
		item.fieldsOwned = tables.ShadingFieldsOwned(a_slot);
	}

	void Coordinator::NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots)
	{
		// Prepared by chunks of keys (across the pool when they are many: FanoutMode), pure: each key's dependents, their slots and
		// what NameShading would name, read from the frozen index and tables. Applied on the pass's thread in key order: the items
		// with their references, the extras' rows, the counters (as the serial loop did, slot by slot).
		struct Named
		{
			std::uint32_t slot = kNoObjectSlot;
			RE::BSShaderProperty* property = nullptr;  // none: NameShading names nothing (free, unbound or propertyless)
			std::uint32_t pass = 0;
			bool member = false;
			std::uint8_t fieldsOwned = 0;
		};
		constexpr std::size_t kGrain = 64;
		std::vector<std::vector<Named>> prepared((a_keys.size() + kGrain - 1) / kGrain);
		auto prepare = [&](std::size_t a_begin, std::size_t a_end) {
			auto& out = prepared[a_begin / kGrain];
			for (std::size_t k = a_begin; k < a_end; ++k) {
				const auto dependents = propertyDependents.find(a_keys[k]);
				if (dependents == propertyDependents.end())
					continue;
				for (auto* geometry : dependents->second) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.objectStamp != objectStamp)
						continue;
					for (const std::uint32_t slot : { it->second.slot, it->second.layerSlot }) {
						if (slot == kNoObjectSlot || slot >= tables.objects.size())
							continue;
						auto& named = out.emplace_back();
						named.slot = slot;
						if (slot >= tables.objectGeometry.size() || (tables.objects[slot].flags & (kObjectNoBindings | kObjectFree)))
							continue;
						if ((named.property = SlotProperty(slot))) {
							named.pass = tables.pipelines[tables.objects[slot].pipelineIndex].passDescriptor;
							named.member = IsResidentSlot(slot);
							named.fieldsOwned = tables.ShadingFieldsOwned(slot);
						}
					}
				}
			}
		};
		if (FanoutMode(a_keys.size()) == 2) {
			SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.ShadingShards", a_keys.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.Scene.ShadingShards");
				const auto scope = ShardScope(*this, true);
				prepare(a_begin, a_end);
			});
		} else {
			for (std::size_t k = 0; k < a_keys.size(); k += kGrain)
				prepare(k, std::min(k + kGrain, a_keys.size()));
		}
		for (const auto& chunk : prepared)
			for (const auto& named : chunk) {
				const std::uint32_t slot = named.slot;
				if (named.property) {
					auto& item = shadingNamed.emplace_back();
					item.property.reset(named.property);
					item.slot = slot;
					item.pass = named.pass;
					item.member = named.member;
					item.fieldsOwned = named.fieldsOwned;
				}
				// The extras' static rows read the same property fields (ProjectedUV's parameters and colour).
				if ((tables.objects[slot].flags & (kObjectProjectedUV | kObjectLandBlend)) && WriteObjectExtras(slot))
					tables.NoteChange(slot, kChangeExtras);
				++shadingStats.resampled;
				if (a_slots)
					a_slots->push_back(slot);
			}
	}

	void Coordinator::NameShadingEvents(std::vector<std::uint32_t>* a_slots)
	{
		// The events: the controllers' writes of the emissive colour and multiplier and of material fields (MaterialSources), the
		// LOD fades and the alpha GetRenderPasses leaves on the property (an actor's fade among them; lodFadeEvents), and external
		// emittance's shared colour (emittanceEvents). Their dependents are sampled at the next frame's start: a value that changes
		// during this frame (a controller on the animation job, a cull's GetRenderPasses) is drawn from the next frame on, a frame
		// after the engine's own draw would show it (dclf-architecture.md, "Accepted differences").
		lodFadeChanged.clear();
		DrainLodFadeEvents(lodFadeChanged);
		const std::size_t lodFades = lodFadeChanged.size();
		DrainEmittanceEvents(lodFadeChanged);
		shadingStats.emittanceEvents += lodFadeChanged.size() - lodFades;
		MaterialWrites::DrainShadingChanges(lodFadeChanged);
		if (!shadingNamedAll || !lodFadeEventsInstalled) {
			// Every slot (the events are not installed: every walk).
			shadingNamedAll = true;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				NameShading(o, IsResidentSlot(o));
				if ((tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) && WriteObjectExtras(o))
					tables.NoteChange(o, kChangeExtras);
			}
			shadingStats.resampled += tables.objects.size();
			return;
		}
		std::sort(lodFadeChanged.begin(), lodFadeChanged.end());
		lodFadeChanged.erase(std::unique(lodFadeChanged.begin(), lodFadeChanged.end()), lodFadeChanged.end());
		shadingStats.lodFadeEvents += lodFadeChanged.size();
		NameShadingKeys(lodFadeChanged, a_slots);
	}

	bool Coordinator::WriteObjectExtras(std::uint32_t a_object)
	{
		if (a_object >= tables.extraOffset.size() || tables.extraOffset[a_object] == kNoExtraRows || a_object >= tables.objectGeometry.size())
			return false;
		const auto& object = tables.objects[a_object];
		const auto* geometry = tables.objectGeometry[a_object];
		// The records: the slot's property's (its layer's for a layer slot) and the geometry's.
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

	void Coordinator::MaterialReference::reset(RE::BSShaderMaterial* a_material)
	{
		// BSIntrusiveRefCounted's count (+0x8), incremented as the engine takes a reference on a property's material
		// (FUN_1414ac820); a material being read off a live property has one already, so it cannot be at zero here.
		if (a_material)
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<std::byte*>(a_material) + 0x8));
		if (material) {
			using Release = void(void*, RE::BSShaderMaterial*);
			static REL::Relocation<Release*> release{ REL::Offset(Engine::Address::kMaterialRelease) };
			static REL::Relocation<void**> manager{ REL::Offset(Engine::Address::kMaterialManager) };
			release(*manager, material);
		}
		material = a_material;
	}

	std::uint32_t Coordinator::TechniqueRowFor(std::uint32_t a_passDescriptor)
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

	void Coordinator::RefreshTechniqueRows()
	{
		// The coordinator's technique rows follow its frame's sample (FrameGlobals, bound to the scene work). The witness is what
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

	void Coordinator::EvaluateTechniqueRow(std::uint32_t a_row)
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

	void Coordinator::ApplyConstantsPosts()
	{
		// The coordinator's tables: a post whose slot holds another key now (a slot reused, the tables made again) is dropped; the
		// render thread evaluates the new tenant when the frame's snapshot shows it.
		bool wrote = false;
		channels.pipelineConstantsPosted.Drain([&](std::unique_ptr<PipelineConstantsPost>&& a_post) {
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
}
