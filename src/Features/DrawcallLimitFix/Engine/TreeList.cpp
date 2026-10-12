#include "TreeList.h"

#include "EngineAccess.h"
#include "PrimaryCull.h"
#include "TreeAnimation.h"
#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Draws/FrameValues.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <numeric>

// T7b5: the tree manager's list loop as DCLF's computation, a dry run against the engine's (drawcall-limit-fix.md, "T7b5";
// skyrim-engine-notes.md, "Tree manager list (T7b5)"). TreeList.h describes it.

namespace DCLF::TreeList
{
	namespace
	{
		using namespace Engine;

		// AE 1.6.1170 (module offsets; Ghidra).
		constexpr std::uintptr_t kManager = 0x20F6A18;       // BSTreeManager*
		constexpr std::uintptr_t kListLoop = 0x437e50;       // FUN_140437e50(manager, process): the tree list's loop
		constexpr std::uintptr_t kListLoopCall = 0x437df1;   // its call in FUN_140437d40 (the manager's update, Main::Update 0x140646176)
		constexpr std::uintptr_t kFirstMultiBound = 0x147d640;  // FUN_14147d640(tree): its first child's AsMultiBoundNode (vfunc 0x30), or null
		constexpr std::uintptr_t kNearCap = 0x200d670;       // uint32: the near list's length after the sort (40)
		constexpr std::uintptr_t kPastCapScale = 0x1ad2848;  // float: past the cap, forced to the LOD child at range * this (0.25)
		constexpr std::uintptr_t kBudgetStart = 0x313fa78;   // int64: the update's start (QueryPerformanceCounter), its time budget's base

		// BSTreeManager.
		constexpr std::size_t kTreeLock = 0x10;   // BSSpinLock of the tree list
		constexpr std::size_t kAnimLock = 0x18;   // BSSpinLock of the animation list
		constexpr std::size_t kTreeList = 0x20;   // BSTArray<BSTreeNode*> (data, capacity +0x8, size +0x10)
		constexpr std::size_t kNearList = 0x38;   // BSTArray<BSTreeNode*>: the loop's near trees, emptied after the update
		constexpr std::size_t kAnimList = 0x50;   // BSTArray<BSLeafAnimNode*>
		constexpr std::size_t kArraySize = 0x10;  // a BSTArray's size, from its data pointer
		constexpr std::size_t kRange = 0x80;      // float: the squared distance of the full model (and of the gust)
		// The loop's process (Main::Update's) and the nodes.
		constexpr std::size_t kProcessCamera = 0x18;
		constexpr std::size_t kProcessCullMode = 0x30198;  // 3: every tree visited
		constexpr std::size_t kTranslate = 0xA0;           // NiAVObject world translate
		constexpr std::size_t kFlags = 0xF4;
		constexpr std::uint32_t kFlagAccumulated = 1u << 26;
		constexpr std::size_t kUserData = 0xF8;        // the reference; its +0x40, the base form (the manager's "model")
		constexpr std::size_t kBaseForm = 0x40;
		constexpr std::size_t kDistance = 0x158;       // the squared camera distance the loop wrote
		constexpr std::size_t kMultiBound = 0x128;     // BSMultiBoundNode's BSMultiBound
		constexpr std::size_t kMultiBoundShape = 0x18;  // BSMultiBound's BSMultiBoundShape
		constexpr std::size_t kShapeResult = 0x10;     // the cull result: 0 stale, 1 in, 2 out

		enum : std::uint8_t
		{
			kResultStale = 0,
			kResultIn = 1,
			kResultOut = 2,
			kResultNone = 3,  // no multibound (no first child, or none under it)
		};

		// The loop's outcome for a tree of the tree list.
		enum Class : std::uint8_t
		{
			kSkipped,        // not visited (its multibound not in)
			kKept,           // within range and among the nearest (the near list after the cut: full model, animated)
			kPastCapForced,  // within range, past the cut, at or past range * scale: forced to the LOD child
			kPastCapKept,    // within range, past the cut, nearer: its switch as the loop's first store left it
			kFar,            // past the range: the LOD child
		};
		constexpr std::array<const char*, 5> kClassNames{ "skipped", "kept", "past the cut, forced to LOD", "past the cut", "far" };

		struct EngineTree
		{
			const void* node = nullptr;
			std::uint8_t result = kResultNone;
			bool visited = false;
			bool kept = false;     // on the near list after the loop
			bool written = false;  // a stood-in tree whose result the loop read was DCLF's (TreeAnimation::BeforeListLoop)
			float distance = 0.0f;  // +0x158 after the loop (written only when visited)
		};
		struct EngineLeaf
		{
			const void* node = nullptr;
			bool model = false;
			bool accumulated = false;
			bool dclfStored = false;  // T7f: this entry's kAccumulated write was DCLF's (T7f checklist item 1), not the engine's last cull's
		};
		struct DclfTree
		{
			std::uint32_t slot = 0;
			const void* node = nullptr;
			std::uint8_t plan = 0;  // kFadeRootLeaf or kFadeRootTree (the seed's: the node's OnVisible)
			bool stoodIn = false;
			bool positioned = false;  // a tree (BSTreeNode) with its seed's position
			float distance = 0.0f;
		};
	}

	struct Sample
	{
		std::uint32_t frame = 0;
		std::vector<EngineTree> trees;
		std::vector<EngineLeaf> leaves;
		std::vector<DclfTree> dclf;
		std::array<float, 3> engineEye{}, dclfEye{};
		bool dclfCamera = false;  // PrimaryCull had a camera that frame
		bool allVisited = false;  // the process's cull mode 3
		float range = 0.0f;
		float pastCapScale = 0.0f;
		std::uint32_t cap = 0;
		std::uint32_t nearBefore = 0;  // the near list's size before the loop (0 unless an update was skipped after a loop)
		std::uint32_t unseeded = 0;    // root slots without a seed of their generation: their plan unknown
	};

	void SampleDelete::operator()(Sample* a_sample) const noexcept
	{
		delete a_sample;
	}

	namespace
	{
		bool installed = false;  // the loop's call detoured (TreeAnimation's stood-in trees, and the observer)
		bool sampling = false;   // CS_DCLF_TREE_PARITY: the observer's samples

		// The render thread's: the samples no decode has reached yet, in frame order.
		std::deque<SamplePtr> samples;

		struct First
		{
			const void* node = nullptr;
			std::uint32_t frame = 0;
			const char* what = nullptr;
		};
		auto& firsts = *new EventQueue<First, 64>();

		struct Stats
		{
			std::atomic<std::uint64_t> samples{ 0 }, sampleNs{ 0 }, frames{ 0 }, incomplete{ 0 }, unmatched{ 0 }, nearBefore{ 0 }, allVisited{ 0 }, noCamera{ 0 }, unseeded{ 0 };
			// The tree list (BSTreeNode).
			std::atomic<std::uint64_t> treeListed{ 0 }, treeCompared{ 0 }, treeSame{ 0 }, treeDiffer{ 0 };
			std::atomic<std::uint64_t> causeStale{ 0 }, causeNoMultiBound{ 0 }, causeBound{ 0 }, causeCamera{ 0 }, causeDistance{ 0 }, causeOrder{ 0 };
			std::atomic<std::uint64_t> treeStoodIn{ 0 }, treeStoodInDiffer{ 0 }, treeStoodInStale{ 0 }, treeStoodInIn{ 0 }, treeStoodInOut{ 0 };
			std::atomic<std::uint64_t> treeStoodInWritten{ 0 }, treeStoodInWrittenDiffer{ 0 };  // T7b5's fix: the loop given DCLF's verdict
			std::atomic<std::uint64_t> treeAllWritten{ 0 }, treeAllWrittenDiffer{ 0 };  // T7f preparation, CS_DCLF_TREE_LIST_ALL: engine-culled trees likewise
			std::atomic<std::uint64_t> treeNotDclf{ 0 }, treeNoVerdict{ 0 }, treeNoPosition{ 0 }, treeMoved{ 0 }, treeDclfAlone{ 0 };
			std::array<std::atomic<std::uint64_t>, 5> engineClass{}, dclfClass{};
			// The animation list (BSLeafAnimNode).
			std::atomic<std::uint64_t> leafListed{ 0 }, leafBoth{ 0 }, leafDclfOnly{ 0 }, leafEngineOnly{ 0 }, leafNeither{ 0 }, leafPlan{ 0 };
			std::atomic<std::uint64_t> leafNoModel{ 0 }, leafNotDclf{ 0 }, leafNoVerdict{ 0 }, leafStoodIn{ 0 }, leafMoved{ 0 }, leafOffList{ 0 }, leafDclfAlone{ 0 };
			std::atomic<std::uint64_t> leafDclfStored{ 0 };
		};
		Stats stats;
		std::atomic<std::uint32_t> firstsKept{ 0 };  // firsts pushed since the last report (the decode pushes, the report resets)
		std::string firstNames;        // the render thread's

		std::uint64_t Take(std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); }
		void Add(std::atomic<std::uint64_t>& a_counter, std::uint64_t a_value = 1) { a_counter.fetch_add(a_value, std::memory_order_relaxed); }

		template <class T>
		const RE::BSTArray<T>& List(const std::byte* a_manager, std::size_t a_offset)
		{
			return *reinterpret_cast<const RE::BSTArray<T>*>(a_manager + a_offset);
		}
		RE::BSSpinLock& Lock(std::byte* a_manager, std::size_t a_offset) { return *reinterpret_cast<RE::BSSpinLock*>(a_manager + a_offset); }

		float DistanceSquared(const float* a_position, const std::array<float, 3>& a_eye)
		{
			const float dx = a_position[0] - a_eye[0], dy = a_position[1] - a_eye[1], dz = a_position[2] - a_eye[2];
			return dx * dx + dy * dy + dz * dz;
		}

		/**
		 * @brief The port: FUN_140437e50's decision for each tree of a set (its distance and whether its multibound is in), as the
		 * loop makes it. Within range (strictly nearer) a visited tree is near; the near ones sorted by distance (the engine's
		 * quicksort, FUN_140439820, is not stable: ties may order otherwise), the first a_cap kept; the rest forced to the LOD child at
		 * or past a_range * a_scale. Past the range (or a NaN distance), far.
		 */
		void Decide(const std::vector<float>& a_distance, const std::vector<std::uint8_t>& a_visited, float a_range, std::uint32_t a_cap, float a_scale, std::vector<Class>& a_out)
		{
			const std::size_t count = a_distance.size();
			a_out.assign(count, kSkipped);
			std::vector<std::uint32_t> nearTrees;
			for (std::size_t i = 0; i < count; ++i) {
				if (!a_visited[i])
					continue;
				if (a_range > a_distance[i])
					nearTrees.push_back(static_cast<std::uint32_t>(i));
				else
					a_out[i] = kFar;
			}
			std::stable_sort(nearTrees.begin(), nearTrees.end(), [&](std::uint32_t a_a, std::uint32_t a_b) { return a_distance[a_a] < a_distance[a_b]; });
			const float forced = a_range * a_scale;
			for (std::size_t n = 0; n < nearTrees.size(); ++n)
				a_out[nearTrees[n]] = n < a_cap ? kKept : (forced <= a_distance[nearTrees[n]] ? kPastCapForced : kPastCapKept);
		}

		/** @brief The engine's outcome for a sampled tree (the loop's own tests on its own results). */
		Class EngineClass(const EngineTree& a_tree, const Sample& a_sample)
		{
			if (!a_tree.visited)
				return kSkipped;
			if (!(a_sample.range > a_tree.distance))
				return kFar;
			if (a_tree.kept)
				return kKept;
			return a_sample.range * a_sample.pastCapScale <= a_tree.distance ? kPastCapForced : kPastCapKept;
		}

		void KeepFirst(const void* a_node, std::uint32_t a_frame, const char* a_what)
		{
			if (firstsKept.fetch_add(1, std::memory_order_relaxed) >= 4)
				return;
			firsts.Push({ a_node, a_frame, a_what });
		}

		/**
		 * @brief Main thread, right after the loop (FUN_140437d40 has not yet run FUN_1404381e0, nor emptied the near list): the engine's
		 * answer, and DCLF's inputs from the frame's tables. The frame is the last scene frame's (SceneStore's, BeginFrame's): its cull is
		 * the one the loop read.
		 */
		void TakeSample(std::byte* a_manager, const std::byte* a_process, std::uint32_t a_nearBefore)
		{
			const std::uint32_t frame = SceneStore::Get().GetFrame();
			if (!frame)
				return;
			SamplePtr sample(new Sample());
			sample->frame = frame;
			sample->nearBefore = a_nearBefore;
			sample->range = At<float>(a_manager, kRange);
			sample->cap = Global<std::uint32_t>(kNearCap);
			sample->pastCapScale = Global<float>(kPastCapScale);
			sample->allVisited = At<std::int32_t>(a_process, kProcessCullMode) == 3;
			if (const auto* camera = At<const std::byte*>(a_process, kProcessCamera)) {
				const auto* t = &At<float>(camera, kTranslate);
				sample->engineEye = { t[0], t[1], t[2] };
			}

			// The engine's: the tree list (the loop reads it without its lock; taken here against the adds of other threads).
			using FirstMultiBound = const std::byte* (*)(const void*);
			static const auto firstMultiBound = reinterpret_cast<FirstMultiBound>(REL::Module::get().base() + kFirstMultiBound);
			ankerl::unordered_dense::set<const void*> kept;
			{
				const auto& nearList = List<const void*>(a_manager, kNearList);
				for (std::uint32_t i = 0; i < nearList.size(); ++i)
					kept.insert(nearList[i]);
			}
			{
				auto& lock = Lock(a_manager, kTreeLock);
				lock.Lock();
				const auto& list = List<const void*>(a_manager, kTreeList);
				sample->trees.reserve(list.size());
				for (std::uint32_t i = 0; i < list.size(); ++i) {
					const void* node = list[i];
					if (!node)
						continue;
					EngineTree tree;
					tree.node = node;
					if (const auto* multiBound = firstMultiBound(node))
						if (const auto* bound = At<const std::byte*>(multiBound, kMultiBound))
							if (const auto* shape = At<const std::byte*>(bound, kMultiBoundShape))
								tree.result = static_cast<std::uint8_t>(std::min<std::int32_t>(At<std::int32_t>(shape, kShapeResult), kResultNone));
					tree.visited = tree.result != kResultNone && (tree.result == kResultIn || sample->allVisited);
					tree.written = TreeAnimation::WroteResult(node);
					tree.kept = kept.contains(node);
					tree.distance = At<float>(node, kDistance);
					sample->trees.push_back(tree);
				}
				lock.Unlock();
			}
			// The animation list: the clock's own tests (a base form, kAccumulated), read under the list's lock (the list holds each node).
			{
				auto& lock = Lock(a_manager, kAnimLock);
				lock.Lock();
				const auto& list = List<const void*>(a_manager, kAnimList);
				sample->leaves.reserve(list.size());
				for (std::uint32_t i = 0; i < list.size(); ++i) {
					const void* node = list[i];
					if (!node)
						continue;
					const auto* reference = At<const std::byte*>(node, kUserData);
					sample->leaves.push_back({ node, reference && At<const void*>(reference, kBaseForm), (At<std::uint32_t>(node, kFlags) & kFlagAccumulated) != 0,
						IndirectDraws::Get().AccumulatedStoredNow(node) });
				}
				lock.Unlock();
			}

			// DCLF's inputs: the frame's tables (the installed publication's, the one the frame's readback was armed with), the seeds
			// (the nodes' values as DCLF sampled them at the frame's start) and PrimaryCull's list camera of the frame.
			const auto eye = PrimaryCull::Get().FadeEye();
			sample->dclfCamera = eye[3] != 0.0f;
			sample->dclfEye = { eye[0], eye[1], eye[2] };
			const auto& tables = SceneStore::Get().GetTables();
			const auto* seeds = FrameValues::Get().FadeSeedsIfDone();
			const auto* treeSeeds = FrameValues::Get().TreeSeedsIfDone();
			for (std::size_t r = 0; r < tables.fadeRootNode.size() && r < tables.fadeRoots.size(); ++r) {
				const void* node = tables.fadeRootNode[r];
				if (!node)
					continue;
				const auto& row = tables.fadeRoots[r];
				FadeRootStatic merged;
				std::uint32_t plan = row.bits & kFadeRootPlanMask;  // the coordinator's guess: BSTreeNode's from the mirror, else BSFadeNode's
				if (seeds && MergeFadeSeed(row, seeds->data(), seeds->size(), static_cast<std::uint32_t>(r), merged))
					plan = merged.bits & kFadeRootPlanMask;
				else if (plan != kFadeRootTree)
					++sample->unseeded;
				if (plan != kFadeRootLeaf && plan != kFadeRootTree)
					continue;
				DclfTree tree;
				tree.slot = static_cast<std::uint32_t>(r);
				tree.node = node;
				tree.plan = static_cast<std::uint8_t>(plan);
				tree.stoodIn = (row.bits & kFadeRootStoodIn) != 0;
				if (plan == kFadeRootTree && sample->dclfCamera && treeSeeds)
					if (const auto it = tables.treeIndex.find(node); it != tables.treeIndex.end() && it->second < tables.trees.size()) {
						TreeStatic seed;
						if (MergeTreeSeed(tables.trees[it->second], treeSeeds->data(), treeSeeds->size(), it->second, seed)) {
							tree.positioned = true;
							tree.distance = DistanceSquared(seed.position, sample->dclfEye);
						}
					}
				sample->dclf.push_back(tree);
			}
			samples.push_back(std::move(sample));
		}

		struct Hooks
		{
			struct ListLoop
			{
				static void thunk(std::byte* a_manager, std::byte* a_process)
				{
					const std::uint32_t nearBefore = a_manager ? List<const void*>(a_manager, kNearList).size() : 0u;
					// T7b5's fix: the stood-in trees' multibound results DCLF's for the loop, the engine's put back after it (and the sample).
					TreeAnimation::BeforeListLoop(a_manager);
					func(a_manager, a_process);
					if (!sampling || !a_manager || !a_process) {
						TreeAnimation::AfterListLoop();
						return;
					}
					const std::int64_t start = Now();
					TakeSample(a_manager, a_process, nearBefore);
					TreeAnimation::AfterListLoop();
					// The observer's time given back to the update's budget (FUN_1404381e0 stops at it): the engine animates as without it.
					const std::int64_t spent = Now() - start;
					Global<std::int64_t>(kBudgetStart) += spent;
					LARGE_INTEGER frequency{};
					QueryPerformanceFrequency(&frequency);
					Add(stats.samples);
					Add(stats.sampleNs, static_cast<std::uint64_t>(double(spent) * 1e9 / double(frequency.QuadPart)));
				}
				static inline REL::Relocation<decltype(thunk)> func;
			};
		};
	}

	void Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		if (!CallsTo(base + kListLoopCall, base + kListLoop)) {
			logger::warn("[DCLF] tree manager (T7b5) not installed: the instruction at {:#x} is not a call of FUN_140437e50; stood-in trees keep the stale multibound result", 0x140000000 + kListLoopCall);
			return;
		}
		// Always (T7b5's fix: TreeAnimation gives the loop the stood-in trees' verdicts); the observer only with its switch.
		stl::write_thunk_call<Hooks::ListLoop>(base + kListLoopCall);
		installed = true;
		sampling = SwitchEnabled(Switch::TreeParity);
		logger::info("[DCLF] tree manager: the list loop's call detoured (stood-in trees' multibound results from DCLF's verdicts){}",
			sampling ? "; CS_DCLF_TREE_PARITY: compared with DCLF's port, frame by frame (T7b5)" : "");
	}

	bool Installed() { return installed; }

	std::uint32_t FrameOf(const Sample& a_sample) { return a_sample.frame; }

	std::vector<SamplePtr> TakeSamples(std::uint32_t a_through)
	{
		std::vector<SamplePtr> taken;
		while (!samples.empty() && samples.front()->frame <= a_through) {
			taken.push_back(std::move(samples.front()));
			samples.pop_front();
		}
		return taken;
	}

	std::uint64_t DropSamples(std::uint32_t a_oldestPending)
	{
		std::uint64_t dropped = 0;
		while (!samples.empty() && samples.front()->frame < a_oldestPending) {
			samples.pop_front();
			++dropped;
		}
		Add(stats.unmatched, dropped);
		return dropped;
	}

	namespace
	{
		void Compare(const Sample& a_sample, const std::function<const void*(std::uint32_t)>& a_rootNode, const std::function<RootVerdict(std::uint32_t)>& a_verdict)
		{
			auto& s = stats;
			Add(s.frames);
			Add(s.nearBefore, a_sample.nearBefore ? 1 : 0);
			Add(s.allVisited, a_sample.allVisited ? 1 : 0);
			Add(s.noCamera, a_sample.dclfCamera ? 0 : 1);
			Add(s.unseeded, a_sample.unseeded);

			// DCLF's trees: whose slot still holds the node in the batch's tables, with the decoder's verdicts.
			const std::size_t count = a_sample.dclf.size();
			std::vector<std::uint8_t> moved(count, 0u), known(count, 0u), visible(count, 0u);
			ankerl::unordered_dense::map<const void*, std::uint32_t> byNode;
			byNode.reserve(count);
			for (std::size_t i = 0; i < count; ++i) {
				const auto& tree = a_sample.dclf[i];
				byNode.emplace(tree.node, static_cast<std::uint32_t>(i));
				if (a_rootNode(tree.slot) != tree.node) {
					moved[i] = 1;
					continue;
				}
				const auto verdict = a_verdict(tree.slot);
				known[i] = verdict.known;
				visible[i] = verdict.visible;
			}

			// The port, over DCLF's trees (BSTreeNode's) with a verdict and a position: stood-in ones too (the engine keeps them on its
			// tree list, under a multibound no cull reaches).
			std::vector<std::uint32_t> ported;
			std::vector<float> distance;
			std::vector<std::uint8_t> visited;
			std::vector<std::int32_t> portIndex(count, -1);
			for (std::size_t i = 0; i < count; ++i) {
				const auto& tree = a_sample.dclf[i];
				if (tree.plan != kFadeRootTree || moved[i] || !known[i] || !tree.positioned)
					continue;
				portIndex[i] = static_cast<std::int32_t>(ported.size());
				ported.push_back(static_cast<std::uint32_t>(i));
				distance.push_back(tree.distance);
				visited.push_back(visible[i]);
			}
			std::vector<Class> dclfClass;
			Decide(distance, visited, a_sample.range, a_sample.cap, a_sample.pastCapScale, dclfClass);

			const float eyeDx = a_sample.engineEye[0] - a_sample.dclfEye[0], eyeDy = a_sample.engineEye[1] - a_sample.dclfEye[1], eyeDz = a_sample.engineEye[2] - a_sample.dclfEye[2];
			const bool cameraDiffers = eyeDx * eyeDx + eyeDy * eyeDy + eyeDz * eyeDz > 1.0f;

			// The tree list.
			Add(s.treeListed, a_sample.trees.size());
			ankerl::unordered_dense::set<const void*> onTreeList;
			onTreeList.reserve(a_sample.trees.size());
			for (const auto& tree : a_sample.trees) {
				onTreeList.insert(tree.node);
				const auto it = byNode.find(tree.node);
				if (it == byNode.end() || a_sample.dclf[it->second].plan != kFadeRootTree) {
					Add(s.treeNotDclf);
					continue;
				}
				const std::uint32_t i = it->second;
				if (moved[i]) {
					Add(s.treeMoved);
					continue;
				}
				if (!known[i]) {
					Add(s.treeNoVerdict);
					continue;
				}
				if (portIndex[i] < 0) {
					Add(s.treeNoPosition);
					continue;
				}
				const Class engine = EngineClass(tree, a_sample);
				const auto port = static_cast<std::size_t>(portIndex[i]);
				const Class dclf = dclfClass[port];
				if (tree.written && !a_sample.dclf[i].stoodIn) {
					// T7f preparation, CS_DCLF_TREE_LIST_ALL: the loop read DCLF's verdict on a tree the engine culls too - aside, as a
					// stood-in one given DCLF's verdict (the rule's window differs from the port's frame by design).
					Add(s.treeAllWritten);
					if (engine != dclf) {
						Add(s.treeAllWrittenDiffer);
						KeepFirst(tree.node, a_sample.frame, "written (CS_DCLF_TREE_LIST_ALL)");
					}
					continue;
				}
				if (a_sample.dclf[i].stoodIn) {
					// The cut stands in for the root: no cull reaches its multibound. Since T7b5's fix the loop reads DCLF's verdict there
					// (TreeAnimation: the root's kAccumulated as T7b2 wrote it, the conservative two-frame rule) - compared with the port
					// on the decoder's verdict of the frame, aside (the rule's window and a new listing's "visible" differ by design). One
					// the loop read unwritten (DCLF off, no multibound) keeps the stale cache's classes.
					Add(s.treeStoodIn);
					if (tree.written) {
						Add(s.treeStoodInWritten);
						if (engine != dclf) {
							Add(s.treeStoodInWrittenDiffer);
							KeepFirst(tree.node, a_sample.frame, "stood in, written");
						}
						continue;
					}
					Add(tree.result == kResultStale ? s.treeStoodInStale : tree.result == kResultIn ? s.treeStoodInIn : s.treeStoodInOut);
					if (engine != dclf)
						Add(s.treeStoodInDiffer);
					continue;
				}
				Add(s.treeCompared);
				Add(s.engineClass[engine]);
				Add(s.dclfClass[dclf]);
				if (engine == dclf) {
					Add(s.treeSame);
					continue;
				}
				Add(s.treeDiffer);
				const char* cause;
				if ((engine == kSkipped) != (dclf == kSkipped)) {
					cause = tree.result == kResultStale ? "multibound stale" : tree.result == kResultNone ? "no multibound" : "bound";
					Add(tree.result == kResultStale ? s.causeStale : tree.result == kResultNone ? s.causeNoMultiBound : s.causeBound);
				} else if (cameraDiffers) {
					cause = "camera";
					Add(s.causeCamera);
				} else if (std::fabs(tree.distance - distance[port]) > 1e-3f * std::max(1.0f, tree.distance)) {
					cause = "distance";
					Add(s.causeDistance);
				} else {
					cause = "order";
					Add(s.causeOrder);
				}
				KeepFirst(tree.node, a_sample.frame, cause);
			}
			for (std::size_t i = 0; i < count; ++i)
				if (a_sample.dclf[i].plan == kFadeRootTree && !onTreeList.contains(a_sample.dclf[i].node))
					Add(s.treeDclfAlone);

			// The animation list: the clock's gate (a base form and kAccumulated) against DCLF's verdict.
			Add(s.leafListed, a_sample.leaves.size());
			ankerl::unordered_dense::set<const void*> onAnimList;
			onAnimList.reserve(a_sample.leaves.size());
			for (const auto& leaf : a_sample.leaves) {
				onAnimList.insert(leaf.node);
				const auto it = byNode.find(leaf.node);
				if (it == byNode.end()) {
					Add(s.leafNotDclf);
					continue;
				}
				const std::uint32_t i = it->second;
				if (moved[i]) {
					Add(s.leafMoved);
					continue;
				}
				if (a_sample.dclf[i].stoodIn) {
					// TreeAnimation takes the stood-in roots off the list; one still on it carries DCLF's own write (T7b2).
					Add(s.leafStoodIn);
					continue;
				}
				if (!known[i]) {
					Add(s.leafNoVerdict);
					continue;
				}
				if (!leaf.model) {
					Add(s.leafNoModel);
					continue;
				}
				// T7f: DCLF stored the bit at this Main::Update's entry (its conservative rule): not the engine's cull.
				if (leaf.dclfStored) {
					Add(s.leafDclfStored);
					continue;
				}
				if (a_sample.dclf[i].plan != kFadeRootLeaf)
					Add(s.leafPlan);
				const bool dclf = visible[i] != 0;
				Add(dclf ? (leaf.accumulated ? s.leafBoth : s.leafDclfOnly) : (leaf.accumulated ? s.leafEngineOnly : s.leafNeither));
				if (dclf != leaf.accumulated)
					KeepFirst(leaf.node, a_sample.frame, dclf ? "clock: DCLF's alone" : "clock: the engine's alone");
			}
			for (std::size_t i = 0; i < count; ++i)
				if (a_sample.dclf[i].plan == kFadeRootLeaf && !onAnimList.contains(a_sample.dclf[i].node))
					Add(a_sample.dclf[i].stoodIn ? s.leafOffList : s.leafDclfAlone);
		}
	}

	void OnBatch(const std::vector<SamplePtr>& a_samples, std::size_t& a_next, std::uint32_t a_frame, bool a_complete,
		const std::function<const void*(std::uint32_t)>& a_rootNode, const std::function<RootVerdict(std::uint32_t)>& a_verdict)
	{
		for (; a_next < a_samples.size() && a_samples[a_next]->frame < a_frame; ++a_next)
			Add(stats.unmatched);
		if (a_next < a_samples.size() && a_samples[a_next]->frame == a_frame) {
			// A batch past its list leaves some verdicts for the next frame: not compared.
			if (a_complete)
				Compare(*a_samples[a_next], a_rootNode, a_verdict);
			else
				Add(stats.incomplete);
			++a_next;
		}
	}

	void OnDecodeEnd(const std::vector<SamplePtr>& a_samples, std::size_t a_next)
	{
		Add(stats.unmatched, a_samples.size() - std::min(a_next, a_samples.size()));
	}

	void NameFirsts()
	{
		firsts.Drain([](First&& a_first) {
			if (firstNames.size() > 600 || !a_first.node)
				return;
			const auto* object = static_cast<const RE::NiAVObject*>(a_first.node);
			firstNames += fmt::format("{}'{}' ({}, frame {})", firstNames.empty() ? "" : ", ", object->name.c_str() ? object->name.c_str() : "?", a_first.what, a_first.frame);
		});
	}

	std::string Report()
	{
		if (!sampling)
			return {};
		auto& s = stats;
		const auto samplesTaken = Take(s.samples), sampleNs = Take(s.sampleNs), frames = Take(s.frames), incomplete = Take(s.incomplete), unmatched = Take(s.unmatched);
		const auto nearBefore = Take(s.nearBefore), allVisited = Take(s.allVisited), noCamera = Take(s.noCamera), unseeded = Take(s.unseeded);
		const auto treeListed = Take(s.treeListed), treeCompared = Take(s.treeCompared), treeSame = Take(s.treeSame), treeDiffer = Take(s.treeDiffer);
		const auto stale = Take(s.causeStale), noMultiBound = Take(s.causeNoMultiBound), bound = Take(s.causeBound), camera = Take(s.causeCamera), distance = Take(s.causeDistance),
				   order = Take(s.causeOrder);
		const auto stoodIn = Take(s.treeStoodIn), stoodInDiffer = Take(s.treeStoodInDiffer), stoodInStale = Take(s.treeStoodInStale), stoodInIn = Take(s.treeStoodInIn),
				   stoodInOut = Take(s.treeStoodInOut), stoodInWritten = Take(s.treeStoodInWritten), stoodInWrittenDiffer = Take(s.treeStoodInWrittenDiffer);
		const auto allWritten = Take(s.treeAllWritten), allWrittenDiffer = Take(s.treeAllWrittenDiffer);
		const auto notDclf = Take(s.treeNotDclf), noVerdict = Take(s.treeNoVerdict), noPosition = Take(s.treeNoPosition), treeMoved = Take(s.treeMoved), dclfAlone = Take(s.treeDclfAlone);
		std::string classes;
		for (std::size_t c = 0; c < kClassNames.size(); ++c)
			classes += fmt::format("{}{} {}/{}", c ? ", " : "", kClassNames[c], Take(s.engineClass[c]), Take(s.dclfClass[c]));
		const auto leafListed = Take(s.leafListed), both = Take(s.leafBoth), dclfOnly = Take(s.leafDclfOnly), engineOnly = Take(s.leafEngineOnly), neither = Take(s.leafNeither),
				   plan = Take(s.leafPlan);
		const auto noModel = Take(s.leafNoModel), leafNotDclf = Take(s.leafNotDclf), leafNoVerdict = Take(s.leafNoVerdict), leafStoodIn = Take(s.leafStoodIn), leafMoved = Take(s.leafMoved),
				   offList = Take(s.leafOffList), leafAlone = Take(s.leafDclfAlone), leafStored = Take(s.leafDclfStored);
		firstsKept.store(0, std::memory_order_relaxed);
		const std::uint64_t compared = treeCompared + both + dclfOnly + engineOnly + neither;
		const std::uint64_t differ = treeDiffer + dclfOnly + engineOnly;
		const double perFrame = frames ? 1.0 / double(frames) : 0.0;
		std::string text = fmt::format(
			"[DCLF] tree manager (T7b5): {} samples ({:.1f} us each), {} frames compared ({} left out: a batch past its list; {} samples no batch reached; {} with the near list "
			"not empty before the loop, {} with cull mode 3, {} without DCLF's camera, {:.1f} root slots a frame unseeded); tree list (BSTreeNode) {:.1f} a frame: {} compared, "
			"{} same, {} differ (visited: {} multibound stale, {} no multibound, {} bound; class: {} camera, {} distance, {} order), classes engine/DCLF: {}; stood in {} "
			"({} given DCLF's verdict, {} of them differ; unwritten: multibound {} stale / {} in / {} out, {} differ), aside {} given DCLF's verdict under "
			"CS_DCLF_TREE_LIST_ALL ({} differ), {} not DCLF's trees, {} no verdict, {} no position, {} moved, {} DCLF's alone; animation list "
			"(BSLeafAnimNode) {:.1f} a frame: clock {} both / {} DCLF's alone / {} the engine's alone / {} neither ({} not BSLeafAnimNode's plan in DCLF), aside {} no base "
			"form, {} not DCLF's roots, {} no verdict, {} stood in still on the list, {} moved, {} stored by DCLF at the entry; DCLF's leaf "
			"roots off the list {} (stood in), {} not on it{}{}\n",
			samplesTaken, samplesTaken ? double(sampleNs) / double(samplesTaken) / 1000.0 : 0.0, frames, incomplete, unmatched, nearBefore, allVisited, noCamera,
			double(unseeded) * perFrame, double(treeListed) * perFrame, treeCompared, treeSame, treeDiffer, stale, noMultiBound, bound, camera, distance, order, classes, stoodIn,
			stoodInWritten, stoodInWrittenDiffer, stoodInStale, stoodInIn, stoodInOut, stoodInDiffer, allWritten, allWrittenDiffer, notDclf, noVerdict, noPosition, treeMoved, dclfAlone, double(leafListed) * perFrame, both, dclfOnly, engineOnly,
			neither, plan, noModel, leafNotDclf, leafNoVerdict, leafStoodIn, leafMoved, leafStored, offList, leafAlone,
			!compared ? " <- NOT COMPARED" : differ ? " <- TREE MANAGER" : " <- OK", firstNames.empty() ? "" : "; first: " + firstNames);
		firstNames.clear();
		return text;
	}
}
