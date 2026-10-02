#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Engine/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Scene/FadeState.h"
#include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"

namespace DCLF
{
	struct SunCandidates;

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
	 * Step 1, the census (CS_DCLF_PRIMARY_EXCLUDE=probe, nothing removed): what the lists hold, against the sun's entry
	 * candidates, and what the main camera registers under them.
	 *
	 * The cut (toggle `excludePrimaryEntries`, CS_DCLF_PRIMARY_EXCLUDE): the list processes' Process1 (vtable slot 0x16,
	 * shared with every other list process, so filtered by process) stands in for the cull of every eligible entry, on
	 * the list job's own thread. For an admitted, settled entry in view (the job's own planes) it:
	 *   - leaves the root's OnVisible update to FadeStateCS (the fade, a leaf node's LOD step, a tree's LOD fix-up), written
	 *     back onto the node, and tests a tree's height;
	 *   - does nothing for its geometries bound by scene membership (SceneStore::IsMember): their records are drawn
	 *     whenever the GPU finds them;
	 *   - hands every other geometry in view to the engine's registration as the cull would (the process's
	 *     AppendVirtual, in the traversal's order): decals, effects and blended objects, and a DCLF geometry not bound yet.
	 * Switch nodes are followed by event (a member is drawn while every switch above it selects its path: memberLive,
	 * updated from SceneStore's switch events, which also bring a newly selected child up to date), and a root that is
	 * fading or cross-fading LOD leaves the entry to the engine's Process1 that frame. An entry with engine-drawn parts
	 * (Cut::mixed) is the engine's Process1's whole: its OnVisible updates the node the engine's draws read, the AppendVirtual
	 * hook keeps DCLF's members from the registration, and FadeStateCS runs the same update for those members. An entry is eligible when its nodes are plain (NiNode, BSMultiBoundNode, switch
	 * nodes; a fade, leaf or tree root) and its geometries use BSGeometry's OnVisible, with at least one DCLF draws;
	 * it is admitted once the colour epoch has drawn all of those (Admit). After the jobs the render thread only
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

		/** @brief CS_DCLF_PRIMARY_EXCLUDE=probe. */
		static bool Probe();

		/**
		 * @brief A registration through FUN_140e28af0 outside the sun's Accumulate, any thread: counted by accumulator
		 * (the main one, render mode 0; the depth one, 0xC) and by whether the geometry is under a listed candidate entry.
		 */
		void NoteRegistration(const void* a_accumulator, const RE::BSGeometry* a_geometry);
		bool Counting() const { return counting.load(std::memory_order_relaxed); }

		/**
		 * @brief Render thread, after the sun's Accumulate: the pass descriptor's ShadowDir and DefShadow bits (13, 14)
		 * GetRenderPasses would give the geometry's main pass this frame, for a geometry no shadowed point light reaches;
		 * ~0u when the cascades are not known this frame. The engine's rule (dclf-gpu-driven-frame.md, "Phase 1, step 2"),
		 * with the sun's mask replaced by the cascade test.
		 */
		static std::uint32_t SunShadowBits(const RE::BSGeometry& a_geometry, const RE::BSLightingShaderProperty* a_property = nullptr);
		/**
		 * @brief The bits SunShadowBits gives a geometry whose bound meets a cascade: what depends on the object and
		 * the frame's globals alone. The GPU makes the cascade test (kObjectSunTest).
		 */
		static std::uint32_t SunShadowStatic(const RE::BSGeometry& a_geometry, const RE::BSLightingShaderProperty* a_property = nullptr);
		/** @brief CS_DCLF_RESIDENT_PARITY: the synthetic pass built from scratch (no cache), for SceneStore's comparison. */
		static bool FreshSyntheticPass(const RE::BSGeometry& a_geometry, AccumulatedPass& a_out);
		/**
		 * @brief The pass SceneStore binds an object with by scene membership: a synthetic pass from the object alone (render
		 * thread; derived descriptors cached per geometry), false where one cannot model it (decals, fading or translucent).
		 */
		bool MembershipPass(const RE::BSGeometry* a_geometry, AccumulatedPass& a_out);
		/** @brief MembershipPass for the geometry's layer (LayerPropertyOf, a_layer): the pass of its property with hint 12. */
		bool MembershipLayerPass(const RE::BSGeometry* a_geometry, const RE::BSLightingShaderProperty& a_layer, AccumulatedPass& a_out);
		/** @brief A fade root's fade-out distance for BuildDraws' fade test (kObjectFadeTest), 0 when it has none. */
		static float MembershipFadeDistance(const RE::NiAVObject* a_root) { return FadeDistanceOf(a_root); }
		/** @brief The frame globals a membership pass reads (the static sun bits, the fade distances): a change rebinds them all. */
		static std::uint32_t MembershipWitness();
		/**
		 * @brief This frame's main camera as BSFadeNode::OnVisible measures from it: its position and its LOD factor
		 * (NiCamera +0x184), for BuildDraws' fade test (kObjectFadeTest). Zero when the cut did not see a camera.
		 */
		std::array<float, 4> FadeEye() const { return fadeEye; }
		/** @brief The main camera's cull test for FadeStateCS this frame (Records.h, kFadeVisibilityBytes), sampled after the list jobs. */
		const std::vector<std::byte>& FadeVisibility() const { return fadeVisibility; }
		/** @brief BSTreeNode::OnVisible's height test this frame, for BuildDraws (kObjectHeightTest): the base and the limit (+infinity: off). */
		std::array<float, 2> TreeHeightTest() const { return treeHeight; }
		/**
		 * @brief The main pass GetRenderPasses would register for the geometry this frame, built without it: the
		 * derived pass descriptor with the sun's bits, the batch list, the accumulation hint and the LOD row. False for
		 * what it does not model (translucent or fading objects, an unknown cascade test, a descriptor not derived).
		 */
		static bool SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out, bool a_sunOnGpu = false,
			const RE::BSLightingShaderProperty* a_layer = nullptr);

		/**
		 * @brief This frame's members in view under the entries the list jobs stood in for: nothing registered them, so one
		 * the colour epoch did not draw is a hole (IndirectDraws::PublishClaims).
		 */
		const std::vector<const RE::BSGeometry*>& StoodInMembers() const { return frameVisible; }
		/** @brief The hole test (IndirectDraws::PublishClaims): a stood-in member the colour epoch did not draw. */
		void CountHole() { ++cutStats.holes; }
		/**
		 * @brief Render thread (SceneStore, a member's residency dropped): the geometry's entry is walked again (Cut::walk) until
		 * the next frame says whether it still needs it.
		 */
		void NoteMemberLost(const RE::BSGeometry* a_geometry);
		/** @brief Render thread: every residency ended (SceneStore::EndAllResidency): every entry walked until the next snapshot. */
		void NoteAllMembersLost();
		/**
		 * @brief Render thread, after the registration jobs: CS_DCLF_PERSISTENT_PARITY's light mask check of the owned members in
		 * view.
		 */
		void CheckLightMasks();
		/**
		 * @brief After the colour epoch, render thread: admission by readiness. An entry is admitted, and left out of the
		 * engine's cull from the next frame on, once the colour build draws every DCLF member it shows (a_drawn), in view or
		 * not. Checked when one of its members starts being drawn (a_newlyDrawn, the build's drawn marks), and once for every
		 * entry of a new snapshot. Only against a current snapshot: a stale one's geometries may have been released since (a
		 * cell unloading), and its successor queues its entries again.
		 */
		void Admit(const std::function<bool(const RE::BSGeometry*)>& a_drawn, const std::vector<const RE::BSGeometry*>& a_newlyDrawn);

		/**
		 * @brief Any thread from the full-frustum cull to the frame's end (a list process's AppendVirtual, the shadow lights' registrations): whether the geometry is DCLF's to draw, so the engine's cull does not
		 * hand it to the registration (leaf exclusion). Owned: bound by scene membership and drawn by the colour build (the
		 * claims). Wherever the engine still culls (an entry not stood in for: actors, a LOD cross-fade), its members are
		 * left out here.
		 */
		bool Owned(const RE::BSGeometry& a_geometry) const;
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);
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
		 * The roots they leave out (CS_DCLF_LIST_FILTER): an entry admitted with nothing for the registration (no engine-drawn
		 * part, every member bound: Cut::walk clear) that the exclusion this frame's full-frustum cull applies takes out of
		 * the sun's cascades, while DCLF draws both occlusion maps. Then the main camera's list jobs (the stand-in's lookup)
		 * and the sun's full-frustum cull never see it.
		 *
		 * How they are made: the object root's part (every cell's roots, and its two whole entries) kept from the last frame
		 * where nothing it reads moved (ListMode::Keep), built by DCLF where something did (Rebuild), or the engine's own build,
		 * filtered, in an interior or on a parity frame (Engine). In the first two the engine's build still runs and adds what
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
		/** @brief Render thread, SceneStore's hidden events: a hidden bit written on a structure node rebuilds the kept lists. */
		void NoteHiddenKey(const void* a_key)
		{
			if (listStructural.contains(a_key)) {
				listStructure.fetch_add(1, std::memory_order_relaxed);
				listStats.structureBy[3].fetch_add(1, std::memory_order_relaxed);
			}
		}

	private:
		struct Cut;
		/** @brief The switch node's selected child index, read at the engine's offset (SceneStore::ReadSwitch; CommonLib's is not AE's). */
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
		static float FadeDistanceOf(const RE::NiAVObject* a_root);
		PrimaryCull() = default;

		struct Hooks;
		friend struct Hooks;

		/** @brief After the full-frustum cull, render thread: the census of the lists. */
		void AfterFullFrustum();
		/** @brief The scene lists' hooks (SceneLists.cpp). */
		struct ListHooks;
		friend struct ListHooks;
		void InstallSceneLists();
		/** @brief The report's scene lists line (a_checked, a_missed: the dry runs' stand-in counts). */
		void ReportSceneLists(std::uint64_t a_checked, std::uint64_t a_missed);
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
		 * @brief The object root's roots, as RebuildSceneLists and the parity enumerate them: a_list(root, a_first) (a_first:
		 * object root children 0 and 1, which go to list 0), a_structural(node) for the nodes whose hidden bit or children decide
		 * which roots those are. Hidden roots are included.
		 */
		template <class List, class Structural>
		static void EnumerateSceneLists(const RE::NiAVObject* a_objectRoot, bool a_skipCategory2, List&& a_list, Structural&& a_structural);
		/** @brief The build's job, Rebuild and Keep: what the engine's build does on the object root besides the lists (kAccumulated). */
		void ListUpkeep();
		/** @brief The build's job, a parity frame: the engine's lists against DCLF's enumeration (hidden roots aside). */
		void CheckSceneLists();
		/** @brief DrawWorld_BuildSceneLists' job, after the engine's build: the published roots taken out of the lists. */
		void FilterSceneLists();
		/** @brief Whether a_list is one of the scene lists (a_extra: or the extra list). */
		static bool IsSceneList(const void* a_list, bool a_extra);

		std::atomic<ListMode> listMode{ ListMode::Engine };          // this frame's, until Present; read by the list jobs' hooks
		std::atomic<std::shared_ptr<const ListFilter>> listFilter;  // this frame's, until Present; read by the build's job
		std::shared_ptr<const ListFilter> listFilterBuilt;          // the last built, published again while nothing moved
		std::shared_ptr<const ListFilter> listFilterKept;           // the one the kept lists leave out (the build's job writes)
		std::shared_ptr<const ListFilter> parityFilter;             // a parity frame's dry run, read by the list jobs
		std::vector<std::uint8_t> listRemovable;                    // per entry index: in listFilterBuilt
		std::uint64_t filterCutVersion = ~0ull, filterExclusionVersion = ~0ull;  // what listFilterBuilt was made from
		std::uint64_t cutVersion = 0;                               // bumped when an entry's admission or walk changes
		std::vector<std::uint8_t> filterScratch;                    // the build's job
		std::atomic<bool> listsFiltered{ false };
		std::atomic<bool> listsDirty{ true };                       // the lists are not DCLF's build (the engine's, or restored)
		std::atomic<std::uint32_t> listStructure{ 0 };              // NoteListStructure, NoteHiddenKey
		std::uint32_t listStructureBuilt = ~0u;                     // the build's job writes, the render thread reads after Finish
		std::uint64_t listWitness = 0, listWitnessBuilt = ~0ull;
		bool listParity = false;                                    // this frame's engine build is checked (CheckSceneLists)
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
			std::uint64_t unexcluded = 0;         // must be 0: filtered lists, and the full-frustum cull applied no exclusion
			std::uint64_t lostWhileOut = 0;       // a member's binding lost while its root was out of the lists
			std::uint64_t engineFrames = 0, notKeepable = 0;  // the engine built them, and of those for want of the exterior branch
			// Why not keepable: no hidden events, no scene or processes, not unbound space (or no entry), an interior, too few scene children.
			std::array<std::uint64_t, 5> notKeepableBy{};
			std::atomic<std::uint64_t> filtered{ 0 }, entries{ 0 }, removed{ 0 };  // the build's job
			std::atomic<std::uint64_t> kept{ 0 }, rebuilt{ 0 }, lateRebuilds{ 0 }, rebuildTicks{ 0 }, rebuildEntries{ 0 };
			std::atomic<std::uint64_t> parityChecks{ 0 }, parityMissing{ 0 }, parityExtra{ 0 };  // missing: the engine's not in DCLF's; extra: DCLF's, not hidden, not the engine's
			std::uint64_t rebuildStructure = 0, rebuildFilter = 0, rebuildWitness = 0, rebuildDirty = 0;
			// The events that rebuild them: a child of the object root, of a cell (a category node), of a category node (a root)
			// attached or detached; a structure node's hidden bit written.
			std::array<std::atomic<std::uint64_t>, 4> structureBy{};
			std::atomic<std::uint64_t> eventsApplied{ 0 }, eventsAdded{ 0 }, eventsRemoved{ 0 }, eventsRebuilt{ 0 };  // the build's job
			std::string parityFirst;  // the build's job, read after Finish
		};
		ListStats listStats;
		/** @brief After the list jobs' Finish, render thread. */
		void AfterListJobs();

		bool installed = false;
		std::atomic<bool> counting{ false };

		struct Census
		{
			std::uint64_t frames = 0;
			std::uint64_t lists = 0;
			std::uint64_t entries = 0;          // all lists' entries
			std::uint64_t firstEntries = 0;     // entry 0 of each list (never removable)
			std::uint64_t candidates = 0;       // entries that are sun candidates
			std::uint64_t candidateGeometries = 0;  // tracked geometries under those
			std::uint64_t actorEntries = 0;     // entries whose reference is an actor
			std::uint64_t extraEntries = 0;     // the first job's extra list (DAT_14338c888)
			std::uint64_t sunCandidates = 0;    // the sun candidates' size, for the share found in the lists
			std::map<std::string, std::uint64_t> others;  // not a candidate: "RTTI / parent" -> count
		};
		Census census;

		// This frame's listed candidate entries (by candidate entry index), for the registration counts.
		std::shared_ptr<const SunCandidates> frameCandidates;
		std::vector<std::uint8_t> listed;  // per candidate entry index

		struct Registrations
		{
			std::atomic<std::uint64_t> main{ 0 }, mainUnder{ 0 };
			std::atomic<std::uint64_t> depth{ 0 }, depthUnder{ 0 };
			std::atomic<std::uint64_t> other{ 0 };
		};
		Registrations registrations;

		std::string playerChain;

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
		/** @brief BSTreeNode::OnVisible's height test: true when the tree is above the limit (not drawn, not updated). */
		static bool TreeAboveLimit(const RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process);
		/** @brief Render thread, after the full-frustum cull: this frame's preconditions, and a new snapshot's plans. */
		void PrepareFrame();
		/** @brief CS_DCLF_FADE_PARITY, after PrepareFrame: the fade port against the engine's functions (FadeState::CheckPort). */
		void CheckFadePort();
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
			std::vector<std::uint32_t> switchOffsets;  // per candidate entry index: its switch nodes in switches
			std::vector<const RE::NiSwitchNode*> switches;
			std::vector<std::uint32_t> memberOffsets;  // per candidate entry index: its geometries in members, in the cull's order
			std::vector<Member> members;
			std::vector<std::int32_t> memberObject;    // per member: its object index in the tables (-1: none)
			// Per member: every switch above it selects its path. Read from the switches for a new snapshot, then kept by
			// the switch events (SceneStore::TakeSwitchChanges) for the entries they name (switchEntry).
			std::vector<std::uint8_t> memberLive;
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchEntry;  // switch node -> entry index
			bool liveEvents = false;  // memberLive is in force (SceneStore::SwitchEventsLive); else the jobs read the switches
			std::vector<std::uint32_t> geometryOffsets;       // per candidate entry index: its tracked geometries in geometryIndices
			std::vector<const RE::BSGeometry*> geometryIndices;
			std::vector<const RE::NiAVObject*> roots;         // per candidate entry index
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> eligible;  // root -> entry index, plan not Rejected
			std::vector<std::uint8_t> admitted;               // per entry index: DCLF has drawn all of it (see the class)
			std::vector<std::uint8_t> mixed;                  // per entry index: some member is the engine's (drawn from the node)
			// Per entry index: the stand-in walks it (its bound test and its members): it has engine-drawn members to hand to
			// the registration, or a member shown but not bound (handed over until it is). Any other admitted entry is left
			// to the GPU whole. Kept by events (RefreshWalk): a new snapshot, a member's binding lost (NoteMemberLost, then
			// the next frame), a member newly drawn.
			std::vector<std::uint8_t> walk;
			// The same by node, kept across snapshots with the set of DCLF members it was admitted with (MemberSignature): a root
			// whose members changed (a decal attached, a part swapped) is admitted again only once the new ones are drawn.
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> admittedRoots;
			std::vector<std::uint32_t> pendingAdmission;       // entries to check for admission (Admit): a new snapshot's, a member newly drawn, in view
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
			std::vector<std::uint32_t> pending;
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0, notSettled = 0, notAdmitted = 0, walked = 0, walkMissed = 0;
			std::uint64_t hidden = 0, engineMembers = 0, switchStale = 0, unselected = 0;
			std::uint64_t unbound = 0;                      // DCLF geometries in view not bound yet, handed to the engine
			std::uint64_t excluded = 0;                     // owned geometries the engine's cull reached, not handed to its registration
			std::uint64_t filterChecked = 0, filterMissed = 0;  // a parity frame's dry run: roots it would leave out, and the stand-in's disagreements
			std::uint64_t mixed = 0;                        // admitted entries with engine-drawn parts, culled by the engine
		};
		std::array<JobOut, 16> jobOut;

		/** @brief The cut's counters since the report (render thread). */
		struct CutStats
		{
			std::uint64_t frames = 0, appliedFrames = 0;
			std::uint64_t skippedStale = 0, skippedPreconditions = 0;
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0;
			std::uint64_t notSettled = 0, notAdmitted = 0, admittedNow = 0;
			std::uint64_t members = 0, unbound = 0, hiddenSkipped = 0;
			std::uint64_t maskChecked = 0, maskSun = 0, maskOther = 0;
			std::uint64_t walked = 0;  // entries the stand-in walked (Cut::walk, or every one under the parity)
			std::uint64_t walkMissed = 0;  // CS_DCLF_PERSISTENT_PARITY: geometries handed to the registration from entries Cut::walk leaves  // CS_DCLF_PERSISTENT_PARITY: owned members in view, and their masks not 0
			std::uint64_t excluded = 0;     // owned geometries the engine's own cull reached and did not register (leaf exclusion)
			std::uint64_t holes = 0;
			std::uint64_t filterChecked = 0, filterMissed = 0;
			std::uint64_t mixed = 0;
			std::uint64_t engineMembers = 0;  // the engine's members in view, handed to its registration
			std::uint64_t switchStale = 0;    // entries the engine culled this frame because a switch's selected child was out of date
			std::uint64_t unselected = 0;     // members under an unselected switch child
			std::uint64_t liveAll = 0;        // frames memberLive was read from every switch (a new snapshot, a resync)
			std::uint64_t liveEntries = 0;    // entries whose memberLive a switch event refreshed
			std::int64_t prepareTicks = 0, afterTicks = 0;
		};
		CutStats cutStats;
		std::string maskFirst;  // the light mask parity's first geometry with bits
		std::uint32_t frameSunBits = 0;  // the sun's cascade bits this frame (AfterListJobs), for CheckLightMasks
		std::vector<const RE::BSGeometry*> frameVisible;  // this frame's members in view under stood-in entries
		std::vector<const RE::NiAVObject*> switchChanges;  // scratch: SceneStore::TakeSwitchChanges
		std::array<float, 4> fadeEye{};  // FadeEye, captured in PrepareFrame
		std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSGeometry*>> frameClaims;  // the claims, for Owned (PrepareFrame)
		std::array<float, 2> treeHeight{ 0.0f, std::numeric_limits<float>::infinity() };  // TreeHeightTest, captured in PrepareFrame
		std::vector<std::byte> fadeVisibility;  // FadeVisibility: by the first list job with a compound frustum, else AfterListJobs
		std::atomic<bool> visibilitySampled{ false };
		std::uint64_t visibilityOverflows = 0;  // frames whose compound frustum outgrew the block (the frustum alone that frame)
		/** @brief A list job (the first with a compound frustum), else AfterListJobs: the list processes' cull test into fadeVisibility. */
		void SampleFadeVisibility(const RE::NiCullingProcess* a_process);
		std::uint64_t frameCounter = 0;
		std::uint32_t fadePortCursor = 0;  // CheckFadePort's next entry
		FadeState::PortCheck fadePort;      // its counts since the report
		bool standInLive = false;        // this frame's snapshot is current: the stand-in runs
		bool liveStale = true;                             // a frame skipped the switch changes: memberLive is read again
		// The fade roots DCLF services (SyncFadeOwnership: the admitted entries' with a fade plan), and whether a frame since
		// the last applied one left every entry to the engine (their nodes are then seeded again).
		std::vector<const RE::NiAVObject*> ownedFadeRoots;
		std::uint32_t standInLodSkins = 0;  // stood-in members whose skin's partitions follow the LOD level (SyncFadeOwnership)
		bool fadeSkipped = false;
		/** @brief SceneStore::SetFadeRootsOwned from the current snapshot's admitted fade entries. */
		void SyncFadeOwnership();
		/** @brief Cut::walk of entry a_e, from its members now (render thread, between the jobs). */
		void RefreshWalk(std::uint32_t a_e);
		std::vector<std::uint32_t> walkRefresh;  // entries whose member lost its binding: refreshed at the next PrepareFrame
		bool walkEverything = false;             // CS_DCLF_PERSISTENT_PARITY: every entry walked (the hole and light mask checks)

		/** @brief The derived pass descriptor per geometry, recomputed when what it reads changes. */
		struct DerivedEntry
		{
			const void* property = nullptr;
			const void* material = nullptr;
			std::uint64_t flags = 0;
			std::uint8_t fadeState = 0;
			std::uint32_t derivedPass = kNotDerived;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, DerivedEntry> derivedCache;
		ankerl::unordered_dense::map<const RE::BSGeometry*, DerivedEntry> layerDerivedCache;  // the layers' (MembershipLayerPass)
	};
}
