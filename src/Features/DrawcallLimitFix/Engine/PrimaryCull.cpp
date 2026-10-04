#include "PrimaryCull.h"

#include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"
#include "PassCapture.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#include "EngineAccess.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "SunAccumulation.h"
#include "TreeAnimation.h"
#include "LocalLightCull.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/FadeState.h"

#include <algorithm>
#include <ranges>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

namespace DCLF
{
	namespace
	{
		using namespace Engine;

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
		constexpr std::uintptr_t kAnimatedFadeCallSite = 0x2d0044;  // FUN_1402cff60 (UpdateAnimationJob) -> FUN_14147a160
		constexpr std::uintptr_t kFadeUpdate = 0x147a160;         // FUN_14147a160(node, fadeAmount, camera): the fade state machine
		constexpr std::uintptr_t kFadeDistance = 0x147b110;       // FUN_14147b110(node, camera, out): the distance and LOD level
		constexpr std::uintptr_t kLeafLodUpdate = 0x147a430;      // FUN_14147a430(node, level): the leaf and tree LOD transition
		// BSTreeNode::OnVisible (AE 0x14147d3c0): the height test and the LOD fix-up.
		constexpr std::size_t kProcessTreeHeightTest = 0x121;     // byte on the culling process
		constexpr std::uintptr_t kTreeHeightTestOn = 0x2032fb8;   // byte
		constexpr std::uintptr_t kTreeHeightBase = 0x332a2f0;     // float
		constexpr std::uintptr_t kTreeHeightLimit = 0x2032fa0;    // float
		constexpr std::size_t kTreeLodData = 0x180;               // pointer; its +0x12C count
		// FUN_14147b110 (the fade value) and FUN_14147a160 (the fade update): what the fade-out distance is made of.
		constexpr std::size_t kFadeNear = 0x128;                  // float, on the fade node
		constexpr std::size_t kFadeFar = 0x12C;                   // float
		constexpr std::size_t kFadeAlwaysOut = 0x109;             // byte: bit 0 fades the node out whatever its distance
		constexpr std::uintptr_t kFadeTypeDivisors = 0x2032e00;   // float per LOD type (+0x153 & 0xF): the LOD factor's divisor
		constexpr std::uintptr_t kFadeDefaultScale = 0x1ad2840;   // float: the scale when the type's divisor is not positive
		constexpr std::uintptr_t kFadeOutThreshold = 0x2032e38;   // float: a faded-in node fades out below this fade value
		constexpr std::uintptr_t kFadeDistanceMult = 0x2032e48;   // float: multiplies the node's near and far distances

		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

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

	bool PrimaryCull::Probe()
	{
		const bool probe = SwitchValue(Switch::PrimaryExclude) == "probe";
		return probe;
	}

	std::int32_t PrimaryCull::SwitchIndex(const RE::NiSwitchNode* a_switch)
	{
		return SceneStore::ReadSwitch(*a_switch).index;
	}

	bool PrimaryCull::FreshSyntheticPass(const RE::BSGeometry& a_geometry, AccumulatedPass& a_out)
	{
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
		if (!lighting)
			return false;
		LightingDescriptors descriptors;
		if (DeriveLightingDescriptors(*lighting, a_geometry, nullptr, descriptors) != Ineligible::None)
			return false;
		if (!SyntheticPass(a_geometry, descriptors.derivedPass, a_out, true))
			return false;
		a_out.resident = true;
		return true;
	}

	float PrimaryCull::FadeDistanceOf(const RE::NiAVObject* a_root)
	{
		// No fade update at all, or no distance term in the fade value (FUN_14147b110 returns 1).
		if (!Global<std::uint8_t>(kFadesOn) || !Global<std::uint8_t>(kFadeLodUpdates))
			return 0.0f;
		const std::uint32_t type = At<std::uint8_t>(a_root, 0x153) & 0xF;
		// Type 8 fades gradually even from out of view (FUN_14147a160), and OnVisible's type-6 branch never fades out
		// (ServiceFade): the engine draws either on the frame it comes into view, as the member is.
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

	std::uint32_t PrimaryCull::MembershipWitness()
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

	bool PrimaryCull::MembershipPass(const RE::BSGeometry* a_geometry, AccumulatedPass& a_out)
	{
		const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		if (!lighting)
			return false;
		auto& cached = derivedCache[a_geometry];
		const std::uint8_t fadeState = FadeStateOf(property);
		if (cached.property != property || cached.material != lighting->material || cached.flags != lighting->flags.underlying() || cached.fadeState != fadeState) {
			LightingDescriptors descriptors;
			const auto reason = DeriveLightingDescriptors(*lighting, *a_geometry, nullptr, descriptors);
			cached = { property, lighting->material, lighting->flags.underlying(), fadeState, reason == Ineligible::None ? descriptors.derivedPass : kNotDerived };
		}
		if (!SyntheticPass(*a_geometry, cached.derivedPass, a_out, true))
			return false;
		a_out.resident = true;
		return true;
	}

	bool PrimaryCull::MembershipLayerPass(const RE::BSGeometry* a_geometry, const RE::BSLightingShaderProperty& a_layer, AccumulatedPass& a_out)
	{
		auto& cached = layerDerivedCache[a_geometry];
		const std::uint8_t fadeState = FadeStateOf(&a_layer);
		if (cached.property != &a_layer || cached.material != a_layer.material || cached.flags != a_layer.flags.underlying() || cached.fadeState != fadeState) {
			LightingDescriptors descriptors;
			const auto reason = DeriveLightingDescriptors(a_layer, *a_geometry, nullptr, descriptors, true);
			cached = { &a_layer, a_layer.material, a_layer.flags.underlying(), fadeState, reason == Ineligible::None ? descriptors.derivedPass : kNotDerived };
		}
		if (!SyntheticPass(*a_geometry, cached.derivedPass, a_out, true, &a_layer))
			return false;
		a_out.resident = true;
		return true;
	}

	void PrimaryCull::RefreshLive(std::uint32_t a_e)
	{
		for (std::uint32_t m = cut.memberOffsets[a_e]; m < cut.memberOffsets[a_e + 1]; ++m)
			cut.memberLive[m] = PathSelected(cut.members[m]) ? 1 : 0;
	}

	bool PrimaryCull::TreeAboveLimit(const RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process)
	{
		// Above the height limit (worldBound.center.z against a base), with the test on: nothing, and no recursion.
		return At<std::uint8_t>(&a_process, kProcessTreeHeightTest) && Global<std::uint8_t>(kTreeHeightTestOn) &&
		       a_node->worldBound.center.z - Global<float>(kTreeHeightBase) > Global<float>(kTreeHeightLimit);
	}

	PrimaryCull::EntryPlan PrimaryCull::PlanOf([[maybe_unused]] std::uint32_t a_entry, const RE::NiAVObject* a_root)
	{
		const auto& candidates = *cut.candidates;
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
				for (std::uint16_t c = 0; c < children.free_idx(); ++c) {
					path.emplace_back(switchNode, static_cast<std::int32_t>(c));
					a_self(a_self, children[c].get(), false);
					path.pop_back();
				}
				return;
			}
			if (!node || !(RttiIs(a_object, "NiNode") || RttiIs(a_object, "BSMultiBoundNode") ||
							  (a_isRoot && (RttiIs(a_object, "BSFadeNode") || RttiIs(a_object, "BSLeafAnimNode") || RttiIs(a_object, "BSTreeNode"))))) {
				rejected = true;
				return;
			}
			for (const auto& child : node->GetChildren())
				a_self(a_self, child.get(), false);
		};
		visit(visit, a_root, true);
		if (rejected || !ours) {
			cut.members.resize(first);
			cut.switchPaths.resize(firstPath);
			cut.switches.resize(firstSwitch);
			return EntryPlan::Rejected;
		}
		return RttiIs(a_root, "BSFadeNode") ? EntryPlan::FadeRoot :
		       RttiIs(a_root, "BSLeafAnimNode") ? EntryPlan::LeafRoot :
		       RttiIs(a_root, "BSTreeNode") ? EntryPlan::TreeRoot : EntryPlan::Plain;
	}

	void PrimaryCull::PrepareFrame()
	{
		++cutStats.frames;
		// The membership witness, before the list jobs: the commit sampled it at the scene phase (a frame without one samples it
		// here), and BindByMembership reads the same sample.
		if (sampledWitnessFrame != SceneStore::Get().GetFrame())
			SampleMembershipWitness();
		// The switches whose selection the walks applied since the last frame; a frame the cut skips drops them, so the
		// next one reads every switch again.
		const bool switchResync = SceneStore::Get().TakeSwitchChanges(switchChanges);
		liveStale = switchResync || liveStale;
		auto candidates = SceneStore::Get().GetSunCandidates();
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto** processes = Global<RE::NiCullingProcess**>(kListProcesses);
		++frameCounter;
		frameSet = PassCapture::Get().CurrentSet();
		standInLive = false;
		// The camera the fade roots' distances are measured from (BuildDraws' fade test).
		fadeEye = {};
		if (const auto* camera = processes && count ? processes[0]->camera : nullptr)
			fadeEye = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z, At<float>(camera, kCameraLodAdjust) };
		// BSTreeNode::OnVisible's height test, as the list processes have it this frame (TreeAboveLimit).
		const bool heightTest = processes && count && processes[0] && At<std::uint8_t>(processes[0], kProcessTreeHeightTest) && Global<std::uint8_t>(kTreeHeightTestOn);
		treeHeight = { Global<float>(kTreeHeightBase), heightTest ? Global<float>(kTreeHeightLimit) : std::numeric_limits<float>::infinity() };
		if (!processes || !count || count > cut.processes.size())
			return;
		// The list processes this frame: the leaf exclusion (AppendVirtual) applies whenever they are known.
		cut.processCount = count;
		for (std::uint32_t i = 0; i < count; ++i) {
			cut.processes[i] = processes[i];
			auto& out = jobOut[i];
			out = JobOut{ std::move(out.visible), std::move(out.pending) };
			out.visible.clear();
			out.pending.clear();
		}
		// The stand-in (CS_DCLF_PRIMARY_EXCLUDE) needs the sun's entry exclusion live (its cascades captured) and a current
		// snapshot; otherwise the engine culls every entry this frame, less what the leaf exclusion keeps from its
		// registration. A member's local shadow lights are the GPU's (LocalShadowLights), so local shadows need nothing of
		// the engine's cull.
		if (!ActiveToggles().excludePrimaryEntries) {
			if (!ownedFadeRoots.empty()) {
				ownedFadeRoots.clear();
				SceneStore::Get().SetFadeRootsOwned({});
				TreeAnimation::SetOwned({});
			}
			frameLive.store(true, std::memory_order_release);
			return;
		}
		const bool current = candidates && candidates->generation == SceneStore::Get().GetSunCandidatesGeneration();
		if (!SunAccumulation::Get().ExclusionLive() || !current) {
			++(current ? cutStats.skippedPreconditions : cutStats.skippedStale);
			liveStale = true;
			fadeSkipped = true;
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
			cut.mixed.assign(entries, 0);
			cut.walk.assign(entries, 1);
			cut.entrySlot.assign(entries, 0xFF);
			entrySlotsVersion.fetch_add(1, std::memory_order_relaxed);
			cut.eligible.clear();
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> admittedRoots;
			for (std::uint32_t e = 0; e < entries; ++e) {
				cut.memberOffsets[e] = static_cast<std::uint32_t>(cut.members.size());
				cut.switchOffsets[e] = static_cast<std::uint32_t>(cut.switches.size());
				cut.plans[e] = PlanOf(e, cut.roots[e]);
				cut.memberOffsets[e + 1] = static_cast<std::uint32_t>(cut.members.size());
				cut.switchOffsets[e + 1] = static_cast<std::uint32_t>(cut.switches.size());
				if (cut.plans[e] == EntryPlan::Rejected)
					continue;
				for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1]; ++m)
					cut.mixed[e] |= cut.members[m].engine ? 1 : 0;
				cut.eligible.emplace(cut.roots[e], e);
				if (const auto it = cut.admittedRoots.find(cut.roots[e]); it != cut.admittedRoots.end() && it->second == MemberSignature(e)) {
					cut.admitted[e] = 1;
					admittedRoots.emplace(cut.roots[e], it->second);
				}
			}
			cut.admittedRoots = std::move(admittedRoots);
			// Every entry not admitted yet is checked once: its members may all be drawn already (Admit).
			cut.pendingAdmission.clear();
			for (std::uint32_t e = 0; e < entries; ++e)
				if (cut.plans[e] != EntryPlan::Rejected && !cut.admitted[e])
					cut.pendingAdmission.push_back(e);
				// The members' object indices (stable while an object stays tracked; a change of membership is a new snapshot).
			cut.memberObject.resize(cut.members.size());
			for (std::size_t m = 0; m < cut.members.size(); ++m)
				cut.memberObject[m] = cut.members[m].engine ? -1 : SceneStore::Get().FindObject(cut.members[m].geometry);
			cut.switchEntry.clear();
			for (std::uint32_t e = 0; e < entries; ++e)
				for (std::uint32_t w = cut.switchOffsets[e]; w < cut.switchOffsets[e + 1]; ++w)
					cut.switchEntry.emplace(cut.switches[w], e);
			walkRefresh.clear();
			++cutVersion;
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
		standInLive = true;
		walkEverything = SwitchEnabled(Switch::PersistentParity);
		// Which entries the stand-in walks: a new snapshot's, from their members once memberLive is current; the entries a
		// member joined or left the set in since.
		if (newSnapshot) {
			for (std::uint32_t e = 0; e < cut.walk.size(); ++e)
				RefreshWalk(e);
		} else {
			for (const std::uint32_t e : std::exchange(walkRefresh, {}))
				RefreshWalk(e);
		}
		// The entries whose members are all in the set now are left out of the engine's cull from this frame on.
		RunAdmission();
		// The fade roots DCLF services: a new snapshot's, and every one again from its node after frames the engine culled
		// them all (its OnVisible ran on them meanwhile).
		if (newSnapshot)
			SyncFadeOwnership();
		else if (std::exchange(fadeSkipped, false))
			SceneStore::Get().ReseedOwnedFadeRoots();
		fadeSkipped = false;
		++cutStats.appliedFrames;
		cutStats.prepareTicks += Now() - start;
		// The list jobs are queued after this returns: their queueing orders everything above before their reads.
		frameLive.store(true, std::memory_order_release);
		gpuSunFrame = true;
	}

	namespace
	{
		/**
		 * @brief FadeStateCS's EngineInView on a block, on the CPU (the latch's frustum fallback aside: -1 when the block does
		 * not apply), with a description of the test that decided it.
		 */
		int VisibilityPort(const std::byte* a_block, const float a_centre[3], float a_radius, std::uint32_t a_flags, std::string& a_why)
		{
			const auto word = [&](std::size_t a_offset) { std::uint32_t v; std::memcpy(&v, a_block + a_offset, 4); return v; };
			const auto plane = [&](std::size_t a_offset) { std::array<float, 4> p; std::memcpy(p.data(), a_block + a_offset, 16); return p; };
			const std::uint32_t header[4]{ word(0), word(4), word(8), word(12) };
			if (!(header[0] & kFadeVisibilityValid))
				return -1;
			const bool alwaysDraw = (a_flags >> 11) & 1, preprocessed = (a_flags >> 12) & 1, hidden = (a_flags >> 20) & 1;
			if (a_radius == 0.0f && !alwaysDraw)
				return a_why = "zero radius", 0;
			const std::uint32_t cullMode = (header[0] >> kFadeVisibilityCullModeShift) & 0xFF;
			if (cullMode == 2)
				return a_why = "cull mode 2", 0;
			if (cullMode == 1 || alwaysDraw)
				return a_why = "always", 1;
			if (preprocessed && !(header[0] & kFadeVisibilityIgnorePreprocess))
				return a_why = "preprocessed", hidden ? 0 : 1;
			const auto d = [&](const std::array<float, 4>& p) { return ((p[1] * a_centre[1] + p[0] * a_centre[0]) + a_centre[2] * p[2]) - p[3]; };
			bool view = true;
			const std::uint32_t mask = word(kFadeVisibilityViewOffset + 96);
			for (std::uint32_t p = 0; p < 6 && view; ++p)
				if ((mask >> p) & 1)
					if (d(plane(kFadeVisibilityViewOffset + p * 16)) <= -a_radius)
						view = false, a_why = fmt::format("view plane {} (mask {:#x})", p, mask);
			if (!(header[0] & kFadeVisibilityCompound))
				return view ? (a_why = "view", 1) : 0;
			if (!(header[0] & kFadeVisibilitySkipView) && !view)
				return 0;
			std::uint32_t op = header[3];
			for (std::uint32_t step = 0; step < 512; ++step) {
				if (op >= header[1])
					return a_why = fmt::format("compound ran off at op {}", op), 1;
				const std::uint32_t type = word(kFadeVisibilityOpsOffset + op * 16), yes = word(kFadeVisibilityOpsOffset + op * 16 + 4), no = word(kFadeVisibilityOpsOffset + op * 16 + 8);
				if (type == 2)
					return a_why = fmt::format("compound accepted at op {}", op), 1;
				if (type == 3)
					return a_why = fmt::format("compound rejected at op {}", op), 0;
				bool result = false;
				if (type == 7 || type == 8) {
					const std::uint32_t set = op + 1 < header[1] ? word(kFadeVisibilityOpsOffset + (op + 1) * 16) : ~0u;
					if (set >= header[2])
						return a_why = fmt::format("compound set {} past {} at op {}", set, header[2], op), 1;
					const std::size_t base = kFadeVisibilitySetsOffset + std::size_t(set) * kFadeVisibilitySetBytes;
					const std::uint32_t setMask = word(base + 96);
					if (!setMask) {
						result = type == 7;
					} else {
						std::uint32_t p = 0;
						for (; p < 6; ++p) {
							if (!((setMask >> p) & 1))
								continue;
							const float distance = d(plane(base + p * 16));
							if (distance <= -a_radius)
								break;
							if (type == 8 && distance < a_radius)
								break;
						}
						result = type == 7 ? p == 6 : p != 6;
					}
				}
				op = result ? yes : no;
			}
			return a_why = "compound loop", 1;
		}
	}

	void PrimaryCull::NoteAnimatedFade(const void* a_node, const float* a_camera)
	{
		std::scoped_lock lock(animatedMutex);
		animatedNodes.push_back(a_node);
		// The inputs FUN_14147a160 reads: the camera FUN_1402cff60 passes (the world root camera's position and lodAdjust), and the
		// fade globals as they are now; the same for every call of one animation update.
		if (a_camera) {
			const AnimatedFadeInputs inputs{ { a_camera[0], a_camera[1], a_camera[2] }, a_camera[3], Global<std::int32_t>(kFadeFrameCounter), Global<float>(kFadeStepTime), 1u };
			if (!animatedInputs.valid)
				animatedInputs = inputs;
			else if (std::memcmp(&inputs, &animatedInputs, offsetof(AnimatedFadeInputs, valid)) != 0)
				animatedInputs.varied = 1;
		}
	}

	void PrimaryCull::TakeAnimatedBatch(std::vector<const void*>& a_nodes, AnimatedFadeInputs& a_inputs)
	{
		std::scoped_lock lock(animatedMutex);
		a_nodes.swap(animatedBatch);
		animatedBatch.clear();
		a_inputs = std::exchange(batchInputs, {});
	}

	int PrimaryCull::FadeVisibilityPort(const std::byte* a_block, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags, std::string& a_why)
	{
		return VisibilityPort(a_block, a_centre, a_radius, a_nodeFlags, a_why);
	}

	void PrimaryCull::SampleFadeVisibility(const RE::NiCullingProcess* a_process, std::uint32_t a_slot)
	{
		if (a_slot >= kFadeVisibilityLists)
			return;
		// BSCullingProcess (AE): cullMode +0x30198, compoundFrustum +0x301A0; NiCullingProcess: ignorePreprocess +0x11F.
		// BSCompoundFrustum: planes (BSTArray<NiFrustumPlanes>) +0x0, functionOperators (BSTArray of 12-byte records) +0x18,
		// freePlane +0xB8, freeOp +0xBC, firstOp +0xC0, skipViewFrustum +0xC4.
		auto* out = fadeVisibility.data() + std::size_t(a_slot) * kFadeVisibilityBytes;
		std::memset(out, 0, kFadeVisibilityBytes);
		const auto put = [&](std::size_t a_offset, const void* a_value, std::size_t a_bytes) { std::memcpy(out + a_offset, a_value, a_bytes); };
		const std::uint32_t cullMode = At<std::uint32_t>(a_process, 0x30198);
		std::uint32_t mode = kFadeVisibilityValid | (cullMode << kFadeVisibilityCullModeShift);
		if (At<std::uint8_t>(a_process, 0x11F) || cullMode == 4)
			mode |= kFadeVisibilityIgnorePreprocess;
		std::uint32_t header[4]{ mode, 0, 0, 0 };
		const auto* compound = At<const std::byte*>(a_process, 0x301A0);
		const std::int32_t freeOp = compound ? At<std::int32_t>(compound, 0xBC) : 0;
		if (compound && freeOp > 0 && cullMode != 3) {
			const auto* ops = At<const std::uint32_t*>(compound, 0x18);
			// The arrays whole: the program's records and plane sets are not all below freeOp and freePlane.
			const std::uint32_t opCount = At<std::uint32_t>(compound, 0x18 + 0x10);
			const auto* sets = At<const std::byte*>(compound, 0x0);
			const std::uint32_t setCount = At<std::uint32_t>(compound, 0x10);
			if (!ops || !sets || opCount > kFadeVisibilityOps || setCount > kFadeVisibilitySets) {
				++visibilityOverflows;
			} else {
				header[0] |= kFadeVisibilityCompound | (At<std::uint8_t>(compound, 0xC4) ? kFadeVisibilitySkipView : 0u);
				header[1] = opCount;
				header[2] = setCount;
				header[3] = At<std::uint32_t>(compound, 0xC0);
				for (std::uint32_t o = 0; o < opCount; ++o)
					put(kFadeVisibilityOpsOffset + o * 16, ops + o * 3, 12);
				for (std::uint32_t s = 0; s < setCount; ++s)
					put(kFadeVisibilitySetsOffset + s * kFadeVisibilitySetBytes, sets + s * 0x70, 0x64);
			}
		}
		// The view test's planes: the process's own, as its sphere test reads them.
		put(kFadeVisibilityViewOffset, reinterpret_cast<const std::byte*>(a_process) + 0x3C, 0x64);
		header[0] |= kFadeVisibilityViewPlanes;
		put(0, header, sizeof(header));
	}

	void PrimaryCull::FadeRootLists(const std::vector<const void*>& a_nodes, std::vector<std::uint32_t>& a_out) const
	{
		// A fade root is its entry's root or under it: the nearest ancestor the cut lists.
		a_out.assign(a_nodes.size(), kFadeRootNoList);
		for (std::size_t r = 0; r < a_nodes.size(); ++r) {
			for (const auto* node = static_cast<const RE::NiAVObject*>(a_nodes[r]); node; node = node->parent) {
				if (const auto it = cut.eligible.find(node); it != cut.eligible.end()) {
					if (it->second < cut.entrySlot.size() && cut.entrySlot[it->second] != 0xFF)
						a_out[r] = cut.entrySlot[it->second];
					break;
				}
			}
		}
	}

	bool PrimaryCull::Owned(const RE::BSGeometry& a_geometry) const
	{
		return frameSet && (frameSet->PhasesOf(&a_geometry) & kSetMain) != 0;
	}

	void PrimaryCull::NoteSetChanges(const std::vector<const RE::BSGeometry*>& a_joined, const std::vector<const RE::BSGeometry*>& a_left)
	{
		auto& store = SceneStore::Get();
		// A geometry that joins the set is no longer main-registered, so nothing would read and clear the bits a shadow light
		// wrote into its mask while it was the engine's: cleared once here, and kept 0 from now on by SunAccumulation
		// (ClearOwnedMask). Only tracked ones are touched, so they are alive.
		for (const auto* geometry : a_joined)
			if (store.IsTracked(geometry))
				if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get())
					if (void* lightData = At<void*>(property, kPropertyLightData))
						At<std::uint32_t>(lightData, kLightDataActiveMask) = 0;
		for (const auto* geometry : a_left)
			NoteMemberLost(geometry);
		if (!cut.candidates)
			return;
		// The entries of the geometries that joined (by pointer alone: nothing here is dereferenced).
		const auto& candidates = *cut.candidates;
		for (const auto* geometry : a_joined)
			if (const auto it = candidates.geometries.find(geometry); it != candidates.geometries.end() && it->second < candidates.geometryEntry.size()) {
				cut.pendingAdmission.push_back(candidates.geometryEntry[it->second]);
				walkRefresh.push_back(candidates.geometryEntry[it->second]);
			}
	}

	void PrimaryCull::RunAdmission()
	{
		auto& store = SceneStore::Get();
		if (!cut.candidates || cut.candidates->generation != store.GetSunCandidatesGeneration())
			return;
		bool admittedAny = false;
		for (const std::uint32_t e : cut.pendingAdmission) {
			if (e >= cut.admitted.size() || cut.admitted[e] || cut.plans[e] == EntryPlan::Rejected)
				continue;
			bool all = true;
			for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1] && all; ++m) {
				const auto* geometry = cut.members[m].geometry;
				// A member no longer tracked may be gone: the entry waits for the snapshot that follows.
				if (!store.IsTracked(geometry)) {
					all = false;
					break;
				}
				all = cut.members[m].engine || !MemberShown(m, cut.roots[e]) || MemberDrawable(cut.memberObject[m]);
			}
			if (all) {
				cut.admitted[e] = 1;
				cut.admittedRoots.insert_or_assign(cut.roots[e], MemberSignature(e));
				++cutStats.admittedNow;
				admittedAny = true;
			}
		}
		cut.pendingAdmission.clear();
		if (admittedAny) {
			SyncFadeOwnership();
			++cutVersion;
		}
	}

	void PrimaryCull::RefreshWalk(std::uint32_t a_e)
	{
		if (a_e >= cut.walk.size() || cut.plans[a_e] == EntryPlan::Rejected)
			return;
		// Engine-drawn members are handed to the registration every frame; a member shown but not bound is, until it is.
		bool walk = cut.mixed[a_e] != 0;
		auto& store = SceneStore::Get();
		for (std::uint32_t m = cut.memberOffsets[a_e]; m < cut.memberOffsets[a_e + 1] && !walk; ++m)
			if (!cut.members[m].engine && !MemberDrawable(cut.memberObject[m]) && store.IsTracked(cut.members[m].geometry) &&
				MemberShown(m, cut.roots[a_e]))
				walk = true;
		cutVersion += cut.walk[a_e] != (walk ? 1 : 0) ? 1 : 0;
		cut.walk[a_e] = walk ? 1 : 0;
	}

	void PrimaryCull::NoteMemberLost(const RE::BSGeometry* a_geometry)
	{
		if (!cut.candidates)
			return;
		const auto& candidates = *cut.candidates;
		if (const auto it = candidates.geometries.find(a_geometry); it != candidates.geometries.end() && it->second < candidates.geometryEntry.size()) {
			const std::uint32_t e = candidates.geometryEntry[it->second];
			if (e < cut.walk.size()) {
				cutVersion += cut.walk[e] ? 0 : 1;
				cut.walk[e] = 1;
				walkRefresh.push_back(e);
				// Out of this frame's lists: the member is handed to nobody until the next frame's filter keeps the root.
				if (const auto filter = listFilter.load(std::memory_order_acquire); filter && ListsFiltered() && filter->roots.contains(cut.roots[e]))
					++listStats.lostWhileOut;
			}
		}
	}


	bool PrimaryCull::MemberDrawable(std::int32_t a_object) const
	{
		return (SceneStore::Get().SetPhasesOf(a_object) & kSetMain) != 0;
	}

	std::uint32_t PrimaryCull::SampleMembershipWitness()
	{
		sampledWitness = MembershipWitness();
		sampledWitnessFrame = SceneStore::Get().GetFrame();
		return sampledWitness;
	}

	std::uint32_t PrimaryCull::FrameMembershipWitness() const
	{
		return sampledWitnessFrame == SceneStore::Get().GetFrame() ? sampledWitness : MembershipWitness();
	}

	void PrimaryCull::NoteAllMembersLost()
	{
		std::fill(cut.walk.begin(), cut.walk.end(), std::uint8_t(1));
		++cutVersion;
		for (std::uint32_t e = 0; e < cut.walk.size(); ++e)
			walkRefresh.push_back(e);
	}

	void PrimaryCull::SyncFadeOwnership()
	{
		// The admitted entries with a fade root, whose members' fade is FadeStateCS's from the next frame on. One with engine-drawn
		// parts is culled by the engine, which updates its node itself; every other is stood in (its state is the GPU's alone,
		// its node left as the engine last updated it), its kAccumulated cleared once (its trees are TreeWindCS's: the tree clock
		// need not advance them), and it is taken off the tree manager's animation list (TreeAnimation).
		ownedFadeRoots.clear();
		standInLodSkins = 0;
		std::vector<SceneStore::OwnedFadeRoot> owned;
		std::vector<const RE::NiAVObject*> animated;
		for (std::uint32_t e = 0; e < cut.plans.size(); ++e) {
			const auto plan = cut.plans[e];
			const bool fade = plan == EntryPlan::FadeRoot || plan == EntryPlan::LeafRoot || plan == EntryPlan::TreeRoot;
			if (!cut.admitted[e] || !fade)
				continue;
			ownedFadeRoots.push_back(cut.roots[e]);
			owned.push_back({ cut.roots[e], !cut.mixed[e] });
			if (!cut.mixed[e]) {
				std::atomic_ref<std::uint32_t>(At<std::uint32_t>(cut.roots[e], kObjectFlags)).fetch_and(~kFlagAccumulated, std::memory_order_relaxed);
				animated.push_back(cut.roots[e]);
				// A skin whose drawn partitions follow the node's LOD level (SceneStore::LodRowOf): read from a node that no
				// longer changes. Counted, for the report.
				for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1]; ++m)
					if (const auto* geometry = cut.members[m].geometry; geometry && geometry->GetFlags().any(RE::NiAVObject::Flag::kMeshLOD) &&
																	   geometry->GetGeometryRuntimeData().skinInstance)
						++standInLodSkins;
			}
		}
		SceneStore::Get().SetFadeRootsOwned(owned);
		TreeAnimation::SetOwned(animated);
	}

	std::uint64_t PrimaryCull::MemberSignature(std::uint32_t a_e) const
	{
		std::uint64_t signature = 0;
		for (std::uint32_t m = cut.memberOffsets[a_e]; m < cut.memberOffsets[a_e + 1]; ++m)
			if (!cut.members[m].engine)
				signature += ankerl::unordered_dense::hash<const RE::BSGeometry*>{}(cut.members[m].geometry);
		return signature;
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
		// The compound frustum (portals, occlusion planes) exists only while a list job culls through it, and each list's
		// process has its own: each job's first call samples its cull test for FadeStateCS (read by the render thread after
		// the jobs' Finish).
		if (const std::uint32_t bit = a_slot >= 0 && a_slot < static_cast<int>(kFadeVisibilityLists) ? 1u << a_slot : 0u;
			bit && !(sampledSlots.load(std::memory_order_relaxed) & bit) && !(sampledSlots.fetch_or(bit, std::memory_order_acq_rel) & bit))
			SampleFadeVisibility(a_process, static_cast<std::uint32_t>(a_slot));
		if (!standInLive)
			return false;
		const auto it = cut.eligible.find(a_object);
		if (it == cut.eligible.end())
			return false;
		const std::uint32_t e = it->second;
		// The list that culls the entry, whose block its fade root is tested against (one job per entry and frame).
		if (cut.entrySlot[e] != static_cast<std::uint8_t>(a_slot)) {
			cut.entrySlot[e] = static_cast<std::uint8_t>(a_slot);
			entrySlotsVersion.fetch_add(1, std::memory_order_relaxed);
		}
		const auto plan = cut.plans[e];
		auto& out = jobOut[a_slot];
		++out.seen;
		// A parity frame's dry run of the list filter: a root it would have left out must be one the stand-in does nothing
		// for (admitted, nothing to walk); the member walk below counts any geometry it hands over all the same.
		const bool dry = parityFilter && parityFilter->roots.contains(a_object);
		if (dry) {
			++out.filterChecked;
			out.filterMissed += !cut.admitted[e] || cut.walk[e] ? 1 : 0;
		}
		const bool fadeRoot = plan == EntryPlan::FadeRoot || plan == EntryPlan::LeafRoot || plan == EntryPlan::TreeRoot;
		// A fade, and a LOD cross-fade, are the feedback's: its fade service steps them, and a faded-out root draws nothing
		// (FadeStateCS). A cross-fade's hint-10 copy is not drawn (drawcall-limit-fix.md, "LOD cross-fades": none occur
		// in SE's settings); crossing roots are counted, for the report.
		if (fadeRoot && (At<std::uint8_t>(a_object, 0x153) & 0x70) != 0x20)
			++out.notSettled;
		if (!cut.admitted[e]) {
			// The engine culls it this frame (less what the leaf exclusion keeps from its registration); Admit decides its admission.
			EngineProcess1(a_process, a_object, a_arg);
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kAccumulated))
				out.pending.push_back(e);
			++out.notAdmitted;
			return true;
		}
		// A switch whose selected child is out of date: NiSwitchNode::OnVisible brings it up to date before culling it
		// (UpdateDownwardPass). With the switch events that is done when the selection changes (SceneStore::CatchUpSwitch),
		// so none is found here; without them, the engine culls this entry this frame.
		if (!cut.liveEvents) {
			for (std::uint32_t w = cut.switchOffsets[e]; w < cut.switchOffsets[e + 1]; ++w) {
				const auto* switchNode = cut.switches[w];
				if (const auto selection = SceneStore::SelectionOf(*switchNode, SceneStore::ReadSwitch(*switchNode)); selection.child && !selection.current) {
					++out.switchStale;
					EngineProcess1(a_process, a_object, a_arg);
					return true;
				}
			}
		}
		// Engine-drawn parts: the engine culls the entry whole, and runs its OnVisible on the node they are drawn from; DCLF's
		// members are kept from the registration by the AppendVirtual hook, and FadeStateCS runs the same update for them.
		if (cut.mixed[e]) {
			const float metricBefore = fadeRoot ? At<float>(a_object, 0x144) : 0.0f;
			const std::int32_t visibleBefore = fadeRoot ? At<std::int32_t>(a_object, kLastVisibleFrame) : 0;
			EngineProcess1(a_process, a_object, a_arg);
			++out.mixed;
			// CS_DCLF_FADE_PARITY: the engine's verdict (its OnVisible ran: the node's metric or last visible frame changed) against
			// FadeStateCS's test on this job's block.
			// (A settled node, its fades at 1, returns before updating anything: nothing tells.)
			const bool settled = fadeRoot && (a_object->GetFlags().underlying() & kFlagFadeSettled) && At<float>(a_object, kFadeAmount) == 1.0f &&
			                     At<float>(a_object, kCurrentFade) == 1.0f;
			if (fadeRoot && !settled && a_slot >= 0 && a_slot < static_cast<int>(kFadeVisibilityLists) && SwitchEnabled(Switch::FadeParity)) {
				const bool engine = At<float>(a_object, 0x144) != metricBefore || At<std::int32_t>(a_object, kLastVisibleFrame) != visibleBefore;
				const float centre[3]{ a_object->worldBound.center.x, a_object->worldBound.center.y, a_object->worldBound.center.z };
				std::string why;
				const int port = VisibilityPort(fadeVisibility.data() + std::size_t(a_slot) * kFadeVisibilityBytes, centre, a_object->worldBound.radius,
					a_object->GetFlags().underlying(), why);
				if (port >= 0) {
					++out.visibilityChecked;
					if ((port != 0) != engine && out.visibilityDiffer++ == 0)
						out.visibilityFirst = fmt::format("'{}' slot {}: the engine {}, the port {} ({}); centre ({:.0f} {:.0f} {:.0f}) r {:.0f}, flags {:#x}",
							a_object->name.c_str() ? a_object->name.c_str() : "", a_slot, engine ? "visible" : "culled", port ? "visible" : "culled", why, centre[0], centre[1],
							centre[2], a_object->worldBound.radius, a_object->GetFlags().underlying());
				}
			}
			return true;
		}
		++out.skipped;
		// An entry with nothing for the registration (Cut::walk): the GPU culls and draws its members, and their fade is
		// FadeStateCS's. Under CS_DCLF_PERSISTENT_PARITY every entry is walked, for the light mask check.
		if (!cut.walk[e] && !walkEverything)
			return true;
		++out.walked;
		// The root's state (fade, LOD) is FadeStateCS's; what is left here is what the frame draws, by the cull's
		// test against the job's own planes (its Process2 set them up from the list's first entry). A tree above the height
		// limit draws nothing.
		if (plan == EntryPlan::TreeRoot && TreeAboveLimit(a_object, *a_process))
			return true;
		if (Outside(a_process->planes, a_object->worldBound))
			return true;
		++out.visibleEntries;
		for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1]; ++m) {
			const auto& member = cut.members[m];
			const auto* geometry = member.geometry;
			const bool selected = cut.liveEvents ? cut.memberLive[m] != 0 : PathSelected(member);
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
			// In the frame's set: drawn from its record whenever the GPU finds it. A member not in it (not ready, or its binding taken
			// again this frame) is the engine's to register.
			if (!member.engine && MemberDrawable(cut.memberObject[m])) {
				out.visible.push_back(geometry);
				continue;
			}
			if (!member.engine && SceneStore::Get().IsMember(cut.memberObject[m]))
				++out.undrawable;
			// The engine's, or DCLF's but not bound yet: what its cull does for a visible geometry, the test of its bound,
			// then the append to this process (BSGeometry::OnVisible), in the traversal's order, for the registration jobs.
			if (!Outside(a_process->planes, geometry->worldBound)) {
				a_process->AppendVirtual(*const_cast<RE::BSGeometry*>(geometry), a_arg);
				++(member.engine ? out.engineMembers : out.unbound);
				// An entry only the parity walks (Cut::walk clear): normally nothing would have handed this one over.
				if (!cut.walk[e])
					++out.walkMissed;
				out.filterMissed += dry ? 1 : 0;
			}
		}
		return true;
	}

	void PrimaryCull::AfterListJobs()
	{
		// The animation job's updates since the last list jobs: complete now (the job runs between two culls), this frame's batch.
		{
			std::scoped_lock lock(animatedMutex);
			animatedBatch.swap(animatedNodes);
			animatedNodes.clear();
			batchInputs = std::exchange(animatedInputs, {});
		}
		// CS_DCLF_FADE_PARITY: the fade roots' nodes now, after this cull's OnVisible and before the next animation job's updates.
		if (SwitchEnabled(Switch::FadeParity) && (SceneStore::Get().GetFrame() % 30) == 0) {
			const auto& roots = SceneStore::Get().GetTables().fadeRootNode;
			nodeSnapshot.resize(roots.size());
			for (std::size_t r = 0; r < roots.size(); ++r)
				nodeSnapshot[r] = roots[r] ? FadeState::ReadNode(*static_cast<const RE::NiAVObject*>(roots[r])) : FadeNodeState{};
			nodeSnapshotFrame = SceneStore::Get().GetFrame();
		}
		// The cull tests FadeStateCS repeats, per list process: a job that made no stand-in call this frame, the process's own.
		{
			const std::uint32_t sampled = sampledSlots.exchange(0, std::memory_order_acq_rel);
			auto** processes = Global<RE::NiCullingProcess**>(kListProcesses);
			const std::uint32_t count = processes ? std::min(Global<std::uint32_t>(kSceneListCount), kFadeVisibilityLists) : 0u;
			for (std::uint32_t i = 0; i < count; ++i)
				if (!(sampled & (1u << i)) && processes[i])
					SampleFadeVisibility(processes[i], i);
			fadeVisibilityBlocks = count;
		}
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
			s.walked += out.walked;
			s.walkMissed += out.walkMissed;
			s.notAdmitted += out.notAdmitted;
			s.hiddenSkipped += out.hidden;
			s.engineMembers += out.engineMembers;
			s.unbound += out.unbound;
			s.undrawable += out.undrawable;
			s.excluded += out.excluded;
			s.filterChecked += out.filterChecked;
			s.filterMissed += out.filterMissed;
			s.mixed += out.mixed;
			s.visibilityChecked += out.visibilityChecked;
			s.visibilityDiffer += out.visibilityDiffer;
			if (s.visibilityFirst.empty() && !out.visibilityFirst.empty())
				s.visibilityFirst = out.visibilityFirst;
			s.switchStale += out.switchStale;
			s.unselected += out.unselected;
		}
		cutStats.members += frameVisible.size();
		// The members' activeLightMask needs no clear: the shadow lights' registrations leave an owned geometry's 0, as its main
		// registration would have (SunAccumulation, ClearOwnedMask); CheckLightMasks counts the exceptions.
		frameSunBits = sunBits;
		cutStats.afterTicks += Now() - start;
	}

	void PrimaryCull::CheckLightMasks()
	{
		// CS_DCLF_PERSISTENT_PARITY: the owned members in view whose mask is not 0, by the sun's and other lights' bits. After
		// the registration jobs: a property an engine-drawn geometry shares holds that one's bits until its main registration.
		if (SwitchEnabled(Switch::PersistentParity))
			for (const auto* geometry : frameVisible)
				if (Owned(*geometry))
					if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get())
						if (void* lightData = At<void*>(property, kPropertyLightData)) {
							const auto mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
							++cutStats.maskChecked;
							cutStats.maskSun += (mask & frameSunBits) ? 1 : 0;
							cutStats.maskOther += (mask & ~frameSunBits) ? 1 : 0;
							if (mask && maskFirst.empty())
								maskFirst = fmt::format("'{}' mask {:#x} (sun {:#x}){}", geometry->name.c_str() ? geometry->name.c_str() : "?", mask, frameSunBits, "");
						}
	}

	void PrimaryCull::AfterFullFrustum()
	{
		ZoneScopedN("CS.DCLF.AfterFullFrustum");
		frameLive.store(false, std::memory_order_relaxed);
		gpuSunFrame = false;
		if (ListsFiltered() && !SunAccumulation::Get().ExclusionLive())
			++listStats.unexcluded;
		// CS_DCLF_PARITY_BOTH: the engine culls and registers everything.
		if (!PassCapture::ParityBoth())
			PrepareFrame();
		CheckFadePort();
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

	void PrimaryCull::CheckFadePort()
	{
		// CS_DCLF_FADE_PARITY: the port against the engine's functions on copies of the snapshot's fade roots, after the
		// feedback's join (nothing writes them until the list jobs).
		if (!standInLive || (frameCounter % 30) != 0 || !SwitchEnabled(Switch::FadeParity))
			return;
		auto** processes = Global<RE::NiCullingProcess**>(kListProcesses);
		const auto* camera = processes && cut.processCount ? processes[0]->camera : nullptr;
		if (!camera)
			return;
		std::uint32_t checked = 0;
		const auto entries = static_cast<std::uint32_t>(cut.roots.size());
		for (std::uint32_t n = 0; n < entries && checked < 64; ++n) {
			const std::uint32_t e = (fadePortCursor + n) % entries;
			const auto plan = cut.plans[e];
			if (plan != EntryPlan::FadeRoot && plan != EntryPlan::LeafRoot && plan != EntryPlan::TreeRoot)
				continue;
			FadeState::CheckPort(*cut.roots[e], *camera, fadePort);
			++checked;
		}
		fadePortCursor = entries ? (fadePortCursor + 64) % entries : 0;
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

	std::uint32_t PrimaryCull::SunShadowBits(const RE::BSGeometry& a_geometry, const RE::BSLightingShaderProperty* a_property)
	{
		const auto inCascades = SunAccumulation::Get().InSunCascades(a_geometry.worldBound);
		if (!inCascades)
			return ~0u;
		// Out of every cascade it loses ShadowDir only: a local shadow light may still give it DefShadow (the draw decides).
		return *inCascades ? SunShadowStatic(a_geometry, a_property) : SunShadowStatic(a_geometry, a_property) & ~0x2000u;
	}

	std::uint32_t PrimaryCull::SunShadowStatic(const RE::BSGeometry& a_geometry, const RE::BSLightingShaderProperty* a_property)
	{
		// The settled state's, as the synthetic pass is: a member's fade is the GPU's (and a stood-in root's node is not it).
		return StaticShadowBits(a_geometry, true, a_property);
	}

	bool PrimaryCull::SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out, bool a_sunOnGpu,
		const RE::BSLightingShaderProperty* a_layer)
	{
		if (a_derivedPass == kNotDerived)
			return false;
		const RE::BSShaderProperty* property = a_layer ? a_layer : a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = a_layer ? a_layer : netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		if (!lighting)
			return false;
		const std::uint64_t flags = lighting->flags.underlying();
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
		const auto* alphaProperty = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		const bool blended = alphaProperty && (alphaProperty->alphaFlags & 1);
		// The object's settled state, not its current fade: GetRenderPasses (1414adfb0) draws a screen-door fade of an
		// unblended, fully opaque material as the plain opaque pass with alpha = materialAlpha, and a member's fade is the
		// GPU's (FadeStateCS). Translucent objects take hints 1 and 9 (blended, sorted); not modelled.
		const bool translucent = (material ? material->materialAlpha : 1.0f) < 1.0f || blended;
		// On the GPU (a_sunOnGpu): the bits the object takes inside a cascade, and BuildDraws drops them on a miss.
		// A layer takes no shadow bits (measured: its passes never carry ShadowDir or DefShadow): its property's light mask never
		// names the sun, the registrations writing masks on the main property alone (FUN_1414b2140).
		const std::uint32_t sun = a_layer ? 0u : a_sunOnGpu ? SunShadowStatic(a_geometry) : SunShadowBits(a_geometry);
		if (sun == ~0u)
			return false;
		std::uint32_t hint = 0;
		if (a_layer) {
			// Every pass of a layer: hint 12 (FUN_1414b2330). A translucent one is not modelled.
			if (translucent)
				return false;
			hint = kLayerHint;
		} else if (flags & 0xc000000ull)
			hint = translucent ? 3 : 2;  // the decal groups
		else if (translucent)
			return false;
		else if (flags & 0x8000000600000000ull)
			hint = (~static_cast<std::uint32_t>(flags >> 33) & 1u) | 6u;
		else if (flags & (1ull << 61))
			hint = 11;  // TreeAnim
		else if (!(sun & 0x2000u))
			hint = 15;  // opaque with no sun shadow work
		a_out = {};
		a_out.subPass = PassCapture::SubPassOf(&a_geometry, flags);
		a_out.technique = DrawnPassDescriptor((a_derivedPass & ~kShadowBits) | sun, a_out.subPass);
		a_out.passEnum = a_out.technique + 0x4800002Du;
		a_out.hint = hint;
		a_out.lodRow = SceneStore::LodRowOf(a_geometry, property);
		a_out.sunTest = a_sunOnGpu && (sun & 0x2000u) != 0;
		return true;
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

		/** @brief The list processes' AppendVirtual (vtable slot 0x18): an owned geometry is not handed to the registration. */
		struct AppendVirtual
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::BSGeometry& a_geometry, std::int32_t a_arg)
			{
				auto& self = PrimaryCull::Get();
				if (self.frameLive.load(std::memory_order_acquire))
					if (const int slot = self.SlotOf(a_process); slot >= 0 && self.Owned(a_geometry)) {
						auto& out = self.jobOut[slot];
						out.visible.push_back(&a_geometry);
						++out.excluded;
						return;
					}
				func(a_process, a_geometry, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FUN_1402cff60's call of the fade update (FUN_14147a160): the animation job's update of an animated reference's fade
		 * node while the cull did not reach it (kAccumulated clear), from the main camera, outside any OnVisible.
		 */
		struct AnimatedFade
		{
			static void thunk(void* a_node, float a_amount, const float* a_camera)
			{
				PrimaryCull::Get().NoteAnimatedFade(a_node, a_camera);
				func(a_node, a_amount, a_camera);
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
		if (installed)
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
		if (CallsTo(base + kAnimatedFadeCallSite, base + kFadeUpdate))
			stl::write_thunk_call<Hooks::AnimatedFade>(base + kAnimatedFadeCallSite);
		else
			logger::warn("[DCLF] primary cull: the animation job's fade update is not where expected");
		stl::write_vfunc<0x16, Hooks::Process1>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		stl::write_vfunc<0x18, Hooks::AppendVirtual>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		InstallSceneLists();
		TreeAnimation::Install();
		LocalLightCull::Install();
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
						 "{:.0f} stood in for ({:.0f} walked, {:.0f} of them in view), {:.1f} cross-fading LOD (stood in), {:.1f} not yet admitted ({:.1f} admitted); {:.0f} members in view, {:.1f} not bound yet (the engine's), "
						 "{:.1f} bound but not in the set (the engine's), {:.1f} hidden;  {:.1f} owned geometries left out of the engine's registration; "
						 "{:.0f} of the engine's members in view registered by it; switches: {:.1f} entries culled by the engine (stale child), {:.0f} members unselected, selection read from every switch on {} frames and from {} events' entries; render thread: prepare {:.3f} ms, after the jobs {:.3f} ms",
				s.appliedFrames, s.frames, s.skippedStale, s.skippedPreconditions, s.seen / applied, s.skipped / applied, s.walked / applied, s.visibleEntries / applied,
				s.notSettled / applied, s.notAdmitted / applied, s.admittedNow / applied, s.members / applied, s.unbound / applied,
				s.undrawable / applied, s.hiddenSkipped / applied, s.excluded / applied,
				s.engineMembers / applied, s.switchStale / applied, s.unselected / applied, s.liveAll, s.liveEntries, s.prepareTicks * toMs / applied, s.afterTicks * toMs / applied);
			{
				const auto r = SceneStore::Get().TakeResidentStats();
				const double rf = std::max<double>(static_cast<double>(r.frames), 1.0);
				logger::info("[DCLF] scene membership: {:.0f} objects bound a frame ({} frames); {} records queued, {} joined, {} failed ({} the engine's pass, {} no record, {} a frame verdict, {} material or extras), {} rewritten ({} kept their binding), {} released, {} layers or bases unpaired; {} registrations of eligible objects not bound{}{}",
					r.resident / rf, r.frames, r.membershipQueued, r.joined, r.failed, r.failedBy[0], r.failedBy[1], r.failedBy[2], r.failedBy[3], r.rewritten, r.membershipKept, r.released,
					r.layerUnpaired, r.registeredUnbound, r.registeredUnboundFirst.empty() ? "" : ", first ", r.registeredUnboundFirst);
				if (r.parityChecks)
					logger::info("[DCLF] resident parity: {} checks, {} records compared, {} passes differ, {} records differ ({} not compared: the root fading, leaving at the next decode){}",
						r.parityChecks, r.parityChecked, r.parityPass, r.parityRecord, r.parityPending, r.parityPass || r.parityRecord ? " <- RESIDENT PARITY" : " <- OK");
			}
			ReportSceneLists(s.filterChecked, s.filterMissed);
			if (TreeAnimation::Installed() && SwitchEnabled(Switch::PersistentParity))
				TreeAnimation::CheckParity();
			logger::info("{}", TreeAnimation::Report());
			for (const auto& line : std::views::split(LocalLightCull::Report(), '\n'))
				if (const std::string text(line.begin(), line.end()); !text.empty())
					logger::info("{}", text);
			if (walkEverything)
				logger::info("[DCLF] stand-in walk parity: {} geometries handed to the registration from entries the stand-in would not walk{}", s.walkMissed,
					s.walkMissed ? " <- STAND-IN WALK" : " <- OK");
			if (s.maskChecked)
				logger::info("[DCLF] light masks (owned members in view, which no main registration clears): {} checked, {} with the sun's bits, {} with other lights' bits{}{}",
					s.maskChecked, s.maskSun, s.maskOther, s.maskSun || s.maskOther ? " <- LIGHT MASKS" : " <- OK", maskFirst.empty() ? "" : "; first: " + std::exchange(maskFirst, {}));
			const double mixedPerFrame = s.mixed / applied;
			if (s.visibilityChecked)
				logger::info("[DCLF] fade visibility parity (the engine's cull of entries with engine-drawn parts against FadeStateCS's test): {} checked, {} differ{}{}",
					s.visibilityChecked, s.visibilityDiffer, s.visibilityDiffer ? " <- FADE VISIBILITY" : " <- OK", s.visibilityFirst.empty() ? "" : "; first: " + s.visibilityFirst);

			cutStats = {};
			if (fadePort.checked) {
				logger::info("[DCLF] fade port parity (the port against the engine's functions on node copies): {} roots, {} differ{}{}", fadePort.checked, fadePort.differ,
					fadePort.differ ? " <- FADE PORT" : " <- OK", fadePort.first.empty() ? "" : "; first: " + fadePort.first);
				fadePort = {};
			}
			logger::info("[DCLF] fade roots: {} stood in ({} of their members skins with LOD levels{}); {:.0f} entries with engine-drawn parts culled by the engine a frame; compound frustum larger than the fade test's block on {} frames",
				SceneStore::Get().StoodInFadeRoots(), standInLodSkins, standInLodSkins ? " <- LOD SKINS" : "", mixedPerFrame, std::exchange(visibilityOverflows, 0));
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
	}
}
