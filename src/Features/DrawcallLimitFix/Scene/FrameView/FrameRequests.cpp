// FrameView's service of the coordinator's requests and its engine captures: switch catch-ups, fade node and material requests,
// the category capture, the mirror's capture requests, the EarlyPrepass inputs (capture drain, room map) and the Lighting shader and
// accumulator latches. Render thread: everything here reads or writes live engine state.

#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Runtime/Hash.h"
#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Engine/Access/Walkers.h"
#include "Features/DrawcallLimitFix/Runtime/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPort.h"
#include "Features/LightLimitFix.h"

namespace DCLF::Scene
{
	namespace
	{
		// Cell 3D category nodes DCLF draws from (engine notes: cell 3D category nodes). The Actor node holds an exterior's actors
		// (an interior moves them into its rooms); walked whatever CS_DCLF_ACTORS says, since that toggle is ClassifyFrame's and live.
		constexpr std::array<std::uint32_t, 4> kDrawnCategories{ 0 /*Actor*/, 3 /*Static*/, 4 /*Dynamic*/, 5 /*MultiBound*/ };
	}

	void FrameView::ServeFrameRequests(bool a_running)
	{
		ZoneScopedN("CS.DCLF.FrameRequests");
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::SceneCaptures);
		// The passes' switch catch-ups (engine writes, before the culls), and every switch after a load. The requests' references
		// are dropped here, on the render thread.
		{
			std::vector<std::shared_ptr<SwitchCatchUp>> requests;
			channels.switchCatchUps.Drain([&requests](std::shared_ptr<SwitchCatchUp>&& a_request) {
				if (a_request)
					requests.push_back(std::move(a_request));
			});
			for (const auto& request : requests) {
				attachedRoots.clear();
				for (const auto& root : request->attached)
					attachedRoots.push_back(root.get());
				CatchUpSwitches(attachedRoots, request->switches);
			}
			attachedRoots.clear();
			if (worldCatchUpPending)
				CatchUpSwitches({}, {});
		}
		if (a_running) {
			// Forced after a detach (it can take a category node before the signature sees its cell go) or a load.
			const bool detached = channels.categoryDetachSeen.exchange(false, std::memory_order_acq_rel);
			CaptureCategories(std::exchange(categoryCapturePending, false) || detached);
			CaptureMirrorRequests();
			Diagnostics::Notify<Diagnostics::ScenePoint::MirrorProbePoint>();
			ServeFadeNodeRequests();
		}
		// Tree LOD's mirror is the render thread's (DecideTreeLod reads it at frame start).
		treeLod.Drain(frame);
	}

	void FrameView::ServeFadeNodeRequests()
	{
		// Each geometry's property read live, as GetRenderPasses reads its fade node. An in-world geometry whose property still names
		// the node draws with it, so the node is alive: its reference is posted back. Otherwise the answer is none (the coordinator
		// asks again for the node its mirror names next). The geometry references are dropped here, on the render thread.
		bool answered = false;
		channels.fadeNodeRequests.Drain([&](FadeNodeRequest&& a_request) {
			FadeNodeAnswer answer;
			answer.geometry = a_request.geometry.get();
			answer.fadeNode = a_request.fadeNode;
			if (auto* geometry = a_request.geometry.get(); geometry && SceneCapture::InWorld(geometry))
				if (auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get(); property && property->fadeNode && property->fadeNode == a_request.fadeNode)
					answer.node.reset(property->fadeNode);
			channels.fadeNodeAnswers.Push(std::move(answer));
			answered = true;
		});
		if (answered)
			WakeScenePass(SceneWake::Capture);
	}

	void FrameView::CatchUpSwitches(std::span<RE::NiAVObject* const> a_attached, std::span<const SwitchEvent> a_switches)
	{
		const bool world = std::exchange(worldCatchUpPending, false);
		if (!world && a_attached.empty() && a_switches.empty())
			return;
		ZoneScopedN("CS.DCLF.Ingest.SwitchCatchUps");
		const auto start = std::chrono::steady_clock::now();
		const auto* root = static_cast<const RE::NiAVObject*>(RE::Main::WorldRootNode());
		auto inWorld = [root](const RE::NiAVObject* a_object) {
			for (std::uint32_t depth = 0; root && a_object && depth <= Engine::kMaxParentDepth; ++depth, a_object = a_object->parent)
				if (a_object == root)
					return true;
			return false;
		};
		// Every switch in a_from's whole subtree.
		std::vector<RE::NiAVObject*> stack;
		auto catchUpUnder = [&](RE::NiAVObject* a_from, std::atomic<std::uint64_t>& a_count) {
			stack.assign(1, a_from);
			std::uint64_t count = 0;
			while (!stack.empty()) {
				auto* object = stack.back();
				stack.pop_back();
				auto* node = object ? object->AsNode() : nullptr;
				if (!node)
					continue;
				if (auto* switchNode = node->AsSwitchNode(); switchNode && CatchUpSwitch(*switchNode))
					++count;
				for (auto& child : node->GetChildren())
					if (child)
						stack.push_back(child.get());
			}
			a_count.fetch_add(count, std::memory_order_relaxed);
		};
		if (world && root)
			catchUpUnder(const_cast<RE::NiAVObject*>(root), channels.catchUpsByAttach);
		for (auto* attachedRoot : a_attached)
			if (inWorld(attachedRoot))
				catchUpUnder(attachedRoot, channels.catchUpsByAttach);
		for (const auto& event : a_switches) {
			auto* node = event.node.get();
			auto* switchNode = node ? node->AsSwitchNode() : nullptr;
			if (!switchNode || !inWorld(node) || (!event.structural && SwitchIndexOf(node) == event.before))
				continue;
			if (CatchUpSwitch(*switchNode))
				channels.catchUpsBySwitch.fetch_add(1, std::memory_order_relaxed);
		}
		channels.catchUpNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	bool FrameView::CatchUpSwitch(RE::NiSwitchNode& a_switch)
	{
		// NiSwitchNode::OnVisible: childRevID.SetAt(index, revID) (FUN_140d29990), then the child's UpdateDownwardPass
		// (vtable slot 0x2C) with NiUpdateData { savedTime (+0x130), flags bit 1 of +0x128 as the update flag }.
		const auto state = Engine::ReadSwitch(a_switch);
		const auto selection = Engine::SelectionOf(a_switch, state);
		if (!selection.child || selection.current)
			return false;
		auto* base = reinterpret_cast<std::byte*>(&a_switch);
		using SetRevision = void (*)(void*, std::uint32_t, const std::uint32_t*);
		static const REL::Relocation<SetRevision> setRevision{ REL::Offset(Engine::Address::kSwitchSetRevision) };
		setRevision(base + 0x138, selection.index, reinterpret_cast<const std::uint32_t*>(base + 0x134));
		struct UpdateData
		{
			float time;
			std::uint32_t flags;
		} data{ *reinterpret_cast<const float*>(base + 0x130), (state.flags >> 1) & 1u };
		auto* child = selection.child;
		using UpdateDownwardPass = void (*)(RE::NiAVObject*, UpdateData*, std::uint32_t);
		(*reinterpret_cast<UpdateDownwardPass* const*>(child))[0x2C](child, &data, 0);
		return true;
	}

	bool FrameView::TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out)
	{
		a_out.clear();
		a_out.swap(frameSwitchChanges);
		return std::exchange(frameSwitchResync, false);
	}

	void FrameView::CaptureCategories(bool a_force)
	{
		// The set changes only when a cell attaches or detaches: remade when the signature moved or a detach was seen (walk parity
		// finds anything both miss).
		ZoneScopedN("CS.DCLF.Ingest.CaptureCategories");
		const auto start = std::chrono::steady_clock::now();
		const auto timed = [&] { categoryCaptureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); };
		// Drop captures superseded by one the coordinator applied, pins included. The coordinator let go of its copy before moving
		// the generation, so these engine references are released here, on the render thread.
		const std::uint64_t applied = channels.categoryAppliedGeneration.load(std::memory_order_acquire);
		while (categoryCapturesHeld.size() > 1 && categoryCapturesHeld.front()->generation < applied)
			categoryCapturesHeld.pop_front();
		const std::uint64_t signature = CategorySignature();
		if (!a_force && categoryCapture && signature == categoryCapture->signature)
			return timed();
		++categoryCapturesMade;
		auto capture = std::make_shared<CategoryCapture>();
		capture->generation = ++categoryCaptures;
		capture->signature = signature;
		auto& current = capture->nodes;
		auto& currentRoots = capture->roots;

		auto addCell = [&](RE::TESObjectCELL* a_cell) {
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			if (!loaded || !loaded->cell3D)
				return;
			auto& children = loaded->cell3D->GetChildren();
			for (auto category : kDrawnCategories) {
				const auto index = static_cast<std::uint16_t>(category);
				if (index < children.free_idx() && children[index]) {
					if (auto* node = children[index]->AsNode())
						current.insert(node);
				}
			}
			// References the engine moves out of the category nodes into multibounds (exteriors) and portal graph rooms
			// (interiors), all under ObjectLODRoot (engine notes: cell multibounds and rooms).
			if (auto* multiBound = loaded->multiBoundNode.get())
				current.insert(multiBound);
			if (auto* graph = loaded->portalGraph.get()) {
				for (auto& room : graph->rooms) {
					if (room)
						current.insert(room.get());
				}
				if (auto* shared = graph->portalSharedNode.get()) {
					current.insert(shared);
					// Parentless references the graph draws (e.g. Bleak Falls Barrow's chamber pieces), filed under the shared node
					// (FindCategoryNode). Held, so a root that leaves the list can still be walked when its tracked content is dropped.
					for (const auto& child : graph->alwaysRenderChildren) {
						if (child && !child->parent)
							currentRoots.try_emplace(child.get(), CategoryCapture::Root{ child, shared });
					}
				}
			}
		};

		if (auto* tes = RE::TES::GetSingleton()) {
			// BSMultiBoundNodes under ObjectLODRoot (TES::objRoot), in exteriors and (holding the rooms) interiors.
			if (auto* objRoot = tes->objRoot) {
				for (auto& child : objRoot->GetChildren()) {
					if (auto* multiBound = child ? netimmerse_cast<RE::BSMultiBoundNode*>(child.get()) : nullptr)
						current.insert(multiBound);
				}
			}
			// Object and terrain LOD: TES::lodLandRoot holds the terrain manager's LOD blocks (a BSMultiBoundNode each: object LOD's
			// BSSubIndexTriShapes, one segment per cell, and the land's BSTriShapes), attached and detached as the camera moves
			// (dclf-lod.md). It is the category node; its content is tracked by the usual structural events. A class whose toggle is
			// off classifies as Lod.
			if ((ActiveToggles().lodObjects || ActiveToggles().lodTerrain) && tes->lodLandRoot)
				current.insert(tes->lodLandRoot);
			Engine::ForEachLoadedCell(*tes, addCell);
		}
		// CS_DCLF_FIRST_PERSON: the player's first-person skeleton (PlayerCharacter +0x8F8, Load3D: no parent; Main::unk_FPCulling and
		// RenderDepth cull it alone). Its root is a category node and a parentless root filed under itself, so the mirror counts it
		// in the world (SetDrawnRoots) and its equipment attaches reach the tracker as events. Its kHidden is view-scoped:
		// ClassifyFrame stops at it.
		if (ActiveToggles().firstPerson)
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				if (auto* firstPerson = player->Get3D(true); firstPerson && firstPerson != player->Get3D(false))
					if (auto* node = firstPerson->AsNode()) {
						current.insert(node);
						currentRoots.try_emplace(node, CategoryCapture::Root{ RE::NiPointer<RE::NiAVObject>(node), node });
						capture->firstPersonRoot = node;
					}
		capture->held.reserve(current.size());
		for (auto* node : current)
			capture->held.emplace_back(node);
		// Pin what the scene work will walk: category nodes new to its set (children's keys, subtrees) and new roots' subtrees. Its
		// set is the capture it applied last, read from the render thread's own copy; none applied, or a rescan, makes all new.
		{
			const CategoryCapture* base = nullptr;
			for (const auto& held : categoryCapturesHeld)
				if (held->generation == applied)
					base = held.get();
			const bool all = rescanCaptureAll || !base;
			auto& pins = capture->pins;
			auto pinSubtree = [&](RE::NiAVObject* a_root) {
				Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t) {
					pins.emplace_back(&a_object);
					// And a geometry's properties (what its entry holds).
					if (auto* geometry = a_object.AsGeometry()) {
						const auto& data = geometry->GetGeometryRuntimeData();
						for (RE::NiRefObject* property : { static_cast<RE::NiRefObject*>(data.shaderProperty.get()), static_cast<RE::NiRefObject*>(LayerPropertyOf(*geometry)),
								 static_cast<RE::NiRefObject*>(data.alphaProperty.get()) })
							if (property)
								pins.emplace_back(property);
					}
				});
			};
			for (auto* node : current) {
				if (!all && base->nodes.contains(node))
					continue;
				auto& keys = capture->children[node];
				for (auto& child : node->GetChildren())
					if (child) {
						keys.push_back(child.get());
						pinSubtree(child.get());
					}
			}
			for (const auto& [root, entry] : currentRoots)
				if (all || !base->roots.contains(root) || !base->nodes.contains(entry.category))
					pinSubtree(entry.root.get());
		}
		// The parentless roots are the mirror's world from now on: each new one captured whole, each gone one detached (the last
		// capture still holds it), pushed onto the tracker's stack mirror-only, after the events already there.
		{
			auto& tracker = SceneTracker::Get();
			std::vector<const RE::NiAVObject*> roots;
			for (const auto& [root, entry] : currentRoots)
				roots.push_back(root);
			SceneCapture::SetDrawnRoots(roots);
			if (categoryCapture)
				for (const auto& [root, entry] : categoryCapture->roots)
					if (!currentRoots.contains(root)) {
						auto* event = new SceneTracker::Event{};
						event->type = SceneTracker::EventType::Detached;
						SceneTracker::CollectGeometry(entry.root.get(), event->removed, &event->removedNodes);
						event->detachedRoot = root;
						tracker.PushCaptured(event);
					}
			for (const auto& [root, entry] : currentRoots)
				if (!categoryCapture || !categoryCapture->roots.contains(root))
					if (auto records = SceneCapture::CaptureAttached(*root)) {
						categoryMirrorRecords += records->nodes.size();
						++categoryMirrorCaptures;
						auto* event = new SceneTracker::Event{};
						event->type = SceneTracker::EventType::Attached;
						event->captured = std::move(records);
						tracker.PushCaptured(event);
					}
		}
		// Held until superseded by an applied one, and posted latest-wins (a newer capture names every node new to the last applied
		// set, so a skipped one loses nothing). Posted after the pushes above, so the pass that takes it drains them too.
		categoryCapturesHeld.push_back(capture);
		channels.categoryPosted.Post(std::make_unique<std::shared_ptr<const CategoryCapture>>(capture));
		categoryCapture = std::move(capture);
		rescanCaptureAll = false;
		WakeScenePass(SceneWake::Capture);
		timed();
	}

	void FrameView::CaptureMirrorRequests()
	{
		// Objects the mirror lacks (an attach no hook captured, e.g. references moved into multibounds): each captured with its
		// subtree and chain to the world root (CaptureAttached) unless an ancestor was just captured, and pushed onto the tracker's
		// stack, mirror-only, in order with the hooks' events.
		std::vector<RE::NiPointer<RE::NiAVObject>> requests;
		channels.mirrorCaptureRequests.Drain([&requests](RE::NiPointer<RE::NiAVObject>&& a_request) { requests.push_back(std::move(a_request)); });
		if (requests.empty())
			return;
		ZoneScopedN("CS.DCLF.Ingest.MirrorRequests");
		auto& tracker = SceneTracker::Get();
		ankerl::unordered_dense::set<const RE::NiAVObject*> captured;
		for (const auto& object : requests) {  // a geometry, or (AddSubtree) a subtree's root
			if (!object)
				continue;
			bool covered = false;
			for (const RE::NiAVObject* at = object.get(); at && !covered; at = at->parent)
				covered = captured.contains(at);
			if (covered)
				continue;
			captured.insert(object.get());
			if (auto records = SceneCapture::CaptureAttached(*object)) {
				categoryMirrorRecords += records->nodes.size();
				++categoryMirrorCaptures;
				auto* event = new SceneTracker::Event{};
				event->type = SceneTracker::EventType::Attached;
				event->captured = std::move(records);
				tracker.PushCaptured(event, true);
			}
		}
	}

	void FrameView::PrepareAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Prepare");
		// No sceneBuilt test (the coordinator's, and the scene pass may be running): requests are answered regardless, and the
		// queue is empty when no join ran.
		ServeMaterialRequests();
		Diagnostics::Notify<Diagnostics::ScenePoint::AccumulatePrepared>();
	}

	void FrameView::ServeMaterialRequests()
	{
		channels.materialRequests.Drain([&](MaterialRequest&& a_request) {
			// Captured off the property the join held. A property whose material changed since is stale (the join asks again
			// after the swap). Answered either way; the property's reference is dropped here, on the render thread.
			const bool current = a_request.property && a_request.property->material == a_request.material;
			if (current)
				MaterialPort::PushCapture(a_request.material);
			channels.materialRequestsAnswered.Push(MaterialAnswer{ a_request.material, current });
		});
	}

	void FrameView::PostAccumulateInputs()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Inputs");
		DrainCapture();
		Diagnostics::Notify<Diagnostics::FramePoint::RegistrationsDone>();
		PostRoomMap();
	}

	void FrameView::DrainCapture()
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained: the buffer is fixed-capacity and would overflow, and the drain takes the withholding counters. The main
		// camera's registrations are no source of anything DCLF draws (membership and the mirror are): only observers read them.
		const auto entries = capture.Drain();
		Diagnostics::Notify(Diagnostics::CaptureDrained{ *this, entries });
	}

	void FrameView::PostRoomMap()
	{
		// Light Limit Fix's map is the render thread's (it swaps it at its own point): only a copy is posted.
		auto& lightFix = globals::features::lightLimitFix;
		if (!lightFix.loaded)
			return;
		const std::uint64_t generation = lightFix.GetRoomMapGeneration();
		if (generation == roomMapPostedGeneration)
			return;
		auto map = std::make_shared<ankerl::unordered_dense::map<const RE::NiNode*, int>>();
		map->reserve(lightFix.roomNodes.size());
		for (const auto& [node, index] : lightFix.roomNodes)
			map->emplace(node, index);
		auto input = std::make_shared<RoomMapInput>();
		input->map = std::move(map);
		input->generation = generation;
		channels.roomMapPosted.store(std::shared_ptr<const RoomMapInput>(std::move(input)), std::memory_order_release);
		roomMapPostedGeneration = generation;
		channels.accumulateInputs.roomMapsPosted.fetch_add(1, std::memory_order_relaxed);
		// No wake: the frame's next pass takes it.
	}

	void FrameView::PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame)
	{
		channels.pipelineFramePosted.store(std::make_shared<const GeometryPort::PipelineFrame>(a_frame), std::memory_order_release);
		channels.accumulateInputs.pipelineFramesPosted.fetch_add(1, std::memory_order_relaxed);
		// No wake: the frame's next pass takes it.
	}

	void FrameView::CaptureLightingShader()
	{
		// The engine's BSLightingShader instance (0x14338ca00, written by its constructor; the shader GetRenderPasses gives a Lighting
		// pass). Read at frame start before the globals (MaterialPort's sources read it) and the frame inputs' post, so the scene
		// lane reads only what was set before that post.
		if (EngineShaders::Lighting())
			return;
		static const REL::Relocation<RE::BSShader**> lightingShader{ REL::Offset(Engine::Address::kLightingShader) };
		if (auto* shader = *lightingShader.get(); shader && shader->shaderType.get() == RE::BSShader::Type::Lighting)
			EngineShaders::SetLighting(shader);
	}

	void FrameView::LatchAccumulator()
	{
		auto* accumulator = *globals::game::currentAccumulator.get();
		if (!accumulator || accumulator == latchedAccumulator)
			return;
		if (latchedAccumulator) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::warn("[DCLF] the main camera's accumulator changed ({} -> {}); the tables follow it",
					static_cast<const void*>(latchedAccumulator), static_cast<const void*>(accumulator));
			}
		}
		latchedAccumulator = accumulator;
	}

	std::uint64_t FrameView::CategorySignature() const
	{
		// A cheap "the category nodes may have changed". A missed change means geometry under a new node is never tracked
		// (AddSubtree needs the category node known), so it folds in everything CaptureCategories reads to *find* nodes, not
		// just the cell pointers.
		Fnv1a hash;
		auto mix = [&hash](const void* a_value) { hash.Mix(a_value); };
		auto mixCell = [&](RE::TESObjectCELL* a_cell) {
			mix(a_cell);
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			mix(loaded);
			RE::NiNode* cell3D = loaded ? loaded->cell3D.get() : nullptr;
			mix(cell3D);
			if (cell3D)
				hash.Mix(cell3D->GetChildren().size());
			// The portal graph's parentless roots: a few dozen pointers.
			if (const auto* graph = loaded ? loaded->portalGraph.get() : nullptr) {
				mix(graph->portalSharedNode.get());
				hash.Mix(graph->alwaysRenderChildren.size());
				for (const auto& child : graph->alwaysRenderChildren) {
					mix(child.get());
					mix(child ? child->parent : nullptr);
				}
			}
		};
		if (auto* tes = RE::TES::GetSingleton()) {
			if (auto* objRoot = tes->objRoot)
				hash.Mix(objRoot->GetChildren().size());
			mix(tes->objRoot);
			// The LOD root, with the toggles that list it.
			mix(ActiveToggles().lodObjects || ActiveToggles().lodTerrain ? tes->lodLandRoot : nullptr);
			hash.Mix((ActiveToggles().lodObjects ? 1u : 0u) | (ActiveToggles().lodTerrain ? 2u : 0u));
			// The first-person root while the toggle tracks it.
			if (ActiveToggles().firstPerson)
				if (auto* player = RE::PlayerCharacter::GetSingleton()) {
					mix(player->Get3D(true));
					mix(player->Get3D(false));
				}
			mix(tes->interiorCell);
			Engine::ForEachLoadedCell(*tes, mixCell);
		}
		return hash.value;
	}
}
