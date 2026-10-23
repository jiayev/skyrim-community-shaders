// SunAccumulation: the sun's entry exclusion (render thread, after the full-frustum cull), the registration-free accumulation's
// thunks on the sun's Accumulate and its registrations (the render thread and the registration jobs), and the sun's bits for removed
// entries.
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>

#include "State.h"
#include "Features/Skylighting.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Shadows::Sun
{
	namespace
	{
		using namespace Engine;

		// Module offsets, AE 1.6.1170 and SE 1.5.97.
		// CalculateAndDrawShadowCasterLights -> FUN_141511f30. AE: a CALL. SE: a tail JMP (SE 0x1412e2f32), at the end of the sun part SE
		// keeps out of line (FUN_1412e2e10, which CalculateAndDrawShadowCasterLights calls first).
		constexpr DCLF::Engine::Offset kFullFrustumCallSite{ 0x14cbcb6, 0x12e2f32 };
		constexpr DCLF::Engine::Offset kFullFrustumCull{ 0x1511f30, 0x1324e00 };
		constexpr DCLF::Engine::Offset kRegisterCallSites[2] = { { 0xe28bc3, 0xd512f3 }, { 0xe28c89, 0xd513d9 } };  // FUN_140e28af0 -> FUN_1414b2140
		constexpr DCLF::Engine::Offset kRegister{ 0x14b2140, 0x12ca110 };
		constexpr DCLF::Engine::Offset kCascadeCallSite{ 0x1511d28, 0x1324bf2 };  // Accumulate -> FUN_1414f0920, once per cascade
		constexpr DCLF::Engine::Offset kCascade{ 0x14f0920, 0x1305240 };
		constexpr DCLF::Engine::Offset kCascadeCullCallSite{ 0x14bf41e, 0x12d6d16 };  // FUN_1414bf320 (mode 2) -> FUN_140e305c0, once per full-frustum process
		constexpr DCLF::Engine::Offset kCascadeCull{ 0xe305c0, 0xd588a0 };
		constexpr DCLF::Engine::Offset kMaskClearCallSite{ 0x644d7e, 0x5b211e };  // Main::Draw -> FUN_1414cb640, the activeLightMask clear
		constexpr DCLF::Engine::Offset kMaskClear{ 0x14cb640, 0x12e2110 };
		constexpr DCLF::Engine::Offset kAppendNonAccum{ 0xe30520, 0xd58850 };  // BSGeometryListCullingProcess::AppendNonAccum, vtable slot 0x19

		/** @brief Whether the instruction at a_address is a JMP rel32 to a_target (CallsTo's check for a tail-call site). */
		bool JumpsTo(std::uintptr_t a_address, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			if (bytes[0] != 0xE9)
				return false;
			std::int32_t displacement = 0;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
		}
		// The engine's AppendNonAccum (Hooks::AppendNonAccum's original), for an entry the veto gives back.
		void (*appendNonAccum)(void*, RE::NiAVObject*, std::uint64_t, std::uint64_t) = nullptr;
		constexpr std::size_t kDescriptorAccumulator = 0x48;  // ShadowmapDescriptor::shaderAccumulator
		// The full-frustum cull's lists (its second argument points at their base: one BSTArray<NiPointer<NiAVObject>> of 0x18 bytes per
		// scene list, kSceneListCount of them); the whole skip passes as many empty ones.
		constexpr std::size_t kSceneListBytes = 0x18;
		constexpr std::uint32_t kMaxSceneLists = 32;
		// FUN_1414f0920's fourth argument: the light's full-frustum processes, as the BSTArray<NiPointer<BSCullingProcess>> it reads (data,
		// then the size at +0x10). The whole skip passes one of its own, of the processes holding entries.
		struct ProcessList
		{
			RE::BSCullingProcess* const* data = nullptr;
			std::uint32_t capacity = 0;
			std::uint32_t pad0C = 0;
			std::uint32_t size = 0;
			std::uint32_t pad14 = 0;
		};
		static_assert(sizeof(ProcessList) == 0x18 && offsetof(ProcessList, size) == 0x10);

		// BSShaderAccumulator fields the sun's Accumulate sets per cascade (FUN_1414b2140 reads them).
		constexpr std::size_t kAccumulatorLightIndex = 0x160;  // shadow-light count + 1; 0xFFFF clears masks; 0 writes none
		constexpr std::size_t kAccumulatorLightBit = 0x164;    // 1 << count
		// BSGeometry (AE) and BSShaderProperty fields FUN_1414b2140 reads.
		constexpr std::size_t kGeometryShaderProperty = 0x128;
		constexpr std::size_t kGeometrySkinInstance = 0x130;
		constexpr std::size_t kGeometryRendererData = 0x138;
		constexpr std::size_t kGeometryType = 0x150;
		constexpr std::size_t kDismemberSkinReady = 0x98;

		// The feature's toggles (SetToggles): written at its install alone (the main thread, before DCLF's threads start), read-only after.
		Api::ToggleId skipToggle, excludeToggle;

		/** @brief Whether the registration skip applies this frame: the set it skips registrations by withholds casters. */
		bool M1Active(const ToggleSet& a_toggles)
		{
			return a_toggles.On(skipToggle) && PassCapture::ShadowWithholdingEnabled() &&
			       !PassCapture::Get().Bypassed();
		}

		/**
		 * @brief The sun's cascades skip the entries DCLF excludes on frame a_frame (else the engine accumulates them): the feature's
		 * Policy question. Normal path: true.
		 */
		bool ExcludeSun(std::uint32_t a_frame)
		{
			// The sun exclusion's check (Diagnostics/SunCasters) watches the excluded entries' registrations on its frames.
			return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
		}

		/** @brief Whether a bound lies outside any of the active planes (the engine's sphere test: n.c - d < -r). */
		bool Outside(const std::array<std::array<float, 4>, 6>& a_planes, std::uint32_t a_mask, const RE::NiBound& a_bound)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(a_mask & (1u << p)))
					continue;
				const auto& plane = a_planes[p];
				if (plane[0] * a_bound.center.x + plane[1] * a_bound.center.y + plane[2] * a_bound.center.z - plane[3] < -a_bound.radius)
					return true;
			}
			return false;
		}

		/**
		 * @brief The sun's Accumulate in progress on this thread: its cascade accumulators and, per accumulator, the
		 * set its registrations are skipped for (null: registered as usual).
		 */
		struct SunCall
		{
			std::vector<const void*> accumulators;
			std::vector<std::shared_ptr<const SetSnapshot>> claims;  // per cascade: the frame's set when DCLF draws its casters
			std::vector<std::uint32_t> descriptors;  // per accumulator: its descriptor index
			std::uint32_t count = 0;
			int cascade = -1;  // the cascade FUN_1414f0920 is culling (index into accumulators), -1 between them
			std::uint64_t skipped = 0;
			std::uint64_t registered = 0;

			int IndexOf(const void* a_accumulator) const
			{
				for (std::uint32_t i = 0; i < count; ++i)
					if (accumulators[i] == a_accumulator)
						return static_cast<int>(i);
				return -1;
			}
		};
		thread_local SunCall* currentCall = nullptr;

		// The sun's accumulators as the last Accumulate saw them, for spotting a sun registration made elsewhere: published
		// whole when they change, read by the registration threads.
		std::atomic<std::shared_ptr<const std::vector<const void*>>> knownAccumulators;

		/**
		 * @brief FUN_1414b2140 without the mode's registration: its early-outs (a dismemberment skin not yet set up, no
		 * shader property, nothing to draw) and its activeLightMask write, from the accumulator's fields.
		 */
		void WriteMaskOnly(void* a_accumulator, void* a_geometry)
		{
			auto* skin = At<RE::NiSkinInstance*>(a_geometry, kGeometrySkinInstance);
			if (skin && netimmerse_cast<RE::BSDismemberSkinInstance*>(skin) && At<std::uint8_t>(skin, kDismemberSkinReady) == 0)
				return;
			void* property = At<void*>(a_geometry, kGeometryShaderProperty);
			if (!property)
				return;
			if (!At<void*>(a_geometry, kGeometryRendererData) && !skin) {
				// vfunc 0x10 of the geometry, and the type byte 0xB: what the engine checks before giving up.
				using Vfunc = void* (*)(void*);
				auto* vtable = *reinterpret_cast<Vfunc**>(a_geometry);
				if (!vtable[0x10](a_geometry) && At<std::uint8_t>(a_geometry, kGeometryType) != 0xB)
					return;
			}
			const std::uint32_t index = At<std::uint32_t>(a_accumulator, kAccumulatorLightIndex);
			if (index == 0)
				return;
			void* lightData = At<void*>(property, kPropertyLightData);
			if (!lightData)
				return;
			auto& mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
			mask = index == 0xFFFF ? 0u : (mask | At<std::uint32_t>(a_accumulator, kAccumulatorLightBit));
		}
	}

	SunAccumulation& SunAccumulation::Get()
	{
		static SunAccumulation instance;
		return instance;
	}

	void SunAccumulation::SetToggles(Api::ToggleId a_skip, Api::ToggleId a_exclude)
	{
		skipToggle = a_skip;
		excludeToggle = a_exclude;
	}

	void SunAccumulation::ArmExclusion(RE::BSShadowDirectionalLight* a_light)
	{
		auto exclusion = Local::SunExclusionSlot::Get().Take();
		previousExclusion = std::move(frameState.exclusion);
		frameState.exclusion.reset();
		armedExclusion.reset();
		const auto toggles = ActiveToggles();
		if (!toggles.On(excludeToggle) || !M1Active(toggles) || !a_light)
			return;
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		if (!node || node->GetRuntimeData().sunShadowDirLight != a_light)
			return;
		if (!exclusion || !exclusion->candidates || !exclusion->removed || !exclusion->cleared) {
			++stats.exclusionMissing;
			return;
		}
		// Not a verdict on the frame's candidates (BeginFrame takes the builder's translation at the frame's start; none then): an entry
		// may hold a caster no epoch has drawn yet. The engine culls everything this frame.
		if (exclusion->candidates != Scene::FrameView::Get().GetSunCandidates()) {
			++stats.exclusionStale;
			return;
		}
		// The processes are the light's, made once with it; the cull's jobs append to them (each its own) only once it is armed.
		vetoProcesses.clear();
		for (auto& process : a_light->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray)
			if (process)
				vetoProcesses.push_back({ process.get() });
		vetoStamp = stampCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		armedExclusion = std::move(exclusion);
		vetoExclusion.store(armedExclusion.get(), std::memory_order_release);
	}

	bool SunAccumulation::ArmWhole(RE::BSShadowDirectionalLight* a_light)
	{
		const auto toggles = ActiveToggles();
		if (!toggles.On(excludeToggle) || !M1Active(toggles) || !a_light)
			return false;
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		if (!node || node->GetRuntimeData().sunShadowDirLight != a_light)
			return false;
		// Every cascade view of the sun DCLF's this frame (AddCaster, before this call site, covered them) and its casters the set's: then
		// nothing the engine's cascades would register is drawn by them. Parity frames keep the exclusion (SunCasters watches them).
		std::uint32_t cascades = 0;
		bool covered = ExcludeSun(Scene::FrameView::Get().GetFrame()) && Global<std::uint32_t>(Address::kSceneListCount) <= kMaxSceneLists;
		for (const auto& view : ShadowViews::Get().All()) {
			if (view.light != a_light || view.kind != ShadowViews::Kind::Directional || view.focus)
				continue;
			++cascades;
			covered = covered && view.covered && PassCapture::Get().CastersForBatch(view.batch);
		}
		if (!covered || cascades < a_light->GetRuntimeData().shadowmapDescriptors.size()) {
			++stats.wholeDeclined;
			return false;
		}
		// The builder's exclusion is not needed: taken as ArmExclusion would, so the slot's hand-off stays the same.
		previousExclusion = Local::SunExclusionSlot::Get().Take();
		frameState.exclusion.reset();
		armedExclusion.reset();
		vetoStamp = stampCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		// No registration reads the set now (the last frame's ended at its mask clear).
		wholeCleared.NewRound();
		frameState.stamp = vetoStamp;
		frameState.whole = true;
		frameState.skipExcluded = true;
		++stats.wholeFrames;
		return true;
	}

	void SunAccumulation::WholeApplied()
	{
		Diagnostics::Notify(Diagnostics::SunExclusionApplied{ true });
		exclusionLive.store(true, std::memory_order_release);
	}

	bool SunAccumulation::ClearedThisFrame(const void* a_geometry, bool a_clears)
	{
		const auto found = wholeCleared.Test(a_geometry, a_clears);
		if (found == FrameAddressSet::Found::kFull)
			bitStats.wholeUnstamped.fetch_add(1, std::memory_order_relaxed);
		return found == FrameAddressSet::Found::kYes;
	}

	void SunAccumulation::ExcludeEntries()
	{
		// The cull's jobs are done (JobList::Finish): their appends and counts are visible here.
		if (!vetoExclusion.exchange(nullptr, std::memory_order_acq_rel))
			return;
		const std::int64_t start = Now();
		auto exclusion = std::move(armedExclusion);
		const std::uint32_t stamp = vetoStamp;
		const auto& candidates = *exclusion->candidates;
		for (auto& p : vetoProcesses) {
			stats.entriesSeen += p.seen;
			// A process left empty by the veto gets its first entry back: the cascade cull sets its planes up from the first.
			if (p.vetoed && p.vetoed == p.seen) {
				exclusion->removed[p.firstEntry].store(0, std::memory_order_relaxed);
				appendNonAccum(const_cast<void*>(p.process), p.firstVetoed, 0, 0);
				--p.vetoed;
				p.givenBack = true;
			}
			stats.entriesRemoved += p.vetoed;
		}
		// CS_DCLF_PERSISTENT_PARITY: no excluded entry in the arrays but one given back (the veto saw every append).
		if (Diagnostics::Observe().scene && Diagnostics::ParityDue(stamp)) {
			std::uint32_t left = 0, givenBack = 0;
			for (const auto& p : vetoProcesses) {
				const auto& array = static_cast<const RE::BSCullingProcess*>(p.process)->objectArray;
				for (std::uint32_t r = 0; r < array.size(); ++r)
					if (const auto it = candidates.entries.find(array[r].get()); it != candidates.entries.end() && exclusion->excluded[it->second])
						++left;
				givenBack += p.givenBack ? 1 : 0;
			}
			if (left > givenBack) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (logged++ < 8)
					logger::warn("[DCLF] sun entry exclusion: {} excluded entries in the full-frustum arrays after the veto ({} given back) <- VETO", left, givenBack);
			}
		}
		++stats.exclusionFrames;
		stats.candidates += candidates.Count();
		stats.excluded += exclusion->excludedCount;
		frameState.exclusion = std::move(exclusion);
		frameState.stamp = stamp;
		frameState.skipExcluded = ExcludeSun(Scene::FrameView::Get().GetFrame());
		Diagnostics::Notify(Diagnostics::SunExclusionApplied{ frameState.skipExcluded });
		exclusionLive.store(true, std::memory_order_release);
		stats.filterTicks += Now() - start;
	}

	void SunAccumulation::ResetFrameSun()
	{
		bitsReady.store(false, std::memory_order_relaxed);
		exclusionLive.store(false, std::memory_order_relaxed);
		frameState.whole = false;
		for (auto& cascade : frameState.cascades)
			cascade = {};
		frameState.cascadeCount = 0;
		frameState.sunBits = 0;
		Diagnostics::Notify(Diagnostics::SunBitsSet{ 0 });
	}

	void SunAccumulation::EndFullFrustumWindow()
	{
		// A frame whose full-frustum cull did not run (the sun does not draw: an interior) has no cascades and no exclusion:
		// nothing of the last frame that ran it stays live.
		if (!std::exchange(fullFrustumRan, false))
			ResetFrameSun();
	}

	std::uint32_t SunAccumulation::RemovedGeometryIndex(const RE::BSGeometry* a_geometry) const
	{
		const auto* exclusion = frameState.exclusion.get();
		if (!exclusion)
			return ~0u;
		const auto& candidates = *exclusion->candidates;
		const auto it = candidates.geometries.find(a_geometry);
		if (it == candidates.geometries.end() || exclusion->removed[candidates.geometryEntry[it->second]].load(std::memory_order_relaxed) != frameState.stamp)
			return ~0u;
		return it->second;
	}

	void SunAccumulation::ApplySunBits(RE::BSGeometry* a_geometry, bool a_clears)
	{
		void* property = At<void*>(a_geometry, kGeometryShaderProperty);
		if (!property)
			return;
		void* lightData = At<void*>(property, kPropertyLightData);
		if (!lightData)
			return;
		const auto& state = frameState;
		// The whole skip: every geometry the main camera registers (none was culled into a cascade), once a frame until its clearing one.
		const bool whole = state.whole;
		if (whole) {
			if (ClearedThisFrame(a_geometry, a_clears))
				return;
		} else {
			const std::uint32_t index = RemovedGeometryIndex(a_geometry);
			if (index == ~0u)
				return;
			auto& cleared = state.exclusion->cleared[index];
			if (cleared.load(std::memory_order_relaxed) == state.stamp)
				return;
			if (a_clears)
				cleared.store(state.stamp, std::memory_order_relaxed);
		}
		const auto& bound = a_geometry->worldBound;
		std::uint32_t bits = 0;
		for (std::uint32_t c = 0; c < state.cascadeCount && c < state.cascades.size(); ++c) {
			const auto& cascade = state.cascades[c];
			if (cascade.captured && !Outside(cascade.planes, cascade.planeMask, bound) &&
				!(cascade.customMask && Outside(cascade.customPlanes, cascade.customMask, bound)))
				bits |= cascade.bit;
		}
		auto& mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
		mask |= bits;
		(whole ? bitStats.wholeWritten : bitStats.written).fetch_add(1, std::memory_order_relaxed);
		if (bits)
			(whole ? bitStats.wholeWithBits : bitStats.withBits).fetch_add(1, std::memory_order_relaxed);
	}

	struct SunAccumulation::Hooks
	{
		/** @brief BSShadowDirectionalLight::Accumulate (vfunc 9, AE 0x141511c80). */
		struct Accumulate
		{
			static void thunk(RE::BSShadowDirectionalLight* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
				// DCLF switched off: the engine's own Accumulate (no claims, no bits of DCLF's).
				if (!node || node->GetRuntimeData().sunShadowDirLight != a_light || !ShellState::Running()) {
					func(a_light, a_count, a_arg2, a_arg3);
					return;
				}
				auto& self = SunAccumulation::Get();
				SunCall call;
				auto& descriptors = a_light->GetRuntimeData().shadowmapDescriptors;
				const bool active = M1Active(ActiveToggles());
				call.accumulators.reserve(descriptors.size());
				call.claims.reserve(descriptors.size());
				for (std::uint32_t i = 0; i < descriptors.size(); ++i) {
					auto* accumulator = descriptors[i].shaderAccumulator.get();
					if (!accumulator)
						continue;
					call.accumulators.push_back(accumulator);
					call.descriptors.push_back(i);
					// The set PassCapture would withhold this accumulator's passes by, when the renderer is a shadow view's.
					const auto* data = active ? accumulator->GetRuntimeData() : nullptr;
					call.claims.push_back(data ? PassCapture::Get().CastersForBatch(data->batchRenderer) : nullptr);
					++call.count;
				}
				if (const auto known = knownAccumulators.load(std::memory_order_acquire); !known || *known != call.accumulators)
					knownAccumulators.store(std::make_shared<const std::vector<const void*>>(call.accumulators), std::memory_order_release);
				// Room for every cascade before any is captured: the registration threads read the cascades only once bitsReady
				// is set, which the full-frustum cull cleared before this.
				if (self.frameState.cascades.size() < call.count)
					self.frameState.cascades.resize(call.count);
				const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
				Diagnostics::Notify<Diagnostics::EnginePoint::SunAccumulateBegin>();
				// Each cascade's volume is DCLF's (the Sun board, from the camera UpdateCamera set up). A cascade without DCLF's stays unknown,
				// its bit unset.
				for (std::uint32_t i = 0; i < call.count; ++i) {
					auto& cascade = self.frameState.cascades[i];
					cascade = {};
					if (const auto* mine = SunBoard().CascadeOf(call.descriptors[i], frame)) {
						cascade.planes = mine->cull.plane;
						cascade.planeMask = mine->cull.mask;
						cascade.customPlanes = mine->caster.plane;
						cascade.customMask = mine->caster.mask;
						cascade.captured = true;
					} else {
						Diagnostics::Notify(Diagnostics::SunCascadeMissing{ frame, call.descriptors[i] });
					}
				}
				currentCall = &call;
				const std::int64_t start = Now();
				func(a_light, a_count, a_arg2, a_arg3);
				const std::int64_t ticks = Now() - start;
				currentCall = nullptr;
				// The captured cascades: what a removed entry's geometries are tested against when the main camera registers
				// them. From here until the next mask clear, those registrations take DCLF's bits.
				{
					auto& state = self.frameState;
					state.cascadeCount = call.count;
					state.cascadesFrame = frame;
					state.sunBits = 0;
					for (std::uint32_t i = 0; i < call.count; ++i)
						state.sunBits |= state.cascades[i].captured ? state.cascades[i].bit : 0u;
					Diagnostics::Notify(Diagnostics::SunBitsSet{ state.sunBits });
					if (self.exclusionLive.load(std::memory_order_relaxed))
						self.bitsReady.store(true, std::memory_order_release);
				}

				auto& stats = self.stats;
				++stats.frames;
				stats.activeFrames += active ? 1 : 0;
				stats.skipped += call.skipped;
				stats.registered += call.registered;
				stats.accumulateTicks += ticks;
				stats.accumulateMax = std::max(stats.accumulateMax, ticks);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief An owned geometry's activeLightMask as the engine would leave it once its main registration had read it: 0. DCLF
		 * draws its main pass (PrimaryCull::Owned), so no main registration ever consumes the bits the shadow lights' accumulations
		 * write, and every later registration of it (the reflections, the other views) reads 0, as with the engine's own.
		 */
		static void ClearOwnedMask(void* a_geometry)
		{
			if (void* property = At<void*>(a_geometry, kGeometryShaderProperty))
				if (void* lightData = At<void*>(property, kPropertyLightData))
					At<std::uint32_t>(lightData, kLightDataActiveMask) = 0;
		}

		/** @brief FUN_140e28af0's calls of the accumulator's registration, FUN_1414b2140(accumulator, geometry, arg). */
		template <std::uint32_t Site>
		struct Register
		{
			static std::uint64_t thunk(void* a_accumulator, void* a_geometry, std::uint64_t a_arg)
			{
				SunCall* call = currentCall;
				const int cascade = call ? call->IndexOf(a_accumulator) : -1;
				auto& self = SunAccumulation::Get();
				auto* geometry = static_cast<RE::BSGeometry*>(a_geometry);
				if (cascade < 0) {
					if (!call) {
						if (const auto known = knownAccumulators.load(std::memory_order_acquire);
							known && std::find(known->begin(), known->end(), a_accumulator) != known->end())
							self.offThread.fetch_add(1, std::memory_order_relaxed);
					}
					// An owned geometry: a shadow light's accumulation (+0x160 neither 0 nor 0xFFFF) leaves its mask 0, and no
					// registration of it takes the sun's bits (ClearOwnedMask).
					const std::uint32_t lightIndex = At<std::uint32_t>(a_accumulator, kAccumulatorLightIndex);
					const bool owned = PrimaryCull::Get().Owned(*geometry);
					if (owned) {
						const auto result = func(a_accumulator, a_geometry, a_arg);
						if (lightIndex != 0 && lightIndex != 0xFFFF)
							ClearOwnedMask(a_geometry);
						return result;
					}
					// The main camera's registrations read the mask, then clear it (+0x160 = 0xFFFF) for the next frame.
					if (self.bitsReady.load(std::memory_order_acquire))
						self.ApplySunBits(geometry, At<std::uint32_t>(a_accumulator, kAccumulatorLightIndex) == 0xFFFF);
					else if (!call && self.exclusionLive.load(std::memory_order_acquire) && self.UnderRemovedEntry(geometry))
						self.bitStats.notReady.fetch_add(1, std::memory_order_relaxed);
					// And the point lights' bits of a geometry their culls left out (Features/Shadows/Local's LocalLightCull: a static call).
					if (lightIndex == 0xFFFF)
						Local::LocalLightCull::NoteMainRegistration(geometry);
					return func(a_accumulator, a_geometry, a_arg);
				}
				std::uint64_t result = 1;
				const auto& claims = call->claims[cascade];
				const bool claimed = claims && (claims->PhasesOf(geometry) & kSetCaster);
				// A removed entry's geometry reaching a cascade: the entry was culled through another path (checked every 64th cull).
				if (self.exclusionLive.load(std::memory_order_relaxed) && (self.frameState.stamp & 63) == 0 && self.UnderRemovedEntry(geometry))
					++self.stats.cascadeRegistrationsUnderRemoved;
				if (claimed) {
					// An owned geometry's mask stays 0 (ClearOwnedMask); any other takes the cascade's bit for its main registration.
					if (PrimaryCull::Get().Owned(*geometry))
						ClearOwnedMask(a_geometry);
					else
						WriteMaskOnly(a_accumulator, a_geometry);
					++call->skipped;
				} else {
					const std::uint32_t passesBefore = PassCapture::PassesOnThisThread();
					result = func(a_accumulator, a_geometry, a_arg);
					// One that built a pass (a registration without one draws nothing).
					if (PassCapture::PassesOnThisThread() != passesBefore)
						Diagnostics::Notify(Diagnostics::SunCasterRegistered{ geometry });
					// A caster the sun's views do not claim is registered for its native shadow; if DCLF draws its main pass, the
					// cascade's bit has no main registration to read it (ClearOwnedMask).
					if (PrimaryCull::Get().Owned(*geometry))
						ClearOwnedMask(a_geometry);
					++call->registered;
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief CalculateAndDrawShadowCasterLights' call of the full-frustum cull (FUN_141511f30). */
		struct FullFrustum
		{
			static void thunk(void* a_light, void* a_lists, void* a_arg)
			{
				auto& self = SunAccumulation::Get();
				self.ResetFrameSun();
				self.fullFrustumRan = true;
				// DCLF switched off: the engine's whole cull (no exclusion, no whole skip: the views' coverage is not decided while off).
				if (!ShellState::Running()) {
					func(a_light, a_lists, a_arg);
					return;
				}
				auto* light = static_cast<RE::BSShadowDirectionalLight*>(a_light);
				if (self.ArmWhole(light)) {
					// The engine's cull with no scene list in it (each empty) and an empty extra list (in place of the main camera's portal
					// graph's alwaysRenderChildren: every loaded cell's actor and dynamic nodes, and the particle manager): its processes' setup
					// as the engine does it, nothing appended, so the cascade culls cull nothing. With the list culls' job list null for the
					// call, their empty culls run inline (the engine's own path without one): no jobs queued, and no wait behind the job
					// threads' other work. Only the render thread reads the job list while the game runs (Main::Draw,
					// CalculateAndDrawShadowCasterLights, this cull; then shutdown).
					static const std::array<std::uint8_t, kSceneListBytes * kMaxSceneLists> emptyLists{};
					auto lists = reinterpret_cast<std::uintptr_t>(emptyLists.data());
					auto& jobs = Global<void*>(Address::kListCullJobs);
					void* const held = jobs;
					const std::int64_t start = Now();
					jobs = nullptr;
					func(a_light, &lists, const_cast<std::uint8_t*>(emptyLists.data()));
					jobs = held;
					self.WholeApplied();
					const std::int64_t ticks = Now() - start;
					self.stats.fullFrustumTicks += ticks;
					self.stats.fullFrustumMax = std::max(self.stats.fullFrustumMax, ticks);
					return;
				}
				self.ArmExclusion(light);
				const std::int64_t start = Now();
				func(a_light, a_lists, a_arg);
				const std::int64_t ticks = Now() - start;
				auto& stats = self.stats;
				stats.fullFrustumTicks += ticks;
				stats.fullFrustumMax = std::max(stats.fullFrustumMax, ticks);
				self.ExcludeEntries();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSGeometryListCullingProcess::AppendNonAccum (vtable slot 0x19, AE 0x140e30520): the only push into a full-frustum
		 * process's objectArray (Process1 -> AppendVirtual -> here, for every entry its cull passes; the sun's processes do not recurse,
		 * so the entry is the list's own). While the exclusion is armed, an excluded entry appended to one of the light's processes is
		 * stamped removed and not pushed: the veto at insertion, on the cull's job threads, instead of a walk of the arrays after it.
		 * The other processes of the class (the scene lists', precipitation's) pass through.
		 */
		struct AppendNonAccum
		{
			static void thunk(void* a_process, RE::NiAVObject* a_object, std::uint64_t a_2, std::uint64_t a_3)
			{
				auto& self = SunAccumulation::Get();
				if (const auto* exclusion = self.vetoExclusion.load(std::memory_order_acquire); exclusion && a_object) {
					for (auto& p : self.vetoProcesses) {
						if (p.process != a_process)
							continue;
						++p.seen;
						const auto& entries = exclusion->candidates->entries;
						if (const auto it = entries.find(a_object); it != entries.end() && exclusion->excluded[it->second]) {
							exclusion->removed[it->second].store(self.vetoStamp, std::memory_order_relaxed);
							if (!p.vetoed++) {
								p.firstVetoed = a_object;
								p.firstEntry = it->second;
							}
							return;
						}
						break;
					}
				}
				func(a_process, a_object, a_2, a_3);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Main::Draw's call of the activeLightMask clear, FUN_1414cb640: from its return until the sun accumulates
		 * again, the engine's masks hold none of the sun's bits, so DCLF's are not written either.
		 */
		struct MaskClear
		{
			static void thunk(std::uint64_t a_1, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
			{
				func(a_1, a_2, a_3, a_4);
				SunAccumulation::Get().bitsReady.store(false, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Accumulate's call of FUN_1414f0920(light, descriptor, count, full-frustum processes, arg), once per cascade. */
		struct Cascade
		{
			static void thunk(void* a_light, void* a_descriptor, std::uint32_t* a_count, void* a_processes, std::uint32_t a_arg)
			{
				SunCall* call = currentCall;
				auto& self = SunAccumulation::Get();
				if (call && a_descriptor) {
					void* accumulator = At<void*>(a_descriptor, kDescriptorAccumulator);
					call->cascade = call->IndexOf(accumulator);
					if (call->cascade >= 0) {
						auto& cascade = self.frameState.cascades[call->cascade];
						// Accumulate has just set it: 1 << the shadow-light count (the light mask's bookkeeping, not a cull's).
						cascade.bit = At<std::uint32_t>(accumulator, kAccumulatorLightBit);
					}
				}
				// The whole skip: the scene lists and the extra list were culled into nothing, so only processes holding entries (none, unless
				// another caller appended) are culled into the cascade; an empty one would cost a cull's set-up and a registration under the
				// context's lock (FUN_140e28af0) for nothing.
				if (call && self.frameState.whole && a_processes) {
					std::array<RE::BSCullingProcess*, kMaxSceneLists> held{};
					ProcessList kept{ held.data() };
					std::uint32_t left = 0;
					for (auto& process : *static_cast<RE::BSTArray<RE::NiPointer<RE::BSCullingProcess>>*>(a_processes)) {
						if (process && !process->objectArray.empty() && kept.size < held.size())
							held[kept.size++] = process.get();
						else
							++left;
					}
					kept.capacity = kept.size;
					self.stats.wholeProcessesCulled += kept.size;
					self.stats.wholeProcessesLeft += left;
					func(a_light, a_descriptor, a_count, &kept, a_arg);
				} else {
					func(a_light, a_descriptor, a_count, a_processes, a_arg);
				}
				if (call)
					call->cascade = -1;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSCullingProcess::Process1 (vtable slot 0x16), for the sun's cascade culls: the objectArray holds no excluded
		 * entry (ExcludeEntries), but one can still be reached through a node above it that stays. Skipped there too, stamped
		 * as removed, so that a geometry of it the main camera registers takes DCLF's bits (ApplySunBits). Only inside the
		 * sun's Accumulate on its own thread (currentCall), and only while the frame skips them (FrameState::skipExcluded).
		 */
		struct CascadeProcess1
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				if (SunCall* call = currentCall; call && call->cascade >= 0 && a_object) {
					auto& self = SunAccumulation::Get();
					const auto& state = self.frameState;
					if (self.exclusionLive.load(std::memory_order_relaxed) && state.skipExcluded && state.exclusion && state.exclusion->candidates) {
						const auto& entries = state.exclusion->candidates->entries;
						if (const auto it = entries.find(a_object); it != entries.end() && state.exclusion->excluded[it->second]) {
							state.exclusion->removed[it->second].store(state.stamp, std::memory_order_relaxed);
							self.skipStats.skipped.fetch_add(1, std::memory_order_relaxed);
							return;
						}
					}
				}
				func(a_process, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief FUN_1414bf320's cascade cull, FUN_140e305c0(full-frustum process, cascade process, arg). */
		struct CascadeCull
		{
			static void thunk(void* a_fullProcess, RE::NiCullingProcess* a_process, std::uint64_t a_arg)
			{
				func(a_fullProcess, a_process, a_arg);
				// After it: the traversal's first call (vfunc 0xB8) has set the process up from the cascade's camera. With
				// an empty objectArray it made none, and the planes are not this cascade's.
				SunCall* call = currentCall;
				if (!call || call->cascade < 0 || !a_process)
					return;
				Diagnostics::Notify(Diagnostics::SunCascadeCulled{ a_fullProcess, a_process, static_cast<std::uint32_t>(call->cascade) });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void SunAccumulation::Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		const auto fullFrustum = base + kFullFrustumCallSite;
		const auto registerA = base + kRegisterCallSites[0];
		const auto registerB = base + kRegisterCallSites[1];
		const auto cascadeCall = base + kCascadeCallSite;
		const auto cascadeCull = base + kCascadeCullCallSite;
		const auto maskClear = base + kMaskClearCallSite;
		REL::Relocation<std::uintptr_t> listVtable{ RE::VTABLE_BSGeometryListCullingProcess[0] };
		// SE's full-frustum site is a tail JMP: patched as one (the thunk's return goes to the sun part's caller, as the engine's would).
		const bool fullFrustumJumps = REL::Module::IsSE();
		const bool fullFrustumFound = fullFrustumJumps ? JumpsTo(fullFrustum, base + kFullFrustumCull) : CallsTo(fullFrustum, base + kFullFrustumCull);
		if (!fullFrustumFound || !CallsTo(registerA, base + kRegister) || !CallsTo(registerB, base + kRegister) ||
			!CallsTo(cascadeCall, base + kCascade) || !CallsTo(cascadeCull, base + kCascadeCull) || !CallsTo(maskClear, base + kMaskClear) ||
			reinterpret_cast<const std::uintptr_t*>(listVtable.address())[0x19] != base + kAppendNonAccum) {
			logger::warn("[DCLF] sun accumulation: the calls are not where expected; the engine keeps the sun's accumulation");
			return;
		}
		// The entry exclusion's veto, at the full-frustum processes' only push.
		stl::write_vfunc<0x19, Hooks::AppendNonAccum>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		appendNonAccum = reinterpret_cast<decltype(appendNonAccum)>(Hooks::AppendNonAccum::func.address());
		stl::write_vfunc<0x9, Hooks::Accumulate>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_thunk_call<Hooks::Register<0>>(registerA);
		stl::write_thunk_call<Hooks::Register<1>>(registerB);
		if (fullFrustumJumps)
			stl::write_thunk_jmp<Hooks::FullFrustum>(fullFrustum);
		else
			stl::write_thunk_call<Hooks::FullFrustum>(fullFrustum);
		stl::write_thunk_call<Hooks::Cascade>(cascadeCall);
		stl::write_thunk_call<Hooks::CascadeCull>(cascadeCull);
		stl::write_thunk_call<Hooks::MaskClear>(maskClear);
		// The cascades' skip of the excluded entries they reach anyway (the point lights' culls have their own: Shadows/Local's LocalLightCull).
		stl::write_vfunc<0x16, Hooks::CascadeProcess1>(RE::VTABLE_BSCullingProcess[0]);
		installed = true;
		logger::info("[DCLF] sun accumulation installed (Accumulate, its cascades and registrations, the full-frustum cull)");
	}

	SunAccumulation::Stats SunAccumulation::TakeStats()
	{
		stats.offThread = offThread.exchange(0, std::memory_order_relaxed);
		const auto forward = Local::SunExclusionSlot::Get().TakeForwardCounts();
		stats.exclusionForwarded = forward.forwarded;
		stats.exclusionUnforwarded = forward.unforwarded;
		return std::exchange(stats, {});
	}

	SunAccumulation::ExclusionCounts SunAccumulation::TakeExclusionCounts()
	{
		const auto take = [](std::atomic<std::uint64_t>& a_value) { return a_value.exchange(0, std::memory_order_relaxed); };
		ExclusionCounts c;
		c.written = take(bitStats.written);
		c.withBits = take(bitStats.withBits);
		c.notReady = take(bitStats.notReady);
		c.skipped = take(skipStats.skipped);
		c.wholeWritten = take(bitStats.wholeWritten);
		c.wholeWithBits = take(bitStats.wholeWithBits);
		c.wholeUnstamped = take(bitStats.wholeUnstamped);
		return c;
	}
}
