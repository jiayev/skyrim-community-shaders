// FrameView's service of the coordinator's requests and its engine captures: switch catch-ups, fade node and material requests,
// the category capture, the mirror's capture requests, the EarlyPrepass inputs (capture drain, ancestor map) and the Lighting shader and
// accumulator latches. Render thread: everything here reads or writes live engine state.

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Runtime/Hash.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"

namespace DCLF::Scene
{
	namespace
	{
		// Cell 3D category nodes DCLF draws from (engine notes: cell 3D category nodes). The Actor node holds an exterior's actors
		// (an interior moves them into its rooms); walked whatever the actors' toggle says (Features/Actors), since its parent contribution is ClassifyFrame's and live.
		constexpr std::array<std::uint32_t, 4> kDrawnCategories{ 0 /*Actor*/, 3 /*Static*/, 4 /*Dynamic*/, 5 /*MultiBound*/ };

		constexpr std::uint32_t kObjectClasses = static_cast<std::uint32_t>(Api::ObjectClass::Count);

		/**
		 * @brief The object classes on under a_toggles that list a category root (Api::ObjectClassContribution::categoryRoot: Features/Lod's
		 * object and terrain LOD, TES::lodLandRoot), a bit each by class (0: no root is listed).
		 */
		std::uint32_t RootListing(const ToggleSet& a_toggles)
		{
			const auto& roots = Extensions::RegisteredCategoryRoots();
			std::uint32_t listing = 0;
			for (std::uint32_t c = 0; c < kObjectClasses; ++c)
				if (roots[c] && Extensions::ObjectClassOn(a_toggles, static_cast<Api::ObjectClass>(c)))
					listing |= 1u << c;
			return listing;
		}

		/**
		 * @brief The frame start's engine reads that need no fixed engine moment and push nothing onto the tracker's stack, done on
		 * DCLF's pool inside the engine-read window (step C2): the category capture's pins, then its post; the fade node requests. Made by
		 * the render thread at the frame's start (ServeFrameRequests), one at a time.
		 *
		 * The mirror's own captures (the category capture's parentless roots, the mirror's capture requests) stay on the render thread:
		 * other threads attach and detach in the world inside the window (SceneTracker::TakeWindowEdits counts them), and a capture read
		 * here and pushed behind such an edit's event would be applied over it. The pins push nothing: the coordinator applies the capture
		 * before the batch it drains with it, and an attach or detach the pins' walk raced is in that batch or a later one.
		 *
		 * Every item takes its own lease (EngineReadWindow), so Close never waits on more than an item; a refused lease leaves the rest,
		 * by index, for the next window. What an item reads is alive: the category nodes and roots are held by the capture, the
		 * requests hold their objects. The references the task lets go of go to EngineReleases. Owned by the render thread, which drops it
		 * once the task is done.
		 */
		struct FrameStartJob
		{
			// The category capture (null: none this frame). The render thread already holds it (categoryCapture, categoryCapturesHeld) and
			// reads only its nodes, roots, generation and signature; the task writes its children and pins, then posts it.
			std::shared_ptr<CategoryCapture> capture;
			std::vector<RE::NiNode*> pinNodes;        // category nodes new to the coordinator's set: children's keys and subtrees
			std::vector<RE::NiAVObject*> pinRoots;    // roots new to it: subtrees
			// The coordinator's fade node requests, drained by the render thread.
			std::vector<FadeNodeRequest> fadeRequests;

			// The task's progress (the next item, by index over the lists above in order, the post after the category's) and results.
			std::size_t next = 0;
			std::atomic<bool> running{ false };

			std::size_t CategoryItems() const { return capture ? pinNodes.size() + pinRoots.size() + 1 : 0; }
			std::size_t Items() const { return CategoryItems() + fadeRequests.size(); }
			bool Empty() const { return !Items(); }
		};
		std::unique_ptr<FrameStartJob> frameStartJob;  // render thread: the job in flight or being made, null when none

		/** @brief One item of a_job (the pool, or the render thread under the parities' inline pass), under its lease. */
		void RunFrameStartItem(FrameStartJob& a_job, std::size_t a_item)
		{
			auto& channels = Channels::Get();
			if (a_job.capture) {
				auto& capture = *a_job.capture;
				// The category nodes and roots new to the coordinator's set: what its scene work will walk, pinned (the children's keys of a
				// node, the subtrees, and each geometry's properties: what its entry holds).
				auto pinSubtree = [&](RE::NiAVObject* a_root) {
					Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t) {
						capture.pins.emplace_back(&a_object);
						if (auto* geometry = a_object.AsGeometry()) {
							const auto& data = geometry->GetGeometryRuntimeData();
							for (RE::NiRefObject* property : { static_cast<RE::NiRefObject*>(data.shaderProperty.get()), static_cast<RE::NiRefObject*>(LayerPropertyOf(*geometry)),
									 static_cast<RE::NiRefObject*>(data.alphaProperty.get()) })
								if (property)
									capture.pins.emplace_back(property);
						}
					});
				};
				if (a_item < a_job.pinNodes.size()) {
					auto* node = a_job.pinNodes[a_item];
					auto& keys = capture.children[node];
					for (auto& child : node->GetChildren())
						if (child) {
							keys.push_back(child.get());
							pinSubtree(child.get());
						}
					return;
				}
				a_item -= a_job.pinNodes.size();
				if (a_item < a_job.pinRoots.size()) {
					pinSubtree(a_job.pinRoots[a_item]);
					return;
				}
				a_item -= a_job.pinRoots.size();
				if (a_item == 0) {
					// Posted latest-wins after the render thread's pushes for it, so the pass that takes it drains them too (a newer capture
					// names every node new to the last applied set, so a skipped one loses nothing; the render thread holds a replaced one).
					channels.categoryPosted.Post(std::make_unique<std::shared_ptr<const CategoryCapture>>(a_job.capture));
					WakeScenePass(SceneWake::Capture);
					return;
				}
				a_item -= 1;
			}
			if (a_item < a_job.fadeRequests.size()) {
				// The geometry's property read live, as GetRenderPasses reads its fade node. An in-world geometry whose property still names
				// the node draws with it, so the node is alive: its reference is posted back. Otherwise the answer is none (the coordinator
				// asks again for the node its mirror names next).
				auto& request = a_job.fadeRequests[a_item];
				FadeNodeAnswer answer;
				answer.geometry = request.geometry.get();
				answer.fadeNode = request.fadeNode;
				if (auto* geometry = request.geometry.get(); geometry && SceneCapture::InWorld(geometry))
					if (auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get(); property && property->fadeNode && property->fadeNode == request.fadeNode)
						answer.node.reset(property->fadeNode);
				channels.fadeNodeAnswers.Push(std::move(answer));
				EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(request.geometry)));
				if (a_item + 1 == a_job.fadeRequests.size())
					WakeScenePass(SceneWake::Capture);
			}
		}

		/** @brief The items left, each under its own lease, until one is refused (the window closed: the rest wait for the next). */
		void RunFrameStartJob(FrameStartJob& a_job)
		{
			ZoneScopedN("CS.DCLF.Ingest.FrameStartJob");
			for (const std::size_t items = a_job.Items(); a_job.next < items; ++a_job.next) {
				const EngineReadWindow::Lease lease;
				if (!lease)
					break;
				RunFrameStartItem(a_job, a_job.next);
			}
			a_job.running.store(false, std::memory_order_release);
		}

		/** @brief Render thread, inside the window: the job's items left, run on the pool (or here, under the parities' inline pass). */
		void DispatchFrameStartJob(FrameStartJob& a_job)
		{
			a_job.running.store(true, std::memory_order_release);
			if (Policy::ScenePassInline()) {
				RunFrameStartJob(a_job);
				return;
			}
			if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "frame-start captures",
					[job = &a_job](const auto&) { RunFrameStartJob(*job); }))
				stl::report_and_fail("Drawcall Limit Fix: the frame start's captures were refused by DCLF's executor");
		}

		/**
		 * @brief The engine pointers each CategoryRoot view's root is found from (Api::ViewRootCapture::inputs: the player's first- and
		 * third-person roots), into a_mix, for the views whose toggles are on; the views' callbacks, in slot order.
		 */
		template <class Mix>
		void MixViewRootInputs(Mix&& a_mix)
		{
			const auto& views = Extensions::Views();
			for (std::uint32_t i = 0; i < views.rootCount; ++i) {
				const auto view = views.rootViews[i];
				if (!Extensions::ViewOn(SceneToggles(), view))
					continue;
				Api::ViewRootCapture capture;
				Extensions::ViewEntryOf(view).captureRoot(capture);
				for (std::uint32_t n = 0; n < capture.inputCount; ++n)
					a_mix(capture.inputs[n]);
			}
		}

		/**
		 * @brief What CategorySignature folds in that costs O(1) to read (step C2): TES's roots, the grid's identity and centre, the
		 * interior, the CategoryRoot views' roots' inputs (the first- and third-person roots) and the toggles that list nodes. Compared
		 * every frame; the rest is read only after an edit of a watched node (SceneTracker::CategoryEdits) or a change here.
		 */
		std::uint64_t CategoryInputs()
		{
			Fnv1a hash;
			const auto& views = Extensions::Views();
			std::uint32_t listing = RootListing(SceneToggles());
			for (std::uint32_t i = 0; i < views.rootCount; ++i)
				listing |= Extensions::ViewOn(SceneToggles(), views.rootViews[i]) ? 1u << (kObjectClasses + i) : 0u;
			hash.Mix(listing);
			// The classes' category roots, listed or not.
			for (const auto root : Extensions::RegisteredCategoryRoots())
				if (root)
					hash.Mix(root());
			if (auto* tes = RE::TES::GetSingleton()) {
				hash.Mix(tes->objRoot);
				hash.Mix(tes->interiorCell);
				const auto* grid = tes->gridCells;
				hash.Mix(grid);
				if (grid) {
					hash.Mix((static_cast<std::uint64_t>(grid->unk08) << 32) | grid->unk0C);
					hash.Mix(grid->length);
					hash.Mix(grid->cells);
				}
			}
			MixViewRootInputs([&hash](const void* a_input) { hash.Mix(a_input); });
			return hash.value;
		}

		/**
		 * @brief A cheap "the category nodes may have changed". A missed change means geometry under a new node is never tracked
		 * (AddSubtree needs the category node known), so it folds in everything CaptureCategories reads to *find* nodes, not just the
		 * cell pointers. a_watch: the nodes whose attaches and detaches can change it (SceneTracker::WatchCategoryNodes).
		 */
		std::uint64_t ReadCategorySignature(std::vector<const void*>* a_watch)
		{
			Fnv1a hash;
			auto mix = [&hash](const void* a_value) { hash.Mix(a_value); };
			auto watch = [a_watch](const void* a_node) {
				if (a_watch && a_node)
					a_watch->push_back(a_node);
			};
			auto mixCell = [&](RE::TESObjectCELL* a_cell) {
				mix(a_cell);
				if (!a_cell || !a_cell->IsAttached())
					return;
				auto* loaded = a_cell->GetRuntimeData().loadedData;
				mix(loaded);
				RE::NiNode* cell3D = loaded ? loaded->cell3D.get() : nullptr;
				mix(cell3D);
				if (cell3D) {
					hash.Mix(cell3D->GetChildren().size());
					// Its category nodes come and go as its children; the cell itself as a child of its parent.
					watch(cell3D);
					watch(cell3D->parent);
				}
				// The portal graph's parentless roots: a few dozen pointers.
				if (const auto* graph = loaded ? loaded->portalGraph.get() : nullptr) {
					mix(graph->portalSharedNode.get());
					watch(graph->portalSharedNode.get());
					hash.Mix(graph->alwaysRenderChildren.size());
					for (const auto& child : graph->alwaysRenderChildren) {
						mix(child.get());
						mix(child ? child->parent : nullptr);
						watch(child.get());
					}
				}
			};
			if (auto* tes = RE::TES::GetSingleton()) {
				if (auto* objRoot = tes->objRoot)
					hash.Mix(objRoot->GetChildren().size());
				mix(tes->objRoot);
				watch(tes->objRoot);
				// The classes' category roots, with the classes that list them.
				const std::uint32_t rootListing = RootListing(SceneToggles());
				const auto& roots = Extensions::RegisteredCategoryRoots();
				for (std::uint32_t c = 0; c < kObjectClasses; ++c)
					if (roots[c])
						mix(rootListing & (1u << c) ? roots[c]() : nullptr);
				hash.Mix(rootListing);
				// The CategoryRoot views' roots' inputs while their toggles track them (the first-person root).
				MixViewRootInputs(mix);
				mix(tes->interiorCell);
				Engine::ForEachLoadedCell(*tes, mixCell);
			}
			// The scene's roots a cell's 3D attaches under (watched whether or not a cell is loaded yet).
			for (auto* sceneNode : RE::BSShaderManager::State::GetSingleton().shadowSceneNode)
				watch(sceneNode);
			return hash.value;
		}

		/**
		 * @brief The category signature's event-driven read (render thread, step C2): the inputs and the watched edits it was last read
		 * at, and (CS_DCLF_PERSISTENT_PARITY) the frames it was not read again checked against a full read.
		 */
		struct CategoryWatch
		{
			bool read = false;
			std::uint64_t edits = 0, inputs = 0, signature = 0;
			std::uint64_t frames = 0, reads = 0, checked = 0, missed = 0, jobs = 0, jobItems = 0, deferred = 0, resumed = 0;
			std::uint64_t windowRender = 0, windowOther = 0;
			std::uint32_t windowThread = 0;
		} categoryWatch;

		void ReportCategoryWatch()
		{
			auto& w = categoryWatch;
			if (!Diagnostics::Observe().scene)
				return;
			const auto edits = SceneTracker::TakeWindowEdits();
			w.windowRender += edits.renderThread;
			w.windowOther += edits.otherThreads;
			if (!w.windowThread)
				w.windowThread = edits.firstThread;
			if (++w.frames % 600)
				return;
			logger::info("[DCLF] frame-start captures by events (C2 parity): {} frames, the category signature read on {} (an edit or an input moved), {} others "
						 "checked against a full read, {} moved unseen{}; {} leased jobs ({} items; {} deferred behind a running job, {} resumed in a later window); "
						 "in-world attaches and detaches inside the read window (why the mirror's captures stay on the render thread): {} by the render thread, {} by others{}",
				w.frames, w.reads, w.checked, w.missed, w.missed ? " <- FLAG" : " <- OK", w.jobs, w.jobItems, w.deferred, w.resumed, w.windowRender, w.windowOther,
				w.windowRender || w.windowOther ? fmt::format(" (first on thread {})", w.windowThread) : std::string());
			w.frames = w.reads = w.checked = w.missed = w.jobs = w.jobItems = w.deferred = w.resumed = 0;
			w.windowRender = w.windowOther = 0;
			w.windowThread = 0;
		}

		/**
		 * @brief CaptureCategories' category signature (render thread): read again on a forced capture, a watched edit or a moved input;
		 * otherwise (CS_DCLF_PERSISTENT_PARITY) checked against a full read.
		 */
		std::uint64_t RefreshCategorySignature(bool a_force, std::uint32_t a_frame)
		{
			// The edits first: one made while the signature is read moves them again, and the next frame reads it again.
			auto& watch = categoryWatch;
			const std::uint64_t edits = SceneTracker::CategoryEdits();
			const std::uint64_t inputs = CategoryInputs();
			if (a_force || !watch.read || edits != watch.edits || inputs != watch.inputs) {
				std::vector<const void*> watched;
				watch.signature = ReadCategorySignature(&watched);
				SceneTracker::WatchCategoryNodes(std::move(watched));
				watch.read = true;
				watch.edits = edits;
				watch.inputs = inputs;
				++watch.reads;
			} else if (Diagnostics::Observe().scene) {
				// CS_DCLF_PERSISTENT_PARITY: a signature that moved with no edit seen and no input moved (an event the watch lacks).
				++watch.checked;
				static std::uint64_t flagged = 0;
				if (const std::uint64_t full = ReadCategorySignature(nullptr); full != watch.signature && std::exchange(flagged, full) != full)
					if (watch.missed++ < 8)
						logger::warn("[DCLF] category signature (C2 parity): moved with no watched edit and no input moved (frame {}) <- FLAG", a_frame);
			}
			return watch.signature;
		}

		/**
		 * @brief The capture's category nodes, parentless roots and view roots (render thread): ObjectLODRoot's multibounds, the listed
		 * classes' category roots, every loaded cell's category nodes, multibound and portal graph, then the CategoryRoot views' roots.
		 */
		void GatherCategoryNodes(CategoryCapture& a_capture)
		{
			auto& current = a_capture.nodes;
			auto& currentRoots = a_capture.roots;

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
				// The classes' category roots while a class that lists one is on (Api::ObjectClassContribution::categoryRoot: Features/Lod's
				// TES::lodLandRoot, the terrain manager's LOD blocks): category nodes, their content tracked by the usual structural events. A
				// root two classes list is one node.
				if (const std::uint32_t rootListing = RootListing(SceneToggles())) {
					const auto& roots = Extensions::RegisteredCategoryRoots();
					for (std::uint32_t c = 0; c < kObjectClasses; ++c)
						if (rootListing & (1u << c))
							if (auto* root = roots[c]())
								current.insert(root);
				}
				Engine::ForEachLoadedCell(*tes, addCell);
			}
			// The CategoryRoot views' roots while their toggles are on (their callbacks, in slot order: Features/FirstPerson's, the player's
			// first-person skeleton, PlayerCharacter +0x8F8, Load3D: no parent; Main::unk_FPCulling and RenderDepth cull it alone). A root is
			// a category node and a parentless root filed under itself, so the mirror counts it in the world (SetDrawnRoots) and its equipment
			// attaches reach the tracker as events. Its kHidden is view-scoped: ClassifyFrame stops at it.
			{
				const auto& views = Extensions::Views();
				for (std::uint32_t i = 0; i < views.rootCount; ++i) {
					const auto view = views.rootViews[i];
					if (!Extensions::ViewOn(SceneToggles(), view))
						continue;
					Api::ViewRootCapture root;
					Extensions::ViewEntryOf(view).captureRoot(root);
					if (auto* node = root.root) {
						current.insert(node);
						currentRoots.try_emplace(node, CategoryCapture::Root{ RE::NiPointer<RE::NiAVObject>(node), node });
						a_capture.viewRoots[static_cast<std::size_t>(view)] = node;
					}
				}
			}
		}

		/**
		 * @brief What the job pins (RunFrameStartItem): category nodes new to the scene work's set (children's keys, subtrees) and new roots'
		 * subtrees. Its set is the capture it applied last (a_applied), read from the render thread's own copy; none applied makes all new.
		 */
		void PlanCategoryPins(FrameStartJob& a_job, const CategoryCapture& a_capture, const std::deque<std::shared_ptr<const CategoryCapture>>& a_held,
			std::uint64_t a_applied)
		{
			const CategoryCapture* base = nullptr;
			for (const auto& held : a_held)
				if (held->generation == a_applied)
					base = held.get();
			const bool all = !base;
			for (auto* node : a_capture.nodes)
				if (all || !base->nodes.contains(node))
					a_job.pinNodes.push_back(node);
			for (const auto& [root, entry] : a_capture.roots)
				if (all || !base->roots.contains(root) || !base->nodes.contains(entry.category))
					a_job.pinRoots.push_back(entry.root.get());
		}

		/**
		 * @brief The parentless roots are the mirror's world from now on: each new one captured whole, each gone one detached (a_previous, the
		 * last capture, still holds it), pushed onto the tracker's stack mirror-only, here (each read and pushed at once, as the hooks do theirs:
		 * other threads attach and detach inside the window), before the job posts the capture.
		 */
		void PushCategoryRoots(const CategoryCapture& a_capture, const CategoryCapture* a_previous, std::uint64_t& a_records, std::uint64_t& a_captures)
		{
			const auto& currentRoots = a_capture.roots;
			auto& tracker = SceneTracker::Get();
			std::vector<const RE::NiAVObject*> roots;
			for (const auto& [root, entry] : currentRoots)
				roots.push_back(root);
			SceneCapture::SetDrawnRoots(roots);
			if (a_previous)
				for (const auto& [root, entry] : a_previous->roots)
					if (!currentRoots.contains(root)) {
						auto* event = new SceneTracker::Event{};
						event->type = SceneTracker::EventType::Detached;
						SceneTracker::CollectGeometry(entry.root.get(), event->removed, &event->removedNodes);
						event->detachedRoot = root;
						tracker.PushCaptured(event);
					}
			for (const auto& [root, entry] : currentRoots)
				if (!a_previous || !a_previous->roots.contains(root))
					if (auto records = SceneCapture::CaptureAttached(*root)) {
						a_records += records->nodes.size();
						++a_captures;
						auto* event = new SceneTracker::Event{};
						event->type = SceneTracker::EventType::Attached;
						event->captured = std::move(records);
						tracker.PushCaptured(event);
					}
		}
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
			// The last frame's job: dropped once done (its references already handed to EngineReleases), resumed in this window when a
			// refused lease left items, and dropped unfinished when a load came between (its capture and requests name the old world; the
			// next capture is forced).
			if (frameStartJob && !frameStartJob->running.load(std::memory_order_acquire)) {
				if (frameStartJob->next >= frameStartJob->Items())
					frameStartJob.reset();
				else if (loadSinceCapture)
					frameStartJob.reset();  // an unposted capture's roots were pushed by the render thread; the forced capture follows them
			}
			if (frameStartJob) {
				// Still running, or resumed: this frame's category capture and fade requests wait for it (a detach seen, a forced capture
				// and the requests stay where they are for the next frame's).
				++categoryWatch.deferred;
				if (!frameStartJob->running.load(std::memory_order_acquire)) {
					++categoryWatch.resumed;
					DispatchFrameStartJob(*frameStartJob);
				}
			} else {
				frameStartJob = std::make_unique<FrameStartJob>();
				// Forced after a detach (it can take a category node before the signature sees its cell go) or a load.
				const bool detached = channels.categoryDetachSeen.exchange(false, std::memory_order_acq_rel);
				CaptureCategories(std::exchange(categoryCapturePending, false) || detached);
				ServeFadeNodeRequests();
				if (frameStartJob->Empty())
					frameStartJob.reset();
				else {
					++categoryWatch.jobs;
					categoryWatch.jobItems += frameStartJob->Items();
					DispatchFrameStartJob(*frameStartJob);
				}
			}
			CaptureMirrorRequests();
			Diagnostics::Notify<Diagnostics::ScenePoint::MirrorProbePoint>();
			ReportCategoryWatch();
		}
		// The draw families' mirrors are the render thread's (Features/TreeLod's: its decision reads it at the frame's start).
		Extensions::ServeDrawFamilies(frame);
	}

	void FrameView::ServeFadeNodeRequests()
	{
		// Answered by the frame start's job (RunFrameStartItem), each under its lease; the geometry references go to EngineReleases.
		channels.fadeNodeRequests.Drain([&](FadeNodeRequest&& a_request) { frameStartJob->fadeRequests.push_back(std::move(a_request)); });
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
		// finds anything both miss). The signature itself is read again only after an edit of a node it reads children of (the tracker's
		// watch) or a move of its O(1) inputs; the pins and the mirror's pushes are the frame start's job (on the pool, leased).
		ZoneScopedN("CS.DCLF.Ingest.CaptureCategories");
		const auto start = std::chrono::steady_clock::now();
		const auto timed = [&] { categoryCaptureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); };
		SceneTracker::RetireCategoryNodes();
		// Drop captures superseded by one the coordinator applied, pins included. The coordinator let go of its copy before moving
		// the generation, so these engine references are released here, on the render thread.
		const std::uint64_t applied = channels.categoryAppliedGeneration.load(std::memory_order_acquire);
		while (categoryCapturesHeld.size() > 1 && categoryCapturesHeld.front()->generation < applied)
			categoryCapturesHeld.pop_front();
		const std::uint64_t signature = RefreshCategorySignature(a_force, frame);
		if (!a_force && categoryCapture && signature == categoryCapture->signature)
			return timed();
		++categoryCapturesMade;
		auto capture = std::make_shared<CategoryCapture>();
		capture->generation = ++categoryCaptures;
		capture->signature = signature;
		GatherCategoryNodes(*capture);
		capture->held.reserve(capture->nodes.size());
		for (auto* node : capture->nodes)
			capture->held.emplace_back(node);
		auto& job = *frameStartJob;
		job.capture = capture;
		PlanCategoryPins(job, *capture, categoryCapturesHeld, applied);
		PushCategoryRoots(*capture, categoryCapture.get(), categoryMirrorRecords, categoryMirrorCaptures);
		// Held until superseded by an applied one; posted by the job after its pins.
		categoryCapturesHeld.push_back(capture);
		categoryCapture = std::move(capture);
		loadSinceCapture = false;
		timed();
	}

	void FrameView::CaptureMirrorRequests()
	{
		// Objects the mirror lacks (an attach no hook captured, e.g. references moved into multibounds): each captured with its
		// subtree and chain to the world root (CaptureAttached) unless an ancestor was just captured, and pushed onto the tracker's
		// stack, mirror-only, in order with the hooks' events. Render thread: read and pushed at once (not the job's: other threads
		// attach and detach inside the window). Rare (the mirror's misses).
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
		PostAncestorMap();
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

	void FrameView::PostAncestorMap()
	{
		// The contribution's map is the render thread's (Light Limit Fix swaps its room map at its own point): only a copy is posted.
		if (!Extensions::AncestorValueOn())
			return;
		const auto& value = *Extensions::AncestorValue();
		const std::uint64_t generation = value.generation();
		if (generation == ancestorMapPostedGeneration)
			return;
		auto map = std::make_shared<Extensions::AncestorValueMap>();
		Api::AncestorMapSink sink(*map);
		value.fill(sink);
		auto input = std::make_shared<AncestorMapInput>();
		input->map = std::move(map);
		input->generation = generation;
		channels.ancestorMapPosted.store(std::shared_ptr<const AncestorMapInput>(std::move(input)), std::memory_order_release);
		ancestorMapPostedGeneration = generation;
		channels.accumulateInputs.ancestorMapsPosted.fetch_add(1, std::memory_order_relaxed);
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
		return ReadCategorySignature(nullptr);
	}
}
