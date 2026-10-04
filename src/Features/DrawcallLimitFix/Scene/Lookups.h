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
	 * @brief What an epoch's build needs from the render thread's services, resolved ahead of the build.
	 *
	 * A build may not call the engine, D3D11, DXVK's interop, or ORG's services (descriptor allocation,
	 * pipeline lookups, uploads). Everything it needs from those is resolved here
	 * by the render thread, where the services are available and the GPU is busy - the pipeline set indices
	 * and register usage at EarlyPrepass, the descriptor indices inside an epoch's own preparation - and
	 * read by the build as plain tables parallel to SceneStore's slot tables. Each entry keeps the key of the
	 * slot it was resolved for, so a swept and reused slot is never served a previous tenant's indices.
	 *
	 * An object joins the DCLF set only once the entries it draws with are resolved (SceneStore::CommitSet), and a resolved
	 * entry stays drawable (a view re-imported keeps the old one until the new one arrives), so a build never finds a member's
	 * entry missing; one that does defers the draw with a reason, which set parity reports as a defect.
	 *
	 * `generation` changes whenever an entry a main-pass build may have read changes its value (a descriptor evicted
	 * and re-imported, the pipeline set recreated, the tables reset); a payload built against an older generation is
	 * not committed. `shadowGeneration` is the same for what a shadow build reads (the shadow textures, slots and
	 * pipelines, the null texture and the samplers), so the main pass's material churn does not stale its jobs.
	 *
	 * An epoch refreshes the lookups after it has taken its worker's build (which read them as they were at the kick),
	 * or before it builds inline: what changed since reaches the next build. The main pass's kicks refresh them first,
	 * outside an epoch (a view never imported is asked for again inside one); the shadow kick takes them as the last
	 * shadow epoch left them, unless shadowRefreshDue says they are behind (a reset, a recreated pipeline set).
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
			std::uint32_t setIndex = kNone;  // DrawPipelines set index; kNone while compiling or unused
			// Which cbuffer register each Lighting variable lives at, per stage, copied from the game's shader
			// objects so the build never touches BSGraphics::*Shader.
			std::vector<std::uint8_t> vsTable, psTable;
			std::array<RegisterUsageBits, 2> usage{};  // by variant (kColorVariant, kDepthVariant)
			std::uint32_t shadowMaskIndex = kNone;     // the technique's shadow mask (t14) this frame, if it binds one
			std::shared_ptr<const void> shadowMaskOwner;
			ID3D11ShaderResourceView* shadowMaskView = nullptr;
			std::uint32_t shadowMaskTextureGeneration = 0;
			std::uint32_t version = 0;                 // new whenever any of the above changes (NextVersion)
		};

		/** @brief Per material slot (parallel to SceneStore::Tables::materials). */
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

		std::vector<Pipeline> pipelines;
		std::vector<Material> materials;
		// Parallel to materials: each entry's version (Material::version), compact for the builds' scans over every pair.
		std::vector<std::uint32_t> materialVersions;
		// The material slots whose version changed (or that were retired), for the resident region's pairs
		// (MainBuild::UpdateRegionPairs). Its generation is this instance's: a reader of another instance's log starts again.
		EventLog<std::uint32_t> materialLog;
		std::uint32_t logGeneration = NextLogGeneration();
		static std::uint32_t NextLogGeneration()
		{
			static std::atomic<std::uint32_t> counter{ 0 };
			return counter.fetch_add(1, std::memory_order_relaxed) + 1;
		}
		// The sampler heap index per (address mode, filter mode) of the engine's sampler table: all of them,
		// resolved once (GpuTextures::Sampler), kNone where the engine has no state.
		std::array<std::uint32_t, 4 * 5> samplers;
		std::shared_ptr<const void> sharedBindingBlock;  // null, fixed samplers and projected textures
		bool samplersResolved = false;
		std::uint32_t nullTexture = kNone;
		std::array<std::uint32_t, 4> projectedTextures{ kNone, kNone, kNone, kNone };
		std::array<std::shared_ptr<const void>, 4> projectedOwners{};
		// Shadow pipelines per (technique with mode bits, raster flags, vertex layout), and the alpha-tested
		// casters' diffuse textures.
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowPipelines;
		// A shadow input names its caster's key slot (technique with mode bits, raster flags without a view
		// state, vertex layout), not a pipeline: the views of one render mode share the inputs but not the
		// rasterizer state. Slots are append-only. shadowMapRows[state][slot] is the pipeline of that slot's
		// key under that view rasterizer state (DrawPipelines::ShadowRasterStateId, as many as are registered),
		// kNone where there is none yet; a view's row goes into the shadow latch block for BuildDrawsCS to
		// resolve its draws through.
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowSlots;
		std::vector<ShadowPipelineKey> shadowSlotKeys;
		std::vector<std::vector<std::uint32_t>> shadowMapRows;
		/** @brief A view rasterizer state's map row, empty when it has none yet. */
		std::span<const std::uint32_t> ShadowMapRow(std::uint32_t a_state) const
		{
			return a_state < shadowMapRows.size() ? std::span<const std::uint32_t>(shadowMapRows[a_state]) : std::span<const std::uint32_t>();
		}
		/** @brief A key slot's pipeline under a view rasterizer state, kNone when it has none yet. */
		std::uint32_t ShadowMapPipeline(std::uint32_t a_state, std::uint32_t a_slot) const
		{
			const auto row = ShadowMapRow(a_state);
			return a_slot < row.size() ? row[a_slot] : kNone;
		}
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::uint32_t> shadowTextures;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::shared_ptr<const void>> shadowTextureOwners;
		ankerl::unordered_dense::set<ID3D11ShaderResourceView*> pendingShadowTextures;
		std::uint32_t pipelineSetGeneration = ~0u;
		// The builds' kept bindings (IndirectDraws' PersistentBindings) key on versions rather than on the entries: a
		// pipeline's and a material's own (Pipeline::version, Material::version), and this one for the entries every
		// record reads (the null texture, the samplers, the projected textures). Unique across all of them.
		std::uint32_t sharedVersion = 0;
		std::uint32_t versionCounter = 0;
		std::uint32_t NextVersion() { return ++versionCounter; }
		std::uint32_t generation = 0;
		std::uint32_t shadowGeneration = 0;
		bool shadowRefreshDue = true;

		Lookups() { samplers.fill(kNone); }

		static std::uint32_t SamplerIndex(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
		{
			return a_addressMode < 4 && a_filterMode < 5 ? a_addressMode * 5 + a_filterMode : kNone;
		}
		std::uint32_t Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode) const
		{
			const auto i = SamplerIndex(a_addressMode, a_filterMode);
			return i == kNone ? kNone : samplers[i];
		}
	};
}
