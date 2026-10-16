#pragma once

// FrameGlobals: the engine globals the scene reads, captured once by the render thread at the frame's start and immutable after;
// the scene-work thread and its pool read the capture their frame carries (FrameGlobals::Scope), the render thread its latest.

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPortFrame.h"

namespace RE
{
	class NiAVObject;
}

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/**
	 * @brief What SetupTechnique reads of the engine: sampled by the render thread at the frame's start (SampleTechniqueInputs,
	 * ConstantEvaluator.cpp), so EvaluateTechnique is a function of the pass descriptor and the frame's sample alone.
	 */
	struct TechniqueInputs
	{
		std::array<float, 4> highDetailRange{};  // LodHighDetailRange's value (0x2033094, less the margin 0x1ad28d0 in z and w)
		ID3D11ShaderResourceView* shadowMask = nullptr;  // kSHADOW_MASK's view
		std::uint32_t shadowMaskFilter = 0;              // t14's filter mode (iShadowMaskQuarter)
		bool shadowMaskSized = false;                    // its texture exists: shadowMaskInverseSize holds 1/width, 1/height
		std::array<float, 2> shadowMaskInverseSize{};
		bool fog = false;  // the scene graph's fog property (FUN_1414dfad0)
		float fogNear = 0.0f, fogFar = 0.0f, fogPower = 0.0f, fogClamp = 0.0f;
		std::array<float, 3> fogNearColor{}, fogFarColor{};
		float invFrameBufferRange = 0.0f;
		std::array<float, 3> colourClamp{ 1.0f, 1.0f, 1.0f };  // fLightingOutputColourClampPost{Lit,Env,Spec}
	};

	/**
	 * @brief One node of a root view's subtree (the first person's; render thread): its key (an identity, dereferenced only right after a
	 * walk of the live tree found it), whether it is a geometry, its hidden bit now (the hidden events applied) and, saved when an event
	 * changed it after the engine's last culls of the view, its bit at those culls. Diagnostics::RootSubtreeSynced hands them out.
	 */
	struct RootSubtreeNode
	{
		const RE::NiAVObject* key = nullptr;
		bool geometry = false, hidden = false, culled = false, saved = false;
	};

	/**
	 * @brief The engine's frame globals the scene reads, captured by the render thread at the frame's start
	 * (FrameDriver::BeginFrame: the engine's update done, before the culls), immutable after.
	 *
	 * The scene work runs on DCLF's coordinator and reads no engine memory: what it needs of engine globals - settings, render
	 * state bytes, the fade update's constants, the features' globals, the frame's cull-hidden roots - it reads from the capture its frame
	 * carries (Scope). A helper shared with the render thread reads the same capture, the render thread's latest. Any other thread
	 * that reads it without a scope is a defect: counted and logged once (it gets the render thread's latest).
	 */
	struct FrameGlobals
	{
		// The scene's prologue.
		bool loading = false;   // a loading screen is up (Coordinator::IsLoadingScreenUp)
		bool interior = false;  // Util::IsInterior
		std::array<std::uint32_t, 4> decalBias{};  // DecalDepthBiasMode per decal group (0 unused)
		// [LightingShader] LOD fade thresholds (GetRenderPasses' specular and envmap LOD fades).
		float specularStart = 0.09f, specularEnd = 0.10f, envmapStart = 0.09f, envmapEnd = 0.10f;
		float refractionStart = 0.025f, refractionEnd = 0.03f;  // the refraction LOD fade (fRefractionLODFade*)
		// SetupTechniqueDescriptor's bytes (0x2032fdb, 0x2035500).
		std::uint8_t techniqueByte12 = 0, techniqueByte7 = 0;
		// GetRenderPasses_ShadowMapOrMask's global (0x2033498): 1 no caster without kCastShadows, 2 the volumetric copy only.
		std::uint8_t shadowGlobal = 0;
		// StaticShadowBits: the main accumulator's deferred-shadow byte (+0x178; whether it exists), no ShadowDir, screen-door fades.
		bool accumulator = false;
		std::uint8_t accumulatorDeferredShadow = 0, noSunShadowDir = 0, screenDoorFades = 0;
		// The fade update's constants (BSFadeNode::OnVisible, FUN_14147a430, FadeDistanceOf, SampleLodFadeFrame).
		std::uint8_t fadesOn = 0, fadeLodUpdates = 0;
		float fadeSpecialA = 0.0f, fadeSpecialB = 0.0f;  // 0x332a254 (the metric override), 0x1769578
		float fadeDistanceMult = 0.0f, fadeOutThreshold = 0.0f, fadeDefaultScale = 0.0f, metricScale = 0.0f;
		std::array<float, 16> fadeTypeDivisors{};
		float lodRadiusBase = 0.0f, lodExponentScale = 0.0f, lodPowBase = 0.0f;  // FUN_14147a430's scale: 0x2032e40, 0x2032e54, 0x2032e44
		// The main camera (SampleLodFadeFrame).
		std::array<float, 3> eye{};
		float lodAdjust = 0.0f;
		// The features' frame-globals contributions (Api::FrameGlobalsContribution, each from its slot: Features/TreeWind's wind globals,
		// which its shading derivation reads).
		std::array<float, Extensions::kFrameGlobalsFloats> features{};
		// The roots whose hidden bit the culls change around the scene's views (CaptureCullHiddenBits), as each view sees it; sorted.
		std::vector<std::pair<const RE::NiAVObject*, bool>> cullHidden;
		// SceneCapture::NextSequence just before cullHidden was read: a hidden store's event numbered after it may be newer than the bit
		// the capture holds for its node (Coordinator::DrainHiddenEvents announces it again at a newer capture).
		std::uint64_t cullHiddenSequence = 0;
		// The geometry keys under the first-person root as the frame's start walked them (sorted; identities): the root view's hidden
		// verdict's diagnostic (ClassifyFrame's Diagnostics::RootViewHidden).
		std::vector<const RE::NiAVObject*> firstPersonGeometry;
		// PrimaryCull::MembershipWitness of the values above.
		std::uint32_t membershipWitness = 0;
		// SetupTechnique's inputs.
		TechniqueInputs technique;
		// A reference on technique.shadowMask, taken with the sample: the technique rows name it, and the scene work asks for its binding
		// (Coordinator::UpdateSharedBindings) while its frame's capture is held, so the view outlives the request.
		winrt::com_ptr<ID3D11ShaderResourceView> shadowMaskHeld;
		// SetupMaterial's per-frame sources (the scene work's material records).
		MaterialPort::MaterialFrame material;

		/** @brief Render thread, the frame's start: the frame's capture, which Current() hands the render thread from now on. */
		static std::shared_ptr<const FrameGlobals> Capture();
		/**
		 * @brief Render thread, at RenderDepth's first-person draw (after the engine's first-person culls): the hidden bit of every
		 * node under a_root as those culls saw them. The next frame's capture lists them (cullHidden) in place of the frame start's bits.
		 */
		static void NoteFirstPersonCulled(const RE::NiAVObject* a_root);
		/** @brief The capture this thread reads: its Scope's, else (the render thread) the latest. */
		static const FrameGlobals& Current();
		/** @brief Since the last call: reads from a thread with neither a scope nor the render thread's identity. */
		static std::uint64_t TakeUnscopedReads();

		/** @brief Binds a capture to this thread (the scene work's task, for its frame). */
		class Scope
		{
		public:
			explicit Scope(std::shared_ptr<const FrameGlobals> a_globals);
			~Scope();
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

		private:
			std::shared_ptr<const FrameGlobals> held;
			const FrameGlobals* previous = nullptr;
		};
	};
}
