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

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"

namespace DCLF
{
	/**
	 * @brief The registration hooks at every point where the engine hands a pass to a batch renderer. A member of the frame's DCLF
	 * set (SceneSet.h) has its passes withheld from the main views (Z-prepass and main accumulators), an InMainRenderers view's
	 * accumulators (the first person's), an OwnRenderers view's renderers (the reflection faces') and the occlusion maps while DCLF draws
	 * them; shadow views withhold the casters DCLF draws. Runs on whatever thread the engine registers on.
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
			// By draw family: its engine shapes' passes into the main views while DCLF draws the family (tree LOD's).
			std::array<std::uint32_t, static_cast<std::size_t>(Api::DrawFamily::Count)> familyWithheld{};
			// By view, an OwnRenderers view's (the water reflection's faces): its members' passes, and by draw family its engine shapes'
			// while DCLF draws the family per face (tree LOD's).
			std::array<std::uint32_t, Extensions::kEngineViews> viewWithheld{};
			std::array<std::array<std::uint32_t, static_cast<std::size_t>(Api::DrawFamily::Count)>, Extensions::kEngineViews> viewFamilyWithheld{};
		};

		/**
		 * @brief Render thread, before the main cull (IndirectDraws::DecideDrawFamilies): whether DCLF draws a draw family this frame (then
		 * the engine's main-view passes its draws replace are withheld: TreeInstances', tree LOD's shapes).
		 */
		void SetFamilyOwned(Api::DrawFamily a_family, bool a_owned) { familyOwned[static_cast<std::size_t>(a_family)].store(a_owned, std::memory_order_release); }
		bool FamilyOwned(Api::DrawFamily a_family) const { return familyOwned[static_cast<std::size_t>(a_family)].load(std::memory_order_acquire); }

		/**
		 * @brief Render thread, BeginSceneFrame: whether a view's epochs draw its members (its coverage; before the first, its
		 * coveredInitially). While an InMainRenderers view's is set, its member's pass into the view's accumulators (the first person's:
		 * *0x14338c840, Main::unk_FPCulling's) or into the main views' (the first person's RenderDepth block culls into the Z-prepass's) is
		 * withheld; while an OwnRenderers view's is, its member's pass into its renderers while its render withholds (the reflection faces').
		 */
		void SetViewCovered(Api::EngineView a_view, bool a_covered) { viewRenderers[static_cast<std::size_t>(a_view)].coverage.store(a_covered ? 2 : 1, std::memory_order_release); }
		/** @brief A view's coverage (SetViewCovered). */
		bool ViewCovered(Api::EngineView a_view) const
		{
			const auto coverage = viewRenderers[static_cast<std::size_t>(a_view)].coverage.load(std::memory_order_acquire);
			return coverage ? coverage == 2 : Extensions::ViewEntryOf(a_view).coveredInitially;
		}
		/** @brief Whether a_batch is one of an InMainRenderers view's accumulators' batch renderers. */
		bool IsViewRenderer(Api::EngineView a_view, const RE::BSBatchRenderer* a_batch) const
		{
			const auto renderers = std::atomic_load(&viewRenderers[static_cast<std::size_t>(a_view)].renderers);
			return renderers && renderers->contains(a_batch);
		}
		/**
		 * @brief An OwnRenderers view's renderers (the water reflection's faces, dclf-architecture.md, "Water reflections"), named by its feature's
		 * engine hooks. SetViewRenderers: render thread, the accumulators whose batch renderers (and their geometry and alpha groups') are the
		 * view's (the cube camera's), made again whenever any of those renderers changes (the engine makes groups as geometry first uses them). SetViewWithholdActive: around the view's render while its members' passes
		 * into them are withheld (the faces' plain mode, 0: the only one whose registrations the hooks see; mode 0x19's are FUN_1414b2b90's).
		 * SetViewRendering: around the view's render, whatever its mode (the observers: OwnViewRendering). SetViewFamilyOwned: once a
		 * frame, a trees view's draw family per face DCLF's (its engine shapes' passes into its renderers withheld; Core's while the family
		 * is taken: IndirectDraws::DecideViewFaces).
		 */
		void SetViewRenderers(Api::EngineView a_view, const Api::ViewAccumulators& a_accumulators);
		void SetViewWithholdActive(Api::EngineView a_view, bool a_active) { viewRenderers[static_cast<std::size_t>(a_view)].withholdActive.store(a_active, std::memory_order_release); }
		void SetViewRendering(Api::EngineView a_view, bool a_rendering) { viewRenderers[static_cast<std::size_t>(a_view)].rendering.store(a_rendering, std::memory_order_release); }
		void SetViewFamilyOwned(Api::EngineView a_view, Api::DrawFamily a_family, bool a_owned)
		{
			auto& owned = viewRenderers[static_cast<std::size_t>(a_view)].familyOwned;
			const std::uint32_t bit = 1u << static_cast<std::uint32_t>(a_family);
			if (a_owned)
				owned.fetch_or(bit, std::memory_order_release);
			else
				owned.fetch_and(~bit, std::memory_order_release);
		}
		/** @brief Whether a_batch is one of an OwnRenderers view's batch renderers (for the census). */
		bool IsOwnViewRenderer(Api::EngineView a_view, const RE::BSBatchRenderer* a_batch) const { return IsViewRenderer(a_view, a_batch); }
		/** @brief Render thread: whether an OwnRenderers view's render runs (a reflection face render: the capture parity's face draws). */
		bool OwnViewRendering() const;
		/** @brief Whether an OwnRenderers view's render withholds its members now (its own gates: active and covered; the census). */
		bool OwnViewWithholding() const;
		/**
		 * @brief An InMainRenderers view's counts since the last call (its report's observer, the first person's <- FIRST PERSON): members'
		 * passes withheld, members' passes handed on (the epochs did not draw), and every pass into the view's accumulators.
		 */
		struct ViewCounts
		{
			std::uint32_t withheld = 0, memberKept = 0, registered = 0;
		};
		ViewCounts TakeViewCounts(Api::EngineView a_view)
		{
			auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
			return { view.withheld.exchange(0, std::memory_order_relaxed), view.memberKept.exchange(0, std::memory_order_relaxed),
				view.registered.exchange(0, std::memory_order_relaxed) };
		}
		/**
		 * @brief An OwnRenderers view's residue watch (IndirectDraws::ViewLodRootsOwned, on the renders its residueDue keeps the roots):
		 * render thread, at a render's start, the view's roots to watch (Api::ViewRootBit; 0: none). While set, passes into the view's
		 * renderers that are not withheld (geometry the engine still draws) are counted by root (not the sky's): a draw family's engine
		 * shape under its faces' root, anything else under the view root contributions' roots.
		 */
		void WatchViewResidue(Api::EngineView a_view, std::uint32_t a_roots) { viewRenderers[static_cast<std::size_t>(a_view)].residueWatch.store(a_roots, std::memory_order_release); }
		struct ViewResidue
		{
			std::uint32_t lodPasses = 0, treePasses = 0;  // under the view root contributions' roots, under a draw family's faces' root
			std::string first;
		};
		/** @brief A view's residue since the last call. */
		ViewResidue TakeViewResidue(Api::EngineView a_view);
		/** @brief A view's residue geometries under the LOD roots since the last call (pointers only: never dereferenced). */
		std::vector<const RE::BSGeometry*> TakeViewResidueGeometries(Api::EngineView a_view);

		static constexpr std::uint32_t kShadowModes = 3;

		static PassCapture& Get();

		void Install();
		/** @brief The patch sites Install requires that another plugin changed, or empty; writes nothing. */
		static std::string SitesMissing();
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
		 * views this frame, else null. The Sun source's accumulation uses the same test.
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
		std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(Api::DrawFamily::Count)> familyWithheld{};
		std::array<std::atomic<bool>, static_cast<std::size_t>(Api::DrawFamily::Count)> familyOwned{};
		static_assert(static_cast<std::size_t>(Api::DrawFamily::Count) <= 32, "ViewRenderers::familyOwned is a 32-bit mask by family");
		/**
		 * @brief The OwnRenderers views' test at every main-mode insertion point, after WithholdMain: a member's pass of the view's phase
		 * into its renderers while its render withholds and its epochs draw (the reflection faces'), or tree LOD's while its tree LOD is DCLF's.
		 */
		bool WithholdOwn(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
		std::atomic<std::uint8_t> occlusionPhase{ 0 };
		struct OcclusionGroupHook;
		friend struct OcclusionGroupHook;
		std::shared_ptr<const SetSnapshot> frameSet;
		using RendererSet = ankerl::unordered_dense::set<const RE::BSBatchRenderer*>;
		std::shared_ptr<const RendererSet> mainRenderers;
		std::array<const void*, 2> mainAccumulators{};  // what mainRenderers was made from (render thread)
		std::vector<const RE::BSBatchRenderer*> mainSources;  // the batch renderers mainRenderers was made from, in order (render thread)
		// By view: its renderers (an InMainRenderers view's accumulators', also in mainRenderers; an OwnRenderers view's, SetViewRenderers),
		// their source (render thread) and its coverage (0: not set yet, its coveredInitially standing for it; 1 not covered; 2 covered).
		// An InMainRenderers view's counts; an OwnRenderers view's render's gates (rendering, withholding), its draw families' ownership, its
		// counts (Drain takes them) and its residue watch (the roots watched; the passes by root, the first and the geometries, the last
		// two under residueLock).
		struct ViewRenderers
		{
			std::shared_ptr<const RendererSet> renderers;
			Api::ViewAccumulators accumulators;
			std::vector<const RE::BSBatchRenderer*> sources;  // an OwnRenderers view's batch renderers its set was made from (render thread)
			std::atomic<std::uint8_t> coverage{ 0 };
			std::atomic<std::uint32_t> withheld{ 0 }, memberKept{ 0 }, registered{ 0 };
			std::atomic<bool> rendering{ false }, withholdActive{ false };
			std::atomic<std::uint32_t> familyOwned{ 0 };  // by draw family, a bit (SetViewFamilyOwned)
			std::atomic<std::uint32_t> ownWithheld{ 0 };
			std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(Api::DrawFamily::Count)> ownFamilyWithheld{};
			std::atomic<std::uint32_t> residueWatch{ 0 }, residueLod{ 0 }, residueTree{ 0 };
			std::mutex residueLock;
			std::string residueFirst;
			std::vector<const RE::BSGeometry*> residueGeometries;
		};
		std::array<ViewRenderers, Extensions::kEngineViews> viewRenderers;
		static void NoteViewResidue(ViewRenderers& a_view, const RE::BSRenderPass* a_pass, bool a_treeLod);
		/**
		 * @brief The main-view test at every insertion point: a member's pass into a main view (all of them: DCLF draws its fades and LOD
		 * cross-fade copy), or tree LOD's or an InMainRenderers view's member's (the first person's) while DCLF draws those.
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
