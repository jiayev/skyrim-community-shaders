// CS_DCLF_ACCUMULATED_PARITY's engine side (Accumulated.h): the render thread's kAccumulated samples and frame camera check, the
// animation gate's detours (job threads) and the prediction published at Main::Update's entry (main thread).

#include "Accumulated.h"

#include "Events.h"
#include "PrimaryCullView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <ankerl/unordered_dense.h>
#include <atomic>
#include <deque>

namespace DCLF::Diagnostics::Accumulated
{
	const std::array<const char*, 3> kAnimGateCallers{ "UpdateAnimationJob", "managed nodes (FUN_1402b41a0)", "FUN_1402b52e0" };

	namespace
	{
		using Engine::kFlagAccumulated;
		using Engine::kFlagHidden;
		constexpr std::uint32_t kFlagIgnoreFade = 1u << 15;  // on a fade node: kIgnoreFade (FUN_1402cff60 runs no fade update)
		constexpr std::uint32_t kFlag29 = 1u << 29;          // FUN_1402cff60: without kAccumulated, visible only with a zero radius
		constexpr std::size_t kBoundRadius = 0xF0;           // NiAVObject::worldBound.radius (FUN_1402cff60 tests its bits against 0)
		constexpr std::size_t kCurrentFade = 0x130;          // BSFadeNode::currentFade

		std::uint32_t Flags(const void* a_node) { return Engine::FlagsOf(a_node); }
		bool AccumulatedBit(const void* a_node) { return (Flags(a_node) & kFlagAccumulated) != 0; }

		// AE 1.6.1170 and SE 1.5.97: FUN_1402cff60 (SE FUN_14027cac0) and its three calls (skyrim-engine-notes.md, "who sets kAccumulated").
		constexpr Engine::Offset kAnimGate{ { 19384 }, { 18972 } };
		constexpr std::array<Engine::Offset, 3> kAnimGateCallSites{ { { { 19204, 0x96 }, { 18714, 0x96 } }, { { 18894, 0x90 }, { 18463, 0x90 } }, { { 18900, 0xa2 }, { 18469, 0xa2 } } } };  // UpdateAnimationJob, FUN_1402b41a0, FUN_1402b52e0

		/** @brief Main::Update's entry's: the stood-in roots and whether DCLF left their kAccumulated set. Immutable once published. */
		struct AnimPrediction
		{
			ankerl::unordered_dense::map<const void*, bool> roots;
		};
		// Published at Main::Update's entry (the main thread), read by the gates on job threads during the frame's cycle; the next
		// entry, which only follows the cycle's end, lets the last go.
		std::atomic<AnimPrediction*> animPrediction{ nullptr };

		/** @brief Per call site, since the last report (job threads). */
		struct AnimCounts
		{
			std::atomic<std::uint64_t> calls{ 0 }, neverFade{ 0 }, fading{ 0 }, rejected{ 0 }, rejectedNeverFade{ 0 };
			std::atomic<std::uint64_t> predictedOut{ 0 }, acceptedPredictedOut{ 0 }, fadedOut{ 0 }, differ{ 0 };
			std::atomic<const void*> firstDiffer{ nullptr };
		};
		std::array<AnimCounts, 3> animCounts;
		std::atomic<std::uint64_t> animPredictions{ 0 }, animPredictedRoots{ 0 }, animPredictedSet{ 0 };

		/** @brief Job thread, inside the engine's gate's caller: a stood-in root's verdict against DCLF's prediction. */
		void NoteAnimGate(std::size_t a_site, const RE::NiAVObject* a_root, bool a_result)
		{
			const auto* prediction = animPrediction.load(std::memory_order_acquire);
			if (!prediction || !a_root)
				return;
			const auto it = prediction->roots.find(a_root);
			if (it == prediction->roots.end())
				return;
			auto& c = animCounts[a_site];
			c.calls.fetch_add(1, std::memory_order_relaxed);
			// The gate's own reads (FUN_1402cff60): the fade node, its flags and radius; the root's flags.
			const auto* fade = const_cast<RE::NiAVObject*>(a_root)->AsFadeNode();
			const std::uint32_t flags = Flags(a_root);
			const bool neverFade = !fade || (Flags(fade) & kFlagIgnoreFade) || Engine::At<std::uint32_t>(fade, kBoundRadius) == 0 || (flags & kFlag29);
			(neverFade ? c.neverFade : c.fading).fetch_add(1, std::memory_order_relaxed);
			const bool predicted = it->second;
			if (!predicted)
				c.predictedOut.fetch_add(1, std::memory_order_relaxed);
			if (a_result) {
				// Out of view as DCLF wrote it, yet updated: a fading root within its fade distance (the gate's own fade step), as the
				// engine does for a root its cull failed.
				if (!predicted)
					c.acceptedPredictedOut.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			c.rejected.fetch_add(1, std::memory_order_relaxed);
			if (neverFade)
				c.rejectedNeverFade.fetch_add(1, std::memory_order_relaxed);
			if (!predicted)
				return;
			// Written visible and rejected: fully faded out (the gate's step 2), else a difference.
			if (fade && Engine::At<float>(fade, kCurrentFade) <= 0.0f) {
				c.fadedOut.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			c.differ.fetch_add(1, std::memory_order_relaxed);
			const void* none = nullptr;
			c.firstDiffer.compare_exchange_strong(none, a_root, std::memory_order_relaxed);
		}

		template <std::size_t Site>
		struct AnimGate
		{
			static bool thunk(RE::NiAVObject* a_root)
			{
				const bool result = func(a_root);
				NoteAnimGate(Site, a_root, result);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The render thread's: the samples no decode has reached yet, in frame order.
		std::deque<std::unique_ptr<Sample>> samples;

		// The frame camera check (render thread).
		FrameCamera frameCamera;

		/** @brief PrimaryCull::Install: <- ANIM RATE on FUN_1402cff60's three calls. */
		void InstallAnimRate(const On<EnginePoint::PrimaryCullInstalled>&)
		{
			const auto base = REL::Module::get().base();
			for (const auto site : kAnimGateCallSites)
				if (!Engine::CallsTo(base + site, base + kAnimGate)) {
					logger::warn("[DCLF] anim rate observer (T7b2): FUN_1402cff60's calls are not where expected; not installed");
					return;
				}
			stl::write_thunk_call<AnimGate<0>>(base + kAnimGateCallSites[0]);
			stl::write_thunk_call<AnimGate<1>>(base + kAnimGateCallSites[1]);
			stl::write_thunk_call<AnimGate<2>>(base + kAnimGateCallSites[2]);
			logger::info("[DCLF] anim rate observer (T7b2) installed on FUN_1402cff60's three calls");
		}

		/** @brief The list processes' camera must be the one BeginFrameCamera read. */
		void CheckFrameCamera(const CutFrameBegin& a_event)
		{
			if (!a_event.processes || !a_event.count || !a_event.processes[0])
				return;
			++frameCamera.checked;
			if (static_cast<const void*>(a_event.processes[0]->camera) != PrimaryCullView::FrameCamera())
				++frameCamera.differ;
		}

		/** @brief After the list jobs' Finish: the bit on the frame's tables' fade roots and geometries. */
		void SampleBits(const On<EnginePoint::ListJobsDone>&)
		{
			// The frame's tables (the installed publication's: the same the frame's depth commit armed its readback with).
			const auto& tables = Scene::FrameView::Get().GetTables();
			auto sample = std::make_unique<Sample>();
			sample->frame = Scene::FrameView::Get().GetFrame();
			const Api::RootListView list = Extensions::RootListOf(tables);
			const std::size_t roots = list.nodes.size();
			sample->rootNodes.assign(list.nodes.begin(), list.nodes.end());
			sample->rootBits.assign(roots, 0u);
			// A root the cut stood in for is not the engine cull's (its bit is DCLF's write at Main::Update's entry).
			for (std::size_t r = 0; r < roots; ++r)
				if (const void* node = list.nodes[r])
					sample->rootBits[r] = (AccumulatedBit(node) ? kSampleAccumulated : 0u) | (PrimaryCullView::StoodInWhole(node) ? kSampleStoodIn : 0u) |
					                      ((Flags(node) & kFlagHidden) ? kSampleHidden : 0u);
			const std::size_t objects = tables.objectGeometry.size();
			sample->objectGeometry.assign(objects, nullptr);
			sample->objectBits.assign(objects, 0u);
			for (std::size_t o = 0; o < objects; ++o) {
				const auto* geometry = tables.objectGeometry[o];
				if (!geometry || tables.IsLayer(static_cast<std::uint32_t>(o)))
					continue;
				sample->objectGeometry[o] = geometry;
				std::uint8_t bits = AccumulatedBit(geometry) ? kSampleAccumulated : 0u;
				// Its fade root's: a geometry the cull did not reach this frame (its root out of view, or faded out) keeps the bit the
				// last cull that did left; the root's says which.
				const std::uint32_t root = list.RootOf(static_cast<std::uint32_t>(o));
				if (root != kNoFadeRoot && root < roots && list.nodes[root]) {
					bits |= (sample->rootBits[root] & kSampleAccumulated) ? kSampleRootAccumulated : 0u;
					bits |= sample->rootBits[root] & (kSampleStoodIn | kSampleHidden);
				} else {
					bits |= kSampleRootAccumulated;
				}
				sample->objectBits[o] = bits;
			}
			samples.push_back(std::move(sample));
		}

		/** @brief Main::Update's entry, after the writes (no animation job or cell animation runs): the prediction for this frame's gates. */
		void PublishPrediction(const UpdateEntry& a_event)
		{
			if (!a_event.running)
				return;
			const auto& stoodIn = PrimaryCullView::StandInList();
			auto* prediction = new AnimPrediction();
			prediction->roots.reserve(stoodIn.size());
			std::uint64_t set = 0;
			for (const void* root : stoodIn) {
				const bool accumulated = AccumulatedBit(root);
				prediction->roots.insert_or_assign(root, accumulated);
				set += accumulated ? 1 : 0;
			}
			animPredictions.fetch_add(1, std::memory_order_relaxed);
			animPredictedRoots.fetch_add(stoodIn.size(), std::memory_order_relaxed);
			animPredictedSet.fetch_add(set, std::memory_order_relaxed);
			// The last frame's gates are done (Main::Update's entry follows the cycle's wait): nothing reads the old one.
			delete animPrediction.exchange(prediction, std::memory_order_acq_rel);
		}
	}

	void Install()
	{
		Subscribe<EnginePoint::PrimaryCullInstalled>(InstallAnimRate);
		Subscribe<CutFrameBegin>(CheckFrameCamera);
		Subscribe<EnginePoint::ListJobsDone>(SampleBits);
		Subscribe<UpdateEntry>(PublishPrediction);
	}

	std::vector<std::unique_ptr<Sample>> TakeSamples(std::uint32_t a_through)
	{
		std::vector<std::unique_ptr<Sample>> taken;
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
		return dropped;
	}

	AnimRate TakeAnimRate()
	{
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		AnimRate r;
		r.predictions = take(animPredictions);
		r.roots = take(animPredictedRoots);
		r.set = take(animPredictedSet);
		for (std::size_t s = 0; s < animCounts.size(); ++s) {
			auto& c = animCounts[s];
			auto& o = r.sites[s];
			o.calls = take(c.calls), o.neverFade = take(c.neverFade), o.fading = take(c.fading), o.rejected = take(c.rejected);
			o.rejectedNeverFade = take(c.rejectedNeverFade), o.predictedOut = take(c.predictedOut), o.acceptedOut = take(c.acceptedPredictedOut);
			o.fadedOut = take(c.fadedOut), o.differ = take(c.differ);
			o.firstDiffer = c.firstDiffer.exchange(nullptr, std::memory_order_relaxed);
		}
		return r;
	}

	FrameCamera TakeFrameCamera()
	{
		return std::exchange(frameCamera, {});
	}
}
