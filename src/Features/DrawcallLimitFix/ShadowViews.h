#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
	class BSShaderProperty;
	class BSShadowLight;
	class BSShaderAccumulator;
}

namespace DCLF
{
	/**
	 * @brief Why the engine would draw no shadow-map pass for an object (engine notes: shadow maps,
	 * `BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask`). None means it is a caster.
	 *
	 * Measured against the engine with `CS_DCLF_SHADOW_PROBE=1`: over ~1M registrations it rejected none
	 * of the casters the engine registered.
	 */
	enum class ShadowReject : std::uint8_t
	{
		None,
		NotLighting,    // not a BSLightingShaderProperty; the rule is that property's
		DeclZero,       // DetermineUtilityShaderDecl() == 0
		Faded,          // fadeNode->currentFade * material->materialAlpha < 1
		Refraction,     // flags kTempRefraction (2) or kRefraction (15)
		AlphaBlended,   // alpha blending, outside the decal exception
		/**
		 * A decal flag (kDecal 26 or kDynamicDecal 27) outside the one exception: kZBufferWrite (32), bit 18 and
		 * alpha blending. The sun's and the spot lights' accumulators (their UpdateCamera: drawDecals +0x12C = 0,
		 * +0x12D = 1) are registered through FUN_1414b2a60 (AE), which drops such a property before it asks for a
		 * pass; GetRenderPasses_ShadowMapOrMask then drops the bit-18 ones again. A paraboloid light keeps the
		 * constructor's drawDecals = 1 and casts the ones without bit 18: not a DCLF caster, so left unclaimed the
		 * engine draws it there and nowhere else.
		 */
		DecalNoZWrite,
		NoCastShadows,  // kCastShadows clear while the engine's shadow global demands it
		/**
		 * kCastShadows clear while the shadow global is 2 (volumetric lighting): a clamped view whose accumulator
		 * has the volumetric flag (+0x12E, which BSShadowDirectionalLight::Accumulate always sets) gives it a pass
		 * only in the property's volumetricShadowUtilityPasses, drawn into the volumetric lighting copy and never
		 * into the sun's cascades (batch group 15, accumulation hint 8). DCLF draws it in the copy's views alone
		 * (kObjectVolumetricOnly) where PassCapture can withhold that pass (VolumetricClaimsAvailable: AE), and
		 * leaves it to the engine otherwise.
		 */
		VolumetricOnly,
		Count
	};
	const char* ShadowRejectName(ShadowReject a_reason);

	/** @brief The engine's verdict on whether an object casts into a shadow map. */
	ShadowReject ShadowCasterReject(const RE::BSShaderProperty* a_property, const RE::BSGeometry* a_geometry);

	/**
	 * @brief The Utility technique the engine derives for a caster, without the view's mode bits.
	 *
	 * `DetermineUtilityShaderDecl()` with the alpha-test bit and the property-flag bits; a view adds its
	 * own mode bits (`ShadowModeBits`), and the pass the engine registers is this plus 0x2B.
	 */
	std::uint32_t ShadowUtilityTechnique(const RE::BSShaderProperty* a_property, const RE::BSGeometry* a_geometry);

	/** @brief The technique bits one accumulator render mode contributes (0xC RenderDepth .. 0xF Pb). */
	std::uint32_t ShadowModeBits(std::uint32_t a_renderMode);

	/**
	 * @brief The shadow views of the frame, as the engine is about to draw them.
	 *
	 * One view per `ShadowmapDescriptor`: a cascade of the sun, a spot light's frustum, one hemisphere of
	 * a paraboloid light, or one of the four focus shadows (engine notes: shadow maps). The list is
	 * rebuilt at `BeforeShadowMaps` from the shadow scene node's caster array, which is the same order
	 * and the same set the engine's own `Render` loop walks a moment later.
	 *
	 * A view is identified downstream by its accumulator and by its batch renderer: the accumulator is
	 * what `FinishAccumulatingPreResolveDepth` is called on when the view is drawn, and the batch renderer
	 * is what the engine registers the view's Utility passes with - which is how a captured registration
	 * is attributed to a view, exactly as `mainBatchRenderers` attributes the main camera's.
	 *
	 * Render thread only, and valid only between BeforeShadowMaps and the end of the frame's shadow draws.
	 */
	class ShadowViews
	{
	public:
		enum class Kind : std::uint8_t
		{
			Directional,  // the sun's cascades
			Frustum,      // spot lights
			Parabolic,    // point lights: two hemispheres in one slice
			Other,
			Count
		};
		static constexpr std::uint32_t kKindCount = static_cast<std::uint32_t>(Kind::Count);
		static const char* KindName(Kind a_kind);

		/** @brief A rectangle of an array slice, as NiRect holds it (left, right, top, bottom; y up). */
		struct Port
		{
			std::int32_t left = 0, right = 0, top = 0, bottom = 0;
		};

		struct View
		{
			const RE::BSShadowLight* light = nullptr;
			std::uint32_t lightIndex = 0;
			Kind kind = Kind::Other;
			std::uint32_t descriptor = 0;  // index in the light's descriptor array
			bool focus = false;
			const RE::BSShaderAccumulator* accumulator = nullptr;
			const RE::BSBatchRenderer* batch = nullptr;
			std::uint32_t renderTarget = 0;  // RENDER_TARGET_DEPTHSTENCIL, or ~0u until the draw assigns one
			std::uint32_t slice = 0;
			Port port;
			bool clear = false;
			bool enabled = false;
			/**
			 * @brief The accumulator's render mode as it stands at Rebuild - the mode the view was drawn
			 * with LAST frame (0xD plain, 0xE clamped, 0xF paraboloid), which is stable for a descriptor;
			 * 0 before its first draw. It is what attributes a registration to a mode's claim set.
			 */
			std::uint32_t renderMode = 0;
		};

		static ShadowViews& Get();

		/** @brief Rebuilds the list from the shadow scene node; call at BeforeShadowMaps. */
		void Rebuild();
		/** @brief Drops the list (the frame's shadow draws are over, or the feature is off). */
		void Clear();

		std::span<const View> All() const { return views; }
		bool Valid() const { return valid; }

		/** @brief The view a batch renderer belongs to, or ~0u. Includes the geometry groups' renderers. */
		std::uint32_t ViewOfBatch(const RE::BSBatchRenderer* a_batch) const;
		/** @brief The view an accumulator belongs to, or ~0u. */
		std::uint32_t ViewOfAccumulator(const void* a_accumulator) const;

		const View* At(std::uint32_t a_id) const { return a_id < views.size() ? &views[a_id] : nullptr; }

	private:
		std::vector<View> views;
		ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint32_t> batchToView;
		ankerl::unordered_dense::map<const void*, std::uint32_t> accumulatorToView;
		bool valid = false;
	};
}
