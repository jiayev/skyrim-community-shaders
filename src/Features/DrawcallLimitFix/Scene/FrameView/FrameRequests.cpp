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
		// Cell 3D category nodes Drawcall Limit Fix draws from (engine notes: cell 3D category nodes).
		// The Actor node holds the actors of an exterior (an interior moves them into its rooms). It is
		// walked whatever CS_DCLF_ACTORS says: the toggle is ClassifyFrame's Actor rule, so it can change live.
		constexpr std::array<std::uint32_t, 4> kDrawnCategories{ 0 /*Actor*/, 3 /*Static*/, 4 /*Dynamic*/, 5 /*MultiBound*/ };
	}

	void FrameView::ServeFrameRequests(bool a_running)
	{
		ZoneScopedN("CS.DCLF.FrameRequests");
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::SceneCaptures);
		// The switch catch-ups the passes asked for (engine code that writes, before the culls: CatchUpSwitches), and every switch in the
		// world on the first frame after a load. The requests' references dropped here, after.
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
			// The category nodes as the passes will diff them: made again when the signature moved, a pass saw a detach (a
			// detach can take a category node with it before the signature sees its cell go), or a load ended (every cell is new).
			const bool detached = channels.categoryDetachSeen.exchange(false, std::memory_order_acq_rel);
			CaptureCategories(std::exchange(categoryCapturePending, false) || detached);
			// What the passes found the mirror lacking, and the mirror's observers' slice.
			CaptureMirrorRequests();
			Diagnostics::Notify(Diagnostics::MirrorProbePoint{ *this });
			// The fade nodes the coordinator holds no reference to, read from the properties where the engine reads them.
			ServeFadeNodeRequests();
		}
		// Tree LOD's mirror is the render thread's (DecideTreeLod reads it at the frame's start).
		treeLod.Drain(frame);
	}

	void FrameView::ServeFadeNodeRequests()
	{
		// Render thread, the frame's start: each request's geometry's property read live, as GetRenderPasses reads its fade node to
		// draw it. A geometry in the world whose property still names the requested node is drawn with that node, so the node is alive here:
		// its reference is taken and posted back. Otherwise the answer is none (the coordinator asks again for the node its mirror names
		// next). The requests' geometry references are dropped here, on the render thread.
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
		// Every switch under a_from (a whole subtree: no bound, unlike an event's walk).
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
		// The set's content changes only when a cell attaches or detaches, so it is made again when the signature says something
		// moved or when a detach was seen (walk parity finds anything both miss).
		ZoneScopedN("CS.DCLF.Ingest.CaptureCategories");
		const auto start = std::chrono::steady_clock::now();
		const auto timed = [&] { categoryCaptureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); };
		// The captures one the coordinator has applied supersedes are let go, their pins with them (the coordinator let go
		// of its copy before it moved the generation on, so these drops are the render thread's, as an engine reference's must be).
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
			// References the engine moves out of the category nodes into multibounds (exteriors) and the
			// portal graph's rooms (interiors), all under ObjectLODRoot (engine notes: cell multibounds and rooms).
			if (auto* multiBound = loaded->multiBoundNode.get())
				current.insert(multiBound);
			if (auto* graph = loaded->portalGraph.get()) {
				for (auto& room : graph->rooms) {
					if (room)
						current.insert(room.get());
				}
				if (auto* shared = graph->portalSharedNode.get()) {
					current.insert(shared);
					// References the graph draws with no parent (Bleak Falls Barrow's chamber pieces and stairs): each root is
					// filed under the shared node (FindCategoryNode). Held, so a root that leaves the list is still there to
					// walk when what was tracked under it is dropped.
					for (const auto& child : graph->alwaysRenderChildren) {
						if (child && !child->parent)
							currentRoots.try_emplace(child.get(), CategoryCapture::Root{ child, shared });
					}
				}
			}
		};

		if (auto* tes = RE::TES::GetSingleton()) {
			// Multibounds: the engine moves references out of the category nodes into BSMultiBoundNodes
			// under ObjectLODRoot (TES::objRoot), in exteriors and (holding the rooms) in interiors
			// (engine notes: multibounds and rooms).
			if (auto* objRoot = tes->objRoot) {
				for (auto& child : objRoot->GetChildren()) {
					if (auto* multiBound = child ? netimmerse_cast<RE::BSMultiBoundNode*>(child.get()) : nullptr)
						current.insert(multiBound);
				}
			}
			// Object and terrain LOD: TES::lodLandRoot holds the terrain manager's LOD blocks (a BSMultiBoundNode per block: object
			// LOD's BSSubIndexTriShapes, one segment per cell, and the land's BSTriShapes), attached and detached as the camera moves
			// (dclf-lod.md). The root is the category node; what attaches under it is tracked by the same structural events as a
			// cell's content. A class whose toggle is off classifies as Lod.
			if ((ActiveToggles().lodObjects || ActiveToggles().lodTerrain) && tes->lodLandRoot)
				current.insert(tes->lodLandRoot);
			Engine::ForEachLoadedCell(*tes, addCell);
		}
		// CS_DCLF_FIRST_PERSON: the player's first-person skeleton (PlayerCharacter +0x8F8, Load3D: no parent; Main::unk_FPCulling and
		// RenderDepth cull it alone). Its root is a category node (the walk up from its geometry ends there) and a root filed under itself
		// (the parentless roots' path: the mirror counts it in the world, SetDrawnRoots, and its attaches - the equipment - reach the
		// tracker as events). Its kHidden is view-scoped: ClassifyFrame stops at it.
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
		// What the scene work will walk: the category nodes new to its set, their children's keys and their subtrees held, and
		// the subtrees of the roots new to it. Its set is the capture it applied last (RefreshCategoryNodes makes them equal), read
		// here from the render thread's own copy, never the coordinator's; none applied yet, or a rescan pending, makes every one new.
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
					// A geometry's properties too (what its entry holds).
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
		// The portal graph's parentless roots, the mirror's world from now on: each new one captured whole, each gone dropped
		// from it (the last capture holds it), both after the events pushed before them (onto the tracker's stack, mirror-only;
		// the captures that follow are the hooks', InWorld).
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
		// Held until one the coordinator applied supersedes it, and posted (latest wins: a newer capture names every node new to the set
		// the coordinator applied last, so one it never takes loses nothing). After the pushes above: a pass that takes it (at its start,
		// TakeCategoryCapture) drains them in the same pass. The post wakes the pump.
		categoryCapturesHeld.push_back(capture);
		channels.categoryPosted.Post(std::make_unique<std::shared_ptr<const CategoryCapture>>(capture));
		categoryCapture = std::move(capture);
		rescanCaptureAll = false;
		WakeScenePass(SceneWake::Capture);
		timed();
	}

	void FrameView::CaptureMirrorRequests()
	{
		// What the scene work tracked that the mirror holds no record of (an attach no hook captured: the references the engine
		// moves into multibounds). Render thread, the frame's start. The coordinator asked because its mirror lacked the record;
		// the mirror is not read here: the requested object is captured with its subtree and its chain up to the world's root
		// (CaptureAttached), unless an ancestor was captured whole just before, and pushed onto the tracker's stack, mirror-only, in order
		// with the hooks' events.
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

	/**
	 * @brief The accumulator half of the frame's render-thread work, at EarlyPrepass: the material part, a frame input and a parity
	 * observer. The records are the scene work's (RefreshMaterialRecords: the writers' captures, the frame components,
	 * the texture transforms); the frame inputs and the other observers are PostAccumulateInputs'; the joins are the scene
	 * pass's, on the scene lane.
	 */
	void FrameView::PrepareAccumulatePhase()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Prepare");
		// No sceneBuilt test (the coordinator's, and the scene pass may be running): a request is answered whether or not a scene
		// is built, and the queue is empty when no join ran.
		// The frame input: the captures the joins asked for (a material no writer or attach captured lately), read where the engine
		// reads its materials.
		ServeMaterialRequests();
		Diagnostics::Notify(Diagnostics::AccumulatePrepared{ *this });
	}

	void FrameView::ServeMaterialRequests()
	{
		channels.materialRequests.Drain([&](MaterialRequest&& a_request) {
			// A material the scene work holds no capture of: captured here, off the property the join read (it holds the
			// property, the property its material), for the scene work's next pass. A property with another material now was swapped
			// after the join's batch: the request is stale (the join asks again after the swap's). Answered either way, so the join may
			// ask again; the property's reference is dropped here, on the render thread.
			const bool current = a_request.property && a_request.property->material == a_request.material;
			if (current)
				MaterialPort::PushCapture(a_request.material);
			channels.materialRequestsAnswered.Push(MaterialAnswer{ a_request.material, current });
		});
	}

	void FrameView::PostAccumulateInputs()
	{
		ZoneScopedN("CS.DCLF.Accumulate.Inputs");
		// The capture drained (always: its buffer is fixed-capacity, and the drain takes the frame's withholding counters); what the
		// registrations say is the observers' alone (DrainCapture), posted for the next scene pass (the pass runs from the frame's start,
		// so this frame's drain is the next pass's), and after the registration jobs the observers' reads of the engine's light data.
		DrainCapture();
		Diagnostics::Notify(Diagnostics::RegistrationsDone{ *this });
		// The joins' frame input: Light Limit Fix's room map, posted when its generation moves.
		PostRoomMap();
	}

	void FrameView::DrainCapture()
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained: the capture buffer is fixed-capacity and a frame that does not drain it overflows, and the drain takes the
		// frame's withholding counters (the report's). The main camera's registrations are no source of anything DCLF draws (scene
		// membership and the mirror are): what was registered is the observers' alone.
		const auto entries = capture.Drain();
		Diagnostics::Notify(Diagnostics::CaptureDrained{ *this, entries });
	}

	void FrameView::PostRoomMap()
	{
		// Light Limit Fix's map is the render thread's (it swaps it at its own point), so it is read here and only the copy is posted.
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
		// No wake: the frame's next pass takes it (a mid-frame post the pass does not need sooner).
	}

	void FrameView::PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame)
	{
		channels.pipelineFramePosted.store(std::make_shared<const GeometryPort::PipelineFrame>(a_frame), std::memory_order_release);
		channels.accumulateInputs.pipelineFramesPosted.fetch_add(1, std::memory_order_relaxed);
		// No wake: the frame's next pass takes it (a mid-frame post the pass does not need sooner).
	}

	void FrameView::CaptureLightingShader()
	{
		// The BSLightingShader instance: the engine's own (0x14338ca00, written by its constructor; the shader GetRenderPasses gives
		// a Lighting pass), not learned from a registration. Read at the frame's start with the coordinator idle, before the frame's
		// globals (MaterialPort's sources read it) and the scene work's kick, so the scene lane reads only what was set before its kick.
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
		// A cheap stand-in for "the set of category nodes may have changed". It has to be conservative in
		// one direction only: if it misses a change, geometry attached under a newly appeared node is
		// never tracked, because AddSubtree needs the category node to already be known. So it folds in
		// everything the refresh below reads to *find* nodes - the interior cell, the grid cells, each
		// cell's loaded data and cell3D and how many children it has, and objRoot's child count - rather
		// than just the cell pointers.
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
			// The portal graph's parentless roots (RefreshCategoryNodes): a few dozen pointers.
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
			// The LOD root (RefreshCategoryNodes), with the toggles that list it.
			mix(ActiveToggles().lodObjects || ActiveToggles().lodTerrain ? tes->lodLandRoot : nullptr);
			hash.Mix((ActiveToggles().lodObjects ? 1u : 0u) | (ActiveToggles().lodTerrain ? 2u : 0u));
			// The first-person root while the toggle tracks it (CaptureCategories).
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
