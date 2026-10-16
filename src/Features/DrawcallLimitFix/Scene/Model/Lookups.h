#pragma once

// Lookups: the descriptor, sampler and pipeline indices a build reads instead of calling the engine, D3D11 or ORG, parallel to the
// scene tables. Written by the coordinator's scene lane, published copy-on-write with each publication, and read on the render
// thread and by the builds from the installed publication.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Scene/Ports/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	/**
	 * @brief A growable array in fixed chunks, shared copy-on-write between the scene lane's Lookups and its published copies
	 * (Lookups::Snapshot). After a copy, Freeze moves the writer's stamp past every chunk's, so the next write to a chunk copies it
	 * first and a published copy never sees a write. Sharing is decided by the stamp, never by a reference count another thread moves.
	 */
	template <class T, std::size_t kChunkSize = 64>
	class SharedChunks
	{
	public:
		class Iterator
		{
		public:
			Iterator(const SharedChunks* a_owner, std::size_t a_index) :
				owner(a_owner), index(a_index) {}
			const T& operator*() const { return (*owner)[index]; }
			Iterator& operator++()
			{
				++index;
				return *this;
			}
			bool operator!=(const Iterator& a_other) const { return index != a_other.index; }

		private:
			const SharedChunks* owner;
			std::size_t index;
		};

		std::size_t size() const { return count; }
		bool empty() const { return count == 0; }
		const T& operator[](std::size_t a_index) const { return (*chunks[a_index / kChunkSize].items)[a_index % kChunkSize]; }
		Iterator begin() const { return { this, 0 }; }
		Iterator end() const { return { this, count }; }

		/** @brief Writer: an entry to write, its chunk copied first when a published copy shares it. */
		T& Mutable(std::size_t a_index)
		{
			auto& chunk = chunks[a_index / kChunkSize];
			if (chunk.stamp != stamp) {
				chunk.items = std::make_shared<Items>(*chunk.items);
				chunk.stamp = stamp;
				++copies;
			}
			++writes;
			return (*chunk.items)[a_index % kChunkSize];
		}
		/** @brief Writer. A shrink resets the dropped entries, so a later growth exposes defaults. */
		void resize(std::size_t a_count)
		{
			for (std::size_t i = a_count; i < count; ++i)
				Mutable(i) = T{};
			count = a_count;
			const std::size_t needed = (a_count + kChunkSize - 1) / kChunkSize;
			if (chunks.size() > needed)
				chunks.resize(needed);
			while (chunks.size() < needed)
				chunks.push_back({ std::make_shared<Items>(), stamp });
			++writes;
		}
		/** @brief Writer, after a copy was taken: every chunk is shared from here on. */
		void Freeze() { ++stamp; }
		/** @brief Writes and chunk copies made through this instance (a copy inherits the counts). */
		std::uint64_t Writes() const { return writes; }
		std::uint64_t Copies() const { return copies; }

	private:
		using Items = std::array<T, kChunkSize>;
		struct Chunk
		{
			std::shared_ptr<Items> items;
			std::uint64_t stamp = 0;
		};
		std::vector<Chunk> chunks;
		std::size_t count = 0;
		std::uint64_t stamp = 1;
		std::uint64_t writes = 0, copies = 0;
	};

	/**
	 * @brief What a build needs from the render thread's services, resolved ahead of it. A build may not call the engine, D3D11,
	 * DXVK's interop or ORG's services (descriptors, pipeline lookups, uploads); it reads these plain tables, parallel to the
	 * SceneTables slots. Each entry keeps the key it was resolved for, so a reused slot never serves a previous tenant's indices.
	 *
	 * Coordinator's one instance, written by the scene work (Coordinator::ResolveLookups resolves pipelines against the pipeline
	 * lane's newest catalog), is published as an immutable Snapshot with that catalog; the frame binds that catalog's set versions.
	 *
	 * An object joins the set only once its entries are resolved (Coordinator::CommitSet), and a resolved entry stays drawable
	 * until its replacement arrives, so a missing member entry (a deferred draw) is a defect set parity reports.
	 *
	 * `generation` moves whenever an entry a main build may have read changes value; a payload built against an older one is not
	 * committed. `shadowGeneration` is the same for what a shadow build reads, so main-pass churn does not stale shadow jobs. A
	 * copy keeps instance, generations and versions.
	 */
	struct Lookups
	{
		static constexpr std::uint32_t kNone = ~0u;
		static constexpr std::uint32_t kTextureSlots = 16;

		/** @brief The registers a pipeline variant's shaders declare (a copy of DrawPipelines' RegisterUsage). */
		struct RegisterUsageBits
		{
			std::uint32_t vertexConstants = 0;  // b0-b13, bit per register
			std::uint32_t pixelConstants = 0;
			std::array<std::uint64_t, 2> textures{};  // pixel t0-t127
			std::uint32_t samplers = 0;                // pixel s0-s15

			bool UsesTexture(std::uint32_t a_register) const { return (textures[a_register / 64] >> (a_register % 64)) & 1; }
		};

		/** @brief Per pipeline slot (parallel to SceneTables::pipelines). */
		struct Pipeline
		{
			PipelineKey key{};
			std::uint32_t setIndex = kNone;  // in the published PipelineCatalog; kNone while compiling or unused
			bool requested = false;          // key asked of the pipeline lane (DrawPipelines::RequestLighting), once per key
			// Each Lighting variable's cbuffer offset per stage (DrawPipelines.h, ConstantTables), reflected from DCLF's modules.
			std::vector<std::uint8_t> vsTable, psTable;
			std::array<RegisterUsageBits, 2> usage{};  // by variant (kColorVariant, kDepthVariant)
			// The technique's shadow mask (t14), if it binds one (Coordinator::ResolveMaskBinding).
			std::uint32_t shadowMaskIndex = kNone;
			std::shared_ptr<const void> shadowMaskOwner;
			ID3D11ShaderResourceView* shadowMaskView = nullptr;
			std::uint32_t shadowMaskTextureGeneration = 0;
			std::uint32_t version = 0;                 // new whenever any of the above changes (NextVersion)
		};

		/**
		 * @brief The owners a material entry's indices hold, sealed together and never written after: the exact imports of its textures and
		 * feature textures, and their ORG execution lease (GpuTextures::Seal), whose last release runs on the cleanup queue. Shared by a
		 * material's slots whose owners are the same (Coordinator::ResolveMaterialBinding).
		 */
		struct MaterialOwners
		{
			std::array<std::shared_ptr<const void>, kTextureSlots> textures{};
			std::array<std::shared_ptr<const void>, Extensions::kFeatureTextureSlots> features{};
			std::shared_ptr<const void> lease;
		};

		/**
		 * @brief Per material slot (parallel to SceneTables::materials). Written by Coordinator::ResolveMaterialBinding, versioned
		 * and logged before the next commit or publication (Coordinator::VersionMaterialBindings). Plain values and one owner, so a
		 * publication's chunk copy (SharedChunks) moves one reference count per entry.
		 */
		struct Material
		{
			std::pair<const RE::BSShaderMaterial*, std::uint32_t> key{};
			std::array<std::uint32_t, kTextureSlots> textureIndex{};  // descriptor heap indices; kNone where unresolvable
			// Pins the exact imports of every index here through this slot incarnation (null: none held); a new block whenever an owner
			// changes, so its identity names this owner set.
			std::shared_ptr<const MaterialOwners> bindingBlock;
			bool resolved = false;
			// What textureIndex was resolved from, so an unchanged material is not resolved again.
			std::array<ID3D11ShaderResourceView*, kTextureSlots> views{};
			// The feature textures (MaterialRecord::featureTextures, by slot: Extensions::FeatureTextureRegister): indices and views.
			std::array<std::uint32_t, Extensions::kFeatureTextureSlots> featureIndex{};
			std::array<ID3D11ShaderResourceView*, Extensions::kFeatureTextureSlots> featureViews{};
			std::uint32_t written = 0;
			std::uint32_t texturesGeneration = 0;
			std::uint64_t recordVersion = 0;  // last material record inspected for texture bindings
			std::uint32_t version = 0;  // new whenever key, resolved, textureIndex or featureIndex changes (NextVersion)
		};

		// A publication copies only the chunks written since the last one (SharedChunks).
		SharedChunks<Pipeline> pipelines;
		SharedChunks<Material> materials;
		// Parallel to materials: Material::version, compact for the builds' scans over every pair.
		std::vector<std::uint32_t> materialVersions;
		// Material slots changed or retired, for MainBuild::UpdateRegionPairs. Copies keep logGeneration, so a reader continues
		// across publications; one on another instance, or behind the trim, starts again.
		EventLog<std::uint32_t> materialLog;
		std::uint32_t logGeneration = NextLogGeneration();
		static std::uint32_t NextLogGeneration()
		{
			static std::atomic<std::uint32_t> counter{ 0 };
			return counter.fetch_add(1, std::memory_order_relaxed) + 1;
		}
		// The shared entries (Coordinator::UpdateSharedBindings). Sampler heap index per (address mode, filter mode) of the engine's
		// sampler table, made once by the render thread (GpuTextures::Fixed); kNone where the engine has no state.
		std::array<std::uint32_t, 4 * 5> samplers;
		std::shared_ptr<const void> sharedBindingBlock;  // null, fixed samplers and projected textures
		bool samplersResolved = false;
		std::uint32_t nullTexture = kNone;
		std::array<std::uint32_t, 4> projectedTextures{ kNone, kNone, kNone, kNone };
		std::array<std::shared_ptr<const void>, 4> projectedOwners{};
		// Shadow pipelines per (technique with mode bits, raster flags, vertex layout).
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowPipelines;
		// A shadow input names its caster's key slot (base key: no mode bits, no view state), not a pipeline, so one slot serves
		// every mode. Append-only. shadowMapRows[mode][state][slot]: the slot's pipeline under that mode and view rasterizer state
		// (DrawPipelines::ShadowRasterStateId), kNone while missing; a view's row goes into the shadow latch block for BuildDrawsCS.
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowSlots;
		std::vector<ShadowPipelineKey> shadowSlotKeys;
		std::array<std::vector<std::vector<std::uint32_t>>, 5> shadowMapRows;  // by mode (kShadowModeCount)
		/** @brief A view's map row (its mode and rasterizer state), empty when it has none yet. */
		std::span<const std::uint32_t> ShadowMapRow(std::uint32_t a_mode, std::uint32_t a_state) const
		{
			if (a_mode >= shadowMapRows.size() || a_state >= shadowMapRows[a_mode].size())
				return {};
			return shadowMapRows[a_mode][a_state];
		}
		/** @brief A key slot's pipeline under a mode and a view rasterizer state, kNone when it has none yet. */
		std::uint32_t ShadowMapPipeline(std::uint32_t a_mode, std::uint32_t a_state, std::uint32_t a_slot) const
		{
			const auto row = ShadowMapRow(a_mode, a_state);
			return a_slot < row.size() ? row[a_slot] : kNone;
		}
		// Alpha-tested casters' diffuse textures answered (kNone: rejected; Coordinator::ResolveShadowTextureBinding). One still
		// pending is absent, so its casters wait.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::uint32_t> shadowTextures;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::shared_ptr<const void>> shadowTextureOwners;
		std::uint32_t pipelineSetGeneration = ~0u;
		// The shadow set generation the indices above belong to (the published catalog's PipelineCatalog::shadowGeneration).
		std::uint32_t shadowSetGeneration = ~0u;
		// The shadow views' modes and states the shadow pipelines were resolved for (ShadowLookupInputs; null: no shadow view),
		// read by the set commit's readiness (IndirectDraws::PhaseReady). shadowInputsSerial (unique across instances) moves when
		// they are replaced, making Coordinator::CommitSet retake members and waiting slots.
		std::shared_ptr<const void> shadowInputs;
		std::uint64_t shadowInputsSerial = 0;
		// MainBuild's kept bindings key on versions: Pipeline::version, Material::version, and this one for the entries every
		// record reads (null texture, samplers, projected textures). Unique across all of them.
		std::uint32_t sharedVersion = 0;
		std::uint32_t versionCounter = 0;
		std::uint32_t NextVersion() { return ++versionCounter; }
		std::uint32_t generation = 0;
		std::uint32_t shadowGeneration = 0;
		// A reset makes a new instance whose generations restart, so state kept against a generation (IndirectDraws' shadow row
		// buckets) keys on this too. A copy keeps it.
		std::uint64_t instance = NextInstance();
		// Writes outside the chunks that nothing versions (a row grown, a binding block sealed): part of ChangeKey, which lets an
		// unchanged instance republish its last copy.
		std::uint64_t changes = 0;

		Lookups() { samplers.fill(kNone); }
		static std::uint64_t NextInstance()
		{
			static std::atomic<std::uint64_t> next{ 0 };
			return next.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		static std::uint32_t SamplerIndex(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
		{
			return a_addressMode < 4 && a_filterMode < 5 ? a_addressMode * 5 + a_filterMode : kNone;
		}
		std::uint32_t Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode) const
		{
			const auto i = SamplerIndex(a_addressMode, a_filterMode);
			return i == kNone ? kNone : samplers[i];
		}

		/** @brief Equal while nothing was written: the instance and every counter a write moves. */
		std::array<std::uint64_t, 6> ChangeKey() const
		{
			return { instance, (std::uint64_t(versionCounter) << 32) | generation, (std::uint64_t(shadowGeneration) << 32) | sharedVersion, materialLog.End(),
				changes, pipelines.Writes() + materials.Writes() };
		}
		/** @brief Scene lane: an immutable copy sharing every chunk (SharedChunks::Freeze); maps and small tables are copied whole. */
		std::shared_ptr<const Lookups> Snapshot()
		{
			auto copy = std::make_shared<const Lookups>(*this);
			pipelines.Freeze();
			materials.Freeze();
			return copy;
		}
	};

	/**
	 * @brief The scene lane's unpublished bookkeeping for Coordinator::ResolveLookups: shadow keys asked of the pipeline lane
	 * (DrawPipelines::RequestShadow, each once; the lane rebuilds them after a format change), and the inputs of the last shadow
	 * resolution that found every pipeline (empty while one is missing), so an identical one is skipped.
	 */
	struct LookupsResolveState
	{
		ankerl::unordered_dense::set<ShadowPipelineKey, ShadowPipelineKeyHash> shadowRequested;
		std::vector<std::uint64_t> shadowPipelinesResolvedFor;
		// The Lighting pipelines' witnesses: the catalog (its identity, revision and generation) and the lookups instance every slot was
		// resolved against, and per slot its key's binding version and used bit then (~0: not resolved). A slot whose witness stands would
		// resolve to what it holds: a key changes only with a new binding version, and a catalog is immutable.
		const void* pipelineCatalog = nullptr;
		std::uint64_t pipelineCatalogRevision = 0;
		std::uint32_t pipelineCatalogGeneration = ~0u;
		std::uint64_t pipelineInstance = 0;
		std::vector<std::uint64_t> pipelineWitness;
		// The shadow resolution's scratch, kept so an unchanged pass allocates nothing (by mode: kShadowModeCount).
		std::vector<std::uint64_t> shadowResolvedScratch;
		std::array<std::vector<std::uint32_t>, 5> shadowModeStates;
		// The work the witnesses saved, and their parity (a full resolution after the witnessed one writes nothing; CS_DCLF_PERSISTENT_PARITY),
		// for the material records' line.
		std::uint64_t calls = 0, pipelinesChecked = 0, pipelinesResolved = 0, pipelineSweeps = 0, shadowSkipped = 0, shadowResolved = 0;
		Diagnostics::ParityCounter pipelineParity;
	};
}
