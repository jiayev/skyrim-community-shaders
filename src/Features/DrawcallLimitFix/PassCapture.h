#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "LightingDescriptors.h"

namespace DCLF
{
	/**
	 * @brief Captures each lighting pass as the engine registers it with a batch renderer.
	 *
	 * DCLF reads the passes the main camera will draw out of the accumulator's batch renderer
	 * (`SceneStore::CollectAccumulatedPasses`). That works only while the passes are in there, so it
	 * cannot survive static ownership, whose whole point is to keep them out. Capturing at registration
	 * gives the same information one step earlier, from the call that puts a pass into a group.
	 *
	 * `BSBatchRenderer::RegisterPass` (vfunc 0x02) is the right place rather than
	 * `BSLightingShaderProperty::GetRenderPasses`: the two fields DCLF depends on do not exist yet when
	 * `GetRenderPasses` returns. `technique` is the batch group's key, which the caller computes and
	 * passes in as `techniqueID` (it differs from the pass's own `passEnum` - the accumulator adds
	 * DoAlphaTest), and `subPass` is chosen inside registration.
	 *
	 * **Registration is threaded.** The engine's own classifier takes a mutex, and the scene lists are
	 * built on the `gJobList_SceneListAccumCulling` job list, so this is called concurrently. Entries go
	 * into a fixed-capacity buffer with an atomic cursor; the render thread drains it once per frame and
	 * is the only reader. Overflow is counted and makes the frame fall back to the accumulator walk.
	 */
	class PassCapture
	{
	public:
		struct Entry
		{
			const RE::BSGeometry* geometry = nullptr;
			const RE::BSRenderPass* pass = nullptr;
			const RE::BSBatchRenderer* batch = nullptr;  // which renderer it was registered with
			std::uint32_t technique = 0;                 // techniqueID, the batch group's key
			std::uint32_t subPass = 0;                   // derived; see SubPassOf
			std::uint32_t passEnum = 0;
			bool fading = false;                         // FadingAtRegistration
			bool withheld = false;                       // kept from the batch renderer (Withhold)
			std::uint8_t fadeState = 0xFF;               // the fade node's LOD state (+0x153) at registration, 0xFF without one
			std::uint8_t hint = 0;                       // accumulationHint
		};

		struct Stats
		{
			std::uint32_t captured = 0;   // entries the last drain saw
			std::uint32_t overflowed = 0;
			std::uint32_t threads = 0;    // distinct threads seen registering
			// Step B gate: the captured set against what the accumulator walk finds.
			std::uint32_t compared = 0;
			std::uint32_t missing = 0;     // in the accumulator, not captured
			std::uint32_t extra = 0;       // captured, not in the accumulator
			std::uint32_t techniqueDiffers = 0;
			std::uint32_t subPassDiffers = 0;
			std::uint32_t withheld = 0;  // passes kept out of the main camera's batch renderer
			// CS_DCLF_SHADOW_OWNERSHIP=static: Utility passes kept out of the shadow views' batch renderers,
			// by the view's render mode (plain 0xD, clamped 0xE, paraboloid 0xF).
			std::array<std::uint32_t, 3> shadowWithheld{};
			std::uint32_t volumetricWithheld = 0;  // volumetric-only passes (hint 8) kept out of batch group 15
			std::uint32_t directWithheld = 0;      // shadow passes of hints 11, 7 and 3, which bypass RegisterPass too
			std::uint32_t claimed = 0;   // objects DCLF said it owns
			// Claimed but not drawn this frame. Withholding means nothing else will draw them either, so
			// any of these is a visible hole - the one failure mode static ownership introduces.
			std::uint32_t holes = 0;
			// Claim churn. A claim that goes away hands the object back to the native loop, so if culling
			// an object unclaims it the engine simply draws it again and the culling saves nothing.
			std::uint32_t claimsAdded = 0;
			std::uint32_t claimsDropped = 0;
			std::uint32_t droppedAfterCull = 0;  // dropped while the engine still had a pass for it
			std::uint32_t handedBack = 0;        // withheld, then returned to the native loop (HandBackUndrawable)
		};

		/** @brief The set of geometries DCLF owns; registration is withheld for these. */
		using ClaimSet = ankerl::unordered_dense::set<const RE::BSGeometry*>;

		static PassCapture& Get();

		void Install();
		bool Installed() const { return installed; }
		/** @brief Makes the hook a plain pass-through while set: DCLF is off (forced, or switched off in the menu). */
		void SetBypassed(bool a_bypassed) { bypassed.store(a_bypassed, std::memory_order_release); }
		bool Bypassed() const { return bypassed.load(std::memory_order_acquire); }

		/**
		 * @brief Publishes the claim set for the frames that follow; render thread only.
		 *
		 * Immutable and swapped whole, never mutated in place: the hook reads it from whatever thread the
		 * engine registers on, and it is consulted before the frame's own BuildFrame has run. That is the
		 * right way round - a claim is meant to be a standing statement that DCLF owns an object, not a
		 * per-frame decision, which is exactly what the old `drawnFrame` skip was and why culling could
		 * not be trusted.
		 */
		void PublishClaims(std::shared_ptr<const ClaimSet> a_claims);

		/** @brief The claim set the registration hook is currently using, for the hole detector. */
		std::shared_ptr<const ClaimSet> CurrentClaims() const { return std::atomic_load(&claims); }

		/** @brief Which batch renderers belong to the main camera; withholding applies only to these. */
		void SetMainBatchRenderers(std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> a_renderers);

		/** @brief CS_DCLF_OWNERSHIP=static: withhold claimed passes from the main camera's batch renderer. */
		static bool WithholdingEnabled();

		/** @brief The shadow views' render modes with claims of their own: 0xD plain, 0xE clamped, 0xF paraboloid. */
		static constexpr std::uint32_t kShadowModes = 3;
		static constexpr std::uint32_t kFirstShadowMode = 0xD;
		/** @brief Which batch renderers belong to shadow views, each with its view's render mode index. */
		using ShadowRendererMap = ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint8_t>;
		void SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers);
		/**
		 * @brief CS_DCLF_SHADOW_OWNERSHIP=static: the casters DCLF's shadow epochs draw under a render mode,
		 * withheld from every shadow view of that mode from the next registration on. Published by the
		 * epoch that submitted them, as the main claims are published by the colour epoch.
		 */
		void PublishShadowClaims(std::uint32_t a_modeIndex, std::shared_ptr<const ClaimSet> a_claims);
		/**
		 * @brief The claims Withhold decides a pass into this batch renderer by: its shadow view's render mode's, or
		 * null when the renderer is not a shadow view's (as far as the last ShadowViews rebuild knows) or shadow
		 * ownership is off. SunAccumulation skips a claimed geometry's registration by the same test.
		 */
		std::shared_ptr<const ClaimSet> ShadowClaimsForBatch(const RE::BSBatchRenderer* a_batch) const;
		std::shared_ptr<const ClaimSet> CurrentShadowClaims(std::uint32_t a_modeIndex) const
		{
			return a_modeIndex < kShadowModes ? std::atomic_load(&shadowClaims[a_modeIndex]) : nullptr;
		}
		/** @brief CS_DCLF_SHADOW_OWNERSHIP=static (live: Toggles.h): withhold claimed casters from the shadow views. */
		static bool ShadowWithholdingEnabled();
		/**
		 * @brief Whether the volumetric lighting copy's passes can be withheld: its registration (accumulation
		 * hint 8) bypasses RegisterPass - the shadow modes' registration (AE FUN_1414b2a60) inserts it into batch
		 * group 15 with a direct call - so it needs a hook of its own on that call, which exists for AE only.
		 * Until it is installed the volumetric-only casters stay the engine's.
		 */
		static bool VolumetricClaimsAvailable();
		/**
		 * @brief How many passes this thread has handed to a batch renderer so far (RegisterPass, and the shadow
		 * modes' direct group insertions), withheld or not: the difference across a registration call is how many
		 * passes it built. SunAccumulation's probe reads it.
		 */
		static std::uint32_t PassesOnThisThread();
		/**
		 * @brief Whether the pass is one DCLF leaves to the native loop because of a fade, as it is registered:
		 * any accumulation hint 10 (the stencil-dithered fade, and a LOD cross-fade's copy of the old level), a
		 * fade the engine draws blended (hint 9), or any fade with CS_DCLF_FADING off. A screen-door fade in an
		 * opaque group is DCLF's, and so is a LOD cross-fade's own pass (the new level). The cull
		 * has just updated the fade (BSFadeNode::OnVisible runs before the node's geometry registers), and this
		 * is the one value both the withholding and the accumulate phase's fading verdict use
		 * (AccumulatedPass::fading).
		 */
		static bool FadingAtRegistration(const RE::BSRenderPass* a_pass);

		/**
		 * @brief EarlyPrepass, render thread: returns every pass withheld this frame whose object DCLF cannot
		 * draw this frame (a_drawable false) to the batch renderer it was kept from, through the original
		 * RegisterPass, before the native depth and main passes draw.
		 *
		 * Withholding is decided at registration from the claims, which are last frame's draws; whether
		 * DCLF can draw the object this frame is only known once the tables and pipeline lookups are built.
		 * Whatever falls between the two - a pipeline variant still compiling, an object that lost its
		 * bindings this frame - would otherwise be drawn by nobody. Returns how many were handed back.
		 */
		std::uint32_t HandBackUndrawable(const std::function<bool(const RE::BSGeometry*)>& a_drawable);
		/** @brief Whether this frame's HandBackUndrawable returned the geometry to the native loop. */
		bool HandedBack(const RE::BSGeometry* a_geometry) const { return handedBack.contains(a_geometry); }
		/**
		 * @brief For the hole reports, render thread: whether a registration of the geometry was withheld from
		 * the main camera this frame and not handed back - the native loop will not draw it.
		 */
		bool WithheldThisFrame(const RE::BSGeometry* a_geometry);

		/** @brief Takes everything registered since the last call; render thread only. */
		std::span<const Entry> Drain();
		/** @brief What the last Drain took (diagnostics); render thread only. */
		std::span<const Entry> LastDrain() const { return lastDrain; }

		/**
		 * @brief CS_DCLF_SHADOW_PROBE: the BSUtilityShader registrations since the last call (the shadow
		 * views' passes, and the main camera's RenderDepth ones), kept in a ring of their own so that the
		 * Lighting capture the tables are built from is untouched; render thread only, after the
		 * registration jobs have finished.
		 */
		std::span<const Entry> DrainUtility();

		const Stats& GetStats() const { return stats; }
		Stats& MutableStats() { return stats; }

		/**
		 * @brief The batch group list a pass lands in, from the property flags alone.
		 *
		 * The engine's classifier reads only the shader property's flags and the geometry's alpha
		 * property: flag bit 54 puts a pass in list 4, otherwise the list is bit 36 (as value 2) plus
		 * whether alpha testing is on. Nothing per-frame goes into it, which is what lets DCLF keep the
		 * field once the passes stop reaching a batch renderer at all.
		 */
		static std::uint32_t SubPassOf(const RE::BSGeometry* a_geometry, std::uint64_t a_propertyFlags);

	private:
		PassCapture();
		static constexpr std::size_t kCapacity = 32768;

		bool installed = false;
		std::atomic<bool> bypassed{ false };
		std::vector<Entry> entries{ kCapacity };
		std::atomic<std::size_t> cursor{ 0 };
		std::atomic<std::uint32_t> overflow{ 0 };
		std::atomic<std::uint32_t> lastThread{ 0 };
		std::atomic<std::uint32_t> threadCount{ 0 };
		Stats stats;
		std::atomic<std::uint32_t> withheld{ 0 };
		std::vector<Entry> utilityEntries;
		std::atomic<std::size_t> utilityCursor{ 0 };
		std::atomic<std::uint32_t> utilityOverflow{ 0 };
		// Published whole by the render thread, read by the registering thread. shared_ptr's atomic
		// load/store keeps the readers safe while the next one is being built.
		std::shared_ptr<const ClaimSet> claims;
		// The last drain (valid until the next frame's registrations), and what HandBackUndrawable returned.
		std::span<const Entry> lastDrain;
		ankerl::unordered_dense::set<const RE::BSGeometry*> handedBack;
		ankerl::unordered_dense::set<const RE::BSGeometry*> withheldThisFrame;  // built on the first WithheldThisFrame
		bool withheldBuilt = false;
		std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> mainRenderers;
		std::shared_ptr<const ShadowRendererMap> shadowRenderers;
		std::array<std::shared_ptr<const ClaimSet>, kShadowModes> shadowClaims;
		std::array<std::atomic<std::uint32_t>, kShadowModes> shadowWithheld{};
		std::atomic<std::uint32_t> volumetricWithheld{ 0 };
		std::atomic<std::uint32_t> directWithheld{ 0 };
		/** @brief The shadow claim test at a direct group insertion: true when the pass is withheld. */
		bool WithholdAtGroup(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::atomic<std::uint32_t>& a_counter);
		struct VolumetricGroupHook;
		friend struct VolumetricGroupHook;
		template <std::uint32_t Hint>
		struct DirectGroupHook;
		template <std::uint32_t Hint>
		friend struct DirectGroupHook;

		struct Hook;
		friend struct Hook;
		void Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_fading, bool a_withheld);
		bool Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_fading);

	public:
		/** @brief CS_DCLF_REGISTER_PROBE: registrations by shader type, and how many into a main renderer. */
		static constexpr std::size_t kShaderTypes = 16;
		std::array<std::atomic<std::uint32_t>, kShaderTypes> probeCounts{};
		std::array<std::atomic<std::uint32_t>, kShaderTypes> probeMain{};

		// [TEMP] CS_DCLF_CASCADE_PROBE: every Utility registration into a shadow view's renderer this frame,
		// withheld or not, so the far-cascade probe can compare the engine's per-view caster set with DCLF's.
		struct ShadowRegistration
		{
			const RE::BSBatchRenderer* batch;
			const RE::BSGeometry* geometry;
			bool withheld;
		};
		static bool CascadeProbeEnabled();
		std::vector<ShadowRegistration> TakeShadowRegistrations();

	private:
		std::mutex shadowRegistrationsLock;  // [TEMP] diagnostics only
		std::vector<ShadowRegistration> shadowRegistrations;
	};
}
