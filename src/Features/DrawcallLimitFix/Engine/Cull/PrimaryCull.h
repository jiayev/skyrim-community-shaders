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
	 * @brief The main camera's cull, and taking DCLF's objects out of it (dclf-gpu-driven-frame.md). AE only. Owns the scene
	 * lists' build (SceneLists.cpp), the list jobs' stand-in, the fade-writer set and the camera values BuildDraws reads.
	 *
	 * The scene lists (`DAT_14338c870`, `DAT_14338c868` of them) are BSTArray<NiPointer<NiAVObject>> that
	 * DrawWorld_BuildSceneLists fills with reference roots, round robin. NiCamera::CalculateAndDrawShadowCasterLights hands them
	 * to the sun's full-frustum cull, then queues one list job per list and waits (JobList::Finish); the registration jobs walk
	 * the output afterwards. Between the full-frustum cull and that Finish only the list jobs read the lists.
	 *
	 * The stand-in (toggle `excludePrimaryEntries`, CS_DCLF_PRIMARY_EXCLUDE) hooks the list processes' Process1 (vtable slot 0x16,
	 * shared with every other list process, so filtered by process) on the job's thread. For an admitted entry in view it leaves
	 * the root's OnVisible update to FadeStateCS (fade, leaf LOD step, tree LOD fix-up; written back) and tests a tree's height,
	 * skips its geometries in the frame's set (the GPU draws them), and hands every other geometry in view to the registration in
	 * traversal order (AppendVirtual): decals, effects, blended objects, DCLF geometry not in the set. Switches are followed by
	 * event (memberLive, from Coordinator's switch events). An entry with engine-drawn parts (Cut::mixed) runs the engine's
	 * Process1 whole; the AppendVirtual hook keeps DCLF's members from the registration. Eligible: every node and geometry under
	 * it has an OnVisible DCLF stands for (PlanOf, WideOnVisible) and at least one geometry is DCLF's; admitted once all those it
	 * shows are in the set (RunAdmission).
	 */
	class PrimaryCull
	{
	public:
		static PrimaryCull& Get();

		/** @brief AE: the call sites in CalculateAndDrawShadowCasterLights, verified before patching. */
		void Install();
		bool Installed() const { return installed; }

		/**
		 * @brief The ShadowDir and DefShadow bits (13, 14) a geometry whose bound meets a cascade takes, from the object and frame
		 * globals alone; the GPU makes the cascade test (kObjectSunTest).
		 */
		static std::uint32_t SunShadowStatic(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property = nullptr);
		/** @brief CS_DCLF_RESIDENT_PARITY: the synthetic pass built from scratch (no cache), for the residents' observer. */
		static bool FreshSyntheticPass(const SceneCapture::LeafView& a_leaf, AccumulatedPass& a_out);
		/** @brief A membership pass: the pass when ok, else why not (LastSyntheticFail), and the derivation cache entry to store. */
		struct Membership
		{
			bool ok = false;
			std::uint8_t fail = 0;
			MembershipDerived derived;
			AccumulatedPass pass;
		};
		/**
		 * @brief The synthetic pass Coordinator binds an object with by membership, from the mirror's records. Pure (any thread with
		 * the frame's globals bound); reuses a_cached when its witnesses match (kept in Tracked::membershipDerived).
		 */
		static Membership MembershipPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached);
		/** @brief MembershipPass for a_leaf's layer (LayerPropertyOf): its property's pass with hint 12. */
		static Membership MembershipLayerPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached);
		/** @brief A fade root's fade-out distance for kObjectFadeTest, 0 when it has none. */
		static float MembershipFadeDistance(const SceneCapture::NodeRecord& a_root) { return FadeDistanceOf(a_root); }
		/** @brief The frame globals a membership pass reads (static sun bits, fade distances): a change rebinds every object. */
		static std::uint32_t MembershipWitness(const FrameGlobals& a_globals);
		/**
		 * @brief The main camera as BSFadeNode::OnVisible measures from it: position and LOD factor (NiCamera +0x184), for
		 * kObjectFadeTest. Zero when no camera was seen.
		 */
		std::array<float, 4> FadeEye() const { return fadeEye; }
		/**
		 * @brief Render thread, FrameDriver at BeginSceneFrame (after Main::Draw's camera setup, before the list build and culls):
		 * FadeEye, the camera cut (IndirectDraws::NoteCameraCut) and TreeHeightTest from the main camera, so none of them depends on
		 * the engine's list processes running.
		 */
		void BeginFrameCamera();
		/**
		 * @brief Main thread at Main::Update's entry, the safe point (last frame's job cycle waited for, this one's not started, so no
		 * list cull, cell animation or animation job writes the flags): writes the stood-in roots' and their geometry's kAccumulated
		 * from DCLF's visibility (IndirectDraws::WriteAccumulated; skyrim-engine-notes.md, "who sets kAccumulated"), then publishes
		 * the fade-writer set. a_running false drops the writes.
		 */
		void AtUpdateEntry(bool a_running);
		/** @brief Whether the cut stands in for a_root (admitted, unmixed). Render (main) thread. */
		bool StoodIn(const void* a_root) const { return standInRoots.contains(a_root); }
		/**
		 * @brief Any thread: whether DCLF is a_node's one fade writer this frame (kFadeRootWritten in the tables published at
		 * Main::Update's entry). The gate's fade update (FUN_1402cff60 -> FUN_14147a160) and FadeOnVisible skip their stores on
		 * those; FadeStateCS makes the update on the GPU and the write-back carries it to the node.
		 */
		bool FadeWrittenByDclf(const void* a_node) const
		{
			const auto* tables = fadeWriterTables.load(std::memory_order_acquire);
			return tables && WrittenByTables(tables, a_node);
		}
		void NoteOnVisibleFadeSkipped() { onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed); }
		/** @brief a_node listed by a_tables (SceneTables) with kFadeRootWritten. Any thread; the tables are immutable. */
		static bool WrittenByTables(const void* a_tables, const void* a_node);
		/** @brief Since the last call: the gate's fade updates and OnVisible fade steps skipped. */
		struct FadeWriterCounts
		{
			std::uint64_t gateSkipped = 0, onVisibleSkipped = 0;
			std::size_t roots = 0;  // the roots in the writer set
		};
		FadeWriterCounts TakeFadeWriterCounts();
		/** @brief Render thread (census): a_geometry is under an entry the cut has whole (admitted, unmixed, not walked). */
		bool DrawsWhole(const RE::NiAVObject* a_geometry) const
		{
			if (!standInLive)
				return false;
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (const auto it = cut.eligible.find(object); it != cut.eligible.end())
					return it->second < cut.admitted.size() && cut.admitted[it->second] && !cut.mixed[it->second] && !cut.walk[it->second];
			return false;
		}
		/** @brief Whether a_node is an eligible entry's root in the cut's snapshot. */
		bool IsEntry(const RE::NiAVObject* a_node) const { return cut.eligible.contains(a_node); }
		/**
		 * @brief Render thread, after the list jobs: the animation job's fade updates since the previous list jobs (nodes and
		 * inputs), which FadeStateCS repeats before its cull test (Records.h, FadeFrame anim*).
		 */
		void TakeAnimatedBatch(std::vector<const void*>& a_nodes, AnimatedFadeInputs& a_inputs);
		/** @brief BSTreeNode::OnVisible's height test for kObjectHeightTest: base and limit (+infinity: off). */
		std::array<float, 2> TreeHeightTest() const { return treeHeight; }
		/** @brief Why this thread's last SyntheticPass failed (kSyntheticFailNames index; 0: it succeeded). */
		static std::uint8_t LastSyntheticFail();
		static constexpr std::array<const char*, 6> kSyntheticFailNames{ "-", "not derived", "not lighting", "-", "translucent layer", "translucent" };
		/**
		 * @brief The main pass GetRenderPasses would register (descriptor + sun bits, batch list, accumulation hint, LOD row). False
		 * when not modelled: descriptor not derived, translucent layer, translucent without the forward epoch.
		 */
		static bool SyntheticPass(const SceneCapture::LeafView& a_leaf, std::uint32_t a_derivedPass, AccumulatedPass& a_out,
			const SceneCapture::PropertyRecord* a_layer = nullptr);

		/** @brief Render thread (Coordinator's residency drop): the geometry's entry is walked (Cut::walk) until the next frame. */
		void NoteMemberLost(const RE::BSGeometry* a_geometry);
		/** @brief Render thread (Coordinator::EndAllResidency): every entry walked until the next snapshot. */
		void NoteAllMembersLost();
		/** @brief Render thread: the frame's MembershipWitness, sampled once so BindByMembership and the frame agree. */
		std::uint32_t SampleMembershipWitness();
		/** @brief Render thread, on install: a joined member's entry is checked for admission, a left member's entry walked. */
		void NoteSetChanges(const std::vector<const RE::BSGeometry*>& a_joined, const std::vector<const RE::BSGeometry*>& a_left);
		/** @brief Brings the cut up to a_candidates: the entries a_changed names are let go of and planned again; the rest stand. */
		void SyncCut(const std::shared_ptr<const SunCandidates>& a_candidates, std::span<const std::uint32_t> a_changed);
		/** @brief Repacks the member and switch pools without the let-go ranges. */
		void CompactCut();
		std::size_t cutGarbage = 0;  // members of let-go ranges still in the pool
		/**
		 * @brief Render thread, before the list jobs: admits (removes from the engine's cull) an entry once every DCLF member it shows
		 * is in the frame's set. An entry with an untracked member (its geometry may be released) waits a frame.
		 */
		void RunAdmission();

		/**
		 * @brief Any thread, full-frustum cull to frame end: in the frame's set with the main phase, so the engine's cull skips it
		 * (leaf exclusion). An optimisation only: PassCapture withholds a member's passes whoever's cull reaches it.
		 */
		bool Owned(const RE::BSGeometry& a_geometry) const;

		/** @brief Render thread: the cut's counters since the report. */
		struct CutCounters
		{
			std::uint64_t frames = 0, appliedFrames = 0;
			std::uint64_t skippedStale = 0, skippedPreconditions = 0;
			std::uint64_t entriesPlanned = 0, compactions = 0;  // entries planned again (SyncCut), pool repacks
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0;
			std::uint64_t notSettled = 0, notAdmitted = 0, admittedNow = 0;
			std::uint64_t members = 0, unbound = 0, hiddenSkipped = 0;
			std::uint64_t undrawable = 0;    // bound members in view not in the set, handed to the engine
			std::uint64_t walked = 0;        // entries walked (Cut::walk, or all under Policy::WalkEveryEntry)
			std::uint64_t excluded = 0;      // owned geometries the engine's cull reached and did not register
			std::uint64_t mixed = 0;         // admitted entries with engine-drawn parts, culled by the engine
			std::uint64_t engineMembers = 0;  // engine members in view, handed to its registration
			std::uint64_t switchStale = 0;    // entries the engine culled because a switch's selected child was stale
			std::uint64_t unselected = 0;     // members under an unselected switch child
			std::uint64_t liveAll = 0;        // frames memberLive was read from every switch
			std::uint64_t liveEntries = 0;    // entries a switch event refreshed
			std::int64_t prepareTicks = 0, afterTicks = 0;
		};
		CutCounters TakeCutCounters() { return std::exchange(cutStats, {}); }
		/** @brief Render thread: the scene lists' counters since the last call. */
		struct ListCounters
		{
			std::uint64_t frames = 0, published = 0, built = 0, dryRuns = 0;
			std::uint64_t notToggled = 0, notCurrent = 0, noExclusion = 0, noOcclusion = 0;
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
			// Roots the build's job let go of, released where the engine unloads (one may hold a detached cell's last reference).
			listGraveyard.clear();
		}

		/**
		 * @brief Render thread, FrameDriver at the scene phase's end (before Main::Draw queues DrawWorld_BuildSceneLists): this
		 * frame's scene lists (drawcall-limit-fix.md, "The scene lists without DCLF's roots").
		 *
		 * Left out: admitted, unmixed entries with Cut::walk clear that this frame's full-frustum exclusion removes from the
		 * cascades, while DCLF draws both occlusion maps. The object root's part (cell roots, its two whole entries) is kept
		 * (ListMode::Keep), rebuilt by DCLF (Rebuild), or engine-built and filtered (Engine: interiors, Policy::FilterSceneLists).
		 * In Keep and Rebuild the engine's build still adds portal rooms, occlusion planes and the extra list but skips the object
		 * root (ObjectRootAsNode). Kept lists hold hidden roots (CullList skips them); a hidden cell or category node, a structure
		 * change or a new filter rebuilds them.
		 */
		void PublishListFilter();
		/** @brief This frame's scene lists leave DCLF's roots out (render thread, after BuildSceneLists' job). */
		bool ListsFiltered() const { return listsFiltered.load(std::memory_order_acquire); }
		/**
		 * @brief Render thread: the engine draws an occlusion map (DCLF's not ready), whose Precipitation::SetupMask culls the lists,
		 * so the left-out roots go back first. Only occlusion maps read the lists between the list jobs and ClearLists.
		 */
		void RestoreSceneLists();
		/**
		 * @brief Any thread, SceneTracker's attach (after) and detach (before) detours: a root under a category node is queued
		 * (listEvents); anything higher (category node, cell) rebuilds the kept lists.
		 */
		void NoteListStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached);
		/** @brief Render thread, Coordinator's hidden events: a hidden bit on a structure node rebuilds the kept lists. */
		void NoteHiddenKey(const void* a_key)
		{
			if (listStructural.contains(a_key)) {
				listStructure.fetch_add(1, std::memory_order_relaxed);
				listStats.structureBy[3].fetch_add(1, std::memory_order_relaxed);
			}
		}

	private:
		struct Cut;
		/** @brief The selected child index at AE's offset (Engine::ReadSwitch; CommonLib's is wrong for AE). */
		static std::int32_t SwitchIndex(const RE::NiSwitchNode* a_switch);
		/** @brief Every switch above the member selects its path, read from the switches. */
		bool PathSelected(const auto& a_member) const
		{
			for (std::uint32_t w = a_member.switchBegin; w < a_member.switchEnd; ++w)
				if (SwitchIndex(cut.switchPaths[w].first) != cut.switchPaths[w].second)
					return false;
			return true;
		}
		/** @brief The cull would reach member a_m: not hidden up to the root, selected by every switch above it. */
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
		/** @brief A set hash of entry a_e's DCLF members in the current snapshot. */
		std::uint64_t MemberSignature(std::uint32_t a_e) const;
		/** @brief Render thread, before the list jobs: entry a_e's memberLive from the switches. */
		void RefreshLive(std::uint32_t a_e);
		/**
		 * @brief Where FUN_14147b110's fade value falls below BSFadeNode::OnVisible's fade-out threshold (kObjectFadeTest). > 0:
		 * scaled by the camera's LOD factor; < 0: by a constant, folded in; 0: never fades out by distance.
		 */
		static float FadeDistanceOf(const SceneCapture::NodeRecord& a_root);
		PrimaryCull() = default;

		struct Hooks;
		friend struct Hooks;
		friend struct Diagnostics::PrimaryCullView;

		/** @brief Render thread, after the full-frustum cull. */
		void AfterFullFrustum();
		/** @brief The scene lists' hooks (SceneLists.cpp). */
		struct ListHooks;
		friend struct ListHooks;
		void InstallSceneLists();
		enum class ListMode : std::uint8_t
		{
			Engine,   // engine-built and ClearLists-cleared; DCLF's roots filtered out after the build
			Rebuild,  // DCLF builds them: structure, a hidden structure node, the filter or the build's globals moved
			Keep,     // as the last frame left them
		};
		/** @brief The roots left out for one snapshot. Immutable once published. */
		struct ListFilter
		{
			std::shared_ptr<const SunCandidates> candidates;
			ankerl::unordered_dense::set<const RE::NiAVObject*> roots;
		};
		/** @brief Render thread: listFilterBuilt again while no entry's verdict moved. */
		std::shared_ptr<const ListFilter> CurrentListFilter();
		/** @brief The exterior branch's preconditions (it walks the object root alike every frame) and its globals' witness. */
		bool ListsKeepable(std::uint64_t& a_witness);
		/** @brief The build's job: DCLF's build of the object root's part less the filter's roots, hidden roots included. */
		void RebuildSceneLists(const ListFilter* a_filter);
		/**
		 * @brief The object root's roots, hidden included: a_list(root, a_first) (a_first: object root children 0 and 1, list 0);
		 * a_structural(node) for nodes whose hidden bit or children decide the roots.
		 */
		template <class List, class Structural>
		static void EnumerateSceneLists(const RE::NiAVObject* a_objectRoot, bool a_skipCategory2, List&& a_list, Structural&& a_structural);
		/** @brief The build's job, Rebuild and Keep: the engine build's object-root work besides the lists (kAccumulated). */
		void ListUpkeep();
		/** @brief The build's job: the object root and its roots, the scene lists observer's reference. False without a scene or TES. */
		static bool ListRoots(const RE::NiAVObject*& a_objectRoot, std::vector<const RE::NiAVObject*>& a_roots);
		/** @brief DrawWorld_BuildSceneLists' job, after the engine's build: removes the published roots. */
		void FilterSceneLists();
		/** @brief Whether a_list is one of the scene lists (a_extra: or the extra list). */
		static bool IsSceneList(const void* a_list, bool a_extra);

		std::atomic<ListMode> listMode{ ListMode::Engine };          // this frame's, until Present; read by the list jobs' hooks
		std::atomic<std::shared_ptr<const ListFilter>> listFilter;  // this frame's, until Present; read by the build's job
		std::shared_ptr<const ListFilter> listFilterBuilt;          // the last built, republished while nothing moved
		std::shared_ptr<const ListFilter> listFilterKept;           // the kept lists' filter (the build's job writes)
		std::vector<std::uint8_t> listRemovable;                    // per entry index: in listFilterBuilt
		std::uint64_t filterCutVersion = ~0ull, filterExclusionVersion = ~0ull;  // what listFilterBuilt was made from
		std::uint64_t cutVersion = 0;                               // bumped when an entry's admission or walk changes
		std::vector<std::uint8_t> filterScratch;                    // the build's job
		std::atomic<bool> listsFiltered{ false };
		std::atomic<bool> listsDirty{ true };                       // the lists are not DCLF's build (the engine's, or restored)
		std::atomic<std::uint32_t> listStructure{ 0 };              // NoteListStructure, NoteHiddenKey
		std::uint32_t listStructureBuilt = ~0u;                     // the build's job writes, the render thread reads after Finish
		std::uint64_t listWitness = 0, listWitnessBuilt = ~0ull;
		// The last build's structure nodes (build's job writes, render thread reads between frames); the detours' pointers.
		ankerl::unordered_dense::set<const void*> listStructural;
		std::atomic<const RE::NiAVObject*> listObjectRoot{ nullptr };
		std::atomic<const RE::NiAVObject*> listWholeEntries[2]{ nullptr, nullptr };
		// Each list's first entry: an empty, never-hidden node, so each cull's Process2 sets its frustum up.
		std::array<RE::NiPointer<RE::NiNode>, 16> listSentinels;
		std::array<std::uint32_t, 16> listKeptSize{};  // each list's kept part (the build's job); the engine's per-frame entries follow
		bool skipObjectRoot = false;                   // the build's job: ObjectRootAsNode returns null
		/** @brief A root attached under or detached from a category node, in event order. */
		struct ListEvent
		{
			RE::NiPointer<RE::NiAVObject> held;  // an attach's root, kept alive until applied (released at Present)
			const RE::NiAVObject* child = nullptr;
			const RE::NiAVObject* parent = nullptr;
			bool attached = false;
		};
		EventQueue<ListEvent> listEvents;
		/** @brief The build's job, Keep: applies the queued roots to the kept lists. False when one needs a rebuild. */
		bool ApplyListEvents(const ListFilter* a_filter);
		/** @brief The build's job, Rebuild and Engine: drops the queued events (the build reads the current structure). */
		void DropListEvents();
		// The build's job: the kept lists' category nodes (child index under the cell), and each root's place.
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint16_t> listCategories;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::pair<std::uint32_t, std::uint32_t>> listPositions;
		std::uint32_t listNext = 0;  // the round robin's next list
		std::vector<RE::NiPointer<RE::NiAVObject>> listGraveyard;  // the build's job fills, Present releases
		/** @brief The build's job: empties the scene lists, keeping their references for Present (listGraveyard). */
		void BuryLists();
		bool listsKeepInstalled = false;               // the object root's call site was found and patched
		struct ListStats
		{
			std::uint64_t frames = 0, published = 0, built = 0, dryRuns = 0;
			std::uint64_t notToggled = 0, notCurrent = 0, noExclusion = 0, noOcclusion = 0;
			std::uint64_t restored = 0;           // frames RestoreSceneLists ran
			std::uint64_t restoreDetached = 0;    // roots detached since the snapshot, not restored
			std::uint64_t unexcluded = 0;         // must be 0: filtered lists without a full-frustum exclusion
			std::uint64_t lostWhileOut = 0;       // a binding lost while its root was out of the lists
			std::uint64_t engineFrames = 0, notKeepable = 0;  // engine-built frames; of those, not keepable
			// Why not keepable: no hidden events, no scene or processes, not unbound space (or no entry), interior, few scene children.
			std::array<std::uint64_t, 5> notKeepableBy{};
			std::atomic<std::uint64_t> filtered{ 0 }, entries{ 0 }, removed{ 0 };  // the build's job
			std::atomic<std::uint64_t> kept{ 0 }, rebuilt{ 0 }, lateRebuilds{ 0 }, rebuildTicks{ 0 }, rebuildEntries{ 0 };
			std::uint64_t rebuildStructure = 0, rebuildFilter = 0, rebuildWitness = 0, rebuildDirty = 0;
			// Rebuilds by: object root child, cell child, category node child attached/detached; structure node hidden bit.
			std::array<std::atomic<std::uint64_t>, 4> structureBy{};
			std::atomic<std::uint64_t> eventsApplied{ 0 }, eventsAdded{ 0 }, eventsRemoved{ 0 }, eventsRebuilt{ 0 };  // the build's job
		};
		ListStats listStats;
		/** @brief After the list jobs' Finish, render thread. */
		void AfterListJobs();

		bool installed = false;

		/** @brief Per candidate entry, from the snapshot and a walk of its subtree (once per generation). */
		enum class EntryPlan : std::uint8_t
		{
			Plain,     // no node under it needs OnVisible, the root included
			FadeRoot,  // the root is exactly a BSFadeNode: DCLF runs its OnVisible fade update (ServiceFade); the rest plain
			LeafRoot,  // ... a BSLeafAnimNode: its LOD step, then the fade update
			TreeRoot,  // ... a BSTreeNode: its height test, the leaf node's update, its LOD fix-up (ServiceTreeState)
			Rejected,
		};
		/** @brief The entry's plan; appends its members to cut.members unless rejected. */
		EntryPlan PlanOf(std::uint32_t a_entry, const RE::NiAVObject* a_root);
		std::uintptr_t geometryOnVisible = 0;  // BSGeometry's OnVisible (vtable slot 0x34), read from BSTriShape's vtable
		/**
		 * @brief OnVisibles (slot 0x34) whose cull side effects DCLF makes or needs none of (skyrim-engine-notes.md, "the OnVisibles
		 * a skipped entry no longer runs"), read from vtables at Install: NiNode's (the slot and the engine's own, which subclasses
		 * call), multibound (frustum cache), switch (selected child, followed by event), billboard (DCLF turns from the unturned
		 * world), ordered node (alpha group number; DCLF orders forward members); three geometry ones that append under a condition.
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
		 * @brief Job thread, a list job's Process1: for an eligible entry, does what the cull would without traversing it (see the
		 * class). False: the engine's Process1 runs.
		 */
		bool StandIn(int a_slot, RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg);

		/**
		 * @brief The snapshot's eligible entries. Rebuilt on the render thread before the jobs and changed only between them
		 * (admission), so the jobs read it without a lock.
		 */
		struct Cut
		{
			std::shared_ptr<const SunCandidates> candidates;  // the snapshot the plans are for
			std::vector<EntryPlan> plans;                     // per candidate entry index
			/** @brief A geometry under an eligible entry: DCLF's (synthetic pass) or the engine's (handed to its registration). */
			struct Member
			{
				const RE::BSGeometry* geometry = nullptr;
				bool engine = false;
				// The switches above it as (switch, child index on its path) in switchPaths: drawn only while each selects that
				// child, as NiSwitchNode::OnVisible culls only its selected child.
				std::uint32_t switchBegin = 0, switchEnd = 0;
			};
			std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> switchPaths;
			// Per entry index: its ranges in switches and members (cull order). Appended by SyncCut, repacked by CompactCut.
			std::vector<std::uint32_t> switchBegin, switchEnd;
			std::vector<const RE::NiSwitchNode*> switches;
			std::vector<std::uint32_t> memberBegin, memberEnd;
			std::vector<Member> members;
			std::vector<std::int32_t> memberObject;    // per member: its object index in the tables (-1: none)
			// Per member: every switch above it selects its path. Read from the switches for a new snapshot, then kept by
			// Coordinator::TakeSwitchChanges for the entries they name (switchEntry).
			std::vector<std::uint8_t> memberLive;
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchEntry;  // switch node -> entry index
			bool liveEvents = false;  // memberLive is in force (Scene::SwitchEventsLive); else the jobs read the switches
			std::vector<const RE::NiAVObject*> roots;         // per candidate entry index
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> eligible;  // root -> entry index, plan not Rejected
			std::vector<std::uint8_t> admitted;               // per entry index: DCLF draws all of it
			std::vector<std::uint8_t> mixed;                  // per entry index: some member is the engine's (drawn from the node)
			// Per entry index: the stand-in walks it: it has engine-drawn members, or a tracked member not in the set (handed over
			// whenever shown; the engine can show one before any event reaches the decision). Other admitted entries go to the GPU
			// whole. Kept by RefreshWalk.
			std::vector<std::uint8_t> walk;
			// Census names per entry: why Rejected (RTTI of the unsupported OnVisible's object, or no DCLF geometry), and a mixed
			// entry's first engine-drawn geometry.
			std::vector<const char*> rejectReason;
			std::vector<std::string> mixedBy;
			// Admission by node across snapshots, with its DCLF members' signature (MemberSignature): a root whose members changed
			// (a decal attached, a part swapped) is admitted again only once the new ones are drawn.
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> admittedRoots;
			std::vector<std::uint32_t> pendingAdmission;       // for RunAdmission: a new snapshot's, a member joined, a member the snapshot does not list yet
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
			std::uint64_t undrawable = 0;                   // bound members in view not drawable, handed to the engine
			std::uint64_t excluded = 0;                     // owned geometries the engine's cull reached, not registered
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
		// The animation job's fade updates (FUN_1402cff60's call of FUN_14147a160, Hooks::AnimatedFade) since the last list jobs,
		// with their inputs; at the list jobs' end they become the batch FadeStateCS applies before its cull test.
		std::mutex animatedMutex;
		std::vector<const void*> animatedNodes, animatedBatch;
		AnimatedFadeInputs animatedInputs{}, batchInputs{};
		void NoteAnimatedFade(const void* a_node, const float* a_camera);
		bool standInLive = false;        // this frame's snapshot is current: the stand-in runs
		bool liveStale = true;                             // a frame skipped the switch changes: memberLive is read again
		// The fade roots DCLF services (admitted entries with a fade plan).
		std::vector<const RE::NiAVObject*> ownedFadeRoots;
		// The roots the cut stands in for (admitted, unmixed), whose kAccumulated no engine cull writes (AtUpdateEntry), and the
		// stood-in fade roots among them.
		ankerl::unordered_dense::set<const void*> standInRoots;
		std::vector<const void*> standInList;
		// FadeWrittenByDclf's set: immutable SceneTables from Main::Update's entry, held by the main thread until the next entry
		// (no OnVisible or gate runs then). Lock-free.
		std::atomic<const void*> fadeWriterTables{ nullptr };
		std::shared_ptr<const void> fadeWriterTablesHeld;
		std::atomic<std::uint64_t> onVisibleFadeSkipped{ 0 };
		/** @brief Main::Update's entry: publishes the accepted tables as the writer set (a_running false: none). */
		void PublishWriterTables(bool a_running);
		std::atomic<std::uint64_t> gateFadeSkipped{ 0 };
		// The camera cut test: the main camera's last position and rotation.
		std::array<float, 12> cutCamera{};
		bool cutCameraKnown = false;
		/** @brief FrameView::SetFadeRootsOwned and TreeAnimation::SetOwned from the snapshot's admitted entries. */
		void SyncFadeOwnership();
		/** @brief Render thread: the cut planned from nothing for its snapshot, leaving cut as is (the cut observer's reference). */
		Cut PlanWholeCut();
		/** @brief Render thread, between the jobs: Cut::walk of entry a_e from its current members. */
		void RefreshWalk(std::uint32_t a_e);
		std::vector<std::uint32_t> walkRefresh;  // entries whose member lost its binding: refreshed at the next PrepareFrame
		bool walkEverything = false;             // every entry walked this frame (Policy::WalkEveryEntry)
		// The membership witness the frame's commit sampled, and its frame.
		std::uint32_t sampledWitness = 0;
		std::uint32_t sampledWitnessFrame = ~0u;
		/** @brief List jobs and render thread: a member of the frame's set with the main phase (FrameView::SetPhasesOf). */
		bool MemberDrawable(std::int32_t a_object) const;
	};
}
