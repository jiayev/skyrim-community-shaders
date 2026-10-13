// PrimaryCull: the list jobs' stand-in (Process1, AppendVirtual; job threads), the render thread's frame around them, the cut's
// plans and admission, the synthetic membership passes (any thread), and the fade writer's hooks.
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Engine/Views/PortalViews.h"

#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Engine/Cull/TreeAnimation.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SideEffects.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include <Tracy/Tracy.hpp>
#include "Features/DrawcallLimitFix/Engine/Cull/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Runtime/Switches.h"
#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FadeState.h"

#include <algorithm>
#include <ranges>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

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
		constexpr std::uintptr_t kListJobsFinishCallSite = 0x14cbf4d;  // ... -> JobList::Finish(scene list culling)
		constexpr std::uintptr_t kProcess1 = 0xe28390;  // BSCullingProcess::Process1, the list processes' vtable slot 0x16
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;  // byte: the accumulator draws the deferred shadow mask
		// The main camera: the world scene graph's (+0x128), carried by the list processes; FUN_1406452c0 (Main::Draw's camera
		// setup, before BeginSceneFrame) sets the tree height base from it.
		constexpr std::size_t kSceneGraphCamera = 0x128;
		// NiAVObject / BSFadeNode fields BSFadeNode::OnVisible (AE 0x141479f50) reads before its fade update.
		constexpr std::uint32_t kFlagFadeSettled = 1u << 15;
		constexpr std::size_t kFadeAmount = 0x100;
		constexpr std::size_t kCurrentFade = 0x130;
		constexpr std::size_t kLastVisibleFrame = 0x13C;
		constexpr std::uint32_t kFlagFadeTargetReached = 1u << 14;
		constexpr std::size_t kCameraLodAdjust = 0x184;
		constexpr std::uintptr_t kAnimatedFadeCallSite = 0x2d0044;  // FUN_1402cff60 (UpdateAnimationJob) -> FUN_14147a160
		// BSTreeNode::OnVisible (AE 0x14147d3c0): the height test and the LOD fix-up.
		constexpr std::size_t kProcessTreeHeightTest = 0x121;     // byte on the culling process
		constexpr std::uintptr_t kTreeHeightTestOn = 0x2032fb8;   // byte
		constexpr std::uintptr_t kTreeHeightBase = 0x332a2f0;     // float
		constexpr std::uintptr_t kTreeHeightLimit = 0x2032fa0;    // float
		constexpr std::size_t kTreeLodData = 0x180;               // pointer; its +0x12C count
		// The NiNode OnVisible BSFadeNode's ends in (its children's cull), and fields the fade OnVisible detours read.
		constexpr std::uintptr_t kNodeOnVisible = 0xd1e2c0;       // NiNode::OnVisible
		constexpr std::size_t kNodeWorldZ = 0xEC;                 // NiAVObject::world.translate.z
		constexpr std::size_t kProcessCameraUpdates = 0x11C;      // byte: NiCullingProcess::cameraRelatedUpdates
		constexpr std::size_t kFadeLodType = 0x153;               // byte: the LOD type in its low nibble
		// FUN_14147b110 (the fade value) and FUN_14147a160 (the fade update): what the fade-out distance is made of.
		constexpr std::size_t kFadeNear = 0x128;                  // float, on the fade node
		constexpr std::size_t kFadeFar = 0x12C;                   // float
		constexpr std::size_t kFadeAlwaysOut = 0x109;             // byte: bit 0 fades the node out whatever its distance
		// The camera cut: a move past any locomotion (a sprinting horse ~20 units/frame at 60 fps), or a turn past 30 degrees (a
		// fast mouse flick ~15).
		constexpr float kCutDistance = 256.0f;
		constexpr float kCutTurnCosine = 0.8660254f;

		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

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
		return Engine::ReadSwitch(*a_switch).index;
	}

	namespace
	{
		/**
		 * @brief An effect member's main pass as BSEffectShaderProperty::GetRenderPasses (0x1414bd890) would register it (hint,
		 * effect pass enum, LOD row). Not cached: MembershipDerived's witnesses miss the alpha property and texture paths; it is cheap.
		 */
		bool EffectMembershipPass(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord& a_effect, AccumulatedPass& a_out, std::uint32_t* a_derived)
		{
			LightingDescriptors descriptors;
			const bool derived = DeriveEffectDescriptors(a_effect, a_leaf, descriptors) == Ineligible::None;
			if (a_derived)
				*a_derived = derived ? descriptors.derivedPass : kNotDerived;
			if (!derived)
				return false;
			a_out = {};
			a_out.subPass = PassCapture::SubPassOf(a_leaf.AlphaTesting(), a_effect.flags);
			a_out.technique = descriptors.derivedPass;
			a_out.passEnum = EffectPassEnum(descriptors.derivedPass);
			// GetRenderPasses' hint: 0 opaque, 2 or 3 decal (by whether its alpha blends), 1 blended.
			a_out.hint = descriptors.opaqueEffect ? 0u : descriptors.decalGroup ? descriptors.decalGroup + 1 : 1u;
			a_out.lodRow = SceneCapture::LodRowOf(a_leaf, &a_effect);
			return true;
		}
	}

	bool PrimaryCull::FreshSyntheticPass(const SceneCapture::LeafView& a_leaf, AccumulatedPass& a_out)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (const auto* effect = lighting ? nullptr : SceneCapture::LeafView::Effect(a_leaf.property); effect && ActiveToggles().effects) {
			if (!EffectMembershipPass(a_leaf, *effect, a_out, nullptr))
				return false;
			a_out.resident = true;
			return true;
		}
		if (!lighting)
			return false;
		LightingDescriptors descriptors;
		if (DeriveLightingDescriptors(*lighting, a_leaf, nullptr, descriptors) != Ineligible::None)
			return false;
		if (!SyntheticPass(a_leaf, descriptors.derivedPass, a_out))
			return false;
		a_out.resident = true;
		return true;
	}

	float PrimaryCull::FadeDistanceOf(const SceneCapture::NodeRecord& a_root)
	{
		// From the fade node's record and the frame's captured globals, never the engine's.
		const auto& g = FrameGlobals::Current();
		// No fade update at all, or no distance term in the fade value (FUN_14147b110 returns 1).
		if (!g.fadesOn || !g.fadeLodUpdates)
			return 0.0f;
		const std::uint32_t type = a_root.fadeType;
		// Type 8 fades gradually even out of view (FUN_14147a160) and OnVisible's type-6 branch never fades out (ServiceFade): the
		// engine draws either the frame it comes into view.
		if (type == 8 || (type == 6 && g.fadeSpecialA == g.fadeSpecialB))
			return 0.0f;
		const float mult = g.fadeDistanceMult;
		const float nearDistance = mult * a_root.fadeNear;
		const float farDistance = mult * a_root.fadeFar;
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

	PrimaryCull::Membership PrimaryCull::MembershipPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached)
	{
		Membership answer;
		answer.derived = a_cached;
		answer.fail = 2;  // not lighting (SyntheticPass's index)
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (const auto* effect = lighting ? nullptr : SceneCapture::LeafView::Effect(a_leaf.property); effect && ActiveToggles().effects) {
			std::uint32_t derived = kNotDerived;
			answer.ok = EffectMembershipPass(a_leaf, *effect, answer.pass, &derived);
			answer.derived = { effect->key, effect->material, effect->flags, 0, derived };
			answer.fail = answer.ok ? 0 : 1;  // derived, or not (SyntheticPass's indices)
			if (answer.ok)
				answer.pass.resident = true;
			return answer;
		}
		if (!lighting)
			return answer;
		const std::uint8_t fadeState = FadeStateOf(lighting);
		if (a_cached.property != lighting->key || a_cached.material != lighting->material || a_cached.flags != lighting->flags || a_cached.fadeState != fadeState) {
			LightingDescriptors descriptors;
			const auto reason = DeriveLightingDescriptors(*lighting, a_leaf, nullptr, descriptors);
			answer.derived = { lighting->key, lighting->material, lighting->flags, fadeState, reason == Ineligible::None ? descriptors.derivedPass : kNotDerived };
		}
		answer.ok = SyntheticPass(a_leaf, answer.derived.derivedPass, answer.pass);
		// Thread-local, from the synthetic pass just made.
		answer.fail = LastSyntheticFail();
		if (answer.ok)
			answer.pass.resident = true;
		return answer;
	}

	PrimaryCull::Membership PrimaryCull::MembershipLayerPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached)
	{
		Membership answer;
		answer.derived = a_cached;
		answer.fail = 2;  // not lighting (SyntheticPass's index)
		const auto* layer = SceneCapture::LeafView::Lighting(a_leaf.layer);
		if (!layer)
			return answer;
		const std::uint8_t fadeState = FadeStateOf(layer);
		if (a_cached.property != layer->key || a_cached.material != layer->material || a_cached.flags != layer->flags || a_cached.fadeState != fadeState) {
			LightingDescriptors descriptors;
			const auto reason = DeriveLightingDescriptors(*layer, a_leaf, nullptr, descriptors, true);
			answer.derived = { layer->key, layer->material, layer->flags, fadeState, reason == Ineligible::None ? descriptors.derivedPass : kNotDerived };
		}
		answer.ok = SyntheticPass(a_leaf, answer.derived.derivedPass, answer.pass, layer);
		answer.fail = LastSyntheticFail();
		if (answer.ok)
			answer.pass.resident = true;
		return answer;
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

	PrimaryCull::EntryPlan PrimaryCull::PlanOf(std::uint32_t a_entry, const RE::NiAVObject* a_root)
	{
		const auto& candidates = *cut.candidates;
		// Each node and geometry is judged by its OnVisible slot (wideOnVisible), not its RTTI. A geometry the snapshot has as DCLF's
		// (Eligibility::PrimaryVerdict) is a member; a hidden static's is left out; any other is the engine's and makes the entry mixed.
		const std::size_t first = cut.members.size();
		const std::size_t firstPath = cut.switchPaths.size();
		const std::size_t firstSwitch = cut.switches.size();
		std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> path;
		const char* rejected = nullptr;
		const RE::BSGeometry* mixedBy = nullptr;
		std::uint32_t ours = 0, hidden = 0;
		const auto nameOf = [](const RE::NiAVObject* a_object) {
			const auto* rtti = a_object->GetRTTI();
			return rtti && rtti->name ? rtti->name : "no RTTI";
		};
		const auto visit = [&](const auto& a_self, const RE::NiAVObject* a_object, bool a_isRoot) -> void {
			if (rejected || !a_object)
				return;
			const std::uintptr_t onVisible = (*reinterpret_cast<const std::uintptr_t* const*>(a_object))[0x34];
			if (const auto* geometry = const_cast<RE::NiAVObject*>(a_object)->AsGeometry()) {
				const bool appends = onVisible == geometryOnVisible || onVisible == wideOnVisible.dynamic || onVisible == wideOnVisible.subIndex ||
				                     onVisible == wideOnVisible.segmented;
				if (!appends) {
					rejected = nameOf(a_object);
					return;
				}
				const auto it = candidates.geometries.find(geometry);
				const std::uint8_t verdict = it != candidates.geometries.end() && it->second < candidates.primaryGeometry.size() ? candidates.primaryGeometry[it->second] : Eligibility::kPrimaryEngine;
				// A hidden static's geometry: neither; unhiding changes the verdict and replans the entry.
				if (verdict == Eligibility::kPrimaryHiddenStatic) {
					++hidden;
					return;
				}
				const bool mine = verdict == Eligibility::kPrimaryMember;
				const auto begin = static_cast<std::uint32_t>(cut.switchPaths.size());
				cut.switchPaths.insert(cut.switchPaths.end(), path.begin(), path.end());
				cut.members.push_back({ geometry, !mine, begin, static_cast<std::uint32_t>(cut.switchPaths.size()) });
				ours += mine ? 1 : 0;
				if (!mine && !mixedBy)
					mixedBy = geometry;
				return;
			}
			auto* node = const_cast<RE::NiAVObject*>(a_object)->AsNode();
			if (node && !a_isRoot && onVisible == wideOnVisible.switchNode) {
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
			const bool fadeRoot = a_isRoot && (RttiIs(a_object, "BSFadeNode") || RttiIs(a_object, "BSLeafAnimNode") || RttiIs(a_object, "BSTreeNode"));
			const bool plain = onVisible == wideOnVisible.node || onVisible == wideOnVisible.engineNode || onVisible == wideOnVisible.multiBound ||
			                   onVisible == wideOnVisible.billboard || onVisible == wideOnVisible.ordered;
			if (!node || !(plain || fadeRoot)) {
				rejected = nameOf(a_object);
				return;
			}
			for (const auto& child : node->GetChildren())
				a_self(a_self, child.get(), false);
		};
		visit(visit, a_root, true);
		// Nothing of DCLF's: the engine's, unless it holds only hidden statics (a marker), which nobody draws until unhidden.
		const bool empty = !ours && !(hidden && !mixedBy);
		cut.rejectReason[a_entry] = rejected ? rejected : empty ? "no DCLF geometry" : nullptr;
		cut.mixedBy[a_entry] = rejected || empty || !mixedBy ? std::string() :
		                                                                    fmt::format("'{}' ({})", mixedBy->name.c_str() ? mixedBy->name.c_str() : "", nameOf(mixedBy));
		if (rejected || empty) {
			cut.members.resize(first);
			cut.switchPaths.resize(firstPath);
			cut.switches.resize(firstSwitch);
			return EntryPlan::Rejected;
		}
		return RttiIs(a_root, "BSFadeNode") ? EntryPlan::FadeRoot :
		       RttiIs(a_root, "BSLeafAnimNode") ? EntryPlan::LeafRoot :
		       RttiIs(a_root, "BSTreeNode") ? EntryPlan::TreeRoot : EntryPlan::Plain;
	}

	void PrimaryCull::BeginFrameCamera()
	{
		// From the main camera, so nothing here depends on the list cull running (the list processes carry the same camera).
		if (!installed)
			return;
		const auto* sceneGraph = Global<const std::byte*>(Engine::Address::kWorldSceneGraph);
		const auto* camera = sceneGraph ? At<const RE::NiCamera*>(sceneGraph, kSceneGraphCamera) : nullptr;
		frameCamera = camera;
		// The fade test's camera.
		fadeEye = {};
		if (camera) {
			fadeEye = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z, At<float>(camera, kCameraLodAdjust) };
			// A camera cut (a jump or snap turn the verdict-to-write latency cannot follow): every DCLF-written root visible until a
			// batch of this frame or later decodes (IndirectDraws::NoteCameraCut). Turn angle from the relative rotation's trace.
			const auto& r = camera->world.rotate.entry;
			const std::array<float, 12> now{ fadeEye[0], fadeEye[1], fadeEye[2], r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2] };
			if (cutCameraKnown) {
				const float dx = now[0] - cutCamera[0], dy = now[1] - cutCamera[1], dz = now[2] - cutCamera[2];
				float trace = 0.0f;
				for (std::uint32_t i = 3; i < 12; ++i)
					trace += now[i] * cutCamera[i];
				const float cosine = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
				if (dx * dx + dy * dy + dz * dz > kCutDistance * kCutDistance || cosine < kCutTurnCosine)
					IndirectDraws::Get().NoteCameraCut(Scene::FrameView::Get().GetFrame());
			}
			cutCamera = now;
			cutCameraKnown = true;
		}
		// BSTreeNode::OnVisible's height test as the list processes have it: their +0x121 is 0 (only the constructors write it;
		// skyrim-engine-notes.md, "the height-test byte"), the base is the camera height (FUN_1406452c0), the switch and limit globals.
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto** processes = Global<RE::NiCullingProcess**>(Engine::Address::kListProcesses);
		const bool heightTest = processes && count && processes[0] && At<std::uint8_t>(processes[0], kProcessTreeHeightTest) && Global<std::uint8_t>(kTreeHeightTestOn);
		treeHeight = { Global<float>(kTreeHeightBase), heightTest ? Global<float>(kTreeHeightLimit) : std::numeric_limits<float>::infinity() };
	}

	void PrimaryCull::PrepareFrame()
	{
		++cutStats.frames;
		// FrameView samples the witness with the frame's inputs; a frame without them samples it here.
		if (sampledWitnessFrame != Scene::FrameView::Get().GetFrame())
			SampleMembershipWitness();
		// A frame the cut skips drops the switch changes, so the next one reads every switch.
		const bool switchResync = Scene::FrameView::Get().TakeSwitchChanges(switchChanges);
		liveStale = switchResync || liveStale;
		auto candidates = Scene::FrameView::Get().GetSunCandidates();
		const std::uint32_t count = Global<std::uint32_t>(Engine::Address::kSceneListCount);
		auto** processes = Global<RE::NiCullingProcess**>(Engine::Address::kListProcesses);
		frameSet = Scene::FrameView::Get().FrameClaims();
		standInLive = false;
		Diagnostics::Notify(Diagnostics::CutFrameBegin{ processes, count });
		if (!processes || !count || count > cut.processes.size())
			return;
		// The leaf exclusion (AppendVirtual) applies whenever the list processes are known.
		cut.processCount = count;
		for (std::uint32_t i = 0; i < count; ++i) {
			cut.processes[i] = processes[i];
			auto& out = jobOut[i];
			out = JobOut{ std::move(out.visible) };
			out.visible.clear();
		}
		Diagnostics::Notify(Diagnostics::CutSlotsReset{ count });
		// The stand-in needs a current snapshot; otherwise the engine culls every entry (less the leaf exclusion).
		if (!ActiveToggles().excludePrimaryEntries) {
			if (!ownedFadeRoots.empty()) {
				ownedFadeRoots.clear();
				standInRoots.clear();
				standInList.clear();
						Scene::FrameView::Get().SetFadeRootsOwned({});
				TreeAnimation::SetOwned({}, {});
			}
			frameLive.store(true, std::memory_order_release);
			return;
		}
		// The sun's exclusion is not a precondition: listed roots' fade is DCLF's alone, and a whole entry the sun's cull reaches
		// without it has its casters' passes withheld (SunAccumulation, PassCapture).
		if (!candidates) {
			++(candidates ? cutStats.skippedPreconditions : cutStats.skippedStale);
			liveStale = true;
			// The engine culls the stood-in roots and writes their kAccumulated this frame; the next decode writes them all.
			if (!ownedFadeRoots.empty())
				IndirectDraws::Get().RequestAccumulatedRefresh();
			frameLive.store(true, std::memory_order_release);
			return;
		}
		const std::int64_t start = Now();
		// Entries whose versions moved since the last snapshot are planned again (SyncCut); the rest stand.
		const bool newSnapshot = cut.candidates != candidates;
		std::vector<std::uint32_t> changedEntries;
		if (newSnapshot) {
			ChangedEntries(cut.candidates.get(), *candidates, changedEntries);
			SyncCut(candidates, changedEntries);
		}
		Diagnostics::Notify<Diagnostics::EnginePoint::CutSynced>();
		// Switch events name the entries to read again (dclf-cull-job-elimination.md); a resync or skipped frame reads all.
		cut.liveEvents = Scene::SwitchEventsLive();
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
		walkEverything = Policy::WalkEveryEntry();
		// Walk flags for entries planned again (once memberLive is current) and entries a member joined or left.
		for (const std::uint32_t e : std::exchange(walkRefresh, {}))
			RefreshWalk(e);
		// Entries whose members are all in the set leave the engine's cull from this frame.
		RunAdmission();
		// Fade roots: a new snapshot's, and all again after frames the engine culled them (its OnVisible ran meanwhile).
		if (!changedEntries.empty())
			SyncFadeOwnership();
		++cutStats.appliedFrames;
		cutStats.prepareTicks += Now() - start;
		// The list jobs are queued after this returns, which orders everything above before their reads.
		frameLive.store(true, std::memory_order_release);
		gpuSunFrame = true;
	}

	void PrimaryCull::NoteAnimatedFade(const void* a_node, const float* a_camera)
	{
		std::scoped_lock lock(animatedMutex);
		animatedNodes.push_back(a_node);
		// FUN_14147a160's inputs: the camera FUN_1402cff60 passes (world root camera's position, lodAdjust) and the fade globals;
		// the same for every call of one animation update.
		if (a_camera) {
			const AnimatedFadeInputs inputs{ { a_camera[0], a_camera[1], a_camera[2] }, a_camera[3], Global<std::int32_t>(Engine::Address::kFadeFrameCounter), Global<float>(Engine::Address::kFadeStepTime), 1u };
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
		// A joining geometry is not main-registered, so nothing clears the light bits it gathered as the engine's: cleared once here,
		// then kept 0 (SunAccumulation, ClearOwnedMask). Only those the installed publication claims (and so holds alive).
		for (const auto* geometry : a_joined)
			if (Scene::FrameView::Get().HeldByInstalled(geometry))
				if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get())
					if (void* lightData = At<void*>(property, kPropertyLightData))
						At<std::uint32_t>(lightData, kLightDataActiveMask) = 0;
		for (const auto* geometry : a_left)
			NoteMemberLost(geometry);
		if (!cut.candidates)
			return;
		// By pointer alone: nothing here is dereferenced.
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
		grow(cut.rejectReason, static_cast<const char*>(nullptr));
		grow(cut.mixedBy, std::string());
		for (const std::uint32_t e : a_changed) {
			// Let go of the old entry; its member ranges become garbage.
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
			cut.rejectReason[e] = nullptr;
			cut.mixedBy[e].clear();
			const auto* root = e < capacity ? candidates.entryNodes[e] : nullptr;
			cut.roots[e] = root;
			if (!root)
				continue;
			// Planned again: members and switches appended (PlanOf), their objects, their switch selection.
			cut.plans[e] = PlanOf(e, root);
			cut.memberEnd[e] = static_cast<std::uint32_t>(cut.members.size());
			cut.switchEnd[e] = static_cast<std::uint32_t>(cut.switches.size());
			if (cut.plans[e] == EntryPlan::Rejected)
				continue;
			cut.memberObject.resize(cut.members.size(), -1);
			cut.memberLive.resize(cut.members.size(), 1);
			for (std::uint32_t m = cut.memberBegin[e]; m < cut.memberEnd[e]; ++m) {
				cut.mixed[e] |= cut.members[m].engine ? 1 : 0;
				// The object the snapshot's walk had (the frame reads no tracked set).
				const auto it = cut.members[m].engine ? candidates.geometries.end() : candidates.geometries.find(cut.members[m].geometry);
				cut.memberObject[m] = it != candidates.geometries.end() && it->second < candidates.geometrySlot.size() ? candidates.geometrySlot[it->second] : -1;
			}
			for (std::uint32_t w = cut.switchBegin[e]; w < cut.switchEnd[e]; ++w)
				cut.switchEntry.insert_or_assign(cut.switches[w], e);
			RefreshLive(e);
			cut.eligible.insert_or_assign(root, e);
			// Admitted again if its DCLF members are unchanged; otherwise queued for RunAdmission.
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
		cut.rejectReason.resize(capacity);
		cut.mixedBy.resize(capacity);
		std::erase_if(cut.pendingAdmission, [capacity](std::uint32_t a_e) { return a_e >= capacity; });
		std::erase_if(walkRefresh, [capacity](std::uint32_t a_e) { return a_e >= capacity; });
		// Repack once garbage exceeds live members (amortised: one copy per member let go).
		if (cutGarbage > cut.members.size() / 2)
			CompactCut();
		++cutVersion;
	}

	PrimaryCull::Cut PrimaryCull::PlanWholeCut()
	{
		// SyncCut on an empty cut, then the current cut put back.
		if (!cut.candidates)
			return {};
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
		return whole;
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
		if (!cut.candidates)
			return;
		const auto& candidates = *cut.candidates;
		bool admittedAny = false;
		// Admission by readiness on events, never by engine verdict.
		std::vector<std::uint32_t> retry;
		for (const std::uint32_t e : cut.pendingAdmission) {
			if (e >= cut.admitted.size() || cut.admitted[e] || cut.plans[e] == EntryPlan::Rejected)
				continue;
			bool all = true;
			for (std::uint32_t m = cut.memberBegin[e]; m < cut.memberEnd[e] && all; ++m) {
				const auto* geometry = cut.members[m].geometry;
				// A member the frame's snapshot does not list may be gone: wait for a snapshot that lists it. The snapshot came with
				// the installed publication, whose retirement chain keeps every geometry it lists alive (by pointer alone here).
				if (!candidates.geometries.contains(geometry)) {
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
		// Walk while there are engine-drawn members or members not in the set. Shown-ness is the walk's per-frame test, not this
		// decision's: the engine shows a member (a cell's terrain chunk once loaded) with no event reaching here first.
		// Tracked: listed by the frame's snapshot (never the coordinator's live map, which the scene pass rewrites meanwhile).
		bool walk = cut.mixed[a_e] != 0;
		const auto& geometries = cut.candidates->geometries;
		for (std::uint32_t m = cut.memberBegin[a_e]; m < cut.memberEnd[a_e] && !walk; ++m)
			if (!cut.members[m].engine && !MemberDrawable(cut.memberObject[m]) && geometries.contains(cut.members[m].geometry))
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
				// Out of this frame's lists: handed to nobody until the next frame's filter keeps the root.
				if (const auto filter = listFilter.load(std::memory_order_acquire); filter && ListsFiltered() && filter->roots.contains(cut.roots[e]))
					++listStats.lostWhileOut;
			}
		}
	}


	bool PrimaryCull::MemberDrawable(std::int32_t a_object) const
	{
		return (Scene::FrameView::Get().SetPhasesOf(a_object) & kSetMain) != 0;
	}

	std::uint32_t PrimaryCull::SampleMembershipWitness()
	{
		sampledWitness = FrameGlobals::Current().membershipWitness;
		sampledWitnessFrame = Scene::FrameView::Get().GetFrame();
		return sampledWitness;
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
		// Admitted entries with a fade root: their fade is FadeStateCS's from the next frame. A mixed one is culled by the engine,
		// which updates its node. Every other is stood in (state on the GPU alone) and taken off the tree manager's animation list
		// (TreeAnimation: TreeWindCS animates its trees; the tree clock reads BSLeafAnimNode's kAccumulated). Its kAccumulated is
		// DCLF's, written at Main::Update's entry (AtUpdateEntry); clearing it would stop FUN_1402cff60 animating a stood-in
		// reference that never fades. Until its first write it keeps the engine's last bit (a new listing is written visible).
		ownedFadeRoots.clear();
		standInRoots.clear();
		standInList.clear();
		std::vector<Scene::OwnedFadeRoot> owned;
		std::vector<const RE::NiAVObject*> animated, trees;
		for (std::uint32_t e = 0; e < cut.plans.size(); ++e) {
			const auto plan = cut.plans[e];
			const bool fade = plan == EntryPlan::FadeRoot || plan == EntryPlan::LeafRoot || plan == EntryPlan::TreeRoot;
			// A stood-in plain entry has no engine cull either: its kAccumulated is DCLF's.
			if (plan == EntryPlan::Plain && cut.admitted[e] && !cut.mixed[e])
				standInRoots.insert(cut.roots[e]);
			if (!cut.admitted[e] || !fade)
				continue;
			ownedFadeRoots.push_back(cut.roots[e]);
			owned.push_back({ cut.roots[e], !cut.mixed[e] });
			if (!cut.mixed[e]) {
				standInRoots.insert(cut.roots[e]);
				standInList.push_back(cut.roots[e]);
				animated.push_back(cut.roots[e]);
				// A stood-in BSTreeNode stays on the tree manager's tree list, its loop given DCLF's verdict (TreeAnimation).
				if (plan == EntryPlan::TreeRoot)
					trees.push_back(cut.roots[e]);
			}
		}
		Scene::FrameView::Get().SetFadeRootsOwned(owned);
		TreeAnimation::SetOwned(animated, trees);
	}

	void PrimaryCull::AtUpdateEntry(bool a_running)
	{
		if (!installed)
			return;
		ZoneScopedN("CS.DCLF.AccumulatedWrites");
		// Roots no longer stood in are left to the engine's cull this frame.
		IndirectDraws::Get().WriteAccumulated(a_running, [this](const void* a_root) { return standInRoots.contains(a_root); });
		// The tree manager's loop (later in this Main::Update) takes the stood-in trees' visibility from those writes.
		TreeAnimation::AtUpdateEntry(a_running);
		// The fade write-back on every root the accepted tables list with kFadeRootWritten, published first as the set the gate
		// and fade OnVisibles skip.
		PublishWriterTables(a_running);
		IndirectDraws::Get().WriteFadeStores(a_running, [this](const void* a_root) { return FadeWrittenByDclf(a_root); });
		Diagnostics::Notify(Diagnostics::UpdateEntry{ a_running });
		// The high actors' animation stamp from DCLF's shadow views.
		SideEffects::UpdateActorRate(a_running);
	}

	PrimaryCull::FadeWriterCounts PrimaryCull::TakeFadeWriterCounts()
	{
		FadeWriterCounts c;
		c.onVisibleSkipped = onVisibleFadeSkipped.exchange(0, std::memory_order_relaxed);
		c.gateSkipped = gateFadeSkipped.exchange(0, std::memory_order_relaxed);
		const auto* tables = static_cast<const SceneTables*>(fadeWriterTables.load(std::memory_order_acquire));
		c.roots = tables ? tables->fadeRootIndex.size() : 0;
		return c;
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
		Diagnostics::Notify(Diagnostics::StandInEntry{ a_slot, e, a_object });
		const bool fadeRoot = plan == EntryPlan::FadeRoot || plan == EntryPlan::LeafRoot || plan == EntryPlan::TreeRoot;
		// Fades and LOD cross-fades are FadeStateCS's; a faded-out root draws nothing. A cross-fade's hint-10 copy is not drawn
		// (drawcall-limit-fix.md, "LOD cross-fades": none in SE's settings); crossing roots (+0x153 bits 4-6 not 2) are counted.
		if (fadeRoot && (At<std::uint8_t>(a_object, 0x153) & 0x70) != 0x20)
			++out.notSettled;
		if (!cut.admitted[e]) {
			// The engine culls it this frame (less the leaf exclusion); admission is by readiness, not its verdict.
			EngineProcess1(a_process, a_object, a_arg);
			++out.notAdmitted;
			return true;
		}
		// A stale selected child: NiSwitchNode::OnVisible updates it before culling (UpdateDownwardPass). Switch events do that on
		// change (Coordinator::CatchUpSwitch); without them, the engine culls this entry this frame.
		if (!cut.liveEvents) {
			for (std::uint32_t w = cut.switchBegin[e]; w < cut.switchEnd[e]; ++w) {
				const auto* switchNode = cut.switches[w];
				if (const auto selection = Engine::SelectionOf(*switchNode, Engine::ReadSwitch(*switchNode)); selection.child && !selection.current) {
					++out.switchStale;
					EngineProcess1(a_process, a_object, a_arg);
					return true;
				}
			}
		}
		// Engine-drawn parts: the engine culls the entry whole; the AppendVirtual hook keeps DCLF's members from the registration.
		if (cut.mixed[e]) {
			EngineProcess1(a_process, a_object, a_arg);
			++out.mixed;
			return true;
		}
		++out.skipped;
		// Nothing for the registration: the GPU culls and draws it whole.
		if (!cut.walk[e] && !walkEverything)
			return true;
		++out.walked;
		// Test against the job's own planes (its Process2 set them up from the list's first entry). A tree above the height limit
		// draws nothing.
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
			// In the frame's set: the GPU draws it. Otherwise (not ready, rebinding) the engine registers it.
			if (!member.engine && MemberDrawable(cut.memberObject[m])) {
				out.visible.push_back(geometry);
				continue;
			}
			if (!member.engine && Scene::FrameView::Get().IsMember(cut.memberObject[m]))
				++out.undrawable;
			// The engine's, or DCLF's but unbound: the cull's bound test, then BSGeometry::OnVisible's append, in traversal order.
			if (!Outside(a_process->planes, geometry->worldBound)) {
				// The geometry's own OnVisible when it is not BSGeometry's (the three that append under a condition).
				if (const std::uintptr_t onVisible = (*reinterpret_cast<const std::uintptr_t* const*>(geometry))[0x34]; onVisible != geometryOnVisible) {
					using OnVisibleFn = void (*)(RE::NiAVObject*, RE::NiCullingProcess*, std::int32_t);
					reinterpret_cast<OnVisibleFn>(onVisible)(const_cast<RE::BSGeometry*>(geometry), a_process, a_arg);
				} else {
					a_process->AppendVirtual(*const_cast<RE::BSGeometry*>(geometry), a_arg);
				}
				++(member.engine ? out.engineMembers : out.unbound);
				Diagnostics::Notify(Diagnostics::StandInHandedOver{ a_slot, e, m, geometry });
			}
		}
		return true;
	}

	void PrimaryCull::AfterListJobs()
	{
		// The animation job runs between two culls, so its updates are complete now.
		{
			std::scoped_lock lock(animatedMutex);
			animatedBatch.swap(animatedNodes);
			animatedNodes.clear();
			batchInputs = std::exchange(animatedInputs, {});
		}
		Diagnostics::Notify<Diagnostics::EnginePoint::ListJobsDone>();
		if (!frameLive.exchange(false, std::memory_order_acq_rel))
			return;
		const std::int64_t start = Now();
		frameVisible.clear();
		for (std::uint32_t i = 0; i < cut.processCount; ++i) {
			auto& out = jobOut[i];
			frameVisible.insert(frameVisible.end(), out.visible.begin(), out.visible.end());
			auto& s = cutStats;
			s.seen += out.seen, s.skipped += out.skipped, s.visibleEntries += out.visibleEntries, s.notSettled += out.notSettled;
			s.walked += out.walked;
			s.notAdmitted += out.notAdmitted;
			s.hiddenSkipped += out.hidden;
			s.engineMembers += out.engineMembers;
			s.unbound += out.unbound;
			s.undrawable += out.undrawable;
			s.excluded += out.excluded;
			s.mixed += out.mixed;
			s.switchStale += out.switchStale;
			s.unselected += out.unselected;
		}
		cutStats.members += frameVisible.size();
		// No activeLightMask clear: the shadow lights' registrations leave an owned geometry's 0 (SunAccumulation, ClearOwnedMask).
		Diagnostics::Notify<Diagnostics::EnginePoint::ListJobsMerged>();
		cutStats.afterTicks += Now() - start;
	}

	void PrimaryCull::AfterFullFrustum()
	{
		ZoneScopedN("CS.DCLF.AfterFullFrustum");
		frameLive.store(false, std::memory_order_relaxed);
		gpuSunFrame = false;
		SunAccumulation::Get().EndFullFrustumWindow();
		if (ListsFiltered() && !SunAccumulation::Get().ExclusionLive() && !Global<std::uint8_t>(Engine::Address::kSunOff))
			++listStats.unexcluded;
		// CS_DCLF_PARITY_BOTH: the engine culls and registers everything.
		if (!!Policy::WithholdFromEngine())
			PrepareFrame();
		Diagnostics::Notify<Diagnostics::EnginePoint::FullFrustumDone>();
	}

	std::uint32_t PrimaryCull::SunShadowStatic(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property)
	{
		// Settled state, like the synthetic pass: a member's fade is the GPU's.
		return StaticShadowBits(a_leaf, true, a_property);
	}

	namespace
	{
		thread_local std::uint8_t syntheticFail = 0;
	}

	std::uint8_t PrimaryCull::LastSyntheticFail()
	{
		return syntheticFail;
	}

	bool PrimaryCull::SyntheticPass(const SceneCapture::LeafView& a_leaf, std::uint32_t a_derivedPass, AccumulatedPass& a_out,
		const SceneCapture::PropertyRecord* a_layer)
	{
		syntheticFail = 1;
		if (a_derivedPass == kNotDerived)
			return false;
		// The mirror's records.
		const auto* lighting = SceneCapture::LeafView::Lighting(a_layer ? a_layer : a_leaf.property);
		syntheticFail = 2;
		if (!lighting)
			return false;
		const std::uint64_t flags = lighting->flags;
		const bool blended = a_leaf.AlphaBlending();
		// Settled state, not the current fade: GetRenderPasses (1414adfb0) draws a screen-door fade of an unblended opaque material
		// as the plain opaque pass with alpha = materialAlpha. Translucent objects take hints 1 and 9; not modelled.
		const bool translucent = lighting->materialAlpha < 1.0f || blended;
		// The bits taken inside a cascade; BuildDraws drops them on a miss (kObjectSunTest). A layer takes none: the registrations
		// write light masks on the main property alone (FUN_1414b2140).
		const std::uint32_t sun = a_layer ? 0u : SunShadowStatic(a_leaf);
		std::uint32_t hint = 0;
		syntheticFail = a_layer ? 4 : 5;
		if (a_layer) {
			// Every pass of a layer: hint 12 (FUN_1414b2330). A translucent one is not modelled.
			if (translucent)
				return false;
			hint = kLayerHint;
		} else if (flags & 0x8004ull) {
			// Refraction (kRefraction, kTempRefraction): one Utility RENDER_NORMAL pass, hint 4 (group 5), whatever the alpha; drawn
			// by the forward epoch from the refraction range (CS_DCLF_REFRACTION).
			if (!ActiveToggles().refraction || !ActiveToggles().forward)
				return false;
			hint = 4;
		} else if (flags & 0xc000000ull)
			hint = translucent ? 3 : 2;  // the decal groups
		else if (translucent && !ActiveToggles().forward)
			return false;
		else if (translucent)
			hint = 1;  // the blended group (FUN_1414b2330's hint 1), drawn by the forward epoch
		else if (flags & 0x8000000600000000ull)
			hint = (~static_cast<std::uint32_t>(flags >> 33) & 1u) | 6u;
		else if (flags & (1ull << 61))
			hint = 11;  // TreeAnim
		else if (!(sun & 0x2000u))
			hint = 15;  // opaque with no sun shadow work
		syntheticFail = 0;
		a_out = {};
		a_out.subPass = PassCapture::SubPassOf(a_leaf.AlphaTesting(), flags);
		a_out.technique = DrawnPassDescriptor((a_derivedPass & ~kShadowBits) | sun, a_out.subPass);
		a_out.passEnum = a_out.technique + 0x4800002Du;
		a_out.hint = hint;
		a_out.lodRow = SceneCapture::LodRowOf(a_leaf, lighting);
		a_out.sunTest = (sun & 0x2000u) != 0;
		return true;
	}

	struct PrimaryCull::Hooks
	{
		/**
		 * @brief BSGeometryListCullingProcess::Process1 (vtable slot 0x16, AE 0x140e28390), called for each list entry after the
		 * first and every child an OnVisible culls. The vtable is shared with the sun's and every other list process; only the scene
		 * lists' processes are stood in for.
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
		 * @brief FUN_1402cff60's call of FUN_14147a160: the animation job updates an animated reference's fade node the cull did not
		 * reach (kAccumulated clear), from the main camera.
		 */
		struct AnimatedFade
		{
			static void thunk(void* a_node, float a_amount, const float* a_camera)
			{
				auto& cull = PrimaryCull::Get();
				// FadeStateCS makes the same update on the GPU (the animated batch), for every root.
				cull.NoteAnimatedFade(a_node, a_camera);
				// One writer: the root keeps the write-back's state; the gate reads its currentFade.
				if (cull.FadeWrittenByDclf(a_node)) {
					cull.gateFadeSkipped.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				func(a_node, a_amount, a_camera);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Main::Update's portal walk (FUN_1401a43c0): DCLF's walk follows, same camera, before the list build. */
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
				std::uint64_t result;
				{
					// The render thread's wait for the main camera's list jobs.
					ZoneScopedN("CS.DCLF.ListJobsWait");
					result = func(a_1, a_2, a_3, a_4);
				}
				PrimaryCull::Get().AfterListJobs();
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	namespace
	{
		/**
		 * @brief BSFadeNode::OnVisible (AE 0x141479f50) without its stores, for a root DCLF writes. Control flow kept (fades off, or
		 * settled with kIgnoreFade at fade and amount 1: children; camera updates with a LOD-type-6 node in its special state:
		 * children; faded out or zero amount: none), but the fade update (FUN_14147a160 or the type-6 step) and +0x13C are skipped.
		 */
		void WrittenFadeOnVisible(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
		{
			using NodeOnVisible = void (*)(void*, RE::NiCullingProcess*, std::int32_t);
			static const auto children = reinterpret_cast<NodeOnVisible>(REL::Module::get().base() + kNodeOnVisible);
			const float one = Global<float>(Engine::Address::kFloatOne);
			const float amount = At<float>(a_node, kFadeAmount);
			const bool settled = (At<std::uint32_t>(a_node, kObjectFlags) & kFlagFadeSettled) && amount == one && At<float>(a_node, kCurrentFade) == one;
			if (!Global<std::uint8_t>(Engine::Address::kFadesOn) || settled) {
				children(a_node, a_process, a_arg);
				return;
			}
			if (At<std::uint8_t>(a_process, kProcessCameraUpdates) && (At<std::uint8_t>(a_node, kFadeLodType) & 0xF) == 6 &&
				!(Global<float>(Engine::Address::kFadeSpecialA) != Global<float>(Engine::Address::kFadeSpecialB))) {
				children(a_node, a_process, a_arg);
				return;
			}
			if (At<float>(a_node, kCurrentFade) <= 0.0f || amount == 0.0f)
				return;
			children(a_node, a_process, a_arg);
		}

		struct FadeOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				auto& cull = PrimaryCull::Get();
				if (!cull.FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				cull.NoteOnVisibleFadeSkipped();
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSLeafAnimNode::OnVisible (AE 0x14147c9c0): for a root DCLF writes, its leaf LOD step and +0x13C store skipped. */
		struct LeafOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				auto& cull = PrimaryCull::Get();
				if (!cull.FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				cull.NoteOnVisibleFadeSkipped();
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSTreeNode::OnVisible (AE 0x14147d3c0): for a root DCLF writes, the engine's height test kept (a tree above the limit is
		 * not visited), BSLeafAnimNode's without its stores, and the LOD fix-up's stores (+0x153, +0x14C) skipped.
		 */
		struct TreeOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				auto& cull = PrimaryCull::Get();
				if (!cull.FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				cull.NoteOnVisibleFadeSkipped();
				if (At<std::uint8_t>(a_process, kProcessTreeHeightTest) && Global<std::uint8_t>(kTreeHeightTestOn) &&
					At<float>(a_node, kNodeWorldZ) - Global<float>(kTreeHeightBase) > Global<float>(kTreeHeightLimit))
					return;
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool PrimaryCull::WrittenByTables(const void* a_tables, const void* a_node)
	{
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		const auto it = tables.fadeRootIndex.find(a_node);
		return it != tables.fadeRootIndex.end() && it->second < tables.fadeRoots.size() && (tables.fadeRoots[it->second].bits & kFadeRootWritten);
	}

	void PrimaryCull::PublishWriterTables(bool a_running)
	{
		// The replaced tables are released after publication: the last cycle was waited for, so no OnVisible or gate reads them.
		std::shared_ptr<const void> tables;
		if (a_running)
			tables = Scene::FrameView::Get().AcceptedTables();
		fadeWriterTables.store(tables.get(), std::memory_order_release);
		fadeWriterTablesHeld = std::move(tables);
	}

	void PrimaryCull::Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		const auto afterFullFrustum = base + kAfterFullFrustumCallSite;
		const auto finish = base + kListJobsFinishCallSite;
		if (!CallsTo(afterFullFrustum, base + kAfterFullFrustum) || !CallsTo(finish, base + Engine::Address::kListJobsFinish) || !CallsTo(base + kPortalWalkCallSite, base + kPortalWalk)) {
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
		if (CallsTo(base + kAnimatedFadeCallSite, base + Engine::Address::kFadeUpdate))
			stl::write_thunk_call<Hooks::AnimatedFade>(base + kAnimatedFadeCallSite);
		else
			logger::warn("[DCLF] primary cull: the animation job's fade update is not where expected");
		stl::write_vfunc<0x16, Hooks::Process1>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		stl::write_vfunc<0x18, Hooks::AppendVirtual>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		InstallSceneLists();
		TreeAnimation::Install();
		Diagnostics::Notify<Diagnostics::EnginePoint::PrimaryCullInstalled>();
		LocalLightCull::Install();
		// Fade OnVisibles leave DCLF-written roots alone (detours, not vtable slots, so subclass tables and FadeState::PlanOf keep
		// the engine's addresses).
		stl::detour_thunk<FadeOnVisible>(base + Engine::Address::kFadeOnVisible);
		stl::detour_thunk<LeafOnVisible>(base + Engine::Address::kLeafOnVisible);
		stl::detour_thunk<TreeOnVisible>(base + Engine::Address::kTreeOnVisible);
		REL::Relocation<std::uintptr_t> triShape{ RE::VTABLE_BSTriShape[0] };
		geometryOnVisible = reinterpret_cast<const std::uintptr_t*>(triShape.address())[0x34];
		// The OnVisibles PlanOf stands for, from the classes' vtables. A slot may hold a hook ending in the engine's function
		// (NiNode's: LocalLightCull's category filter; NiBillboardNode's: the billboard observer under CS_DCLF_SET_PARITY), so both
		// the slot and the engine's NiNode::OnVisible (which subclasses keep) count. The startup log names each slot's module.
		const auto slot = [](std::uintptr_t a_vtable) { return reinterpret_cast<const std::uintptr_t*>(a_vtable)[0x34]; };
		wideOnVisible.node = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_NiNode[0] }.address());
		wideOnVisible.engineNode = base + kNodeOnVisible;
		wideOnVisible.multiBound = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSMultiBoundNode[0] }.address());
		wideOnVisible.switchNode = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_NiSwitchNode[0] }.address());
		wideOnVisible.billboard = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_NiBillboardNode[0] }.address());
		wideOnVisible.ordered = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSOrderedNode[0] }.address());
		wideOnVisible.dynamic = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSDynamicTriShape[0] }.address());
		wideOnVisible.subIndex = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSSubIndexTriShape[0] }.address());
		wideOnVisible.segmented = slot(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSSegmentedTriShape[0] }.address());
		const auto where = [base](std::uintptr_t a_function) {
			HMODULE module = nullptr;
			char path[MAX_PATH]{};
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a_function), &module) &&
				module && GetModuleFileNameA(module, path, MAX_PATH)) {
				const std::string_view name(path);
				const auto cut = name.find_last_of("\\/");
				return fmt::format("{:#x} ({})", a_function - (reinterpret_cast<std::uintptr_t>(module) == base ? base : reinterpret_cast<std::uintptr_t>(module)),
					cut == std::string_view::npos ? name : name.substr(cut + 1));
			}
			return fmt::format("{:#x}", a_function);
		};
		logger::info("[DCLF] culls off (T7f item 8): OnVisible slots: NiNode {}, BSMultiBoundNode {}, NiSwitchNode {}, NiBillboardNode {}, BSOrderedNode {}, BSGeometry {}, "
					 "BSDynamicTriShape {}, BSSubIndexTriShape {}, BSSegmentedTriShape {}",
			where(wideOnVisible.node), where(wideOnVisible.multiBound), where(wideOnVisible.switchNode), where(wideOnVisible.billboard), where(wideOnVisible.ordered),
			where(geometryOnVisible), where(wideOnVisible.dynamic), where(wideOnVisible.subIndex), where(wideOnVisible.segmented));
		installed = true;
		logger::info("[DCLF] primary cull installed (the scene lists after the full-frustum cull, the list jobs' Finish)");
	}
}
