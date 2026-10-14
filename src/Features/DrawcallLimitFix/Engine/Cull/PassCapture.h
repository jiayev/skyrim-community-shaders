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

#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Scene/Model/SceneSet.h"

namespace DCLF
{
	/**
	 * @brief The registration hooks at every point where the engine hands a pass to a batch renderer. A member of the frame's DCLF
	 * set (SceneSet.h) has its passes withheld from the main views (Z-prepass and main accumulators), the first-person accumulator,
	 * the reflection faces and the occlusion maps while DCLF draws them; shadow views withhold the casters DCLF draws. Runs on
	 * whatever thread the engine registers on.
	 *
	 * Lighting passes are also recorded, for observers only (FrameView::DrainCapture): DCLF draws nothing from registrations.
	 * `BSBatchRenderer::RegisterPass` (vfunc 0x02), not GetRenderPasses, because `technique` (the caller's `techniqueID`, which
	 * differs from `passEnum`: the accumulator adds DoAlphaTest) and `subPass` only exist there.
	 *
	 * Registration is concurrent (the classifier takes a mutex; scene lists build on `gJobList_SceneListAccumCulling`): entries go
	 * into a fixed buffer with an atomic cursor, drained once a frame by the render thread; overflow is counted. CS_DCLF_CENSUS also
	 * notes every pass handed on (RegistrationCensus.h).
	 */
	class PassCapture
	{
	public:
		struct Entry
		{
			const RE::BSGeometry* geometry = nullptr;
			const RE::BSRenderPass* pass = nullptr;
			const RE::BSBatchRenderer* batch = nullptr;
			std::uint32_t technique = 0;                 // techniqueID, the batch group's key
			std::uint32_t subPass = 0;                   // SubPassOf
			std::uint32_t passEnum = 0;
			bool withheld = false;
			std::uint8_t fadeState = 0xFF;               // the fade node's LOD state (+0x153), 0xFF without one
			std::uint8_t hint = 0;                       // accumulationHint
		};

		struct Stats
		{
			std::uint32_t captured = 0;   // entries the last drain saw
			std::uint32_t overflowed = 0;
			std::uint32_t threads = 0;
			// Casters' Utility passes withheld from shadow views, by render mode (plain 0xD, clamped 0xE, paraboloid 0xF).
			std::array<std::uint32_t, 3> shadowWithheld{};
			std::uint32_t volumetricWithheld = 0;  // volumetric-only passes (hint 8) kept out of batch group 15
			std::uint32_t directWithheld = 0;      // shadow passes of hints 11, 7 and 3, which bypass RegisterPass too
			std::uint32_t mainWithheld = 0;
			std::uint32_t occlusionWithheld = 0;
			std::uint32_t treeLodWithheld = 0;    // tree LOD into the main views while DCLF draws it
			// The water reflection's faces: reflection-phase members, and tree LOD while DCLF draws the faces' tree LOD.
			std::uint32_t reflectionWithheld = 0, reflectionTreeLodWithheld = 0;
		};

		/** @brief Render thread, before the main cull: whether DCLF draws tree LOD this frame (then its main-view passes are withheld). */
		void SetTreeLodOwned(bool a_owned) { treeLodOwned.store(a_owned, std::memory_order_release); }
		bool TreeLodOwned() const { return treeLodOwned.load(std::memory_order_acquire); }

		/**
		 * @brief The water reflection's faces (dclf-lod.md, "Water reflections"). SetReflectionCamera: render thread, the cube camera
		 * whose accumulators' renderers are the faces'. SetReflectionFace: around a face render in plain mode (0), the only one whose
		 * registrations the hooks see (mode 0x19's are FUN_1414b2b90's). SetReflectionTreeLodOwned: once a frame.
		 */
		void SetReflectionCamera(const RE::NiCamera* a_camera);
		void SetReflectionFace(bool a_plain) { reflectionFace.store(a_plain, std::memory_order_release); }
		void SetReflectionTreeLodOwned(bool a_owned) { reflectionTreeLodOwned.store(a_owned, std::memory_order_release); }
		/**
		 * @brief Render thread, BeginSceneFrame: whether the first-person epochs draw the first-person members. While set, a
		 * kSetFirstPerson member's pass into the first-person accumulator (*0x14338c840, Main::unk_FPCulling's) or the Z-prepass's
		 * (RenderDepth's first-person block culls into it) is withheld.
		 */
		void SetFirstPersonCovered(bool a_covered) { firstPersonCovered.store(a_covered, std::memory_order_release); }
		bool FirstPersonCovered() const { return firstPersonCovered.load(std::memory_order_acquire); }
		/** @brief Whether a_batch is one of the first-person accumulator's batch renderers (for the census). */
		bool IsFirstPersonRenderer(const RE::BSBatchRenderer* a_batch) const
		{
			const auto renderers = std::atomic_load(&firstPersonRenderers);
			return renderers && renderers->contains(a_batch);
		}
		/**
		 * @brief The <- FIRST PERSON observer's counts since the last call: members' passes withheld, members' passes handed on (the
		 * epochs did not draw), and every pass into the first-person accumulator.
		 */
		struct FirstPersonCounts
		{
			std::uint32_t withheld = 0, memberKept = 0, registered = 0;
		};
		FirstPersonCounts TakeFirstPersonCounts()
		{
			return { firstPersonWithheld.exchange(0, std::memory_order_relaxed), firstPersonMemberKept.exchange(0, std::memory_order_relaxed),
				firstPersonRegistered.exchange(0, std::memory_order_relaxed) };
		}
		/** @brief Render thread, BeginSceneFrame: whether the frame's faces are DCLF's at all. */
		void SetReflectionCovered(bool a_covered) { reflectionCovered.store(a_covered, std::memory_order_release); }
		bool ReflectionTreeLodOwned() const { return reflectionTreeLodOwned.load(std::memory_order_acquire); }
		/** @brief Whether a_batch is one of the reflection faces' batch renderers (for the census). */
		bool IsReflectionRenderer(const RE::BSBatchRenderer* a_batch) const
		{
			const auto renderers = std::atomic_load(&reflectionRenderers);
			return renderers && renderers->contains(a_batch);
		}
		/**
		 * @brief The faces' residue watch: render thread, at an update's start, the cube camera's roots to watch (0: none). While set,
		 * passes into a face's renderer that are not withheld (geometry the engine still draws) are counted by root (not the sky's).
		 */
		void WatchReflectionResidue(std::uint32_t a_roots) { reflectionResidueWatch.store(a_roots, std::memory_order_release); }
		struct ReflectionResidue
		{
			std::uint32_t lodPasses = 0, treePasses = 0;  // under the LOD land and objects roots, under the tree root
			std::string first;
		};
		/** @brief Since the last call. */
		ReflectionResidue TakeReflectionResidue();
		/** @brief The residue's geometries under the LOD roots since the last call (pointers only: never dereferenced). */
		std::vector<const RE::BSGeometry*> TakeReflectionResidueGeometries();

		static constexpr std::uint32_t kShadowModes = 3;

		static PassCapture& Get();

		void Install();
		bool Installed() const { return installed; }
		/** @brief The hook is a plain pass-through while set (DCLF off). */
		void SetBypassed(bool a_bypassed) { bypassed.store(a_bypassed, std::memory_order_release); }
		bool Bypassed() const { return bypassed.load(std::memory_order_acquire); }

		/**
		 * @brief The frame's DCLF set (FrameView::PublishFrameClaims); render thread. Immutable and swapped whole, so every registration
		 * of the frame, on any thread, reads the same one. Null: nothing is withheld.
		 */
		void PublishSet(std::shared_ptr<const SetSnapshot> a_set) { std::atomic_store(&frameSet, std::move(a_set)); }
		std::shared_ptr<const SetSnapshot> CurrentSet() const { return std::atomic_load(&frameSet); }
		/**
		 * @brief The main views' batch renderers: the Z-prepass accumulator (render mode 0xC, *0x14338c828) and the main one
		 * (*0x14338c830), with their geometry groups'. Render thread, before registrations; rebuilt only when an accumulator changes.
		 */
		void RefreshMainRenderers();
		/** @brief Whether a_batch is one of the main camera's views' batch renderers (RefreshMainRenderers). */
		bool IsMainRenderer(const RE::BSBatchRenderer* a_batch) const
		{
			const auto renderers = std::atomic_load(&mainRenderers);
			return renderers && renderers->contains(a_batch);
		}
		/**
		 * @brief The occlusion map the engine registers next (Precipitation::SetupMask, render mode 0x1C) as its phase, or 0 when DCLF
		 * does not draw it. Render thread, before SetupMask.
		 */
		void SetOcclusionPhase(std::uint8_t a_phase) { occlusionPhase.store(a_phase, std::memory_order_release); }
		/** @brief The shadow views' render modes: 0xD plain, 0xE clamped, 0xF paraboloid. */
		static constexpr std::uint32_t kFirstShadowMode = 0xD;
		/** @brief Which batch renderers belong to shadow views, each with its view's render mode index. */
		using ShadowRendererMap = ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint8_t>;
		void SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers);
		/** @brief The covered focus views' renderers, by focus descriptor (for the focus membership observer). */
		void SetFocusBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers);
		/** @brief Whether a_batch is a shadow view's renderer this frame, with its mode index. */
		bool ShadowModeOfBatch(const RE::BSBatchRenderer* a_batch, std::uint32_t& a_mode) const;
		/**
		 * @brief The set Withhold tests a pass into a_batch against: the frame's when a_batch is a shadow view's and DCLF draws the shadow
		 * views this frame, else null. SunAccumulation uses the same test.
		 */
		std::shared_ptr<const SetSnapshot> CastersForBatch(const RE::BSBatchRenderer* a_batch, std::uint8_t* a_phase = nullptr) const;
		/** @brief Whether the frame's set withholds a shadow phase's members from its views (DCLF draws them this frame). */
		bool CastersWithheld(std::uint8_t a_phase) const;
		/** @brief The render graph draws the shadow views (Toggles.h) and CS_DCLF_PARITY_BOTH is off. */
		static bool ShadowWithholdingEnabled();
		/**
		 * @brief Passes this thread has handed to a batch renderer so far (withheld or not): the difference across a registration call
		 * is how many it built.
		 */
		static std::uint32_t PassesOnThisThread();

		/** @brief Takes everything registered since the last call; render thread only. */
		std::span<const Entry> Drain();
		/** @brief The last Drain's entries (diagnostics); render thread only. */
		std::span<const Entry> LastDrain() const { return lastDrain; }

		const Stats& GetStats() const { return stats; }
		Stats& MutableStats() { return stats; }

		/**
		 * @brief The batch group list a pass lands in. The engine's classifier reads only the property flags and the alpha property:
		 * flag bit 54 means list 4, else bit 36 (as 2) plus alpha testing. Nothing per-frame, so DCLF can compute it without registering.
		 */
		static std::uint32_t SubPassOf(const RE::BSGeometry* a_geometry, std::uint64_t a_propertyFlags);
		/** @brief SubPassOf from the alpha property's testing bit (a record's). */
		static std::uint32_t SubPassOf(bool a_alphaTesting, std::uint64_t a_propertyFlags);

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
		std::atomic<std::uint32_t> mainWithheld{ 0 }, occlusionWithheld{ 0 };
		std::atomic<std::uint32_t> treeLodWithheld{ 0 };
		std::atomic<bool> treeLodOwned{ false };
		std::atomic<std::uint32_t> reflectionWithheld{ 0 }, reflectionTreeLodWithheld{ 0 };
		std::atomic<std::uint32_t> reflectionResidueWatch{ 0 }, reflectionResidueLod{ 0 }, reflectionResidueTree{ 0 };
		std::mutex reflectionResidueLock;
		std::string reflectionResidueFirst;  // under reflectionResidueLock
		std::vector<const RE::BSGeometry*> reflectionResidueGeometries;  // under reflectionResidueLock
		void NoteReflectionResidue(const RE::BSRenderPass* a_pass, bool a_treeLod);
		std::atomic<bool> reflectionFace{ false }, reflectionTreeLodOwned{ false }, reflectionCovered{ true };
		std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> reflectionRenderers;
		std::array<const void*, 2> reflectionAccumulators{};  // what reflectionRenderers was made from (render thread)
		/** @brief The reflection test at every main-mode insertion point: a reflection-phase member's pass into a face renderer. */
		bool WithholdReflection(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
		std::atomic<std::uint8_t> occlusionPhase{ 0 };
		struct OcclusionGroupHook;
		friend struct OcclusionGroupHook;
		std::shared_ptr<const SetSnapshot> frameSet;
		using RendererSet = ankerl::unordered_dense::set<const RE::BSBatchRenderer*>;
		std::shared_ptr<const RendererSet> mainRenderers;
		std::array<const void*, 2> mainAccumulators{};  // what mainRenderers was made from (render thread)
		// The first-person accumulator's renderers (also in mainRenderers), their source, and the counts.
		std::shared_ptr<const RendererSet> firstPersonRenderers;
		const void* firstPersonAccumulator = nullptr;
		std::atomic<bool> firstPersonCovered{ false };
		std::atomic<std::uint32_t> firstPersonWithheld{ 0 }, firstPersonMemberKept{ 0 }, firstPersonRegistered{ 0 };
		/**
		 * @brief The main-view test at every insertion point: a member's pass into a main view (all of them: DCLF draws its fades and LOD
		 * cross-fade copy), or tree LOD's or a first-person member's while DCLF draws those.
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
		void Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_withheld);
		/** @brief A pass handed on to the engine, for observers (Diagnostics::PassKept). */
		void NoteKept(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint8_t a_site, std::uint8_t a_occlusionPhase = 0) const;
		bool Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
	};
}
