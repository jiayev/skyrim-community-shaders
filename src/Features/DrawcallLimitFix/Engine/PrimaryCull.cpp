#include "PrimaryCull.h"
#include "PortalViews.h"

#include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"
#include "PassCapture.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#include "EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/FrameGlobals.h"
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
		constexpr std::uintptr_t kPortalWalkCallSite = 0x64685f;  // Main::Update -> FUN_1401a43c0, the main camera's portal walk
		constexpr std::uintptr_t kPortalWalk = 0x1a43c0;
		constexpr std::uintptr_t kSunOff = 0x338c911;  // set: the sun does not draw (no full-frustum cull, no cascades)
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

	}

	PrimaryCull& PrimaryCull::Get()
	{
		static PrimaryCull instance;
		return instance;
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
		// The frame's globals (step 6e F2: the scene work reads the frame's capture, never the engine's).
		const auto& g = FrameGlobals::Current();
		// No fade update at all, or no distance term in the fade value (FUN_14147b110 returns 1).
		if (!g.fadesOn || !g.fadeLodUpdates)
			return 0.0f;
		const std::uint32_t type = At<std::uint8_t>(a_root, 0x153) & 0xF;
		// Type 8 fades gradually even from out of view (FUN_14147a160), and OnVisible's type-6 branch never fades out
		// (ServiceFade): the engine draws either on the frame it comes into view, as the member is.
		if (type == 8 || (type == 6 && g.fadeSpecialA == g.fadeSpecialB))
			return 0.0f;
		const float mult = g.fadeDistanceMult;
		const float nearDistance = mult * At<float>(a_root, kFadeNear);
		const float farDistance = mult * At<float>(a_root, kFadeFar);
		if (!(farDistance > nearDistance))
			return 0.0f;  // the fade value never falls below 1
		// The fade value 1 - (x - near) / (far - near), x the scaled distance, reaches the threshold at x = limit.
		const float limit = nearDistance + (1.0f - g.fadeOutThreshold) * (farDistance - nearDistance);
		const float divisor = g.fadeTypeDivisors[type];
		// x = distance * lodFactor / divisor, or distance * the default scale when the divisor is not positive.
		const float distance = divisor > 0.0f ? limit * divisor : -(limit / g.fadeDefaultScale);
		return std::isfinite(distance) ? distance : 0.0f;
	}

	std::uint32_t PrimaryCull::MembershipWitness(const FrameGlobals& a_g)
	{
		std::uint32_t witness = 2166136261u;
		const auto mix = [&](std::uint32_t a_value) { witness = (witness ^ a_value) * 16777619u; };
		// The static sun bits (SunShadowStatic).
		mix(a_g.noSunShadowDir | (a_g.accumulator && a_g.accumulatorDeferredShadow ? 2u : 0u) | (a_g.screenDoorFades ? 4u : 0u));
		// The fade distances (FadeDistanceOf).
		mix(a_g.fadesOn | (a_g.fadeLodUpdates << 8) | (a_g.fadeSpecialA == a_g.fadeSpecialB ? 0x10000u : 0u));
		mix(std::bit_cast<std::uint32_t>(a_g.fadeDistanceMult));
		mix(std::bit_cast<std::uint32_t>(a_g.fadeOutThreshold));
		mix(std::bit_cast<std::uint32_t>(a_g.fadeDefaultScale));
		for (std::uint32_t type = 0; type < 13; ++type)
			mix(std::bit_cast<std::uint32_t>(a_g.fadeTypeDivisors[type]));
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
		for (std::uint32_t m = cut.memberBegin[a_e]; m < cut.memberEnd[a_e]; ++m)
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
			out = JobOut{ std::move(out.visible) };
			out.visible.clear();
		}
		// The stand-in (CS_DCLF_PRIMARY_EXCLUDE) needs a current snapshot, and the sun's entry exclusion live (its cascades
		// captured) unless the sun does not draw this frame (T5b: an interior, no cascades to take an entry out of); otherwise the
		// engine culls every entry this frame, less what the leaf exclusion keeps from its registration. A member's local shadow lights are the GPU's (LocalShadowLights), so local shadows need nothing of
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
		if (!candidates || !(SunAccumulation::Get().ExclusionLive() || Global<std::uint8_t>(kSunOff))) {
			++(candidates ? cutStats.skippedPreconditions : cutStats.skippedStale);
			liveStale = true;
			fadeSkipped = true;
			frameLive.store(true, std::memory_order_release);
			return;
		}
		const std::int64_t start = Now();
		// The cut follows the frame's snapshot entry by entry: the indices whose versions moved since the one it followed are planned
		// again (SyncCut), every other entry stands.
		const bool newSnapshot = cut.candidates != candidates;
		std::vector<std::uint32_t> changedEntries;
		if (newSnapshot) {
			ChangedEntries(cut.candidates.get(), *candidates, changedEntries);
			SyncCut(candidates, changedEntries);
		}
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(SceneStore::Get().GetFrame(), 11))
			CheckCut();
		// Which members their switches select: the switch events name the entries to read again (dclf-cull-job-elimination.md,
		// "Phase 3"); a new snapshot, a resync or a skipped frame reads them all.
		cut.liveEvents = SceneStore::SwitchEventsLive();
		if (cut.liveEvents) {
			if (liveStale) {
				std::fill(cut.memberLive.begin(), cut.memberLive.end(), std::uint8_t(1));
				for (std::uint32_t e = 0; e < cut.switchBegin.size(); ++e)
					if (cut.switchBegin[e] != cut.switchEnd[e])
						RefreshLive(e);
				++cutStats.liveAll;
			} else {
				// The entries planned again read their switches when planned (SyncCut).
				for (const auto* node : switchChanges)
					if (const auto it = cut.switchEntry.find(node); it != cut.switchEntry.end()) {
						RefreshLive(it->second);
						++cutStats.liveEntries;
					}
			}
			liveStale = false;
		}
		cutStats.entriesPlanned += changedEntries.size();
		standInLive = true;
		walkEverything = SwitchEnabled(Switch::PersistentParity);
		// Which entries the stand-in walks: the entries planned again (SyncCut queues them), from their members once memberLive is
		// current; the entries a member joined or left the set in since.
		for (const std::uint32_t e : std::exchange(walkRefresh, {}))
			RefreshWalk(e);
		// The entries whose members are all in the set now are left out of the engine's cull from this frame on.
		RunAdmission();
		// The fade roots DCLF services: a new snapshot's, and every one again from its node after frames the engine culled
		// them all (its OnVisible ran on them meanwhile).
		if (!changedEntries.empty())
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

	void PrimaryCull::SyncCut(const std::shared_ptr<const SunCandidates>& a_candidates, std::span<const std::uint32_t> a_changed)
	{
		cut.candidates = a_candidates;
		const auto& candidates = *a_candidates;
		const std::uint32_t capacity = candidates.Capacity();
		const std::size_t span = std::max<std::size_t>(capacity, cut.roots.size());
		auto grow = [span](auto& a_column, auto a_value) {
			if (a_column.size() < span)
				a_column.resize(span, a_value);
		};
		grow(cut.roots, static_cast<const RE::NiAVObject*>(nullptr));
		grow(cut.plans, EntryPlan::Rejected);
		grow(cut.memberBegin, 0u);
		grow(cut.memberEnd, 0u);
		grow(cut.switchBegin, 0u);
		grow(cut.switchEnd, 0u);
		grow(cut.admitted, std::uint8_t(0));
		grow(cut.mixed, std::uint8_t(0));
		grow(cut.walk, std::uint8_t(0));
		for (const std::uint32_t e : a_changed) {
			// The entry as the cut had it, let go: its root's verdicts, its switches; its members' ranges become garbage.
			if (const auto* old = cut.roots[e]) {
				if (const auto it = cut.eligible.find(old); it != cut.eligible.end() && it->second == e)
					cut.eligible.erase(it);
				for (std::uint32_t w = cut.switchBegin[e]; w < cut.switchEnd[e]; ++w)
					if (const auto it = cut.switchEntry.find(cut.switches[w]); it != cut.switchEntry.end() && it->second == e)
						cut.switchEntry.erase(it);
				if (e >= capacity || candidates.entryNodes[e] != old)
					cut.admittedRoots.erase(old);
			}
			cutGarbage += cut.memberEnd[e] - cut.memberBegin[e];
			cut.memberBegin[e] = cut.memberEnd[e] = static_cast<std::uint32_t>(cut.members.size());
			cut.switchBegin[e] = cut.switchEnd[e] = static_cast<std::uint32_t>(cut.switches.size());
			cut.plans[e] = EntryPlan::Rejected;
			cut.admitted[e] = cut.mixed[e] = cut.walk[e] = 0;
			const auto* root = e < capacity ? candidates.entryNodes[e] : nullptr;
			cut.roots[e] = root;
			if (!root)
				continue;
			// Planned again: its members and switches appended (PlanOf), their objects, whether its switches select them.
			cut.plans[e] = PlanOf(e, root);
			cut.memberEnd[e] = static_cast<std::uint32_t>(cut.members.size());
			cut.switchEnd[e] = static_cast<std::uint32_t>(cut.switches.size());
			if (cut.plans[e] == EntryPlan::Rejected)
				continue;
			cut.memberObject.resize(cut.members.size(), -1);
			cut.memberLive.resize(cut.members.size(), 1);
			for (std::uint32_t m = cut.memberBegin[e]; m < cut.memberEnd[e]; ++m) {
				cut.mixed[e] |= cut.members[m].engine ? 1 : 0;
				// The object the snapshot's walk had (step 6c: the frame reads no tracked set).
				const auto it = cut.members[m].engine ? candidates.geometries.end() : candidates.geometries.find(cut.members[m].geometry);
				cut.memberObject[m] = it != candidates.geometries.end() && it->second < candidates.geometrySlot.size() ? candidates.geometrySlot[it->second] : -1;
			}
			for (std::uint32_t w = cut.switchBegin[e]; w < cut.switchEnd[e]; ++w)
				cut.switchEntry.insert_or_assign(cut.switches[w], e);
			RefreshLive(e);
			cut.eligible.insert_or_assign(root, e);
			// Admitted again with the same DCLF members it was admitted with; checked for admission otherwise (Admit).
			if (const auto it = cut.admittedRoots.find(root); it != cut.admittedRoots.end() && it->second == MemberSignature(e))
				cut.admitted[e] = 1;
			else
				cut.pendingAdmission.push_back(e);
			cut.walk[e] = 1;
			walkRefresh.push_back(e);
		}
		for (auto* column : { &cut.roots })
			column->resize(capacity);
		cut.plans.resize(capacity);
		cut.memberBegin.resize(capacity);
		cut.memberEnd.resize(capacity);
		cut.switchBegin.resize(capacity);
		cut.switchEnd.resize(capacity);
		cut.admitted.resize(capacity);
		cut.mixed.resize(capacity);
		cut.walk.resize(capacity);
		std::erase_if(cut.pendingAdmission, [capacity](std::uint32_t a_e) { return a_e >= capacity; });
		std::erase_if(walkRefresh, [capacity](std::uint32_t a_e) { return a_e >= capacity; });
		// The members' pool gathered again once more of it is garbage than live (each live member copied once per as many let go).
		if (cutGarbage > cut.members.size() / 2)
			CompactCut();
		++cutVersion;
	}

	void PrimaryCull::CheckCut()
	{
		// CS_DCLF_PERSISTENT_PARITY: the cut as SyncCut kept it against one planned whole for the same snapshot, entry by entry: the
		// plan, the members (their geometries, engine-drawn or not, objects, switch paths), the switches, mixed, the eligible roots.
		// Admission and the walk are history (the set's joins since), not compared.
		if (!cut.candidates)
			return;
		Cut kept = std::move(cut);
		const auto keptWalkRefresh = walkRefresh;
		const auto keptGarbage = cutGarbage;
		const auto keptVersion = cutVersion;
		cut = Cut{};
		cut.processes = kept.processes;
		cut.processCount = kept.processCount;
		cut.admittedRoots = kept.admittedRoots;
		std::vector<std::uint32_t> all;
		ChangedEntries(nullptr, *kept.candidates, all);
		SyncCut(kept.candidates, all);
		Cut whole = std::move(cut);
		cut = std::move(kept);
		walkRefresh = keptWalkRefresh;
		cutGarbage = keptGarbage;
		cutVersion = keptVersion;
		++cutStats.parityChecks;
		std::string first;
		std::uint32_t differ = 0;
		auto note = [&](std::uint32_t a_e, const char* a_what) {
			if (!differ++)
				first = fmt::format("entry {} ({}): {}", a_e, static_cast<const void*>(a_e < cut.roots.size() ? cut.roots[a_e] : nullptr), a_what);
		};
		if (whole.roots.size() != cut.roots.size())
			note(0, "the index space");
		for (std::uint32_t e = 0; e < std::min(whole.roots.size(), cut.roots.size()); ++e) {
			if (whole.roots[e] != cut.roots[e] || whole.plans[e] != cut.plans[e]) {
				if (!differ) {
					const auto* root = cut.roots[e];
					note(e, "");
					first += fmt::format("root {} / {}, plan {} kept, {} whole; members {} kept, {} whole; '{}' {}", static_cast<const void*>(cut.roots[e]),
						static_cast<const void*>(whole.roots[e]), static_cast<int>(cut.plans[e]), static_cast<int>(whole.plans[e]), cut.memberEnd[e] - cut.memberBegin[e],
						whole.memberEnd[e] - whole.memberBegin[e], root && root->name.c_str() ? root->name.c_str() : "?", root ? root->GetRTTI() ? root->GetRTTI()->name : "?" : "");
				} else {
					note(e, "root or plan");
				}
				continue;
			}
			if (whole.mixed[e] != cut.mixed[e])
				note(e, "mixed");
			const auto members = cut.memberEnd[e] - cut.memberBegin[e];
			if (whole.memberEnd[e] - whole.memberBegin[e] != members) {
				note(e, "member count");
				continue;
			}
			for (std::uint32_t i = 0; i < members; ++i) {
				const auto& a = cut.members[cut.memberBegin[e] + i];
				const auto& b = whole.members[whole.memberBegin[e] + i];
				const bool samePath = std::equal(cut.switchPaths.begin() + a.switchBegin, cut.switchPaths.begin() + a.switchEnd, whole.switchPaths.begin() + b.switchBegin,
					whole.switchPaths.begin() + b.switchEnd);
				if (a.geometry != b.geometry || a.engine != b.engine || !samePath || cut.memberObject[cut.memberBegin[e] + i] != whole.memberObject[whole.memberBegin[e] + i]) {
					note(e, "a member");
					break;
				}
			}
			if (!std::equal(cut.switches.begin() + cut.switchBegin[e], cut.switches.begin() + cut.switchEnd[e], whole.switches.begin() + whole.switchBegin[e],
					whole.switches.begin() + whole.switchEnd[e]))
				note(e, "switches");
		}
		if (whole.eligible.size() != cut.eligible.size())
			note(0, "the eligible roots");
		if (differ) {
			++cutStats.parityMismatches;
			static std::uint32_t logged = 0;
			if (logged++ < 8)
				logger::warn("[DCLF] cut parity at frame {}: {} entries differ from a whole plan; first {} <- CUT", SceneStore::Get().GetFrame(), differ, first);
		}
	}

	void PrimaryCull::CompactCut()
	{
		std::vector<Cut::Member> members;
		std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> switchPaths;
		std::vector<const RE::NiSwitchNode*> switches;
		std::vector<std::int32_t> memberObject;
		std::vector<std::uint8_t> memberLive;
		for (std::uint32_t e = 0; e < cut.roots.size(); ++e) {
			const auto begin = static_cast<std::uint32_t>(members.size());
			for (std::uint32_t m = cut.memberBegin[e]; m < cut.memberEnd[e]; ++m) {
				auto member = cut.members[m];
				const auto pathBegin = static_cast<std::uint32_t>(switchPaths.size());
				switchPaths.insert(switchPaths.end(), cut.switchPaths.begin() + member.switchBegin, cut.switchPaths.begin() + member.switchEnd);
				member.switchBegin = pathBegin;
				member.switchEnd = static_cast<std::uint32_t>(switchPaths.size());
				members.push_back(member);
				memberObject.push_back(cut.memberObject[m]);
				memberLive.push_back(cut.memberLive[m]);
			}
			cut.memberBegin[e] = begin;
			cut.memberEnd[e] = static_cast<std::uint32_t>(members.size());
			const auto switchBegin = static_cast<std::uint32_t>(switches.size());
			switches.insert(switches.end(), cut.switches.begin() + cut.switchBegin[e], cut.switches.begin() + cut.switchEnd[e]);
			cut.switchBegin[e] = switchBegin;
			cut.switchEnd[e] = static_cast<std::uint32_t>(switches.size());
		}
		cut.members = std::move(members);
		cut.switchPaths = std::move(switchPaths);
		cut.switches = std::move(switches);
		cut.memberObject = std::move(memberObject);
		cut.memberLive = std::move(memberLive);
		cutGarbage = 0;
		++cutStats.compactions;
	}

	void PrimaryCull::RunAdmission()
	{
		auto& store = SceneStore::Get();
		if (!cut.candidates)
			return;
		bool admittedAny = false;
		// Admission is by readiness, on events (T5c: no engine verdict): a new snapshot's entries and the entries a member joined the
		// set in (OnSetChanged) are checked; one with a member not tracked (a stale snapshot's, during a load) again next frame.
		std::vector<std::uint32_t> retry;
		for (const std::uint32_t e : cut.pendingAdmission) {
			if (e >= cut.admitted.size() || cut.admitted[e] || cut.plans[e] == EntryPlan::Rejected)
				continue;
			bool all = true;
			for (std::uint32_t m = cut.memberBegin[e]; m < cut.memberEnd[e] && all; ++m) {
				const auto* geometry = cut.members[m].geometry;
				// A member no longer tracked may be gone: the entry waits for the snapshot that follows, or the next frame.
				if (!store.IsTracked(geometry)) {
					all = false;
					retry.push_back(e);
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
		cut.pendingAdmission = std::move(retry);
		if (admittedAny) {
			SyncFadeOwnership();
			++cutVersion;
		}
	}

	void PrimaryCull::RefreshWalk(std::uint32_t a_e)
	{
		if (a_e >= cut.walk.size() || cut.plans[a_e] == EntryPlan::Rejected)
			return;
		// Engine-drawn members are handed to the registration every frame; a member not in the set is, until it is. Whether it is
		// shown (its hidden bits, its switches) is the walk's test each frame, not this decision's: the engine shows a member (a cell's
		// terrain chunk once it loads) with no event that reaches here before the frame culls it.
		bool walk = cut.mixed[a_e] != 0;
		auto& store = SceneStore::Get();
		for (std::uint32_t m = cut.memberBegin[a_e]; m < cut.memberEnd[a_e] && !walk; ++m)
			if (!cut.members[m].engine && !MemberDrawable(cut.memberObject[m]) && store.IsTracked(cut.members[m].geometry))
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
		sampledWitness = FrameGlobals::Current().membershipWitness;
		sampledWitnessFrame = SceneStore::Get().GetFrame();
		return sampledWitness;
	}

	std::uint32_t PrimaryCull::FrameMembershipWitness() const
	{
		return sampledWitnessFrame == SceneStore::Get().GetFrame() ? sampledWitness : FrameGlobals::Current().membershipWitness;
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
			}
		}
		SceneStore::Get().SetFadeRootsOwned(owned);
		TreeAnimation::SetOwned(animated);
	}

	std::uint64_t PrimaryCull::MemberSignature(std::uint32_t a_e) const
	{
		std::uint64_t signature = 0;
		for (std::uint32_t m = cut.memberBegin[a_e]; m < cut.memberEnd[a_e]; ++m)
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
		if (!standInLive)
			return false;
		const auto it = cut.eligible.find(a_object);
		if (it == cut.eligible.end())
			return false;
		const std::uint32_t e = it->second;
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
			// The engine culls it this frame (less what the leaf exclusion keeps from its registration). Its admission is by
			// readiness alone (RunAdmission, on events), not by the engine's verdict.
			EngineProcess1(a_process, a_object, a_arg);
			++out.notAdmitted;
			return true;
		}
		// A switch whose selected child is out of date: NiSwitchNode::OnVisible brings it up to date before culling it
		// (UpdateDownwardPass). With the switch events that is done when the selection changes (SceneStore::CatchUpSwitch),
		// so none is found here; without them, the engine culls this entry this frame.
		if (!cut.liveEvents) {
			for (std::uint32_t w = cut.switchBegin[e]; w < cut.switchEnd[e]; ++w) {
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
			// FadeStateCS's test, DCLF's portal program of the root's room (PortalViews::Visible, on the words the GPU reads).
			// (A settled node, its fades at 1, returns before updating anything: nothing tells.)
			const bool settled = fadeRoot && (a_object->GetFlags().underlying() & kFlagFadeSettled) && At<float>(a_object, kFadeAmount) == 1.0f &&
			                     At<float>(a_object, kCurrentFade) == 1.0f;
			if (fadeRoot && !settled && a_slot >= 0 && SwitchEnabled(Switch::FadeParity)) {
				const bool engine = At<float>(a_object, 0x144) != metricBefore || At<std::int32_t>(a_object, kLastVisibleFrame) != visibleBefore;
				const float centre[3]{ a_object->worldBound.center.x, a_object->worldBound.center.y, a_object->worldBound.center.z };
				std::string why;
				auto& portals = PortalViews::Get();
				const int port = PortalViews::Visible(portals.Encoded(), portals.ProgramOf(a_object), centre, a_object->worldBound.radius, a_object->GetFlags().underlying(), why);
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
		for (std::uint32_t m = cut.memberBegin[e]; m < cut.memberEnd[e]; ++m) {
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
				if (!cut.walk[e]) {
					++out.walkMissed;
					// What the walk's decision missed (the first few a job, a frame).
					if (out.walkMissed <= 4) {
						auto& store = SceneStore::Get();
						const auto& view = store.GetTables();
						const std::int32_t slot = cut.memberObject[m];
						const auto* atSlot = slot >= 0 && static_cast<std::size_t>(slot) < view.objectGeometry.size() ? view.objectGeometry[slot] : nullptr;
						out.walkMissedFirst += fmt::format("{}'{}' (entry {} '{}', {}): slot {} {}, set phases {:#x}, resident {}, claimed {:#x}, admitted {}",
							out.walkMissedFirst.empty() ? "" : "; ", geometry->name.c_str() ? geometry->name.c_str() : "", e,
							cut.roots[e] && cut.roots[e]->name.c_str() ? cut.roots[e]->name.c_str() : "", member.engine ? "the engine's" : "DCLF's", slot,
							atSlot == geometry ? "holds it" : atSlot ? "holds another" : "empty", store.SetPhasesOf(slot), store.IsMember(slot),
							frameSet ? frameSet->PhasesOf(geometry) : 0xFF, cut.admitted[e]);
					}
				}
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
		if (!frameLive.exchange(false, std::memory_order_acq_rel))
			return;
		const std::int64_t start = Now();
		const std::uint32_t sunBits = SunAccumulation::Get().SunBits();
		frameVisible.clear();
		for (std::uint32_t i = 0; i < cut.processCount; ++i) {
			auto& out = jobOut[i];
			frameVisible.insert(frameVisible.end(), out.visible.begin(), out.visible.end());
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
			if (s.walkMissedFirst.size() < 2000 && !out.walkMissedFirst.empty())
				s.walkMissedFirst += fmt::format(" [frame {}] {}", SceneStore::Get().GetFrame(), out.walkMissedFirst);
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
		// The frame's walk runs on (step 6c): what this reads of it is the last walk's, handed over at the frame's start.
		ZoneScopedN("CS.DCLF.AfterFullFrustum");
		frameLive.store(false, std::memory_order_relaxed);
		gpuSunFrame = false;
		SunAccumulation::Get().EndFullFrustumWindow();
		if (ListsFiltered() && !SunAccumulation::Get().ExclusionLive() && !Global<std::uint8_t>(kSunOff))
			++listStats.unexcluded;
		// CS_DCLF_PARITY_BOTH: the engine culls and registers everything.
		if (!PassCapture::ParityBoth())
			PrepareFrame();
		CheckFadePort();
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

	namespace
	{
		thread_local std::uint8_t syntheticFail = 0;
	}

	std::uint8_t PrimaryCull::LastSyntheticFail()
	{
		return syntheticFail;
	}

	bool PrimaryCull::SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out, bool a_sunOnGpu,
		const RE::BSLightingShaderProperty* a_layer)
	{
		syntheticFail = 1;
		if (a_derivedPass == kNotDerived)
			return false;
		const RE::BSShaderProperty* property = a_layer ? a_layer : a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = a_layer ? a_layer : netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		syntheticFail = 2;
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
		syntheticFail = 3;
		if (sun == ~0u)
			return false;
		std::uint32_t hint = 0;
		syntheticFail = a_layer ? 4 : 5;
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
		syntheticFail = 0;
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

		/** @brief Main::Update's portal walk (FUN_1401a43c0): DCLF's walk right after it, from the same camera, before the lists are built. */
		struct PortalWalk
		{
			static void thunk(void* a_tes, void* a_process)
			{
				func(a_tes, a_process);
				PortalViews::Get().Build();
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
		if (!CallsTo(afterFullFrustum, base + kAfterFullFrustum) || !CallsTo(finish, base + kListJobsFinish) || !CallsTo(base + kPortalWalkCallSite, base + kPortalWalk)) {
			logger::warn("[DCLF] primary cull: the calls are not where expected; the engine keeps the primary's cull");
			return;
		}
		// The list processes' Process1 slot must still be the engine's (no other hook in the way).
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSGeometryListCullingProcess[0] };
		if (reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x16] != base + kProcess1) {
			logger::warn("[DCLF] primary cull: BSGeometryListCullingProcess::Process1 is not the engine's; the engine keeps the primary's cull");
			return;
		}
		stl::write_thunk_call<Hooks::PortalWalk>(base + kPortalWalkCallSite);
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
		logger::info("[DCLF] primary cull installed (the scene lists after the full-frustum cull, the list jobs' Finish)");
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
			logger::info("[DCLF] primary exclusion: applied on {} of {} frames ({} without candidates, {} preconditions), {:.1f} entries planned again a frame as their candidates moved ({} pool gatherings; parity {} checks, {} differ); per frame {:.0f} eligible entries reached, "
						 "{:.0f} stood in for ({:.0f} walked, {:.0f} of them in view), {:.1f} cross-fading LOD (stood in), {:.1f} not yet admitted ({:.1f} admitted); {:.0f} members in view, {:.1f} not bound yet (the engine's), "
						 "{:.1f} bound but not in the set (the engine's), {:.1f} hidden;  {:.1f} owned geometries left out of the engine's registration; "
						 "{:.0f} of the engine's members in view registered by it; switches: {:.1f} entries culled by the engine (stale child), {:.0f} members unselected, selection read from every switch on {} frames and from {} events' entries; render thread: prepare {:.3f} ms, after the jobs {:.3f} ms",
				s.appliedFrames, s.frames, s.skippedStale, s.skippedPreconditions, double(s.entriesPlanned) / applied, s.compactions, s.parityChecks, s.parityMismatches, s.seen / applied, s.skipped / applied, s.walked / applied, s.visibleEntries / applied,
				s.notSettled / applied, s.notAdmitted / applied, s.admittedNow / applied, s.members / applied, s.unbound / applied,
				s.undrawable / applied, s.hiddenSkipped / applied, s.excluded / applied,
				s.engineMembers / applied, s.switchStale / applied, s.unselected / applied, s.liveAll, s.liveEntries, s.prepareTicks * toMs / applied, s.afterTicks * toMs / applied);
			{
				const auto r = SceneStore::Get().TakeResidentStats();
				const double rf = std::max<double>(static_cast<double>(r.frames), 1.0);
				logger::info("[DCLF] scene membership: {:.0f} objects bound a frame ({} frames); {} records queued, {} joined, {} failed ({} the engine's pass, {} no record, {} a frame verdict, {} material or extras; {} waited for a material record, {} served), {} rewritten ({} kept their binding), {} released, {} layers or bases unpaired; {} registrations of eligible objects not bound{}{}",
					r.resident / rf, r.frames, r.membershipQueued, r.joined, r.failed, r.failedBy[0], r.failedBy[1], r.failedBy[2], r.failedBy[3], r.materialWaits, r.materialsServed, r.rewritten, r.membershipKept, r.released,
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
			if (!s.walkMissedFirst.empty())
				logger::info("[DCLF] stand-in walk parity, missed:{}", s.walkMissedFirst);
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
			// LOD skins: records whose partitions BuildDraws picks by their root's level in FadeStateCS's state (T1b).
			const auto& lodTables = SceneStore::Get().GetTables();
			const auto lodSkins = std::count_if(lodTables.skinLodPartitions.begin(), lodTables.skinLodPartitions.end(), [](std::uint32_t a_word) { return a_word != 0; });
			logger::info("[DCLF] fade roots: {} stood in; {} LOD skins (partitions by their root's level on the GPU); {:.0f} entries with engine-drawn parts culled by the engine a frame",
				SceneStore::Get().StoodInFadeRoots(), lodSkins, mixedPerFrame);
		}
	}
}
