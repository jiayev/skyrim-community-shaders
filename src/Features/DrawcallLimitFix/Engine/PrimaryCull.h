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
	 *   - leaves the root's OnVisible update to the visibility feedback (ConsumeFeedback: the fade, a leaf node's LOD
	 *     step, a tree's LOD fix-up and the visibility bit the tree clock reads), and tests a tree's height;
	 *   - does nothing for its geometries bound by scene membership (SceneStore::IsMember): their records are drawn
	 *     whenever the GPU finds them;
	 *   - hands every other geometry in view to the engine's registration as the cull would (the process's
	 *     AppendVirtual, in the traversal's order): decals, effects and blended objects, and a DCLF geometry not bound yet.
	 * Switch nodes are followed by event (a member is drawn while every switch above it selects its path: memberLive,
	 * updated from SceneStore's switch events, which also bring a newly selected child up to date), and a root that is
	 * fading or cross-fading LOD leaves the entry to the engine's Process1 that frame. An entry is eligible when its nodes are plain (NiNode, BSMultiBoundNode, switch
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

		/** @brief Render thread, after the list jobs: whether the geometry is under a candidate entry the lists hold. */
		bool UnderListedCandidate(const RE::BSGeometry* a_geometry) const;
		/**
		 * @brief The accumulate phase, render thread, for an object under a listed candidate entry: what its
		 * registration gave it (the technique, the light assignment, the LOD mode, the fade), against what DCLF
		 * derives without it.
		 */
		void NoteDerived(const RE::BSGeometry& a_geometry, const LightingDescriptors& a_descriptors, const AccumulatedPass& a_accumulated,
			Ineligible a_reason, std::uint32_t a_derivedLodRow);

		/**
		 * @brief Render thread, after the sun's Accumulate: the pass descriptor's ShadowDir and DefShadow bits (13, 14)
		 * GetRenderPasses would give the geometry's main pass this frame, for a geometry no shadowed point light reaches;
		 * ~0u when the cascades are not known this frame. The engine's rule (dclf-gpu-driven-frame.md, "Phase 1, step 2"),
		 * with the sun's mask replaced by the cascade test.
		 */
		static std::uint32_t SunShadowBits(const RE::BSGeometry& a_geometry);
		/**
		 * @brief The bits SunShadowBits gives a geometry whose bound meets a cascade: what depends on the object and
		 * the frame's globals alone. The GPU makes the cascade test (kObjectSunTest).
		 */
		static std::uint32_t SunShadowStatic(const RE::BSGeometry& a_geometry);
		/** @brief CS_DCLF_RESIDENT_PARITY: the synthetic pass built from scratch (no cache), for SceneStore's comparison. */
		static bool FreshSyntheticPass(const RE::BSGeometry& a_geometry, AccumulatedPass& a_out);
		/**
		 * @brief The pass SceneStore binds an object with by scene membership: a synthetic pass from the object alone (render
		 * thread; derived descriptors cached per geometry), false where one cannot model it (decals, fading or translucent).
		 */
		bool MembershipPass(const RE::BSGeometry* a_geometry, AccumulatedPass& a_out);
		/** @brief A fade root's fade-out distance for BuildDraws' fade test (kObjectFadeTest), 0 when it has none. */
		static float MembershipFadeDistance(const RE::NiAVObject* a_root) { return FadeDistanceOf(a_root); }
		/** @brief The frame globals a membership pass reads (the static sun bits, the fade distances): a change rebinds them all. */
		static std::uint32_t MembershipWitness();
		/**
		 * @brief This frame's main camera as BSFadeNode::OnVisible measures from it: its position and its LOD factor
		 * (NiCamera +0x184), for BuildDraws' fade test (kObjectFadeTest). Zero when the cut did not see a camera.
		 */
		std::array<float, 4> FadeEye() const { return fadeEye; }
		/** @brief BSTreeNode::OnVisible's height test this frame, for BuildDraws (kObjectHeightTest): the base and the limit (+infinity: off). */
		std::array<float, 2> TreeHeightTest() const { return treeHeight; }
		/**
		 * @brief The colour commit, render thread (IndirectDraws::ArmFeedback): this frame's stood-in entries, carried
		 * with the frame's feedback copy to its decode. Null when the cut did not apply this frame.
		 */
		std::shared_ptr<void> TakeFeedbackTag() { return std::exchange(pendingTag, {}); }
		/**
		 * @brief The accumulate phase, render thread: gives an engine-registered pass the synthetic pass's sun bits (the
		 * static rule, and kObjectSunTest for the GPU's cascade test) when the geometry is one PrimaryCull draws
		 * synthetically, so the two sources of its pass share one pipeline key. Drawing it before admission is then
		 * the evidence that its synthetic pass can be drawn. Only on a frame the cut applies (no local shadow light).
		 */
		void UnifySunBits(const RE::BSGeometry* a_geometry, AccumulatedPass& a_pass) const;

		/**
		 * @brief The main pass GetRenderPasses would register for the geometry this frame, built without it: the
		 * derived pass descriptor with the sun's bits, the batch list, the accumulation hint and the LOD row. False for
		 * what it does not model (translucent or fading objects, an unknown cascade test, a descriptor not derived).
		 */
		static bool SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out, bool a_sunOnGpu = false);

		/**
		 * @brief This frame's members in view under the entries the list jobs stood in for: nothing registered them, so one
		 * the colour epoch did not draw is a hole (IndirectDraws::PublishClaims).
		 */
		const std::vector<const RE::BSGeometry*>& StoodInMembers() const { return frameVisible; }
		/** @brief The hole test (IndirectDraws::PublishClaims): a stood-in member the colour epoch did not draw. */
		void CountHole() { ++cutStats.holes; }
		/** @brief Render thread, after the registration jobs: the decode of the completed feedback frames, on the worker. */
		void KickFeedbackDecode();
		/** @brief Render thread, before the decode's kick: the roots the last decode found faded out or back in, onto their members. */
		void ApplyFadeChanges();
		/**
		 * @brief After the colour epoch, render thread: admission by readiness. An entry is admitted, and left out of the
		 * engine's cull from the next frame on, once the colour build draws every DCLF member it shows (a_drawn), in view or
		 * not. Checked when one of its members starts being drawn (a_newlyDrawn, the build's drawn marks), and once for every
		 * entry of a new snapshot. Only against a current snapshot: a stale one's geometries may have been released since (a
		 * cell unloading), and its successor queues its entries again.
		 */
		void Admit(const std::function<bool(const RE::BSGeometry*)>& a_drawn, const std::vector<const RE::BSGeometry*>& a_newlyDrawn);

		void Report(std::uint32_t a_frame, std::uint32_t a_interval);
		/** @brief At Present, render thread: the frame's feedback decode is joined (the next frame's update reads what it wrote). */
		void EndFrame() { JoinFeedback(); }

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

		/** @brief NoteDerived's counters (render thread). */
		struct Derived
		{
			std::uint64_t objects = 0;
			std::uint64_t notDerived = 0;               // the derivation leaves it native
			std::uint64_t ineligible = 0;               // this frame's verdict is not None
			std::array<std::uint64_t, 40> byReason{};   // ... by Ineligible
			std::uint64_t differ = 0;                   // derived pass descriptor != registered, outside bits 13 and 14
			std::uint64_t sunOnly = 0;                  // ... only in bits 13 and 14
			std::array<std::uint64_t, 32> bits{};       // per differing bit
			std::uint64_t shadowLights = 0;             // the pass has shadowed point lights (bits 6-8, LLF's mask)
			std::uint64_t lodRowDiffer = 0;             // the fade node's LOD row != the pass's LODMode (skinned LOD)
			std::uint64_t fading = 0;                   // fading at registration
			std::uint64_t alphaMask = 0;                // AdditionalAlphaMask (screen-door fade)
			std::array<std::uint64_t, 32> hints{};      // accumulation hint
			std::uint64_t sunAgree = 0;                 // SunShadowBits against the registered bits 13 and 14
			std::uint64_t sunEngineOnly = 0;
			std::uint64_t sunDclfOnly = 0;
			std::uint64_t sunOther = 0;                 // the two bits disagree in another way (one of them)
			std::uint64_t sunUnknown = 0;               // the cascades were not known
			std::map<std::string, std::uint64_t> sunSamples;
			std::uint64_t synthAgree = 0;               // SyntheticPass against the captured pass, every field
			std::uint64_t synthUnmodeled = 0;
			std::uint64_t synthTechnique = 0, synthSubPass = 0, synthHint = 0, synthLodRow = 0;
			std::map<std::string, std::uint64_t> synthSamples;
			std::map<std::string, std::uint64_t> samples;  // a few differing objects
		};
		Derived derived;
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
		/**
		 * @brief BSFadeNode::OnVisible (AE 0x141479f50) and BSLeafAnimNode::OnVisible (0x14147c9c0) for the main camera,
		 * without the recursion: the fade and LOD state the engine's cull would have updated. Returns whether OnVisible
		 * would have gone on into the children (false once the node has faded out).
		 */
		static bool ServiceFade(RE::NiAVObject* a_node, bool a_leaf, const RE::NiCamera& a_camera);
		/** @brief BSTreeNode::OnVisible's height test: true when the tree is above the limit (not drawn, not updated). */
		static bool TreeAboveLimit(const RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process);
		/** @brief BSTreeNode::OnVisible past its height test: the leaf update and the LOD fix-up. */
		static bool ServiceTreeState(RE::NiAVObject* a_node, const RE::NiCamera& a_camera);
		/** @brief The decode (worker): one frame's feedback, applied to the entries stood in for in that frame. */
		void ConsumeFeedback(std::uint32_t a_stamp, std::uint32_t a_objects, const std::uint32_t* a_words, const std::shared_ptr<void>& a_tag);
		/** @brief Render thread, before the list jobs: the last decode job is done (it wrote the nodes they read). */
		void JoinFeedback();
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
		 * @brief A list process's AppendVirtual, job thread: whether the geometry is DCLF's to draw, so the engine's cull does not
		 * hand it to the registration (leaf exclusion). Owned: bound by scene membership and drawn by the colour build (the
		 * claims). Wherever the engine still culls (an entry not stood in for: actors, a LOD cross-fade), its members are
		 * left out here.
		 */
		bool Owned(const RE::BSGeometry& a_geometry) const;

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
			std::vector<std::int32_t> memberObject;    // per member: its object index in the tables (-1: none), for the feedback
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
			std::vector<std::uint32_t> stoodIn;  // entries the job left to DCLF this frame, in view or not
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0, notSettled = 0, notAdmitted = 0;
			std::uint64_t hidden = 0, engineMembers = 0, switchStale = 0, unselected = 0;
			std::uint64_t unbound = 0;                      // DCLF geometries in view not bound yet, handed to the engine
			std::uint64_t excluded = 0;                     // owned geometries the engine's cull reached, not handed to its registration
		};
		std::array<JobOut, 16> jobOut;

		/** @brief The cut's counters since the report (render thread). */
		struct CutStats
		{
			std::uint64_t frames = 0, appliedFrames = 0;
			std::uint64_t skippedStale = 0, skippedPreconditions = 0;
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0;
			std::uint64_t notSettled = 0, notAdmitted = 0, admittedNow = 0;
			std::uint64_t members = 0, unbound = 0, hiddenSkipped = 0, localShadowed = 0;
			std::uint64_t fadeChanges = 0;  // roots the fade service found faded out or back in
			std::uint64_t excluded = 0;     // owned geometries the engine's own cull reached and did not register (leaf exclusion)
			std::uint64_t holes = 0;
			std::uint64_t engineMembers = 0;  // the engine's members in view, handed to its registration
			std::uint64_t switchStale = 0;    // entries the engine culled this frame because a switch's selected child was out of date
			std::uint64_t unselected = 0;     // members under an unselected switch child
			std::uint64_t liveAll = 0;        // frames memberLive was read from every switch (a new snapshot, a resync)
			std::uint64_t liveEntries = 0;    // entries whose memberLive a switch event refreshed
			std::int64_t prepareTicks = 0, afterTicks = 0;
		};
		CutStats cutStats;
		std::vector<const RE::BSGeometry*> frameVisible;  // this frame's members in view under stood-in entries
		std::vector<const RE::NiAVObject*> switchChanges;  // scratch: SceneStore::TakeSwitchChanges
		std::array<float, 4> fadeEye{};  // FadeEye, captured in PrepareFrame
		std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSGeometry*>> frameClaims;  // the claims, for Owned (PrepareFrame)
		std::array<float, 2> treeHeight{ 0.0f, std::numeric_limits<float>::infinity() };  // TreeHeightTest, captured in PrepareFrame
		std::uint64_t frameCounter = 0;
		bool standInLive = false;        // this frame's snapshot is current: the stand-in runs
		bool liveStale = true;                             // a frame skipped the switch changes: memberLive is read again
		std::shared_ptr<void> feedbackJob;                 // the worker's feedback decode (an AsyncWorker::JobHandle)
		std::shared_ptr<void> pendingTag;                  // this frame's stood-in entries, until the colour commit takes them
		/** @brief A feedback frame's tag: the entries stood in for, and the snapshot their indices belong to. */
		struct FeedbackTag
		{
			std::shared_ptr<const SunCandidates> candidates;
			std::vector<std::uint32_t> stoodIn;
			// The stood-in entries' roots, held: the decode touches them a frame or more later, when a cell unload may
			// have freed what the snapshot names. Taken while the list jobs had just traversed them (alive), released on
			// the render thread (retiredTags), never on the worker.
			std::vector<RE::NiPointer<RE::NiAVObject>> roots;
		};
		std::vector<std::uint32_t> stoodInScratch;
		// The fade service's verdicts (the decode, on the worker): per entry of the snapshot, whether its root has faded out, and
		// the changes since ApplyFadeChanges; the objects marked faded out (render thread), unmarked on a new snapshot.
		std::vector<std::uint8_t> entryFadedOut;
		std::vector<std::pair<std::uint32_t, bool>> fadeChanges;
		ankerl::unordered_dense::set<std::int32_t> fadedOutObjects;
		std::vector<std::shared_ptr<void>> retiredTags;  // decoded frames' tags: the worker appends, the render thread clears after the join
		/** @brief The decode's counters (worker), read and reset by the report. */
		struct FeedbackCounters
		{
			std::atomic<std::uint64_t> frames{ 0 }, stale{ 0 }, entries{ 0 }, visible{ 0 }, serviced{ 0 }, unresolved{ 0 };
		};
		FeedbackCounters feedbackCounters;

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
	};
}
