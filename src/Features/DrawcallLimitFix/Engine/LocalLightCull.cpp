#include "LocalLightCull.h"

#include "EngineAccess.h"
#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "PassCapture.h"
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
		std::mutex firstMutex;  // TEMP: the first lost pass
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

		// BSShadowParabolicLight::Accumulate (vtable slot 9, 0x14151b960): the point lights' cull and registration, timed.
		std::atomic<std::int64_t> accumulateTicks{ 0 };
		std::atomic<std::uint64_t> accumulateCalls{ 0 };
		struct Accumulate
		{
			static void thunk(void* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				func(a_light, a_count, a_arg2, a_arg3);
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
		logger::info("[DCLF] point lights' shadow culls without DCLF's entries{}", probe ? " (probe: nothing skipped)" : "");
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
		frameHeld = std::move(next);
	}

	void NoteRegistration(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_withheld, std::uint32_t a_source)
	{
		if (!installed || !parityFrame.load(std::memory_order_relaxed) || a_withheld || !a_pass || !a_pass->geometry)
			return;
		const auto* exclusion = frameExclusion.load(std::memory_order_acquire);
		std::uint32_t mode = 0;
		if (!exclusion || !PassCapture::Get().ShadowModeOfBatch(a_batch, mode) || mode != kParabolicMode)
			return;
		stats.parityPasses.fetch_add(1, std::memory_order_relaxed);
		const auto& geometries = exclusion->candidates->geometries;
		if (const auto it = geometries.find(a_pass->geometry); it != geometries.end() && exclusion->excluded[exclusion->candidates->geometryEntry[it->second]]) {
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
		return timing + "\n" + withFirst;
	}
}
