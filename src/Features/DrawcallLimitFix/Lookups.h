#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "ConstantEvaluator.h"
#include "Records.h"

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
	 * pipeline lookups, uploads). Everything the epoch used to fetch from those as it went is resolved here
	 * by the render thread, where the services are available and the GPU is busy - the pipeline set indices
	 * and register usage at EarlyPrepass, the descriptor indices inside an epoch's own preparation - and
	 * read by the build as plain tables parallel to SceneStore's slot tables. Each entry keeps the key of the
	 * slot it was resolved for, so a swept and reused slot is never served a previous tenant's indices.
	 *
	 * A slot without an entry is not skipped silently: the build defers the draw with a reason, and the
	 * render thread resolves the entry at its next opportunity, so the draw lands a frame later (the same
	 * class of delay as a pipeline still compiling, and never a hole: the native loop is only told to
	 * withhold what DCLF has drawn).
	 *
	 * `generation` changes whenever an entry a build may have read changes its value (a descriptor evicted
	 * and re-imported, the pipeline set recreated, the tables reset); a payload built against an older
	 * generation is not committed.
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
			std::uint32_t version = 0;                 // new whenever any of the above changes (NextVersion)
		};

		/** @brief Per material slot (parallel to SceneStore::Tables::materials). */
		struct Material
		{
			std::pair<const RE::BSShaderMaterial*, std::uint32_t> key{};
			std::array<std::uint32_t, kTextureSlots> textureIndex{};  // descriptor heap indices; kNone where unresolvable
			bool resolved = false;
			// What textureIndex was resolved from, so an unchanged material is not resolved again (every epoch
			// refreshes the lookups): its views, which of them were written, GpuTextures' generation, and when.
			std::array<ID3D11ShaderResourceView*, kTextureSlots> views{};
			// The feature textures (MaterialRecord::featureTextures, kFeatureMaterialRegisters): indices and views.
			std::array<std::uint32_t, kFeatureMaterialTextures> featureIndex{};
			std::array<ID3D11ShaderResourceView*, kFeatureMaterialTextures> featureViews{};
			std::uint32_t written = 0;
			std::uint32_t texturesGeneration = 0;
			std::uint32_t resolvedFrame = 0;
			std::uint32_t version = 0;  // new whenever key, resolved, textureIndex or featureIndex changes (NextVersion)
		};

		std::vector<Pipeline> pipelines;
		std::vector<Material> materials;
		// The sampler heap index per (address mode, filter mode) of the engine's sampler table: all of them,
		// resolved once (GpuTextures::Sampler), kNone where the engine has no state.
		std::array<std::uint32_t, 4 * 5> samplers;
		bool samplersResolved = false;
		std::uint32_t nullTexture = kNone;
		std::array<std::uint32_t, 4> projectedTextures{ kNone, kNone, kNone, kNone };
		// Shadow pipelines per (technique with mode bits, raster flags, vertex layout), and the alpha-tested
		// casters' diffuse textures.
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowPipelines;
		// A shadow input names its caster's key slot (technique with mode bits, raster flags without a view
		// state, vertex layout), not a pipeline: the views of one render mode share the inputs but not the
		// rasterizer state. Slots are append-only. shadowMapRows[state][slot] is the pipeline of that slot's
		// key under that view rasterizer state (DrawPipelines::ShadowRasterStateId), kNone where there is none
		// yet; a view's row goes into the shadow latch block for BuildDrawsCS to resolve its draws through.
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowSlots;
		std::vector<ShadowPipelineKey> shadowSlotKeys;
		std::array<std::vector<std::uint32_t>, 16> shadowMapRows;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::uint32_t> shadowTextures;
		std::uint32_t pipelineSetGeneration = ~0u;
		// The builds' kept bindings (IndirectDraws' PersistentBindings) key on versions rather than on the entries: a
		// pipeline's and a material's own (Pipeline::version, Material::version), and this one for the entries every
		// record reads (the null texture, the samplers, the projected textures). Unique across all of them.
		std::uint32_t sharedVersion = 0;
		std::uint32_t versionCounter = 0;
		std::uint32_t NextVersion() { return ++versionCounter; }
		std::uint32_t generation = 0;

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
