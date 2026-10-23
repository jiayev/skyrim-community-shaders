#pragma once

#include "Feature.h"

/**
 * @brief Drawcall Limit Fix: draws the scene the engine would draw per object with GPU-driven indirect draws, executed by
 * the render graph on DXVK's Vulkan device (docs/development/dclf-architecture.md).
 *
 * This file is the feature's shell: install, the engine hook thunks, the render-thread lifecycle points (each forwards to
 * Draws/FrameDriver under a RenderThreadBudget hook), the menu toggle and the settings UI. Render thread unless noted.
 *
 * It is DCLF's one integration point with the rest of CS, and the one place its dependencies are decided: DCLF needs the
 * render graph and ORGModuleServices (CS_HAS_DCLF, set by cmake/RenderGraph.cmake). Without them the rest of
 * Features/DrawcallLimitFix/ is not built, and every entry point here is inert. Other CS code calls DCLF only through this header.
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

	/** @brief SKSE kPostLoad (CS's message handler): SceneWriterFramework's provider election, when PostPostLoad will install DCLF. */
	void PostLoad();
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;
	virtual bool AppliesBootToggleLive() const override { return true; }
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void EarlyPrepass() override;
	virtual void DrawSettings() override;
	/**
	 * @brief CS's "Clear shader cache" (the menu's, and the shader watcher's full clear; any thread): every live render graph pipeline built
	 * again from the shader files as they are now (RenderGraphRuntime::RequestShaderReload; docs/development/dclf-hot-reload.md).
	 */
	virtual void ClearShaderCache() override;

	/** @brief Where the main pass's opaque batches start: what it binds, for the colour epoch (IndirectDraws::CaptureMainPass). */
	void BeforeOpaquePass();
	/**
	 * @brief The end of the main pass's opaque batches (Deferred's Main_RenderWorld_BlendedDecals hook): DCLF's
	 * colour epoch. What the engine draws after this point blends onto the G-buffer or tests against the opaque
	 * depth, so the objects DCLF drew in place of native ones have to be in it: Terrain Blending's deferred
	 * terrain, then the blended decals.
	 */
	void AfterOpaquePass();
	/**
	 * @brief CS_DCLF_TARGET_PROBE=x,y (dclf-validation.md, "The target probe"): every bound G-buffer target, averaged over a 64 x 64 block
	 * at the pixel, where the opaque pass ends, DCLF on or off.
	 */
	void ProbeOpaqueTarget(std::uint32_t a_stage);

	/**
	 * @brief Main_RenderShadowMaps, before the engine draws the shadow maps: the scene graph and the main
	 * camera's culling are final, the main accumulator's passes are not yet (their registration jobs run
	 * concurrently with the shadow draws). The shadow views are drawn between this and EarlyPrepass.
	 */
	void BeforeShadowMaps();
	/**
	 * @brief Main_RenderShadowMaps, after the call returned (before EarlyPrepasses): the shadow epoch, with the occlusion maps
	 * OcclusionReady took before the call.
	 */
	void AfterShadowMaps();

	/** @brief Called after the Lighting shader's SetupGeometry for every native lighting draw. */
	void OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	/**
	 * @brief The occlusion maps, drawn by DCLF in its shadow epoch (docs/development/dclf-architecture.md, "The occlusion maps, drawn by
	 * DCLF"): Skylighting's sky map, and the precipitation mask it renders first. Render thread. OcclusionReady, at the shadow maps
	 * (Skylighting::RenderOcclusionAtShadowMaps, before the engine draws them): whether DCLF draws the map this frame, in the shadow epoch
	 * at the shadow maps' end (AfterShadowMaps); Skylighting then sets its view up there, without the engine's SetupMask, and RenderMask
	 * clears it. OcclusionNeedsEngine, at the engine's occlusion point (Skylighting::RenderOcclusion): whether the engine renders the map
	 * there, whole.
	 */
	enum OcclusionMap : std::uint32_t
	{
		kSkyOcclusion = 0,            // DCLF::Api::OcclusionMap::Sky
		kPrecipitationOcclusion = 1,  // DCLF::Api::OcclusionMap::Precipitation
	};
	/**
	 * @brief At the shadow maps, before the engine draws them: whether DCLF draws the map in this frame's shadow epoch (the selected
	 * revision's shadow shape has its view, and the frame's set draws its phase). An occluder not yet a member of the map's phase
	 * (joining: FrameView::SetLacking) is left out of the map until it joins, never handed to the engine's cull.
	 */
	bool OcclusionReady(OcclusionMap a_map);
	/**
	 * @brief At the engine's occlusion point: whether the engine must render the map there, culling and registering its scene
	 * (Precipitation::SetupMask): DCLF's shadow epoch did not draw it this frame (not taken at the shadow maps, or the epoch could not
	 * draw it); the scene lists the frame's filter left out are put back first.
	 */
	bool OcclusionNeedsEngine(OcclusionMap a_map);
	/**
	 * @brief Once the map's camera is set up (Precipitation's occlusion camera, after its projection: SetupMask or the
	 * projection alone), before its RenderMask, on every frame the map renders: DCLF's view of it, computed from that camera.
	 */
	void OcclusionView(OcclusionMap a_map);
	/**
	 * @brief After OcclusionReady (DCLF draws the map): whether the engine renders the map first this frame, whole, as a reference
	 * (the map's referenceDue: Features/OcclusionMaps' CS_DCLF_SKYLIGHT_PARITY); its render is announced with OcclusionReferenceRendered
	 * before the map is cleared for DCLF's.
	 */
	bool OcclusionReferenceFrame(OcclusionMap a_map);
	void OcclusionReferenceRendered(OcclusionMap a_map);

	// ---- Calls from the rest of CS (each inert in a build without DCLF) ----

	/** @brief Any thread, lock-free: a material's fields were written in place (CS's PBR materials, which bypass the hooked writers). */
	static void NoteMaterialWritten(const RE::BSShaderMaterial* a_material);
	/** @brief NoteMaterialWritten when the scope ends: for an in-place rewrite with early returns. */
	struct MaterialWriteScope
	{
		const RE::BSShaderMaterial* material;
		~MaterialWriteScope() { NoteMaterialWritten(material); }
	};
	/** @brief ShaderCache's precompile of a Lighting/Effect/DistantTree permutation: DCLF's SPIR-V build of the same one. */
	static void PrecompileShader(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);
	/** @brief The D3D11 context's Map/Unmap/UpdateSubresource (Globals' hooks): DCLF's constant buffer mirror and capture parity. */
	static void OnMap(ID3D11Resource* a_resource, void* a_data);
	static void OnUnmap(ID3D11Resource* a_resource);
	static void OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data);
	/** @brief CS_DCLF_TARGET_DUMP's points from other features (Upscaling): see DCLF::TargetDump. */
	static bool TargetDumpArmed();
	static void TargetDumpPoint(const char* a_label, ID3D11Texture2D* a_extra = nullptr);
	/** @brief Every render graph submission up to a_submissions completed on the GPU (PersistentGraphHost's retired callback): DCLF's retirement clock. */
	static void OnGraphSubmissionsRetired(std::uint64_t a_submissions);

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
		 * draws: where the Z-prepass runs (IndirectDraws::CaptureDepthPass).
		 *
		 * It is the last point where the world camera is current. In first person the depth pass then draws the
		 * first-person model with the first-person camera, whose eye is the player's head (posAdjust) and whose
		 * VS_PerFrame is its own, and leaves both set: Main::Draw restores the world camera only after the depth
		 * pass returns, so a Z-prepass at the end of the depth pass would draw the world with the first-person
		 * camera (skyrim-engine-notes.md, "The depth pass and first person").
		 */
		struct Main_RenderDepth_WorldDrawn
		{
			static void thunk(void* a_accumulator, bool a_a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Main::Draw's first call (AE +0xD3, after its NiUpdateData update and before the main camera's
		 * cull jobs are queued): where the frame's scene phase starts (BeginSceneFrame), ~1 ms ahead of
		 * Main_RenderShadowMaps.
		 */
		struct Main_Draw_Early
		{
			static std::int64_t thunk(void* a_main);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Main::Update's first call (AE 0x140645ea0 +0x3E, FUN_140cfd280: the thread stages' events reset), on the main thread
		 * before the frame's job lists start (0x14064613f) and after the last frame's cycle was waited for (0x14064694d): the stood-in
		 * roots' kAccumulated writes and the fade write-back stores (PrimaryCull::AtUpdateEntry), the one point every reader sees in the
		 * same frame (skyrim-engine-notes.md, "who sets kAccumulated").
		 */
		struct Main_Update_Entry
		{
			static void thunk(void* a_stages);
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

	// The frame's start, from Main::Draw's early hook (FrameDriver::BeginFrame). Returns Running().
	bool BeginSceneFrame();
	// Off when the menu's toggle is off or the feature is unloaded (Feature::loaded, which the remote toggle
	// flips): either way nothing may keep drawing, skipping or withholding.
	void UpdateActive();
	void SetActive(bool a_active);
	// Why DCLF is off for the session (not AE, or the render graph could not come up); shown in the menu.
	std::string unavailableReason;
};
