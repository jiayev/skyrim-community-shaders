#include "SunAccumulation.h"

#include "PassCapture.h"
#include "PrimaryCull.h"
#include "SceneStore.h"
#include "ShadowProbe.h"
#include "Switches.h"
#include "Toggles.h"
#include "VolumetricProbe.h"

#include <array>
#include <cstring>
#include <map>
#include <string>

#include "State.h"
#include "Features/Skylighting.h"

namespace DCLF
{
	namespace
	{
		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kFullFrustumCallSite = 0x14cbcb6;  // CalculateAndDrawShadowCasterLights -> FUN_141511f30
		constexpr std::uintptr_t kFullFrustumCull = 0x1511f30;
		constexpr std::uintptr_t kRegisterCallSites[2] = { 0xe28bc3, 0xe28c89 };  // FUN_140e28af0 -> FUN_1414b2140
		constexpr std::uintptr_t kRegister = 0x14b2140;
		constexpr std::uintptr_t kCascadeCallSite = 0x1511d28;  // Accumulate -> FUN_1414f0920, once per cascade
		constexpr std::uintptr_t kCascade = 0x14f0920;
		constexpr std::uintptr_t kCascadeCullCallSite = 0x14bf41e;  // FUN_1414bf320 (mode 2) -> FUN_140e305c0, once per full-frustum process
		constexpr std::uintptr_t kCascadeCull = 0xe305c0;
		constexpr std::uintptr_t kMaskClearCallSite = 0x644d7e;  // Main::Draw -> FUN_1414cb640, the activeLightMask clear
		constexpr std::uintptr_t kMaskClear = 0x14cb640;
		constexpr std::size_t kDescriptorAccumulator = 0x48;  // ShadowmapDescriptor::shaderAccumulator
		constexpr std::size_t kProcessObjectArray = 0x128;    // BSCullingProcess::objectArray

		// BSShaderAccumulator fields the sun's Accumulate sets per cascade (FUN_1414b2140 reads them).
		constexpr std::size_t kAccumulatorLightIndex = 0x160;  // shadow-light count + 1; 0xFFFF clears masks; 0 writes none
		constexpr std::size_t kAccumulatorLightBit = 0x164;    // 1 << count
		// BSGeometry (AE) and BSShaderProperty fields FUN_1414b2140 reads.
		constexpr std::size_t kGeometryShaderProperty = 0x128;
		constexpr std::size_t kGeometrySkinInstance = 0x130;
		constexpr std::size_t kGeometryRendererData = 0x138;
		constexpr std::size_t kGeometryType = 0x150;
		constexpr std::size_t kDismemberSkinReady = 0x98;
		constexpr std::size_t kPropertyLightData = 0x70;
		constexpr std::size_t kLightDataActiveMask = 0x1C;
		constexpr std::uint32_t kMaxCascades = 4;

		bool CallsTo(std::uintptr_t a_address, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			if (bytes[0] != 0xE8)
				return false;
			std::int32_t displacement = 0;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
		}

		template <class T>
		T& At(void* a_base, std::size_t a_offset)
		{
			return *reinterpret_cast<T*>(static_cast<std::byte*>(a_base) + a_offset);
		}

		bool TimingEnabled()
		{
			static const bool enabled = SwitchEnabled("CS_DCLF_SUN_TIMING");
			return enabled;
		}

		/** @brief Diagnostics that need the engine's sun registrations: M1 stays off for the whole run. */
		bool RegistrationsNeeded()
		{
			static const bool needed = PassCapture::CascadeProbeEnabled() || VolumetricProbe::Enabled() || ShadowProbe::Enabled();
			return needed;
		}

		/** @brief Whether M1 applies this frame: the claims it skips registrations by exist and are in force. */
		bool M1Active(const ToggleSet& a_toggles)
		{
			return a_toggles.skipSunAccumulation && PassCapture::ShadowWithholdingEnabled() && PassCapture::VolumetricClaimsAvailable() &&
			       !PassCapture::Get().Bypassed() && !RegistrationsNeeded();
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

		void CopyPlanes(const RE::NiFrustumPlanes& a_from, std::array<std::array<float, 4>, 6>& a_to, std::uint32_t& a_mask)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				const auto& plane = a_from.cullingPlanes[p];
				a_to[p] = { plane.normal.x, plane.normal.y, plane.normal.z, plane.constant };
			}
			a_mask = a_from.activePlanes.underlying() & 0x3Fu;
		}

		std::int64_t Now()
		{
			LARGE_INTEGER value{};
			QueryPerformanceCounter(&value);
			return value.QuadPart;
		}

		/**
		 * @brief The sun's Accumulate in progress on this thread: its cascade accumulators and, per accumulator, the
		 * claim set its registrations are skipped for (null: registered as usual).
		 */
		struct SunCall
		{
			std::array<const void*, kMaxCascades> accumulators{};
			std::array<std::shared_ptr<const PassCapture::ClaimSet>, kMaxCascades> claims{};
			std::uint32_t count = 0;
			int cascade = -1;  // the cascade FUN_1414f0920 is culling (index into accumulators), -1 between them
			std::int64_t registrationTicks = 0;
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

		/**
		 * @brief [TEMP] CS_DCLF_SKYLIGHT_PROBE: what the engine draws into Skylighting's occlusion map (render mode 0x1C
		 * while Skylighting::inOcclusion), by what DCLF's tables hold of it. Render thread (SetupMask registers there).
		 */
		struct SkylightProbe
		{
			std::uint64_t calls = 0, withPass = 0;
			std::array<std::uint64_t, 3> byState{};  // untracked, tracked without a record, table object
			std::array<std::uint64_t, static_cast<std::size_t>(Ineligible::Count)> byReason{};
			std::map<std::string, std::uint64_t> untracked;  // "type / property / parent chain" -> count
			std::uint64_t frames = 0;
		};
		SkylightProbe skylightProbe;

		bool SkylightProbeEnabled()
		{
			static const bool enabled = SwitchEnabled("CS_DCLF_SKYLIGHT_PROBE") || SwitchEnabled("CS_DCLF_SKYLIGHT_PARITY");
			return enabled;
		}

		void NoteSkylightRegistration(RE::BSGeometry* a_geometry)
		{
			auto& probe = skylightProbe;
			++probe.calls;
			auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
			auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property);
			if (!lighting || !lighting->occlusionPasses.head)
				return;
			++probe.withPass;
			if (SunAccumulation::Get().skyRegistrations.size() < 65536)
				SunAccumulation::Get().skyRegistrations.push_back(a_geometry);
			Ineligible reason = Ineligible::None;
			const int state = SceneStore::Get().ProbeTableState(a_geometry, reason);
			++probe.byState[state];
			if (state == 1)
				++probe.byReason[static_cast<std::size_t>(reason)];
			if (state == 0 && probe.untracked.size() < 400) {
				std::string chain;
				int depth = 0;
				for (auto* node = a_geometry->parent; node && depth < 4; node = node->parent, ++depth)
					chain += fmt::format("/{}", node->name.c_str() && *node->name.c_str() ? node->name.c_str() : (node->GetRTTI() ? node->GetRTTI()->name : "?"));
				const auto* reference = a_geometry->GetUserData();
				++probe.untracked[fmt::format("{} {} ref {:X} {}", a_geometry->GetRTTI() ? a_geometry->GetRTTI()->name : "?", a_geometry->name.c_str() ? a_geometry->name.c_str() : "?",
					reference ? reference->GetFormID() : 0, chain)];
			} else if (state == 0) {
				++probe.untracked["(more)"];
			}
		}
		// The sun's accumulators as the last Accumulate saw them, for spotting a sun registration made elsewhere.
		std::array<std::atomic<const void*>, kMaxCascades> knownAccumulators{};

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

	bool SunAccumulation::ExclusionProbe()
	{
		static const bool probe = SwitchValue("CS_DCLF_SUN_EXCLUDE") == "probe";
		return probe;
	}

	void SunAccumulation::ExcludeEntries(RE::BSShadowDirectionalLight* a_light)
	{
		auto exclusion = std::move(pendingExclusion);
		previousExclusion = std::move(frameState.exclusion);
		frameState.exclusion.reset();
		const auto toggles = Toggles::Get().Active();
		if (!toggles.excludeSunEntries || !M1Active(toggles) || !a_light)
			return;
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		if (!node || node->GetRuntimeData().sunShadowDirLight != a_light)
			return;
		if (!exclusion || !exclusion->candidates || !exclusion->removed || !exclusion->cleared) {
			++stats.exclusionMissing;
			return;
		}
		// Built for other candidates: the scene changed since the claims it follows, and an entry may hold a caster no
		// epoch has drawn yet. The engine culls everything this frame.
		if (exclusion->candidates->generation != SceneStore::Get().GetSunCandidatesGeneration()) {
			++stats.exclusionStale;
			return;
		}
		const std::int64_t start = Now();
		const bool probe = ExclusionProbe();
		const std::uint32_t stamp = stampCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto& candidates = *exclusion->candidates;
		for (auto& process : a_light->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray) {
			if (!process)
				continue;
			auto& array = process->objectArray;
			const std::uint32_t size = array.size();
			if (!size)
				continue;
			removeScratch.assign(size, 0);
			std::uint32_t removable = 0;
			for (std::uint32_t r = 0; r < size; ++r) {
				const auto* object = array[r].get();
				if (!object)
					continue;
				const auto it = candidates.entries.find(object);
				if (it != candidates.entries.end() && exclusion->excluded[it->second]) {
					removeScratch[r] = it->second + 1;
					++removable;
				}
			}
			if (removable == size) {
				removeScratch[0] = 0;
				--removable;
			}
			stats.entriesSeen += size;
			stats.entriesRemoved += removable;
			if (!removable)
				continue;
			std::uint32_t kept = 0;
			for (std::uint32_t r = 0; r < size; ++r) {
				if (removeScratch[r]) {
					exclusion->removed[removeScratch[r] - 1].store(stamp, std::memory_order_relaxed);
					if (!probe) {
						array[r].reset();
						continue;
					}
				}
				if (!probe && kept != r)
					array[kept] = std::move(array[r]);
				++kept;
			}
			if (!probe)
				array.resize(kept);
		}
		++stats.exclusionFrames;
		stats.candidates += candidates.entries.size();
		stats.excluded += exclusion->excludedCount;
		frameState.exclusion = std::move(exclusion);
		frameState.stamp = stamp;
		frameState.probe = probe;
		frameState.cascades = {};
		frameState.cascadeCount = 0;
		frameState.sunBits = 0;
		exclusionLive.store(true, std::memory_order_release);
		stats.filterTicks += Now() - start;
	}

	std::optional<bool> SunAccumulation::InSunCascades(const RE::NiBound& a_bound) const
	{
		// The cascades are captured only on a frame whose full-frustum cull applied the entry exclusion.
		if (!exclusionLive.load(std::memory_order_acquire))
			return std::nullopt;
		const auto& state = frameState;
		bool captured = false;
		for (std::uint32_t c = 0; c < state.cascadeCount && c < state.cascades.size(); ++c) {
			const auto& cascade = state.cascades[c];
			if (!cascade.captured)
				continue;
			captured = true;
			if (!Outside(cascade.planes, cascade.planeMask, a_bound) && !(cascade.customMask && Outside(cascade.customPlanes, cascade.customMask, a_bound)))
				return true;
		}
		if (!captured)
			return std::nullopt;
		return false;
	}

	std::uint32_t SunAccumulation::GpuCascades(std::uint32_t (&a_masks)[4][2], float (&a_planes)[4][12][4]) const
	{
		if (!exclusionLive.load(std::memory_order_acquire))
			return 0;
		std::uint32_t count = 0;
		const auto& state = frameState;
		for (std::uint32_t c = 0; c < state.cascadeCount && c < state.cascades.size() && count < 4; ++c) {
			const auto& cascade = state.cascades[c];
			if (!cascade.captured)
				continue;
			for (std::uint32_t p = 0; p < 6; ++p) {
				std::memcpy(a_planes[count][p], cascade.planes[p].data(), sizeof(float) * 4);
				std::memcpy(a_planes[count][6 + p], cascade.customPlanes[p].data(), sizeof(float) * 4);
			}
			a_masks[count][0] = cascade.planeMask;
			a_masks[count][1] = cascade.customMask;
			++count;
		}
		return count;
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
		const std::uint32_t index = RemovedGeometryIndex(a_geometry);
		if (index == ~0u)
			return;
		const auto& state = frameState;
		auto& cleared = state.exclusion->cleared[index];
		if (cleared.load(std::memory_order_relaxed) == state.stamp)
			return;
		if (a_clears)
			cleared.store(state.stamp, std::memory_order_relaxed);
		const auto& bound = a_geometry->worldBound;
		std::uint32_t bits = 0;
		for (std::uint32_t c = 0; c < state.cascadeCount && c < state.cascades.size(); ++c) {
			const auto& cascade = state.cascades[c];
			if (cascade.captured && !Outside(cascade.planes, cascade.planeMask, bound) &&
				!(cascade.customMask && Outside(cascade.customPlanes, cascade.customMask, bound)))
				bits |= cascade.bit;
		}
		auto& mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
		if (!state.probe) {
			mask |= bits;
			bitStats.written.fetch_add(1, std::memory_order_relaxed);
			if (bits)
				bitStats.withBits.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const std::uint32_t engine = mask & state.sunBits;
		bitStats.compared.fetch_add(1, std::memory_order_relaxed);
		if (engine == bits) {
			bitStats.agree.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (engine & ~bits)
			bitStats.engineOnly.fetch_add(1, std::memory_order_relaxed);
		if (bits & ~engine)
			bitStats.dclfOnly.fetch_add(1, std::memory_order_relaxed);
		static std::atomic<std::uint32_t> loggedEngine{ 0 }, loggedDclf{ 0 };
		if ((engine & ~bits ? loggedEngine : loggedDclf).fetch_add(1, std::memory_order_relaxed) < 12)
			logger::info("[DCLF] sun exclusion probe: bits differ: '{}' under '{}', engine {:#x}, DCLF {:#x} (cascades {:#x}), bound ({:.0f} {:.0f} {:.0f}) r {:.0f}",
				a_geometry->name.c_str() ? a_geometry->name.c_str() : "?", a_geometry->parent && a_geometry->parent->name.c_str() ? a_geometry->parent->name.c_str() : "?",
				engine, bits, state.sunBits, bound.center.x, bound.center.y, bound.center.z, bound.radius);
	}

	void SunAccumulation::NoteProbeUnclaimed(RE::BSGeometry* a_geometry, std::uint32_t a_passes)
	{
		// Without a pass the registration wrote only the mask, which DCLF's bits replace.
		if (!a_passes) {
			bitStats.probeNoPass.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		bitStats.probeUnclaimed.fetch_add(1, std::memory_order_relaxed);
		static std::uint32_t logged = 0;
		if (logged++ < 30) {
			auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
			logger::info("[DCLF] sun exclusion probe: the engine built {} pass(es) for '{}' (parent '{}', {}, property {}) under a would-be-removed entry",
				a_passes, a_geometry->name.c_str() ? a_geometry->name.c_str() : "?",
				a_geometry->parent && a_geometry->parent->name.c_str() ? a_geometry->parent->name.c_str() : "?",
				a_geometry->GetRTTI() ? a_geometry->GetRTTI()->name : "?", property && property->GetRTTI() ? property->GetRTTI()->name : "none");
		}
	}

	struct SunAccumulation::Hooks
	{
		/** @brief BSShadowDirectionalLight::Accumulate (vfunc 9, AE 0x141511c80). */
		struct Accumulate
		{
			static void thunk(RE::BSShadowDirectionalLight* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
				if (!node || node->GetRuntimeData().sunShadowDirLight != a_light) {
					func(a_light, a_count, a_arg2, a_arg3);
					return;
				}
				auto& self = SunAccumulation::Get();
				SunCall call;
				auto& descriptors = a_light->GetRuntimeData().shadowmapDescriptors;
				const bool active = M1Active(Toggles::Get().Active());
				for (std::uint32_t i = 0; i < descriptors.size() && call.count < kMaxCascades; ++i) {
					auto* accumulator = descriptors[i].shaderAccumulator.get();
					if (!accumulator)
						continue;
					call.accumulators[call.count] = accumulator;
					knownAccumulators[call.count].store(accumulator, std::memory_order_relaxed);
					// The claims PassCapture would withhold this accumulator's passes by: its batch renderer's
					// render mode's, when the renderer is a shadow view's.
					if (active)
						if (const auto* data = accumulator->GetRuntimeData())
							call.claims[call.count] = PassCapture::Get().ShadowClaimsForBatch(data->batchRenderer);
					++call.count;
				}
				currentCall = &call;
				const std::int64_t start = Now();
				func(a_light, a_count, a_arg2, a_arg3);
				const std::int64_t ticks = Now() - start;
				currentCall = nullptr;
				// The cascades as the engine culled them: what a removed entry's geometries are tested against when the
				// main camera registers them. From here until the next mask clear, those registrations take DCLF's bits.
				if (self.exclusionLive.load(std::memory_order_relaxed)) {
					auto& state = self.frameState;
					state.cascadeCount = call.count;
					state.sunBits = 0;
					for (std::uint32_t i = 0; i < call.count; ++i)
						state.sunBits |= state.cascades[i].captured ? state.cascades[i].bit : 0u;
					self.bitsReady.store(true, std::memory_order_release);
				}

				auto& stats = self.stats;
				++stats.frames;
				stats.activeFrames += active ? 1 : 0;
				stats.skipped += call.skipped;
				stats.registered += call.registered;
				stats.accumulateTicks += ticks;
				stats.accumulateMax = std::max(stats.accumulateMax, ticks);
				stats.registrationTicks += call.registrationTicks;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

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
						for (const auto& known : knownAccumulators)
							if (known.load(std::memory_order_relaxed) == a_accumulator) {
								self.offThread.fetch_add(1, std::memory_order_relaxed);
								break;
							}
					}
					if (PrimaryCull::Probe() && PrimaryCull::Get().Counting())
						PrimaryCull::Get().NoteRegistration(a_accumulator, geometry);
					if (SkylightProbeEnabled() && globals::features::skylighting.inOcclusion && At<std::uint32_t>(a_accumulator, 0x150) == 0x1C) {
						const auto result = func(a_accumulator, a_geometry, a_arg);
						NoteSkylightRegistration(geometry);
						return result;
					}
					// The main camera's registrations read the mask, then clear it (+0x160 = 0xFFFF) for the next frame.
					if (self.bitsReady.load(std::memory_order_acquire))
						self.ApplySunBits(geometry, At<std::uint32_t>(a_accumulator, kAccumulatorLightIndex) == 0xFFFF);
					else if (!call && self.exclusionLive.load(std::memory_order_acquire) && self.UnderRemovedEntry(geometry))
						self.bitStats.notReady.fetch_add(1, std::memory_order_relaxed);
					return func(a_accumulator, a_geometry, a_arg);
				}
				const bool timing = TimingEnabled();
				const std::int64_t start = timing ? Now() : 0;
				std::uint64_t result = 1;
				const auto& claims = call->claims[cascade];
				const bool claimed = claims && claims->contains(geometry);
				// A removed entry's geometry reaching a cascade: live, it means the entry was culled through another path
				// (checked every 64th cull); dry, an unclaimed one that builds a pass is a caster the exclusion would lose.
				const bool underRemoved = self.exclusionLive.load(std::memory_order_relaxed) &&
				                          (self.frameState.probe || (self.frameState.stamp & 63) == 0) && self.UnderRemovedEntry(geometry);
				if (underRemoved && !self.frameState.probe)
					++self.stats.cascadeRegistrationsUnderRemoved;
				if (claimed) {
					WriteMaskOnly(a_accumulator, a_geometry);
					++call->skipped;
				} else {
					const std::uint32_t passesBefore = PassCapture::PassesOnThisThread();
					result = func(a_accumulator, a_geometry, a_arg);
					++call->registered;
					if (underRemoved && self.frameState.probe)
						self.NoteProbeUnclaimed(geometry, PassCapture::PassesOnThisThread() - passesBefore);
				}
				if (timing)
					call->registrationTicks += Now() - start;
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
				self.bitsReady.store(false, std::memory_order_relaxed);
				self.exclusionLive.store(false, std::memory_order_relaxed);
				const std::int64_t start = Now();
				func(a_light, a_lists, a_arg);
				const std::int64_t ticks = Now() - start;
				auto& stats = self.stats;
				stats.fullFrustumTicks += ticks;
				stats.fullFrustumMax = std::max(stats.fullFrustumMax, ticks);
				self.ExcludeEntries(static_cast<RE::BSShadowDirectionalLight*>(a_light));
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
					if (call->cascade >= 0 && self.exclusionLive.load(std::memory_order_relaxed)) {
						auto& cascade = self.frameState.cascades[call->cascade];
						cascade = {};
						// Accumulate has just set it: 1 << the shadow-light count.
						cascade.bit = At<std::uint32_t>(accumulator, kAccumulatorLightBit);
					}
				}
				func(a_light, a_descriptor, a_count, a_processes, a_arg);
				if (call)
					call->cascade = -1;
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
				auto& self = SunAccumulation::Get();
				if (!call || call->cascade < 0 || !a_process || !self.exclusionLive.load(std::memory_order_relaxed))
					return;
				auto& cascade = self.frameState.cascades[call->cascade];
				if (cascade.captured || At<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>(a_fullProcess, kProcessObjectArray).empty())
					return;
				CopyPlanes(a_process->planes, cascade.planes, cascade.planeMask);
				if (a_process->doCustomCullPlanes)
					CopyPlanes(a_process->customCullPlanes, cascade.customPlanes, cascade.customMask);
				cascade.captured = true;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void SunAccumulation::Install()
	{
		if (installed || !REL::Module::IsAE())
			return;
		const auto base = REL::Module::get().base();
		const auto fullFrustum = base + kFullFrustumCallSite;
		const auto registerA = base + kRegisterCallSites[0];
		const auto registerB = base + kRegisterCallSites[1];
		const auto cascadeCall = base + kCascadeCallSite;
		const auto cascadeCull = base + kCascadeCullCallSite;
		const auto maskClear = base + kMaskClearCallSite;
		if (!CallsTo(fullFrustum, base + kFullFrustumCull) || !CallsTo(registerA, base + kRegister) || !CallsTo(registerB, base + kRegister) ||
			!CallsTo(cascadeCall, base + kCascade) || !CallsTo(cascadeCull, base + kCascadeCull) || !CallsTo(maskClear, base + kMaskClear)) {
			logger::warn("[DCLF] sun accumulation: the calls are not where expected; the engine keeps the sun's accumulation");
			return;
		}
		stl::write_vfunc<0x9, Hooks::Accumulate>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_thunk_call<Hooks::Register<0>>(registerA);
		stl::write_thunk_call<Hooks::Register<1>>(registerB);
		stl::write_thunk_call<Hooks::FullFrustum>(fullFrustum);
		stl::write_thunk_call<Hooks::Cascade>(cascadeCall);
		stl::write_thunk_call<Hooks::CascadeCull>(cascadeCull);
		stl::write_thunk_call<Hooks::MaskClear>(maskClear);
		installed = true;
		logger::info("[DCLF] sun accumulation installed (Accumulate, its cascades and registrations, the full-frustum cull)");
	}

	void SunAccumulation::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if (!installed || (a_frame % a_interval) != 0)
			return;
		if (SkylightProbeEnabled() && skylightProbe.calls) {
			auto& probe = skylightProbe;
			const double f = a_interval;
			std::string reasons;
			for (std::size_t r = 0; r < probe.byReason.size(); ++r)
				if (probe.byReason[r])
					reasons += fmt::format(" {}={:.0f}", kIneligibleNames[r], probe.byReason[r] / f);
			logger::info("[DCLF][TEMP] skylight probe, per frame: {:.0f} registrations, {:.0f} with a pass: {:.0f} table objects, {:.0f} tracked without a record ({}), {:.0f} untracked",
				probe.calls / f, probe.withPass / f, probe.byState[2] / f, probe.byState[1] / f, reasons, probe.byState[0] / f);
			std::vector<std::pair<std::uint64_t, std::string>> top;
			for (const auto& [key, count] : probe.untracked)
				top.emplace_back(count, key);
			std::sort(top.rbegin(), top.rend());
			for (std::size_t i = 0; i < top.size() && i < 25; ++i)
				logger::info("[DCLF][TEMP] skylight probe untracked: {:.1f}/frame {}", top[i].first / f, top[i].second);
			probe = {};
		}
		stats.offThread = offThread.exchange(0, std::memory_order_relaxed);
		if (stats.frames) {
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
			const double frames = stats.frames;
			std::string timing;
			if (TimingEnabled())
				timing = fmt::format("; per frame, render thread: full-frustum cull {:.3f} ms (max {:.3f}), Accumulate {:.3f} ms (max {:.3f}), of which registration {:.3f} ms",
					stats.fullFrustumTicks * toMs / frames, stats.fullFrustumMax * toMs, stats.accumulateTicks * toMs / frames, stats.accumulateMax * toMs,
					stats.registrationTicks * toMs / frames);
			logger::info("[DCLF] sun accumulation: skipping registration on {} of {} frames; {:.0f} claimed geometries a frame not registered, {:.0f} registered by the engine, {} sun registrations outside Accumulate{}",
				stats.activeFrames, stats.frames, stats.skipped / frames, stats.registered / frames, stats.offThread, timing);
			if (stats.exclusionFrames || stats.exclusionStale || stats.exclusionMissing) {
				const double applied = std::max<double>(stats.exclusionFrames, 1.0);
				const auto take = [](std::atomic<std::uint64_t>& a_value) { return a_value.exchange(0, std::memory_order_relaxed); };
				const auto written = take(bitStats.written), withBits = take(bitStats.withBits), notReady = take(bitStats.notReady);
				const auto compared = take(bitStats.compared), agree = take(bitStats.agree), engineOnly = take(bitStats.engineOnly), dclfOnly = take(bitStats.dclfOnly);
				const auto unclaimed = take(bitStats.probeUnclaimed), noPass = take(bitStats.probeNoPass);
				logger::info("[DCLF] sun entry exclusion{}: applied on {} frames ({} stale, {} with none); per frame {:.0f} of {:.0f} objectArray entries removed, "
							 "{:.0f} of {:.0f} candidates excluded, filter {:.3f} ms; {:.0f} registrations took DCLF's bits ({:.0f} with a cascade), {} before the cascades, "
							 "{} cascade registrations under a removed entry{}",
					ExclusionProbe() ? " (probe: dry)" : "", stats.exclusionFrames, stats.exclusionStale, stats.exclusionMissing,
					stats.entriesRemoved / applied, stats.entriesSeen / applied, stats.excluded / applied, stats.candidates / applied,
					stats.filterTicks * toMs / applied, written / applied, withBits / applied, notReady, stats.cascadeRegistrationsUnderRemoved,
					ExclusionProbe() ? fmt::format("; bits compared {}, agree {}, engine only {}, DCLF only {}; unclaimed cascade registrations {} with a pass, {} without",
										   compared, agree, engineOnly, dclfOnly, unclaimed, noPass) :
									   std::string());
			}
		}
		stats = {};
	}
}
