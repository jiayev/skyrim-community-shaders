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

#include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"

namespace DCLF
{
	/**
	 * @brief Captures each lighting pass as the engine registers it with a batch renderer.
	 *
	 * The main camera's registrations are no source of DCLF's bindings (scene membership is): the diagnostics read them
	 * (SceneStore::DrainCapture, the hole detector, the decal order probe), and the shadow views' registrations are withheld
	 * for the casters DCLF draws.
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
			// CS_DCLF_SHADOW_OWNERSHIP=static: Utility passes kept out of the shadow views' batch renderers,
			// by the view's render mode (plain 0xD, clamped 0xE, paraboloid 0xF).
			std::array<std::uint32_t, 3> shadowWithheld{};
			std::uint32_t volumetricWithheld = 0;  // volumetric-only passes (hint 8) kept out of batch group 15
			std::uint32_t directWithheld = 0;      // shadow passes of hints 11, 7 and 3, which bypass RegisterPass too
			std::uint32_t claimed = 0;   // objects DCLF said it owns
			// Left out of the engine's cull or registration and not drawn this frame: nothing else draws them, so any of
			// these is a visible hole.
			std::uint32_t holes = 0;
			// Claim churn. A claim that goes away hands the object back to the native loop, so if culling
			// an object unclaims it the engine simply draws it again and the culling saves nothing.
			std::uint32_t claimsAdded = 0;
			std::uint32_t claimsDropped = 0;
			std::uint32_t droppedAfterCull = 0;  // dropped while the engine still had a pass for it
		};

		/**
		 * @brief The set of geometries DCLF draws (the colour build's drawn marks, dropped a frame after the last draw): the
		 * main camera's leaf exclusion (PrimaryCull::Owned) and the native loop's skip read it; a shadow mode's claims are its
		 * casters, withheld from its views' registration.
		 */
		using ClaimSet = ankerl::unordered_dense::set<const RE::BSGeometry*>;
		static constexpr std::uint32_t kShadowModes = 3;
		struct FrameClaims
		{
			std::array<std::shared_ptr<const ClaimSet>, kShadowModes> shadow;
		};

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
		/** @brief Freeze the shadow claims before registration, or install a selected publication's claims. */
		void SelectLegacyFrameClaims();
		void InstallFrameClaims(std::shared_ptr<const FrameClaims> a_claims);
		void ClearFrameClaims() { InstallFrameClaims(nullptr); }

		/** @brief Latest producer result (the selected frame bundle may still hold the preceding one). */
		std::shared_ptr<const ClaimSet> CurrentClaims() const { return std::atomic_load(&claims); }

		/** @brief The shadow views' render modes with claims of their own: 0xD plain, 0xE clamped, 0xF paraboloid. */
		static constexpr std::uint32_t kFirstShadowMode = 0xD;
		/** @brief Which batch renderers belong to shadow views, each with its view's render mode index. */
		using ShadowRendererMap = ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint8_t>;
		void SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers);
		/**
		 * @brief CS_DCLF_SHADOW_OWNERSHIP=static: the casters DCLF's shadow epochs draw under a render mode,
		 * withheld from every shadow view of that mode from the next frame's selection on. Published by the
		 * epoch that submitted them, as the main claims are published by the colour epoch.
		 */
		void PublishShadowClaims(std::uint32_t a_modeIndex, std::shared_ptr<const ClaimSet> a_claims);
		/** @brief Whether the frame's selection withholds any caster from the shadow mode's views (a non-empty claim set). */
		bool ShadowModeWithheld(std::uint32_t a_modeIndex) const;
		/** @brief Whether a_batch is a shadow view's batch renderer this frame, and its mode index. */
		bool ShadowModeOfBatch(const RE::BSBatchRenderer* a_batch, std::uint32_t& a_mode) const;
		/**
		 * @brief The claims Withhold decides a pass into this batch renderer by: its shadow view's render mode's, or
		 * null when the renderer is not a shadow view's (as far as the last ShadowViews rebuild knows) or shadow
		 * ownership is off. SunAccumulation skips a claimed geometry's registration by the same test.
		 */
		std::shared_ptr<const ClaimSet> ShadowClaimsForBatch(const RE::BSBatchRenderer* a_batch) const;
		/** @brief CS_DCLF_SHADOW_OWNERSHIP=static (live: Toggles.h): withhold claimed casters from the shadow views. */
		static bool ShadowWithholdingEnabled();
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

		/** @brief Takes everything registered since the last call; render thread only. */
		std::span<const Entry> Drain();
		/** @brief What the last Drain took (diagnostics); render thread only. */
		std::span<const Entry> LastDrain() const { return lastDrain; }

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
		// Published whole by the render thread, read by the registering thread. shared_ptr's atomic
		// load/store keeps the readers safe while the next one is being built.
		std::shared_ptr<const ClaimSet> claims;
		// One shadow selection for every registration in a frame. A later PublishShadowClaims only affects the next frame.
		std::shared_ptr<const FrameClaims> frameClaims;
		// The last drain (valid until the next frame's registrations).
		std::span<const Entry> lastDrain;
		std::shared_ptr<const ShadowRendererMap> shadowRenderers;
		std::array<std::shared_ptr<const ClaimSet>, kShadowModes> shadowClaims;
		std::array<std::atomic<std::uint32_t>, kShadowModes> shadowWithheld{};
		std::atomic<std::uint32_t> volumetricWithheld{ 0 };
		std::atomic<std::uint32_t> directWithheld{ 0 };
		/** @brief ShadowClaimsForBatch whatever the toggles, and the view's render mode (a_mode) when it is a shadow view's. */
		std::shared_ptr<const ClaimSet> ShadowClaimsOf(const RE::BSBatchRenderer* a_batch, std::uint32_t& a_mode) const;
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
	};
}
