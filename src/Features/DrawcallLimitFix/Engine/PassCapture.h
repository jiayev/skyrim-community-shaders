#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Scene/SceneSet.h"

namespace DCLF
{
	/**
	 * @brief Captures each lighting pass as the engine registers it with a batch renderer.
	 *
	 * The main camera's registrations are no source of DCLF's bindings (scene membership is): the diagnostics read them
	 * (SceneStore::DrainCapture, the decal order probe). A member of the DCLF set (SceneSet.h) has its passes withheld from the
	 * main camera's views (the Z-prepass's accumulator and the main one) at every insertion point of their registrations, and
	 * the shadow views' registrations are withheld for the casters DCLF draws.
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
			// The set's casters' Utility passes kept out of the shadow views' batch renderers, by the view's render mode (plain 0xD,
			// clamped 0xE, paraboloid 0xF).
			std::array<std::uint32_t, 3> shadowWithheld{};
			std::uint32_t volumetricWithheld = 0;  // volumetric-only passes (hint 8) kept out of batch group 15
			std::uint32_t directWithheld = 0;      // shadow passes of hints 11, 7 and 3, which bypass RegisterPass too
			// The main camera's views: a member's passes withheld; a member's LOD cross-fade copy (hint 10) and its fades DCLF does
			// not model, left to the engine.
			std::uint32_t mainWithheld = 0, mainCrossfadeCopies = 0, mainUnmodelledFades = 0;
			// Since the last report (TakeFadeTotals): the frames' cross-fade copies and unmodelled fades, summed.
			std::uint64_t crossfadeTotal = 0, unmodelledTotal = 0;
			std::uint32_t occlusionWithheld = 0;  // the occlusion maps' registrations: members' passes withheld
			std::uint32_t treeLodWithheld = 0;    // tree LOD's passes into the main camera's views, while DCLF draws it
			// The water reflection's faces: reflection-phase members' passes, and tree LOD's while DCLF draws the faces' tree LOD.
			std::uint32_t reflectionWithheld = 0, reflectionTreeLodWithheld = 0;
		};

		/**
		 * @brief Render thread, at the frame's first DCLF point (before the main camera's cull): whether DCLF draws tree LOD this
		 * frame (IndirectDraws::DecideTreeLod). While it does, every tree LOD pass into the main camera's views is withheld.
		 */
		void SetTreeLodOwned(bool a_owned) { treeLodOwned.store(a_owned, std::memory_order_release); }
		bool TreeLodOwned() const { return treeLodOwned.load(std::memory_order_acquire); }

		/**
		 * @brief The water reflection's faces (dclf-lod.md, "Water reflections"). SetReflectionCamera: render thread, at a face render,
		 * the cube camera whose accumulators' batch renderers are the faces' (made again only when they change). SetReflectionFace:
		 * around a face render in the plain render mode (0), the one whose registrations the hooks see (mode 0x19's are
		 * FUN_1414b2b90's); only then is anything withheld. SetReflectionTreeLodOwned: once a frame, whether DCLF draws the faces'
		 * tree LOD (IndirectDraws::PrepareReflection).
		 */
		void SetReflectionCamera(const RE::NiCamera* a_camera);
		void SetReflectionFace(bool a_plain) { reflectionFace.store(a_plain, std::memory_order_release); }
		void SetReflectionTreeLodOwned(bool a_owned) { reflectionTreeLodOwned.store(a_owned, std::memory_order_release); }
		/** @brief Render thread, BeginSceneFrame: whether the frame's faces are DCLF's at all (IndirectDraws::DecideCoverage). */
		void SetReflectionCovered(bool a_covered) { reflectionCovered.store(a_covered, std::memory_order_release); }
		bool ReflectionTreeLodOwned() const { return reflectionTreeLodOwned.load(std::memory_order_acquire); }
		/**
		 * @brief The faces' residue parity (T4): render thread, at an update's start, the cube camera's roots (ReflectionFaces::RootBits)
		 * to watch this update, which keeps them, 0 for none. While set, a pass registered into a face's renderer and not withheld - a
		 * geometry under those roots the engine still draws - is counted, by root (the sky's are not).
		 */
		void WatchReflectionResidue(std::uint32_t a_roots) { reflectionResidueWatch.store(a_roots, std::memory_order_release); }
		struct ReflectionResidue
		{
			std::uint32_t lodPasses = 0, treePasses = 0;  // under the LOD land and objects roots, under the tree root
			std::string first;
		};
		/** @brief Since the last call. */
		ReflectionResidue TakeReflectionResidue();

		static constexpr std::uint32_t kShadowModes = 3;

		static PassCapture& Get();

		void Install();
		bool Installed() const { return installed; }
		/** @brief Makes the hook a plain pass-through while set: DCLF is off (forced, or switched off in the menu). */
		void SetBypassed(bool a_bypassed) { bypassed.store(a_bypassed, std::memory_order_release); }
		bool Bypassed() const { return bypassed.load(std::memory_order_acquire); }

		/**
		 * @brief The frame's DCLF set (SceneStore::CommitSet, at the scene phase); render thread. Immutable and swapped whole:
		 * the hooks read it from whatever thread the engine registers on, and every registration of the frame reads this one.
		 * Null: nothing is withheld (DCLF off, a load).
		 */
		void PublishSet(std::shared_ptr<const SetSnapshot> a_set) { std::atomic_store(&frameSet, std::move(a_set)); }
		std::shared_ptr<const SetSnapshot> CurrentSet() const { return std::atomic_load(&frameSet); }
		/**
		 * @brief The main camera's views' batch renderers: the Z-prepass's accumulator (render mode 0xC, *0x14338c828) and the
		 * main one (*0x14338c830), each with its geometry groups'. Render thread, before the frame's registrations; the
		 * accumulators are the engine's for the session, so the set is made again only when one of them changes.
		 */
		void RefreshMainRenderers();
		/** @brief Whether a_batch is one of the main camera's views' batch renderers (RefreshMainRenderers). */
		bool IsMainRenderer(const RE::BSBatchRenderer* a_batch) const
		{
			const auto renderers = std::atomic_load(&mainRenderers);
			return renderers && renderers->contains(a_batch);
		}
		/** @brief Report thread: the cross-fade copies and unmodelled fades summed since the last call (Stats::crossfadeTotal, unmodelledTotal). */
		std::pair<std::uint64_t, std::uint64_t> TakeFadeTotals() { return { std::exchange(stats.crossfadeTotal, 0), std::exchange(stats.unmodelledTotal, 0) }; }
		/**
		 * @brief The geometries whose main-camera registration met a fade DCLF does not model since the last call (render thread,
		 * SceneStore::CommitSet): their passes went to the engine, and they leave the set until the fade ends.
		 */
		template <class F>
		void TakeUnmodelledFades(F&& a_visit)
		{
			unmodelledFades.Drain([&](auto&& a_geometry) { a_visit(a_geometry); });
		}
		/**
		 * @brief The occlusion map whose scene the engine registers next (Precipitation::SetupMask, render mode 0x1C), as its phase
		 * (kSetOccluderSky, kSetOccluderPrecipitation), or 0 when nothing of it is withheld (DCLF does not draw the map this frame).
		 * Render thread, before SetupMask.
		 */
		void SetOcclusionPhase(std::uint8_t a_phase) { occlusionPhase.store(a_phase, std::memory_order_release); }
		/** @brief CS_DCLF_PARITY_BOTH: nothing is withheld, so the engine draws what DCLF draws as well (capture parity). */
		static bool ParityBoth();
		/** @brief The shadow views' render modes: 0xD plain, 0xE clamped, 0xF paraboloid. */
		static constexpr std::uint32_t kFirstShadowMode = 0xD;
		/** @brief Which batch renderers belong to shadow views, each with its view's render mode index. */
		using ShadowRendererMap = ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint8_t>;
		void SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers);
		/** @brief The covered focus views' renderers, by focus descriptor: the parity's membership check (FocusViews::NoteRegistration). */
		void SetFocusBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers);
		/** @brief Whether a_batch is a shadow view's batch renderer this frame, and its mode index. */
		bool ShadowModeOfBatch(const RE::BSBatchRenderer* a_batch, std::uint32_t& a_mode) const;
		/**
		 * @brief The set Withhold decides a pass into this batch renderer by (a member with the caster phase is withheld): the frame's,
		 * when the renderer is a shadow view's (as far as the last ShadowViews rebuild knows) and DCLF draws the shadow views this
		 * frame; else null. SunAccumulation skips a member's cascade registration by the same test.
		 */
		std::shared_ptr<const SetSnapshot> CastersForBatch(const RE::BSBatchRenderer* a_batch, std::uint8_t* a_phase = nullptr) const;
		/** @brief Whether the frame's set withholds a shadow phase's members from its views (DCLF draws them this frame). */
		bool CastersWithheld(std::uint8_t a_phase) const;
		/** @brief The shadow views are drawn by the render graph (live: Toggles.h), and CS_DCLF_PARITY_BOTH is off. */
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
		// The last drain (valid until the next frame's registrations).
		std::span<const Entry> lastDrain;
		std::shared_ptr<const ShadowRendererMap> shadowRenderers;
		std::shared_ptr<const ShadowRendererMap> focusRenderers;
		std::array<std::atomic<std::uint32_t>, kShadowModes> shadowWithheld{};
		std::atomic<std::uint32_t> volumetricWithheld{ 0 };
		std::atomic<std::uint32_t> directWithheld{ 0 };
		std::atomic<std::uint32_t> mainWithheld{ 0 }, mainCrossfadeCopies{ 0 }, mainUnmodelledFades{ 0 }, occlusionWithheld{ 0 };
		std::atomic<std::uint32_t> treeLodWithheld{ 0 };
		std::atomic<bool> treeLodOwned{ false };
		std::atomic<std::uint32_t> reflectionWithheld{ 0 }, reflectionTreeLodWithheld{ 0 };
		std::atomic<std::uint32_t> reflectionResidueWatch{ 0 }, reflectionResidueLod{ 0 }, reflectionResidueTree{ 0 };
		std::mutex reflectionResidueLock;
		std::string reflectionResidueFirst;  // under reflectionResidueLock
		void NoteReflectionResidue(const RE::BSRenderPass* a_pass, bool a_treeLod);
		std::atomic<bool> reflectionFace{ false }, reflectionTreeLodOwned{ false }, reflectionCovered{ true };
		std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> reflectionRenderers;
		std::array<const void*, 2> reflectionAccumulators{};  // what reflectionRenderers was made from (render thread)
		/** @brief The reflection test at every main-mode insertion point: a reflection-phase member's pass into a face's renderer. */
		bool WithholdReflection(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
		std::atomic<std::uint8_t> occlusionPhase{ 0 };
		struct OcclusionGroupHook;
		friend struct OcclusionGroupHook;
		std::shared_ptr<const SetSnapshot> frameSet;
		using RendererSet = ankerl::unordered_dense::set<const RE::BSBatchRenderer*>;
		std::shared_ptr<const RendererSet> mainRenderers;
		std::array<const void*, 2> mainAccumulators{};  // what mainRenderers was made from (render thread)
		EventQueue<const RE::BSGeometry*, 1024> unmodelledFades;
		/**
		 * @brief The main-view test at every insertion point: true when the pass is a member's, into one of the main camera's views,
		 * and not one the engine keeps (a cross-fade's copy of the old level, a fade DCLF does not model).
		 */
		bool WithholdMain(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
		template <std::uint32_t Group>
		struct MainGroupHook;
		template <std::uint32_t Group>
		friend struct MainGroupHook;
		struct MainListHook;
		friend struct MainListHook;
		/** @brief The shadow test at a direct group insertion: true when the pass is withheld. */
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
