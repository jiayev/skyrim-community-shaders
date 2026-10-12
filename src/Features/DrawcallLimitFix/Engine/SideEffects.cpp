#include "SideEffects.h"

#include "EngineAccess.h"
#include "LightViews.h"
#include "SunViews.h"
#include "Features/DrawcallLimitFix/Common/EventQueue.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include <ankerl/unordered_dense.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <deque>

// The engine side effects DCLF produces (T7b). SideEffects.h describes them.

namespace DCLF::SideEffects
{
	namespace
	{
		constexpr std::size_t kObjectFlags = 0xF4;          // NiAVObject::flags
		constexpr std::uint32_t kFlagHidden = 1u << 0;        // kHidden (app-culled)
		constexpr std::uint32_t kFlagAccumulated = 1u << 26;  // kAccumulated: passed the last cull that reached it
		constexpr std::uint32_t kFlagIgnoreFade = 1u << 15;   // on a fade node: kIgnoreFade (FUN_1402cff60 runs no fade update)
		constexpr std::uint32_t kFlag29 = 1u << 29;           // FUN_1402cff60: without kAccumulated, visible only with a zero radius
		constexpr std::size_t kBoundRadius = 0xF0;            // NiAVObject::worldBound.radius (FUN_1402cff60 tests its bits against 0)
		constexpr std::size_t kCurrentFade = 0x130;           // BSFadeNode::currentFade

		std::uint32_t Flags(const void* a_node) { return *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(a_node) + kObjectFlags); }
		bool Accumulated(const void* a_node) { return (Flags(a_node) & kFlagAccumulated) != 0; }

		// AE 1.6.1170: FUN_1402cff60 and its three calls (skyrim-engine-notes.md, "T7b0").
		constexpr std::uintptr_t kAnimGate = 0x2cff60;
		constexpr std::array<std::uintptr_t, 3> kAnimGateCallSites{ 0x2c73f6, 0x2b4230, 0x2b5382 };  // UpdateAnimationJob, FUN_1402b41a0, FUN_1402b52e0
		constexpr std::array<const char*, 3> kAnimGateCallers{ "UpdateAnimationJob", "managed nodes (FUN_1402b41a0)", "FUN_1402b52e0" };

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
		std::deque<std::unique_ptr<AccumulatedSample>> samples;

		// T7f checklist item 6 (AE 1.6.1170): the actors' animation rate (UpdateActorRate).
		constexpr std::uintptr_t kFrameCounter = 0x328cc6c;     // gFrameCounter: Renderer::End (0x140e4482b) increments it
		constexpr std::size_t kMiddleHighSkinProperty = 0x178;  // MiddleHighProcessData: the property Actor::ModifyAnimationUpdateData reads
		constexpr std::size_t kPropertyStamp = 0x80;            // BSShaderProperty::lastAccumulatedFrameCount
		constexpr std::uint32_t kFlagNotVisible = 1u << 20;     // the actor pre-cull's (HighActorCuller, BSFadeNodeCuller) off screen
		constexpr std::uint32_t kRaceAlwaysAnimates = 1u << 7;  // RACE_DATA flag: ModifyAnimationUpdateData ignores kNotVisible

		/** @brief The main thread's: the properties stamped at the last entry (the producer's), so the observer can tell the engine's stamps. */
		ankerl::unordered_dense::set<const void*> actorStamped;
		struct ActorRateCounts
		{
			std::uint64_t entries = 0, actors = 0, noProperty = 0, stamped = 0, inSun = 0, inLocal = 0;
			// The observer, kNotVisible actors: DCLF's verdict of the last frame's views against the engine's stamp of that frame.
			std::uint64_t notVisible = 0, both = 0, dclfOnly = 0, engineOnly = 0, neither = 0, ownStamp = 0;
			std::string first;
		};
		ActorRateCounts actorRate;

		// CS_DCLF_FADE_PARITY: the fade snaps since the last entry (NoteFadeSnap).
		auto& fadeSnaps = *new EventQueue<const void*>();

		bool SphereInside(const SunViews::Planes& a_planes, const RE::NiPoint3& a_centre, float a_radius)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!((a_planes.mask >> p) & 1))
					continue;
				const auto& plane = a_planes.plane[p];
				if (!(plane[0] * a_centre.x + plane[1] * a_centre.y + plane[2] * a_centre.z - plane[3] > -a_radius))
					return false;
			}
			return true;
		}
	}

	bool AccumulatedParity()
	{
		static const bool enabled = SwitchEnabled(Switch::AccumulatedParity);
		return enabled;
	}

	void SampleAccumulated(std::uint32_t a_frame, const std::function<bool(const void*)>& a_stoodIn)
	{
		if (!AccumulatedParity())
			return;
		// The frame's tables (the installed publication's: the same the frame's depth commit armed its readback with).
		const auto& tables = SceneStore::Get().GetTables();
		auto sample = std::make_unique<AccumulatedSample>();
		sample->frame = a_frame;
		const std::size_t roots = tables.fadeRootNode.size();
		sample->rootNodes.assign(tables.fadeRootNode.begin(), tables.fadeRootNode.end());
		sample->rootBits.assign(roots, 0u);
		for (std::size_t r = 0; r < roots; ++r)
			if (const void* node = tables.fadeRootNode[r])
				sample->rootBits[r] = (Accumulated(node) ? kSampleAccumulated : 0u) | (a_stoodIn(node) ? kSampleStoodIn : 0u) |
				                      ((Flags(node) & kFlagHidden) ? kSampleHidden : 0u);
		const std::size_t objects = tables.objectGeometry.size();
		sample->objectGeometry.assign(objects, nullptr);
		sample->objectBits.assign(objects, 0u);
		for (std::size_t o = 0; o < objects; ++o) {
			const auto* geometry = tables.objectGeometry[o];
			if (!geometry || tables.IsLayer(static_cast<std::uint32_t>(o)))
				continue;
			sample->objectGeometry[o] = geometry;
			std::uint8_t bits = Accumulated(geometry) ? kSampleAccumulated : 0u;
			// Its fade root's: a geometry the cull did not reach this frame (its root out of view, or faded out) keeps the bit the
			// last cull that did left; the root's says which.
			const std::uint32_t root = o < tables.objectFadeRoot.size() ? tables.objectFadeRoot[o] : kNoFadeRoot;
			if (root != kNoFadeRoot && root < roots && tables.fadeRootNode[root]) {
				bits |= (sample->rootBits[root] & kSampleAccumulated) ? kSampleRootAccumulated : 0u;
				bits |= sample->rootBits[root] & (kSampleStoodIn | kSampleHidden);
			} else {
				bits |= kSampleRootAccumulated;
			}
			sample->objectBits[o] = bits;
		}
		samples.push_back(std::move(sample));
	}

	std::vector<std::unique_ptr<AccumulatedSample>> TakeAccumulatedSamples(std::uint32_t a_through)
	{
		std::vector<std::unique_ptr<AccumulatedSample>> taken;
		while (!samples.empty() && samples.front()->frame <= a_through) {
			taken.push_back(std::move(samples.front()));
			samples.pop_front();
		}
		return taken;
	}

	void InstallAnimRate()
	{
		if (!AccumulatedParity())
			return;
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

	void PublishAnimPrediction(const std::vector<const void*>& a_stoodInRoots)
	{
		if (!AccumulatedParity())
			return;
		auto* prediction = new AnimPrediction();
		prediction->roots.reserve(a_stoodInRoots.size());
		std::uint64_t set = 0;
		for (const void* root : a_stoodInRoots) {
			const bool accumulated = Accumulated(root);
			prediction->roots.insert_or_assign(root, accumulated);
			set += accumulated ? 1 : 0;
		}
		animPredictions.fetch_add(1, std::memory_order_relaxed);
		animPredictedRoots.fetch_add(a_stoodInRoots.size(), std::memory_order_relaxed);
		animPredictedSet.fetch_add(set, std::memory_order_relaxed);
		// The last frame's gates are done (Main::Update's entry follows the cycle's wait): nothing reads the old one.
		delete animPrediction.exchange(prediction, std::memory_order_acq_rel);
	}

	std::string AnimRateReport()
	{
		if (!AccumulatedParity())
			return {};
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		const auto predictions = take(animPredictions), roots = take(animPredictedRoots), set = take(animPredictedSet);
		std::string sites;
		std::uint64_t differ = 0;
		std::string first;
		for (std::size_t s = 0; s < animCounts.size(); ++s) {
			auto& c = animCounts[s];
			const auto calls = take(c.calls), neverFade = take(c.neverFade), fading = take(c.fading), rejected = take(c.rejected);
			const auto rejectedNeverFade = take(c.rejectedNeverFade), predictedOut = take(c.predictedOut), acceptedOut = take(c.acceptedPredictedOut);
			const auto fadedOut = take(c.fadedOut), siteDiffer = take(c.differ);
			differ += siteDiffer;
			if (const void* node = c.firstDiffer.exchange(nullptr, std::memory_order_relaxed); node && first.empty())
				first = fmt::format("; first: {:p} ({})", node, kAnimGateCallers[s]);
			sites += fmt::format("; {}: {} stood-in calls ({} never fading, {} fading), {} rejected ({} never fading), {} written out of view ({} of them updated by "
								 "their fade step), {} written visible but faded out, {} differ",
				kAnimGateCallers[s], calls, neverFade, fading, rejected, rejectedNeverFade, predictedOut, acceptedOut, fadedOut, siteDiffer);
		}
		return fmt::format("[DCLF] anim rate (T7b2): {} predictions, {:.1f} stood-in roots a frame ({:.1f} written visible){}{}{}\n", predictions,
			predictions ? double(roots) / double(predictions) : 0.0, predictions ? double(set) / double(predictions) : 0.0, sites,
			!predictions ? " <- NOT COMPARED" : differ ? " <- ANIM RATE" : " <- OK", first);
	}

	void UpdateActorRate(bool a_running)
	{
		const bool observe = AccumulatedParity();
		if (!a_running) {
			actorStamped.clear();
			return;
		}
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists)
			return;
		auto& c = actorRate;
		++c.entries;
		const std::uint32_t counter = Engine::Global<std::uint32_t>(kFrameCounter);
		// The last scene frame's shadow views (the frame whose stamps the engine left): the sun's cascade culls, the local lights' views.
		const std::uint32_t sceneFrame = SceneStore::Get().GetFrame();
		const auto& sun = SunViews::Get().Current();
		const bool sunViews = sun.valid && sun.sceneFrame == sceneFrame;
		ankerl::unordered_dense::set<const void*> stampedNow;
		for (const auto& handle : lists->highActorHandles) {
			const auto actor = handle.get();
			if (!actor)
				continue;
			const auto* process = actor->GetActorRuntimeData().currentProcess;
			const auto* middleHigh = process ? process->middleHigh : nullptr;
			auto* property = middleHigh ? Engine::At<std::byte*>(middleHigh, kMiddleHighSkinProperty) : nullptr;
			auto* root = actor->Get3D2();
			++c.actors;
			if (!property || !root) {
				++c.noProperty;
				continue;
			}
			// DCLF's verdict: the 3D's bound inside a shadow view of the last scene frame.
			const auto& bound = root->worldBound;
			bool inSun = false, inLocal = false;
			// The sun: the cascade culls walk what the full-frustum cull kept (SunViews::Frame::fullFrusta), then test their own planes.
			bool inFull = sun.fullFrusta.empty();
			for (const auto& planes : sun.fullFrusta)
				inFull = inFull || SphereInside(planes, bound.center, bound.radius);
			if (sunViews && inFull)
				for (std::uint32_t i = 0; i < sun.count && i < SunViews::kMaxCascades && !inSun; ++i)
					inSun = sun.cascades[i].valid && SphereInside(sun.cascades[i].cull, bound.center, bound.radius);
			if (!inSun)
				LightViews::Get().ForEachView(sceneFrame, [&](const SunViews::Cascade& a_view, const std::array<float, 4>& a_sphere) {
					// A spot light's view: its cull planes; a point light's (no planes): the bound within the light's radius.
					if (inLocal)
						return;
					if (a_view.cull.mask) {
						inLocal = SphereInside(a_view.cull, bound.center, bound.radius);
					} else if (a_sphere[3] > 0.0f) {
						const float dx = bound.center.x - a_sphere[0], dy = bound.center.y - a_sphere[1], dz = bound.center.z - a_sphere[2];
						const float reach = a_sphere[3] + bound.radius;
						inLocal = dx * dx + dy * dy + dz * dz <= reach * reach;
					}
				});
			const bool dclf = inSun || inLocal;
			c.inSun += inSun ? 1 : 0;
			c.inLocal += inLocal ? 1 : 0;
			std::atomic_ref<std::uint32_t> stamp(Engine::At<std::uint32_t>(property, kPropertyStamp));
			// The observer: the kNotVisible actors (the branch that reads the stamp), the last frame's stamp against the same frame's views.
			// A stamp DCLF made at the last entry is not the engine's: such an actor is counted as DCLF's own.
			const auto* race = actor->GetRace();
			const bool readsStamp = (root->GetFlags().underlying() & kFlagNotVisible) && !(race && (race->data.flags.underlying() & kRaceAlwaysAnimates));
			if (observe && readsStamp) {
				++c.notVisible;
				const bool engine = stamp.load(std::memory_order_relaxed) == counter - 1;
				if (engine && actorStamped.contains(property)) {
					++c.ownStamp;
				} else {
					++(dclf ? (engine ? c.both : c.dclfOnly) : (engine ? c.engineOnly : c.neither));
					if (engine && !dclf && c.first.size() < 300)
						c.first += fmt::format("{}'{}'", c.first.empty() ? "" : ", ", actor->GetName() ? actor->GetName() : "?");
				}
			}
			// The producer: this frame's stamp, for every actor DCLF's shadow views hold (the engine reads it only for a kNotVisible one).
			if (dclf) {
				stamp.store(counter, std::memory_order_relaxed);
				stampedNow.insert(property);
				++c.stamped;
			}
		}
		actorStamped = std::move(stampedNow);
	}

	void NoteFadeSnap(const void* a_node)
	{
		static const bool parity = SwitchEnabled(Switch::FadeParity);
		if (parity && a_node)
			fadeSnaps.Push(a_node);
	}

	void TakeFadeSnaps(std::vector<const void*>& a_out)
	{
		fadeSnaps.Drain([&](const void* a_node) { a_out.push_back(a_node); });
	}

	std::string ActorRateReport()
	{
		auto& c = actorRate;
		if (!c.entries)
			return {};
		const bool observe = AccumulatedParity();
		std::string text = fmt::format(
			"[DCLF] actor rate (T7f checklist item 6): {} entries, {:.1f} high actors a frame ({:.1f} without a skin property or 3D), {:.1f} in the "
			"sun's cascade culls and {:.1f} in a local shadow view a frame, {:.1f} stamped by DCLF a frame",
			c.entries, double(c.actors) / double(c.entries), double(c.noProperty) / double(c.entries),
			double(c.inSun) / double(c.entries), double(c.inLocal) / double(c.entries), double(c.stamped) / double(c.entries));
		if (observe)
			text += fmt::format("; kNotVisible actors (their animation reads the stamp) {}: in DCLF's shadow views / stamped by the engine last frame {} both / {} DCLF's alone / "
								"{} the engine's alone / {} neither, {} DCLF's own stamp{}{}",
				c.notVisible, c.both, c.dclfOnly, c.engineOnly, c.neither, c.ownStamp, c.engineOnly ? " <- ACTOR RATE" : " <- OK", c.first.empty() ? "" : "; first: " + c.first);
		c = {};
		return text + "\n";
	}

	std::uint64_t DropAccumulatedSamples(std::uint32_t a_oldestPending)
	{
		std::uint64_t dropped = 0;
		while (!samples.empty() && samples.front()->frame < a_oldestPending) {
			samples.pop_front();
			++dropped;
		}
		return dropped;
	}
}
