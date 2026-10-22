// The coordinator's classification (scene lane; subtree prepares across the pool): category nodes found and refreshed from the
// render thread's capture, subtrees and geometries tracked from the mirror (prepare, then merge in event order), and the per-entry
// verdicts (ClassifyFrame, HiddenForWalk, billboards, first person).

#include "Internal.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include <cassert>

namespace DCLF::Scene
{
	RE::NiNode* Coordinator::FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored) const
	{
		constexpr auto kRead = static_cast<std::size_t>(MirrorRead::CategoryNode);
		++MirrorReads().reads[kRead];
		Ineligible reason = Ineligible::None;
		bool unmirrored = false;
		RE::NiNode* found = MirrorCategoryNode(a_object, reason, unmirrored);
		if (unmirrored)
			++MirrorReads().unmirrored[kRead];
		if (a_unmirrored)
			*a_unmirrored = unmirrored;
		if (a_object)
			Diagnostics::Notify(Diagnostics::CategoryNodeRead{ *this, a_object, found, reason, unmirrored });
		if (found && a_parentReason)
			*a_parentReason = reason;
		return found;
	}

	RE::NiNode* Coordinator::MirrorCategoryNode(const void* a_object, Ineligible& a_reason, bool& a_unmirrored) const
	{
		Ineligible reason = Ineligible::None;
		RE::NiNode* found = nullptr;
		bool unmirrored = false;
		const void* top = a_object;
		// A CategoryRoot view's root (the first-person root) is its own category node, never filed under one above it (whose members take
		// no part in the view's phase).
		if (ViewRoot(a_object)) {
			a_reason = Ineligible::None;
			a_unmirrored = !mirror.Node(a_object);
			return static_cast<RE::NiNode*>(const_cast<void*>(a_object));
		}
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
			reason = Eligibility::CombineParentReasons(reason, Eligibility::ParentReasonOf(*parent));
			top = key;
			key = parent->parent;
		}
		// A parentless root the portal graph draws (alwaysRenderChildren): filed under its graph's shared portal node.
		if (!found && !key && !unmirrored && top && !alwaysRenderRoots.empty())
			if (const auto it = alwaysRenderRoots.find(static_cast<const RE::NiAVObject*>(top)); it != alwaysRenderRoots.end())
				found = it->second.category;
		a_reason = reason;
		a_unmirrored = unmirrored;
		return found;
	}

	void Coordinator::RecheckDeferredReads()
	{
		// After the next batch: agreeing now means the read lagged, else it differs.
		for (auto& deferred : std::exchange(deferredReads, {})) {
			const auto r = static_cast<std::size_t>(deferred.read);
			if (deferred.recheck())
				++mirrorReads.lagged[r];
			else if (mirrorReads.differ[r]++ == 0)
				mirrorReads.first[r] = std::move(deferred.what);
		}
	}

	RE::NiNode* Coordinator::FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const
	{
		EngineReadWindow::Touch("Coordinator::FindCategoryNodeLive");
		// A CategoryRoot view's root files its subtree under itself (MirrorCategoryNode).
		if (ViewRoot(a_object)) {
			if (a_parentReason)
				*a_parentReason = Ineligible::None;
			return static_cast<RE::NiNode*>(a_object);
		}
		Ineligible reason = Ineligible::None;
		const RE::NiAVObject* top = a_object;
		RE::NiNode* node = a_object ? a_object->parent : nullptr;
		for (std::uint32_t depth = 0; node && depth < kMaxParentDepth; ++depth, node = node->parent) {
			if (categoryNodes.contains(node)) {
				if (a_parentReason)
					*a_parentReason = reason;
				return node;
			}
			reason = Eligibility::CombineParentReasons(reason, Eligibility::ParentReason(node));
			top = node;
		}
		// A parentless portal-graph root: filed under its graph's shared portal node.
		if (!node && top && !top->parent && !alwaysRenderRoots.empty()) {
			if (const auto it = alwaysRenderRoots.find(top); it != alwaysRenderRoots.end()) {
				if (a_parentReason)
					*a_parentReason = reason;
				return it->second.category;
			}
		}
		return nullptr;
	}

	void Coordinator::TakeCategoryCapture()
	{
		// The newest CaptureCategories post, taken before the drains so the portal roots it pushed are in this pass's batch. The render
		// thread keeps the previous one until RefreshCategoryNodes stores its generation.
		if (auto taken = channels.categoryPosted.Take())
			categoryTaken = std::move(*taken);
	}

	void Coordinator::RefreshCategoryNodes(bool a_force)
	{
		// Diffed when new, or forced (the rescan after a load).
		const auto* capture = categoryTaken.get();
		if (!capture || (!a_force && capture->generation == channels.categoryAppliedGeneration.load(std::memory_order_relaxed)))
			return;
		const std::uint8_t cause = a_force ? 1 : 0;
		channels.categoryAppliedGeneration.store(capture->generation, std::memory_order_release);
		// Pins held by the capture until the render thread drops it.
		for (const auto& pin : capture->pins) {
			batchPins.insert_or_assign(pin.get(), pin.get());
			++referenceStats.pins;
		}
		categorySignature = capture->signature;
		viewRoots = capture->viewRoots;
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
		if (removedAny || removedRoot)
			DropRemovedCategories(*capture, removedRoot);
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
		// Copies of the capture's references (never made from a key).
		alwaysRenderRoots.clear();
		for (const auto& [root, entry] : currentRoots)
			alwaysRenderRoots.emplace(root, AlwaysRenderRoot{ entry.root, entry.category });
		std::erase_if(categoryFound, [&](const auto& a_entry) { return !categoryNodes.contains(const_cast<RE::NiNode*>(a_entry.first)); });
		TrackAppearedCategories(*capture, added, addedRoots, cause);
	}

	void Coordinator::DropRemovedCategories(const CategoryCapture& a_capture, bool a_removedRoot)
	{
		const auto& current = a_capture.nodes;
		const auto& currentRoots = a_capture.roots;
		// The members of the cells that went away, and of the category nodes a changed portal root was filed under (only those can
		// hold an entry whose chain tops out at that root): the index's, not a walk over every tracked entry.
		auto& a = attachStats;
		std::vector<RE::BSGeometry*> stale;
		// Filed under a portal root: stale when the root left.
		auto portalStale = [&](RE::BSGeometry* a_geometry, const RE::NiNode* a_category) {
			const void* top = a_geometry;
			const void* parent = mirror.Parent(top);
			for (; parent && parent != a_category; parent = mirror.Parent(top))
				top = parent;
			if (parent || !alwaysRenderRoots.contains(static_cast<const RE::NiAVObject*>(top)))
				return false;
			const auto it = currentRoots.find(static_cast<const RE::NiAVObject*>(top));
			return it == currentRoots.end() || it->second.category != a_category;
		};
		// Every category node entries are filed under that the capture lacks (a portal category too, not a category node itself).
		std::vector<const RE::NiNode*> scanned;
		for (const auto& [node, members] : categoryMembers)
			if (!current.contains(const_cast<RE::NiNode*>(node))) {
				++a.categoriesLeft;
				scanned.push_back(node);
			}
		const std::size_t gone = scanned.size();
		if (a_removedRoot)
			for (const auto& [root, entry] : alwaysRenderRoots)
				if (const auto it = currentRoots.find(root); (it == currentRoots.end() || it->second.category != entry.category) && current.contains(const_cast<RE::NiNode*>(entry.category)) &&
															 std::find(scanned.begin(), scanned.end(), entry.category) == scanned.end())
					scanned.push_back(entry.category);
		for (std::size_t i = 0; i < scanned.size(); ++i) {
			const auto members = categoryMembers.find(scanned[i]);
			if (members == categoryMembers.end())
				continue;
			for (auto* geometry : members->second) {
				const auto entry = tracked.find(geometry);
				if (entry == tracked.end() || entry->second.categoryNode != scanned[i])
					continue;
				a.rootsLeftScanned += i >= gone ? 1 : 0;
				if (i < gone || portalStale(geometry, scanned[i]))
					stale.push_back(geometry);
			}
		}
		a.categoryMembersDropped += stale.size();
		if (Policy::CheckCategoryMembers(true, passSerial)) {
			// Parity: the whole scan's stale set (the walk the index replaced) against the index's.
			std::vector<RE::BSGeometry*> whole;
			for (auto& [geometry, entry] : tracked)
				if (!current.contains(entry.categoryNode) || (a_removedRoot && alwaysRenderRoots.size() && portalStale(geometry, entry.categoryNode)))
					whole.push_back(geometry);
			std::vector<RE::BSGeometry*> indexed = stale;
			std::ranges::sort(whole);
			std::ranges::sort(indexed);
			++a.categoryChecks;
			if (whole != indexed) {
				if (a.categoryDiffer++ == 0)
					a.categoryFirst = fmt::format("a refresh dropped {} entries by the index, {} by the whole scan ({} cells gone, {} portal categories scanned)", indexed.size(),
						whole.size(), gone, scanned.size() - gone);
			}
			CheckCategoryMembers("a refresh's removal");
		}
		for (auto* geometry : stale)
			EraseTracked(geometry);
	}

	void Coordinator::TrackAppearedCategories(const CategoryCapture& a_capture, const std::vector<RE::NiNode*>& a_added,
		const std::vector<RE::NiAVObject*>& a_addedRoots, std::uint8_t a_cause)
	{
		const TrackSource previousSource = addSource;
		if (addSource != TrackSource::Rescan)
			addSource = TrackSource::CategoryAppeared;
		// Roots listed in walk order, then tracked together (AddSubtrees prepares on the pool for a rescan and merges in this order).
		std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> walked;
		for (auto* node : a_added) {
			categoryFound[node] = { sceneFrame, a_cause };
			const auto keys = a_capture.children.find(node);
			if (keys == a_capture.children.end()) {
				++categoryChildrenMissing;
				continue;
			}
			for (const auto* child : keys->second)
				walked.emplace_back(const_cast<RE::NiAVObject*>(child), SubtreeSource::CategoryWalk);
		}
		// Roots of a graph whose shared node just appeared.
		for (const auto& [root, entry] : alwaysRenderRoots) {
			if (std::find(a_added.begin(), a_added.end(), entry.category) != a_added.end())
				walked.emplace_back(entry.root.get(), SubtreeSource::CategoryWalk);
		}
		for (auto* root : a_addedRoots)
			walked.emplace_back(root, SubtreeSource::CategoryWalk);
		AddSubtrees(walked);
		addSource = previousSource;
	}

	void Coordinator::UnlistCategoryMember(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		if (a_tracked.categoryNode)
			Unlist(categoryMembers, categoryMembersAt, static_cast<const RE::NiNode*>(a_tracked.categoryNode), a_geometry);
	}

	void Coordinator::CheckCategoryMembers(const char* a_where)
	{
		// Every tracked entry listed under its category node at its position, and every listed one tracked under that node.
		auto& a = attachStats;
		++a.categoryChecks;
		std::size_t missing = 0, stray = 0, misplaced = 0;
		std::string first;
		for (const auto& [geometry, entry] : tracked)
			if (!categoryMembersAt.contains(std::pair<const void*, const RE::BSGeometry*>{ entry.categoryNode, geometry }) && missing++ == 0)
				first = fmt::format("geometry {} filed under {} is not listed", static_cast<const void*>(geometry), static_cast<const void*>(entry.categoryNode));
		for (const auto& [node, members] : categoryMembers)
			for (std::uint32_t i = 0; i < members.size(); ++i) {
				const auto entry = tracked.find(members[i]);
				if (entry == tracked.end() || entry->second.categoryNode != node) {
					if (stray++ == 0 && first.empty())
						first = fmt::format("geometry {} listed under {} is {}", static_cast<const void*>(members[i]), static_cast<const void*>(node),
							entry == tracked.end() ? "not tracked" : fmt::format("filed under {}", static_cast<const void*>(entry->second.categoryNode)));
					continue;
				}
				const auto at = categoryMembersAt.find(std::pair<const void*, const RE::BSGeometry*>{ node, members[i] });
				if ((at == categoryMembersAt.end() || at->second != i) && misplaced++ == 0 && first.empty())
					first = fmt::format("geometry {} at {} under {}, its position {}", static_cast<const void*>(members[i]), i, static_cast<const void*>(node),
						at == categoryMembersAt.end() ? std::string("none") : fmt::format("{}", at->second));
			}
		if (!missing && !stray && !misplaced)
			return;
		if (a.categoryDiffer++ == 0)
			a.categoryFirst = fmt::format("at {}: {} tracked entries unlisted, {} listed entries stray, {} misplaced; first: {}", a_where, missing, stray, misplaced, first);
	}

	void Coordinator::CheckAncestorKeys()
	{
		if (!Extensions::AncestorValueOn())
			return;
		auto& a = attachStats;
		++a.ancestorChecks;
		const std::int32_t none = Extensions::AncestorValue()->none;
		for (const auto& [geometry, entry] : tracked) {
			++a.ancestorEntries;
			const void* key = AncestorKeyOf(geometry);
			std::int32_t value = none;
			if (ancestorMap && entry.ancestorKey)
				if (const auto found = ancestorMap->find(entry.ancestorKey); found != ancestorMap->end())
					value = found->second;
			// The value the GPU reads: the resident slots' (the map's for the kept key).
			bool slotStale = false;
			for (const std::uint32_t slot : { entry.slot, entry.layerSlot })
				slotStale |= slot != kNoObjectSlot && slot < tables.ancestorValue.size() && tables.residentSlot[slot] && tables.ancestorValue[slot].value != value;
			const bool staleKey = key != entry.ancestorKey, staleValue = slotStale || (entry.ancestorMapGeneration == ancestorMapGeneration && value != entry.ancestorValue);
			a.ancestorStaleKeys += staleKey ? 1 : 0;
			a.ancestorStaleValues += staleValue ? 1 : 0;
			if ((staleKey || staleValue) && a.ancestorFirst.empty()) {
				a.ancestorFirst = fmt::format("geometry {} '{}' (tracked frame {}, by {}): kept key {} value {} (map's {}, slot's {}), the mirror's chain's key {} (value {})",
					static_cast<const void*>(geometry), geometry->name.c_str() ? geometry->name.c_str() : "?", entry.trackedFrame, static_cast<int>(entry.trackedBy), entry.ancestorKey,
					entry.ancestorValue, value, entry.slot != kNoObjectSlot && entry.slot < tables.ancestorValue.size() ? tables.ancestorValue[entry.slot].value : none, key,
					[&] {
						if (ancestorMap && key)
							if (const auto found = ancestorMap->find(key); found != ancestorMap->end())
								return found->second;
						return none;
					}());
			}
		}
	}

	std::string Coordinator::TakeAttachLine()
	{
		auto& a = attachStats;
		if (!a.mirrorBatches && !a.geometryRounds && !a.categoriesLeft && !a.usesChecks && !a.categoryChecks && !a.mirrorKindEvents[5] && !a.structuralEvents && !a.applyChecks)
			return {};
		std::string line = fmt::format(
			"[DCLF] attach burst (W8): mirror {} batches, {} captures prepared ({} batches on the pool), {} records, prepare {:.2f} ms, apply {:.2f} ms; subtree geometries "
			"{} rounds ({} on the pool), {} prepared ({} on the pool); category nodes left {}, members dropped {}, portal-category members scanned {}",
			a.mirrorBatches, a.mirrorCaptures, a.mirrorPooled, a.mirrorRecords, a.mirrorPrepareMs, a.mirrorApplyMs, a.geometryRounds, a.geometryPooled, a.geometries,
			a.geometriesPooled, a.categoriesLeft, a.categoryMembersDropped, a.rootsLeftScanned);
		// The mirror's apply by event kind, and its replays.
		std::string kinds;
		static constexpr std::array<const char*, AttachStats::kMirrorKinds> kKindNames{ "captures", "fills", "evictions", "detaches", "gates", "updates" };
		for (std::size_t k = 0; k < AttachStats::kMirrorKinds; ++k)
			if (a.mirrorKindEvents[k])
				kinds += fmt::format("{}{} {} {:.2f} ms", kinds.empty() ? "" : ", ", kKindNames[k], a.mirrorKindEvents[k], a.mirrorKindMs[k]);
		line += fmt::format("; mirror apply by kind: {}; replays skipped {}, walked {}; updates kept {}, let go {}", kinds.empty() ? "none" : kinds, a.replaysSkipped,
			a.replaysWalked, a.updatesKept, a.updatesForgotten);
		line += fmt::format("; attach/detach: {} events, sun ancestors {:.2f} ms, merges {:.2f} ms, detached geometries {} ({} erased) {:.2f} ms; leaf holds {} of {} keys {:.2f} ms",
			a.structuralEvents, a.ancestorMs, a.mergeMs, a.detachedGeometries, a.detachedErased, a.detachMs, a.leafHolds, a.leafKeys, a.leafMs);
		if (a.applyChecks)
			line += fmt::format("; mirror apply parity {} checks, {} wrong{}", a.applyChecks, a.applyWrong, a.applyWrong ? " <- MIRROR APPLY" : " <- OK");
		if (a.usesChecks)
			line += fmt::format("; mirror uses parity {} checks, {} unused records{}", a.usesChecks, a.usesUnused, a.usesUnused ? " <- MIRROR USES" : " <- OK");
		if (a.categoryChecks)
			line += fmt::format("; category index parity {} checks, {} differ{}{}", a.categoryChecks, a.categoryDiffer, a.categoryDiffer ? " <- CATEGORY INDEX" : " <- OK",
				a.categoryFirst.empty() ? "" : "; first: " + a.categoryFirst);
		if (a.ancestorChecks)
			line += fmt::format("; ancestor key parity {} checks, {} entries, {} kept keys stale, {} values stale (kept or resident slots'){}{}", a.ancestorChecks, a.ancestorEntries, a.ancestorStaleKeys,
				a.ancestorStaleValues, a.ancestorStaleKeys || a.ancestorStaleValues ? " <- ANCESTOR KEY" : " <- OK", a.ancestorFirst.empty() ? "" : "; first: " + a.ancestorFirst);
		a = {};
		return line;
	}

	const void* Coordinator::MirrorSunEntry(const void* a_geometry) const
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

	void Coordinator::ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live)
	{
		if (!a_tracked.sunEntryResolved) {
			a_tracked.sunEntryResolved = true;
			// The reference's root, or the multibound above.
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SunEntry)];
			const void* entry = MirrorSunEntry(&a_geometry);
			// The mirror's (root pinned by the attach's capture), or on a live walk the live chain's; observers compare the two.
			if (!a_live) {
				a_tracked.entryRoot = static_cast<const RE::NiAVObject*>(entry);
				Diagnostics::Notify(Diagnostics::SunEntryRead{ *this, a_geometry, entry, nullptr, false });
				return;
			}
			const EngineReadWindow::Lease lease;
			const RE::NiAVObject* live = LiveSunEntry(a_geometry);
			a_tracked.entryRoot = live;
			Diagnostics::Notify(Diagnostics::SunEntryRead{ *this, a_geometry, entry, live, true });
		}
	}

	const RE::NiAVObject* Coordinator::LiveSunEntry(const RE::BSGeometry& a_geometry)
	{
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
		return live;
	}

	std::int32_t Coordinator::NearestFadeNodeBsx(const SceneCapture::LeafView& a_leaf) const
	{
		const SceneCapture::NodeRecord* fadeNode = nullptr;
		for (const auto* record = a_leaf.parent; record && !fadeNode; record = record->parent ? mirror.Node(record->parent) : nullptr)
			fadeNode = (record->kind & SceneCapture::kKindFadeNode) ? record : nullptr;
		return fadeNode ? fadeNode->bsx : -1;
	}

	const void* Coordinator::AncestorKeyOf(const void* a_object) const
	{
		// The nearest node of the contribution's classes up the chain, by RTTI (Light Limit Fix's GetParentRoomNode: BSMultiBoundRoom or
		// BSPortalSharedNode). Its classes were resolved at install, before any scene work.
		const auto* value = Extensions::AncestorValue();
		if (!value)
			return nullptr;
		for (const void* key = a_object; key;) {
			const auto* record = mirror.Node(key);
			if (!record)
				return nullptr;
			if (value->IsKey(record->rtti))
				return key;
			key = record->parent;
		}
		return nullptr;
	}

	Coordinator::HeldKeys Coordinator::HeldKeysOf(const void* a_geometry) const
	{
		HeldKeys keys;
		const auto* record = mirror.Geometry(a_geometry);
		if (!record)
			return keys;
		keys.property = record->property;
		keys.layer = record->Layer();
		// The property's fade node, for the fade-root owners and the components' (an attach's capture pins every ancestor).
		const auto* property = record->property ? mirror.Property(record->property) : nullptr;
		keys.fadeNode = property ? property->fadeNode : nullptr;
		// The engine's "no fade node" sentinel is no root (Features/Fades' switch, default on: read by name, as the fade roots read it).
		static const bool emptyIsNone = RegisteredSwitchValue("CS_DCLF_EMPTY_FADE_NODE") != "0";
		if (emptyIsNone && keys.fadeNode == Engine::EmptyFadeNode())
			keys.fadeNode = nullptr;
		keys.propertyPin = PinOf(keys.property);
		keys.layerPin = PinOf(keys.layer);
		keys.fadeNodePin = PinOf(keys.fadeNode);
		return keys;
	}

	void Coordinator::HoldProperties(Tracked& a_entry, const HeldKeys& a_keys)
	{
		auto hold = [&](RE::NiPointer<RE::BSShaderProperty>& a_held, const void* a_key, RE::NiRefObject* a_pin, std::uint32_t a_slot) {
			if (a_held.get() == a_key)
				return;
			// A resident's pipeline template is its first member's held property (KeepResidentsAlive): recounted, so it takes the new one.
			if (a_slot != kNoObjectSlot)
				NoteResidentTouched(a_slot);
			// The batch's pin, from the event that named it.
			auto next = FromPin<RE::BSShaderProperty>(a_pin);
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
		hold(a_entry.property, a_keys.property, a_keys.propertyPin, a_entry.slot);
		hold(a_entry.layerProperty, a_keys.layer, a_keys.layerPin, a_entry.layerSlot);
		const void* fadeNode = a_keys.fadeNode;
		if (a_entry.fadeNodeRef.get() != fadeNode) {
			HandBack(std::move(a_entry.fadeNodeRef));
			// Its pin as AcquireFadeNode's first step takes it; without one, AcquireFadeNode's other ways.
			if (fadeNode && (a_entry.fadeNodeRef = FromPin<RE::NiAVObject>(a_keys.fadeNodePin)))
				++referenceStats.fadeNodes;
			else if (fadeNode)
				AcquireFadeNode(a_entry, fadeNode, "a capture");
		}
	}

	bool Coordinator::AcquireFadeNode(Tracked& a_entry, const void* a_node, const char* a_path)
	{
		auto& r = referenceStats;
		const auto* key = static_cast<const RE::NiAVObject*>(a_node);
		if ((a_entry.fadeNodeRef = Pinned(key))) {
			++r.fadeNodes;
			return true;
		}
		++r.fadeNodesRefused;
		// A shared model's property names the fade node of the instance cloned last: use that root's owner.
		if (const auto owner = rootOwners.find(key); owner != rootOwners.end() && owner->second) {
			a_entry.fadeNodeRef = owner->second;
			++r.fadeNodesFromRoots;
			return true;
		}
		if (r.fadeNodeNamed < 4) {
			++r.fadeNodeNamed;
			const auto* geometry = a_entry.geometry.get();
			const auto* leafRecord = geometry ? mirror.Node(geometry) : nullptr;
			const auto* nodeRecord = mirror.Node(a_node);
			bool ancestor = false;
			for (const void* at = geometry ? mirror.Parent(geometry) : nullptr; at && !ancestor; at = mirror.Parent(at))
				ancestor = at == a_node;
			r.fadeNodeFirst += fmt::format("{}'{}' {}: fade node '{}' {} ({}{}), {} the geometry's mirror chain, wanted at {}, the batch {} pins", r.fadeNodeFirst.empty() ? "" : "; ",
				leafRecord && leafRecord->name ? leafRecord->name : "", static_cast<const void*>(geometry), nodeRecord && nodeRecord->name ? nodeRecord->name : "", a_node,
				!nodeRecord ? "no record" : (nodeRecord->kind & SceneCapture::kKindTree) ? "a tree node" : (nodeRecord->kind & SceneCapture::kKindFadeNode) ? "a fade node" : "another node",
				nodeRecord && nodeRecord->userData ? ", a reference's" : "", ancestor ? "on" : "off", a_path, batchPins.empty() ? "had no" : fmt::format("had {}", batchPins.size()));
		}
		// The render thread pins it (ServeFadeNodeRequests); asked once per key.
		if (a_entry.geometry && a_entry.fadeNodeRequested != a_node) {
			a_entry.fadeNodeRequested = a_node;
			channels.fadeNodeRequests.Push(FadeNodeRequest{ a_entry.geometry, a_node });
			++r.fadeNodesRequested;
		}
		return false;
	}

	void Coordinator::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live, GeometryPrep& a_prep)
	{
		// The reads are a_prep's (maybe from the pool: the mirror, the batch's pins, category nodes, gate roots); here the stores, in event
		// order. Reference: the entry's, else the batch's pin, else (a live walk) the pointer.
		RE::NiPointer<RE::BSGeometry> reference;
		auto it = tracked.find(a_geometry);
		if (it != tracked.end() && it->second.geometry.get() == a_geometry)
			reference = it->second.geometry;
		else if (!(reference = FromPin<RE::BSGeometry>(a_prep.geometryPin)) && a_live) {
			++referenceStats.live;
			reference.reset(a_geometry);
		}
		if (!reference) {
			// Nothing holds it: it left (its event is next batch's) or a pin is missing. Rechecked after the next batch.
			++referenceStats.refused;
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::Reference)];
			NoteMirrorRead(MirrorRead::Reference, true, [&] { return fmt::format("geometry {} under category node {}", static_cast<const void*>(a_geometry), static_cast<const void*>(a_categoryNode)); },
				[this, key = static_cast<const void*>(a_geometry), a_categoryNode] {
					Ineligible reason = Ineligible::None;
					bool missing = false;
					return MirrorCategoryNode(key, reason, missing) != a_categoryNode;
				});
			return;
		}
		const bool inserted = it == tracked.end();
		if (inserted)
			it = tracked.try_emplace(a_geometry).first;
		trackedLayout += inserted ? 1 : 0;
		auto& entry = it->second;
		const auto* ownerReference = a_prep.ownerReference;
		const auto identity = sceneIdentity.Attach(a_geometry, ownerReference);
		if (identity.replaced) {
			ReleaseObjectSlot(entry);
			entry.ownerResolved = false;
			entry.entryClassResolved = false;
		}
		if (inserted) {
			entry.trackedFrame = sceneFrame;
			entry.trackedBy = addSource;
		}
		entry.identity = identity.member;
		entry.groupIdentity = identity.group;
		entry.ownerReference = ownerReference;
		// A changed key (the engine moved the object to another room) is looked up now: its resident slots keep the old value otherwise,
		// since the joins write it only when they evaluate the object again.
		if (entry.ancestorKey != a_prep.ancestorKey) {
			entry.ancestorKey = a_prep.ancestorKey;
			if (Extensions::AncestorValueOn())
				RefreshAncestorValue(entry);
			else
				entry.ancestorMapGeneration = 0;
		}
		entry.geometry = std::move(reference);
		HoldProperties(entry, a_prep.held);
		// Its entry class's held parent (a face shape's head), held from the batch's pins.
		{
			const auto* head = a_prep.source;
			if (entry.sourceRef.get() != head) {
				HandBack(std::move(entry.sourceRef));
				if (head && !(entry.sourceRef = FromPin<RE::NiNode>(a_prep.sourcePin)))
					++referenceStats.refused;
			}
		}
		// Its category node's members (RefreshCategoryNodes, DrainHiddenEvents).
		if (entry.categoryNode != a_categoryNode) {
			if (entry.categoryNode)
				Unlist(categoryMembers, categoryMembersAt, static_cast<const RE::NiNode*>(entry.categoryNode), a_geometry);
			List(categoryMembers, categoryMembersAt, static_cast<const RE::NiNode*>(a_categoryNode), a_geometry);
		}
		entry.categoryNode = a_categoryNode;
		// No mirror record (an uncaptured attach): the render thread captures it at the next frame's start.
		if (!a_prep.mirrored)
			channels.mirrorCaptureRequests.Push(RE::NiPointer<RE::NiAVObject>(a_geometry));
		entry.parentReason = a_parentReason;
		// (Re)attached: reclassify, and re-evaluate the sun entry's other dependents (its bound now includes this one).
		entry.candidateFrame = 0;
		{
			UnlistDependents(a_geometry, entry, true);
			entry.sunEntryResolved = false;
			entry.entryRoot = nullptr;
			// The prepare's read (counted here), or ResolveSunEntry's with its live check.
			const bool prepared = a_prep.resolved && !a_live;
			if (prepared) {
				++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SunEntry)];
				entry.sunEntryResolved = true;
				entry.entryRoot = static_cast<const RE::NiAVObject*>(a_prep.sunEntry);
			} else {
				ResolveSunEntry(entry, *a_geometry, a_live);
			}
			if (entry.entryRoot && !OwnRoot(entry.entryRoot, a_live))
				entry.entryRoot = nullptr;
			if (entry.entryRoot) {
				List(rootDependents, rootDependentsAt, entry.entryRoot, a_geometry);
				// The root's reference: the prepare's read, or the mirror's.
				const auto* rootRecord = prepared ? nullptr : mirror.Node(entry.entryRoot);
				if (const auto* rootRef = prepared ? a_prep.sunEntryReference : rootRecord ? static_cast<const RE::TESObjectREFR*>(rootRecord->userData) : nullptr;
					rootRef && rootReference.try_emplace(entry.entryRoot, rootRef).second)
					referenceRoot[rootRef] = entry.entryRoot;
				entry.listedRoot = entry.entryRoot;
				dirtyRoots.push_back(entry.entryRoot);
				MarkListedRootDirty(entry.entryRoot);
			}
			// As LightEntryOf: the prepare's actor root, else the sun entry unless a category node owns it.
			const RE::NiAVObject* lightEntry = nullptr;
			if (!prepared) {
				lightEntry = LightEntryOf(entry, *a_geometry, a_live);
			} else {
				if (entry.categoryNode)
					++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::LightEntry)];
				lightEntry = a_prep.lightActorRoot ? a_prep.lightActorRoot : entry.entryRoot && !a_prep.sunEntryCategory ? entry.entryRoot : nullptr;
			}
			if (lightEntry && OwnRoot(lightEntry, a_live)) {
				auto& dependents = lightDependents[lightEntry];
				lightEntriesAppeared += dependents.empty() ? 1 : 0;
				if (dependents.empty())
					lightEntryChanges.push_back({ lightEntry, 1 });  // a light entry new to the frame's set (IsLightEntry, through the log)
				List(lightDependents, lightDependentsAt, lightEntry, a_geometry);
				entry.lightRoot = lightEntry;
				MarkLightEntryDirty(lightEntry);
			}
		}
		// A ranged entry (object LOD): drawn ranges from here on follow its ranges events (the prepare's copy, as their component takes it).
		if (a_prep.ranged)
			entryRanges[a_geometry] = std::move(a_prep.ranges);
		pendingEvaluation.push_back(a_geometry);
		// Under a root marked earlier (a LOD gate's): the marking component's (the nearest, found by the prepare's mark walk).
		if (a_prep.mark)
			ComponentsUnderMark(a_geometry, a_prep.mark);
	}

	const RE::NiAVObject* Coordinator::LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const
	{
		// An actor's root (the category node's child it hangs from, its reference an owner class's: Extensions::OwnerClassOf) is the light
		// entry for everything under it, carried items too.
		if (const auto* category = a_tracked.categoryNode) {
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::LightEntry)];
			const void* root = &a_geometry;
			for (const void* node = mirror.Parent(&a_geometry); node && node != category; node = mirror.Parent(node))
				root = node;
			const auto* record = root != &a_geometry ? mirror.Node(root) : nullptr;
			const bool actorRoot = record && record->parent == category && Extensions::OwnerClassOf(*record).ownerClass != Api::EntryClass::None;
			// The mirror's, or on a live walk the live chain's; observers compare the two.
			if (!a_live) {
				Diagnostics::Notify(Diagnostics::LightEntryRead{ *this, a_geometry, category, root, actorRoot, nullptr, false, false });
				if (actorRoot)
					return static_cast<const RE::NiAVObject*>(root);
				return a_tracked.entryRoot && !CategoryNodeOwn(a_tracked.entryRoot) ? a_tracked.entryRoot : nullptr;
			}
			const EngineReadWindow::Lease lease;
			bool liveActor = false;
			const RE::NiAVObject* live = LiveLightRoot(a_geometry, category, liveActor);
			Diagnostics::Notify(Diagnostics::LightEntryRead{ *this, a_geometry, category, root, actorRoot, live, liveActor, true });
			if (liveActor)
				return live;
		}
		return a_tracked.entryRoot && !CategoryNodeOwn(a_tracked.entryRoot) ? a_tracked.entryRoot : nullptr;
	}

	const RE::NiAVObject* Coordinator::LiveLightRoot(const RE::BSGeometry& a_geometry, const RE::NiNode* a_category, bool& a_actor)
	{
		const RE::NiAVObject* live = &a_geometry;
		for (const auto* node = a_geometry.parent; node && node != a_category; node = node->parent)
			live = node;
		const auto* reference = live != &a_geometry && live->parent == a_category ? live->GetUserData() : nullptr;
		a_actor = reference && Extensions::OwnerClassOfForm(static_cast<std::uint8_t>(reference->GetFormType())).ownerClass != Api::EntryClass::None;
		return live;
	}

	void Coordinator::AddSubtree(RE::NiAVObject* a_root, SubtreeSource a_source)
	{
		// Serial unit: prepare then merge.
		SubtreePrep prep;
		PrepareSubtree(a_root, a_source, prep);
		fanoutStats.subtreeGeometries += prep.geometries.size();
		MergeSubtree(prep);
	}

	void Coordinator::PrepareSubtree(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const
	{
		PrepareSubtreeWalk(a_root, a_source, a_out);
		for (auto& geometry : a_out.geometries)
			PrepareGeometry(geometry.geometry, geometry.reason, a_out.category, geometry);
	}

	void Coordinator::PrepareSubtreeWalk(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const
	{
		// Pure over the mirror, category nodes and portal roots, none of which tracking writes: any thread under a ShardScope. No engine
		// memory (the root is a key), except the mirror-read parity's live check, which runs serially on the pass's thread.
		a_out.root = a_root;
		a_out.source = a_source;
		a_out.category = nullptr;
		a_out.unmirrored = false;
		a_out.geometries.clear();
		// The attach is drained a frame or more after it was queued; a cell transition may have released the subtree.
		if (!a_root)
			return;
		Ineligible reasonAbove = Ineligible::None;
		a_out.category = FindCategoryNode(a_root, &reasonAbove, &a_out.unmirrored);
		if (!a_out.category)
			return;

		// Walk down the mirror carrying the parent reasons. Each geometry's reference is its capture's pin, taken at the merge (the
		// mirror may name one the engine has since freed; the pin holds it).
		std::vector<std::pair<RE::BSGeometry*, Ineligible>> found;
		bool lacking = false;
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::Subtree)];
		MirrorSubtree(a_root, reasonAbove, found, lacking);
		if (lacking)
			++MirrorReads().unmirrored[static_cast<std::size_t>(MirrorRead::Subtree)];
		Diagnostics::Notify(Diagnostics::SubtreeRead{ *this, a_root, reasonAbove, found, lacking });
		// Each geometry's prepare is PrepareGeometry's (PrepareSubtree's, or PrepareSubtrees' across the round's subtrees).
		a_out.geometries.resize(found.size());
		for (std::size_t i = 0; i < found.size(); ++i) {
			a_out.geometries[i].geometry = found[i].first;
			a_out.geometries[i].reason = found[i].second;
		}
	}

	void Coordinator::PrepareGeometry(RE::BSGeometry* a_geometry, Ineligible a_reason, const RE::NiNode* a_category, GeometryPrep& a_out) const
	{
		// AddGeometry's mirror reads, keys only. Sun and light entry reads are counted at the merge (a refused reference reads neither).
		a_out = GeometryPrep{};
		a_out.geometry = a_geometry;
		a_out.reason = a_reason;
		const auto* record = mirror.Node(a_geometry);
		a_out.mirrored = record != nullptr;
		a_out.ownerReference = record && Extensions::OwnerClassOf(*record).ownerClass != Api::EntryClass::None ?
		                           static_cast<const RE::TESObjectREFR*>(record->userData) :
		                           nullptr;
		a_out.ancestorKey = AncestorKeyOf(a_geometry);
		// Its entry class's held parent: the parent its geometry type's rule matches under and holds (Api::GeometryTypeContribution::entry:
		// a face shape's head, a dynamic shape's BSFaceGenNiNode).
		const auto* g = mirror.Geometry(a_geometry);
		const auto* parent = record && record->parent ? mirror.Node(record->parent) : nullptr;
		const auto entryClass = parent && g ? Extensions::EntryClassOf(g->type, parent->kind) : Api::EntryClass::None;
		const bool held = entryClass != Api::EntryClass::None && Extensions::EntryClassRuleOf(entryClass).holdsParent;
		a_out.source = held ? static_cast<const RE::NiNode*>(record->parent) : nullptr;
		a_out.ranged = g && Extensions::GeometryTypeRanged(g->type);
		// The batch's pins (frozen while the batch's adds prepare and merge), HoldProperties' reads, a ranged entry's (object LOD's) ranges.
		a_out.geometryPin = PinOf(a_geometry);
		a_out.sourcePin = a_out.source ? PinOf(a_out.source) : nullptr;
		a_out.held = HeldKeysOf(a_geometry);
		if (a_out.ranged && g)
			a_out.ranges = g->ranges;
		// The prepare's mark walk: the mirror's chain up to the category node, the nearest marked root on it (the marks change only with the
		// mirror's events, before the adds: the parallel prepares read them as data).
		if (!rootMarks.empty())
			for (const void* key = record ? record->parent : nullptr; key;) {
				if (const auto at = rootMarks.find(key); at != rootMarks.end()) {
					a_out.mark = at->second.mark;
					break;
				}
				if (key == a_category)
					break;
				const auto* node = mirror.Node(key);
				key = node ? node->parent : nullptr;
			}
		// While an observer checks these reads against the live chain, the merge resolves both.
		if (Diagnostics::Armed<Diagnostics::SunEntryRead>())
			return;
		a_out.resolved = true;
		a_out.sunEntry = MirrorSunEntry(a_geometry);
		if (a_out.sunEntry) {
			const auto* rootRecord = mirror.Node(a_out.sunEntry);
			a_out.sunEntryReference = rootRecord ? static_cast<const RE::TESObjectREFR*>(rootRecord->userData) : nullptr;
			a_out.sunEntryCategory = CategoryNodeOwn(static_cast<const RE::NiAVObject*>(a_out.sunEntry));
		}
		// LightEntryOf's mirror walk.
		if (a_category) {
			const void* root = a_geometry;
			for (const void* node = mirror.Parent(a_geometry); node && node != a_category; node = mirror.Parent(node))
				root = node;
			const auto* rootRecord = root != static_cast<const void*>(a_geometry) ? mirror.Node(root) : nullptr;
			if (rootRecord && rootRecord->parent == a_category && Extensions::OwnerClassOf(*rootRecord).ownerClass != Api::EntryClass::None)
				a_out.lightActorRoot = static_cast<const RE::NiAVObject*>(root);
		}
	}

	void Coordinator::MergeSubtree(SubtreePrep& a_prep)
	{
		// In event order, on the pass's thread.
		if (!a_prep.root)
			return;
		if (!a_prep.category) {
			if (a_prep.unmirrored && a_prep.source == SubtreeSource::CategoryWalk) {
				// A pin's copy, never a reference made from a key.
				if (auto root = Pinned(static_cast<const RE::NiAVObject*>(a_prep.root))) {
					++subtreesPended;
					channels.mirrorCaptureRequests.Push(root);
					pendingSubtrees.push_back(std::move(root));
				} else {
					++subtreesDropped;
				}
			} else if (a_prep.unmirrored && a_prep.source == SubtreeSource::Retry) {
				++subtreesDropped;
			}
			return;
		}
		for (auto& geometry : a_prep.geometries)
			AddGeometry(geometry.geometry, a_prep.category, geometry.reason, false, geometry);
	}

	void Coordinator::PrepareSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots, std::uint32_t a_mode, std::vector<SubtreePrep>& a_out)
	{
		// By chunks on the pool (mode 2) or here; mirror reads counted per chunk (not per thread) and merged in chunk order.
		const std::size_t count = a_roots.size();
		const bool parallel = a_mode == 2;
		const std::size_t chunks = (count + kSubtreeGrain - 1) / kSubtreeGrain;
		auto& f = fanoutStats;
		f.subtreeParallelRounds += parallel ? 1 : 0;
		f.subtreeParallelRoots += parallel ? count : 0;
		a_out.resize(count);
		// The walks by roots, then every geometry's prepare across the round's subtrees by its own chunks: a few large attaches (a cell's
		// root, hundreds of geometries) reach the pool though their roots are few.
		auto walk = [&](std::vector<SubtreePrep>& a_into, std::vector<MirrorReadStats>& a_counters, bool a_pool) {
			auto body = [&](std::size_t a_begin, std::size_t a_end) {
				ShardScope scope(*this, a_pool, &a_counters[a_begin / kSubtreeGrain]);
				for (std::size_t i = a_begin; i < a_end; ++i)
					PrepareSubtreeWalk(a_roots[i].first, a_roots[i].second, a_into[i]);
			};
			if (a_pool) {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.Subtrees", count, kSubtreeGrain, body);
			} else {
				for (std::size_t begin = 0; begin < count; begin += kSubtreeGrain)
					body(begin, std::min(count, begin + kSubtreeGrain));
			}
		};
		std::vector<MirrorReadStats> counters(chunks);
		std::vector<MirrorReadStats> geometryCounters;
		{
			ZoneScopedN("CS.DCLF.Scene.SubtreesFanout");
			const auto start = std::chrono::steady_clock::now();
			walk(a_out, counters, parallel);
			auto& index = subtreeGeometryIndex;
			index.clear();
			for (std::uint32_t p = 0; p < a_out.size(); ++p)
				if (a_out[p].category)
					for (std::uint32_t g = 0; g < a_out[p].geometries.size(); ++g)
						index.emplace_back(p, g);
			const std::size_t geometries = index.size();
			const bool pool = geometries && FanoutMode(geometries, kGeometryFanoutMin) == 2;
			auto& a = attachStats;
			a.geometryRounds += geometries ? 1 : 0;
			a.geometryPooled += pool ? 1 : 0;
			a.geometries += geometries;
			a.geometriesPooled += pool ? geometries : 0;
			geometryCounters.resize((geometries + kGeometryGrain - 1) / kGeometryGrain);
			auto body = [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.Scene.SubtreeGeometriesShards");
				ShardScope scope(*this, pool, &geometryCounters[a_begin / kGeometryGrain]);
				for (std::size_t i = a_begin; i < a_end; ++i) {
					auto& prep = a_out[index[i].first];
					auto& geometry = prep.geometries[index[i].second];
					PrepareGeometry(geometry.geometry, geometry.reason, prep.category, geometry);
				}
			};
			if (pool) {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.SubtreeGeometries", geometries, kGeometryGrain, body);
			} else {
				for (std::size_t begin = 0; begin < geometries; begin += kGeometryGrain)
					body(begin, std::min(geometries, begin + kGeometryGrain));
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			AddScenePartMs(ScenePart::SubtreesFanout, ms);
			f.subtreePrepareMs += ms;
		}
		for (auto& chunk : counters)
			MergeMirrorReads(chunk);
		for (auto& chunk : geometryCounters)
			MergeMirrorReads(chunk);
		for (const auto& prep : a_out)
			f.subtreeGeometries += prep.geometries.size();
	}

	void Coordinator::AddSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots)
	{
		if (a_roots.empty())
			return;
		auto& f = fanoutStats;
		++f.subtreeRounds;
		f.subtreeRoots += a_roots.size();
		const std::uint32_t mode = FanoutMode(a_roots.size(), kSubtreeFanoutMin);
		if (mode == 0) {
			for (const auto& [root, source] : a_roots)
				AddSubtree(root, source);
			return;
		}
		// Prepare all, then merge in order: merges never write what a prepare reads (mirror, category nodes, portal roots: witnessed).
		[[maybe_unused]] const std::uint64_t mirrorWrites = mirror.Writes();
		[[maybe_unused]] const std::size_t categories = categoryNodes.size();
		[[maybe_unused]] const std::size_t portalRoots = alwaysRenderRoots.size();
		PrepareSubtrees(a_roots, mode, subtreePreps);
		{
			ZoneScopedN("CS.DCLF.Scene.SubtreesMerge");
			const auto start = std::chrono::steady_clock::now();
			for (auto& prep : subtreePreps)
				MergeSubtree(prep);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			AddScenePartMs(ScenePart::SubtreesMerge, ms);
			f.subtreeMergeMs += ms;
		}
		assert(mirror.Writes() == mirrorWrites && categoryNodes.size() == categories && alwaysRenderRoots.size() == portalRoots);
	}

	void Coordinator::MirrorSubtree(const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored) const
	{
		// Switch selections were caught up at ingestion (CatchUpSwitches).
		if (ForEachMirror(mirror, a_root, a_reason, [&](const void* a_key, const SceneCapture::NodeRecord& a_record, Ineligible a_above, Ineligible& a_below) {
				if (a_record.kind & SceneCapture::kKindGeometry)
					a_out.emplace_back(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key)), a_above);
				else if (a_record.kind & SceneCapture::kKindNode)
					a_below = Eligibility::CombineParentReasons(a_above, Eligibility::ParentReasonOf(a_record));
			}))
			a_unmirrored = true;
	}




	bool Coordinator::HiddenForWalk(const RE::NiAVObject* a_object) const
	{
		// The render thread's frame-start capture first.
		const auto& cullHidden = FrameGlobals::Current().cullHidden;
		const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), a_object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHidden.end() && it->first == a_object)
			return it->second;
		return IsHidden(a_object);
	}

	bool Coordinator::HiddenForWalk(const void* a_object, const SceneCapture::NodeRecord& a_record) const
	{
		const auto& cullHidden = FrameGlobals::Current().cullHidden;
		const auto* object = static_cast<const RE::NiAVObject*>(a_object);
		const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHidden.end() && it->first == object)
			return it->second;
		// A gated LOD block's own hide is the gate's (SceneMirror::SetGated), not the scene's.
		return (a_record.flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)) != 0 && !mirror.Gated(a_object);
	}

	std::int32_t Coordinator::BillboardModeOf(const Tracked& a_tracked) const
	{
		if (a_tracked.parentReason != Ineligible::Billboard)
			return -1;
		// First-person entries too (magic hand glows): the first-person forward part's b12 block (Features/FirstPerson's depth epoch) is world
		// space throughout (c0-c2 the camera rotation, c40 the eye at GetFirstPersonNodePosition), sampled before the shift.
		// The turned shapes (the census's rule too): the parent contribution admits them (Features/Billboards: the billboards toggle), a
		// skinless BSTriShape with the property of a port the billboards turn (the Effect port's).
		const auto leaf = mirror.Leaf(a_tracked.geometry.get());
		const auto* port = leaf ? SceneCapture::LeafView::Port(leaf.property) : nullptr;
		if (!leaf || !Extensions::BillboardTurnedLeaf(ActiveToggles(), port ? port->port : std::uint8_t{ 0 }, leaf.Type(), leaf.Skinned()))
			return -1;
		std::int32_t mode = -1;
		std::uint32_t billboards = 0;
		const auto* record = mirror.Node(a_tracked.geometry.get());
		for (const void* key = record ? record->parent : nullptr; key && key != a_tracked.categoryNode; key = record->parent) {
			record = mirror.Node(key);
			if (!record)
				return -1;
			if (record->kind & SceneCapture::kKindBillboard) {
				++billboards;
				mode = static_cast<std::int32_t>(SceneCapture::BillboardModeOf(record->kind));
			}
		}
		// Exactly one: a nested billboard turns from a parent world the outer node's cull wrote.
		return billboards == 1 && mode <= 5 ? mode : -1;
	}

	void Coordinator::ViewDecalForward(const Tracked& a_tracked, LightingDescriptors& a_descriptors) const
	{
		// Decal groups 1 and 2 (hints 2 and 3); group 3 is a multi-index layer's, never a view root's (first person's).
		const std::uint32_t group = a_descriptors.decalGroup;
		if (group != 1 && group != 2)
			return;
		const auto view = ViewRootOf(a_tracked.categoryNode);
		if (view == Api::EngineView::Count || !Extensions::ViewForwardDecals(view))
			return;
		// The forward pixel stage: drop its kind's Deferred bit, as LightingShaderDescriptors does for a forward key.
		a_descriptors.pixel &= ~Extensions::PortRules(a_descriptors.port).deferredPixelBit;
		a_descriptors.forward = true;
		a_descriptors.forwardBlendMode = group == 2 ? a_descriptors.decalBlendMode : 0u;
		a_descriptors.forwardDepthTest = true;
		a_descriptors.forwardDepthWrite = group == 1;
		a_descriptors.forwardDecalGroup = group;
		a_descriptors.decalGroup = 0;
		a_descriptors.decalBlendMode = 0;
		a_descriptors.decalWriteMode = 0;
	}

	Ineligible Coordinator::ClassifyFrame(const Tracked& a_tracked) const
	{
		// The leaves under switch nodes are the parent contribution's (Features/SwitchNodes: CS_DCLF_SWITCH_NODES and its toggle).
		const auto switchLeaves = [] { return Extensions::ParentAdmits(ActiveToggles(), Api::EngineParent::Switch, Api::ParentLeaves::SelectedLeaves); };
		const bool underSwitch = a_tracked.parentReason == Ineligible::Switch;
		if (underSwitch && !switchLeaves())
			return Ineligible::Switch;
		// An effect leaf under one billboard node is DCLF's, turned by its vertex stage (Billboards.h). Billboard outranks switch and
		// ordered in parentReason, so the chain walk below applies their tests too.
		const bool underBillboard = a_tracked.parentReason == Ineligible::Billboard;
		// A particle system under billboards needs no vertex-stage turn: 0x140d3e650 runs each child's UpdateDownwardPass (slot 0x2C,
		// NiPSysUpdateCtlr), after which ParticleSnapshots snapshots it, and NiParticleSystem::UpdateWorldData (vtable 0x1419c3898 slot
		// 0x30, 0x140dd4c90) is empty, so the engine draws the last update's world and particles, which the snapshot copies.
		// The parent contribution's (Features/Billboards: CS_DCLF_BILLBOARD_PARTICLES and the billboards toggle).
		const auto particlesUnderBillboard = [&] {
			if (!Extensions::ParentAdmits(ActiveToggles(), Api::EngineParent::Billboard, Api::ParentLeaves::ParticleSystems))
				return false;
			const auto leaf = mirror.Leaf(a_tracked.geometry.get());
			return leaf && ParticleGeometryType(leaf.Type());
		};
		if (underBillboard && BillboardModeOf(a_tracked) < 0 && !particlesUnderBillboard())
			return Ineligible::Billboard;
		const bool chainTests = underSwitch || underBillboard;
		// Under an ordered node only a blended leaf is rejected (the node orders only those passes), unless the forward epoch takes it,
		// sorted in its node's group by depth and child order (ForwardSortCS). Under a switch the chain walk finds it.
		const bool ordered = a_tracked.parentReason == Ineligible::UnsupportedParent;
		if (a_tracked.parentReason != Ineligible::None && !underSwitch && !ordered && !underBillboard)
			return a_tracked.parentReason;
		const auto orderedLeaf = [&] {
			return !Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Forward) && Eligibility::OrderedLeaf(mirror.Leaf(a_tracked.geometry.get()));
		};
		if (ordered && orderedLeaf())
			return Ineligible::UnsupportedParent;

		// Hidden anywhere up to the category node; part of an actor; under a switch not drawing this branch. The leaves under an actor
		// are the parent contribution's (Features/Actors: CS_DCLF_ACTORS and its toggle).
		const bool actors = Extensions::ParentAdmits(ActiveToggles(), Api::EngineParent::Actor, Api::ParentLeaves::AllLeaves);
		const void* child = nullptr;
		const void* key = a_tracked.geometry.get();
		std::uint32_t depth = 0;
		for (const auto* record = mirror.Node(key); record; child = key, key = record->parent, record = key ? mirror.Node(key) : nullptr, ++depth) {
			// A CategoryRoot view's root's kHidden belongs to the view (the first-person root's: Main::Draw, unk_FPCulling and RenderDepth
			// toggle it within the frame, FUN_140736150 at the camera switch): the chain stops before it. Whether the view draws is the
			// view's (the first-person latch).
			if (const auto view = ViewRootOf(key); view != Api::EngineView::Count) {
				if (a_tracked.categoryNode != key)
					Diagnostics::Notify(Diagnostics::ViewMisfiled{ view, a_tracked.geometry.get(), a_tracked.categoryNode });
				break;
			}
			if (HiddenForWalk(key, *record)) {
				// Diagnostic (pointers only, never dereferenced): a root view's entry or subtree geometry hidden (Features/FirstPerson's log).
				if (Diagnostics::Armed<Diagnostics::RootViewHidden>() && FirstViewRoot()) {
					const auto& globals = FrameGlobals::Current();
					const auto& skeleton = globals.firstPersonGeometry;
					const bool sameRoot = ViewRootEntry(a_tracked.categoryNode);
					if (sameRoot ||
						std::binary_search(skeleton.begin(), skeleton.end(), static_cast<const RE::NiAVObject*>(static_cast<const void*>(a_tracked.geometry.get()))))
						Diagnostics::Notify(Diagnostics::RootViewHidden{ *this, a_tracked.geometry.get(), a_tracked.categoryNode, FirstViewRoot(), sameRoot, key, depth,
							(record->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)) != 0, globals.cullHidden });
				}
				return Ineligible::Hidden;
			}
			if (key == a_tracked.categoryNode)
				break;
			if (underBillboard && (record->kind & SceneCapture::kKindSwitch) && !switchLeaves())
				return Ineligible::Switch;
			if (chainTests && child && (record->kind & SceneCapture::kKindSwitch) && !SwitchSelects(*record, child))
				return Ineligible::Switch;
			if (chainTests && (record->kind & SceneCapture::kKindOrdered) && orderedLeaf())
				return Ineligible::UnsupportedParent;
			if (!actors && record->userData && record->actor)
				return Ineligible::Actor;
		}


		return Ineligible::None;
	}

}
