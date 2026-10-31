// The lookups' pipeline entries: the scene lane resolves the Lighting and shadow pipelines a publication's tables use from the
// pipeline lane's catalog (no engine or render-thread reads); the render thread posts the frame inputs they are resolved for
// (PostLookupInputs).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <algorithm>
#include <iterator>

namespace DCLF::Draws
{
	namespace
	{
		// The witnessed resolution's parity offset (Diagnostics::ParityDue, by ResolveLookups calls).
		constexpr std::uint32_t kPipelineParityOffset = 43;

		bool SameUsage(const Lookups::RegisterUsageBits& a_bits, const RegisterUsage& a_usage)
		{
			return a_bits.vertexConstants == a_usage.vertexConstants && a_bits.pixelConstants == a_usage.pixelConstants && a_bits.textures == a_usage.textures &&
			       a_bits.samplers == a_usage.samplers;
		}

		/** @brief The set was recreated (a target change): every index a build may hold is stale. And the entries as many as the slots. */
		void BeginPipelineLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups)
		{
			const std::uint32_t setGeneration = a_catalog ? a_catalog->generation : 0u;
			if (a_lookups.pipelineSetGeneration != setGeneration) {
				a_lookups.pipelineSetGeneration = setGeneration;
				++a_lookups.generation;
				++a_lookups.shadowGeneration;
			}
			if (a_lookups.pipelines.size() != a_tables.pipelines.size())
				a_lookups.pipelines.resize(a_tables.pipelines.size());
		}

		/** @brief One slot's entry from a_catalog, written only where it differs. */
		void ResolvePipelineSlot(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, std::size_t p)
		{
			// Read first: a write copies the entry's chunk when a publication shares it (SharedChunks), so an unchanged entry is not written.
			const auto& current = a_lookups.pipelines[p];
			if (!a_tables.PipelineUsed(p)) {
				if (current.setIndex != Lookups::kNone) {
					auto& entry = a_lookups.pipelines.Mutable(p);
					entry.version = a_lookups.NextVersion();
					entry.setIndex = Lookups::kNone;
				}
				return;
			}
			const auto& key = a_tables.pipelines[p];
			const auto* built = a_catalog ? a_catalog->Find(key) : nullptr;
			const std::uint32_t resolved = built ? built->setIndex : Lookups::kNone;
			bool same = current.key == key && current.requested && current.setIndex == resolved;
			if (same && resolved != Lookups::kNone)
				same = current.vsTable == built->tables->vs && current.psTable == built->tables->ps && SameUsage(current.usage[0], built->usage[0]) &&
				       SameUsage(current.usage[1], built->usage[1]);
			if (same)
				return;
			auto& entry = a_lookups.pipelines.Mutable(p);
			// Written only where it differs, so an entry's version is new only when it changed. A re-keyed entry keeps its technique mask:
			// that is the shared bindings' (ResolveMaskBinding, by the slot's technique row; a retired slot's dropped: RetireMaskBinding).
			bool changed = false;
			if (!(entry.key == key)) {
				entry.key = key;
				entry.setIndex = Lookups::kNone;
				entry.requested = false;
				changed = true;
			}
			// Asked of the lane once per key: it keeps every key, and builds them again after a target change.
			if (!entry.requested) {
				DrawPipelines::Get().RequestLighting(key, static_cast<std::uint32_t>(p));
				entry.requested = true;
			}
			if (entry.setIndex != Lookups::kNone && entry.setIndex != resolved)
				++a_lookups.generation;  // a build may hold the old index
			changed |= entry.setIndex != resolved;
			entry.setIndex = resolved;
			if (resolved != Lookups::kNone) {
				// Its stages' constant tables (which cbuffer offset each Lighting variable has), the lane's with the build (reflected
				// from DCLF's own modules, fixed per built entry; ShaderCache's are the diagnostics', CheckConstantTables). Copied where they differ.
				auto assign = [&](std::vector<std::uint8_t>& a_table, const std::vector<std::uint8_t>& a_source) {
					if (a_table != a_source) {
						a_table = a_source;
						changed = true;
					}
				};
				assign(entry.vsTable, built->tables->vs);
				assign(entry.psTable, built->tables->ps);
				for (std::uint32_t variant = 0; variant < 2; ++variant) {
					const auto& usage = built->usage[variant];
					auto& bits = entry.usage[variant];
					changed |= !SameUsage(bits, usage);
					bits.vertexConstants = usage.vertexConstants;
					bits.pixelConstants = usage.pixelConstants;
					bits.textures = usage.textures;
					bits.samplers = usage.samplers;
				}
			}
			if (changed)
				entry.version = a_lookups.NextVersion();
		}

		/**
		 * @brief ResolvePipelineLookups for the slots whose witness moved (LookupsResolveState): every slot when the catalog or the lookups
		 * instance is another, else those whose key's binding version or used bit moved since they were resolved.
		 */
		void ResolveChangedPipelineLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, LookupsResolveState& a_state)
		{
			ZoneScopedN("CS.DCLF.Scene.PipelineLookups");
			BeginPipelineLookups(a_tables, a_catalog, a_lookups);
			const std::uint64_t revision = a_catalog ? a_catalog->revision : 0;
			const std::uint32_t generation = a_catalog ? a_catalog->generation : ~0u;
			if (a_state.pipelineCatalog != a_catalog || a_state.pipelineCatalogRevision != revision || a_state.pipelineCatalogGeneration != generation ||
				a_state.pipelineInstance != a_lookups.instance) {
				a_state.pipelineCatalog = a_catalog;
				a_state.pipelineCatalogRevision = revision;
				a_state.pipelineCatalogGeneration = generation;
				a_state.pipelineInstance = a_lookups.instance;
				a_state.pipelineWitness.assign(a_tables.pipelines.size(), ~0ull);
				++a_state.pipelineSweeps;
			}
			a_state.pipelineWitness.resize(a_tables.pipelines.size(), ~0ull);
			a_state.pipelinesChecked += a_tables.pipelines.size();
			for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p) {
				const std::uint32_t binding = p < a_tables.pipelineBindingVersion.size() ? a_tables.pipelineBindingVersion[p] : 0u;
				const std::uint64_t witness = (std::uint64_t(binding) << 1) | (a_tables.PipelineUsed(p) ? 1u : 0u);
				if (a_state.pipelineWitness[p] == witness)
					continue;
				ResolvePipelineSlot(a_tables, a_catalog, a_lookups, p);
				a_state.pipelineWitness[p] = witness;
				++a_state.pipelinesResolved;
			}
		}
	}

	void ResolvePipelineLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups)
	{
		// Every slot (the witnessed resolution's reference: its parity).
		ZoneScopedN("CS.DCLF.Scene.PipelineLookupsFull");
		BeginPipelineLookups(a_tables, a_catalog, a_lookups);
		for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p)
			ResolvePipelineSlot(a_tables, a_catalog, a_lookups, p);
	}

	void ResolveShadowLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, const ShadowLookupInputs& a_inputs, LookupsResolveState& a_state,
		Lookups& a_lookups)
	{
		ZoneScopedN("CS.DCLF.Scene.ShadowLookups");
		// The shadow pipelines are the pipeline lane's: asked of it here, each key once, and resolved from the catalog the
		// lookups are published with alone (its shadow entries, whose set version GetShadowIndirectState binds), so the indices and the set
		// agree. Nothing below reads the engine or a render-thread service.
		if (!a_catalog)
			return;
		// The shadow set was recreated (a shadow map format change): every index a build may hold is stale.
		if (a_lookups.shadowSetGeneration != a_catalog->shadowGeneration) {
			a_lookups.shadowSetGeneration = a_catalog->shadowGeneration;
			for (auto& found : a_lookups.shadowPipelines)
				found.second = Lookups::kNone;
			for (auto& modeRows : a_lookups.shadowMapRows)
				for (auto& row : modeRows)
					std::ranges::fill(row, Lookups::kNone);
			++a_lookups.shadowGeneration;
		}
		// Every state of each mode's views, either caster class (ModeRasterStates::All, into the kept scratch).
		static_assert(std::tuple_size_v<decltype(a_state.shadowModeStates)> == kShadowModeCount);
		auto& modeStates = a_state.shadowModeStates;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			const auto& states = a_inputs.rasterStates[m];
			modeStates[m].clear();
			std::set_union(states.casters.begin(), states.casters.end(), states.volumetric.begin(), states.volumetric.end(), std::back_inserter(modeStates[m]));
		}
		// What the pipelines are found for: the lookups instance, the shadow set and its format, each used mode's keys, states and format.
		// A resolution for the same as the last one, which found every pipeline, would find the same ones again (a found pipeline stays in
		// the set until the set is recreated, a new generation): it is skipped, and a pass pays only when a key, a state or the set changes.
		auto& resolvedFor = a_state.shadowResolvedScratch;
		resolvedFor.clear();
		resolvedFor.push_back(a_lookups.instance);
		resolvedFor.push_back((std::uint64_t(a_catalog->shadowGeneration) << 32) | a_catalog->shadowFormat);
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_inputs.modes[m])
				continue;
			const auto& keys = IsOcclusionMode(m) ? a_tables.occlusionKeysUsed[OcclusionOfMode(m)] : a_tables.shadowKeysUsed;
			resolvedFor.push_back((std::uint64_t(m) << 32) | (IsOcclusionMode(m) ? a_inputs.occlusionFormats[OcclusionOfMode(m)] : a_inputs.dsvFormat));
			resolvedFor.push_back((std::uint64_t(keys.size()) << 32) | modeStates[m].size());
			for (const auto& key : keys) {
				resolvedFor.push_back((std::uint64_t(key.technique) << 32) | key.rasterFlags);
				resolvedFor.push_back(key.vertexLayout);
			}
			for (const std::uint32_t state : modeStates[m])
				resolvedFor.push_back(state);
		}
		if (resolvedFor == a_state.shadowPipelinesResolvedFor) {
			++a_state.shadowSkipped;
			return;
		}
		++a_state.shadowResolved;
		bool resolved = true;
		auto& pipelines = DrawPipelines::Get();
		// A view's pipeline: the canonical depth-only technique and no layout (the draws read theirs by key slot), so slots of one
		// output share it and its bucket (CS_DCLF_SHADOW_MERGE=0: every technique bit and layout their own).
		static const bool merge = SwitchOnUnlessZero(Switch::ShadowMerge);
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_inputs.modes[m])
				continue;
			// An occlusion view takes its own keys (complete techniques) and its target's format. The shadow set is built for one
			// format (the lane's input, DrawPipelines::SetShadowInputs): a mode whose views draw into another has no pipeline in it.
			const auto& keys = IsOcclusionMode(m) ? a_tables.occlusionKeysUsed[OcclusionOfMode(m)] : a_tables.shadowKeysUsed;
			const DXGI_FORMAT format = IsOcclusionMode(m) ? a_inputs.occlusionFormats[OcclusionOfMode(m)] : a_inputs.dsvFormat;
			const bool inSet = format != DXGI_FORMAT_UNKNOWN && format == a_catalog->shadowFormat;
			for (const auto& key : keys) {
				const std::uint32_t technique = key.technique | ModeBitsOf(m);
				const ShadowPipelineKey slotKey{ key.technique, key.rasterFlags, key.vertexLayout };
				auto slotIt = a_lookups.shadowSlots.find(slotKey);
				if (slotIt == a_lookups.shadowSlots.end()) {
					// The latch's map rows hold every slot a publication can name (MakeRevision reserves for the scene lane's
					// slots and the keys used).
					slotIt = a_lookups.shadowSlots.emplace(slotKey, static_cast<std::uint32_t>(a_lookups.shadowSlotKeys.size())).first;
					a_lookups.shadowSlotKeys.push_back(slotKey);
					++a_lookups.shadowGeneration;
				}
				const std::uint32_t slot = slotIt->second;
				const std::uint32_t occlusion = IsOcclusionMode(m) ? OcclusionOfMode(m) : ~0u;
				// The key under each rasterizer state its mode's views draw with.
				for (const std::uint32_t state : modeStates[m]) {
					const ShadowPipelineKey viewKey = merge ? ShadowPipelineKey{ CanonicalShadowTechnique(technique), slotKey.rasterFlags, 0, state } :
					                                          ShadowPipelineKey{ technique, slotKey.rasterFlags, slotKey.vertexLayout, state };
					// Asked of the lane once per key (it keeps every key, and builds them all again after a format change). Everything it
					// builds is built at runtime: it logs each build a request starts, with the casters named here.
					if (a_state.shadowRequested.insert(viewKey).second)
						pipelines.RequestShadow(viewKey, key, occlusion);
					const auto* built = inSet ? a_catalog->FindShadow(viewKey) : nullptr;
					const std::uint32_t index = built ? built->setIndex : Lookups::kNone;
					// Pending (its program, its build or an input the lane waits for) is looked up again next pass; a failed key is
					// final, as is a mode the set has no format for (until the inputs above change).
					resolved &= !inSet || (built && (built->setIndex != PipelineCatalog::kNone || built->failed));
					auto [it, inserted] = a_lookups.shadowPipelines.try_emplace(viewKey, index);
					if (inserted || it->second != index) {
						++a_lookups.shadowGeneration;
						it->second = index;
					}
					auto& rows = a_lookups.shadowMapRows[m];
					if (rows.size() <= state) {
						rows.resize(std::size_t(state) + 1);
						++a_lookups.changes;
					}
					auto& row = rows[state];
					if (row.size() <= slot) {
						row.resize(slot + 1, Lookups::kNone);
						++a_lookups.changes;
					}
					// A row entry is its mode's: two modes whose techniques are the same (the occlusion maps' share their keys) share the
					// pipeline above but not the row, which moves the generation itself.
					if (row[slot] != index) {
						row[slot] = index;
						++a_lookups.shadowGeneration;
					}
				}
			}
		}
		// A pipeline not in the catalog yet (its program or its build still pending) is looked up again next pass. Swapped, so the scratch
		// keeps a buffer.
		if (resolved)
			std::swap(a_state.shadowPipelinesResolvedFor, resolvedFor);
		else
			a_state.shadowPipelinesResolvedFor.clear();
	}
}

namespace DCLF
{
	void IndirectDraws::ResolveLookups(const void* a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, LookupsResolveState& a_state)
	{
		if (!impl || failed)
			return;
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		// The Lighting pipelines once their programs can be built (the Lighting shader known: EngineShaders::Lighting, set once): the slots
		// whose witness moved. The parity resolves every slot after, which must write nothing.
		++a_state.calls;
		if (EngineShaders::Lighting() && ShaderPrograms::Get().Enabled()) {
			Draws::ResolveChangedPipelineLookups(tables, a_catalog, a_lookups, a_state);
			if (Diagnostics::Observe().scene && Diagnostics::ParityDue(a_state.calls, Draws::kPipelineParityOffset)) {
				const std::uint64_t writes = a_lookups.pipelines.Writes();
				const std::uint32_t versions = a_lookups.versionCounter;
				Draws::ResolvePipelineLookups(tables, a_catalog, a_lookups);
				a_state.pipelineParity.Check(writes == a_lookups.pipelines.Writes() && versions == a_lookups.versionCounter, [&] {
					return fmt::format("{} pipeline entries written by a full resolution after the witnessed one", a_lookups.pipelines.Writes() - writes);
				});
			}
		}
		// The shadow pipelines for the views the frame's start last posted, once the walk has classified the casters.
		auto inputs = impl->shadowEngine->shadowLookupInputs.load(std::memory_order_acquire);
		if (inputs && !inputs->enabled)
			inputs.reset();
		// The inputs the lookups' shadow pipelines are for, held by the lookups (the set commit's shadow readiness reads them, PhaseReady):
		// replaced together with the resolution below, so readiness and pipelines always agree. The held one is alive, so the pointer
		// compare is exact.
		if (a_lookups.shadowInputs.get() != static_cast<const void*>(inputs.get())) {
			a_lookups.shadowInputs = inputs;
			// Unique across lookups instances (a reset's new instance never repeats a value the commit last saw).
			static std::atomic<std::uint64_t> shadowInputsSerials{ 0 };
			a_lookups.shadowInputsSerial = shadowInputsSerials.fetch_add(1, std::memory_order_relaxed) + 1;
			++a_lookups.changes;
		}
		if (!inputs || tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return;
		ResolveShadowLookups(tables, a_catalog, *inputs, a_state, a_lookups);
	}

	void IndirectDraws::PostLookupInputs()
	{
		if (!impl || failed)
			return;
		// What the import thread answers the scene work's requests with, and the fixed bindings (the null descriptor, the engine's
		// samplers: made here, the render thread's), published before the scene work runs.
		GpuTextures::Get().PublishImportContext();
		UpdateShadowCapability();
		// The shadow views' frame inputs of the pipeline lane: the format its shadow set is built for (the views' depth
		// targets', or an occlusion map's while no shadow view has drawn) and the Utility shader. The lane builds the keys the scene lane
		// asks for, and its next catalog has what it built.
		auto& engine = *impl->shadowEngine;
		auto& views = engine.lastShadow;
		ShadowLookupInputs inputs;
		inputs.enabled = SceneToggles().shadows && views.known && globals::game::utilityShader;
		if (inputs.enabled) {
			DXGI_FORMAT shadowFormat = views.dsvFormat;
			for (const DXGI_FORMAT occlusionFormat : engine.OcclusionFormats())
				if (shadowFormat == DXGI_FORMAT_UNKNOWN)
					shadowFormat = occlusionFormat;
			DrawPipelines::Get().SetShadowInputs(shadowFormat, *globals::game::utilityShader);
			inputs.modes = views.modes;
			inputs.rasterStates = views.rasterStates;
			inputs.dsvFormat = views.dsvFormat;
			inputs.occlusionFormats = engine.OcclusionFormats();
		}
		// To the scene lane when they changed (latest wins): its shadow pipelines are resolved for them from its next pass on.
		if (inputs == engine.postedShadowLookups)
			return;
		engine.postedShadowLookups = inputs;
		engine.shadowLookupInputs.store(std::make_shared<const ShadowLookupInputs>(std::move(inputs)), std::memory_order_release);
	}
}
