#include "LocalLightCull.h"

#include "EngineAccess.h"
#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "PassCapture.h"
#include "PrimaryCull.h"
#include "SunAccumulation.h"

#include <array>
#include <atomic>
#include <mutex>

// The point lights' shadow culls without DCLF's entries: LocalLightCull.h describes it.

namespace DCLF::LocalLightCull
{
	namespace
	{
		constexpr std::uintptr_t kParabolicProcess1 = 0x1519a90;  // BSParabolicCullingProcess::Process1, vtable slot 0x16
		constexpr std::uint32_t kParabolicMode = 0xF - PassCapture::kFirstShadowMode;

		bool installed = false;
		bool probe = false;
		// The exclusion the next frame applies (render thread), and this frame's, held until the next selection: the culls
		// that read it run between the two on the render thread.
		std::shared_ptr<SunExclusion> pending;
		std::shared_ptr<SunExclusion> frameHeld;
		std::atomic<const SunExclusion*> frameExclusion{ nullptr };
		std::atomic<bool> parityFrame{ false };
		std::mutex firstMutex;  // the parities' first lost pass and first missed caster
		std::string firstLost;
		std::array<std::uint64_t, 16> lostByReject{};

		struct Stats
		{
			std::atomic<std::uint64_t> visited{ 0 }, skipped{ 0 }, wouldSkip{ 0 }, parityPasses{ 0 }, parityLost{ 0 };
			std::uint64_t frames = 0, live = 0, noExclusion = 0, noClaims = 0, stale = 0, parityFrames = 0;
		};
		Stats stats;

		bool Excluded(const SunExclusion& a_exclusion, const void* a_object)
		{
			const auto& entries = a_exclusion.candidates->entries;
			const auto it = entries.find(static_cast<const RE::NiAVObject*>(a_object));
			return it != entries.end() && a_exclusion.excluded[it->second];
		}

		// ---- The light list (LocalLightCull.h).
		constexpr std::size_t kSceneAccumArray = 0x528;  // BSShadowLight::sceneAccumArray (AE)
		constexpr std::uint32_t kListDepth = 3;          // the object root's entries: object root, cell, category node, entry
		using NodeArray = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;
		// The array's own layout, to lend it the list's storage for one call (BSTArray: data, capacity, size).
		struct ArrayHeader
		{
			void* data = nullptr;
			std::uint32_t capacity = 0, pad0 = 0;
			std::uint32_t size = 0, pad1 = 0;
		};
		static_assert(sizeof(ArrayHeader) == sizeof(NodeArray));

		bool listInstalled = false;
		struct StructureEvent
		{
			RE::NiPointer<RE::NiAVObject> held;  // an attach's child, alive until applied (then released at Present)
			const RE::NiAVObject* child = nullptr;
			const RE::NiAVObject* parent = nullptr;
			std::uint32_t depth = 0;  // the parent's below the object root
			bool attached = false;
		};
		EventQueue<StructureEvent> structureEvents;
		std::atomic<const RE::NiAVObject*> listObjectRoot{ nullptr };
		// Events pushed and drained: a light uses the list only while the two are equal (nothing moved since the selection).
		std::atomic<std::uint64_t> eventsPushed{ 0 };
		std::uint64_t eventsDrained = 0;
		std::atomic<bool> hiddenMoved{ false };  // a hidden bit on a node the build went through, since the build

		struct LightList
		{
			std::vector<RE::NiPointer<RE::NiAVObject>> roots;
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> position;  // into roots
			std::vector<std::uint32_t> rootParent;                                         // per root: its parent, into throughNodes
			// The plain nodes gone through, and each one's parent (kNoParent: the object root's): a lent list tests a root's
			// parents as the walk would have (ParentsVisible).
			ankerl::unordered_dense::map<const void*, std::uint32_t> through;  // into throughNodes
			std::vector<RE::NiAVObject*> throughNodes;
			std::vector<std::uint32_t> throughParent;
			ankerl::unordered_dense::set<const void*> watched;  // through, and the hidden ones not
			const RE::NiAVObject* objectRoot = nullptr;
			const SunCandidates* candidates = nullptr;
			std::uint64_t exclusionVersion = ~0ull;
			bool valid = false;
		};
		LightList lightList;
		std::vector<RE::NiPointer<RE::NiAVObject>> listGraveyard;
		std::atomic<bool> listLive{ false };  // this frame's lights may use it
		thread_local const void* lentArray = nullptr;
		constexpr std::uint32_t kNoParent = ~0u;
		// Per node gone through, this light's verdict (lent list only): -1 not tested yet, 0 culled, 1 visible.
		thread_local std::vector<std::int8_t> parentVerdicts;

		struct ListStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, listed{ 0 }, walkedMoved{ 0 }, walkedOwn{ 0 }, walkedStrict{ 0 }, walkedNotLive{ 0 }, listEntries{ 0 };
			std::atomic<std::uint64_t> parityChecked{ 0 }, parityMissed{ 0 }, parentCulled{ 0 };
			std::uint64_t builds = 0, added = 0, removed = 0;
			std::int64_t buildTicks = 0;
		};
		ListStats listStats;
		std::string listFirstMissed;  // under firstMutex

		bool Hidden(const RE::NiAVObject* a_object) { return a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden); }

		/**
		 * @brief A node whose OnVisible, for a light that is not portal-strict, only culls its children: exactly NiNode, or a
		 * BSMultiBoundNode (an exterior cell: its OnVisible, AE 0x140e2d710, makes no multibound test and keeps the cull mode
		 * under cull mode 3, which such a light's cull has); not preprocessed (BSCullingProcess::Process1, AE 0x140e28390: a
		 * preprocessed node with bit 20 is not visited at all). kAlwaysDraw (the object root has it) skips only the node's own
		 * bound test: its children are tested one by one either way.
		 */
		bool Plain(RE::NiAVObject* a_object)
		{
			static const REL::Relocation<const RE::NiRTTI*> niNode{ RE::NiNode::Ni_RTTI };
			static const REL::Relocation<const RE::NiRTTI*> multiBoundNode{ RE::BSMultiBoundNode::Ni_RTTI };
			constexpr std::uint32_t kPreProcessed = 1u << 12;
			const auto* rtti = a_object->GetRTTI();
			return (rtti == niNode.get() || rtti == multiBoundNode.get()) && !(a_object->GetFlags().underlying() & kPreProcessed);
		}

		void ListRoot(RE::NiAVObject* a_object, std::uint32_t a_parent)
		{
			lightList.position.insert_or_assign(a_object, static_cast<std::uint32_t>(lightList.roots.size()));
			lightList.roots.emplace_back(a_object);
			lightList.rootParent.push_back(a_parent);
		}

		void UnlistRoot(const RE::NiAVObject* a_object)
		{
			const auto it = lightList.position.find(a_object);
			if (it == lightList.position.end())
				return;
			const std::uint32_t at = it->second;
			lightList.position.erase(it);
			listGraveyard.push_back(std::move(lightList.roots[at]));
			if (at + 1 != lightList.roots.size()) {
				lightList.roots[at] = std::move(lightList.roots.back());
				lightList.rootParent[at] = lightList.rootParent.back();
				lightList.position[lightList.roots[at].get()] = at;
			}
			lightList.roots.pop_back();
			lightList.rootParent.pop_back();
			++listStats.removed;
		}

		void Visit(RE::NiAVObject* a_object, std::uint32_t a_depth, std::uint32_t a_index, std::uint32_t a_parent, const SunExclusion& a_exclusion)
		{
			if (!a_object || Excluded(a_exclusion, a_object))
				return;
			// The object root's children 0 and 1 are whole entries (the main lists' rule): no event reports what is under them.
			const bool whole = a_depth == 1 && a_index < 2;
			auto* node = a_depth < kListDepth && !whole && Plain(a_object) ? a_object->AsNode() : nullptr;
			if (!node) {
				ListRoot(a_object, a_parent);  // its own hidden bit is the list cull's (OwnsList)
				return;
			}
			lightList.watched.insert(node);
			if (Hidden(node))
				return;  // nothing under it is culled; unhidden, it is built again
			const auto index = static_cast<std::uint32_t>(lightList.throughNodes.size());
			lightList.through.emplace(node, index);
			lightList.throughNodes.push_back(node);
			lightList.throughParent.push_back(a_parent);
			const auto& children = node->GetChildren();
			for (std::uint16_t i = 0; i < children.free_idx(); ++i)
				Visit(children[i].get(), a_depth + 1, i, index, a_exclusion);
		}

		void BuildList(RE::NiAVObject* a_objectRoot, const SunExclusion& a_exclusion)
		{
			LARGE_INTEGER start{}, end{};
			QueryPerformanceCounter(&start);
			for (auto& root : lightList.roots)
				listGraveyard.push_back(std::move(root));
			lightList.roots.clear();
			lightList.position.clear();
			lightList.rootParent.clear();
			lightList.through.clear();
			lightList.throughNodes.clear();
			lightList.throughParent.clear();
			lightList.watched.clear();
			Visit(a_objectRoot, 0, 0, kNoParent, a_exclusion);
			lightList.objectRoot = a_objectRoot;
			lightList.candidates = a_exclusion.candidates.get();
			lightList.exclusionVersion = a_exclusion.version;
			lightList.valid = true;
			hiddenMoved.store(false, std::memory_order_relaxed);
			QueryPerformanceCounter(&end);
			++listStats.builds;
			listStats.buildTicks += end.QuadPart - start.QuadPart;
		}

		/** @brief Render thread, at the selection: the events since, applied; the list built again where they cannot be. */
		void UpdateList(const SunExclusion* a_exclusion)
		{
			const auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
			RE::NiAVObject* objectRoot = nullptr;
			if (sceneNode)
				if (const auto& scene = sceneNode->GetChildren(); scene.free_idx() > 3)
					objectRoot = scene[3].get();
			listObjectRoot.store(objectRoot, std::memory_order_release);
			bool rebuild = !lightList.valid || lightList.objectRoot != objectRoot || hiddenMoved.load(std::memory_order_relaxed);
			const std::uint64_t pushed = eventsPushed.load(std::memory_order_acquire);
			structureEvents.Drain([&](StructureEvent&& a_event) {
				++eventsDrained;
				RE::NiPointer<RE::NiAVObject> held = std::move(a_event.held);
				if (held)
					listGraveyard.push_back(std::move(held));
				const auto parent = rebuild ? lightList.through.end() : lightList.through.find(a_event.parent);
				if (parent == lightList.through.end())
					return;  // under a node listed whole, or not gone through: nothing listed moved
				if (a_event.depth + 1 < kListDepth) {
					rebuild = true;  // a node the build goes through
					return;
				}
				if (!a_event.attached) {
					UnlistRoot(a_event.child);
					return;
				}
				// A new entry under a category node: listed unless excluded (an exclusion built since lists it again).
				auto* child = const_cast<RE::NiAVObject*>(a_event.child);
				if (!lightList.position.contains(child) && a_exclusion && !Excluded(*a_exclusion, child)) {
					ListRoot(child, parent->second);
					++listStats.added;
				}
			});
			(void)pushed;
			if (!a_exclusion || !objectRoot) {
				lightList.valid = false;
				return;
			}
			rebuild = rebuild || lightList.candidates != a_exclusion->candidates.get() || lightList.exclusionVersion != a_exclusion->version;
			if (rebuild)
				BuildList(objectRoot, *a_exclusion);
		}

		/**
		 * @brief A lent list's root reached: whether the walk would have reached it, its parents' tests as Process1 makes them for
		 * a light that is not portal-strict (BSParabolicCullingProcess::Process1, AE 0x141519a90, cull mode 3: a node with
		 * kAlwaysDraw is visited untested, any other when FUN_14151a1e0 finds its bound in the light's volume), each made once
		 * per light, and a visited one marked kAccumulated where the process updates it (+0x11D).
		 */
		bool ParentsVisible(RE::NiCullingProcess* a_process, std::uint32_t a_parent)
		{
			if (a_parent == kNoParent || a_parent >= parentVerdicts.size())
				return true;
			auto& verdict = parentVerdicts[a_parent];
			if (verdict < 0) {
				bool visible = ParentsVisible(a_process, lightList.throughParent[a_parent]);
				if (visible) {
					using BoundTest = std::uint8_t (*)(RE::NiCullingProcess*, const RE::NiBound*);
					static const REL::Relocation<BoundTest> boundTest{ REL::Offset(0x151a1e0) };
					auto* node = lightList.throughNodes[a_parent];
					constexpr std::uint32_t kAlwaysDraw = 1u << 11, kAccumulatedFlag = 1u << 26;
					visible = (node->GetFlags().underlying() & kAlwaysDraw) || boundTest(a_process, &node->worldBound) != 0;
					if (visible && reinterpret_cast<const std::uint8_t*>(a_process)[0x11D])
						std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(node) + 0xF4)).fetch_or(kAccumulatedFlag, std::memory_order_relaxed);
				}
				verdict = visible ? 1 : 0;
			}
			return verdict != 0;
		}

		/** @brief Whether a geometry is under a listed root (parity frames: the list is not lent, the object root is walked). */
		bool UnderListedRoot(const RE::NiAVObject* a_object, bool& a_underObjectRoot)
		{
			a_underObjectRoot = false;
			const auto* objectRoot = lightList.objectRoot;
			for (const auto* object = a_object; object; object = object->parent) {
				if (lightList.position.contains(object))
					return true;
				if (object == objectRoot)
					a_underObjectRoot = true;
			}
			return false;
		}

		// BSShadowParabolicLight::Accumulate (vtable slot 9, 0x14151b960): the point lights' cull and registration, timed; a light
		// without a root list of its own is lent the light list for the call.
		std::atomic<std::int64_t> accumulateTicks{ 0 };
		std::atomic<std::uint64_t> accumulateCalls{ 0 };
		struct Accumulate
		{
			static void thunk(void* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				auto& array = *reinterpret_cast<ArrayHeader*>(static_cast<std::byte*>(a_light) + kSceneAccumArray);
				bool lend = false;
				if (listInstalled) {
					listStats.lights.fetch_add(1, std::memory_order_relaxed);
					if (!listLive.load(std::memory_order_acquire))
						listStats.walkedNotLive.fetch_add(1, std::memory_order_relaxed);
					else if (array.size || a_arg3)
						listStats.walkedOwn.fetch_add(1, std::memory_order_relaxed);
					else if (static_cast<const RE::BSLight*>(a_light)->portalStrict)
						listStats.walkedStrict.fetch_add(1, std::memory_order_relaxed);  // cull mode 4: a cell's OnVisible tests its multibound
					else if (eventsPushed.load(std::memory_order_acquire) != eventsDrained || hiddenMoved.load(std::memory_order_acquire))
						listStats.walkedMoved.fetch_add(1, std::memory_order_relaxed);
					else
						lend = !lightList.roots.empty();
				}
				if (lend) {
					// The engine's list path reads the array for this call alone (FUN_1414bf320, mode 1); its own header is put back.
					const ArrayHeader own = array;
					array = { lightList.roots.data(), static_cast<std::uint32_t>(lightList.roots.size()), 0, static_cast<std::uint32_t>(lightList.roots.size()), 0 };
					lentArray = &array;
					parentVerdicts.assign(lightList.throughNodes.size(), std::int8_t(-1));
					func(a_light, a_count, a_arg2, a_arg3);
					lentArray = nullptr;
					array = own;
					listStats.listed.fetch_add(1, std::memory_order_relaxed);
					listStats.listEntries.fetch_add(lightList.roots.size(), std::memory_order_relaxed);
				} else {
					func(a_light, a_count, a_arg2, a_arg3);
				}
				QueryPerformanceCounter(&end);
				accumulateTicks.fetch_add(end.QuadPart - start.QuadPart, std::memory_order_relaxed);
				accumulateCalls.fetch_add(1, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Process1
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				// A lent list's root: only where the walk would have reached it.
				if (a_object && lentArray)
					if (const auto it = lightList.position.find(a_object); it != lightList.position.end() && !ParentsVisible(a_process, lightList.rootParent[it->second])) {
						listStats.parentCulled.fetch_add(1, std::memory_order_relaxed);
						return;
					}
				if (a_object)
					if (const auto* exclusion = frameExclusion.load(std::memory_order_acquire)) {
						stats.visited.fetch_add(1, std::memory_order_relaxed);
						if (Excluded(*exclusion, a_object)) {
							if (!probe && !parityFrame.load(std::memory_order_relaxed)) {
								stats.skipped.fetch_add(1, std::memory_order_relaxed);
								return;
							}
							stats.wouldSkip.fetch_add(1, std::memory_order_relaxed);
						}
					}
				func(a_process, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		static bool timed = false;
		if (!timed) {
			// Timed whether or not the skip is on: the A/B's measure.
			stl::write_vfunc<0x9, Accumulate>(RE::VTABLE_BSShadowParabolicLight[0]);
			timed = true;
		}
		if (installed || SwitchValue(Switch::LightExclude) == "0")
			return;
		const auto base = REL::Module::get().base();
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSParabolicCullingProcess[0] };
		if (reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x16] != base + kParabolicProcess1) {
			logger::warn("[DCLF] point lights' shadow culls: BSParabolicCullingProcess::Process1 is not the engine's; they keep culling everything");
			return;
		}
		stl::write_vfunc<0x16, Process1>(RE::VTABLE_BSParabolicCullingProcess[0]);
		probe = SwitchValue(Switch::LightExclude) == "probe";
		installed = true;
		// The list's hidden entries are skipped by the list cull's hook (PrimaryCull's CullList, OwnsList).
		listInstalled = !probe && SwitchValue(Switch::LightList) != "0" && PrimaryCull::CullListHooked();
		logger::info("[DCLF] point lights' shadow culls without DCLF's entries{}{}", probe ? " (probe: nothing skipped)" : "",
			listInstalled ? ", through a list of the rest" : "");
	}

	void Publish(std::shared_ptr<SunExclusion> a_exclusion)
	{
		if (installed)
			pending = std::move(a_exclusion);
	}

	void SelectFrame(std::uint32_t a_frame)
	{
		if (!installed)
			return;
		++stats.frames;
		// The culls of the frame read this one; the previous frame's culls are over (they ran before its shadow epoch).
		std::shared_ptr<SunExclusion> next = pending;
		if (!next || !next->candidates)
			++stats.noExclusion, next.reset();
		else if (!PassCapture::Get().ShadowModeWithheld(kParabolicMode))
			++stats.noClaims, next.reset();  // the registration withholds nothing of the mode: the engine draws its casters
		else if (next->candidates->generation != SceneStore::Get().GetSunCandidatesGeneration())
			++stats.stale, next.reset();  // built for other candidates: an entry may hold a caster no epoch has drawn yet
		const bool parity = next && SwitchEnabled(Switch::PersistentParity) && ParityDue(a_frame);
		parityFrame.store(parity, std::memory_order_relaxed);
		stats.parityFrames += parity ? 1 : 0;
		stats.live += next ? 1 : 0;
		frameExclusion.store(next.get(), std::memory_order_release);
		if (listInstalled) {
			UpdateList(next.get());
			listLive.store(next && !parity && lightList.valid, std::memory_order_release);
		}
		frameHeld = std::move(next);
	}

	void NoteStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached)
	{
		const auto* objectRoot = listInstalled ? listObjectRoot.load(std::memory_order_acquire) : nullptr;
		if (!objectRoot || !a_parent || !a_child)
			return;
		// The parent's depth below the object root; deeper parents are inside what the list holds whole.
		std::uint32_t depth = 0;
		const RE::NiAVObject* node = a_parent;
		while (node && node != objectRoot && depth < kListDepth) {
			node = node->parent;
			++depth;
		}
		if (node != objectRoot || depth >= kListDepth)
			return;
		structureEvents.Push({ a_attached ? RE::NiPointer<RE::NiAVObject>(a_child) : RE::NiPointer<RE::NiAVObject>(), a_child, a_parent, depth, a_attached });
		eventsPushed.fetch_add(1, std::memory_order_release);
	}

	void NoteHiddenKey(const void* a_key)
	{
		if (listInstalled && lightList.watched.contains(a_key))
			hiddenMoved.store(true, std::memory_order_release);
	}

	bool OwnsList(const void* a_list)
	{
		return a_list && a_list == lentArray;
	}

	void EndFrame()
	{
		listGraveyard.clear();
	}

	void NoteRegistration(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_withheld, std::uint32_t a_source)
	{
		if (!installed || !parityFrame.load(std::memory_order_relaxed) || !a_pass || !a_pass->geometry)
			return;
		const auto* exclusion = frameExclusion.load(std::memory_order_acquire);
		std::uint32_t mode = 0;
		if (!exclusion || !PassCapture::Get().ShadowModeOfBatch(a_batch, mode) || mode != kParabolicMode)
			return;
		// Under an excluded entry: any of its ancestors, as the skip takes it out (Process1 returns there). A caster of one need not
		// be a candidate geometry: an entry is excluded by its candidates alone (an effect under a cell entry, for one).
		bool underExcluded = false;
		for (const RE::NiAVObject* object = a_pass->geometry; object && !underExcluded; object = object->parent)
			underExcluded = Excluded(*exclusion, object);
		// The light list: a caster under the object root that no listed root holds would not be registered with the list lent,
		// withheld or not (the registration also writes the engine's geometry's light masks).
		if (listInstalled && lightList.valid && !underExcluded) {
			bool underObjectRoot = false;
			if (!UnderListedRoot(a_pass->geometry, underObjectRoot) && underObjectRoot) {
				listStats.parityMissed.fetch_add(1, std::memory_order_relaxed);
				std::string chain;
				for (const RE::NiAVObject* object = a_pass->geometry; object && object != lightList.objectRoot; object = object->parent)
					chain += fmt::format(" <- {}:{}{}{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "", Hidden(object) ? " hidden" : "",
						exclusion->candidates->entries.contains(object) ? (Excluded(*exclusion, object) ? " [excluded entry]" : " [entry]") : "",
						lightList.watched.contains(object) ? " [gone through]" : "");
				std::scoped_lock lock(firstMutex);
				if (listFirstMissed.empty())
					listFirstMissed = chain;
			}
			listStats.parityChecked.fetch_add(1, std::memory_order_relaxed);
		}
		if (a_withheld)
			return;
		stats.parityPasses.fetch_add(1, std::memory_order_relaxed);
		if (underExcluded) {
			stats.parityLost.fetch_add(1, std::memory_order_relaxed);
			{
				const auto& tables = SceneStore::Get().GetTables();
				std::uint32_t reject = 15;
				std::string what = fmt::format("'{}' (insert {})", a_pass->geometry->name.c_str(), a_source);
				for (std::uint32_t o = 0; o < tables.objectGeometry.size(); ++o)
					if (tables.objectGeometry[o] == a_pass->geometry) {
						reject = tables.shadowReject[o];
						what += fmt::format(" object {} flags {:#x} reject {} technique {:#x}", o, tables.objects[o].flags, tables.shadowReject[o], tables.shadowTechnique[o]);
						break;
					}
				std::scoped_lock lock(firstMutex);
				++lostByReject[std::min<std::uint32_t>(reject, 15)];
				if (firstLost.empty())
					firstLost = std::move(what);
			}
		}
	}

	std::string Report()
	{
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		const std::uint64_t calls = accumulateCalls.exchange(0);
		const std::int64_t ticks = accumulateTicks.exchange(0);
		const auto timing = fmt::format("[DCLF] point lights' Accumulate: {} calls, {:.3f} ms in all", calls, ticks * 1000.0 / static_cast<double>(frequency.QuadPart));
		if (!installed)
			return timing;
		auto& s = stats;
		const auto lost = s.parityLost.exchange(0);
		const auto text = fmt::format("[DCLF] point lights' shadow culls: {} frames, {} with the exclusion ({} none built, {} the mode's claims not live, {} stale); {} entries visited, {} skipped{}; parity {} frames: {} paraboloid passes not withheld, {} of them under an excluded entry{}",
			s.frames, s.live, s.noExclusion, s.noClaims, s.stale, s.visited.exchange(0), s.skipped.exchange(0), probe ? fmt::format(" (probe: {} would be)", s.wouldSkip.load()) : std::string(),
			s.parityFrames, s.parityPasses.exchange(0), lost, s.parityFrames ? (lost ? " <- LIGHT EXCLUSION" : " <- OK") : "");
		std::scoped_lock lock(firstMutex);
		std::string by;
		for (std::size_t r = 0; r < lostByReject.size(); ++r)
			if (lostByReject[r])
				by += fmt::format(" reject {}={}", r, std::exchange(lostByReject[r], 0));
		const auto withFirst = firstLost.empty() ? text : text + "; lost by" + by + "; first lost " + std::exchange(firstLost, {});
		s.wouldSkip = 0;
		s.frames = s.live = s.noExclusion = s.noClaims = s.stale = s.parityFrames = 0;
		std::string list;
		if (listInstalled) {
			auto& l = listStats;
			const auto listed = l.listed.exchange(0);
			const auto checked = l.parityChecked.exchange(0);
			const auto missed = l.parityMissed.exchange(0);
			const std::string first = std::exchange(listFirstMissed, {});  // firstMutex is held (above)
			list = fmt::format("\n[DCLF] point lights' list: {} lights, {} through the list ({:.0f} entries each, {:.0f} of them culled by a parent's test; {} roots now), walked: {} with a root list of their own, {} portal-strict, {} on frames without it, {} after a move since the selection; {} builds ({:.3f} ms each), {} entries added and {} removed by events; parity {} casters checked, {} under no listed root{}{}",
				l.lights.exchange(0), listed, listed ? static_cast<double>(l.listEntries.exchange(0)) / static_cast<double>(listed) : 0.0,
				listed ? static_cast<double>(l.parentCulled.exchange(0)) / static_cast<double>(listed) : 0.0,
				lightList.roots.size(),
				l.walkedOwn.exchange(0), l.walkedStrict.exchange(0), l.walkedNotLive.exchange(0), l.walkedMoved.exchange(0), l.builds,
				l.builds ? static_cast<double>(l.buildTicks) * 1000.0 / static_cast<double>(frequency.QuadPart) / static_cast<double>(l.builds) : 0.0, l.added, l.removed,
				checked, missed, checked || missed ? (missed ? " <- LIGHT LIST" : " <- OK") : "", first.empty() ? "" : "; first:" + first);
			l.listEntries = 0;
			l.builds = l.added = l.removed = 0;
			l.buildTicks = 0;
		}
		return timing + "\n" + withFirst + list;
	}
}
