#include "Internal.h"

namespace DCLF
{
	std::uint32_t SceneStore::LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property)
	{
		if (!a_geometry.GetFlags().any(RE::NiAVObject::Flag::kMeshLOD) || !a_property || !a_property->fadeNode)
			return 3;
		return a_property->fadeNode->GetRuntimeData().unk152 & 0xF;
	}

	std::uint32_t SceneStore::LodRowOf(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property)
	{
		const auto* fadeNode = a_property && a_property->fadeNode ? a_leaf.FadeNodeOf(a_property) : nullptr;
		if (!a_leaf.node || !(a_leaf.node->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kMeshLOD)) || !fadeNode)
			return 3;
		return fadeNode->fadeLevel;
	}

	std::uint32_t SceneStore::LodRowOf(const RE::BSRenderPass& a_pass)
	{
		return a_pass.LODMode.index + (a_pass.LODMode.singleLevel ? 4u : 0u);
	}

	std::uint32_t SceneStore::SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow)
	{
		const auto* partition = a_skin.skinPartition.get();
		if (!partition || a_lodRow >= 8)
			return 0;
		static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
		const RE::BSDismemberSkinInstance::Data* shown = a_skin.GetRTTI() == dismemberSkinInstance.get() ?
		                                                     static_cast<const RE::BSDismemberSkinInstance&>(a_skin).GetRuntimeData().partitions :
		                                                     nullptr;
		std::uint32_t mask = 0;
		for (std::uint32_t i = 0; i < partition->numPartitions && i < kMaxSkinPartitions; ++i) {
			if (shown && !shown[i].editorVisible)
				continue;
			const std::uint32_t lodByte = partition->partitions[i].pad42 & 0xFF;
			if (lodByte <= 2 && kPartitionLodTable[a_lodRow * 3 + lodByte])
				mask |= 1u << i;
		}
		return mask;
	}

	std::uint32_t SceneStore::SkinPartitionMask(const SceneCapture::GeometryRecord& a_geometry, std::uint32_t a_lodRow)
	{
		if (!a_geometry.skinPartition || a_lodRow >= 8)
			return 0;
		// The shown flags (a dismember skin's, the record's: none for another skin, or a dismember skin without its array).
		const auto& shown = a_geometry.shown;
		std::uint32_t mask = 0;
		for (std::uint32_t i = 0; i < a_geometry.partitions.size() && i < kMaxSkinPartitions; ++i) {
			if (i < shown.size() && !shown[i])
				continue;
			const std::uint32_t lodByte = a_geometry.partitions[i].lodByte;
			if (lodByte <= 2 && kPartitionLodTable[a_lodRow * 3 + lodByte])
				mask |= 1u << i;
		}
		return mask;
	}

	std::uint16_t SceneStore::SkinPartitionsOf(const SceneCapture::GeometryRecord& a_geometry)
	{
		if (!a_geometry.skin || !a_geometry.skinPartition)
			return 0;
		// Row 3 (cumulative: every level's partitions), whatever the node's level; a LOD skin's draw narrows it (SkinLodPartitionsOf).
		const std::uint32_t mask = SkinPartitionMask(a_geometry, 3);
		return static_cast<std::uint16_t>(!mask ? kNoPartitions : a_geometry.partitions.size() > 1 ? mask : 0u);
	}

	std::uint32_t SceneStore::SkinLodPartitionsOf(const SceneCapture::LeafView& a_leaf)
	{
		if (!a_leaf || !a_leaf.geometry->skin || !a_leaf.geometry->skinPartition || !(a_leaf.node->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kMeshLOD)) ||
			!a_leaf.property || !a_leaf.property->fadeNode)
			return 0;
		std::uint32_t word = 0;
		for (std::uint32_t level = 0; level < 4; ++level)
			word |= (SkinPartitionMask(*a_leaf.geometry, level) & 0xFFu) << (8 * level);
		return word;
	}

	RE::NiNode* SceneStore::FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored) const
	{
		constexpr auto kRead = static_cast<std::size_t>(MirrorRead::CategoryNode);
		++mirrorReads.reads[kRead];
		Ineligible reason = Ineligible::None;
		bool unmirrored = false;
		RE::NiNode* found = MirrorCategoryNode(a_object, reason, unmirrored);
		if (unmirrored)
			++mirrorReads.unmirrored[kRead];
		if (a_unmirrored)
			*a_unmirrored = unmirrored;
		if (mirrorReadParity && a_object)
			if (const auto lease = LiveCheckLease(MirrorRead::CategoryNode)) {
			Ineligible liveReason = Ineligible::None;
			const auto* live = FindCategoryNodeLive(a_object, &liveReason);
			NoteMirrorRead(MirrorRead::CategoryNode, live != found || (found && liveReason != reason), [&] {
				// Both chains, to where they part (the live one named).
				std::string chains;
				const auto mirrored = MirrorChain(a_object, nullptr);
				std::size_t at = 0;
				for (const RE::NiAVObject* object = a_object; object && at < 16; object = object->parent, ++at) {
					const void* other = at < mirrored.size() ? mirrored[at] : nullptr;
					chains += fmt::format(" {}'{}'{}", at ? "<- " : "", object->name.c_str() ? object->name.c_str() : "", other == object ? "" : fmt::format(" (mirror {})", other));
				}
				return fmt::format("'{}' {}: mirror {} (reason {}), live {} (reason {}){}; live chain:{}; mirror chain {} long", a_object->name.c_str() ? a_object->name.c_str() : "",
					static_cast<const void*>(a_object), static_cast<const void*>(found), static_cast<int>(reason), static_cast<const void*>(live), static_cast<int>(liveReason),
					unmirrored ? ", a record missing on the way" : "", chains, mirrored.size());
			}, [this, key = static_cast<const void*>(a_object), live, liveReason] {
				Ineligible r = Ineligible::None;
				bool u = false;
				const auto* now = MirrorCategoryNode(key, r, u);
				return now == live && (!now || r == liveReason);
			});
		}
		if (found && a_parentReason)
			*a_parentReason = reason;
		return found;
	}

	RE::NiNode* SceneStore::MirrorCategoryNode(const void* a_object, Ineligible& a_reason, bool& a_unmirrored) const
	{
		Ineligible reason = Ineligible::None;
		RE::NiNode* found = nullptr;
		bool unmirrored = false;
		const void* top = a_object;
		const SceneCapture::NodeRecord* record = a_object ? mirror.Node(a_object) : nullptr;
		unmirrored = a_object && !record;
		const void* key = record ? record->parent : nullptr;
		for (std::uint32_t depth = 0; key && depth < kMaxParentDepth; ++depth) {
			auto* node = static_cast<RE::NiNode*>(const_cast<void*>(key));
			if (categoryNodes.contains(node)) {
				found = node;
				break;
			}
			const auto* parent = mirror.Node(key);
			if (!parent) {
				unmirrored = true;
				break;
			}
			// ParentReason's, from the record's kind (the capture's casts).
			const Ineligible here = (parent->kind & SceneCapture::kKindSwitch)  ? Ineligible::Switch :
			                        (parent->kind & SceneCapture::kKindOrdered) ? Ineligible::UnsupportedParent :
			                        (parent->kind & SceneCapture::kKindBillboard) ? Ineligible::Billboard :
			                                                                         Ineligible::None;
			reason = CombineParentReasons(reason, here);
			top = key;
			key = parent->parent;
		}
		// A root the portal graph draws with no parent at all (alwaysRenderChildren): filed under its graph's shared portal node.
		if (!found && !key && !unmirrored && top && !alwaysRenderRoots.empty())
			if (const auto it = alwaysRenderRoots.find(static_cast<const RE::NiAVObject*>(top)); it != alwaysRenderRoots.end())
				found = it->second.category;
		a_reason = reason;
		a_unmirrored = unmirrored;
		return found;
	}

	void SceneStore::RecheckDeferredReads()
	{
		// After the batch the differing reads' changes were in: the mirror agreeing now, the read lagged; not, it differs.
		for (auto& deferred : std::exchange(deferredReads, {})) {
			const auto r = static_cast<std::size_t>(deferred.read);
			if (deferred.recheck())
				++mirrorReads.lagged[r];
			else if (mirrorReads.differ[r]++ == 0)
				mirrorReads.first[r] = std::move(deferred.what);
		}
	}

	std::string SceneStore::MirrorReadReport()
	{
		auto& m = mirrorReads;
		std::string text;
		for (std::size_t r = 0; r < m.reads.size(); ++r) {
			if (!m.reads[r])
				continue;
			text += fmt::format("{}{} {} reads ({} with a record missing){}{}", text.empty() ? "" : "; ", kMirrorReadNames[r], m.reads[r], m.unmirrored[r],
				m.checked[r] ? fmt::format(", {} checked, {} lagged a batch, {} differ{}{}", m.checked[r], m.lagged[r], m.differ[r], m.differ[r] ? ": " : "", m.first[r]) : std::string(),
				m.refused[r] ? fmt::format(", {} checks skipped outside the read window", m.refused[r]) : std::string());
		}
		bool differ = false;
		for (const auto n : m.differ)
			differ |= n != 0;
		const bool checked = std::ranges::any_of(m.checked, [](auto n) { return n != 0; });
		m = {};
		if (text.empty())
			return {};
		return fmt::format("[DCLF] mirror reads (T6b1b): {}{}\n", text, checked ? (differ ? " <- MIRROR READ" : " <- OK") : "");
	}

	RE::NiNode* SceneStore::FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const
	{
		EngineReadWindow::Touch("SceneStore::FindCategoryNodeLive");
		Ineligible reason = Ineligible::None;
		const RE::NiAVObject* top = a_object;
		RE::NiNode* node = a_object ? a_object->parent : nullptr;
		for (std::uint32_t depth = 0; node && depth < kMaxParentDepth; ++depth, node = node->parent) {
			if (categoryNodes.contains(node)) {
				if (a_parentReason)
					*a_parentReason = reason;
				return node;
			}
			reason = CombineParentReasons(reason, ParentReason(node));
			top = node;
		}
		// A root the portal graph draws with no parent at all (alwaysRenderChildren): filed under its graph's shared portal
		// node, a category node no walk up from it reaches.
		if (!node && top && !top->parent && !alwaysRenderRoots.empty()) {
			if (const auto it = alwaysRenderRoots.find(top); it != alwaysRenderRoots.end()) {
				if (a_parentReason)
					*a_parentReason = reason;
				return it->second.category;
			}
		}
		return nullptr;
	}

	std::uint64_t SceneStore::CategorySignature() const
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
			mix(tes->interiorCell);
			if (tes->interiorCell) {
				mixCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					mixCell(grid->cells[i]);
			}
		}
		return hash.value;
	}

	void SceneStore::CaptureMirrorRequests(EventBatch& a_batch)
	{
		// T6b1a: what the scene work tracked that the mirror holds no record of (an attach no hook captured: the references the engine
		// moves into multibounds), captured from its highest ancestor the mirror lacks. Render thread, the frame's start: the scene work
		// is joined, the mirror idle.
		std::vector<RE::NiPointer<RE::NiAVObject>> requests;
		mirrorCaptureRequests.Drain([&requests](RE::NiPointer<RE::NiAVObject>&& a_request) { requests.push_back(std::move(a_request)); });
		if (requests.empty())
			return;
		ZoneScopedN("CS.DCLF.Ingest.MirrorRequests");
		// T6b3a: the mirror is the coordinator's, read here as the frame start's join leaves it (left for T6b3c: the requests served from
		// what the coordinator finds the mirror lacks); a read while the scene work runs is counted and named.
		GuardFrameAccess("CaptureMirrorRequests");
		ankerl::unordered_dense::set<const RE::NiAVObject*> captured;
		for (const auto& geometry : requests) {  // a geometry, or (AddSubtree) a subtree's root
			if (!geometry || mirror.Node(geometry.get()))
				continue;
			const RE::NiAVObject* top = geometry.get();
			bool covered = false;
			for (const RE::NiAVObject* at = geometry.get(); at; at = at->parent) {
				if (captured.contains(at)) {
					covered = true;
					break;
				}
				top = at;
				if (!at->parent || mirror.Node(at->parent))
					break;
			}
			if (covered || !captured.insert(top).second)
				continue;
			if (auto records = SceneCapture::CaptureAttached(*top)) {
				categoryMirrorRecords += records->nodes.size();
				++categoryMirrorCaptures;
				auto* event = new SceneTracker::Event{};
				event->type = SceneTracker::EventType::Attached;
				event->captured = std::move(records);
				a_batch.AppendLate(event);
			}
		}
	}

	void SceneStore::CaptureCategories(bool a_force, EventBatch& a_batch)
	{
		// The set's content changes only when a cell attaches or detaches, so it is made again when the signature says something
		// moved or when a detach was seen (walk parity finds anything both miss).
		ZoneScopedN("CS.DCLF.Ingest.CaptureCategories");
		const auto start = std::chrono::steady_clock::now();
		const auto timed = [&] { categoryCaptureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); };
		// The captures one the coordinator has applied supersedes are let go, their pins with them (T6b1c; T6b3a: the coordinator let go
		// of its copy before it moved the generation on, so these drops are the render thread's, as an engine reference's must be).
		const std::uint64_t applied = categoryAppliedGeneration.load(std::memory_order_acquire);
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
			if (tes->interiorCell) {
				addCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					addCell(grid->cells[i]);
			}
		}
		capture->held.reserve(current.size());
		for (auto* node : current)
			capture->held.emplace_back(node);
		// What the scene work will walk (T6b1c): the category nodes new to its set, their children's keys and their subtrees held, and
		// the subtrees of the roots new to it. T6b3a: its set is the capture it applied last (RefreshCategoryNodes makes them equal), read
		// here from the render thread's own copy, never the coordinator's; none applied yet, or a rescan pending, makes every one new.
		{
			const CategoryCapture* base = nullptr;
			for (const auto& held : categoryCapturesHeld)
				if (held->generation == applied)
					base = held.get();
			const bool all = rescanCaptureAll || !base;
			auto& pins = capture->pins;
			auto pinSubtree = [&](RE::NiAVObject* a_root) {
				std::vector<RE::NiAVObject*> stack{ a_root };
				while (!stack.empty()) {
					auto* object = stack.back();
					stack.pop_back();
					pins.emplace_back(object);
					// A geometry's properties too (T6b1b: what its entry holds).
					if (auto* geometry = object->AsGeometry()) {
						const auto& data = geometry->GetGeometryRuntimeData();
						for (RE::NiRefObject* property : { static_cast<RE::NiRefObject*>(data.shaderProperty.get()), static_cast<RE::NiRefObject*>(LayerPropertyOf(*geometry)),
								 static_cast<RE::NiRefObject*>(data.alphaProperty.get()) })
							if (property)
								pins.emplace_back(property);
					}
					if (auto* node = object->AsNode())
						for (auto& child : node->GetChildren())
							if (child)
								stack.push_back(child.get());
				}
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
		// The portal graph's parentless roots, the mirror's world from now on (T6b1b): each new one captured whole, each gone dropped
		// from it (the last capture holds it), both after the batch's events (the captures that follow are the hooks', InWorld).
		{
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
						a_batch.AppendLate(event);
					}
			for (const auto& [root, entry] : currentRoots)
				if (!categoryCapture || !categoryCapture->roots.contains(root))
					if (auto records = SceneCapture::CaptureAttached(*root)) {
						categoryMirrorRecords += records->nodes.size();
						++categoryMirrorCaptures;
						auto* event = new SceneTracker::Event{};
						event->type = SceneTracker::EventType::Attached;
						event->captured = std::move(records);
						a_batch.AppendLate(event);
					}
		}
		// Held until one the coordinator applied supersedes it, and posted (latest wins: a newer capture names every node new to the set
		// the coordinator applied last, so one it never takes loses nothing).
		categoryCapturesHeld.push_back(capture);
		categoryPosted.Post(std::make_unique<std::shared_ptr<const CategoryCapture>>(capture));
		categoryCapture = std::move(capture);
		rescanCaptureAll = false;
		timed();
	}

	void SceneStore::RefreshCategoryNodes(bool a_force)
	{
		// The render thread's capture (CaptureCategories, posted: T6b3a), the newest taken: diffed again when it is new, or forced (the rescan
		// after a load). Taking one lets go of the one before, which the render thread still holds (it waits for the generation below).
		if (auto taken = categoryPosted.Take())
			categoryTaken = std::move(*taken);
		const auto* capture = categoryTaken.get();
		if (!capture || (!a_force && capture->generation == categoryAppliedGeneration.load(std::memory_order_relaxed)))
			return;
		const std::uint8_t cause = a_force ? 1 : 0;
		categoryAppliedGeneration.store(capture->generation, std::memory_order_release);
		// Its pins (T6b1c): held by the capture until the render thread lets go of it (after a newer one is applied).
		for (const auto& pin : capture->pins) {
			batchPins.insert_or_assign(pin.get(), pin.get());
			++referenceStats.pins;
		}
		categorySignature = capture->signature;
		const auto& current = capture->nodes;
		const auto& currentRoots = capture->roots;

		// Cells that went away, and parentless roots the portal graph no longer lists: drop what was tracked under them.
		bool removedAny = false;
		for (auto* node : categoryNodes) {
			if (!current.contains(node)) {
				removedAny = true;
				break;
			}
		}
		bool removedRoot = false;
		for (const auto& [root, entry] : alwaysRenderRoots) {
			if (const auto it = currentRoots.find(root); it == currentRoots.end() || it->second.category != entry.category) {
				removedRoot = true;
				break;
			}
		}
		if (removedAny || removedRoot) {
			std::vector<RE::BSGeometry*> stale;
			for (auto& [geometry, entry] : tracked) {
				if (!current.contains(entry.categoryNode)) {
					stale.push_back(geometry);
					continue;
				}
				// Filed under a root (its walk up ends without reaching the category node): stale when the root left.
				if (removedRoot && alwaysRenderRoots.size()) {
					// The mirror's chain (T6b1b).
					const void* top = geometry;
					const void* parent = MirrorParent(top);
					for (; parent && parent != entry.categoryNode; parent = MirrorParent(top))
						top = parent;
					if (!parent && alwaysRenderRoots.contains(static_cast<const RE::NiAVObject*>(top))) {
						const auto it = currentRoots.find(static_cast<const RE::NiAVObject*>(top));
						if (it == currentRoots.end() || it->second.category != entry.categoryNode)
							stale.push_back(geometry);
					}
				}
			}
			for (auto* geometry : stale)
				EraseTracked(geometry);
		}
		std::vector<RE::NiAVObject*> addedRoots;
		for (const auto& [root, entry] : currentRoots) {
			if (!alwaysRenderRoots.contains(root) && categoryNodes.contains(entry.category))
				addedRoots.push_back(entry.root.get());
		}

		// Cells that appeared: their content was attached before the cell was, so scan it now.
		std::vector<RE::NiNode*> added;
		for (auto* node : current) {
			if (!categoryNodes.contains(node))
				added.push_back(node);
		}
		categoryNodes = current;
		nodeSetsDirty = true;  // the publication's copy made again (IsCategoryNode)
		for (auto& [root, entry] : alwaysRenderRoots)
			HandBack(std::move(entry.root));
		// Copies of the capture's references (the scene work never makes one from a key).
		alwaysRenderRoots.clear();
		for (const auto& [root, entry] : currentRoots)
			alwaysRenderRoots.emplace(root, AlwaysRenderRoot{ entry.root, entry.category });
		std::erase_if(categoryFound, [&](const auto& a_entry) { return !categoryNodes.contains(const_cast<RE::NiNode*>(a_entry.first)); });
		const TrackSource previousSource = addSource;
		if (addSource != TrackSource::Rescan)
			addSource = TrackSource::CategoryAppeared;
		for (auto* node : added) {
			categoryFound[node] = { sceneFrame, cause };
			// The capture's keys of its children (T6b1c: not read here).
			const auto keys = capture->children.find(node);
			if (keys == capture->children.end()) {
				++categoryChildrenMissing;
				continue;
			}
			for (const auto* child : keys->second)
				AddSubtree(const_cast<RE::NiAVObject*>(child), SubtreeSource::CategoryWalk);
		}
		// Roots of a graph whose shared node appeared now are walked here too (FindCategoryNode finds them through it).
		for (const auto& [root, entry] : alwaysRenderRoots) {
			if (std::find(added.begin(), added.end(), entry.category) != added.end())
				AddSubtree(entry.root.get(), SubtreeSource::CategoryWalk);
		}
		for (auto* root : addedRoots)
			AddSubtree(root, SubtreeSource::CategoryWalk);
		addSource = previousSource;
	}

	const void* SceneStore::MirrorSunEntry(const void* a_geometry) const
	{
		const auto* record = mirror.Node(a_geometry);
		const void* entry = nullptr;
		if (const void* reference = record ? record->userData : nullptr) {
			// An actor's entry is its cell's container, which the full-frustum cull never tests.
			if (record->formType != static_cast<std::uint8_t>(RE::FormType::ActorCharacter)) {
				const void* root = a_geometry;
				for (const void* node = record->parent; node;) {
					const auto* at = mirror.Node(node);
					if (!at || at->userData != reference)
						break;
					root = node;
					node = at->parent;
				}
				entry = root;
			}
		} else if (record) {
			static const REL::Relocation<const RE::NiRTTI*> multiBound{ RE::BSMultiBoundNode::Ni_RTTI };
			for (const void* node = record->parent; node;) {
				const auto* at = mirror.Node(node);
				if (!at)
					break;
				if (at->rtti == multiBound.get()) {
					entry = node;
					break;
				}
				node = at->parent;
			}
		}
		return entry;
	}

	void SceneStore::ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live)
	{
		if (!a_tracked.sunEntryResolved) {
			a_tracked.sunEntryResolved = true;
			// The mirror's chain (T6b1b): the reference's root, or the multibound above.
			++mirrorReads.reads[static_cast<std::size_t>(MirrorRead::SunEntry)];
			const void* entry = MirrorSunEntry(&a_geometry);
			// The mirror's (its root pinned by the attach's capture: T6b1c), or on a live walk's path the live chain's; the other checked
			// (under a lease, T6b1d).
			if (!a_live && !mirrorReadParity) {
				a_tracked.sunEntryNode = static_cast<const RE::NiAVObject*>(entry);
				return;
			}
			const auto lease = a_live ? EngineReadWindow::Lease() : LiveCheckLease(MirrorRead::SunEntry);
			if (!a_live && !lease) {
				a_tracked.sunEntryNode = static_cast<const RE::NiAVObject*>(entry);
				return;
			}
			const RE::NiAVObject* live = nullptr;
			auto* geometry = const_cast<RE::BSGeometry*>(&a_geometry);
			if (auto* reference = geometry->GetUserData()) {
				// An actor's entry is its cell's container, which the full-frustum cull never tests.
				if (reference->GetFormType() != RE::FormType::ActorCharacter) {
					const RE::NiAVObject* root = geometry;
					for (auto* node = geometry->parent; node && node->GetUserData() == reference; node = node->parent)
						root = node;
					live = root;
				}
			} else {
				static const REL::Relocation<const RE::NiRTTI*> multiBound{ RE::BSMultiBoundNode::Ni_RTTI };
				for (auto* node = geometry->parent; node; node = node->parent)
					if (node->GetRTTI() == multiBound.get()) {
						live = node;
						break;
					}
			}
			a_tracked.sunEntryNode = a_live ? live : static_cast<const RE::NiAVObject*>(entry);
			if (mirrorReadParity)
				NoteMirrorRead(MirrorRead::SunEntry, live != entry, [&] { return fmt::format("'{}' {}: mirror {}, live {}", a_geometry.name.c_str() ? a_geometry.name.c_str() : "",
					static_cast<const void*>(&a_geometry), entry, static_cast<const void*>(live)); },
					[this, key = static_cast<const void*>(&a_geometry), live] { return MirrorSunEntry(key) == live; });
		}
	}

	std::uint32_t SceneStore::OcclusionTechniqueOf(const SceneCapture::LeafView& a_leaf, bool a_skylighting) const
	{
		using enum RE::BSShaderProperty::EShaderPropertyFlag;
		using enum RE::BSUtilityShader::Flags;
		const auto* property = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!property || !a_leaf.geometry)
			return 0;
		const auto any = [&](auto... a_flags) { return (property->flags & (static_cast<std::uint64_t>(a_flags) | ...)) != 0; };
		if (a_skylighting ? any(kSkinned) && !any(kTreeAnim) : any(kSkinned))
			return 0;
		if (a_skylighting && a_leaf.node->userData) {
			// The nearest fade node above it (AsFadeNode's: the record's kind), its BSX flags.
			const SceneCapture::NodeRecord* fadeNode = nullptr;
			for (const auto* record = a_leaf.parent; record && !fadeNode; record = record->parent ? mirror.Node(record->parent) : nullptr)
				fadeNode = (record->kind & SceneCapture::kKindFadeNode) ? record : nullptr;
			using Bsx = RE::BSXFlags::Flag;
			constexpr auto kRejected = static_cast<std::int32_t>(Bsx::kRagdoll) | static_cast<std::int32_t>(Bsx::kEditorMarker) | static_cast<std::int32_t>(Bsx::kDynamic) |
			                           static_cast<std::int32_t>(Bsx::kAddon) | static_cast<std::int32_t>(Bsx::kNeedsTransformUpdate) |
			                           static_cast<std::int32_t>(Bsx::kMagicShaderParticles) | static_cast<std::int32_t>(Bsx::kLights) |
			                           static_cast<std::int32_t>(Bsx::kBreakable) | static_cast<std::int32_t>(Bsx::kSearchedBreakable);
			if (fadeNode && fadeNode->bsx != -1 && (fadeNode->bsx & kRejected))
				return 0;
		}
		const bool valid = a_skylighting ? any(kZBufferWrite) && !any(kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal) :
		                                   any(kZBufferWrite) && !any(kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
		if (!valid)
			return 0;
		stl::enumeration<RE::BSUtilityShader::Flags> technique;
		technique.set(RenderDepth);
		if (any(kVertexColors))
			technique.set(Vc);
		if (a_leaf.AlphaTesting()) {
			technique.set(Texture);
			technique.set(AlphaTest);
		}
		if (any(kLODObjects, kHDLODObjects))
			technique.set(LodObject);
		if (any(kTreeAnim))
			technique.set(TreeAnim);
		return technique.underlying();
	}

	const RE::NiNode* SceneStore::RoomNodeOf(const void* a_object) const
	{
		// Light Limit Fix's room (GetParentRoomNode): the nearest node from the object up that is a BSMultiBoundRoom or a
		// BSPortalSharedNode, by its RTTI. The mirror's chain (T6b1b).
		static const auto* roomRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundRoom }.get();
		static const auto* portalRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSPortalSharedNode }.get();
		for (const void* key = a_object; key;) {
			const auto* record = mirror.Node(key);
			if (!record)
				return nullptr;
			if (record->rtti == roomRtti || record->rtti == portalRtti)
				return static_cast<const RE::NiNode*>(key);
			key = record->parent;
		}
		return nullptr;
	}

	void SceneStore::HoldProperties(Tracked& a_entry)
	{
		const auto* record = mirror.Geometry(a_entry.geometry.get());
		auto hold = [&](RE::NiPointer<RE::BSShaderProperty>& a_held, const void* a_key) {
			if (a_held.get() == a_key)
				return;
			// The event that named it (an attach's capture, a leaf update) holds it: the batch's pin.
			auto next = a_key ? Pinned(static_cast<const RE::BSShaderProperty*>(a_key)) : nullptr;
			if (a_key && !next) {
				++referenceStats.propertiesRefused;
				if (referenceStats.firstPropertyRefused.empty())
					referenceStats.firstPropertyRefused = fmt::format("property {} of geometry {}", a_key, static_cast<const void*>(a_entry.geometry.get()));
			} else if (next) {
				++referenceStats.properties;
			}
			HandBack(std::move(a_held));
			a_held = std::move(next);
		};
		hold(a_entry.property, record ? record->property : nullptr);
		hold(a_entry.layerProperty, record ? record->Layer() : nullptr);
	}

	void SceneStore::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live)
	{
		// Its reference first (T6b1c): the entry's own when tracked already, else the batch's pin, else (a live walk's) the pointer.
		RE::NiPointer<RE::BSGeometry> reference;
		if (const auto known = tracked.find(a_geometry); known != tracked.end() && known->second.geometry.get() == a_geometry)
			reference = known->second.geometry;
		else if (!(reference = Pinned(a_geometry)) && a_live) {
			++referenceStats.live;
			reference.reset(a_geometry);
		}
		if (!reference) {
			// The mirror names it under a_categoryNode and nothing holds it: the live scene no longer has it there (its event the next
			// batch's), or a pin is missing. Checked after the next batch: the mirror moving it on, it lagged (T6b1c).
			++referenceStats.refused;
			++mirrorReads.reads[static_cast<std::size_t>(MirrorRead::Reference)];
			NoteMirrorRead(MirrorRead::Reference, true, [&] { return fmt::format("geometry {} under category node {}", static_cast<const void*>(a_geometry), static_cast<const void*>(a_categoryNode)); },
				[this, key = static_cast<const void*>(a_geometry), a_categoryNode] {
					Ineligible reason = Ineligible::None;
					bool missing = false;
					return MirrorCategoryNode(key, reason, missing) != a_categoryNode;
				});
			return;
		}
		const auto [it, inserted] = tracked.try_emplace(a_geometry);
		trackedLayout += inserted ? 1 : 0;
		auto& entry = it->second;
		// Its node's record (T6b1b): the reference, an actor's.
		const auto* record = mirror.Node(a_geometry);
		const auto* actorOwner = record && record->userData && record->formType == static_cast<std::uint8_t>(RE::FormType::ActorCharacter) ?
		                             static_cast<const RE::TESObjectREFR*>(record->userData) :
		                             nullptr;
		const auto identity = sceneIdentity.Attach(a_geometry, actorOwner);
		if (identity.replaced) {
			ReleaseObjectSlot(entry);
			entry.actorOwnedResolved = false;
			entry.faceShapeResolved = false;
		}
		if (inserted) {
			entry.trackedFrame = sceneFrame;
			entry.trackedBy = addSource;
		}
		entry.identity = identity.member;
		entry.groupIdentity = identity.group;
		entry.actorOwner = actorOwner;
		entry.roomNode = RoomNodeOf(a_geometry);
		entry.roomMapGeneration = 0;
		entry.geometry = std::move(reference);
		HoldProperties(entry);
		// A face shape's head (a dynamic shape under a BSFaceGenNiNode, the mirror's): held from the batch's pins (T6b1b).
		{
			const auto* parent = record && record->parent ? mirror.Node(record->parent) : nullptr;
			const auto* g = mirror.Geometry(a_geometry);
			const bool face = parent && (parent->kind & SceneCapture::kKindFaceGen) && g && g->type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kDynamicTriShape);
			const auto* head = face ? static_cast<const RE::BSFaceGenNiNode*>(record->parent) : nullptr;
			if (entry.faceHeadRef.get() != head) {
				HandBack(std::move(entry.faceHeadRef));
				if (head && !(entry.faceHeadRef = Pinned(head)))
					++referenceStats.refused;
			}
		}
		entry.categoryNode = a_categoryNode;
		// The mirror holds no record of it (an attach no hook captured): the render thread captures it at the next frame's start (T6b1a).
		if (!mirror.Node(a_geometry))
			mirrorCaptureRequests.Push(RE::NiPointer<RE::NiAVObject>(a_geometry));
		entry.parentReason = a_parentReason;
		// Attached (again): its classification stands no more, and every other entry reading the same sun entry node is
		// evaluated again, since that node's bound takes this one in now (dclf-event-driven-tables.md, "Phase 3").
		entry.candidateFrame = 0;
		{
			UnlistDependents(a_geometry, entry, true);
			entry.sunEntryResolved = false;
			entry.sunEntryNode = nullptr;
			ResolveSunEntry(entry, *a_geometry, a_live);
			if (entry.sunEntryNode && !OwnRoot(entry.sunEntryNode, a_live))
				entry.sunEntryNode = nullptr;
			if (entry.sunEntryNode) {
				rootDependents[entry.sunEntryNode].push_back(a_geometry);
				const auto* rootRecord = mirror.Node(entry.sunEntryNode);  // its reference, the mirror's
				if (const auto* rootRef = rootRecord ? static_cast<const RE::TESObjectREFR*>(rootRecord->userData) : nullptr;
					rootRef && rootReference.try_emplace(entry.sunEntryNode, rootRef).second)
					referenceRoot[rootRef] = entry.sunEntryNode;
				entry.listedRoot = entry.sunEntryNode;
				dirtyRoots.push_back(entry.sunEntryNode);
				MarkSunEntryDirty(entry.sunEntryNode);
			}
			if (const auto* lightEntry = LightEntryOf(entry, *a_geometry, a_live); lightEntry && OwnRoot(lightEntry, a_live)) {
				auto& dependents = lightDependents[lightEntry];
				lightEntriesAppeared += dependents.empty() ? 1 : 0;
				if (dependents.empty())
					lightEntryChanges.push_back({ lightEntry, 1 });  // a light entry new to the frame's set (IsLightEntry, through the log)
				dependents.push_back(a_geometry);
				entry.lightRoot = lightEntry;
				MarkLightEntryDirty(lightEntry);
			}
		}
		// Object LOD: its drawn ranges from now on, by its segment events (lodSegmentEvents).
		if (const auto* g = mirror.Geometry(a_geometry); g && g->type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kSubIndexTriShape))
			SampleLodRanges(*a_geometry, false);
		pendingEvaluation.push_back(a_geometry);
	}

	const RE::NiAVObject* SceneStore::LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const
	{
		// The category node's child it hangs from: an actor's root is its light entry, whatever is under it (carried items too).
		// The mirror's chain (T6b1b).
		if (const auto* category = a_tracked.categoryNode) {
			++mirrorReads.reads[static_cast<std::size_t>(MirrorRead::LightEntry)];
			const void* root = &a_geometry;
			for (const void* node = MirrorParent(&a_geometry); node && node != category; node = MirrorParent(node))
				root = node;
			const auto* record = root != &a_geometry ? mirror.Node(root) : nullptr;
			const bool actorRoot = record && record->parent == category && record->userData && record->formType == static_cast<std::uint8_t>(RE::FormType::ActorCharacter);
			// The mirror's (its root pinned by the attach's capture: T6b1c), or on a live walk's path the live chain's; the other checked.
			if (!a_live && !mirrorReadParity) {
				if (actorRoot)
					return static_cast<const RE::NiAVObject*>(root);
				return a_tracked.sunEntryNode && !CategoryNodeOwn(a_tracked.sunEntryNode) ? a_tracked.sunEntryNode : nullptr;
			}
			// The check under a lease (T6b1d).
			const auto lease = a_live ? EngineReadWindow::Lease() : LiveCheckLease(MirrorRead::LightEntry);
			if (!a_live && !lease) {
				if (actorRoot)
					return static_cast<const RE::NiAVObject*>(root);
				return a_tracked.sunEntryNode && !CategoryNodeOwn(a_tracked.sunEntryNode) ? a_tracked.sunEntryNode : nullptr;
			}
			const RE::NiAVObject* live = &a_geometry;
			for (const auto* node = a_geometry.parent; node && node != category; node = node->parent)
				live = node;
			const auto* reference = live != &a_geometry && live->parent == category ? live->GetUserData() : nullptr;
			const bool liveActor = reference && reference->GetFormType() == RE::FormType::ActorCharacter;
			if (mirrorReadParity)
				NoteMirrorRead(MirrorRead::LightEntry, liveActor != actorRoot || (actorRoot && live != root), [&] {
					return fmt::format("'{}' {}: mirror {} (actor {}), live {} (actor {})", a_geometry.name.c_str() ? a_geometry.name.c_str() : "", static_cast<const void*>(&a_geometry), root,
						actorRoot, static_cast<const void*>(live), liveActor);
				}, [this, key = static_cast<const void*>(&a_geometry), category, live = static_cast<const void*>(live), liveActor] {
					const auto chain = MirrorChain(key, category);
					const auto* record = chain.size() > 1 ? mirror.Node(chain.back()) : nullptr;
					const bool actor = record && record->parent == category && record->userData && record->formType == static_cast<std::uint8_t>(RE::FormType::ActorCharacter);
					return actor == liveActor && (!actor || chain.back() == live);
				});
			if (a_live ? liveActor : actorRoot)
				return a_live ? live : static_cast<const RE::NiAVObject*>(root);
		}
		return a_tracked.sunEntryNode && !CategoryNodeOwn(a_tracked.sunEntryNode) ? a_tracked.sunEntryNode : nullptr;
	}

	void SceneStore::AddSubtree(RE::NiAVObject* a_root, SubtreeSource a_source)
	{
		// The attach event is drained a frame or more after it was queued, so its node can already be gone
		// (a cell transition releases the subtree). Walking it then dereferences null.
		if (!a_root)
			return;
		Ineligible reasonAbove = Ineligible::None;
		bool unmirrored = false;
		RE::NiNode* category = FindCategoryNode(a_root, &reasonAbove, &unmirrored);
		if (!category) {
			if (unmirrored && a_source == SubtreeSource::CategoryWalk) {
				// Its reference a pin's copy (the capture's): never one made from a key.
				if (auto root = Pinned(static_cast<const RE::NiAVObject*>(a_root))) {
					++subtreesPended;
					mirrorCaptureRequests.Push(root);
					pendingSubtrees.push_back(std::move(root));
				} else {
					++subtreesDropped;
				}
			} else if (unmirrored && a_source == SubtreeSource::Retry) {
				++subtreesDropped;
			}
			return;
		}

		// Walk down the mirror, carrying what the nodes between the category node and the leaf make of it (ParentReason's); each
		// geometry's reference the pin of an attach's capture or the category capture (T6b1c: the mirror can name one the engine has
		// detached and freed since; its pin holds it).
		{
			std::vector<std::pair<RE::BSGeometry*, Ineligible>> found;
			bool lacking = false;
			++mirrorReads.reads[static_cast<std::size_t>(MirrorRead::Subtree)];
			MirrorSubtree(a_root, reasonAbove, found, lacking);
			if (lacking)
				++mirrorReads.unmirrored[static_cast<std::size_t>(MirrorRead::Subtree)];
			if (mirrorReadParity)
				if (const auto lease = LiveCheckLease(MirrorRead::Subtree)) {
				std::vector<std::pair<RE::BSGeometry*, Ineligible>> live;
				std::vector<std::pair<RE::NiAVObject*, Ineligible>> stack;
				stack.emplace_back(a_root, reasonAbove);
				while (!stack.empty()) {
					auto [object, reason] = stack.back();
					stack.pop_back();
					if (!object)
						continue;
					if (auto* geometry = object->AsGeometry()) {
						live.emplace_back(geometry, reason);
						continue;
					}
					if (auto* node = object->AsNode()) {
						const Ineligible below = CombineParentReasons(reason, ParentReason(node));
						for (auto& child : node->GetChildren())
							if (child)
								stack.emplace_back(child.get(), below);
					}
				}
				NoteMirrorRead(MirrorRead::Subtree, live != found, [&] {
					return fmt::format("'{}' {}: mirror {} geometries, live {}{}", a_root->name.c_str() ? a_root->name.c_str() : "", static_cast<const void*>(a_root), found.size(),
						live.size(), lacking ? ", a record missing on the way" : "");
				}, [this, key = static_cast<const void*>(a_root), reasonAbove, live] {
					std::vector<std::pair<RE::BSGeometry*, Ineligible>> now;
					bool missing = false;
					MirrorSubtree(key, reasonAbove, now, missing);
					return now == live;
				});
			}
			for (const auto& [geometry, reason] : found)
				AddGeometry(geometry, category, reason, false);
		}
	}

	void SceneStore::MirrorSubtree(const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored) const
	{
		std::vector<std::pair<const void*, Ineligible>> stack;
		stack.emplace_back(a_root, a_reason);
		while (!stack.empty()) {
			auto [key, reason] = stack.back();
			stack.pop_back();
			const auto* record = mirror.Node(key);
			if (!record) {
				a_unmirrored = true;
				continue;
			}
			if (record->kind & SceneCapture::kKindGeometry) {
				a_out.emplace_back(static_cast<RE::BSGeometry*>(const_cast<void*>(key)), reason);
				continue;
			}
			if (record->kind & SceneCapture::kKindNode) {
				// Its switches' selected children were caught up at ingestion (CatchUpSwitches).
				const Ineligible here = (record->kind & SceneCapture::kKindSwitch)  ? Ineligible::Switch :
				                        (record->kind & SceneCapture::kKindOrdered) ? Ineligible::UnsupportedParent :
				                        (record->kind & SceneCapture::kKindBillboard) ? Ineligible::Billboard :
				                                                                         Ineligible::None;
				const Ineligible below = CombineParentReasons(reason, here);
				for (const void* child : record->children)
					if (child)
						stack.emplace_back(child, below);
			}
		}
	}

	bool SceneStore::CacheableVerdict(Ineligible a_reason)
	{
		// Only the verdicts that follow from the geometry's own shape - its type, its skin instance, its
		// renderer data and the *type* of its shader property. Those are exactly what the pointer
		// witnesses cover.
		//
		// Everything decided later is deliberately left out, because its inputs can change without any
		// witness noticing. AlphaBlend reads `materialAlpha`, which is animated and lives behind an
		// unchanged material pointer; the reasons DeriveLightingDescriptors produces (Decal, Technique,
		// ProjectedUV, Lod, Fading) read property *flags*, which Community Shaders' own features set in
		// place behind an unchanged property pointer. Caching those would trade a real correctness
		// surface for the cheapest third of the population.
		switch (a_reason) {
		case Ineligible::NotTriShape:
		case Ineligible::Skinned:
		case Ineligible::NoRendererData:
		case Ineligible::NotLightingShader:
			return true;
		default:
			return false;
		}
	}

	Ineligible SceneStore::ClassifyStatic(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated)
	{
		if (!a_leaf)
			return Ineligible::NotTriShape;
		const auto& g = *a_leaf.geometry;
		// An NPC face shape (a dynamic shape under a BSFaceGenNiNode) takes the checks below like any shape. Its
		// positions are not in its buffers but in FaceSnapshots: the walk gives every record of one its stream
		// (SceneStore::Tracked::faceShape), whatever the verdict, and the draws bind it as the second stream.
		const auto type = static_cast<RE::BSGeometry::Type>(g.type);
		const bool face = type == RE::BSGeometry::Type::kDynamicTriShape && FaceSnapshots::Enabled() && a_leaf.parent &&
		                  (a_leaf.parent->kind & SceneCapture::kKindFaceGen);
		// A BSMultiIndexTriShape (cave walls, rocks with a snow layer) draws every pass but one as a tri-shape: its renderer data and
		// its triangle count (FUN_1414f2ad0, type 7). The exception is the additional property's main-pass layer (accumulation
		// hint 12), drawn from its second index list: DCLF's layer object (Tracked::layerSlot, ClassifyLayer). The shadow modes
		// register the main property's passes alone (FUN_1414b2a60), so it casts as a tri-shape. One whose layer DCLF cannot
		// draw is MultiIndex, a shadow-only caster (ShadowOnlyReason): its two main passes stay the engine's, together.
		const bool multiIndex = type == RE::BSGeometry::Type::kMultiIndexTriShape;
		// Object LOD (dclf-lod.md): a BSSubIndexTriShape whose Lighting property has the LOD object flags, drawn by the segment
		// runs the engine leaves visible (FUN_1414f2ad0, type 8): a tri-shape over the ranges LodSegments mirrors. Tracked under
		// TES::lodLandRoot; LOD without its toggle. Any other sub-index shape is not one DCLF draws.
		bool lodObject = false;
		if (type == RE::BSGeometry::Type::kSubIndexTriShape) {
			const auto* lodProperty = SceneCapture::LeafView::Lighting(a_leaf.property);
			if (lodProperty && IsLodObject(lodProperty->flags, g.type)) {
				if (!ActiveToggles().lodObjects)
					return Ineligible::Lod;
				lodObject = true;
			}
		}
		if (type != RE::BSGeometry::Type::kTriShape && !multiIndex && !lodObject) {
			if (!face)
				return Ineligible::NotTriShape;
		}

		if (g.skin) {
			if (!ActiveToggles().skinned)
				return Ineligible::Skinned;
			// The skinned path (engine notes: skinning): a NiSkinInstance, or with CS_DCLF_SKIN_PARTITIONS a
			// BSDismemberSkinInstance or several partitions, and a palette the native shader could index (240
			// rows). Each partition the engine draws is one draw of that PARTITION's buffer, not the geometry's
			// rendererData, which for a skinned shape is a different TriShape; which partitions it draws is
			// SkinPartitionMask's, per frame.
			static const REL::Relocation<const RE::NiRTTI*> niSkinInstance{ RE::NiSkinInstance::Ni_RTTI };
			static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
			const bool dismember = g.skinRtti == dismemberSkinInstance.get();
			if (g.skinRtti != niSkinInstance.get() && !dismember)
				return Ineligible::SkinShape;
			const auto partitions = static_cast<std::uint32_t>(g.partitions.size());
			if (!g.skinPartition || !g.skinData || partitions == 0 || g.boneCount == 0 || g.boneCount * 3 > 240)
				return Ineligible::SkinShape;
			if ((partitions > 1 || dismember) && !ActiveToggles().skinPartitions)
				return Ineligible::SkinShape;
			if (partitions > kMaxSkinPartitions)
				return Ineligible::SkinShape;
			// The engine indexes the dismember flags by partition; a flag array of another length is not one it
			// could have drawn from either.
			if (dismember && !g.shown.empty() && g.shown.size() != partitions)
				return Ineligible::SkinShape;
			// Every partition is drawn with the first one's pipeline, so they share its vertex layout; a LOD
			// byte past 2 would index the next row of the engine's table.
			const auto& first = g.partitions[0];
			for (const auto& p : g.partitions) {
				if (!p.vertexBuffer || !p.indexBuffer)
					return Ineligible::NoRendererData;
				if (p.vertexDesc != first.vertexDesc || p.lodByte > 2)
					return Ineligible::SkinShape;
			}
		} else if (!g.rendererData || !g.vertexBuffer || !g.indexBuffer) {
			return Ineligible::NoRendererData;
		}

		const auto* property = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!property)
			return Ineligible::NotLightingShader;

		// GetRenderPasses treats material alpha below one as transparent (the record's: 1 without a material).
		if (property->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;

		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*property, a_leaf, a_accumulated, descriptors);
		if (a_descriptors) {
			if (reason == Ineligible::None) {
				*a_descriptors = descriptors;
			} else {
				// The rejection diagnostics have to survive the rejection. Copying the descriptors only on
				// success left rejectedTechnique at its default 0, and 0 is `none` - a SUPPORTED technique -
				// so the histogram read "none(0)=1337" and looked like a finding rather than an empty field.
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
			}
		}
		if (!multiIndex || reason != Ineligible::None)
			return reason;
		if (!g.layerProperty)
			return Ineligible::None;  // nothing but its own passes: a tri-shape
		return ActiveToggles().layers && ClassifyLayer(a_leaf, nullptr, nullptr) == Ineligible::None ? Ineligible::None : Ineligible::MultiIndex;
	}

	Ineligible SceneStore::ClassifyLayer(const SceneCapture::LeafView& a_leaf, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated)
	{
		// The layer's passes are GetRenderPasses of the additional property for this geometry (FUN_1414b2330): the geometry's
		// alpha property and skin, the layer's flags and material.
		const auto* layer = SceneCapture::LeafView::Lighting(a_leaf.layer);
		if (!layer)
			return Ineligible::NotLightingShader;
		if (a_leaf.Skinned())
			return Ineligible::Skinned;  // a skinned layer is not modelled
		if (layer->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;
		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*layer, a_leaf, a_accumulated, descriptors, true);
		if (a_descriptors) {
			if (reason == Ineligible::None)
				*a_descriptors = descriptors;
			else
				a_descriptors->rejectedTechnique = descriptors.rejectedTechnique;
		}
		return reason;
	}

	SceneStore::SwitchState SceneStore::ReadSwitch(const RE::NiSwitchNode& a_switch)
	{
		const auto* base = reinterpret_cast<const std::byte*>(&a_switch);
		SwitchState state;
		state.flags = *reinterpret_cast<const std::uint16_t*>(base + 0x128);
		state.index = *reinterpret_cast<const std::int32_t*>(base + 0x12C);
		state.revID = *reinterpret_cast<const std::uint32_t*>(base + 0x134);
		// childRevID, an NiTPrimitiveArray at +0x138: its data pointer at +0x8, its capacity at +0x10.
		state.childRevID = *reinterpret_cast<const std::uint32_t* const*>(base + 0x140);
		state.childRevCapacity = *reinterpret_cast<const std::uint16_t*>(base + 0x148);
		return state;
	}

	SceneStore::SwitchSelection SceneStore::SelectionOf(const RE::NiSwitchNode& a_switch, const SwitchState& a_state)
	{
		const auto& children = a_switch.GetChildren();
		if (a_state.index < 0 || static_cast<std::uint32_t>(a_state.index) >= children.capacity() || !a_state.childRevID ||
			static_cast<std::uint32_t>(a_state.index) >= a_state.childRevCapacity)
			return {};
		const auto index = static_cast<std::uint16_t>(a_state.index);
		return { children[index].get(), index, a_state.childRevID[index] == a_state.revID };
	}

	bool SceneStore::SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child)
	{
		// NiSwitchNode::OnVisible (AE 140d29700) culls children[index] and nothing else. A child that has
		// become the selected one since the last update pass is brought up to date there, in the cull
		// (childRevID[index] != revID), which is after this walk read its transforms: leave it native for
		// that frame.
		const auto selection = SelectionOf(a_switch, ReadSwitch(a_switch));
		return selection.child && selection.child == a_child && selection.current;
	}

	bool SceneStore::HiddenForWalk(const RE::NiAVObject* a_object) const
	{
		// The frame's capture (FrameGlobals::cullHidden, taken by the render thread when the scene work is kicked).
		const auto& cullHidden = FrameGlobals::Current().cullHidden;
		const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), a_object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHidden.end() && it->first == a_object)
			return it->second;
		return IsHidden(a_object);
	}

	bool SceneStore::HiddenForWalk(const void* a_object, const SceneCapture::NodeRecord& a_record) const
	{
		const auto& cullHidden = FrameGlobals::Current().cullHidden;
		const auto* object = static_cast<const RE::NiAVObject*>(a_object);
		const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHidden.end() && it->first == object)
			return it->second;
		return (a_record.flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)) != 0;
	}

	Ineligible SceneStore::ClassifyFrame(const Tracked& a_tracked, const AccumulatedPass* a_accumulated) const
	{
		const bool underSwitch = a_tracked.parentReason == Ineligible::Switch;
		if (underSwitch && !ActiveToggles().switchNodes)
			return Ineligible::Switch;
		if (a_tracked.parentReason != Ineligible::None && !underSwitch)
			return a_tracked.parentReason;

		// App-culled or hidden anywhere between the leaf and its category node; part of an actor; under a
		// switch node that does not draw this branch. The mirror's chain (T6b1b).
		const bool actors = ActiveToggles().actors;
		const void* child = nullptr;
		const void* key = a_tracked.geometry.get();
		for (const auto* record = mirror.Node(key); record; child = key, key = record->parent, record = key ? mirror.Node(key) : nullptr) {
			if (HiddenForWalk(key, *record))
				return Ineligible::Hidden;
			if (key == a_tracked.categoryNode)
				break;
			if (underSwitch && child && (record->kind & SceneCapture::kKindSwitch) && !SwitchSelects(*record, child))
				return Ineligible::Switch;
			if (!actors && record->userData && record->actor)
				return Ineligible::Actor;
		}

		// Fading: a membership pass is built from the settled state (the fade is the feedback's). Without one (the scene
		// phase, for the shadow views) a fade is only the native loop's when fades are not DCLF's at all; the shadow views
		// skip faded casters themselves (ShadowReject::Faded). The fade node's currentFade, its record's.
		bool fading = false;
		if (!a_accumulated && !ActiveToggles().fading) {
			const auto leaf = mirror.Leaf(a_tracked.geometry.get());
			fading = leaf.fadeNode && leaf.fadeNode->currentFade < 1.0f;
		}
		if (fading)
			return Ineligible::Fading;

		return Ineligible::None;
	}

	const std::vector<std::uint32_t>& SceneStore::GetMaterialPatchedFloats() const
	{
		return MaterialSources::FramePSFloats();
	}

	const std::vector<std::uint32_t>& SceneStore::GetMaterialPatchedVSFloats() const
	{
		return MaterialSources::FrameVSFloats();
	}
}
