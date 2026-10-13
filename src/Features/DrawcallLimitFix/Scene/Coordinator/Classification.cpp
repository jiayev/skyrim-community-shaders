// The coordinator's classification (scene lane; subtree prepares across the pool): category nodes found and refreshed from the
// render thread's capture, subtrees and geometries tracked from the mirror (prepare, then merge in event order), and the per-entry
// verdicts (ClassifyFrame, HiddenForWalk, billboards, first person).

#include "Internal.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Runtime/SceneScheduler.h"
#include <cassert>

namespace DCLF::Scene
{
	std::uint32_t Coordinator::SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow)
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

	std::uint32_t Coordinator::SkinPartitionMask(const SceneCapture::GeometryRecord& a_geometry, std::uint32_t a_lodRow)
	{
		if (!a_geometry.skinPartition || a_lodRow >= 8)
			return 0;
		// A dismember skin's shown flags (empty otherwise).
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

	std::uint16_t Coordinator::SkinPartitionsOf(const SceneCapture::GeometryRecord& a_geometry)
	{
		if (!a_geometry.skin || !a_geometry.skinPartition)
			return 0;
		// Row 3 (cumulative over levels); a LOD skin's draw narrows it (SkinLodPartitionsOf).
		const std::uint32_t mask = SkinPartitionMask(a_geometry, 3);
		return static_cast<std::uint16_t>(!mask ? kNoPartitions : a_geometry.partitions.size() > 1 ? mask : 0u);
	}

	std::uint32_t Coordinator::SkinLodPartitionsOf(const SceneCapture::LeafView& a_leaf)
	{
		if (!a_leaf || !a_leaf.geometry->skin || !a_leaf.geometry->skinPartition || !(a_leaf.node->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kMeshLOD)) ||
			!a_leaf.property || !a_leaf.property->fadeNode)
			return 0;
		std::uint32_t word = 0;
		for (std::uint32_t level = 0; level < 4; ++level)
			word |= (SkinPartitionMask(*a_leaf.geometry, level) & 0xFFu) << (8 * level);
		return word;
	}

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
		// The first-person root is its own category node, never filed under one above it (whose members take no first-person phase).
		if (a_object && a_object == firstPersonCategory) {
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
		// The first-person root files its subtree under itself (MirrorCategoryNode).
		if (a_object && a_object == firstPersonCategory) {
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
		firstPersonCategory = capture->firstPersonRoot;
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
				// Filed under a portal root: stale when the root left.
				if (removedRoot && alwaysRenderRoots.size()) {
					const void* top = geometry;
					const void* parent = mirror.Parent(top);
					for (; parent && parent != entry.categoryNode; parent = mirror.Parent(top))
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
		// Copies of the capture's references (never made from a key).
		alwaysRenderRoots.clear();
		for (const auto& [root, entry] : currentRoots)
			alwaysRenderRoots.emplace(root, AlwaysRenderRoot{ entry.root, entry.category });
		std::erase_if(categoryFound, [&](const auto& a_entry) { return !categoryNodes.contains(const_cast<RE::NiNode*>(a_entry.first)); });
		const TrackSource previousSource = addSource;
		if (addSource != TrackSource::Rescan)
			addSource = TrackSource::CategoryAppeared;
		// Roots listed in walk order, then tracked together (AddSubtrees prepares on the pool for a rescan and merges in this order).
		std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> walked;
		for (auto* node : added) {
			categoryFound[node] = { sceneFrame, cause };
			const auto keys = capture->children.find(node);
			if (keys == capture->children.end()) {
				++categoryChildrenMissing;
				continue;
			}
			for (const auto* child : keys->second)
				walked.emplace_back(const_cast<RE::NiAVObject*>(child), SubtreeSource::CategoryWalk);
		}
		// Roots of a graph whose shared node just appeared.
		for (const auto& [root, entry] : alwaysRenderRoots) {
			if (std::find(added.begin(), added.end(), entry.category) != added.end())
				walked.emplace_back(entry.root.get(), SubtreeSource::CategoryWalk);
		}
		for (auto* root : addedRoots)
			walked.emplace_back(root, SubtreeSource::CategoryWalk);
		AddSubtrees(walked);
		addSource = previousSource;
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
				a_tracked.sunEntryNode = static_cast<const RE::NiAVObject*>(entry);
				Diagnostics::Notify(Diagnostics::SunEntryRead{ *this, a_geometry, entry, nullptr, false });
				return;
			}
			const EngineReadWindow::Lease lease;
			const RE::NiAVObject* live = LiveSunEntry(a_geometry);
			a_tracked.sunEntryNode = live;
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

	std::uint32_t Coordinator::OcclusionTechniqueOf(const SceneCapture::LeafView& a_leaf, bool a_skylighting) const
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
			// The nearest fade node's BSX flags.
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

	const RE::NiNode* Coordinator::RoomNodeOf(const void* a_object) const
	{
		// Light Limit Fix's GetParentRoomNode: the nearest BSMultiBoundRoom or BSPortalSharedNode up the chain, by RTTI.
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

	void Coordinator::HoldProperties(Tracked& a_entry)
	{
		const auto* record = mirror.Geometry(a_entry.geometry.get());
		auto hold = [&](RE::NiPointer<RE::BSShaderProperty>& a_held, const void* a_key) {
			if (a_held.get() == a_key)
				return;
			// The batch's pin, from the event that named it.
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
		// The property's fade node, for the tree and fade-root owners (an attach's capture pins every ancestor).
		const auto* property = record && record->property ? mirror.Property(record->property) : nullptr;
		const void* fadeNode = property ? property->fadeNode : nullptr;
		// The engine's "no fade node" sentinel is no root.
		static const bool emptyIsNone = SwitchOnUnlessZero(Switch::EmptyFadeNode);
		if (emptyIsNone && fadeNode == Engine::EmptyFadeNode())
			fadeNode = nullptr;
		if (a_entry.fadeNodeRef.get() != fadeNode) {
			HandBack(std::move(a_entry.fadeNodeRef));
			if (fadeNode)
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

	void Coordinator::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live, const GeometryPrep& a_prep)
	{
		// The mirror reads are a_prep's (maybe from the pool); here the writes, in event order. Reference: the entry's, else the batch's
		// pin, else (a live walk) the pointer.
		RE::NiPointer<RE::BSGeometry> reference;
		if (const auto known = tracked.find(a_geometry); known != tracked.end() && known->second.geometry.get() == a_geometry)
			reference = known->second.geometry;
		else if (!(reference = Pinned(a_geometry)) && a_live) {
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
		const auto [it, inserted] = tracked.try_emplace(a_geometry);
		trackedLayout += inserted ? 1 : 0;
		auto& entry = it->second;
		const auto* actorOwner = a_prep.actorOwner;
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
		entry.roomNode = a_prep.roomNode;
		entry.roomMapGeneration = 0;
		entry.geometry = std::move(reference);
		HoldProperties(entry);
		// A face shape's head, held from the batch's pins.
		{
			const auto* head = a_prep.faceHead;
			if (entry.faceHeadRef.get() != head) {
				HandBack(std::move(entry.faceHeadRef));
				if (head && !(entry.faceHeadRef = Pinned(head)))
					++referenceStats.refused;
			}
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
			entry.sunEntryNode = nullptr;
			// The prepare's read (counted here), or ResolveSunEntry's with its live check.
			const bool prepared = a_prep.resolved && !a_live;
			if (prepared) {
				++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SunEntry)];
				entry.sunEntryResolved = true;
				entry.sunEntryNode = static_cast<const RE::NiAVObject*>(a_prep.sunEntry);
			} else {
				ResolveSunEntry(entry, *a_geometry, a_live);
			}
			if (entry.sunEntryNode && !OwnRoot(entry.sunEntryNode, a_live))
				entry.sunEntryNode = nullptr;
			if (entry.sunEntryNode) {
				rootDependents[entry.sunEntryNode].push_back(a_geometry);
				const auto* rootRecord = mirror.Node(entry.sunEntryNode);
				if (const auto* rootRef = rootRecord ? static_cast<const RE::TESObjectREFR*>(rootRecord->userData) : nullptr;
					rootRef && rootReference.try_emplace(entry.sunEntryNode, rootRef).second)
					referenceRoot[rootRef] = entry.sunEntryNode;
				entry.listedRoot = entry.sunEntryNode;
				dirtyRoots.push_back(entry.sunEntryNode);
				MarkSunEntryDirty(entry.sunEntryNode);
			}
			// As LightEntryOf: the prepare's actor root, else the sun entry unless a category node owns it.
			const RE::NiAVObject* lightEntry = nullptr;
			if (!prepared) {
				lightEntry = LightEntryOf(entry, *a_geometry, a_live);
			} else {
				if (entry.categoryNode)
					++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::LightEntry)];
				lightEntry = a_prep.lightActorRoot ? a_prep.lightActorRoot : entry.sunEntryNode && !CategoryNodeOwn(entry.sunEntryNode) ? entry.sunEntryNode : nullptr;
			}
			if (lightEntry && OwnRoot(lightEntry, a_live)) {
				auto& dependents = lightDependents[lightEntry];
				lightEntriesAppeared += dependents.empty() ? 1 : 0;
				if (dependents.empty())
					lightEntryChanges.push_back({ lightEntry, 1 });  // a light entry new to the frame's set (IsLightEntry, through the log)
				dependents.push_back(a_geometry);
				entry.lightRoot = lightEntry;
				MarkLightEntryDirty(lightEntry);
			}
		}
		// Object LOD: drawn ranges from here on follow its segment events.
		if (a_prep.subIndex)
			SampleLodRanges(*a_geometry, false);
		pendingEvaluation.push_back(a_geometry);
		// Under a LOD gate's root applied earlier: tag it.
		if (!gateOfRoot.empty())
			TagGateAbove(a_geometry, entry);
	}

	const RE::NiAVObject* Coordinator::LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const
	{
		// An actor's root (the category node's child it hangs from) is the light entry for everything under it, carried items too.
		if (const auto* category = a_tracked.categoryNode) {
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::LightEntry)];
			const void* root = &a_geometry;
			for (const void* node = mirror.Parent(&a_geometry); node && node != category; node = mirror.Parent(node))
				root = node;
			const auto* record = root != &a_geometry ? mirror.Node(root) : nullptr;
			const bool actorRoot = record && record->parent == category && record->IsActorCharacter();
			// The mirror's, or on a live walk the live chain's; observers compare the two.
			if (!a_live) {
				Diagnostics::Notify(Diagnostics::LightEntryRead{ *this, a_geometry, category, root, actorRoot, nullptr, false, false });
				if (actorRoot)
					return static_cast<const RE::NiAVObject*>(root);
				return a_tracked.sunEntryNode && !CategoryNodeOwn(a_tracked.sunEntryNode) ? a_tracked.sunEntryNode : nullptr;
			}
			const EngineReadWindow::Lease lease;
			bool liveActor = false;
			const RE::NiAVObject* live = LiveLightRoot(a_geometry, category, liveActor);
			Diagnostics::Notify(Diagnostics::LightEntryRead{ *this, a_geometry, category, root, actorRoot, live, liveActor, true });
			if (liveActor)
				return live;
		}
		return a_tracked.sunEntryNode && !CategoryNodeOwn(a_tracked.sunEntryNode) ? a_tracked.sunEntryNode : nullptr;
	}

	const RE::NiAVObject* Coordinator::LiveLightRoot(const RE::BSGeometry& a_geometry, const RE::NiNode* a_category, bool& a_actor)
	{
		const RE::NiAVObject* live = &a_geometry;
		for (const auto* node = a_geometry.parent; node && node != a_category; node = node->parent)
			live = node;
		const auto* reference = live != &a_geometry && live->parent == a_category ? live->GetUserData() : nullptr;
		a_actor = reference && reference->GetFormType() == RE::FormType::ActorCharacter;
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
		a_out.geometries.resize(found.size());
		for (std::size_t i = 0; i < found.size(); ++i)
			PrepareGeometry(found[i].first, found[i].second, a_out.category, a_out.geometries[i]);
	}

	void Coordinator::PrepareGeometry(RE::BSGeometry* a_geometry, Ineligible a_reason, const RE::NiNode* a_category, GeometryPrep& a_out) const
	{
		// AddGeometry's mirror reads, keys only. Sun and light entry reads are counted at the merge (a refused reference reads neither).
		a_out = GeometryPrep{};
		a_out.geometry = a_geometry;
		a_out.reason = a_reason;
		const auto* record = mirror.Node(a_geometry);
		a_out.mirrored = record != nullptr;
		a_out.actorOwner = record && record->IsActorCharacter() ?
		                       static_cast<const RE::TESObjectREFR*>(record->userData) :
		                       nullptr;
		a_out.roomNode = RoomNodeOf(a_geometry);
		// A face shape's head: a dynamic shape under a BSFaceGenNiNode.
		const auto* g = mirror.Geometry(a_geometry);
		const auto* parent = record && record->parent ? mirror.Node(record->parent) : nullptr;
		const bool face = parent && (parent->kind & SceneCapture::kKindFaceGen) && g && g->type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kDynamicTriShape);
		a_out.faceHead = face ? static_cast<const RE::BSFaceGenNiNode*>(record->parent) : nullptr;
		a_out.subIndex = g && g->type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kSubIndexTriShape);
		// While an observer checks these reads against the live chain, the merge resolves both.
		if (Diagnostics::Armed<Diagnostics::SunEntryRead>())
			return;
		a_out.resolved = true;
		a_out.sunEntry = MirrorSunEntry(a_geometry);
		// LightEntryOf's mirror walk.
		if (a_category) {
			const void* root = a_geometry;
			for (const void* node = mirror.Parent(a_geometry); node && node != a_category; node = mirror.Parent(node))
				root = node;
			const auto* rootRecord = root != static_cast<const void*>(a_geometry) ? mirror.Node(root) : nullptr;
			if (rootRecord && rootRecord->parent == a_category && rootRecord->IsActorCharacter())
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
		for (const auto& geometry : a_prep.geometries)
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
		auto prepare = [&](std::vector<SubtreePrep>& a_into, std::vector<MirrorReadStats>& a_counters, bool a_pool) {
			auto body = [&](std::size_t a_begin, std::size_t a_end) {
				ShardScope scope(*this, a_pool, &a_counters[a_begin / kSubtreeGrain]);
				for (std::size_t i = a_begin; i < a_end; ++i)
					PrepareSubtree(a_roots[i].first, a_roots[i].second, a_into[i]);
			};
			if (a_pool) {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.Subtrees", count, kSubtreeGrain, body);
			} else {
				for (std::size_t begin = 0; begin < count; begin += kSubtreeGrain)
					body(begin, std::min(count, begin + kSubtreeGrain));
			}
		};
		std::vector<MirrorReadStats> counters(chunks);
		{
			ZoneScopedN("CS.DCLF.Scene.SubtreesFanout");
			const auto start = std::chrono::steady_clock::now();
			prepare(a_out, counters, parallel);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			AddScenePartMs(ScenePart::SubtreesFanout, ms);
			f.subtreePrepareMs += ms;
		}
		for (auto& chunk : counters)
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

	void Coordinator::NoteFirstPersonHidden(const Tracked& a_tracked, const void* a_key, const SceneCapture::NodeRecord& a_record, std::uint32_t a_depth) const
	{
		static std::atomic<std::uint32_t> logged{ 0 };
		if (logged.fetch_add(1, std::memory_order_relaxed) >= 16)
			return;
		const auto& cullHidden = FrameGlobals::Current().cullHidden;
		const auto* object = static_cast<const RE::NiAVObject*>(a_key);
		const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_object) { return a_entry.first < a_object; });
		const bool listed = it != cullHidden.end() && it->first == object;
		std::string chain;
		const void* at = a_tracked.geometry.get();
		for (std::uint32_t n = 0; at && n < 16; ++n) {
			const auto* node = mirror.Node(at);
			chain += fmt::format("{}{}", n ? " <- " : "", at);
			at = node ? node->parent : nullptr;
			if (!node)
				chain += " (no record)";
		}
		logger::info("[DCLF] first-person hidden verdict (T7e): geometry {} filed under {} (the first-person root {}{}); hidden at chain depth {}: node {}{}, {} (the mirror's bit {}); "
					 "{} nodes listed in the frame's capture; the mirror's chain: {}",
			static_cast<const void*>(a_tracked.geometry.get()), static_cast<const void*>(a_tracked.categoryNode), static_cast<const void*>(firstPersonCategory),
			FirstPersonEntry(a_tracked.categoryNode) ? ", the same" : ", another", a_depth, a_key, a_key == a_tracked.categoryNode ? " (its category node)" : "",
			listed ? fmt::format("listed hidden {} in the frame's capture", it->second) : std::string("not in the frame's capture"),
			(a_record.flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)) != 0, cullHidden.size(), chain);
	}

	std::int32_t Coordinator::BillboardModeOf(const Tracked& a_tracked) const
	{
		if (!ActiveToggles().billboards || a_tracked.parentReason != Ineligible::Billboard)
			return -1;
		// First-person entries too (magic hand glows): the first-person forward part's b12 block (ExecuteFirstPersonDepth) is world
		// space throughout (c0-c2 the camera rotation, c40 the eye at GetFirstPersonNodePosition), sampled before the shift.
		const auto leaf = mirror.Leaf(a_tracked.geometry.get());
		if (!leaf || !SceneCapture::LeafView::Effect(leaf.property) || leaf.Type() != static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape) || leaf.Skinned())
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

	void Coordinator::FirstPersonDecalForward(const Tracked& a_tracked, LightingDescriptors& a_descriptors) const
	{
		static const bool on = SwitchOnUnlessZero(Switch::FirstPersonDecals);
		// Decal groups 1 and 2 (hints 2 and 3); group 3 is a multi-index layer's, never first person's.
		const std::uint32_t group = a_descriptors.decalGroup;
		if (!on || (group != 1 && group != 2) || !FirstPersonEntry(a_tracked.categoryNode))
			return;
		// The forward pixel stage: drop Deferred, as LightingShaderDescriptors does for a forward key.
		a_descriptors.pixel &= a_descriptors.effect ? ~EffectFlag::Deferred : ~kLightingPixelDeferred;
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
		const bool underSwitch = a_tracked.parentReason == Ineligible::Switch;
		if (underSwitch && !ActiveToggles().switchNodes)
			return Ineligible::Switch;
		// An effect leaf under one billboard node is DCLF's, turned by its vertex stage (Billboards.h). Billboard outranks switch and
		// ordered in parentReason, so the chain walk below applies their tests too.
		const bool underBillboard = a_tracked.parentReason == Ineligible::Billboard;
		// A particle system under billboards needs no vertex-stage turn: 0x140d3e650 runs each child's UpdateDownwardPass (slot 0x2C,
		// NiPSysUpdateCtlr), after which ParticleSnapshots snapshots it, and NiParticleSystem::UpdateWorldData (vtable 0x1419c3898 slot
		// 0x30, 0x140dd4c90) is empty, so the engine draws the last update's world and particles, which the snapshot copies.
		const auto particlesUnderBillboard = [&] {
			static const bool on = SwitchOnUnlessZero(Switch::BillboardParticles);
			if (!on || !ActiveToggles().billboards)
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
		const auto orderedLeaf = [&] { return !ActiveToggles().forward && Eligibility::OrderedLeaf(mirror.Leaf(a_tracked.geometry.get())); };
		if (ordered && orderedLeaf())
			return Ineligible::UnsupportedParent;

		// Hidden anywhere up to the category node; part of an actor; under a switch not drawing this branch.
		const bool actors = ActiveToggles().actors;
		const void* child = nullptr;
		const void* key = a_tracked.geometry.get();
		std::uint32_t depth = 0;
		for (const auto* record = mirror.Node(key); record; child = key, key = record->parent, record = key ? mirror.Node(key) : nullptr, ++depth) {
			// The first-person root's kHidden belongs to the view (Main::Draw, unk_FPCulling and RenderDepth toggle it within the frame,
			// FUN_140736150 at the camera switch): the chain stops before it. Whether the view draws is the first-person latch's.
			if (key && key == firstPersonCategory) {
				if (a_tracked.categoryNode != firstPersonCategory)
					Diagnostics::Notify(Diagnostics::FirstPersonMisfiled{ a_tracked.geometry.get(), a_tracked.categoryNode });
				break;
			}
			if (HiddenForWalk(key, *record)) {
				// Diagnostic (pointers only, never dereferenced).
				if (firstPersonCategory) {
					const auto& skeleton = FrameGlobals::Current().firstPersonGeometry;
					if (FirstPersonEntry(a_tracked.categoryNode) ||
						std::binary_search(skeleton.begin(), skeleton.end(), static_cast<const RE::NiAVObject*>(static_cast<const void*>(a_tracked.geometry.get()))))
						NoteFirstPersonHidden(a_tracked, key, *record, depth);
				}
				return Ineligible::Hidden;
			}
			if (key == a_tracked.categoryNode)
				break;
			if (underBillboard && (record->kind & SceneCapture::kKindSwitch) && !ActiveToggles().switchNodes)
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
