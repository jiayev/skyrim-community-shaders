// PrimaryCull: the list jobs' stand-in (Process1, AppendVirtual; job threads), the render thread's frame around them, the cut's
// plans and admission, and the synthetic membership passes (any thread).
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"

#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include <Tracy/Tracy.hpp>
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

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

		// AE 1.6.1170 and SE 1.5.97 (module offsets).
		constexpr Engine::Offset kAfterFullFrustumCallSite{ 0x14cbcea, 0x12e26ab };  // CalculateAndDrawShadowCasterLights -> FUN_1414a0840, after either branch
		constexpr Engine::Offset kAfterFullFrustum{ 0x14a0840, 0x12b9200 };
		constexpr Engine::Offset kListJobsFinishCallSite{ 0x14cbf4d, 0x12e293d };  // ... -> JobList::Finish(scene list culling)
		constexpr Engine::Offset kProcess1{ 0xe28390, 0xd50c80 };  // BSCullingProcess::Process1, the list processes' vtable slot 0x16
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;  // byte: the accumulator draws the deferred shadow mask
		// The main camera: the world scene graph's (+0x128), carried by the list processes.
		constexpr std::size_t kSceneGraphCamera = 0x128;
		// BSTreeNode::OnVisible (AE 0x14147d3c0): the height test (the stand-in's, TreeAboveLimit) and the LOD fix-up.
		constexpr std::size_t kProcessTreeHeightTest = 0x121;     // byte on the culling process
		constexpr Engine::Offset kTreeHeightTestOn{ 0x2032fb8, 0x1e0de18 };   // byte
		constexpr Engine::Offset kTreeHeightBase{ 0x332a2f0, 0x31d0df8 };     // float
		constexpr Engine::Offset kTreeHeightLimit{ 0x2032fa0, 0x1e0de00 };    // float
		constexpr std::size_t kTreeLodData = 0x180;               // pointer; its +0x12C count
		// The NiNode OnVisible BSFadeNode's ends in (its children's cull).
		constexpr Engine::Offset kNodeOnVisible{ 0xd1e2c0, 0xc58c70 };       // NiNode::OnVisible
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
		 * @brief A material port member's main pass as its property's GetRenderPasses would register it (hint, the port's pass enum, LOD row;
		 * the Effect port's: BSEffectShaderProperty::GetRenderPasses, 0x1414bd890). Not cached: MembershipDerived's witnesses miss the alpha
		 * property and texture paths; it is cheap.
		 */
		bool PortMembershipPass(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord& a_port, AccumulatedPass& a_out, std::uint32_t* a_derived)
		{
			LightingDescriptors descriptors;
			const bool derived = Extensions::DerivePort(a_port.port, a_port, a_leaf, descriptors) == Ineligible::None;
			if (a_derived)
				*a_derived = derived ? descriptors.derivedPass : kNotDerived;
			if (!derived)
				return false;
			a_out = {};
			a_out.subPass = PassCapture::SubPassOf(a_leaf.AlphaTesting(), a_port.flags);
			a_out.technique = descriptors.derivedPass;
			a_out.passEnum = descriptors.derivedPass + Extensions::PortRules(a_port.port).passEnumBase;
			// GetRenderPasses' hint: 0 opaque, 2 or 3 decal (by whether its alpha blends), 1 blended.
			a_out.hint = descriptors.opaqueInDecalGroup ? 0u : descriptors.decalGroup ? descriptors.decalGroup + 1 : 1u;
			a_out.lodRow = SceneCapture::LodRowOf(a_leaf, &a_port);
			return true;
		}
	}

	bool PrimaryCull::FreshSyntheticPass(const SceneCapture::LeafView& a_leaf, AccumulatedPass& a_out)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (const auto* port = lighting ? nullptr : SceneCapture::LeafView::Port(a_leaf.property); port && Extensions::PortOn(ActiveToggles(), port->port)) {
			if (!PortMembershipPass(a_leaf, *port, a_out, nullptr))
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

	std::uint32_t PrimaryCull::MembershipWitness(const FrameGlobals& a_g)
	{
		std::uint32_t witness = 2166136261u;
		const auto mix = [&](std::uint32_t a_value) { witness = (witness ^ a_value) * 16777619u; };
		// The static sun bits (SunShadowStatic).
		mix(a_g.noSunShadowDir | (a_g.accumulator && a_g.accumulatorDeferredShadow ? 2u : 0u) | (a_g.screenDoorFades ? 4u : 0u));
		// The root test's globals (the fade distances: Api::RootStatesContribution::globalsKey).
		Extensions::RootStatesGlobalsKey(a_g, witness);
		return witness;
	}

	PrimaryCull::Membership PrimaryCull::MembershipPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached)
	{
		Membership answer;
		answer.derived = a_cached;
		answer.fail = 2;  // not lighting (SyntheticPass's index)
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (const auto* port = lighting ? nullptr : SceneCapture::LeafView::Port(a_leaf.property); port && Extensions::PortOn(ActiveToggles(), port->port)) {
			std::uint32_t derived = kNotDerived;
			answer.ok = PortMembershipPass(a_leaf, *port, answer.pass, &derived);
			answer.derived = { port->key, port->material, port->flags, 0, derived };
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

	namespace
	{
		const char* PlanNameOf(const RE::NiAVObject* a_object)
		{
			const auto* rtti = a_object->GetRTTI();
			return rtti && rtti->name ? rtti->name : "no RTTI";
		}
	}

	PrimaryCull::EntryPlan PrimaryCull::PlanEntry(const EntrySpace& a_candidates, const RE::NiAVObject* a_root, PlannedEntry& a_out) const
	{
		// Each node and geometry is judged by its OnVisible slot (wideOnVisible), not its RTTI. A geometry the snapshot has as DCLF's
		// (Eligibility::PrimaryVerdict) is a member; a hidden static's is left out; any other is the engine's and makes the entry mixed.
		std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> path;
		const char* rejected = nullptr;
		const RE::BSGeometry* mixedBy = nullptr;
		std::uint32_t ours = 0, hidden = 0;
		const auto visit = [&](const auto& a_self, const RE::NiAVObject* a_object, bool a_isRoot) -> void {
			if (rejected || !a_object)
				return;
			const std::uintptr_t onVisible = (*reinterpret_cast<const std::uintptr_t* const*>(a_object))[0x34];
			if (const auto* geometry = const_cast<RE::NiAVObject*>(a_object)->AsGeometry()) {
				const bool appends = onVisible == geometryOnVisible || onVisible == wideOnVisible.dynamic || onVisible == wideOnVisible.subIndex ||
				                     onVisible == wideOnVisible.segmented;
				if (!appends) {
					rejected = PlanNameOf(a_object);
					return;
				}
				const auto it = a_candidates.geometries.find(geometry);
				const std::uint8_t verdict = it != a_candidates.geometries.end() && it->second < a_candidates.primaryGeometry.size() ? a_candidates.primaryGeometry[it->second] : Eligibility::kPrimaryEngine;
				// A hidden static's geometry: neither; unhiding changes the verdict and replans the entry.
				if (verdict == Eligibility::kPrimaryHiddenStatic) {
					++hidden;
					return;
				}
				const bool mine = verdict == Eligibility::kPrimaryMember;
				const auto begin = static_cast<std::uint32_t>(a_out.switchPaths.size());
				a_out.switchPaths.insert(a_out.switchPaths.end(), path.begin(), path.end());
				a_out.members.push_back({ geometry, !mine, begin, static_cast<std::uint32_t>(a_out.switchPaths.size()) });
				// The object the snapshot's walk had (the frame reads no tracked set).
				a_out.memberObject.push_back(mine && it != a_candidates.geometries.end() && it->second < a_candidates.geometrySlot.size() ? a_candidates.geometrySlot[it->second] : -1);
				ours += mine ? 1 : 0;
				if (!mine && !mixedBy)
					mixedBy = geometry;
				return;
			}
			auto* node = const_cast<RE::NiAVObject*>(a_object)->AsNode();
			if (node && !a_isRoot && onVisible == wideOnVisible.switchNode) {
				const auto* switchNode = static_cast<const RE::NiSwitchNode*>(node);
				a_out.switches.push_back(switchNode);
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
				rejected = PlanNameOf(a_object);
				return;
			}
			for (const auto& child : node->GetChildren())
				a_self(a_self, child.get(), false);
		};
		visit(visit, a_root, true);
		return ConcludePlan(a_root, rejected, mixedBy, ours, hidden, a_out);
	}

	PrimaryCull::EntryPlan PrimaryCull::ConcludePlan(const RE::NiAVObject* a_root, const char* a_rejected, const RE::BSGeometry* a_mixedBy, std::uint32_t a_ours,
		std::uint32_t a_hidden, PlannedEntry& a_out)
	{
		// Nothing of DCLF's: the engine's, unless it holds only hidden statics (a marker), which nobody draws until unhidden.
		const bool empty = !a_ours && !(a_hidden && !a_mixedBy);
		a_out.rejectReason = a_rejected ? a_rejected : empty ? "no DCLF geometry" : nullptr;
		a_out.mixedBy = a_rejected || empty || !a_mixedBy ? std::string() :
		                                                    fmt::format("'{}' ({})", a_mixedBy->name.c_str() ? a_mixedBy->name.c_str() : "", PlanNameOf(a_mixedBy));
		if (a_rejected || empty) {
			a_out.members.clear();
			a_out.memberObject.clear();
			a_out.switchPaths.clear();
			a_out.switches.clear();
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
		if (camera) {
			// A camera cut (a jump or snap turn the verdict-to-write latency cannot follow): every DCLF-written root visible until a
			// batch of this frame or later decodes (IndirectDraws::NoteCameraCut). Turn angle from the relative rotation's trace.
			const auto& r = camera->world.rotate.entry;
			const auto& t = camera->world.translate;
			const std::array<float, 12> now{ t.x, t.y, t.z, r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2] };
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
			if (!ownedRoots.empty() || !standInRoots.empty())
				ClearRootOwnership();
			frameLive.store(true, std::memory_order_release);
			return;
		}
		// The sun's exclusion is not a precondition: listed roots' fade is DCLF's alone, and a whole entry the sun's cull reaches
		// without it has its casters' passes withheld (the Sun source's accumulation, PassCapture).
		if (!candidates) {
			++(candidates ? cutStats.skippedPreconditions : cutStats.skippedStale);
			liveStale = true;
			// The engine culls the stood-in roots and writes their kAccumulated this frame; the next decode writes them all.
			if (!ownedRoots.empty())
				IndirectDraws::Get().RequestAccumulatedRefresh();
			frameLive.store(true, std::memory_order_release);
			return;
		}
		const std::int64_t start = Now();
		// The plans the pool made since, spliced in first.
		TakePlans();
		// Entries whose versions moved since the last snapshot are planned again (SyncCut); the rest stand.
		const bool newSnapshot = cut.candidates != candidates;
		std::vector<std::uint32_t> changedEntries;
		if (newSnapshot) {
			ChangedEntriesByLog(cut.candidates.get(), *candidates, changedEntries);
			SyncCut(candidates, changedEntries);
		}
		Diagnostics::Notify<Diagnostics::EnginePoint::CutSynced>();
		// Switch events name the entries to read again (dclf-architecture.md, "Switch nodes"); a resync or skipped frame reads all.
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
		// Entries refused a lease, or queued by the list filter's sync: sent again.
		KickPlans();
		cutStats.awaiting += cut.planQueue.size() + plansOutstanding;
		standInLive = true;
		walkEverything = Policy::WalkEveryEntry();
		// Walk flags for entries planned again (once memberLive is current) and entries a member joined or left.
		for (const std::uint32_t e : std::exchange(walkRefresh, {}))
			RefreshWalk(e);
		// Entries whose members are all in the set leave the engine's cull from this frame.
		RunAdmission();
		// Fade roots: the entries a new snapshot planned again or admission admitted.
		if (!ownDirty.empty())
			SyncRootOwnership();
		++cutStats.appliedFrames;
		cutStats.prepareTicks += Now() - start;
		// The list jobs are queued after this returns, which orders everything above before their reads.
		frameLive.store(true, std::memory_order_release);
		gpuSunFrame = true;
	}

	bool PrimaryCull::Owned(const RE::BSGeometry& a_geometry) const
	{
		return frameSet && (frameSet->PhasesOf(&a_geometry) & kSetMain) != 0;
	}

	void PrimaryCull::NoteSetChanges(const std::vector<const RE::BSGeometry*>& a_joined, const std::vector<const RE::BSGeometry*>& a_left)
	{
		// A joining geometry is not main-registered, so nothing clears the light bits it gathered as the engine's: cleared once here,
		// then kept 0 (the Sun source's registration hook, ClearOwnedMask). Only those the installed publication claims (and so holds alive).
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

	void PrimaryCull::SyncCut(const std::shared_ptr<const EntrySpace>& a_candidates, std::span<const std::uint32_t> a_changed, bool a_inline)
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
		grow(cut.awaiting, std::uint8_t(kPlanNone));
		grow(cut.ticket, std::uint64_t(0));
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
			NoteFilterEntry(e);
			ownDirty.push_back(e);
			const auto* root = e < capacity ? candidates.entryNodes[e] : nullptr;
			cut.roots[e] = root;
			// A plan in flight for the old entry is stale from here.
			cut.ticket[e] = ++planTickets;
			cut.awaiting[e] = kPlanNone;
			if (!root)
				continue;
			// Planned again (PlanEntry): on the pool, Rejected until spliced in, or here.
			if (!a_inline) {
				cut.awaiting[e] = kPlanQueued;
				cut.planQueue.push_back(e);
				continue;
			}
			PlannedEntry planned;
			planned.plan = PlanEntry(candidates, root, planned);
			SplicePlan(e, planned);
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
		cut.awaiting.resize(capacity);
		cut.ticket.resize(capacity);
		std::erase_if(cut.pendingAdmission, [capacity](std::uint32_t a_e) { return a_e >= capacity; });
		std::erase_if(walkRefresh, [capacity](std::uint32_t a_e) { return a_e >= capacity; });
		std::erase_if(cut.planQueue, [&](std::uint32_t a_e) { return a_e >= capacity || cut.awaiting[a_e] != kPlanQueued; });
		// Repack once garbage exceeds live members (amortised: one copy per member let go).
		if (cutGarbage > cut.members.size() / 2)
			CompactCut();
		// Frames that sync the cut but stop before SyncRootOwnership: the entries to judge kept once each.
		if (ownDirty.size() > 2 * std::size_t(capacity) + 1024) {
			std::sort(ownDirty.begin(), ownDirty.end());
			ownDirty.erase(std::unique(ownDirty.begin(), ownDirty.end()), ownDirty.end());
		}
		if (!a_inline)
			KickPlans();
	}

	void PrimaryCull::SplicePlan(std::uint32_t a_e, PlannedEntry& a_planned)
	{
		const std::uint32_t e = a_e;
		const auto* root = cut.roots[e];
		cut.plans[e] = a_planned.plan;
		cut.rejectReason[e] = a_planned.rejectReason;
		cut.mixedBy[e] = std::move(a_planned.mixedBy);
		cut.memberBegin[e] = cut.memberEnd[e] = static_cast<std::uint32_t>(cut.members.size());
		cut.switchBegin[e] = cut.switchEnd[e] = static_cast<std::uint32_t>(cut.switches.size());
		NoteFilterEntry(e);
		ownDirty.push_back(e);
		if (cut.plans[e] == EntryPlan::Rejected)
			return;
		// Its members and switches appended to the pools, their switch paths rebased.
		const auto pathBase = static_cast<std::uint32_t>(cut.switchPaths.size());
		cut.switchPaths.insert(cut.switchPaths.end(), a_planned.switchPaths.begin(), a_planned.switchPaths.end());
		for (auto member : a_planned.members) {
			member.switchBegin += pathBase;
			member.switchEnd += pathBase;
			cut.members.push_back(member);
		}
		cut.switches.insert(cut.switches.end(), a_planned.switches.begin(), a_planned.switches.end());
		cut.memberEnd[e] = static_cast<std::uint32_t>(cut.members.size());
		cut.switchEnd[e] = static_cast<std::uint32_t>(cut.switches.size());
		cut.memberObject.resize(cut.memberBegin[e], -1);
		cut.memberObject.insert(cut.memberObject.end(), a_planned.memberObject.begin(), a_planned.memberObject.end());
		cut.memberLive.resize(cut.members.size(), 1);
		for (std::uint32_t m = cut.memberBegin[e]; m < cut.memberEnd[e]; ++m)
			cut.mixed[e] |= cut.members[m].engine ? 1 : 0;
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

	void PrimaryCull::TakePlans()
	{
		plansReady.Drain([&](PlanBatch*&& a_batch) {
			std::unique_ptr<PlanBatch> batch(a_batch);
			plansOutstanding -= batch->entries.size();
			for (auto& planned : batch->entries) {
				const std::uint32_t e = planned.entry;
				// Still the let-go it was made for: same ticket, so the same root and an unchanged entry since (its members' objects stand).
				if (e >= cut.ticket.size() || cut.ticket[e] != planned.ticket || cut.awaiting[e] != kPlanInFlight) {
					++cutStats.plansStale;
					continue;
				}
				if (!planned.planned) {
					++cutStats.plansRefused;
					cut.awaiting[e] = kPlanQueued;
					cut.planQueue.push_back(e);
					continue;
				}
				cut.awaiting[e] = kPlanNone;
				SplicePlan(e, planned);
				++cutStats.plansSpliced;
			}
			// The batch's snapshot (and the roots it holds) is let go of here, on the render thread.
		});
	}

	void PrimaryCull::KickPlans()
	{
		if (cut.planQueue.empty() || !cut.candidates)
			return;
		auto batch = std::make_unique<PlanBatch>();
		batch->candidates = cut.candidates;
		batch->entries.reserve(cut.planQueue.size());
		for (const std::uint32_t e : std::exchange(cut.planQueue, {})) {
			if (e >= cut.awaiting.size() || cut.awaiting[e] != kPlanQueued)
				continue;
			cut.awaiting[e] = kPlanInFlight;
			auto& planned = batch->entries.emplace_back();
			planned.entry = e;
			planned.ticket = cut.ticket[e];
			planned.root = cut.roots[e];
		}
		if (batch->entries.empty())
			return;
		cutStats.plansKicked += batch->entries.size();
		plansOutstanding += batch->entries.size();
		// The subtree walks read engine memory: a lease per entry, so the window's Close waits on one entry at most. A refused one is
		// sent back unplanned and kicked again. The batch goes back whole to the render thread, which drops its snapshot.
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "CS.DCLF.CutPlans",
				[this, batch = batch.release()](const auto&) {
					ZoneScopedN("CS.DCLF.CutPlans");
					for (auto& planned : batch->entries) {
						const EngineReadWindow::Lease lease;
						if (!lease)
							continue;
						planned.plan = PlanEntry(*batch->candidates, planned.root, planned);
						planned.planned = true;
					}
					plansReady.Push(batch);
				}))
			stl::report_and_fail("Drawcall Limit Fix: the cut's plans were refused by DCLF's executor");
	}

	PrimaryCull::Cut PrimaryCull::PlanWholeCut()
	{
		// SyncCut on an empty cut, then the current cut put back.
		if (!cut.candidates)
			return {};
		Cut kept = std::move(cut);
		const auto keptWalkRefresh = walkRefresh;
		const auto keptGarbage = cutGarbage;
		const auto keptDirty = filterDirty;
		const bool keptDirtyAll = filterDirtyAll;
		const auto keptOwnDirty = ownDirty;
		cut = Cut{};
		cut.processes = kept.processes;
		cut.processCount = kept.processCount;
		cut.admittedRoots = kept.admittedRoots;
		std::vector<std::uint32_t> all;
		ChangedEntries(nullptr, *kept.candidates, all);
		SyncCut(kept.candidates, all, true);
		Cut whole = std::move(cut);
		cut = std::move(kept);
		walkRefresh = keptWalkRefresh;
		cutGarbage = keptGarbage;
		filterDirty = keptDirty;
		filterDirtyAll = keptDirtyAll;
		ownDirty = keptOwnDirty;
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
				NoteFilterEntry(e);
				ownDirty.push_back(e);
				++cutStats.admittedNow;
			}
		}
		cut.pendingAdmission = std::move(retry);
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
		if (cut.walk[a_e] != (walk ? 1 : 0))
			NoteFilterEntry(a_e);
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
				if (!cut.walk[e])
					NoteFilterEntry(e);
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
		filterDirtyAll = true;
		for (std::uint32_t e = 0; e < cut.walk.size(); ++e)
			walkRefresh.push_back(e);
	}

	std::uint8_t PrimaryCull::OwnStateOf(std::uint32_t a_e) const
	{
		if (a_e >= cut.plans.size() || !cut.roots[a_e] || !cut.admitted[a_e])
			return kOwnNone;
		switch (cut.plans[a_e]) {
		case EntryPlan::Plain:
			return cut.mixed[a_e] ? kOwnNone : kOwnPlain;
		case EntryPlan::FadeRoot:
		case EntryPlan::LeafRoot:
			return cut.mixed[a_e] ? kOwnMixedFade : kOwnFade;
		case EntryPlan::TreeRoot:
			return cut.mixed[a_e] ? kOwnMixedFade : kOwnTree;
		default:
			return kOwnNone;
		}
	}

	void PrimaryCull::SyncRootOwnership()
	{
		// Admitted entries with a fade root: their fade is FadeStateCS's from the next frame. A mixed one is culled by the engine,
		// which updates its node. Every other is stood in (state on the GPU alone) and, through the stand-in observers (Api::StandInObserver),
		// taken off the tree manager's animation list (Features/TreeWind's TreeAnimation: TreeWindCS animates its trees; the tree clock
		// reads BSLeafAnimNode's kAccumulated). Its kAccumulated is
		// DCLF's, written at Main::Update's entry (AtUpdateEntry); clearing it would stop FUN_1402cff60 animating a stood-in
		// reference that never fades. Until its first write it keeps the engine's last bit (a new listing is written visible).
		// A stood-in plain entry has no engine cull either: its kAccumulated is DCLF's. A stood-in BSTreeNode stays on the tree
		// manager's tree list, its loop given DCLF's verdict (TreeAnimation).
		// By delta: only the entries planned again or admitted (ownDirty) are judged; those whose state moved leave their old state's
		// sets first, then join their new one (a root that moved to another index within the delta stays).
		std::sort(ownDirty.begin(), ownDirty.end());
		ownDirty.erase(std::unique(ownDirty.begin(), ownDirty.end()), ownDirty.end());
		std::vector<std::uint32_t> joined;
		std::vector<const RE::NiAVObject*> animatedLeft, animatedJoined;
		bool ownedMoved = false;
		const auto erase = [](auto& a_list, auto& a_position, const void* a_root) {
			const auto it = a_position.find(a_root);
			if (it == a_position.end())
				return;
			const std::uint32_t at = it->second;
			a_position.erase(it);
			if (at + 1 != a_list.size()) {
				a_list[at] = std::move(a_list.back());
				a_position.insert_or_assign(KeyOf(a_list[at]), at);
			}
			a_list.pop_back();
		};
		for (const std::uint32_t e : std::exchange(ownDirty, {})) {
			if (ownState.size() <= e) {
				ownState.resize(e + 1, kOwnNone);
				ownRoot.resize(e + 1, nullptr);
			}
			const std::uint8_t state = OwnStateOf(e);
			const auto* root = state != kOwnNone ? cut.roots[e] : nullptr;
			if (state == ownState[e] && root == ownRoot[e])
				continue;
			if (const auto* old = ownRoot[e]) {
				const std::uint8_t was = ownState[e];
				if (was == kOwnPlain || was == kOwnFade || was == kOwnTree)
					standInRoots.erase(old);
				if (was == kOwnMixedFade || was == kOwnFade || was == kOwnTree) {
					erase(ownedRoots, ownedRootPosition, old);
					ownedMoved = true;
				}
				if (was == kOwnFade || was == kOwnTree) {
					erase(standInList, standInPosition, old);
					animatedLeft.push_back(old);
				}
			}
			ownState[e] = state;
			ownRoot[e] = root;
			if (root)
				joined.push_back(e);
		}
		for (const std::uint32_t e : joined) {
			const std::uint8_t state = ownState[e];
			const auto* root = ownRoot[e];
			if (state == kOwnPlain || state == kOwnFade || state == kOwnTree)
				standInRoots.insert(root);
			if (state == kOwnMixedFade || state == kOwnFade || state == kOwnTree) {
				if (const auto [it, added] = ownedRootPosition.try_emplace(root, static_cast<std::uint32_t>(ownedRoots.size())); added)
					ownedRoots.push_back({ root, state != kOwnMixedFade });
				else
					ownedRoots[it->second].standIn = state != kOwnMixedFade;
				ownedMoved = true;
			}
			if (state == kOwnFade || state == kOwnTree) {
				if (standInPosition.try_emplace(root, static_cast<std::uint32_t>(standInList.size())).second)
					standInList.push_back(root);
				animatedJoined.push_back(root);
			}
		}
		// The coordinator takes the owned set whole (latest wins, applied by its scene pass).
		if (ownedMoved)
			Scene::FrameView::Get().SetStandInRoots(ownedRoots);
		if (!animatedLeft.empty() || !animatedJoined.empty())
			Extensions::StandInRootsChanged({ animatedLeft, animatedJoined });
	}

	void PrimaryCull::ClearRootOwnership()
	{
		ownedRoots.clear();
		ownedRootPosition.clear();
		standInRoots.clear();
		standInList.clear();
		standInPosition.clear();
		Scene::FrameView::Get().SetStandInRoots({});
		Extensions::StandInRootsCleared();
		// Judged again whole when the stand-in comes back.
		std::fill(ownState.begin(), ownState.end(), kOwnNone);
		std::fill(ownRoot.begin(), ownRoot.end(), nullptr);
		ownDirty.clear();
		for (std::uint32_t e = 0; e < cut.plans.size(); ++e)
			ownDirty.push_back(e);
	}

	void PrimaryCull::AtUpdateEntry(bool a_running)
	{
		if (!installed)
			return;
		ZoneScopedN("CS.DCLF.AccumulatedWrites");
		// Roots no longer stood in are left to the engine's cull this frame.
		IndirectDraws::Get().WriteAccumulated(a_running, [this](const void* a_root) { return standInRoots.contains(a_root); });
		// The stand-in observers (the tree manager's loop, later in this Main::Update, takes the stood-in trees' visibility from those
		// writes); Features/Fades' fade writers last.
		Extensions::StandInUpdateEntry(a_running);
		Diagnostics::Notify(Diagnostics::UpdateEntry{ a_running });
		// The stand-in observers' late point, after the writes (Features/Actors: the high actors' animation stamp from DCLF's shadow views).
		Extensions::StandInAfterWrites(a_running);
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
		// (dclf-architecture.md, "LOD cross-fades": none in SE's settings); crossing roots (+0x153 bits 4-6 not 2) are counted.
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
				++out.inView;
				if (collectVisible)
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
		// The stand-in observers' list jobs point (Features/Fades: the animated fades' batch).
		Extensions::StandInListJobsDone();
		Diagnostics::Notify<Diagnostics::EnginePoint::ListJobsDone>();
		if (!frameLive.exchange(false, std::memory_order_acq_rel))
			return;
		const std::int64_t start = Now();
		frameVisible.clear();
		for (std::uint32_t i = 0; i < cut.processCount; ++i) {
			auto& out = jobOut[i];
			if (collectVisible)
				frameVisible.insert(frameVisible.end(), out.visible.begin(), out.visible.end());
			auto& s = cutStats;
			s.members += out.inView;
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
		// No activeLightMask clear: the shadow lights' registrations leave an owned geometry's 0 (the Sun source's registration hook, ClearOwnedMask).
		Diagnostics::Notify<Diagnostics::EnginePoint::ListJobsMerged>();
		cutStats.afterTicks += Now() - start;
	}

	void PrimaryCull::AfterFullFrustum()
	{
		ZoneScopedN("CS.DCLF.AfterFullFrustum");
		frameLive.store(false, std::memory_order_relaxed);
		gpuSunFrame = false;
		// The Sun source ends its frame's state (Features/Shadows/Sun) and says whether its exclusion applied.
		const auto& sun = Extensions::ShadowSourceOf(Api::ShadowSource::Sun);
		const bool live = sun.registered && sun.fullFrustumWindowEnd();
		if (ListsFiltered() && !live && !Global<std::uint8_t>(Engine::Address::kSunOff))
			++listStats.unexcluded;
		// DCLF switched off (this hook runs every frame): the main camera's cull is the engine's alone - no entry stood in, no owned
		// geometry withheld from its registration (frameLive stays false) - until DCLF runs again.
		if (!ShellState::Running()) {
			standInLive = false;
			// Nothing is owned: the engine draws every main pass, which takes its light bits from the shadow lights' registrations.
			frameSet = {};
			Diagnostics::Notify<Diagnostics::EnginePoint::FullFrustumDone>();
			return;
		}
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
			if (!Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Refraction) ||
			    !Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Forward))
				return false;
			hint = 4;
		} else if (flags & 0xc000000ull)
			hint = translucent ? 3 : 2;  // the decal groups
		else if (translucent && !Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Forward))
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
						++out.inView;
						if (self.collectVisible)
							out.visible.push_back(&a_geometry);
						++out.excluded;
						return;
					}
				func(a_process, a_geometry, a_arg);
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

	void PrimaryCull::Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		const auto afterFullFrustum = base + kAfterFullFrustumCallSite;
		const auto finish = base + kListJobsFinishCallSite;
		// The main walk's hook (Api::MainCullInputContribution: Features/Portals' early hooks) is in, or nothing would build its programs;
		// and the root states' (Api::RootStatesContribution: Features/Fades' early hooks), or nothing would list the roots it stands in for.
		if (!CallsTo(afterFullFrustum, base + kAfterFullFrustum) || !CallsTo(finish, base + Engine::Address::kListJobsFinish) || !Extensions::MainWalkInstalled() ||
			!Extensions::RootStatesInstalled()) {
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
		stl::write_vfunc<0x18, Hooks::AppendVirtual>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		InstallSceneLists();
		// The stand-in observers' engine hooks go in with the features' install (Api::InstallContext::RegisterStandInObserver: Features/Fades'
		// animation job's fade update and fade OnVisibles too).
		Extensions::NoteStandInInstalled();
		Diagnostics::Notify<Diagnostics::EnginePoint::PrimaryCullInstalled>();
		REL::Relocation<std::uintptr_t> triShape{ RE::VTABLE_BSTriShape[0] };
		geometryOnVisible = reinterpret_cast<const std::uintptr_t*>(triShape.address())[0x34];
		// The OnVisibles PlanOf stands for, from the classes' vtables. A slot may hold a hook ending in the engine's function
		// (NiNode's: the point lights' category filter, Features/Shadows/Local; NiBillboardNode's: the billboard observer under CS_DCLF_SET_PARITY), so both
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
