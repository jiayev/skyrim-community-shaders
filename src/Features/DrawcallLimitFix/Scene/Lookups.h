#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "ConstantEvaluator.h"
#include "Records.h"
#include "Features/DrawcallLimitFix/Common/KeptState.h"

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	/**
	 * @brief A growable array in fixed chunks, shared copy-on-write between the scene lane's Lookups and the copies it publishes
	 * (Lookups::Snapshot).
	 *
	 * A copy shares every chunk. The writer's Freeze then marks them all shared (its stamp moves past theirs), so its next write to a
	 * chunk copies that chunk first, once per publication, and a published copy never sees a write. Whether a chunk is shared is
	 * decided by the stamp, not by its reference count: nothing reads a count another thread moves. Reads are by index, as a vector's.
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

		/** @brief The writer: an entry to write, its chunk copied first when a published copy shares it. */
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
		/** @brief The writer: entries past the count are default in their chunk (a shrink resets them), so a growth exposes defaults. */
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
		/** @brief The writer, after a copy was taken: every chunk is shared from here on. */
		void Freeze() { ++stamp; }
		/** @brief Writes and chunk copies made through this instance (a copy starts from the counts of what it copied). */
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
	 * @brief What an epoch's build needs from the render thread's services, resolved ahead of the build.
	 *
	 * A build may not call the engine, D3D11, DXVK's interop, or ORG's services (descriptor allocation,
	 * pipeline lookups, uploads). Everything it needs from those is resolved here, ahead of it, and read by the build as plain tables
	 * parallel to SceneStore's slot tables. Each entry keeps the key of the slot it was resolved for, so a swept and reused slot is
	 * never served a previous tenant's indices.
	 *
	 * The scene lane's (T6b2c step 5): one instance, SceneStore's, written by the scene work - the texture bindings straight in where
	 * they are made (MaterialBindings, SharedBindings), the pipeline entries and the shadow pipelines resolved against the pipeline
	 * lane's newest catalog before each commit and publication (SceneStore::ResolveLookups) - and published with the scene as an
	 * immutable copy (Snapshot), with the catalog it was resolved from. The frame reads the installed publication's
	 * (SceneStore::GetLookups) and binds that catalog's set versions, so the indices a frame resolves and the set it binds agree, and
	 * the entries are parallel to the tables they were published with.
	 *
	 * An object joins the DCLF set only once the entries it draws with are resolved (SceneStore::CommitSet, against the scene lane's
	 * own), and a resolved entry stays drawable (a view re-imported keeps the old one until the new one arrives), so a build never
	 * finds a member's entry missing; one that does defers the draw with a reason, which set parity reports as a defect.
	 *
	 * `generation` changes whenever an entry a main-pass build may have read changes its value (a descriptor evicted
	 * and re-imported, the pipeline set recreated, the tables reset); a payload built against an older generation is
	 * not committed. `shadowGeneration` is the same for what a shadow build reads (the shadow textures, slots and
	 * pipelines, the null texture and the samplers), so the main pass's material churn does not stale its jobs. A copy keeps its
	 * instance, its generations and its versions: equal ones are equal entries, whichever publication of the instance holds them.
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

		/** @brief Per pipeline slot (parallel to SceneStore::Tables::pipelines). */
		struct Pipeline
		{
			PipelineKey key{};
			std::uint32_t setIndex = kNone;  // DrawPipelines set index (the PipelineCatalog's published with these lookups); kNone while compiling or unused
			bool requested = false;          // key asked of the pipeline lane (DrawPipelines::RequestLighting), once per key
			// Which cbuffer offset each Lighting variable has, per stage: the catalog entry's (DrawPipelines.h, ConstantTables),
			// reflected from DCLF's own modules with the build.
			std::vector<std::uint8_t> vsTable, psTable;
			std::array<RegisterUsageBits, 2> usage{};  // by variant (kColorVariant, kDepthVariant)
			// The technique's shadow mask (t14), if it binds one: written by the scene work where it resolves it (T6b2c:
			// SceneStore::ResolveMaskBinding).
			std::uint32_t shadowMaskIndex = kNone;
			std::shared_ptr<const void> shadowMaskOwner;
			ID3D11ShaderResourceView* shadowMaskView = nullptr;
			std::uint32_t shadowMaskTextureGeneration = 0;
			std::uint32_t version = 0;                 // new whenever any of the above changes (NextVersion)
		};

		/**
		 * @brief Per material slot (parallel to SceneStore::Tables::materials). Written by the scene work where it resolves the slot
		 * (T6b2c: SceneStore::ResolveMaterialBinding), versioned and logged before the next commit or publication
		 * (SceneStore::VersionMaterialBindings).
		 */
		struct Material
		{
			std::pair<const RE::BSShaderMaterial*, std::uint32_t> key{};
			std::array<std::uint32_t, kTextureSlots> textureIndex{};  // descriptor heap indices; kNone where unresolvable
			std::array<std::shared_ptr<const void>, kTextureSlots> textureOwners{};  // pins exact imports through this slot incarnation
			std::shared_ptr<const void> bindingBlock;  // immutable ORG ownership root for this record version
			bool resolved = false;
			// What textureIndex was resolved from, so an unchanged material is not resolved again.
			std::array<ID3D11ShaderResourceView*, kTextureSlots> views{};
			// The feature textures (MaterialRecord::featureTextures, kFeatureMaterialRegisters): indices and views.
			std::array<std::uint32_t, kFeatureMaterialTextures> featureIndex{};
			std::array<std::shared_ptr<const void>, kFeatureMaterialTextures> featureOwners{};
			std::array<ID3D11ShaderResourceView*, kFeatureMaterialTextures> featureViews{};
			std::uint32_t written = 0;
			std::uint32_t texturesGeneration = 0;
			std::uint64_t recordVersion = 0;  // last material record inspected for texture bindings
			std::uint32_t version = 0;  // new whenever key, resolved, textureIndex or featureIndex changes (NextVersion)
		};

		// Shared by chunks with the publications (SharedChunks): a publication copies only the chunks written since the last one.
		SharedChunks<Pipeline> pipelines;
		SharedChunks<Material> materials;
		// Parallel to materials: each entry's version (Material::version), compact for the builds' scans over every pair.
		std::vector<std::uint32_t> materialVersions;
		// The material slots whose version changed (or that were retired), for the resident region's pairs
		// (MainBuild::UpdateRegionPairs). Its generation is this instance's, kept by its copies (a reader continues from one publication
		// into the next): a reader of another instance's log starts again. Trimmed by the scene lane (a reader behind starts again).
		EventLog<std::uint32_t> materialLog;
		std::uint32_t logGeneration = NextLogGeneration();
		static std::uint32_t NextLogGeneration()
		{
			static std::atomic<std::uint32_t> counter{ 0 };
			return counter.fetch_add(1, std::memory_order_relaxed) + 1;
		}
		// The shared entries, written by the scene work (T6b2c: SceneStore::UpdateSharedBindings). The sampler heap index per (address
		// mode, filter mode) of the engine's sampler table: all of them, made once by the render thread (GpuTextures::Fixed), kNone where
		// the engine has no state.
		std::array<std::uint32_t, 4 * 5> samplers;
		std::shared_ptr<const void> sharedBindingBlock;  // null, fixed samplers and projected textures
		bool samplersResolved = false;
		std::uint32_t nullTexture = kNone;
		std::array<std::uint32_t, 4> projectedTextures{ kNone, kNone, kNone, kNone };
		std::array<std::shared_ptr<const void>, 4> projectedOwners{};
		// Shadow pipelines per (technique with mode bits, raster flags, vertex layout), and the alpha-tested
		// casters' diffuse textures.
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowPipelines;
		// A shadow input names its caster's key slot (its base technique - a caster's without any render mode's bits, an occlusion
		// view's whole - raster flags without a view state, vertex layout), not a pipeline: one slot for every mode an object draws
		// in, and the views of a mode share the inputs but not the rasterizer state. Slots are append-only.
		// shadowMapRows[mode][state][slot] is the pipeline of that slot's technique with the mode's bits under that view rasterizer
		// state (DrawPipelines::ShadowRasterStateId, as many as are registered; a state may serve several modes), kNone where there is
		// none yet; a view's row goes into the shadow latch block for BuildDrawsCS to resolve its draws through.
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
		// The alpha-tested casters' diffuse textures answered (kNone: rejected), written by the scene work as they are answered or let go
		// (T6b2c: SceneStore::ResolveShadowTextureBinding); one still asked for is not here, so its casters wait.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::uint32_t> shadowTextures;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::shared_ptr<const void>> shadowTextureOwners;
		std::uint32_t pipelineSetGeneration = ~0u;
		// The shadow set generation the indices above are of (PipelineCatalog::shadowGeneration, the catalog published with them).
		std::uint32_t shadowSetGeneration = ~0u;
		// The shadow views' modes and rasterizer states the shadow pipelines above were resolved for (IndirectDraws' ShadowLookupInputs, as
		// the frame's start posted them; null: no shadow view drawn), immutable: what the set commit's shadow readiness reads
		// (IndirectDraws::PhaseReady), never the render thread's own lists. shadowInputsSerial moves whenever they are replaced (unique
		// across instances: IndirectDraws::ResolveLookups), which takes the set's members and waiting slots again (SceneStore::CommitSet).
		std::shared_ptr<const void> shadowInputs;
		std::uint64_t shadowInputsSerial = 0;
		// The builds' kept bindings (IndirectDraws' PersistentBindings) key on versions rather than on the entries: a
		// pipeline's and a material's own (Pipeline::version, Material::version), and this one for the entries every
		// record reads (the null texture, the samplers, the projected textures). Unique across all of them.
		std::uint32_t sharedVersion = 0;
		std::uint32_t versionCounter = 0;
		std::uint32_t NextVersion() { return ++versionCounter; }
		std::uint32_t generation = 0;
		std::uint32_t shadowGeneration = 0;
		// Which lookups these are: a reset makes new ones, whose generations start again, so what is kept against a generation
		// (IndirectDraws' shadow row buckets) keys on this too. A copy keeps it.
		std::uint64_t instance = NextInstance();
		// The scene lane's count of its writes outside the chunks (a row grown, a binding block sealed, anything not versioned): with the
		// chunks' writes and the generations, what tells an unchanged instance (ChangeKey), whose last copy is published again.
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

		/** @brief Equal while nothing was written: the instance, every counter a write moves. */
		std::array<std::uint64_t, 6> ChangeKey() const
		{
			return { instance, (std::uint64_t(versionCounter) << 32) | generation, (std::uint64_t(shadowGeneration) << 32) | sharedVersion, materialLog.End(),
				changes, pipelines.Writes() + materials.Writes() };
		}
		/**
		 * @brief The scene lane, publishing: an immutable copy sharing every chunk with this instance, whose next writes copy a chunk
		 * first (SharedChunks::Freeze). The maps and the small tables are copied whole.
		 */
		std::shared_ptr<const Lookups> Snapshot()
		{
			auto copy = std::make_shared<const Lookups>(*this);
			pipelines.Freeze();
			materials.Freeze();
			return copy;
		}
	};

	/**
	 * @brief The scene lane's bookkeeping beside its Lookups, never published (SceneStore::ResolveLookups): the shadow view keys asked of
	 * the pipeline lane (DrawPipelines::RequestShadow, each once: the lane keeps every key and builds them again after a format change),
	 * and what the last shadow resolution that found every shadow pipeline was made for (empty when one is still missing: the same again
	 * is skipped).
	 */
	struct LookupsResolveState
	{
		ankerl::unordered_dense::set<ShadowPipelineKey, ShadowPipelineKeyHash> shadowRequested;
		std::vector<std::uint64_t> shadowPipelinesResolvedFor;
	};
}
