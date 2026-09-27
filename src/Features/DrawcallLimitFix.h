#pragma once

#include "Feature.h"

/**
 * @brief Drawcall Limit Fix: replaces the native opaque render loop with GPU-driven indirect draws
 * executed by the render graph on DXVK's Vulkan device.
 *
 * Phase 2 (this state): static rigid geometry under the Static, Dynamic and MultiBound nodes of attached
 * cells is tracked and turned into tables each frame (Phase 1), and drawn by the render graph with indirect
 * commands into off-screen copies of the main pass's targets (IndirectDraws); the frame itself still comes
 * from the native draws. See docs/development/drawcall-limit-fix.md.
 */
struct DrawcallLimitFix : Feature
{
	virtual inline std::string GetName() override { return "Drawcall Limit Fix"; }
	virtual inline std::string GetShortName() override { return "DrawcallLimitFix"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual bool IsInMenu() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Replaces the game's per-object opaque draw loop with GPU-driven indirect draws on the Vulkan render graph.",
			{ "Tracks static scene geometry directly from the scene graph",
				"Builds per-object draw tables for indirect execution" } };
	}

	virtual void PostPostLoad() override;
	virtual void SetupResources() override;
	virtual bool AppliesBootToggleLive() const override { return true; }
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void EarlyPrepass() override;
	virtual void DrawSettings() override;

	/**
	 * @brief The end of the main pass's opaque batches (Deferred's Main_RenderWorld_BlendedDecals hook): DCLF's
	 * colour epoch. What the engine draws after this point blends onto the G-buffer or tests against the opaque
	 * depth, so the objects DCLF drew in place of native ones have to be in it: Terrain Blending's deferred
	 * terrain, then the blended decals.
	 */
	/** @brief Where the main pass's opaque batches start: what it binds, for the colour epoch (IndirectDraws::CaptureMainPass). */
	void BeforeOpaquePass();
	void AfterOpaquePass();
	/** @brief [TEMP] CS_DCLF_TARGET_PROBE=x,y: render target 0 at a pixel where the opaque pass ends, DCLF on or off. */
	void ProbeOpaqueTarget(bool a_afterDCLF);

	/** @brief Deferred::EndDeferred, before the deferred composite (Phase 2 debug view). */
	void BeforeDeferredComposite();

	/**
	 * @brief Main_RenderShadowMaps, before the engine draws the shadow maps: the scene graph and the main
	 * camera's culling are final, the main accumulator's passes are not yet (their registration jobs run
	 * concurrently with the shadow draws). The shadow views are drawn between this and EarlyPrepass. The
	 * scene phase starts here only when Main::Draw's early hook did not start it (BeginSceneFrame).
	 */
	void BeforeShadowMaps();
	/** @brief Main_RenderShadowMaps, after the call returned (before EarlyPrepasses). */
	void AfterShadowMaps();

	/** @brief Called after the Lighting shader's SetupGeometry for every native lighting draw. */
	void OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	/**
	 * @brief Hybrid path (CS_DCLF_HYBRID=1): whether the native loop leaves this pass to DCLF.
	 *
	 * True only inside the main camera's depth and opaque ranges, and only for geometry the indirect draws
	 * drew in the frame before. Shadow, reflection and cubemap passes go through the same batch renderer and
	 * are never skipped.
	 */
	bool SkipNativePass(RE::BSRenderPass* a_pass);
	// Whether DCLF can draw the geometry this frame: a record with bindings and a built pipeline. Valid from
	// EarlyPrepass (the accumulate phase and the pipeline lookups) to the end of the frame.
	static bool DrawableThisFrame(const RE::BSGeometry* a_geometry);

	/**
	 * @brief After the hybrid Z-prepass: rebuild what was derived from the depth buffer before it ran.
	 *
	 * The engine's prepass depth copy and Terrain Blending's blended depth are both built at the end of the
	 * native depth pass, so they hold a scene without DCLF's objects, and every effect that reads depth
	 * afterwards paints the background over them.
	 */
	void RefreshDepthConsumers();

	/** @brief How many native passes were skipped in the last frame, and how many were offered. */
	struct SkipStats
	{
		std::uint32_t skipped = 0;
		std::uint32_t offered = 0;
		std::uint32_t skippedInDepth = 0;
		std::uint32_t skippedInOpaque = 0;
		std::uint32_t notInTables = 0;
		std::uint32_t undrawable = 0;  // drawn last frame, in the tables, but not drawable this frame: kept native
		std::uint32_t kept = 0;
	};
	const SkipStats& GetSkipStats() const { return skipStats; }

	/**
	 * @brief Skylighting's occlusion map, drawn by DCLF (Skylighting::RenderOcclusion's variant while DCLF runs;
	 * docs/development/drawcall-limit-fix.md, "Skylighting's occlusion map, drawn by DCLF"). Render thread.
	 * SkyOcclusionReady: DCLF draws this frame's map, so the engine's SetupMask is skipped. DrawSkyOcclusion: after
	 * RenderMask, which set the view up and cleared the map, DCLF's occluders into it.
	 */
	bool SkyOcclusionReady();
	void DrawSkyOcclusion();
	/**
	 * @brief CS_DCLF_SKYLIGHT_PARITY=1: every 120th map is rendered both ways, the engine's first; CopySkyOcclusion(0)
	 * keeps the engine's, CopySkyOcclusion(1) DCLF's, and the two are compared texel by texel a few frames later.
	 */
	bool SkyOcclusionParityFrame();
	void CopySkyOcclusion(std::uint32_t a_stage);

private:
	struct Hooks
	{
		/** @brief The main camera's depth pass: DCLF's objects are skipped inside it too. */
		struct Main_RenderDepth
		{
			static void thunk(bool a_firstPerson, bool a_a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Inside the depth pass (AE `Main::RenderDepth` +0x1AA), the call that follows the world's depth
		 * draws: where the Z-prepass runs (RunZPrepass).
		 *
		 * It is the last point where the world camera is current. In first person the depth pass then draws the
		 * first-person model with the first-person camera, whose eye is the player's head (posAdjust) and whose
		 * VS_PerFrame is its own, and leaves both set: Main::Draw restores the world camera only after the depth
		 * pass returns. A Z-prepass at the end of the depth pass drew the whole world with that camera, and the
		 * colour epoch, which replays the prepass's vertex inputs, drew nothing either (engine notes: the depth
		 * pass and first person).
		 */
		struct Main_RenderDepth_WorldDrawn
		{
			static void thunk(void* a_accumulator, bool a_a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Main::Draw's first call (AE +0xD3, after its NiUpdateData update and before the main camera's
		 * cull jobs are queued): where the frame's scene phase starts (BeginSceneFrame), ~1 ms ahead of
		 * Main_RenderShadowMaps. AE only: the SE and VR offsets are unverified, and there the phase starts at
		 * BeforeShadowMaps as before.
		 */
		struct Main_Draw_Early
		{
			static std::int64_t thunk(void* a_main);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSShaderAccumulator::FinishAccumulatingPreResolveDepth (vfunc 0x2A): a shadow view's draw.
		 * After the native draws of the view, DCLF's epoch culls the frame's casters and draws them into the
		 * same slice (CS_DCLF_SHADOWS=1).
		 */
		struct BSShaderAccumulator_FinishAccumulating
		{
			static void thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The call sites of BSBatchRenderer::RenderPassImmediately, as Light Limit Fix hooks them. */
		template <int N>
		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};

	bool installed = false;
	// The menu's on/off toggle, applied at the next Present. Off keeps the hooks and the scene tracking (so
	// switching back on needs no rescan) but does no frame work: nothing is built, drawn, skipped or withheld.
	bool switchedOn = true;
	bool Running() const { return installed && switchedOn; }

	// The frame's scene phase (toggles, then SceneStore's scene half, whose walk goes to the worker), from
	// Main::Draw's early hook or else from BeforeShadowMaps. Returns Running().
	bool BeginSceneFrame();
	bool sceneFrameBegun = false;  // BeginSceneFrame ran for the frame BeforeShadowMaps is about to continue
	// Off when the menu's toggle is off or the feature is unloaded (Feature::loaded, which the remote toggle
	// flips): either way nothing may keep drawing, skipping or withholding.
	void UpdateActive();
	void SetActive(bool a_active);
	// Why DCLF was forced off after install (the render graph could not come up); shown in the menu.
	std::string unavailableReason;
	bool inDepthPass = false;
	// The Z-prepass ran inside this depth pass (Main_RenderDepth_WorldDrawn); the end of the pass then skips it.
	bool zPrepassInDepthPass = false;
	/** @brief The Z-prepass epoch, then what was derived from the depth before it (RefreshDepthConsumers). */
	void RunZPrepass(bool a_refreshConsumers);
	std::uint32_t captureFrame = ~0u;  // the frame whose main-pass bindings have been captured
	std::uint32_t parityFrame = ~0u;   // the frame CS_DCLF_CAPTURE_POINT_PARITY last checked
	bool loggedCaptureFailure = false;
	/** @brief The main pass's bindings and target formats for this frame's colour epoch, when its targets are bound. */
	bool CaptureMainPass();
	SkipStats skipStats;
	SkipStats skipCounters;  // accumulating; published into skipStats every frame
	std::vector<std::string> skipSamples;  // names of a few skipped passes, for the report
};
