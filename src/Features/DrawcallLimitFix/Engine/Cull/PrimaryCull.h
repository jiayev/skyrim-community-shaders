#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DCLF::Diagnostics
{
	struct PrimaryCullView;
}

namespace DCLF::SceneCapture
{
	struct LeafView;
	struct NodeRecord;
	struct PropertyRecord;
}

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FadeState.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Scene/Model/SceneSet.h"

namespace DCLF
{
	struct SunCandidates;
	struct FrameGlobals;

	/**
	 * @brief The primary view's cull, and taking DCLF's objects out of it (docs/development/dclf-gpu-driven-frame.md;
	 * the plan: the main camera's list jobs and registration stop visiting what DCLF draws). AE only.
	 *
	 * The main camera culls the scene lists (`DAT_14338c870`, `DAT_14338c868` of them): BSTArray<NiPointer<NiAVObject>>
	 * that DrawWorld_BuildSceneLists fills with reference roots, round robin. NiCamera::CalculateAndDrawShadowCasterLights
	 * hands them to the sun's full-frustum cull first, then queues one list job per list and waits for them
	 * (JobList::Finish); the registration jobs walk the list processes' output afterwards. Between the full-frustum
	 * cull and that Finish, nothing but the list jobs reads the lists.
	 *
	 * The cut (toggle `excludePrimaryEntries`, CS_DCLF_PRIMARY_EXCLUDE): the list processes' Process1 (vtable slot 0x16,
	 * shared with every other list process, so filtered by process) stands in for the cull of every eligible entry, on
	 * the list job's own thread. For an admitted, settled entry in view (the job's own planes) it:
	 *   - leaves the root's OnVisible update to FadeStateCS (the fade, a leaf node's LOD step, a tree's LOD fix-up), written
	 *     back onto the node, and tests a tree's height;
	 *   - does nothing for its geometries in the DCLF set (SceneSet.h, the frame's): their records are drawn whenever the GPU
	 *     finds them;
	 *   - hands every other geometry in view to the engine's registration as the cull would (the process's
	 *     AppendVirtual, in the traversal's order): decals, effects and blended objects, and a DCLF geometry not in the set.
	 * Switch nodes are followed by event (a member is drawn while every switch above it selects its path: memberLive,
	 * updated from Coordinator's switch events, which also bring a newly selected child up to date), and a root that is
	 * fading or cross-fading LOD leaves the entry to the engine's Process1 that frame. An entry with engine-drawn parts
	 * (Cut::mixed) is the engine's Process1's whole: its OnVisible updates the node the engine's draws read, the AppendVirtual
	 * hook keeps DCLF's members from the registration, and FadeStateCS runs the same update for those members. An entry is eligible when its nodes are plain (NiNode, BSMultiBoundNode, switch
	 * nodes; a fade, leaf or tree root) and its geometries use BSGeometry's OnVisible, with at least one DCLF draws;
	 * it is admitted once all of those it shows are in the set (RunAdmission). After the jobs the render thread only
	 * gathers the jobs' output and clears the activeLightMask of what DCLF draws, as the main registration would.
	 * Frame precondition: the sun's entry exclusion is live (its cascades are captured).
	 */
	class PrimaryCull
	{
	public:
		static PrimaryCull& Get();

		/** @brief AE: the call sites in CalculateAndDrawShadowCasterLights, verified before patching. */
		void Install();
		bool Installed() const { return installed; }

		/**
		 * @brief The pass descriptor's ShadowDir and DefShadow bits (13, 14) a geometry whose bound meets a cascade takes: what depends on
		 * the object and the frame's globals alone. The GPU makes the cascade test (kObjectSunTest), as the engine's rule does with the
		 * sun's mask (dclf-gpu-driven-frame.md, "Phase 1, step 2").
		 */
		static std::uint32_t SunShadowStatic(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property = nullptr);
		/** @brief CS_DCLF_RESIDENT_PARITY: the synthetic pass built from scratch (no cache), for Coordinator's comparison. */
		static bool FreshSyntheticPass(const SceneCapture::LeafView& a_leaf, AccumulatedPass& a_out);
		/**
		 * @brief A membership pass's answer (T6b3e): the pass when ok, why there is none otherwise (LastSyntheticFail's, taken right
		 * after the synthetic pass on the thread that made it), and the derivation cache entry for the caller to store.
		 */
		struct Membership
		{
			bool ok = false;
			std::uint8_t fail = 0;
			MembershipDerived derived;
			AccumulatedPass pass;
		};
		/**
		 * @brief The pass Coordinator binds an object with by scene membership: a synthetic pass from the object alone, none where one
		 * cannot model it (decals, fading or translucent). T6b1b: from the geometry's records (a_leaf: the mirror's). T6b3e: pure (any
		 * thread with the frame's globals bound): the derived descriptor comes from a_cached when its witnesses match, and the entry to
		 * store comes back in the answer (Coordinator keeps it in the tracked entry: Tracked::membershipDerived).
		 */
		static Membership MembershipPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached);
		/** @brief MembershipPass for the geometry's layer (LayerPropertyOf: a_leaf's layer): the pass of its property with hint 12. */
		static Membership MembershipLayerPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached);
		/** @brief A fade root's fade-out distance for BuildDraws' fade test (kObjectFadeTest), 0 when it has none. */
		static float MembershipFadeDistance(const SceneCapture::NodeRecord& a_root) { return FadeDistanceOf(a_root); }
		/** @brief The frame globals a membership pass reads (the static sun bits, the fade distances): a change rebinds them all. */
		static std::uint32_t MembershipWitness(const FrameGlobals& a_globals);
		/**
		 * @brief This frame's main camera as BSFadeNode::OnVisible measures from it: its position and its LOD factor
		 * (NiCamera +0x184), for BuildDraws' fade test (kObjectFadeTest). Zero when the cut did not see a camera.
		 */
		std::array<float, 4> FadeEye() const { return fadeEye; }
		/**
		 * @brief T7f preparation, render thread, BeginSceneFrame (Main::Draw's camera setup done, before the list build and the culls): the
		 * frame's camera duties - FadeEye, T7b2's camera cut (IndirectDraws::NoteCameraCut), TreeHeightTest - from the main camera, so
		 * nothing DCLF needs depends on the engine's list processes running (they were PrepareFrame's, at the sun's call site).
		 */
		void BeginFrameCamera();
		/**
		 * @brief T7b2, the main thread at Main::Update's entry (DrawcallLimitFix's hook; the safe point: the last frame's job cycle was
		 * waited for, this one's not started, so no list cull, cell animation or animation job writes the flags): the stood-in roots'
		 * and their geometry's kAccumulated written from DCLF's visibility (IndirectDraws::WriteAccumulated), every reader seeing it this
		 * frame (skyrim-engine-notes.md, "T7b0"). a_running false: the writes are dropped.
		 */
		void AtUpdateEntry(bool a_running);
		/** @brief Whether the cut stands in for a_root now (admitted, unmixed: SyncFadeOwnership's set). Render (main) thread. */
		bool StoodIn(const void* a_root) const { return standInRoots.contains(a_root); }
		/**
		 * @brief T7b4, any thread (the animation gate's job threads): whether DCLF is a_node's one fade writer this frame - its root is
		 * in the set the main thread published at Main::Update's entry (T7f checklist items 4-5: every listed root, kFadeRootWritten in
		 * the accepted tables the entry published). The gate's fade update (FUN_1402cff60 -> FUN_14147a160) skips those, and so do the
		 * fade nodes' OnVisible (FadeOnVisible);
		 * FadeStateCS makes the same update on the GPU, and the write-back carries its milestones to the node.
		 */
		bool FadeWrittenByDclf(const void* a_node) const
		{
			const auto* tables = fadeWriterTables.load(std::memory_order_acquire);
			return tables && WrittenByTables(tables, a_node);
		}
		void NoteOnVisibleFadeSkipped() { onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed); }
		/** @brief a_node listed by a_tables (Coordinator::Tables) with kFadeRootWritten. Any thread; the tables are immutable. */
		static bool WrittenByTables(const void* a_tables, const void* a_node);
		/** @brief The single writer's counts since the last call: the animation gate's fade updates and the OnVisible fade steps skipped. */
		struct FadeWriterCounts
		{
			std::uint64_t gateSkipped = 0, onVisibleSkipped = 0;
			std::size_t roots = 0;  // the roots in the writer set
		};
		FadeWriterCounts TakeFadeWriterCounts();
		/**
		 * @brief T7f item 8, the census (render thread): whether a_geometry is under an entry the cut has as whole this frame (admitted,
		 * nothing engine-drawn, nothing to hand over), which no engine cull should reach.
		 */
		bool DrawsWhole(const RE::NiAVObject* a_geometry) const
		{
			if (!standInLive)
				return false;
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (const auto it = cut.eligible.find(object); it != cut.eligible.end())
					return it->second < cut.admitted.size() && cut.admitted[it->second] && !cut.mixed[it->second] && !cut.walk[it->second];
			return false;
		}
		/** @brief Whether a_node is an eligible entry's root of the cut's snapshot (the observers' descriptions). */
		bool IsEntry(const RE::NiAVObject* a_node) const { return cut.eligible.contains(a_node); }
		/**
		 * @brief The animation job's fade updates since the list jobs before this frame's (render thread, after the list jobs): the
		 * nodes and the inputs. FadeStateCS makes the same update on these roots before its cull test (Records.h, FadeFrame anim*).
		 */
		void TakeAnimatedBatch(std::vector<const void*>& a_nodes, AnimatedFadeInputs& a_inputs);
		/** @brief BSTreeNode::OnVisible's height test this frame, for BuildDraws (kObjectHeightTest): the base and the limit (+infinity: off). */
		std::array<float, 2> TreeHeightTest() const { return treeHeight; }
		/**
		 * @brief The main pass GetRenderPasses would register for the geometry this frame, built without it: the
		 * derived pass descriptor with the sun's bits, the batch list, the accumulation hint and the LOD row. False for
		 * what it does not model (translucent or fading objects, an unknown cascade test, a descriptor not derived).
		 */
		/** @brief T6b0: why this thread's last SyntheticPass gave none (kSyntheticFailNames' index; 0: it gave one). */
		static std::uint8_t LastSyntheticFail();
		static constexpr std::array<const char*, 6> kSyntheticFailNames{ "-", "not derived", "not lighting", "-", "translucent layer", "translucent" };
		static bool SyntheticPass(const SceneCapture::LeafView& a_leaf, std::uint32_t a_derivedPass, AccumulatedPass& a_out,
			const SceneCapture::PropertyRecord* a_layer = nullptr);

		/**
		 * @brief Render thread (Coordinator, a member's residency dropped): the geometry's entry is walked again (Cut::walk) until
		 * the next frame says whether it still needs it.
		 */
		void NoteMemberLost(const RE::BSGeometry* a_geometry);
		/** @brief Render thread: every residency ended (Coordinator::EndAllResidency): every entry walked until the next snapshot. */
		void NoteAllMembersLost();
		/**
		 * @brief The frame's MembershipWitness, sampled once (Coordinator::CommitSet, at the scene phase): the commit keeps every
		 * resident out of the set on the frame it changes, and BindByMembership binds them all again from the same sample.
		 */
		std::uint32_t SampleMembershipWitness();
		/**
		 * @brief Render thread, Coordinator::CommitSet: the geometries that joined the set's main phase and those that left it. An
		 * entry with one that joined is checked for admission before the list jobs; one with a member that left is walked, so the
		 * stand-in hands that member to the engine's registration.
		 */
		void NoteSetChanges(const std::vector<const RE::BSGeometry*>& a_joined, const std::vector<const RE::BSGeometry*>& a_left);
		/** @brief Render thread, after the registration jobs: the hook point the light mask observer checks the owned members in view at. */
		void CheckLightMasks();
		/**
		 * @brief Before the list jobs, render thread: admission by the set. An entry is admitted, and left out of the engine's cull,
		 * once every DCLF member it shows is in the frame's set, in view or not. Checked when one of its members joins
		 * (NoteSetChanges), and once for every entry of a new snapshot. Only against a current snapshot: a stale one's geometries
		 * may have been released since (a cell unloading), and its successor queues its entries again.
		 */
		/**
		 * @brief The cut brought up to a_candidates at the entries a_changed names (ChangedEntries): each let go of and planned again
		 * (PlanOf, its objects, its switches, admission, the walk); every other entry stands.
		 */
		void SyncCut(const std::shared_ptr<const SunCandidates>& a_candidates, std::span<const std::uint32_t> a_changed);
		/** @brief The members' and switches' pools gathered again, without the ranges the entries planned again let go of. */
		void CompactCut();
		std::size_t cutGarbage = 0;  // members of let-go ranges still in the pool
		void RunAdmission();

		/**
		 * @brief Any thread from the full-frustum cull to the frame's end (a list process's AppendVirtual, the shadow lights' registrations): whether the geometry is DCLF's to draw, so the engine's cull does not
		 * hand it to the registration (leaf exclusion): in the frame's set, with the main phase. Wherever the engine still culls
		 * (an entry not stood in for: actors, a LOD cross-fade), its members are left out here. An optimisation only: the
		 * registration withholds a member's passes whoever's cull reaches it (PassCapture).
		 */
		bool Owned(const RE::BSGeometry& a_geometry) const;

		/** @brief The cut's counters since the report (render thread). */
		struct CutCounters
		{
			std::uint64_t frames = 0, appliedFrames = 0;
			std::uint64_t skippedStale = 0, skippedPreconditions = 0;
			std::uint64_t entriesPlanned = 0, compactions = 0;  // entries planned again as their snapshot rows moved (SyncCut), pool gatherings
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0;
			std::uint64_t notSettled = 0, notAdmitted = 0, admittedNow = 0;
			std::uint64_t members = 0, unbound = 0, hiddenSkipped = 0;
			std::uint64_t undrawable = 0;    // bound members in view handed to the engine: not in the set (not ready, rebinding)
			std::uint64_t walked = 0;        // entries the stand-in walked (Cut::walk, or every one under Policy::WalkEveryEntry)
			std::uint64_t excluded = 0;      // owned geometries the engine's own cull reached and did not register (leaf exclusion)
			std::uint64_t mixed = 0;         // admitted entries with engine-drawn parts, culled by the engine
			std::uint64_t engineMembers = 0;  // the engine's members in view, handed to its registration
			std::uint64_t switchStale = 0;    // entries the engine culled this frame because a switch's selected child was out of date
			std::uint64_t unselected = 0;     // members under an unselected switch child
			std::uint64_t liveAll = 0;        // frames memberLive was read from every switch (a new snapshot, a resync)
			std::uint64_t liveEntries = 0;    // entries whose memberLive a switch event refreshed
			std::int64_t prepareTicks = 0, afterTicks = 0;
		};
		CutCounters TakeCutCounters() { return std::exchange(cutStats, {}); }
		/** @brief The scene lists' counters since the last call (render thread, the report; the build's job wrote some before Finish). */
		struct ListCounters
		{
			std::uint64_t frames = 0, published = 0, built = 0, dryRuns = 0;
			std::uint64_t notToggled = 0, notCurrent = 0, noExclusion = 0, noOcclusion = 0, decalOrder = 0;
			std::uint64_t restored = 0, restoreDetached = 0, unexcluded = 0, lostWhileOut = 0, engineFrames = 0, notKeepable = 0;
			std::array<std::uint64_t, 5> notKeepableBy{};
			std::uint64_t filtered = 0, entries = 0, removed = 0, kept = 0, rebuilt = 0, lateRebuilds = 0, rebuildTicks = 0, rebuildEntries = 0;
			std::uint64_t rebuildStructure = 0, rebuildFilter = 0, rebuildWitness = 0, rebuildDirty = 0;
			std::array<std::uint64_t, 4> structureBy{};
			std::uint64_t eventsApplied = 0, eventsAdded = 0, eventsRemoved = 0, eventsRebuilt = 0;
		};
		ListCounters TakeListCounters();
		/** @brief At Present, render thread. */
		void EndFrame()
		{
			LocalLightCull::EndFrame();
			listFilter.store(nullptr, std::memory_order_release);
			listMode.store(ListMode::Engine, std::memory_order_release);
			// Roots the build's job let go of: released here, where the engine unloads (one may hold a detached cell's last
			// reference).
			listGraveyard.clear();
		}

		/**
		 * @brief Render thread, at the scene phase's end (before Main::Draw queues DrawWorld_BuildSceneLists): this frame's
		 * scene lists (Engine/SceneLists.cpp; drawcall-limit-fix.md, "The scene lists without DCLF's roots").
		 *
		 * The roots they leave out: an entry admitted with nothing for the registration (no engine-drawn
		 * part, every member bound: Cut::walk clear) that the exclusion this frame's full-frustum cull applies takes out of
		 * the sun's cascades, while DCLF draws both occlusion maps. Then the main camera's list jobs (the stand-in's lookup)
		 * and the sun's full-frustum cull never see it.
		 *
		 * How they are made: the object root's part (every cell's roots, and its two whole entries) kept from the last frame
		 * where nothing it reads moved (ListMode::Keep), built by DCLF where something did (Rebuild), or the engine's own build,
		 * filtered, in an interior or on a frame Policy::FilterSceneLists keeps whole (Engine). In the first two the engine's build still runs and adds what
		 * follows the camera (a portal graph's rooms and occlusion planes) and the extra list, but skips the object root
		 * (ObjectRootAsNode). Kept lists hold hidden roots too: every scene list's cull skips them (the list cull's own flag,
		 * CullList), and a hidden cell or category node, a structure change under them (NoteListStructure) or a new filter
		 * rebuilds them.
		 */
		void PublishListFilter();
		/** @brief This frame's scene lists leave DCLF's roots out (render thread, after BuildSceneLists' job). */
		bool ListsFiltered() const { return listsFiltered.load(std::memory_order_acquire); }
		/**
		 * @brief Render thread: an occlusion map the engine draws this frame (DCLF's not ready), whose Precipitation::SetupMask
		 * culls the lists: the roots this frame's filter left out are put back into them first (one map that frame needs
		 * them; nothing reads the lists between the list jobs and ClearLists but the occlusion maps).
		 */
		void RestoreSceneLists();
		/**
		 * @brief Any thread, from the attach and detach detours (SceneTracker): a_child attached to a_parent (after the attach)
		 * or about to be detached from it. A root under a category node is applied to the kept lists by the next build
		 * (listEvents); anything higher (a category node under a cell, a cell under the object root) rebuilds them.
		 */
		void NoteListStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached);
		/** @brief Render thread, Coordinator's hidden events: a hidden bit written on a structure node rebuilds the kept lists. */
		void NoteHiddenKey(const void* a_key)
		{
			if (listStructural.contains(a_key)) {
				listStructure.fetch_add(1, std::memory_order_relaxed);
				listStats.structureBy[3].fetch_add(1, std::memory_order_relaxed);
			}
		}

	private:
		struct Cut;
		/** @brief The switch node's selected child index, read at the engine's offset (Coordinator::ReadSwitch; CommonLib's is not AE's). */
		static std::int32_t SwitchIndex(const RE::NiSwitchNode* a_switch);
		/** @brief Every switch above the member selects its path, read from the switches (the events' input, and their check). */
		bool PathSelected(const auto& a_member) const
		{
			for (std::uint32_t w = a_member.switchBegin; w < a_member.switchEnd; ++w)
				if (SwitchIndex(cut.switchPaths[w].first) != cut.switchPaths[w].second)
					return false;
			return true;
		}
		/** @brief Whether the cull would reach member a_m: not app-culled up to the root, and selected by every switch above it. */
		bool MemberShown(std::uint32_t a_m, const RE::NiAVObject* a_root) const
		{
			const auto& member = cut.members[a_m];
			if (!(cut.liveEvents ? cut.memberLive[a_m] != 0 : PathSelected(member)))
				return false;
			for (const RE::NiAVObject* object = member.geometry; object; object = object == a_root ? nullptr : object->parent)
				if (object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					return false;
			return true;
		}
		/** @brief A set hash of entry a_e's DCLF members (not the engine's) in the current snapshot. */
		std::uint64_t MemberSignature(std::uint32_t a_e) const;
		/** @brief Render thread, before the list jobs: memberLive of entry a_e's members, from the switches. */
		void RefreshLive(std::uint32_t a_e);
		/**
		 * @brief The fade root's fade-out distance for BuildDraws (kObjectFadeTest): where FUN_14147b110's fade value falls
		 * below BSFadeNode::OnVisible's fade-out threshold. > 0: against the distance times the camera's LOD factor; < 0:
		 * against the distance times a constant, folded in; 0: the root never fades out by distance.
		 */
		static float FadeDistanceOf(const SceneCapture::NodeRecord& a_root);
		PrimaryCull() = default;

		struct Hooks;
		friend struct Hooks;
		friend struct Diagnostics::PrimaryCullView;

		/** @brief After the full-frustum cull, render thread: the stand-in's frame. */
		void AfterFullFrustum();
		/** @brief The scene lists' hooks (SceneLists.cpp). */
		struct ListHooks;
		friend struct ListHooks;
		void InstallSceneLists();
		enum class ListMode : std::uint8_t
		{
			Engine,   // the engine builds and ClearLists clears them; the filter takes DCLF's roots out after the build
			Rebuild,  // DCLF builds them: the structure, a hidden structure node, the filter or the build's globals moved
			Keep,     // as the last frame left them
		};
		/** @brief The roots the scene lists leave out (PublishListFilter), for one snapshot. Immutable once published. */
		struct ListFilter
		{
			std::shared_ptr<const SunCandidates> candidates;
			ankerl::unordered_dense::set<const RE::NiAVObject*> roots;
		};
		/** @brief The current filter (render thread): listFilterBuilt again while no entry's verdict moved. */
		std::shared_ptr<const ListFilter> CurrentListFilter();
		/**
		 * @brief The engine's build walks the object root alike every frame in the exterior branch (an interior's follows the
		 * camera): its preconditions, and the globals it reads, as a witness.
		 */
		bool ListsKeepable(std::uint64_t& a_witness);
		/** @brief The build's job: DCLF's build of the object root's part, less the filter's roots, hidden roots included. */
		void RebuildSceneLists(const ListFilter* a_filter);
		/**
		 * @brief The object root's roots, as RebuildSceneLists and ListRoots enumerate them: a_list(root, a_first) (a_first:
		 * object root children 0 and 1, which go to list 0), a_structural(node) for the nodes whose hidden bit or children decide
		 * which roots those are. Hidden roots are included.
		 */
		template <class List, class Structural>
		static void EnumerateSceneLists(const RE::NiAVObject* a_objectRoot, bool a_skipCategory2, List&& a_list, Structural&& a_structural);
		/** @brief The build's job, Rebuild and Keep: what the engine's build does on the object root besides the lists (kAccumulated). */
		void ListUpkeep();
		/**
		 * @brief DCLF's enumeration of the object root's roots (hidden ones included) into a_roots, and the object root (a_objectRoot): the
		 * reference the scene lists' observer compares the engine's lists with. False without a scene or TES. The build's job.
		 */
		static bool ListRoots(const RE::NiAVObject*& a_objectRoot, std::vector<const RE::NiAVObject*>& a_roots);
		/** @brief DrawWorld_BuildSceneLists' job, after the engine's build: the published roots taken out of the lists. */
		void FilterSceneLists();
		/** @brief Whether a_list is one of the scene lists (a_extra: or the extra list). */
		static bool IsSceneList(const void* a_list, bool a_extra);

		std::atomic<ListMode> listMode{ ListMode::Engine };          // this frame's, until Present; read by the list jobs' hooks
		std::atomic<std::shared_ptr<const ListFilter>> listFilter;  // this frame's, until Present; read by the build's job
		std::shared_ptr<const ListFilter> listFilterBuilt;          // the last built, published again while nothing moved
		std::shared_ptr<const ListFilter> listFilterKept;           // the one the kept lists leave out (the build's job writes)
		std::vector<std::uint8_t> listRemovable;                    // per entry index: in listFilterBuilt
		std::uint64_t filterCutVersion = ~0ull, filterExclusionVersion = ~0ull;  // what listFilterBuilt was made from
		std::uint64_t cutVersion = 0;                               // bumped when an entry's admission or walk changes
		std::vector<std::uint8_t> filterScratch;                    // the build's job
		std::atomic<bool> listsFiltered{ false };
		std::atomic<bool> listsDirty{ true };                       // the lists are not DCLF's build (the engine's, or restored)
		std::atomic<std::uint32_t> listStructure{ 0 };              // NoteListStructure, NoteHiddenKey
		std::uint32_t listStructureBuilt = ~0u;                     // the build's job writes, the render thread reads after Finish
		std::uint64_t listWitness = 0, listWitnessBuilt = ~0ull;
		// The structure nodes of the last build (the build's job writes, the render thread reads between frames), and the
		// pointers the detours compare with (any thread).
		ankerl::unordered_dense::set<const void*> listStructural;
		std::atomic<const RE::NiAVObject*> listObjectRoot{ nullptr };
		std::atomic<const RE::NiAVObject*> listWholeEntries[2]{ nullptr, nullptr };
		// Each list's first entry: an empty node, never hidden, so each cull's Process2 sets its frustum up.
		std::array<RE::NiPointer<RE::NiNode>, 16> listSentinels;
		std::array<std::uint32_t, 16> listKeptSize{};  // each list's kept part (the build's job); the engine's per-frame entries follow
		bool skipObjectRoot = false;                   // the build's job: ObjectRootAsNode returns null
		/** @brief A root attached under or detached from a category node, in event order. */
		struct ListEvent
		{
			RE::NiPointer<RE::NiAVObject> held;  // an attach's root, kept alive until applied (then released at Present)
			const RE::NiAVObject* child = nullptr;
			const RE::NiAVObject* parent = nullptr;
			bool attached = false;
		};
		EventQueue<ListEvent> listEvents;
		/** @brief The build's job, Keep: the queued roots onto the kept lists. False when one needs a rebuild. */
		bool ApplyListEvents(const ListFilter* a_filter);
		/** @brief The build's job, Rebuild and Engine: the queued events, dropped (the build reads the structure as it is now). */
		void DropListEvents();
		// The build's job: the kept lists' listed category nodes (their child index under the cell), and each root's place.
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint16_t> listCategories;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::pair<std::uint32_t, std::uint32_t>> listPositions;
		std::uint32_t listNext = 0;  // the round robin's next list
		std::vector<RE::NiPointer<RE::NiAVObject>> listGraveyard;  // the build's job fills, Present releases
		/** @brief The build's job: the scene lists emptied, their references kept for Present (listGraveyard). */
		void BuryLists();
		bool listsKeepInstalled = false;               // the object root's call site was found and patched
		struct ListStats
		{
			std::uint64_t frames = 0, published = 0, built = 0, dryRuns = 0;
			std::uint64_t notToggled = 0, notCurrent = 0, noExclusion = 0, noOcclusion = 0, decalOrder = 0;
			std::uint64_t restored = 0;           // frames an occlusion map the engine drew put the left-out roots back
			std::uint64_t restoreDetached = 0;    // roots of those the engine had detached since the snapshot, left out
			std::uint64_t unexcluded = 0;         // must be 0: filtered lists, and the full-frustum cull applied no exclusion
			std::uint64_t lostWhileOut = 0;       // a member's binding lost while its root was out of the lists
			std::uint64_t engineFrames = 0, notKeepable = 0;  // the engine built them, and of those for want of the exterior branch
			// Why not keepable: no hidden events, no scene or processes, not unbound space (or no entry), an interior, too few scene children.
			std::array<std::uint64_t, 5> notKeepableBy{};
			std::atomic<std::uint64_t> filtered{ 0 }, entries{ 0 }, removed{ 0 };  // the build's job
			std::atomic<std::uint64_t> kept{ 0 }, rebuilt{ 0 }, lateRebuilds{ 0 }, rebuildTicks{ 0 }, rebuildEntries{ 0 };
			std::uint64_t rebuildStructure = 0, rebuildFilter = 0, rebuildWitness = 0, rebuildDirty = 0;
			// The events that rebuild them: a child of the object root, of a cell (a category node), of a category node (a root)
			// attached or detached; a structure node's hidden bit written.
			std::array<std::atomic<std::uint64_t>, 4> structureBy{};
			std::atomic<std::uint64_t> eventsApplied{ 0 }, eventsAdded{ 0 }, eventsRemoved{ 0 }, eventsRebuilt{ 0 };  // the build's job
		};
		ListStats listStats;
		/** @brief After the list jobs' Finish, render thread. */
		void AfterListJobs();

		bool installed = false;

		/** @brief Per candidate entry, what the snapshot and a walk of its subtree say (once per generation). */
		enum class EntryPlan : std::uint8_t
		{
			Plain,     // no node under it needs OnVisible, the root included
			FadeRoot,  // the root is a BSFadeNode (exactly): DCLF runs its OnVisible fade update (ServiceFade); the rest plain
			LeafRoot,  // ... a BSLeafAnimNode: its LOD step, then the fade update
			TreeRoot,  // ... a BSTreeNode: its height test, the leaf node's update, its LOD fix-up (ServiceTreeState)
			Rejected,
		};
		/** @brief The entry's plan; its members are appended to cut.members (not for a rejected entry). */
		EntryPlan PlanOf(std::uint32_t a_entry, const RE::NiAVObject* a_root);
		std::uintptr_t geometryOnVisible = 0;  // BSGeometry's OnVisible (vtable slot 0x34): a member's must be it
		/**
		 * @brief T7f item 8: the OnVisibles (slot 0x34) whose cull side effects DCLF makes or needs none of
		 * (skyrim-engine-notes.md, "T7f item 8"), read from the classes' vtables at Install. Nodes: NiNode's (the slot, and the engine's
		 * own, which every subclass that keeps it calls), the multibound's frustum cache, a switch's selected child (followed by event), a
		 * billboard's turn (DCLF turns its billboards from the unturned world), an ordered node's alpha group number (DCLF orders its
		 * forward members itself). Geometry besides BSGeometry's: three that append under a condition.
		 */
		struct WideOnVisible
		{
			std::uintptr_t node = 0, engineNode = 0, multiBound = 0, switchNode = 0, billboard = 0, ordered = 0, dynamic = 0, subIndex = 0, segmented = 0;
		};
		WideOnVisible wideOnVisible;
		/** @brief BSTreeNode::OnVisible's height test: true when the tree is above the limit (not drawn, not updated). */
		static bool TreeAboveLimit(const RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process);
		/** @brief Render thread, after the full-frustum cull: this frame's preconditions, and a new snapshot's plans. */
		void PrepareFrame();
		/** @brief The list process's index among the scene lists', or -1 (any other process sharing the vtable). */
		int SlotOf(const RE::NiCullingProcess* a_process) const;
		/**
		 * @brief A list job's Process1 on an object, job thread: when the object is an eligible entry, what the cull would
		 * have done, done without traversing it (the class comment). False: the engine's Process1 runs as usual.
		 */
		bool StandIn(int a_slot, RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg);

		/**
		 * @brief The snapshot's eligible entries, as the list jobs read them. Rebuilt on the render thread before the jobs
		 * (a new snapshot) and changed only between them (admission), so the jobs read it without a lock.
		 */
		struct Cut
		{
			std::shared_ptr<const SunCandidates> candidates;  // the snapshot the plans are for
			std::vector<EntryPlan> plans;                     // per candidate entry index
			/** @brief A geometry under an eligible entry: DCLF's (a synthetic pass) or the engine's (handed to its registration). */
			struct Member
			{
				const RE::BSGeometry* geometry = nullptr;
				bool engine = false;
				// The switch nodes above it, as (switch, the child index on its path) in switchPaths: drawn only while each
				// selects that child, as NiSwitchNode::OnVisible culls only its selected child.
				std::uint32_t switchBegin = 0, switchEnd = 0;
			};
			std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> switchPaths;
			// Per candidate entry index: its switch nodes in switches, and its geometries in members in the cull's order. The pools
			// are appended to as entries are planned again (SyncCut), and gathered again when their garbage outgrows them (CompactCut).
			std::vector<std::uint32_t> switchBegin, switchEnd;
			std::vector<const RE::NiSwitchNode*> switches;
			std::vector<std::uint32_t> memberBegin, memberEnd;
			std::vector<Member> members;
			std::vector<std::int32_t> memberObject;    // per member: its object index in the tables (-1: none)
			// Per member: every switch above it selects its path. Read from the switches for a new snapshot, then kept by
			// the switch events (Coordinator::TakeSwitchChanges) for the entries they name (switchEntry).
			std::vector<std::uint8_t> memberLive;
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchEntry;  // switch node -> entry index
			bool liveEvents = false;  // memberLive is in force (Scene::SwitchEventsLive); else the jobs read the switches
			std::vector<const RE::NiAVObject*> roots;         // per candidate entry index
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> eligible;  // root -> entry index, plan not Rejected
			std::vector<std::uint8_t> admitted;               // per entry index: DCLF has drawn all of it (see the class)
			std::vector<std::uint8_t> mixed;                  // per entry index: some member is the engine's (drawn from the node)
			// Per entry index: the stand-in walks it (its bound test and its members): it has engine-drawn members to hand to
			// the registration, or a tracked member not in the set, shown or not (handed over whenever the walk finds it shown, until
			// it is in the set: the engine shows one with no event that reaches the decision first). Any other admitted entry is left
			// to the GPU whole. Kept by events (RefreshWalk): a new snapshot, a member's binding lost (NoteMemberLost, then
			// the next frame), a member newly drawn.
			std::vector<std::uint8_t> walk;
			// T7f item 8, the census's names: per entry index, why its plan is Rejected (the RTTI of the node or geometry whose OnVisible
			// the cut cannot stand for, or no DCLF geometry; null otherwise), and the first engine-drawn geometry of a mixed one.
			std::vector<const char*> rejectReason;
			std::vector<std::string> mixedBy;
			// The same by node, kept across snapshots with the set of DCLF members it was admitted with (MemberSignature): a root
			// whose members changed (a decal attached, a part swapped) is admitted again only once the new ones are drawn.
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> admittedRoots;
			std::vector<std::uint32_t> pendingAdmission;       // entries to check for admission (RunAdmission): a new snapshot's, a member joined, a member not tracked yet
			std::array<const RE::NiCullingProcess*, 16> processes{};  // the list processes this frame
			std::uint32_t processCount = 0;
		};
		Cut cut;
		std::atomic<bool> frameLive{ false };  // set before the list jobs when the cut applies this frame, cleared after them
		bool gpuSunFrame = false;              // the cut applies this frame (PrepareFrame), until the next full-frustum cull

		/** @brief One list job's output and counters: written by its job thread only, read after Finish. */
		struct JobOut
		{
			std::vector<const RE::BSGeometry*> visible;
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0, notSettled = 0, notAdmitted = 0, walked = 0;
			std::uint64_t hidden = 0, engineMembers = 0, switchStale = 0, unselected = 0;
			std::uint64_t unbound = 0;                      // DCLF geometries in view not bound yet, handed to the engine
			std::uint64_t undrawable = 0;                   // bound members in view the colour build cannot draw, handed to the engine
			std::uint64_t excluded = 0;                     // owned geometries the engine's cull reached, not handed to its registration
			std::uint64_t mixed = 0;                        // admitted entries with engine-drawn parts, culled by the engine
		};
		std::array<JobOut, 16> jobOut;

		CutCounters cutStats;
		std::vector<const RE::BSGeometry*> frameVisible;  // this frame's members in view under stood-in entries
		std::vector<const RE::NiAVObject*> switchChanges;  // scratch: Coordinator::TakeSwitchChanges
		std::array<float, 4> fadeEye{};  // FadeEye, captured in BeginFrameCamera
		const void* frameCamera = nullptr;  // BeginFrameCamera's camera
		std::shared_ptr<const SetSnapshot> frameSet;  // the frame's set, for Owned (PrepareFrame)
		std::array<float, 2> treeHeight{ 0.0f, std::numeric_limits<float>::infinity() };  // TreeHeightTest, captured in BeginFrameCamera
		// The animation job's fade updates (FUN_1402cff60's call of FUN_14147a160, Hooks::AnimatedFade): the nodes since the last
		// list jobs, with the inputs they were made with; at the list jobs' end they become the batch FadeStateCS applies before this
		// frame's cull test (TakeAnimatedBatch).
		std::mutex animatedMutex;
		std::vector<const void*> animatedNodes, animatedBatch;
		AnimatedFadeInputs animatedInputs{}, batchInputs{};
		void NoteAnimatedFade(const void* a_node, const float* a_camera);
		bool standInLive = false;        // this frame's snapshot is current: the stand-in runs
		bool liveStale = true;                             // a frame skipped the switch changes: memberLive is read again
		// The fade roots DCLF services (SyncFadeOwnership: the admitted entries' with a fade plan), and whether a frame since
		// the last applied one left every entry to the engine (their nodes are then seeded again).
		std::vector<const RE::NiAVObject*> ownedFadeRoots;
		// T7b2: the roots the cut stands in for (ownedFadeRoots without engine-drawn parts, and since T7f item 8 the plain entries without
		// them), whose kAccumulated no engine cull writes (AtUpdateEntry), and the stood-in fade roots among them.
		ankerl::unordered_dense::set<const void*> standInRoots;
		std::vector<const void*> standInList;
		// T7b4, T7f checklist items 4-5: the roots whose fade DCLF alone writes (FadeWrittenByDclf) are the accepted tables the entry found
		// (Coordinator::Tables, immutable; their rows' kFadeRootWritten), held by the main thread until the next entry (the last cycle was
		// waited for: no OnVisible or gate reads them). Lock-free; the gate's and OnVisibles' skips since the last report.
		std::atomic<const void*> fadeWriterTables{ nullptr };
		std::shared_ptr<const void> fadeWriterTablesHeld;
		std::atomic<std::uint64_t> onVisibleFadeSkipped{ 0 };
		/** @brief Main::Update's entry: the accepted tables published as the writer set (a_running false: none). */
		void PublishWriterTables(bool a_running);
		std::atomic<std::uint64_t> gateFadeSkipped{ 0 };
		// T7b2's camera cut test (BeginFrameCamera, the main camera): the last frame's position and rotation.
		std::array<float, 12> cutCamera{};
		bool cutCameraKnown = false;
		/** @brief Coordinator::SetFadeRootsOwned from the current snapshot's admitted fade entries. */
		void SyncFadeOwnership();
		/**
		 * @brief The cut planned whole for its snapshot (every entry planned again from nothing), the cut as it stands left as it was: the
		 * reference the cut observer compares the kept cut with. Render thread.
		 */
		Cut PlanWholeCut();
		/** @brief Cut::walk of entry a_e, from its members now (render thread, between the jobs). */
		void RefreshWalk(std::uint32_t a_e);
		std::vector<std::uint32_t> walkRefresh;  // entries whose member lost its binding: refreshed at the next PrepareFrame
		bool walkEverything = false;             // every entry walked this frame (Policy::WalkEveryEntry)
		// The membership witness the frame's commit sampled (SampleMembershipWitness), and the frame it did.
		std::uint32_t sampledWitness = 0;
		std::uint32_t sampledWitnessFrame = ~0u;
		/** @brief List jobs and render thread: a member of the frame's set with the main phase (Coordinator::SetPhasesOf). */
		bool MemberDrawable(std::int32_t a_object) const;
	};
}
