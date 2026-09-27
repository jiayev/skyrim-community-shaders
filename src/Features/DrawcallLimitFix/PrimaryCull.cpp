#include "PrimaryCull.h"

#include "LightingDescriptors.h"
#include "PassCapture.h"
#include "SceneStore.h"
#include "AsyncWorker.h"
#include "IndirectDraws.h"
#include "SunAccumulation.h"
#include "Switches.h"
#include "Toggles.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

namespace DCLF
{
	namespace
	{
		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kAfterFullFrustumCallSite = 0x14cbcea;  // CalculateAndDrawShadowCasterLights -> FUN_1414a0840, after either branch
		constexpr std::uintptr_t kAfterFullFrustum = 0x14a0840;
		constexpr std::uintptr_t kListJobsFinishCallSite = 0x14cbf4d;  // ... -> JobList::Finish(scene list culling)
		constexpr std::uintptr_t kListJobsFinish = 0xcf6810;
		constexpr std::uintptr_t kProcess1 = 0xe28390;  // BSCullingProcess::Process1, the list processes' vtable slot 0x16
		constexpr std::uintptr_t kSceneLists = 0x338c870;      // BSTArray<NiPointer<NiAVObject>>*
		constexpr std::uintptr_t kSceneListCount = 0x338c868;  // std::uint32_t
		constexpr std::uintptr_t kExtraList = 0x338c888;       // BSTArray<NiPointer<NiAVObject>>, culled by the first job only
		constexpr std::uintptr_t kMainAccumulator = 0x338c830;   // BSShaderAccumulator*, render mode 0
		constexpr std::uintptr_t kDepthAccumulator = 0x338c828;  // BSShaderAccumulator*, render mode 0xC
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;  // byte: the accumulator draws the deferred shadow mask
		constexpr std::uintptr_t kNoSunShadowDir = 0x20330a4;      // byte: GetRenderPasses gives no pass ShadowDir
		constexpr std::uintptr_t kScreenDoorFades = 0x2033468;     // byte: screen-door fades are on
		constexpr std::uintptr_t kListProcesses = 0x338c8a0;       // BSGeometryListCullingProcess**, one per scene list
		constexpr std::uintptr_t kFadesOn = 0x2032dfd;             // byte: BSFadeNode::OnVisible updates fades
		// NiAVObject / BSFadeNode fields BSFadeNode::OnVisible (AE 0x141479f50) reads before its fade update.
		constexpr std::size_t kObjectFlags = 0xF4;
		constexpr std::uint32_t kFlagFadeSettled = 1u << 15;
		constexpr std::uint32_t kFlagAccumulated = 1u << 26;  // NiAVObject::kAccumulated
		constexpr std::size_t kFadeAmount = 0x100;
		constexpr std::size_t kCurrentFade = 0x130;
		constexpr std::size_t kPropertyLightData = 0x70;
		constexpr std::size_t kLastVisibleFrame = 0x13C;
		constexpr std::uint32_t kFlagFadeTargetReached = 1u << 14;
		constexpr std::size_t kCameraLodAdjust = 0x184;
		constexpr std::uintptr_t kFadeFrameCounter = 0x2032e50;   // std::int32_t: the fade code's frame counter
		constexpr std::uintptr_t kFadeLodUpdates = 0x2032dfc;     // byte
		constexpr std::uintptr_t kFadeStepTime = 0x2033084;       // float: the frame time
		constexpr std::uintptr_t kFadeStepDivisor = 0x2032e2c;    // float
		constexpr std::uintptr_t kFadeStepMax = 0x2032e4c;        // float
		constexpr std::uintptr_t kFadeSpecialA = 0x332a254;       // float: with kFadeSpecialB, OnVisible's LOD-type-6 branch
		constexpr std::uintptr_t kFadeSpecialB = 0x1769578;       // float
		constexpr std::uintptr_t kFadeUpdate = 0x147a160;         // FUN_14147a160(node, fadeAmount, camera): the fade state machine
		constexpr std::uintptr_t kFadeDistance = 0x147b110;       // FUN_14147b110(node, camera, out): the distance and LOD level
		constexpr std::uintptr_t kLeafLodUpdate = 0x147a430;      // FUN_14147a430(node, level): the leaf and tree LOD transition
		// BSTreeNode::OnVisible (AE 0x14147d3c0): the height test and the LOD fix-up.
		constexpr std::size_t kProcessTreeHeightTest = 0x121;     // byte on the culling process
		constexpr std::uintptr_t kTreeHeightTestOn = 0x2032fb8;   // byte
		constexpr std::uintptr_t kTreeHeightBase = 0x332a2f0;     // float
		constexpr std::uintptr_t kTreeHeightLimit = 0x2032fa0;    // float
		constexpr std::size_t kTreeLodData = 0x180;               // pointer; its +0x12C count
		constexpr std::size_t kLightDataActiveMask = 0x1C;
		constexpr std::size_t kJoinsPerFrame = 256;  // resident joins a frame, of each kind (admitted, probation)
		// FUN_14147b110 (the fade value) and FUN_14147a160 (the fade update): what the fade-out distance is made of.
		constexpr std::size_t kFadeNear = 0x128;                  // float, on the fade node
		constexpr std::size_t kFadeFar = 0x12C;                   // float
		constexpr std::size_t kFadeAlwaysOut = 0x109;             // byte: bit 0 fades the node out whatever its distance
		constexpr std::uintptr_t kFadeTypeDivisors = 0x2032e00;   // float per LOD type (+0x153 & 0xF): the LOD factor's divisor
		constexpr std::uintptr_t kFadeDefaultScale = 0x1ad2840;   // float: the scale when the type's divisor is not positive
		constexpr std::uintptr_t kFadeOutThreshold = 0x2032e38;   // float: a faded-in node fades out below this fade value
		constexpr std::uintptr_t kFadeDistanceMult = 0x2032e48;   // float: multiplies the node's near and far distances

		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

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
		T& Global(std::uintptr_t a_offset)
		{
			return *reinterpret_cast<T*>(REL::Module::get().base() + a_offset);
		}

		template <class T>
		T& At(const void* a_base, std::size_t a_offset)
		{
			return *reinterpret_cast<T*>(const_cast<std::byte*>(static_cast<const std::byte*>(a_base)) + a_offset);
		}

		std::int64_t Now()
		{
			LARGE_INTEGER value{};
			QueryPerformanceCounter(&value);
			return value.QuadPart;
		}

		bool RttiIs(const RE::NiAVObject* a_object, const char* a_name)
		{
			const auto* rtti = a_object->GetRTTI();
			return rtti && rtti->name && std::strcmp(rtti->name, a_name) == 0;
		}

		/** @brief BSFadeNode::OnVisible's early out: nothing but the recursion when fades are off or the node is settled. */
		bool FadeSettled(const RE::NiAVObject* a_node)
		{
			if (!Global<std::uint8_t>(kFadesOn))
				return true;
			return (At<std::uint32_t>(a_node, kObjectFlags) & kFlagFadeSettled) && At<float>(a_node, kFadeAmount) == 1.0f &&
			       At<float>(a_node, kCurrentFade) == 1.0f;
		}

		/**
		 * @brief A fade root the synthetic pass can stand for: fully faded in, and its LOD state settled (+0x153 & 0x70 is
		 * 0x20; otherwise GetRenderPasses adds the old level's cross-fade copy, a hint-10 pass, which is the engine's).
		 */
		bool Settled(const RE::NiAVObject* a_node)
		{
			return At<float>(a_node, kCurrentFade) == 1.0f && At<float>(a_node, kFadeAmount) == 1.0f && (At<std::uint8_t>(a_node, 0x153) & 0x70) == 0x20;
		}

		/** @brief The engine's sphere test against the active planes (outside when n.c - d < -r). */
		bool Outside(const RE::NiFrustumPlanes& a_planes, const RE::NiBound& a_bound)
		{
			const std::uint32_t mask = a_planes.activePlanes.underlying() & 0x3Fu;
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(mask & (1u << p)))
					continue;
				const auto& plane = a_planes.cullingPlanes[p];
				if (plane.normal.Dot(a_bound.center) - plane.constant < -a_bound.radius)
					return true;
			}
			return false;
		}

		/** @brief The engine's Process1 (the slot's original, verified at install). */
		void EngineProcess1(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
		{
			using Fn = void (*)(RE::NiCullingProcess*, RE::NiAVObject*, std::int32_t);
			reinterpret_cast<Fn>(REL::Module::get().base() + kProcess1)(a_process, a_object, a_arg);
		}

		std::string NameOf(const RE::NiAVObject* a_object)
		{
			if (!a_object)
				return "null";
			const char* name = a_object->name.c_str();
			return fmt::format("{}:{}", a_object->GetRTTI() ? a_object->GetRTTI()->name : "?", name && *name ? name : "-");
		}
	}

	PrimaryCull& PrimaryCull::Get()
	{
		static PrimaryCull instance;
		return instance;
	}

	bool PrimaryCull::ReasonsProbe()
	{
		static const bool probe = SwitchEnabled("CS_DCLF_PRIMARY_REASONS");
		return probe;
	}

	bool PrimaryCull::Probe()
	{
		static const bool probe = SwitchValue("CS_DCLF_PRIMARY_EXCLUDE") == "probe";
		return probe;
	}

	bool PrimaryCull::ServiceFade(RE::NiAVObject* a_node, bool a_leaf, const RE::NiCamera& a_camera)
	{
		using FadeUpdate = void (*)(RE::NiAVObject*, float, const float*);
		// FUN_14147b110 writes the LOD distance through its third argument, and FUN_14147a430 takes it in XMM1
		// (BSLeafAnimNode::OnVisible, AE 0x14147ca3e): floats, not integers.
		using FadeDistance = float (*)(RE::NiAVObject*, const float*, float*);
		using LeafLodUpdate = void (*)(RE::NiAVObject*, float);
		const auto base = REL::Module::get().base();
		const float camera[4] = { a_camera.world.translate.x, a_camera.world.translate.y, a_camera.world.translate.z, At<float>(&a_camera, kCameraLodAdjust) };
		auto& lastVisible = At<std::int32_t>(a_node, kLastVisibleFrame);
		const std::int32_t counter = Global<std::int32_t>(kFadeFrameCounter);
		// BSLeafAnimNode::OnVisible: its LOD step first, then BSFadeNode::OnVisible.
		if (a_leaf && Global<std::uint8_t>(kFadeLodUpdates)) {
			float level = 0.0f;
			reinterpret_cast<FadeDistance>(base + kFadeDistance)(a_node, camera, &level);
			reinterpret_cast<LeafLodUpdate>(base + kLeafLodUpdate)(a_node, level);
			lastVisible = counter;
		}
		if (!Global<std::uint8_t>(kFadesOn) || FadeSettled(a_node))
			return true;
		auto& flags = At<std::uint32_t>(a_node, kObjectFlags);
		auto& currentFade = At<float>(a_node, kCurrentFade);
		const float fadeAmount = At<float>(a_node, kFadeAmount);
		if ((At<std::uint8_t>(a_node, 0x153) & 0xF) == 6 && Global<float>(kFadeSpecialA) == Global<float>(kFadeSpecialB)) {
			float level = 0.0f;
			reinterpret_cast<FadeDistance>(base + kFadeDistance)(a_node, camera, &level);
			std::uint32_t value = flags;
			if (!Global<std::uint8_t>(kFadeLodUpdates)) {
				value |= kFlagFadeTargetReached;
				currentFade = 1.0f;
				flags = value;
			}
			if (!(value & kFlagFadeTargetReached)) {
				bool reached = true;
				if (counter - lastVisible <= 1) {
					const float step = std::min(Global<float>(kFadeStepTime) / Global<float>(kFadeStepDivisor), Global<float>(kFadeStepMax));
					currentFade = std::min(step + currentFade, 1.0f);
					reached = !(currentFade < 1.0f);
				} else {
					currentFade = 1.0f;
				}
				if (reached)
					flags = value | kFlagFadeTargetReached;
			}
			lastVisible = counter;
			return true;
		}
		reinterpret_cast<FadeUpdate>(base + kFadeUpdate)(a_node, fadeAmount, camera);
		lastVisible = counter;
		return currentFade > 0.0f && fadeAmount != 0.0f;
	}

	std::int32_t PrimaryCull::SwitchIndex(const RE::NiSwitchNode* a_switch)
	{
		SceneStore::SwitchState state;
		return SceneStore::ReadSwitch(*a_switch, state) ? state.index : -1;
	}

	bool PrimaryCull::ResidentOn()
	{
		static const bool on = SwitchValue("CS_DCLF_RESIDENT") != "0" && FeedbackOn() && SceneStore::SwitchEventsLive();
		return on;
	}

	bool PrimaryCull::ProbationOn()
	{
		static const bool on = ResidentOn() && SwitchValue("CS_DCLF_RESIDENT_PROBATION") != "0";
		return on;
	}

	bool PrimaryCull::FreshSyntheticPass(const RE::BSGeometry& a_geometry, AccumulatedPass& a_out)
	{
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
		if (!lighting)
			return false;
		LightingDescriptors descriptors;
		if (DeriveLightingDescriptors(*lighting, a_geometry, nullptr, descriptors, true) != Ineligible::None)
			return false;
		if (!SyntheticPass(a_geometry, descriptors.derivedPass, a_out, SunOnGpu()))
			return false;
		a_out.resident = true;
		return true;
	}

	PrimaryCull::Refusal PrimaryCull::ResidentRefusal(std::uint32_t a_e, bool a_probation) const
	{
		if (a_e >= cut.plans.size() || (!a_probation && !cut.admitted[a_e]) || cut.plans[a_e] == EntryPlan::Rejected)
			return Refusal::Plan;
		const auto* root = cut.roots[a_e];
		if (cut.plans[a_e] != EntryPlan::Plain && (!Settled(root) || (At<std::uint8_t>(root, kFadeAlwaysOut) & 1)))
			return Refusal::NotSettled;
		auto& store = SceneStore::Get();
		bool any = false;
		for (std::uint32_t m = cut.memberOffsets[a_e]; m < cut.memberOffsets[a_e + 1]; ++m) {
			const auto& member = cut.members[m];
			// A member no switch selects is not drawn, and a switch that changes evicts the entry (PrepareFrame).
			if (!(cut.liveEvents ? cut.memberLive[m] != 0 : PathSelected(member)))
				continue;
			// The engine's members keep the entry in the stand-in (phase 5).
			if (member.engine)
				return Refusal::EngineMember;
			// Nothing app-culled up to the root: a resident record is drawn whenever the GPU finds it.
			if (!MemberShown(m, root))
				return Refusal::HiddenMember;
			if (!store.ResidentCapable(member.geometry)) {
				// [TEMP] why, by plan and cause.
				if (a_probation || true) {
					static std::atomic<std::uint32_t> logged{ 0 };
					static std::map<std::string, std::uint32_t> seen;
					const auto key = fmt::format("{} plan, {}", static_cast<int>(cut.plans[a_e]), store.ResidentIncapableReason(member.geometry));
					if (seen[key]++ == 0 && logged.fetch_add(1) < 24)
						logger::info("[DCLF][TEMP] resident refusal: '{}' under '{}' ({}): {}", member.geometry->name.c_str() ? member.geometry->name.c_str() : "?",
							root->name.c_str() ? root->name.c_str() : "?", root->GetRTTI() ? root->GetRTTI()->name : "?", key);
				}
				return Refusal::MemberNotCapable;
			}
			any = true;
		}
		return any ? Refusal::None : Refusal::Plan;
	}

	float PrimaryCull::FadeDistanceOf(const RE::NiAVObject* a_root)
	{
		// No fade update at all, or no distance term in the fade value (FUN_14147b110 returns 1).
		if (!Global<std::uint8_t>(kFadesOn) || !Global<std::uint8_t>(kFadeLodUpdates))
			return 0.0f;
		const std::uint32_t type = At<std::uint8_t>(a_root, 0x153) & 0xF;
		// Type 8 fades gradually even from out of view (FUN_14147a160), and OnVisible's type-6 branch never fades out
		// (ServiceFade): the engine draws either on the frame it comes into view, as the resident is.
		if (type == 8 || (type == 6 && Global<float>(kFadeSpecialA) == Global<float>(kFadeSpecialB)))
			return 0.0f;
		const float mult = Global<float>(kFadeDistanceMult);
		const float nearDistance = mult * At<float>(a_root, kFadeNear);
		const float farDistance = mult * At<float>(a_root, kFadeFar);
		if (!(farDistance > nearDistance))
			return 0.0f;  // the fade value never falls below 1
		// The fade value 1 - (x - near) / (far - near), x the scaled distance, reaches the threshold at x = limit.
		const float limit = nearDistance + (1.0f - Global<float>(kFadeOutThreshold)) * (farDistance - nearDistance);
		const float divisor = Global<float>(kFadeTypeDivisors + type * 4);
		// x = distance * lodFactor / divisor, or distance * the default scale when the divisor is not positive.
		const float distance = divisor > 0.0f ? limit * divisor : -(limit / Global<float>(kFadeDefaultScale));
		return std::isfinite(distance) ? distance : 0.0f;
	}

	std::uint16_t PrimaryCull::FadeWitnessOf(const RE::NiAVObject* a_root, bool a_sensitive)
	{
		const std::uint32_t level = At<std::uint8_t>(a_root, 0x152) & 0xF;
		const std::uint32_t metric = a_sensitive ? LodFadeStateOf(static_cast<const RE::BSFadeNode*>(a_root)) : 0u;
		return static_cast<std::uint16_t>(level | (metric << 8));
	}

	std::uint32_t PrimaryCull::ResidentWitness()
	{
		const auto* accumulator = Global<std::uint8_t*>(kMainAccumulator);
		std::uint32_t witness = 2166136261u;
		const auto mix = [&](std::uint32_t a_value) { witness = (witness ^ a_value) * 16777619u; };
		// The static sun bits (SunShadowStatic).
		mix(Global<std::uint8_t>(kNoSunShadowDir) | (accumulator && accumulator[kAccumulatorDeferredShadow] ? 2u : 0u) | (Global<std::uint8_t>(kScreenDoorFades) ? 4u : 0u));
		// The fade distances (FadeDistanceOf).
		mix(Global<std::uint8_t>(kFadesOn) | (Global<std::uint8_t>(kFadeLodUpdates) << 8) | (Global<float>(kFadeSpecialA) == Global<float>(kFadeSpecialB) ? 0x10000u : 0u));
		mix(std::bit_cast<std::uint32_t>(Global<float>(kFadeDistanceMult)));
		mix(std::bit_cast<std::uint32_t>(Global<float>(kFadeOutThreshold)));
		mix(std::bit_cast<std::uint32_t>(Global<float>(kFadeDefaultScale)));
		for (std::uint32_t type = 0; type < 13; ++type)
			mix(std::bit_cast<std::uint32_t>(Global<float>(kFadeTypeDivisors + type * 4)));
		return witness;
	}

	void PrimaryCull::EvictResident(const RE::NiAVObject* a_root, Eviction a_cause)
	{
		const auto it = residents.find(a_root);
		if (it == residents.end())
			return;
		auto& store = SceneStore::Get();
		for (const auto* geometry : it->second.members) {
			store.EndResidency(geometry);
			residentMemberRoot.erase(geometry);
		}
		// An entry that keeps leaving tries again later.
		const std::uint64_t until = frameCounter + (a_cause == Eviction::Unsettled ? 30 : 120);
		joinBackoff[a_root] = until;
		if (it->second.entry < joinBlocked.size() && cut.roots[it->second.entry] == a_root)
			joinBlocked[it->second.entry] = until;
		residents.erase(it);
		++cutStats.evicted[static_cast<std::size_t>(a_cause)];
	}

	void PrimaryCull::EndAllResidents()
	{
		residents.clear();
		residentMemberRoot.clear();
		SceneStore::Get().EndAllResidency();
		++cutStats.endedAll;
	}

	bool PrimaryCull::ResidentPassOf(const RE::BSGeometry* a_geometry, AccumulatedPass& a_out)
	{
		const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		if (!lighting)
			return false;
		auto& cached = derivedCache[a_geometry];
		const std::uint8_t fadeState = FadeStateOf(property);
		if (cached.property != property || cached.material != lighting->material || cached.flags != lighting->flags.underlying() || cached.fadeState != fadeState) {
			LightingDescriptors descriptors;
			const auto reason = DeriveLightingDescriptors(*lighting, *a_geometry, nullptr, descriptors, true);
			cached = { property, lighting->material, lighting->flags.underlying(), fadeState, reason == Ineligible::None ? descriptors.derivedPass : kNotDerived,
					descriptors.projectedUV || descriptors.technique == 8 || descriptors.technique == 19 };
		}
		if (cached.extras || !SyntheticPass(*a_geometry, cached.derivedPass, a_out, SunOnGpu()))
			return false;
		a_out.resident = true;
		return true;
	}

	void PrimaryCull::UpdateResidents(bool a_current)
	{
		auto& store = SceneStore::Get();
		// The events that end residency: records the walk rewrote or released, patches that failed (last frame's
		// accumulate phase), roots something was attached under or detached from, roots the feedback found fading.
		store.TakeResidentEvictions(evictedGeometries, evictedRoots);
		for (const auto* geometry : evictedGeometries)
			if (const auto it = residentMemberRoot.find(geometry); it != residentMemberRoot.end())
				EvictResident(it->second, Eviction::Record);
		for (const auto* root : evictedRoots)
			EvictResident(root, Eviction::Members);
		for (const auto* root : std::exchange(unsettledRoots, {}))
			EvictResident(root, Eviction::Unsettled);
		// Last frame's probation joins the build did not draw in full: before this frame's list jobs could return at once
		// for them.
		for (const auto* root : std::exchange(probationRoots, {}))
			if (const auto it = residents.find(root); it != residents.end() && it->second.probation)
				EvictResident(root, Eviction::NotReady);
		residentPasses.clear();
		if (!a_current || !ResidentOn())
			return;
		// Joins: entries the stand-in reached admitted and settled, a bounded number a frame (each one's objects are
		// patched in this frame's accumulate phase).
		std::size_t taken = 0, joined = 0;
		for (; taken < joinQueue.size() && joined < kJoinsPerFrame; ++taken) {
			const std::uint32_t e = joinQueue[taken];
			joinQueued[e] = 0;
			joined += JoinEntry(e, false) ? 1 : 0;
		}
		joinQueue.erase(joinQueue.begin(), joinQueue.begin() + static_cast<std::ptrdiff_t>(taken));
		cutStats.joins += joined;
	}

	bool PrimaryCull::JoinEntry(std::uint32_t a_e, bool a_probation)
	{
		const auto* root = cut.roots[a_e];
		if (residents.contains(root))
			return false;
		if (const auto backoff = joinBackoff.find(root); backoff != joinBackoff.end()) {
			if (backoff->second > frameCounter)
				return false;
			joinBackoff.erase(backoff);
		}
		const auto refuse = [&](Refusal a_cause, std::uint64_t a_frames) {
			// Its plan, members, fade or passes do not allow it now: it is not asked again for a while.
			joinBackoff[root] = frameCounter + a_frames;
			joinBlocked[a_e] = frameCounter + a_frames;
			++(a_probation ? cutStats.probationRefused : cutStats.joinRefused);
			++cutStats.refusedBy[static_cast<std::size_t>(a_cause)];
			return false;
		};
		if (const auto refusal = ResidentRefusal(a_e, a_probation); refusal != Refusal::None)
			return refuse(refusal, 120);
		const auto selected = [&](std::uint32_t a_m) {
			return !cut.members[a_m].engine && (cut.liveEvents ? cut.memberLive[a_m] != 0 : PathSelected(cut.members[a_m]));
		};
		// A fade root's objects carry its fade-out distance for BuildDraws' test. Decals are never in the depth segment,
		// whose first phase tests it, so an entry that needs the test and has one stays in the stand-in.
		const float fadeDistance = cut.plans[a_e] == EntryPlan::Plain ? 0.0f : FadeDistanceOf(root);
		// [TEMP] the fade distance's inputs, for the first joins that have one.
		static std::uint32_t loggedFade = 0;
		if (fadeDistance != 0.0f && loggedFade < 12) {
			++loggedFade;
			const std::uint32_t type = At<std::uint8_t>(root, 0x153) & 0xF;
			const float dx = root->worldBound.center.x - fadeEye[0], dy = root->worldBound.center.y - fadeEye[1], dz = root->worldBound.center.z - fadeEye[2];
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			logger::info("[DCLF][TEMP] fade distance: '{}' type {} near {} far {} mult {} threshold {} divisor {} -> {}; now at {} (x LOD factor {} = {}), metric {}",
				root->name.c_str() ? root->name.c_str() : "?", type, At<float>(root, kFadeNear), At<float>(root, kFadeFar), Global<float>(kFadeDistanceMult),
				Global<float>(kFadeOutThreshold), Global<float>(kFadeTypeDivisors + type * 4), fadeDistance, distance, fadeEye[3], distance * fadeEye[3],
				std::bit_cast<float>(At<std::uint32_t>(root, 0x144)));
		}
		// Every member's pass first: an entry with a member the synthetic pass cannot model stays in the stand-in.
		const std::size_t firstPass = residentPasses.size();
		const bool tree = cut.plans[a_e] == EntryPlan::TreeRoot;
		for (std::uint32_t m = cut.memberOffsets[a_e]; m < cut.memberOffsets[a_e + 1]; ++m)
			if (selected(m)) {
				AccumulatedPass pass;
				const bool built = ResidentPassOf(cut.members[m].geometry, pass);
				const bool decal = pass.hint == 2 || pass.hint == 3;
				// Decals are never in the depth segment, whose first phase makes the fade and height tests.
				if (!built || (decal && (fadeDistance != 0.0f || tree))) {
					residentPasses.resize(firstPass);
					return refuse(built ? Refusal::FadeDecal : Refusal::PassNotBuilt, 600);
				}
				pass.fadeDistance = fadeDistance;
				pass.heightTest = tree;
				residentPasses.emplace_back(cut.members[m].geometry, pass);
			}
		Resident resident{ a_e, {}, RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(root)), a_probation };
		for (std::uint32_t m = cut.memberOffsets[a_e]; m < cut.memberOffsets[a_e + 1]; ++m)
			if (selected(m)) {
				const auto* geometry = cut.members[m].geometry;
				resident.members.push_back(geometry);
				residentMemberRoot[geometry] = root;
				const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				resident.fadeSensitive = resident.fadeSensitive || (FadeSensitive(property) && property->fadeNode == root);
			}
		if (cut.plans[a_e] != EntryPlan::Plain)
			resident.fadeWitness = FadeWitnessOf(root, resident.fadeSensitive);
		residents.emplace(root, std::move(resident));
		if (a_probation) {
			probationRoots.push_back(root);
			++cutStats.probationJoins;
		}
		return true;
	}

	bool PrimaryCull::SwitchProbe()
	{
		static const bool probe = SwitchEnabled("CS_DCLF_SWITCH_PROBE");
		return probe;
	}

	void PrimaryCull::RefreshLive(std::uint32_t a_e)
	{
		for (std::uint32_t m = cut.memberOffsets[a_e]; m < cut.memberOffsets[a_e + 1]; ++m)
			cut.memberLive[m] = PathSelected(cut.members[m]) ? 1 : 0;
	}

	bool PrimaryCull::FeedbackProbe()
	{
		static const bool probe = SwitchEnabled("CS_DCLF_FEEDBACK_PROBE");
		return probe;
	}

	bool PrimaryCull::FeedbackOn()
	{
		static const bool on = SwitchValue("CS_DCLF_FEEDBACK") != "0";
		return on;
	}

	bool PrimaryCull::TreeAboveLimit(const RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process)
	{
		// Above the height limit (worldBound.center.z against a base), with the test on: nothing, and no recursion.
		return At<std::uint8_t>(&a_process, kProcessTreeHeightTest) && Global<std::uint8_t>(kTreeHeightTestOn) &&
		       a_node->worldBound.center.z - Global<float>(kTreeHeightBase) > Global<float>(kTreeHeightLimit);
	}

	bool PrimaryCull::ServiceTree(RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process)
	{
		if (TreeAboveLimit(a_node, a_process))
			return false;
		return a_process.camera ? ServiceTreeState(a_node, *a_process.camera) : true;
	}

	bool PrimaryCull::ServiceTreeState(RE::NiAVObject* a_node, const RE::NiCamera& a_camera)
	{
		const bool recurse = ServiceFade(a_node, true, a_camera);
		if (const auto* lod = At<const std::byte*>(a_node, kTreeLodData); lod && At<std::int32_t>(lod, 0x12C) > 0) {
			auto& state = At<std::uint8_t>(a_node, 0x153);
			state = static_cast<std::uint8_t>((state & 0xaf) | 0x20);
			At<float>(a_node, 0x14C) = 0.0f;
		}
		return recurse;
	}

	PrimaryCull::EntryPlan PrimaryCull::PlanOf([[maybe_unused]] std::uint32_t a_entry, const RE::NiAVObject* a_root)
	{
		const auto& candidates = *cut.candidates;
		lastCause.clear();
		// Only nodes whose OnVisible is the plain recursion (NiNode, and a multibound's frustum cache), under a root that
		// may also be a fade node; only geometries whose OnVisible is BSGeometry's (the append to the process). A tracked
		// geometry PrimaryEntryAllows is DCLF's; every other is the engine's, handed to its registration in the cull's order.
		const std::size_t first = cut.members.size();
		const std::size_t firstPath = cut.switchPaths.size();
		const std::size_t firstSwitch = cut.switches.size();
		std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> path;
		bool rejected = false;
		std::uint32_t ours = 0;
		const auto visit = [&](const auto& a_self, const RE::NiAVObject* a_object, bool a_isRoot) -> void {
			if (rejected || !a_object)
				return;
			if (const auto* geometry = const_cast<RE::NiAVObject*>(a_object)->AsGeometry()) {
				if ((*reinterpret_cast<const std::uintptr_t* const*>(geometry))[0x34] != geometryOnVisible) {
					rejected = true;
					lastCause = fmt::format("geometry {} with its own OnVisible", a_object->GetRTTI() ? a_object->GetRTTI()->name : "?");
					return;
				}
				const auto it = candidates.geometries.find(geometry);
				const bool mine = it != candidates.geometries.end() && it->second < candidates.primaryGeometry.size() && candidates.primaryGeometry[it->second];
				const auto begin = static_cast<std::uint32_t>(cut.switchPaths.size());
				cut.switchPaths.insert(cut.switchPaths.end(), path.begin(), path.end());
				cut.members.push_back({ geometry, !mine, begin, static_cast<std::uint32_t>(cut.switchPaths.size()) });
				ours += mine ? 1 : 0;
				return;
			}
			auto* node = const_cast<RE::NiAVObject*>(a_object)->AsNode();
			if (node && !a_isRoot && RttiIs(a_object, "NiSwitchNode")) {
				const auto* switchNode = static_cast<const RE::NiSwitchNode*>(node);
				cut.switches.push_back(switchNode);
				const auto& children = node->GetChildren();
				for (std::uint16_t c = 0; c < children.size(); ++c) {
					path.emplace_back(switchNode, static_cast<std::int32_t>(c));
					a_self(a_self, children[c].get(), false);
					path.pop_back();
				}
				return;
			}
			if (!node || !(RttiIs(a_object, "NiNode") || RttiIs(a_object, "BSMultiBoundNode") ||
							  (a_isRoot && (RttiIs(a_object, "BSFadeNode") || RttiIs(a_object, "BSLeafAnimNode") || RttiIs(a_object, "BSTreeNode"))))) {
				rejected = true;
				lastCause = fmt::format("{} node {}", a_isRoot ? "root" : "inner", a_object->GetRTTI() ? a_object->GetRTTI()->name : "?");
				return;
			}
			for (const auto& child : node->GetChildren())
				a_self(a_self, child.get(), false);
		};
		visit(visit, a_root, true);
		if (!rejected && !ours) {
			rejected = true;
			lastCause = "nothing DCLF draws";
		}
		if (rejected) {
			cut.members.resize(first);
			cut.switchPaths.resize(firstPath);
			cut.switches.resize(firstSwitch);
			++cutStats.planReasons[lastCause];
			return EntryPlan::Rejected;
		}
		return RttiIs(a_root, "BSFadeNode") ? EntryPlan::FadeRoot :
		       RttiIs(a_root, "BSLeafAnimNode") ? EntryPlan::LeafRoot :
		       RttiIs(a_root, "BSTreeNode") ? EntryPlan::TreeRoot : EntryPlan::Plain;
	}

	void PrimaryCull::PrepareFrame()
	{
		++cutStats.frames;
		// The switches whose selection the walks applied since the last frame; a frame the cut skips drops them, so the
		// next one reads every switch again.
		const bool switchResync = SceneStore::Get().TakeSwitchChanges(switchChanges);
		liveStale = switchResync || liveStale;
		// A resident's members are the ones its switches selected at the join: a switch under its root that changed ends it
		// (the newly selected child's record is not a resident one), and a resync checks every resident's selection.
		if (!residents.empty()) {
			for (const auto* node : switchChanges)
				if (const auto it = cut.switchEntry.find(node); it != cut.switchEntry.end() && it->second < cut.roots.size())
					EvictResident(cut.roots[it->second], Eviction::Switch);
			if (switchResync) {
				std::vector<const RE::NiAVObject*> changed;
				for (const auto& [root, resident] : residents)
					if (resident.entry >= cut.roots.size() || cut.roots[resident.entry] != root || !SelectionSame(resident))
						changed.push_back(root);
				for (const auto* root : changed)
					EvictResident(root, Eviction::Switch);
			}
		}
		auto candidates = SceneStore::Get().GetSunCandidates();
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto** processes = Global<RE::NiCullingProcess**>(kListProcesses);
		// A synthetic pass has no point-light shadow: any local light that cast shadows last frame keeps everything in.
		bool localShadows = false;
		if (auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr)
			for (const auto& light : node->GetRuntimeData().activeShadowLights)
				localShadows = localShadows || (light && light.get() != node->GetRuntimeData().sunShadowDirLight);
		++frameCounter;
		// [TEMP] CS_DCLF_TEST_FADE_DIVISOR="<frame>:<value>": the LOD type 1 (objects) fade divisor written at that frame, so
		// resident objects fall past their fade-out distance within the loaded cells (the GPU's fade test).
		static const auto testDivisor = [] {
			const auto value = SwitchValue("CS_DCLF_TEST_FADE_DIVISOR");
			const auto colon = value.find(':');
			return colon == std::string::npos ? std::pair{ 0ull, 0.0f } :
			                                    std::pair{ std::strtoull(value.c_str(), nullptr, 10), std::strtof(value.c_str() + colon + 1, nullptr) };
		}();
		if (testDivisor.first && frameCounter == testDivisor.first) {
			logger::info("[DCLF][TEMP] test: the type 1 fade divisor {} -> {}", Global<float>(kFadeTypeDivisors + 4), testDivisor.second);
			Global<float>(kFadeTypeDivisors + 4) = testDivisor.second;
		}
		standInLive = false;
		residentsLive = false;
		// The camera the fade roots' distances are measured from (BuildDraws' fade test).
		fadeEye = {};
		if (const auto* camera = processes && count ? processes[0]->camera : nullptr)
			fadeEye = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z, At<float>(camera, kCameraLodAdjust) };
		// BSTreeNode::OnVisible's height test, as the list processes have it this frame (TreeAboveLimit).
		const bool heightTest = processes && count && processes[0] && At<std::uint8_t>(processes[0], kProcessTreeHeightTest) && Global<std::uint8_t>(kTreeHeightTestOn);
		treeHeight = { Global<float>(kTreeHeightBase), heightTest ? Global<float>(kTreeHeightLimit) : std::numeric_limits<float>::infinity() };
		residentPasses.clear();  // only this frame's joins, and none on a frame that keeps no residents
		if (!processes || !count || count > cut.processes.size() || !SunAccumulation::Get().ExclusionLive() || localShadows) {
			++cutStats.skippedPreconditions;
			liveStale = true;
			// A frame the engine culls everything: no resident may stay drawn from its kept record.
			if (!residents.empty())
				EndAllResidents();
			return;
		}
		// The frame globals the static sun bits and the fade distances read: the residents' passes carry them.
		if (ResidentOn()) {
			const std::uint32_t witness = ResidentWitness();
			if (witness != sunWitness && !residents.empty())
				EndAllResidents();
			sunWitness = witness;
		}
		const bool current = candidates && candidates->generation == SceneStore::Get().GetSunCandidatesGeneration();
		if (!current) {
			// A stale snapshot: the residents do not use it and stay; the engine culls everything else this frame.
			++cutStats.skippedStale;
			liveStale = true;
			UpdateResidents(false);
			if (residents.empty())
				return;
			cut.processCount = count;
			for (std::uint32_t i = 0; i < count; ++i) {
				cut.processes[i] = processes[i];
				auto& out = jobOut[i];
				out = JobOut{ std::move(out.visible), std::move(out.pending) };
				out.visible.clear();
				out.pending.clear();
			}
			residentsLive = true;
			cutStats.residentFrames += 1;
			cutStats.residentEntries += residents.size();
			frameLive.store(true, std::memory_order_release);
			return;
		}
		const std::int64_t start = Now();
		const bool newSnapshot = cut.candidates != candidates;
		if (newSnapshot) {
			// A new snapshot: each entry's geometries, the plans and the eligible roots, again; admission by node.
			cut.candidates = candidates;
			const std::uint32_t entries = static_cast<std::uint32_t>(candidates->entries.size());
			cut.geometryOffsets.assign(entries + 2, 0);
			for (const auto entry : candidates->geometryEntry)
				++cut.geometryOffsets[entry + 2];
			for (std::size_t e = 2; e < cut.geometryOffsets.size(); ++e)
				cut.geometryOffsets[e] += cut.geometryOffsets[e - 1];
			cut.geometryIndices.assign(candidates->geometryEntry.size(), nullptr);
			for (const auto& [geometry, index] : candidates->geometries)
				cut.geometryIndices[cut.geometryOffsets[candidates->geometryEntry[index] + 1]++] = geometry;
			cut.geometryOffsets.pop_back();
			cut.roots.assign(entries, nullptr);
			for (const auto& [root, index] : candidates->entries)
				cut.roots[index] = root;
			cut.plans.assign(entries, EntryPlan::Rejected);
			cut.memberOffsets.assign(entries + 1, 0);
			cut.members.clear();
			cut.switchPaths.clear();
			cut.switchOffsets.assign(entries + 1, 0);
			cut.switches.clear();
			cut.admitted.assign(entries, 0);
			cut.eligible.clear();
			cut.rejected.clear();
			cut.causes.clear();
			cut.causeOf.assign(entries, 0);
			ankerl::unordered_dense::set<const RE::NiAVObject*> admittedRoots;
			for (std::uint32_t e = 0; e < entries; ++e) {
				cut.memberOffsets[e] = static_cast<std::uint32_t>(cut.members.size());
				cut.switchOffsets[e] = static_cast<std::uint32_t>(cut.switches.size());
				cut.plans[e] = PlanOf(e, cut.roots[e]);
				cut.memberOffsets[e + 1] = static_cast<std::uint32_t>(cut.members.size());
				cut.switchOffsets[e + 1] = static_cast<std::uint32_t>(cut.switches.size());
				if (cut.plans[e] == EntryPlan::Rejected) {
					if (ReasonsProbe()) {
						auto found = std::find(cut.causes.begin(), cut.causes.end(), lastCause);
						if (found == cut.causes.end() && cut.causes.size() < 63)
							found = cut.causes.insert(cut.causes.end(), lastCause);
						cut.causeOf[e] = static_cast<std::uint16_t>(found == cut.causes.end() ? 63 : found - cut.causes.begin());
						cut.rejected.emplace(cut.roots[e], e);
					}
					continue;
				}
				cut.eligible.emplace(cut.roots[e], e);
				if (cut.admittedRoots.contains(cut.roots[e])) {
					cut.admitted[e] = 1;
					admittedRoots.insert(cut.roots[e]);
				}
			}
			cut.admittedRoots = std::move(admittedRoots);
			cut.pendingAdmission.clear();
			// The members' object indices, for reading the feedback (stable while an object stays tracked; a change of
			// membership is a new snapshot).
			cut.memberObject.resize(cut.members.size());
			for (std::size_t m = 0; m < cut.members.size(); ++m)
				cut.memberObject[m] = cut.members[m].engine ? -1 : SceneStore::Get().FindObject(cut.members[m].geometry);
			cut.switchEntry.clear();
			for (std::uint32_t e = 0; e < entries; ++e)
				for (std::uint32_t w = cut.switchOffsets[e]; w < cut.switchOffsets[e + 1]; ++w)
					cut.switchEntry.emplace(cut.switches[w], e);
			joinQueue.clear();
			joinQueued.assign(entries, 0);
			joinBlocked.assign(entries, 0);
			for (std::uint32_t e = 0; e < entries; ++e)
				if (const auto backoff = joinBackoff.find(cut.roots[e]); backoff != joinBackoff.end())
					joinBlocked[e] = backoff->second;
		}
		// Which members their switches select: the switch events name the entries to read again (dclf-cull-job-elimination.md,
		// "Phase 3"); a new snapshot, a resync or a skipped frame reads them all.
		cut.liveEvents = SceneStore::SwitchEventsLive();
		if (cut.liveEvents) {
			if (newSnapshot || liveStale) {
				cut.memberLive.assign(cut.members.size(), 1);
				for (std::uint32_t e = 0; e + 1 < cut.switchOffsets.size(); ++e)
					if (cut.switchOffsets[e] != cut.switchOffsets[e + 1])
						RefreshLive(e);
				++cutStats.liveAll;
			} else {
				for (const auto* node : switchChanges)
					if (const auto it = cut.switchEntry.find(node); it != cut.switchEntry.end()) {
						RefreshLive(it->second);
						++cutStats.liveEntries;
					}
			}
			liveStale = false;
		}
		// Residents under the new snapshot: each one's entry again, and those whose plan or members changed leave.
		if (newSnapshot && !residents.empty()) {
			std::vector<const RE::NiAVObject*> leaving;
			for (auto& [root, resident] : residents) {
				const auto it = cut.eligible.find(root);
				bool same = it != cut.eligible.end();
				if (same) {
					const std::uint32_t e = it->second;
					resident.entry = e;
					same = SelectionSame(resident) && ResidentOk(e);
				}
				if (!same)
					leaving.push_back(root);
			}
			for (const auto* root : leaving)
				EvictResident(root, Eviction::Snapshot);
		}
		UpdateResidents(true);
		standInLive = true;
		residentsLive = true;
		cutStats.residentFrames += 1;
		cutStats.residentEntries += residents.size();
		cut.processCount = count;
		for (std::uint32_t i = 0; i < count; ++i) {
			cut.processes[i] = processes[i];
			auto& out = jobOut[i];
			out = JobOut{ std::move(out.visible), std::move(out.pending) };
			out.visible.clear();
			out.pending.clear();
		}
		++cutStats.appliedFrames;
		cutStats.prepareTicks += Now() - start;
		// The list jobs are queued after this returns: their queueing orders everything above before their reads.
		frameLive.store(true, std::memory_order_release);
		gpuSunFrame = SunOnGpu();
	}

	int PrimaryCull::SlotOf(const RE::NiCullingProcess* a_process) const
	{
		for (std::uint32_t i = 0; i < cut.processCount; ++i)
			if (cut.processes[i] == a_process)
				return static_cast<int>(i);
		return -1;
	}

	bool PrimaryCull::StandIn(int a_slot, RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
	{
		// Resident: its records are drawn whenever the GPU finds them, and the feedback services its root.
		if (!residents.empty() && residents.contains(a_object)) {
			++jobOut[a_slot].resident;
			return true;
		}
		if (!standInLive)
			return false;
		const auto it = cut.eligible.find(a_object);
		if (it == cut.eligible.end()) {
			if (ReasonsProbe())
				if (const auto rejected = cut.rejected.find(a_object); rejected != cut.rejected.end()) {
					EngineProcess1(a_process, a_object, a_arg);
					if (a_object->GetFlags().any(RE::NiAVObject::Flag::kAccumulated)) {
						const std::uint32_t e = rejected->second;
						jobOut[a_slot].causeGeometries[cut.causeOf[e]] += cut.geometryOffsets[e + 1] - cut.geometryOffsets[e];  // tracked ones
					}
					return true;
				}
			return false;
		}
		const std::uint32_t e = it->second;
		const auto plan = cut.plans[e];
		auto& out = jobOut[a_slot];
		++out.seen;
		const bool fadeRoot = plan == EntryPlan::FadeRoot || plan == EntryPlan::LeafRoot || plan == EntryPlan::TreeRoot;
		// Fading: the engine's (its fade, and the registration's fade handling).
		if (fadeRoot && !Settled(a_object)) {
			++out.notSettled;
			return false;
		}
		if (!cut.admitted[e]) {
			// The engine culls it this frame; if it is in view, the colour epoch's draws decide its admission. Out of view
			// it may join on probation: nothing draws it this frame, and the build's draws of its patched records admit it.
			EngineProcess1(a_process, a_object, a_arg);
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kAccumulated))
				out.pending.push_back(e);
			else if (ProbationOn() && e < joinBlocked.size() && joinBlocked[e] <= frameCounter)
				out.probeCandidates.push_back(e);
			++out.notAdmitted;
			return true;
		}
		// A switch whose selected child is out of date: NiSwitchNode::OnVisible brings it up to date before culling it
		// (UpdateDownwardPass). With the switch events that is done when the selection changes (SceneStore::CatchUpSwitch),
		// so none is found here (CS_DCLF_SWITCH_PROBE counts them); without them, the engine culls this entry this frame.
		if (!cut.liveEvents || SwitchProbe()) {
			for (std::uint32_t w = cut.switchOffsets[e]; w < cut.switchOffsets[e + 1]; ++w) {
				const auto* switchNode = cut.switches[w];
				SceneStore::SwitchState state;
				if (!SceneStore::ReadSwitch(*switchNode, state) || state.index < 0)
					continue;
				const auto index = static_cast<std::uint16_t>(state.index);
				if (index < switchNode->GetChildren().capacity() && switchNode->GetChildren()[index] && state.childRevID && index < state.childRevCapacity &&
					state.childRevID[index] != state.revID) {
					if (cut.liveEvents) {
						// A switch updated every frame (an animated one) has revID one ahead of its selected child while
						// its own update pass runs alongside the list jobs, which then updates the child: not stale.
						if (state.childRevID[index] + 1 == state.revID) {
							++out.switchMidUpdate;
							continue;
						}
						if (++out.switchStaleSeen == 1) {
							static std::atomic<std::uint32_t> logged{ 0 };
							if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
								const auto* child = switchNode->GetChildren()[index].get();
								logger::info("[DCLF][TEMP] switch probe: stale selected child under '{}' ({}): switch '{}' {:#x} index {} of {}, revID {}, childRevID {}, flags {:#x}; child '{}' {}",
									a_object->name.c_str(), a_object->GetRTTI() ? a_object->GetRTTI()->name : "?", switchNode->name.c_str(), reinterpret_cast<std::uintptr_t>(switchNode),
									state.index, switchNode->GetChildren().size(), state.revID, state.childRevID[index], state.flags, child->name.c_str(),
									child->GetRTTI() ? child->GetRTTI()->name : "?");
							}
						}
						continue;
					}
					++out.switchStale;
					EngineProcess1(a_process, a_object, a_arg);
					return true;
				}
			}
		}
		++out.skipped;
		if (ResidentOn() && e < joinBlocked.size() && joinBlocked[e] <= frameCounter)
			out.joinCandidates.push_back(e);
		const bool feedback = FeedbackOn();
		if (feedback) {
			// The root's state (fade, LOD, the tree clock's bit) is the visibility feedback's (ConsumeFeedback); what is
			// left here is what the frame draws. A tree above the height limit draws nothing.
			out.stoodIn.push_back(e);
			if (plan == EntryPlan::TreeRoot && TreeAboveLimit(a_object, *a_process))
				return true;
			if (Outside(a_process->planes, a_object->worldBound))
				return true;
		}
		// The cull's test, against the job's own planes (its Process2 set them up from the list's first entry), and the
		// visibility bit Process1 keeps on what it tests (flags bit 26, which the tree clock reads).
		auto& flags = At<std::uint32_t>(a_object, kObjectFlags);
		if (!feedback && Outside(a_process->planes, a_object->worldBound)) {
			flags &= ~kFlagAccumulated;
			return true;
		}
		if (!feedback)
			flags |= kFlagAccumulated;
		++out.visibleEntries;
		// What OnVisible would have done to the root: its fade and LOD state. A root that faded out is not drawn. One that
		// has started to fade is the engine's this frame: its geometries go into this process's output, which the
		// registration jobs walk as they walk what the cull found (and the next frame's cull keeps it in).
		bool engineDraws = false;
		if (fadeRoot && a_process->camera && !feedback) {
			++out.fadeServiced;
			const bool recurse = plan == EntryPlan::TreeRoot ? ServiceTree(a_object, *a_process) : ServiceFade(a_object, plan == EntryPlan::LeafRoot, *a_process->camera);
			if (!recurse) {
				++out.fadedOut;
				return true;
			}
			engineDraws = !Settled(a_object);
		}
		for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1]; ++m) {
			const auto& member = cut.members[m];
			const auto* geometry = member.geometry;
			const bool selected = cut.liveEvents ? cut.memberLive[m] != 0 : PathSelected(member);
			if (cut.liveEvents && SwitchProbe() && selected != PathSelected(member))
				++out.switchMismatch;
			if (!selected) {
				++out.unselected;
				continue;
			}
			// NiAVObject::Cull skips an app-culled object and everything under it.
			bool hidden = false;
			for (const RE::NiAVObject* object = geometry; object && !hidden; object = object == a_object ? nullptr : object->parent)
				hidden = object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
			if (hidden) {
				++out.hidden;
				continue;
			}
			// The engine's: what its cull does for a visible geometry, the test of its bound, then the append to this
			// process (BSGeometry::OnVisible), in the traversal's order, for the registration jobs to register.
			if (member.engine || engineDraws) {
				if (!Outside(a_process->planes, geometry->worldBound)) {
					a_process->AppendVirtual(*const_cast<RE::BSGeometry*>(geometry), a_arg);
					++(member.engine ? out.engineMembers : out.handedBack);
				}
				continue;
			}
			out.visible.push_back(geometry);
		}
		return true;
	}

	void PrimaryCull::AfterListJobs()
	{
		if (!frameLive.exchange(false, std::memory_order_acq_rel))
			return;
		const std::int64_t start = Now();
		const std::uint32_t sunBits = SunAccumulation::Get().SunBits();
		frameVisible.clear();
		for (std::uint32_t i = 0; i < cut.processCount; ++i) {
			auto& out = jobOut[i];
			frameVisible.insert(frameVisible.end(), out.visible.begin(), out.visible.end());
			cut.pendingAdmission.insert(cut.pendingAdmission.end(), out.pending.begin(), out.pending.end());
			auto& s = cutStats;
			s.seen += out.seen, s.skipped += out.skipped, s.visibleEntries += out.visibleEntries, s.notSettled += out.notSettled;
			s.notAdmitted += out.notAdmitted, s.fadeServiced += out.fadeServiced, s.fadedOut += out.fadedOut, s.handedBack += out.handedBack;
			s.hiddenSkipped += out.hidden;
			s.engineMembers += out.engineMembers;
			if (FeedbackOn()) {
				stoodInScratch.insert(stoodInScratch.end(), out.stoodIn.begin(), out.stoodIn.end());
				out.stoodIn.clear();
			}
			s.switchStale += out.switchStale;
			s.unselected += out.unselected;
			s.switchMismatch += out.switchMismatch;
			s.switchStaleSeen += out.switchStaleSeen;
			s.switchMidUpdate += out.switchMidUpdate;
			s.residentSkips += out.resident;
			for (const std::uint32_t e : out.joinCandidates)
				if (e < joinQueued.size() && !joinQueued[e]) {
					joinQueued[e] = 1;
					joinQueue.push_back(e);
				}
			out.joinCandidates.clear();
			probeScratch.insert(probeScratch.end(), out.probeCandidates.begin(), out.probeCandidates.end());
			out.probeCandidates.clear();
			for (std::size_t c = 0; c < s.causeGeometries.size(); ++c)
				s.causeGeometries[c] += out.causeGeometries[c];
		}
		// The main registration reads the mask and clears it (+0x160 = 0xFFFF), so every later registration in the frame
		// reads 0. Here, after Finish, because the sun's Accumulate writes masks while the list jobs run.
		for (const auto* geometry : frameVisible)
			if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get())
				if (void* lightData = At<void*>(property, kPropertyLightData)) {
					auto& mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
					cutStats.localShadowed += (mask & ~sunBits) ? 1 : 0;
					mask = 0;
				}
		// Probation joins (dclf-cull-job-elimination.md, "Phase 4 in detail"): entries not yet admitted that the engine found
		// out of view join now, their records patched in this frame's accumulate phase; the colour epoch's build admits
		// those it drew in full (Admit), and the next PrepareFrame ends the others.
		if (standInLive && ResidentOn()) {
			std::size_t joined = 0;
			for (std::size_t i = 0; i < probeScratch.size() && joined < kJoinsPerFrame; ++i)
				joined += JoinEntry(probeScratch[i], true) ? 1 : 0;
		}
		probeScratch.clear();
		// This frame's stood-in entries ride with its feedback copy (IndirectDraws::ArmFeedback), and the frames whose
		// copies have completed are decoded on the worker: after this frame's list jobs, joined before the next's.
		if (FeedbackOn()) {
			auto tag = std::make_shared<FeedbackTag>();
			tag->candidates = cut.candidates;
			tag->stoodIn = std::move(stoodInScratch);
			stoodInScratch.clear();
			// The residents: the decode services them as it does the stood-in entries (their fade, LOD and kAccumulated),
			// and reports a root it finds fading (unsettledRoots).
			tag->residentFrom = static_cast<std::uint32_t>(tag->stoodIn.size());
			tag->fadeWitness.reserve(residents.size());
			for (const auto& [root, resident] : residents) {
				tag->stoodIn.push_back(resident.entry);
				tag->fadeWitness.push_back(resident.fadeWitness | (resident.fadeSensitive ? 0x10000u : 0u) |
										   (cut.plans[resident.entry] != EntryPlan::Plain ? 0x20000u : 0u));
			}
			tag->roots.reserve(tag->stoodIn.size());
			for (const std::uint32_t e : tag->stoodIn)
				tag->roots.emplace_back(const_cast<RE::NiAVObject*>(cut.roots[e]));
			pendingTag = std::move(tag);
		}
		// The synthetic passes on the worker, joined at the accumulate phase (BuildSyntheticPasses).
		if (!frameVisible.empty() && AsyncModeSetting() != AsyncMode::Off && AsyncJobEnabled("primary")) {
			synthJobDone.store(false, std::memory_order_relaxed);
			auto handle = std::make_shared<AsyncWorker::JobHandle>(AsyncWorker::Get().Submit("primary synthetic passes", [this](std::stop_token) {
				synthetic.clear();
				synthJobUnmodelled = 0;
				BuildSyntheticInto(synthetic, synthJobUnmodelled);
				synthJobDone.store(true, std::memory_order_release);
			}));
			synthJob = std::static_pointer_cast<void>(handle);
		}
		cutStats.afterTicks += Now() - start;
	}

	void PrimaryCull::KickFeedbackDecode()
	{
		// The feedback frames whose copies have completed, decoded on the worker every frame (whether or not the cut
		// applies this one). Kicked after the registration jobs (their CPU is not shared with it), joined at Present
		// (EndFrame), before the next frame's update reads the tree bits and its list jobs read the fade state.
		if (!FeedbackOn() || !Toggles::Get().Active().excludePrimaryEntries || feedbackJob)
			return;
		auto drain = [this] {
			IndirectDraws::Get().DrainVisibilityFeedback([this](const IndirectDraws::VisibilityFeedbackFrame& a_frame) {
				ConsumeFeedback(a_frame.stamp, a_frame.objects, a_frame.words, a_frame.tag);
			});
		};
		if (AsyncModeSetting() != AsyncMode::Off && AsyncJobEnabled("primary"))
			feedbackJob = std::static_pointer_cast<void>(std::make_shared<AsyncWorker::JobHandle>(
				AsyncWorker::Get().Submit("primary feedback", [drain](std::stop_token) { drain(); })));
		else
			drain();
	}

	void PrimaryCull::JoinFeedback()
	{
		if (!feedbackJob)
			return;
		auto handle = std::static_pointer_cast<AsyncWorker::JobHandle>(std::exchange(feedbackJob, {}));
		// It finished long ago (it runs right after the list jobs); a late one is waited for, as the jobs read the nodes it writes.
		if (AsyncWorker::Get().Wait(*handle, AsyncWaitBudget()) != AsyncWorker::WaitResult::Done)
			AsyncWorker::Get().Cancel(*handle);
		// The decoded frames' roots are released here, on the render thread.
		retiredTags.clear();
	}

	void PrimaryCull::ConsumeFeedback(std::uint32_t a_stamp, std::uint32_t a_objects, const std::uint32_t* a_words, const std::shared_ptr<void>& a_tag)
	{
		auto& counters = feedbackCounters;
		counters.frames.fetch_add(1, std::memory_order_relaxed);
		const auto* tag = static_cast<const FeedbackTag*>(a_tag.get());
		retiredTags.push_back(a_tag);  // its node references outlive the slot's, until the render thread's join
		// A resident whose passes no longer stand for its root goes back to the stand-in (and so to the engine): the root
		// started to fade or cross-fade, its LOD level changed (FUN_14147a430: the LOD row), or its LOD metric crossed the
		// specular or envmap fade end (the pass descriptor). Checked for every resident of every decoded frame, in view or not
		// and whatever the snapshot: the servicing below is not the roots' only writer (other views' OnVisible).
		const auto checkResident = [&](RE::NiAVObject* a_root, std::uint32_t a_witness) {
			if (!(a_witness & 0x20000u))
				return;
			if (!Settled(a_root)) {
				unsettledRoots.push_back(a_root);
			} else if (FadeWitnessOf(a_root, (a_witness & 0x10000u) != 0) != (a_witness & 0xFFFFu)) {
				unsettledRoots.push_back(a_root);
				counters.residentsWitness.fetch_add(1, std::memory_order_relaxed);
			}
		};
		// Another snapshot: the entry indices mean something else now, but the residents' roots are the tag's own.
		if (!tag || tag->candidates != cut.candidates) {
			counters.stale.fetch_add(1, std::memory_order_relaxed);
			if (tag)
				for (std::size_t i = tag->residentFrom; i < tag->roots.size() && i < tag->stoodIn.size(); ++i)
					checkResident(tag->roots[i].get(), tag->fadeWitness[i - tag->residentFrom]);
			return;
		}
		auto** processes = Global<RE::NiCullingProcess**>(kListProcesses);
		const auto* camera = processes && processes[0] ? processes[0]->camera : nullptr;
		std::uint64_t visible = 0, serviced = 0, unresolved = 0;
		std::uint64_t residentsVisible = 0, residentsFadeHidden = 0, residentRecordsInView = 0;
		for (std::size_t i = 0; i < tag->stoodIn.size(); ++i) {
			const std::uint32_t e = tag->stoodIn[i];
			if (e >= cut.roots.size() || i >= tag->roots.size())
				continue;
			const bool resident = i >= tag->residentFrom;
			// In view in that frame: any of the entry's own geometries inside the frustum (the GPU's phase 1).
			bool inView = false, fadeHidden = false;
			for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1] && !inView; ++m) {
				const std::int32_t object = cut.memberObject[m];
				if (cut.members[m].engine)
					continue;
				if (object < 0 || static_cast<std::uint32_t>(object) >= a_objects) {
					++unresolved;
					continue;
				}
				// The stamp's low 28 bits; bit 31 is the fade test's (BuildDrawsCS.hlsl, kFrustumFadeHidden).
				inView = (a_words[object] & 0x0FFFFFFFu) == a_stamp;
				fadeHidden = inView && (a_words[object] & 0x80000000u) != 0;
			}
			// [TEMP] every member in view, not only the first.
			if (i >= tag->residentFrom)
				for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1]; ++m)
					if (const std::int32_t object = cut.memberObject[m]; !cut.members[m].engine && object >= 0 && static_cast<std::uint32_t>(object) < a_objects &&
																	  (a_words[object] & 0x8FFFFFFFu) == a_stamp)
						++residentRecordsInView;
			auto* root = tag->roots[i].get();
			std::atomic_ref<std::uint32_t> flags(At<std::uint32_t>(root, kObjectFlags));
			const auto checkThis = [&] {
				if (resident)
					checkResident(root, tag->fadeWitness[i - tag->residentFrom]);
			};
			if (!inView) {
				flags.fetch_and(~kFlagAccumulated, std::memory_order_relaxed);
				checkThis();
				continue;
			}
			++visible;
			residentsVisible += resident ? 1 : 0;
			residentsFadeHidden += resident && fadeHidden ? 1 : 0;
			flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
			if (!camera) {
				checkThis();
				continue;
			}
			// [TEMP] the fade distance against the engine's servicing.
			bool beyond = false;
			if (resident && (cut.plans[e] == EntryPlan::FadeRoot || cut.plans[e] == EntryPlan::LeafRoot))
				if (const float distance = FadeDistanceOf(root); distance != 0.0f) {
					const auto& eye = camera->world.translate;
					const float dx = root->worldBound.center.x - eye.x, dy = root->worldBound.center.y - eye.y, dz = root->worldBound.center.z - eye.z;
					beyond = std::sqrt(dx * dx + dy * dy + dz * dz) * (distance > 0.0f ? At<float>(camera, kCameraLodAdjust) : 1.0f) > std::abs(distance);
				}
			const auto plan = cut.plans[e];
			switch (plan) {
			case EntryPlan::FadeRoot:
			case EntryPlan::LeafRoot:
				ServiceFade(root, plan == EntryPlan::LeafRoot, *camera);
				if (resident) {
					const bool fading = At<float>(root, kCurrentFade) < 1.0f || !(At<std::uint32_t>(root, kObjectFlags) & kFlagFadeTargetReached);
					if (beyond)
						(fading ? counters.fadeBeyondFading : counters.fadeBeyondSettled).fetch_add(1, std::memory_order_relaxed);
					else if (fading)
						counters.fadeWithinFading.fetch_add(1, std::memory_order_relaxed);
				}
				++serviced;
				break;
			case EntryPlan::TreeRoot:
				// BSTreeNode::OnVisible does nothing for a tree above the height limit (BuildDraws drops a resident one).
				if (processes && processes[0] && TreeAboveLimit(root, *processes[0])) {
					checkThis();
					continue;
				}
				ServiceTreeState(root, *camera);
				++serviced;
				if (FeedbackProbe()) {
					const float clock = At<float>(root, 0x164);
					const auto [it, inserted] = treeClocks.try_emplace(root, clock);
					counters.trees.fetch_add(1, std::memory_order_relaxed);
					if (!inserted && it->second != clock)
						counters.treesAdvanced.fetch_add(1, std::memory_order_relaxed);
					it->second = clock;
				}
				break;
			default:
				break;
			}
			checkThis();
		}
		counters.entries.fetch_add(tag->stoodIn.size(), std::memory_order_relaxed);
		counters.visible.fetch_add(visible, std::memory_order_relaxed);
		counters.serviced.fetch_add(serviced, std::memory_order_relaxed);
		counters.unresolved.fetch_add(unresolved, std::memory_order_relaxed);
		counters.residents.fetch_add(tag->residentFrom < tag->stoodIn.size() ? tag->stoodIn.size() - tag->residentFrom : 0, std::memory_order_relaxed);
		counters.residentsVisible.fetch_add(residentsVisible, std::memory_order_relaxed);
		counters.residentsFadeHidden.fetch_add(residentsFadeHidden, std::memory_order_relaxed);
		counters.residentRecordsInView.fetch_add(residentRecordsInView, std::memory_order_relaxed);
	}

	void PrimaryCull::BuildSyntheticInto(std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& a_out, std::uint64_t& a_unmodelled)
	{
		for (const auto* geometry : frameVisible) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
			if (!lighting) {
				++a_unmodelled;
				continue;
			}
			auto& cached = derivedCache[geometry];
			const std::uint8_t fadeState = FadeStateOf(property);
			if (cached.property != property || cached.material != lighting->material || cached.flags != lighting->flags.underlying() ||
				cached.fadeState != fadeState) {
				LightingDescriptors descriptors;
				const auto reason = DeriveLightingDescriptors(*lighting, *geometry, nullptr, descriptors, true);
				cached = { property, lighting->material, lighting->flags.underlying(), fadeState, reason == Ineligible::None ? descriptors.derivedPass : kNotDerived,
					descriptors.projectedUV || descriptors.technique == 8 || descriptors.technique == 19 };
			}
			AccumulatedPass pass;
			if (!SyntheticPass(*geometry, cached.derivedPass, pass, SunOnGpu())) {
				++a_unmodelled;
				continue;
			}
			a_out.emplace_back(geometry, pass);
		}
	}

	const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& PrimaryCull::JoinSyntheticPasses()
	{
		const std::int64_t start = Now();
		if (synthJob) {
			auto handle = std::static_pointer_cast<AsyncWorker::JobHandle>(synthJob);
			synthJob.reset();
			const auto result = AsyncWorker::Get().Wait(*handle, AsyncWaitBudget());
			if (result != AsyncWorker::WaitResult::Done) {
				// Late or dropped: it is ended (waited for when it has started), and built here when it never ran.
				++cutStats.synthLate;
				AsyncWorker::Get().Cancel(*handle);
			}
			if (synthJobDone.load(std::memory_order_acquire)) {
				cutStats.synthetic += synthetic.size();
				cutStats.unmodelled += synthJobUnmodelled;
				frameVisible.clear();
				cutStats.synthWaitTicks += Now() - start;
				return synthetic;
			}
		}
		synthetic.clear();
		if (!frameVisible.empty()) {
			++cutStats.synthInline;
			std::uint64_t unmodelled = 0;
			BuildSyntheticInto(synthetic, unmodelled);
			cutStats.unmodelled += unmodelled;
			cutStats.synthetic += synthetic.size();
		}
		frameVisible.clear();
		cutStats.synthWaitTicks += Now() - start;
		return synthetic;
	}

	const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& PrimaryCull::BuildSyntheticPasses()
	{
		const auto& passes = JoinSyntheticPasses();
		// After the synthetic job's join, so the decode never delays it on the worker.
		KickFeedbackDecode();
		return passes;
	}

	void PrimaryCull::AfterFullFrustum()
	{
		frameLive.store(false, std::memory_order_relaxed);
		gpuSunFrame = false;
		JoinFeedback();
		if (Toggles::Get().Active().excludePrimaryEntries)
			PrepareFrame();
		else if (!residents.empty())
			EndAllResidents();
		if (!Probe())
			return;
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
		if (!lists || !count)
			return;
		auto candidates = SceneStore::Get().GetSunCandidates();
		frameCandidates = candidates;
		listed.assign(candidates ? candidates->entries.size() : 0, 0);
		ankerl::unordered_dense::set<const RE::NiAVObject*> entrySet;
		auto& c = census;
		++c.frames;
		c.lists += count;
		c.sunCandidates += candidates ? candidates->entries.size() : 0;
		for (std::uint32_t l = 0; l < count; ++l) {
			const auto& list = lists[l];
			c.firstEntries += list.empty() ? 0 : 1;
			for (const auto& entry : list) {
				const auto* object = entry.get();
				if (!object)
					continue;
				++c.entries;
				entrySet.insert(object);
				const auto* reference = object->GetUserData();
				if (reference && reference->IsActor())
					++c.actorEntries;
				if (candidates) {
					const auto it = candidates->entries.find(object);
					if (it != candidates->entries.end()) {
						++c.candidates;
						listed[it->second] = 1;
						continue;
					}
				}
				if (c.others.size() < 200)
					++c.others[fmt::format("{} under {}{}", NameOf(object), NameOf(object->parent), reference && reference->IsActor() ? " (actor)" : "")];
				else
					++c.others["(more)"];
			}
		}
		if (candidates)
			for (std::uint32_t g = 0; g < candidates->geometryEntry.size(); ++g)
				c.candidateGeometries += listed[candidates->geometryEntry[g]];
		c.extraEntries += Global<SceneList>(kExtraList).size();

		// Where the player's third-person 3D reaches the cull: its chain of parents, marking the list entries.
		if (playerChain.empty() || (c.frames % 300) == 1) {
			std::string chain;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				for (auto* node = player->Get3D(false); node; node = node->parent)
					chain += fmt::format(" <- {}{}", NameOf(node), entrySet.contains(node) ? " [LIST ENTRY]" : "");
			playerChain = chain;
		}
		counting.store(true, std::memory_order_release);
	}

	void PrimaryCull::NoteRegistration(const void* a_accumulator, const RE::BSGeometry* a_geometry)
	{
		const bool main = a_accumulator == Global<void*>(kMainAccumulator);
		const bool depth = !main && a_accumulator == Global<void*>(kDepthAccumulator);
		if (!main && !depth) {
			registrations.other.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		bool under = false;
		if (const auto* candidates = frameCandidates.get()) {
			const auto it = candidates->geometries.find(a_geometry);
			under = it != candidates->geometries.end() && listed[candidates->geometryEntry[it->second]];
		}
		(main ? registrations.main : registrations.depth).fetch_add(1, std::memory_order_relaxed);
		if (under)
			(main ? registrations.mainUnder : registrations.depthUnder).fetch_add(1, std::memory_order_relaxed);
	}

	bool PrimaryCull::SunOnGpu()
	{
		static const bool onGpu = SwitchValue("CS_DCLF_SUN_GPU") != "0";
		return onGpu;
	}

	std::uint32_t PrimaryCull::SunShadowBits(const RE::BSGeometry& a_geometry)
	{
		const auto inCascades = SunAccumulation::Get().InSunCascades(a_geometry.worldBound);
		if (!inCascades)
			return ~0u;
		return *inCascades ? SunShadowStatic(a_geometry) : 0u;
	}

	std::uint32_t PrimaryCull::SunShadowStatic(const RE::BSGeometry& a_geometry)
	{
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		if (!lighting)
			return 0;
		const std::uint64_t flags = lighting->flags.underlying();
		const auto* fadeNode = lighting->fadeNode;
		const float fade = fadeNode ? fadeNode->GetRuntimeData().currentFade : 1.0f;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
		const float alpha = (material ? material->materialAlpha : 1.0f) * fade;
		const auto* alphaProperty = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		const bool blended = alphaProperty && (alphaProperty->alphaFlags & 1);
		// GetRenderPasses (AE 0x1414adfb0): local_164, local_167, local_165/local_168 and local_158.
		const bool translucent = alpha < 1.0f || blended;
		const bool screenDoor = Global<std::uint8_t>(kScreenDoorFades) && fadeNode && fadeNode->GetRuntimeData().unk154 && fade < 1.0f &&
		                        !(flags & (1ull << 19));
		constexpr std::uint64_t kOpaqueClasses = 0x800c000000ull;
		const bool eligible = !translucent || (screenDoor && !blended) || (flags & kOpaqueClasses);
		const bool shadowDirAllowed = eligible && !Global<std::uint8_t>(kNoSunShadowDir);
		bool deferred = Global<std::uint8_t*>(kMainAccumulator)[kAccumulatorDeferredShadow] != 0;
		if (!screenDoor || blended)
			deferred = deferred && !((alpha < 1.0f || blended || (flags & (1ull << 33))) && !(flags & kOpaqueClasses));
		// FUN_1414fcf80: ShadowDir when a mask bit names the sun (a cascade the bound meets: the caller's test).
		bool shadowDir = shadowDirAllowed;
		bool defShadow = deferred;
		if (!deferred)
			shadowDir = false;
		else if (!shadowDir)
			defShadow = false;  // no shadowed point light either (the caller's precondition)
		if (!(flags & 0x800c000100ull) && !lighting->shadowMapOrMaskPasses.head)
			shadowDir = defShadow = false;
		return (shadowDir ? 0x2000u : 0u) | (defShadow ? 0x4000u : 0u);
	}

	void PrimaryCull::UnifySunBits(const RE::BSGeometry* a_geometry, AccumulatedPass& a_pass) const
	{
		if (!gpuSunFrame || !cut.candidates || a_pass.sunTest)
			return;
		const auto& candidates = *cut.candidates;
		const auto it = candidates.geometries.find(a_geometry);
		if (it == candidates.geometries.end() || it->second >= candidates.primaryGeometry.size() || !candidates.primaryGeometry[it->second])
			return;
		const std::uint32_t sun = SunShadowStatic(*a_geometry);
		a_pass.technique = (a_pass.technique & ~0x6000u) | sun;
		a_pass.passEnum = a_pass.technique + 0x4800002Du;
		a_pass.sunTest = sun != 0;
	}

	bool PrimaryCull::SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out, bool a_sunOnGpu)
	{
		if (a_derivedPass == kNotDerived)
			return false;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		if (!lighting)
			return false;
		const std::uint64_t flags = lighting->flags.underlying();
		const auto* fadeNode = lighting->fadeNode;
		const float fade = fadeNode ? fadeNode->GetRuntimeData().currentFade : 1.0f;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
		const auto* alphaProperty = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		const bool blended = alphaProperty && (alphaProperty->alphaFlags & 1);
		const bool translucent = (material ? material->materialAlpha : 1.0f) * fade < 1.0f || blended;
		// Fading and translucent objects take GetRenderPasses' other hints (1, 9, 10) and the screen-door bit; not modelled.
		if (fade < 1.0f)
			return false;
		// On the GPU (a_sunOnGpu): the bits the object takes inside a cascade, and BuildDraws drops them on a miss.
		const std::uint32_t sun = a_sunOnGpu ? SunShadowStatic(a_geometry) : SunShadowBits(a_geometry);
		if (sun == ~0u)
			return false;
		std::uint32_t hint = 0;
		if (flags & 0xc000000ull)
			hint = translucent ? 3 : 2;  // the decal groups
		else if (translucent)
			return false;
		else if (flags & 0x8000000600000000ull)
			hint = (~static_cast<std::uint32_t>(flags >> 33) & 1u) | 6u;
		else if (flags & (1ull << 61))
			hint = 11;  // TreeAnim
		else if (!sun)
			hint = 15;  // opaque with no shadow work (no shadowed point light: the caller's precondition)
		a_out = {};
		a_out.subPass = PassCapture::SubPassOf(&a_geometry, flags);
		a_out.technique = DrawnPassDescriptor((a_derivedPass & ~0x6000u) | sun, a_out.subPass);
		a_out.passEnum = a_out.technique + 0x4800002Du;
		a_out.hint = hint;
		a_out.lodRow = SceneStore::LodRowOf(a_geometry, property);
		a_out.sunTest = a_sunOnGpu && sun != 0;
		return true;
	}

	bool PrimaryCull::UnderListedCandidate(const RE::BSGeometry* a_geometry) const
	{
		const auto* candidates = frameCandidates.get();
		if (!candidates)
			return false;
		const auto it = candidates->geometries.find(a_geometry);
		return it != candidates->geometries.end() && listed[candidates->geometryEntry[it->second]];
	}

	void PrimaryCull::NoteDerived(const RE::BSGeometry& a_geometry, const LightingDescriptors& a_descriptors, const AccumulatedPass& a_accumulated,
		Ineligible a_reason, std::uint32_t a_derivedLodRow)
	{
		auto& d = derived;
		++d.objects;
		++d.hints[std::min<std::uint32_t>(a_accumulated.hint, 31)];
		d.fading += a_accumulated.fading ? 1 : 0;
		d.alphaMask += (a_accumulated.technique & kPassAdditionalAlphaMask) ? 1 : 0;
		if (const auto* pass = a_accumulated.pass; pass && pass->numShadowLights)
			++d.shadowLights;
		if (a_geometry.GetGeometryRuntimeData().skinInstance && a_derivedLodRow != a_accumulated.lodRow)
			++d.lodRowDiffer;
		if (a_reason != Ineligible::None) {
			++d.ineligible;
			++d.byReason[std::min<std::size_t>(static_cast<std::size_t>(a_reason), d.byReason.size() - 1)];
			return;
		}
		if (a_descriptors.derivedPass == kNotDerived) {
			++d.notDerived;
			return;
		}
		constexpr std::uint32_t kSunBits = 0x6000u;
		const std::uint32_t differing = a_descriptors.derivedPass ^ a_descriptors.pass;
		if (differing & ~kSunBits)
			++d.differ;
		else if (differing)
			++d.sunOnly;
		for (std::uint32_t remaining = differing; remaining; remaining &= remaining - 1)
			++d.bits[std::countr_zero(remaining)];
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = static_cast<const RE::BSLightingShaderProperty*>(property);
		if (std::abs(a_descriptors.derivedSpecularLODFade - lighting->specularLODFade) > 1e-3f)
			++d.specularFadeDiffer;
		if (std::abs(a_descriptors.derivedEnvmapLODFade - lighting->envmapLODFade) > 1e-3f)
			++d.envmapFadeDiffer;
		if (const std::uint32_t sun = SunShadowBits(a_geometry); sun == ~0u) {
			++d.sunUnknown;
		} else {
			const std::uint32_t engine = a_descriptors.pass & kSunBits;
			if (sun == engine)
				++d.sunAgree;
			else if (!sun)
				++d.sunEngineOnly;
			else if (!engine)
				++d.sunDclfOnly;
			else
				++d.sunOther;
			if (sun != engine && d.sunSamples.size() < 24) {
				const auto& bound = a_geometry.worldBound;
				++d.sunSamples[fmt::format("'{}' under '{}': engine {:#x} DCLF {:#x}, bound ({:.0f} {:.0f} {:.0f}) r {:.0f}, hint {}",
					a_geometry.name.c_str() ? a_geometry.name.c_str() : "?", a_geometry.parent && a_geometry.parent->name.c_str() ? a_geometry.parent->name.c_str() : "?",
					engine, sun, bound.center.x, bound.center.y, bound.center.z, bound.radius, a_accumulated.hint)];
			}
		}
		if (AccumulatedPass built; !SyntheticPass(a_geometry, a_descriptors.derivedPass, built)) {
			++d.synthUnmodeled;
		} else {
			const bool technique = built.technique != a_accumulated.technique, subPass = built.subPass != a_accumulated.subPass;
			const bool hint = built.hint != a_accumulated.hint, lodRow = built.lodRow != a_accumulated.lodRow;
			d.synthTechnique += technique, d.synthSubPass += subPass, d.synthHint += hint, d.synthLodRow += lodRow;
			if (!technique && !subPass && !hint && !lodRow)
				++d.synthAgree;
			else if (d.synthSamples.size() < 24)
				++d.synthSamples[fmt::format("'{}' under '{}': technique {:08X}/{:08X} subPass {}/{} hint {}/{} lodRow {}/{} (engine/DCLF)",
					a_geometry.name.c_str() ? a_geometry.name.c_str() : "?", a_geometry.parent && a_geometry.parent->name.c_str() ? a_geometry.parent->name.c_str() : "?",
					a_accumulated.technique, built.technique, a_accumulated.subPass, built.subPass, a_accumulated.hint, built.hint, a_accumulated.lodRow, built.lodRow)];
		}
		if ((differing & ~kSunBits) && d.samples.size() < 40)
			++d.samples[fmt::format("'{}' under '{}': registered {:08X} derived {:08X} (hint {}, subPass {})", a_geometry.name.c_str() ? a_geometry.name.c_str() : "?",
				a_geometry.parent && a_geometry.parent->name.c_str() ? a_geometry.parent->name.c_str() : "?", a_descriptors.pass, a_descriptors.derivedPass,
				a_accumulated.hint, a_accumulated.subPass)];
	}

	struct PrimaryCull::Hooks
	{
		/**
		 * @brief BSGeometryListCullingProcess::Process1 (vtable slot 0x16, AE 0x140e28390), which every list job calls for
		 * each entry after its first and for every child a node's OnVisible culls. The vtable is shared with the sun's
		 * full-frustum processes and every other list process; only the scene lists' own processes are stood in for.
		 */
		struct Process1
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				auto& self = PrimaryCull::Get();
				if (a_object && self.frameLive.load(std::memory_order_acquire))
					if (const int slot = self.SlotOf(a_process); slot >= 0 && self.StandIn(slot, a_process, a_object, a_arg))
						return;
				func(a_process, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief CalculateAndDrawShadowCasterLights' call of FUN_1414a0840, right after the full-frustum cull. */
		struct AfterFullFrustum
		{
			static std::uint64_t thunk(std::uint64_t a_1, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
			{
				PrimaryCull::Get().AfterFullFrustum();
				return func(a_1, a_2, a_3, a_4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Its JobList::Finish of the scene list culling jobs. */
		struct ListJobsFinish
		{
			static std::uint64_t thunk(std::uint64_t a_1, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
			{
				const auto result = func(a_1, a_2, a_3, a_4);
				PrimaryCull::Get().AfterListJobs();
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void PrimaryCull::Install()
	{
		if (installed || !REL::Module::IsAE())
			return;
		const auto base = REL::Module::get().base();
		const auto afterFullFrustum = base + kAfterFullFrustumCallSite;
		const auto finish = base + kListJobsFinishCallSite;
		if (!CallsTo(afterFullFrustum, base + kAfterFullFrustum) || !CallsTo(finish, base + kListJobsFinish)) {
			logger::warn("[DCLF] primary cull: the calls are not where expected; the engine keeps the primary's cull");
			return;
		}
		// The list processes' Process1 slot must still be the engine's (no other hook in the way).
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSGeometryListCullingProcess[0] };
		if (reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x16] != base + kProcess1) {
			logger::warn("[DCLF] primary cull: BSGeometryListCullingProcess::Process1 is not the engine's; the engine keeps the primary's cull");
			return;
		}
		stl::write_thunk_call<Hooks::AfterFullFrustum>(afterFullFrustum);
		stl::write_thunk_call<Hooks::ListJobsFinish>(finish);
		stl::write_vfunc<0x16, Hooks::Process1>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		REL::Relocation<std::uintptr_t> triShape{ RE::VTABLE_BSTriShape[0] };
		geometryOnVisible = reinterpret_cast<const std::uintptr_t*>(triShape.address())[0x34];
		installed = true;
		logger::info("[DCLF] primary cull installed (the scene lists after the full-frustum cull, the list jobs' Finish){}", Probe() ? "; census on" : "");
	}

	void PrimaryCull::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if (!installed || (a_frame % a_interval) != 0)
			return;
		if (const auto& s = cutStats; s.frames) {
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
			const double applied = std::max<double>(static_cast<double>(s.appliedFrames), 1.0);
			logger::info("[DCLF] primary exclusion: applied on {} of {} frames ({} stale, {} preconditions); per frame {:.0f} eligible entries reached, "
						 "{:.0f} stood in for ({:.0f} in view), {:.1f} fading, {:.1f} not yet admitted ({:.1f} admitted); {:.0f} synthetic passes, {:.1f} not modelled "
						 "({:.1f} built inline, {:.1f} late), {:.1f} hidden, {:.1f} with a local light's shadow bit; {} holes; fades serviced {:.0f}, faded out {:.1f}, "
						 "handed back {:.1f}; {:.0f} of the engine's members in view registered by it; switches: {:.1f} entries culled by the engine (stale child), {:.0f} members unselected, selection read from every switch on {} frames and from {} events' entries; render thread: prepare {:.3f} ms, after the jobs {:.3f} ms, synthetic join {:.3f} ms",
				s.appliedFrames, s.frames, s.skippedStale, s.skippedPreconditions, s.seen / applied, s.skipped / applied, s.visibleEntries / applied,
				s.notSettled / applied, s.notAdmitted / applied, s.admittedNow / applied, s.synthetic / applied, s.unmodelled / applied,
				s.synthInline / applied, s.synthLate / applied, s.hiddenSkipped / applied, s.localShadowed / applied, s.holes, s.fadeServiced / applied,
				s.fadedOut / applied, s.handedBack / applied, s.engineMembers / applied, s.switchStale / applied, s.unselected / applied, s.liveAll, s.liveEntries, s.prepareTicks * toMs / applied, s.afterTicks * toMs / applied, s.synthWaitTicks * toMs / applied);
			for (std::size_t c = 0; c < s.causeGeometries.size(); ++c)
				if (s.causeGeometries[c])
					logger::info("[DCLF][TEMP] primary exclusion kept in view: {:.1f} geometries/frame under entries rejected for {}", s.causeGeometries[c] / applied,
						c < cut.causes.size() ? cut.causes[c] : std::string("(more)"));
			for (const auto& [cause, count] : s.planReasons)
				logger::info("[DCLF][TEMP] primary exclusion plan rejects: {} x {}", count, cause);
			if (ResidentOn()) {
				const auto r = SceneStore::Get().TakeResidentStats();
				const double rf = std::max<double>(static_cast<double>(r.frames), 1.0);
				const std::uint64_t residentVisible = feedbackCounters.residentsVisible.exchange(0, std::memory_order_relaxed);
				const std::uint64_t residentDecoded = feedbackCounters.residents.exchange(0, std::memory_order_relaxed);
				const double decodedFrames = std::max<double>(static_cast<double>(feedbackCounters.frames.load(std::memory_order_relaxed)), 1.0);
				logger::info("[DCLF] resident entries: {:.0f} a frame ({} frames), {:.0f} list-job returns; {} joined, {} refused, {} ended all; evicted: {} record, {} members, {} fading, {} snapshot, {} switch; "
							 "records: {:.0f} resident, {} patched, {} failed ({} the engine's pass, {} no record, {} a frame verdict, {} material or extras), {} rewritten, {} released, {:.1f} a frame registered by the engine anyway; feedback: {:.0f} resident entries, {:.0f} of them in view, per decoded frame",
					s.residentEntries / std::max<double>(static_cast<double>(s.residentFrames), 1.0), s.residentFrames, s.residentSkips / applied, s.joins, s.joinRefused, s.endedAll,
					s.evicted[0], s.evicted[1], s.evicted[2], s.evicted[3], s.evicted[5], r.resident / rf, r.joined, r.failed, r.failedBy[0], r.failedBy[1], r.failedBy[2], r.failedBy[3], r.rewritten, r.released, r.registered / rf,
					residentDecoded / decodedFrames, residentVisible / decodedFrames);
				logger::info("[DCLF] resident probation: {} joined out of view before admission, {} admitted by the build's draws, {} ended not drawn, {} refused; "
							 "refusals: {} plan, {} root not settled, {} an engine member, {} a hidden member, {} a member not event-written, {} a pass not built, {} a decal under the "
							 "GPU's tests; feedback: {} resident entries dropped by the GPU's fade or height test, {} left for a stale LOD level or metric",
					s.probationJoins, s.probationConfirmed, s.evicted[4], s.probationRefused, s.refusedBy[1], s.refusedBy[2], s.refusedBy[3], s.refusedBy[4], s.refusedBy[5],
					s.refusedBy[6], s.refusedBy[7],
					feedbackCounters.residentsFadeHidden.exchange(0, std::memory_order_relaxed), feedbackCounters.residentsWitness.exchange(0, std::memory_order_relaxed));
				{
					std::array<std::uint32_t, 5> byPlan{};
					for (const auto& [root, resident] : residents)
						if (resident.entry < cut.plans.size())
							++byPlan[static_cast<std::size_t>(cut.plans[resident.entry])];
					logger::info("[DCLF][TEMP] residents by plan: {} plain, {} fade, {} leaf, {} tree; {:.0f} resident records in view (GPU frustum, not fade-dropped) per decoded frame",
						byPlan[0], byPlan[1], byPlan[2], byPlan[3],
						feedbackCounters.residentRecordsInView.exchange(0, std::memory_order_relaxed) / std::max<double>(static_cast<double>(feedbackCounters.frames.load(std::memory_order_relaxed)), 1.0));
				}
				logger::info("[DCLF][TEMP] fade distance probe: residents in view past it {} fading, {} not; within it {} fading",
					feedbackCounters.fadeBeyondFading.exchange(0, std::memory_order_relaxed), feedbackCounters.fadeBeyondSettled.exchange(0, std::memory_order_relaxed),
					feedbackCounters.fadeWithinFading.exchange(0, std::memory_order_relaxed));
				if (r.parityChecks)
					logger::info("[DCLF] resident parity: {} checks, {} records compared, {} passes differ, {} records differ ({} not compared: the root fading, leaving at the next decode){}",
						r.parityChecks, r.parityChecked, r.parityPass, r.parityRecord, r.parityPending, r.parityPass || r.parityRecord ? " <- RESIDENT PARITY" : " <- OK");
			}
			if (SwitchProbe())
				logger::info("[DCLF][TEMP] switch probe: {} members whose event-driven selection differs from their switches, {} selected children out of date, {} seen mid-update (every applied frame since the last report)",
					s.switchMismatch, s.switchStaleSeen, s.switchMidUpdate);
			cutStats = {};
			if (FeedbackOn()) {
				const auto io = IndirectDraws::Get().TakeFeedbackStats();
				auto& c = feedbackCounters;
				const auto frames = c.frames.exchange(0), stale = c.stale.exchange(0), entries = c.entries.exchange(0);
				const auto visibleEntries = c.visible.exchange(0), serviced = c.serviced.exchange(0), unresolved = c.unresolved.exchange(0);
				const double decodedFrames = std::max<double>(static_cast<double>(frames - stale), 1.0);
				if (FeedbackProbe()) {
					const auto trees = c.trees.exchange(0), advanced = c.treesAdvanced.exchange(0);
					logger::info("[DCLF][TEMP] primary feedback probe: {} stood-in tree services in view, {} of them with the tree clock moved since the last decode", trees, advanced);
				}
				logger::info("[DCLF] primary feedback: {} frames armed, {} dropped (no free slot), {} abandoned, {} decoded ({} stale); per decoded frame {:.0f} stood-in entries, "
							 "{:.0f} of them in view, {:.0f} serviced, {:.1f} member objects unresolved",
					io.armed, io.dropped, io.abandoned, io.decoded, stale, entries / decodedFrames, visibleEntries / decodedFrames, serviced / decodedFrames,
					unresolved / decodedFrames);
			}
		}
		if (!Probe() || !census.frames)
			return;
		const auto& c = census;
		const double f = static_cast<double>(c.frames);
		const auto take = [](std::atomic<std::uint64_t>& a_value) { return a_value.exchange(0, std::memory_order_relaxed); };
		const auto main = take(registrations.main), mainUnder = take(registrations.mainUnder);
		const auto depth = take(registrations.depth), depthUnder = take(registrations.depthUnder), other = take(registrations.other);
		logger::info("[DCLF] primary census, per frame: {:.0f} lists, {:.0f} entries ({:.0f} first), {:.0f} extra; {:.0f} are sun candidates ({:.0f} of the candidates' {:.0f}; {:.0f} tracked geometries under them), {:.0f} actor entries; "
					 "registrations: main {:.0f} ({:.0f} under a listed candidate), depth {:.0f} ({:.0f}), other {:.0f}",
			c.lists / f, c.entries / f, c.firstEntries / f, c.extraEntries / f, c.candidates / f, c.candidates / f, c.sunCandidates / f, c.candidateGeometries / f,
			c.actorEntries / f, main / f, mainUnder / f, depth / f, depthUnder / f, other / f);
		std::vector<std::pair<std::uint64_t, std::string>> top;
		for (const auto& [key, count] : c.others)
			top.emplace_back(count, key);
		std::sort(top.rbegin(), top.rend());
		for (std::size_t i = 0; i < top.size() && i < 30; ++i)
			logger::info("[DCLF] primary census, not a candidate: {:.2f}/frame {}", top[i].first / f, top[i].second);
		logger::info("[DCLF] primary census, the player's 3D:{}", playerChain);
		census = {};
		if (derived.objects) {
			const double n = static_cast<double>(derived.objects) / f;
			auto& d = derived;
			std::string bits, hints, reasons;
			for (std::uint32_t b = 0; b < 32; ++b)
				if (d.bits[b])
					bits += fmt::format(" {}={:.1f}", b, d.bits[b] / f);
			for (std::uint32_t h = 0; h < 32; ++h)
				if (d.hints[h])
					hints += fmt::format(" {}={:.1f}", h, d.hints[h] / f);
			for (std::size_t r = 0; r < d.byReason.size(); ++r)
				if (d.byReason[r])
					reasons += fmt::format(" {}={:.1f}", r < kIneligibleNames.size() ? kIneligibleNames[r] : "?", d.byReason[r] / f);
			logger::info("[DCLF] primary derivation, per frame: {:.0f} registered objects under listed candidates: {:.1f} ineligible ({}), {:.1f} not derived; "
						 "descriptor: {:.1f} differ outside the sun bits, {:.1f} only in them; bits:{}; {:.1f} with shadowed point lights, {:.1f} LOD rows differ, "
						 "{:.1f} fading, {:.1f} screen-door; LOD fades differ: specular {:.1f}, envmap {:.1f}; hints:{}",
				n, d.ineligible / f, reasons, d.notDerived / f, d.differ / f, d.sunOnly / f, bits, d.shadowLights / f, d.lodRowDiffer / f, d.fading / f, d.alphaMask / f,
				d.specularFadeDiffer / f, d.envmapFadeDiffer / f, hints);
			for (const auto& [sample, count] : d.samples)
				logger::info("[DCLF] primary derivation differs: {:.2f}/frame {}", count / f, sample);
			logger::info("[DCLF] primary sun bits, per frame: {:.1f} agree, {:.1f} engine only, {:.1f} DCLF only, {:.1f} otherwise, {:.1f} unknown",
				d.sunAgree / f, d.sunEngineOnly / f, d.sunDclfOnly / f, d.sunOther / f, d.sunUnknown / f);
			for (const auto& [sample, count] : d.sunSamples)
				logger::info("[DCLF] primary sun bits differ: {:.2f}/frame {}", count / f, sample);
			logger::info("[DCLF] primary synthetic pass, per frame: {:.1f} agree in every field, {:.1f} not modelled; differ: technique {:.1f}, subPass {:.1f}, hint {:.1f}, LOD row {:.1f}",
				d.synthAgree / f, d.synthUnmodeled / f, d.synthTechnique / f, d.synthSubPass / f, d.synthHint / f, d.synthLodRow / f);
			for (const auto& [sample, count] : d.synthSamples)
				logger::info("[DCLF] primary synthetic pass differs: {:.2f}/frame {}", count / f, sample);
			derived = {};
		}
	}
}
