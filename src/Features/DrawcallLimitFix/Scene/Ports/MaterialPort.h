#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <memory>

#include <d3d11.h>
#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Runtime/SceneWake.h"
#include "Features/DrawcallLimitFix/Scene/Ports/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPortFrame.h"

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	/**
	 * @brief BSLightingShader::SetupMaterial ported (engine notes, "BSLightingShader::SetupMaterial: where every material
	 * constant comes from"), Community Shaders' hooks on it included (TruePBR's replacement, Advanced Skin's and TerrainHelper's
	 * extra textures).
	 *
	 * The port is a pure function of two inputs, so the scene work can make a material record without the engine:
	 *
	 * - MaterialSnapshot: the material's own fields (the M sources) and the views of its textures, captured where the material is
	 *   written (the writers MaterialSources hooks) or first seen (an attach's capture).
	 * - MaterialFrame: the per-frame sources (S, G, R: the shader object, engine globals, render targets), sampled by the render
	 *   thread at the frame's start.
	 */
	namespace MaterialPort
	{
		/** @brief Bytes a snapshot keeps of a material object: at least sizeof of the largest class the port reads (checked by Capture). */
		inline constexpr std::uint32_t kMaxMaterialBytes = 0x400;
		/** @brief Texture fields a snapshot resolves (every NiSourceTexture pointer field of the largest class). */
		inline constexpr std::uint32_t kMaxTextureFields = 32;

		/** @brief One texture pointer field of the material (its byte offset) and the view SetupMaterial would bind for it now. */
		struct TextureField
		{
			std::uint16_t offset = 0;
			ID3D11ShaderResourceView* view = nullptr;
		};

		/** @brief A material's fields and texture views at one point (pure data: no engine pointer is followed after Capture). */
		struct MaterialSnapshot
		{
			const void* key = nullptr;     // the material
			const void* vtable = nullptr;  // its class
			std::uint32_t feature = 0;     // BSShaderMaterial::GetFeature()
			bool pbr = false;              // CS's BSLightingShaderMaterialPBR
			bool effect = false;           // a BSEffectShaderMaterial (EvaluateEffect)
			std::uint32_t size = 0;        // bytes taken (the class's size)
			std::array<std::byte, kMaxMaterialBytes> bytes{};
			std::uint32_t textureCount = 0;
			std::array<TextureField, kMaxTextureFields> textures{};

			template <class T>
			T At(std::size_t a_offset) const
			{
				T value{};
				if (a_offset + sizeof(T) <= size)
					std::memcpy(&value, bytes.data() + a_offset, sizeof(T));
				return value;
			}
			/** @brief The view resolved for the texture field at a_offset (null: no texture there, or no view). */
			ID3D11ShaderResourceView* View(std::uint16_t a_offset) const
			{
				for (std::uint32_t i = 0; i < textureCount; ++i)
					if (textures[i].offset == a_offset)
						return textures[i].view;
				return nullptr;
			}
			/** @brief Whether the field at a_offset holds a texture (a non-null NiSourceTexture pointer when captured). */
			bool HasTexture(std::uint16_t a_offset) const { return At<const void*>(a_offset) != nullptr; }
		};

		/** @brief Any thread that may read the material (its writer, or the render thread): the snapshot of a_material. */
		bool Capture(const RE::BSShaderMaterial& a_material, MaterialSnapshot& a_out);

		/**
		 * @brief A snapshot on its way to the scene work, with a reference on the material its capture took (the material's
		 * count, InterlockedIncrement: any thread, the material alive where it is captured). The scene work hands the reference on: to a
		 * material slot, or to the render thread's releases (the engine's release is its); never dropped where it lands.
		 */
		struct HeldSnapshot
		{
			MaterialSnapshot snapshot;
			std::uint64_t sequence = 0;                 // SceneCapture::NextSequence before the read: the newer of two wins
			RE::BSShaderMaterial* reference = nullptr;  // one count, the holder's
			// A reference on each of the snapshot's views, taken with the capture (where the material, and so its textures, is alive):
			// a texture its writer swaps out may be freed before the scene work asks for its binding (GpuTextures::Request takes its own
			// from these), so the views are held while the snapshot is.
			std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kMaxTextureFields> views;
		};
		/** @brief The captures for the scene work (Coordinator::DrainMaterialCaptures, its one consumer). */
		// No wake of its own: a requested capture's answer wakes the pass (ServeMaterialRequests), a writer's (controllers write
		// every frame) waits for the frame's pass, an attach's comes with the attach's event.
		inline EventQueue<std::unique_ptr<HeldSnapshot>> captures;
		/**
		 * @brief Any thread where a_material is alive and readable (a material writer after its write: MaterialSources::NoteWritten;
		 * an attach's capture of a leaf; the render thread for a request): its snapshot, with a reference, onto `captures`.
		 */
		void PushCapture(const RE::BSShaderMaterial* a_material);

		/** @brief What separates two captures of one material (SnapshotChangeOf). */
		enum class SnapshotChange : std::uint8_t
		{
			None,        // the same fields and views (the reference count aside)
			Transforms,  // only the two texture-transform buffers (BSShaderMaterial +0xc..+0x2c, TextureTransformsOf's fields)
			Fields,      // anything else
		};
		/**
		 * @brief Pure: how a_new differs from a_held, two captures of the same material. The reference count (+0x8) is left out: every
		 * capture moves it. A record's only input from the transform buffers is TexcoordOffset, the transform watch's.
		 */
		SnapshotChange SnapshotChangeOf(const MaterialSnapshot& a_held, const MaterialSnapshot& a_new);

		/** @brief Render thread, the frame's start: the frame's sources. */
		MaterialFrame SampleFrame();

		/**
		 * @brief Pure: what SetupMaterial (with CS's hooks) writes for a_snapshot under a_passDescriptor (the technique and flags the
		 * shader's +0x94 would hold), in MaterialRecord's terms (constants in StageLayout order with kUnwrittenBits elsewhere, the
		 * pixel textures, address and filter modes, textureWritten, featureTextures). False: a material the port does not cover.
		 */
		bool Evaluate(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialFrame& a_frame, MaterialRecord& a_out);

		// The parts Evaluate composes (each file owns its own).
		/** @brief Vanilla BSLightingShader::SetupMaterial (1414dc310) for the vanilla material classes (MaterialPortVanilla.cpp). */
		bool EvaluateVanilla(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const VanillaFrame& a_frame, MaterialRecord& a_out);
		/** @brief TruePBR::BSLightingShader_SetupMaterial for CS's PBR materials (MaterialPortFeature.cpp). */
		bool EvaluatePBR(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialFrame& a_frame, MaterialRecord& a_out);
		/** @brief Advanced Skin's and TerrainHelper's hooks: featureTextures' Advanced Skin entries and anything else they bind (MaterialPortFeature.cpp). */
		void EvaluateFeatureHooks(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const FeatureFrame& a_frame, MaterialRecord& a_out);
		/** @brief The vanilla classes' texture fields (offsets; a_count is set, not added to), for Capture (MaterialPortVanilla.cpp). */
		void VanillaTextureFields(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count);
		/** @brief A vanilla class's size (its Create's allocation), 0 for any other class (MaterialPortVanilla.cpp). */
		std::uint32_t VanillaClassSize(const RE::BSShaderMaterial& a_material);
		/** @brief The PBR class's texture fields (MaterialPortFeature.cpp). */
		void PBRTextureFields(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count);
		/** @brief BSEffectShaderMaterial's size (its Create's allocation, CommonLib's sizeof). */
		inline constexpr std::uint32_t kEffectMaterialBytes = 0x88;
		/** @brief Whether a material is a BSEffectShaderMaterial (its vtable; MaterialPortEffect.cpp). */
		bool EffectMaterial(const RE::BSShaderMaterial& a_material);
		/** @brief BSEffectShaderMaterial's texture fields: the source (+0x58) and the greyscale palette (+0x60). */
		void EffectTextureFields(std::uint16_t* a_out, std::uint32_t& a_count);
		/**
		 * @brief BSEffectShader::SetupMaterial (0x1414ea6c0, the non-membrane half) and SetupTechnique's filter modes for
		 * an effect material's snapshot, into a record of kind kMaterialKindEffect (EffectVSLayout / EffectPSLayout order). LightingInfluence.y
		 * holds softFalloffDepth itself (the pixel stage divides the frame's near * far by it). False: a membrane pass, a greyscale pass without
		 * its palette, or a snapshot that is not an effect material's (MaterialPortEffect.cpp).
		 */
		bool EvaluateEffect(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const VanillaFrame& a_frame, MaterialRecord& a_out);
		/** @brief The view SetupMaterial binds for a texture object (NiSourceTexture -> renderer texture -> view), or null. */
		ID3D11ShaderResourceView* ViewOf(const void* a_texture);
	}
}
