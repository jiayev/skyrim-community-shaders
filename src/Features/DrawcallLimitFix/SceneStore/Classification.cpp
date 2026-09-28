#include "Internal.h"

namespace DCLF
{
	std::uint32_t SceneStore::LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property)
	{
		if (!a_geometry.GetFlags().any(RE::NiAVObject::Flag::kMeshLOD) || !a_property || !a_property->fadeNode)
			return 3;
		return a_property->fadeNode->GetRuntimeData().unk152 & 0xF;
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

	RE::NiNode* SceneStore::FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason) const
	{
		Ineligible reason = Ineligible::None;
		RE::NiNode* node = a_object ? a_object->parent : nullptr;
		for (std::uint32_t depth = 0; node && depth < kMaxParentDepth; ++depth, node = node->parent) {
			if (categoryNodes.contains(node)) {
				if (a_parentReason)
					*a_parentReason = reason;
				return node;
			}
			reason = CombineParentReasons(reason, ParentReason(node));
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
		std::uint64_t hash = 0xcbf29ce484222325ull;
		auto mix = [&hash](auto a_value) {
			hash = (hash ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_value))) * 0x100000001b3ull;
		};
		auto mixCell = [&](RE::TESObjectCELL* a_cell) {
			mix(a_cell);
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			mix(loaded);
			RE::NiNode* cell3D = loaded ? loaded->cell3D.get() : nullptr;
			mix(cell3D);
			if (cell3D)
				hash = (hash ^ cell3D->GetChildren().size()) * 0x100000001b3ull;
		};
		if (auto* tes = RE::TES::GetSingleton()) {
			if (auto* objRoot = tes->objRoot)
				hash = (hash ^ objRoot->GetChildren().size()) * 0x100000001b3ull;
			mix(tes->objRoot);
			mix(tes->interiorCell);
			if (tes->interiorCell) {
				mixCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					mixCell(grid->cells[i]);
			}
		}
		return hash;
	}

	void SceneStore::RefreshCategoryNodes(bool a_force)
	{
		// This used to rebuild and diff the whole set on every Present, including an O(tracked) scan
		// whenever any node had gone. Its *content* changes only when a cell attaches or detaches, so it
		// now runs when the signature says something moved, when a detach was seen, or on a slow backstop
		// cadence in case both miss something.
		constexpr std::uint32_t kBackstopFrames = 30;
		const std::uint64_t signature = CategorySignature();
		if (!a_force && signature == categorySignature && ++categoryIdleFrames < kBackstopFrames)
			return;
		const std::uint8_t cause = a_force ? 1 : signature != categorySignature ? 0 : 2;
		categorySignature = signature;
		categoryIdleFrames = 0;

		ankerl::unordered_dense::set<RE::NiNode*> current;

		auto addCell = [&](RE::TESObjectCELL* a_cell) {
			if (!a_cell || !a_cell->IsAttached())
				return;
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			if (!loaded || !loaded->cell3D)
				return;
			auto& children = loaded->cell3D->GetChildren();
			for (auto category : kDrawnCategories) {
				const auto index = static_cast<std::uint16_t>(category);
				if (index < children.size() && children[index]) {
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
				if (auto* shared = graph->portalSharedNode.get())
					current.insert(shared);
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
			if (tes->interiorCell) {
				addCell(tes->interiorCell);
			} else if (auto* grid = tes->gridCells) {
				const std::uint32_t count = grid->length * grid->length;
				for (std::uint32_t i = 0; i < count; ++i)
					addCell(grid->cells[i]);
			}
		}

		// Cells that went away: drop what was tracked under them.
		bool removedAny = false;
		for (auto* node : categoryNodes) {
			if (!current.contains(node)) {
				removedAny = true;
				break;
			}
		}
		if (removedAny) {
			std::vector<RE::BSGeometry*> stale;
			for (auto& [geometry, entry] : tracked) {
				if (!current.contains(entry.categoryNode))
					stale.push_back(geometry);
			}
			for (auto* geometry : stale)
				EraseTracked(geometry);
		}

		// Cells that appeared: their content was attached before the cell was, so scan it now.
		std::vector<RE::NiNode*> added;
		for (auto* node : current) {
			if (!categoryNodes.contains(node))
				added.push_back(node);
		}
		categoryNodes = std::move(current);
		std::erase_if(categoryFound, [&](const auto& a_entry) { return !categoryNodes.contains(const_cast<RE::NiNode*>(a_entry.first)); });
		const TrackSource previousSource = addSource;
		if (addSource != TrackSource::Rescan)
			addSource = TrackSource::CategoryAppeared;
		for (auto* node : added) {
			categoryFound[node] = { frame, cause };
			for (auto& child : node->GetChildren()) {
				if (child)
					AddSubtree(child.get());
			}
		}
		addSource = previousSource;
	}

	std::array<float, 4> SceneStore::SunEntryOf(Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		constexpr std::array<float, 4> kNeverTested{ 0.0f, 0.0f, 0.0f, -1.0f };
		ResolveSunEntry(a_tracked, a_geometry);
		if (!a_tracked.sunEntryNode)
			return kNeverTested;
		const auto& bound = a_tracked.sunEntryNode->worldBound;
		return { bound.center.x, bound.center.y, bound.center.z, bound.radius };
	}

	void SceneStore::ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		if (!a_tracked.sunEntryResolved) {
			a_tracked.sunEntryResolved = true;
			auto* geometry = const_cast<RE::BSGeometry*>(&a_geometry);
			if (auto* reference = geometry->GetUserData()) {
				// An actor's entry is its cell's container, which the full-frustum cull never tests.
				if (reference->GetFormType() == RE::FormType::ActorCharacter) {
					a_tracked.sunEntryNode = nullptr;
				} else {
					const RE::NiAVObject* root = geometry;
					for (auto* node = geometry->parent; node && node->GetUserData() == reference; node = node->parent)
						root = node;
					a_tracked.sunEntryNode = root;
				}
			} else {
				static const REL::Relocation<const RE::NiRTTI*> multiBound{ RE::BSMultiBoundNode::Ni_RTTI };
				for (auto* node = geometry->parent; node; node = node->parent)
					if (node->GetRTTI() == multiBound.get()) {
						a_tracked.sunEntryNode = node;
						break;
					}
			}
		}
	}

	void SceneStore::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason)
	{
		const auto [it, inserted] = tracked.try_emplace(a_geometry);
		auto& entry = it->second;
		const auto* owner = a_geometry->GetUserData();
		const auto* actorOwner = owner && owner->GetFormType() == RE::FormType::ActorCharacter ? owner : nullptr;
		const auto identity = sceneIdentity.Attach(a_geometry, actorOwner);
		if (identity.replaced) {
			ReleaseObjectSlot(entry);
			entry.actorOwnedResolved = false;
			entry.faceShapeResolved = false;
		}
		if (inserted) {
			entry.trackedFrame = frame;
			entry.trackedBy = addSource;
		}
		entry.identity = identity.member;
		entry.groupIdentity = identity.group;
		entry.actorOwner = actorOwner;
		entry.roomNode = globals::features::lightLimitFix.GetRoomNode(a_geometry);
		entry.roomMapGeneration = 0;
		entry.geometry.reset(a_geometry);
		entry.categoryNode = a_categoryNode;
		entry.parentReason = a_parentReason;
		// Attached (again): its classification stands no more, and every other entry reading the same sun entry node is
		// evaluated again, since that node's bound takes this one in now (dclf-event-driven-tables.md, "Phase 3").
		entry.candidateFrame = 0;
		{
			UnlistDependents(a_geometry, entry, true);
			entry.sunEntryResolved = false;
			entry.sunEntryNode = nullptr;
			ResolveSunEntry(entry, *a_geometry);
			if (entry.sunEntryNode) {
				rootDependents[entry.sunEntryNode].push_back(a_geometry);
				entry.listedRoot = entry.sunEntryNode;
				dirtyRoots.push_back(entry.sunEntryNode);
				MarkSunEntryDirty(entry.sunEntryNode);
			}
		}
		pendingEvaluation.push_back(a_geometry);
	}

	void SceneStore::AddSubtree(RE::NiAVObject* a_root)
	{
		// The attach event is drained a frame or more after it was queued, so its node can already be gone
		// (a cell transition releases the subtree). Walking it then dereferences null.
		if (!a_root)
			return;
		Ineligible reasonAbove = Ineligible::None;
		RE::NiNode* category = FindCategoryNode(a_root, &reasonAbove);
		if (!category)
			return;

		// Walk down, carrying what the nodes between the category node and the leaf make of it (ParentReason).
		std::vector<std::pair<RE::NiAVObject*, Ineligible>> stack;
		stack.emplace_back(a_root, reasonAbove);
		while (!stack.empty()) {
			auto [object, reason] = stack.back();
			stack.pop_back();
			if (!object)
				continue;
			if (auto* geometry = object->AsGeometry()) {
				AddGeometry(geometry, category, reason);
				continue;
			}
			if (auto* node = object->AsNode()) {
				// Its selected child as the cull would find it (with the switch events, nothing else does it before the
				// walk classifies it).
				if (SwitchEventsLive())
					if (auto* switchNode = node->AsSwitchNode(); switchNode && CatchUpSwitch(*switchNode))
						++delta.attachCatchUps;
				const Ineligible below = CombineParentReasons(reason, ParentReason(node));
				for (auto& child : node->GetChildren()) {
					if (child)
						stack.emplace_back(child.get(), below);
				}
			}
		}
	}

	void SceneStore::FindLightingShader()
	{
		// Any lighting render pass carries the BSLightingShader instance.
		for (auto& [geometry, entry] : tracked) {
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property)
				continue;
			for (auto* pass = property->renderPassList.head; pass; pass = pass->next) {
				if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					ConstantEvaluator::Get().SetLightingShader(pass->shader);
					return;
				}
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

	Ineligible SceneStore::ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated,
		bool a_wantDerived, RE::BSLightingShaderProperty** a_castCache)
	{
		// An NPC face shape (a dynamic shape under a BSFaceGenNiNode) takes the checks below like any shape. Its
		// positions are not in its buffers but in FaceSnapshots: the walk gives every record of one its stream
		// (SceneStore::Tracked::faceShape), whatever the verdict, and the draws bind it as the second stream.
		const auto type = a_geometry.GetType().get();
		const bool face = type == RE::BSGeometry::Type::kDynamicTriShape && FaceSnapshots::Enabled() && a_geometry.parent &&
		                  netimmerse_cast<RE::BSFaceGenNiNode*>(a_geometry.parent);
		if (type != RE::BSGeometry::Type::kTriShape) {
			if (!face)
				return Ineligible::NotTriShape;
		}

		auto& data = a_geometry.GetGeometryRuntimeData();
		if (auto* skin = data.skinInstance.get()) {
			if (!ActiveToggles().skinned)
				return Ineligible::Skinned;
			// The skinned path (engine notes: skinning): a NiSkinInstance, or with CS_DCLF_SKIN_PARTITIONS a
			// BSDismemberSkinInstance or several partitions, and a palette the native shader could index (240
			// rows). Each partition the engine draws is one draw of that PARTITION's buffer, not the geometry's
			// rendererData, which for a skinned shape is a different TriShape; which partitions it draws is
			// SkinPartitionMask's, per frame.
			static const REL::Relocation<const RE::NiRTTI*> niSkinInstance{ RE::NiSkinInstance::Ni_RTTI };
			static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
			const bool dismember = skin->GetRTTI() == dismemberSkinInstance.get();
			if (skin->GetRTTI() != niSkinInstance.get() && !dismember)
				return Ineligible::SkinShape;
			auto* partition = skin->skinPartition.get();
			auto* skinData = skin->skinData.get();
			if (!partition || !skinData || partition->numPartitions == 0 || skinData->GetBoneCount() == 0 || skinData->GetBoneCount() * 3 > 240)
				return Ineligible::SkinShape;
			if ((partition->numPartitions > 1 || dismember) && !ActiveToggles().skinPartitions)
				return Ineligible::SkinShape;
			if (partition->numPartitions > kMaxSkinPartitions)
				return Ineligible::SkinShape;
			// The engine indexes the dismember flags by partition; a flag array of another length is not one it
			// could have drawn from either.
			if (dismember) {
				const auto& flags = static_cast<const RE::BSDismemberSkinInstance*>(skin)->GetRuntimeData();
				if (flags.partitions && static_cast<std::uint32_t>(flags.numPartitions) != partition->numPartitions)
					return Ineligible::SkinShape;
			}
			// Every partition is drawn with the first one's pipeline, so they share its vertex layout; a LOD
			// byte past 2 would index the next row of the engine's table.
			const auto& first = partition->partitions[0];
			for (std::uint32_t i = 0; i < partition->numPartitions; ++i) {
				const auto& p = partition->partitions[i];
				if (!p.buffData || !p.buffData->vertexBuffer || !p.buffData->indexBuffer)
					return Ineligible::NoRendererData;
				if (!first.buffData || std::bit_cast<std::uint64_t>(p.buffData->vertexDesc) != std::bit_cast<std::uint64_t>(first.buffData->vertexDesc) ||
					(p.pad42 & 0xFF) > 2)
					return Ineligible::SkinShape;
			}
		} else if (!data.rendererData || !data.rendererData->vertexBuffer || !data.rendererData->indexBuffer) {
			return Ineligible::NoRendererData;
		}

		// The caller may already know what this property is (see Tracked::castProperty); the cast is a walk
		// of dependent RTTI pointer loads, which is the part of this function that neither a memo nor the
		// reads BuildFrame makes later can remove.
		auto* property = a_castCache ? *a_castCache : netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get());
		if (!property)
			return Ineligible::NotLightingShader;

		// GetRenderPasses treats material alpha below one as transparent.
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
		if (material && material->materialAlpha < 1.0f)
			return Ineligible::AlphaBlend;

		LightingDescriptors descriptors;
		const Ineligible reason = DeriveLightingDescriptors(*property, a_geometry, a_accumulated, descriptors, a_wantDerived);
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
		return reason;
	}

	bool SceneStore::ReadSwitch(const RE::NiSwitchNode& a_switch, SwitchState& a_out)
	{
		const auto* base = reinterpret_cast<const std::byte*>(&a_switch);
		a_out.flags = *reinterpret_cast<const std::uint16_t*>(base + 0x128);
		a_out.index = *reinterpret_cast<const std::int32_t*>(base + 0x12C);
		a_out.revID = *reinterpret_cast<const std::uint32_t*>(base + 0x134);
		// childRevID, an NiTPrimitiveArray at +0x138: its data pointer at +0x8, its capacity at +0x10.
		a_out.childRevID = *reinterpret_cast<const std::uint32_t* const*>(base + 0x140);
		a_out.childRevCapacity = *reinterpret_cast<const std::uint16_t*>(base + 0x148);
		return true;
	}

	bool SceneStore::SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child)
	{
		// NiSwitchNode::OnVisible (AE 140d29700) culls children[index] and nothing else. A child that has
		// become the selected one since the last update pass is brought up to date there, in the cull
		// (childRevID[index] != revID), which is after this walk read its transforms: leave it native for
		// that frame.
		// Bounded by capacity, not size: a Gamebryo array is indexed by slot, and size counts the used ones.
		SwitchState state;
		if (!ReadSwitch(a_switch, state))
			return false;
		const auto& children = a_switch.GetChildren();
		if (state.index < 0 || static_cast<std::uint32_t>(state.index) >= children.capacity() || !state.childRevID ||
			static_cast<std::uint32_t>(state.index) >= state.childRevCapacity)
			return false;
		const auto index = static_cast<std::uint16_t>(state.index);
		return children[index].get() == a_child && state.childRevID[index] == state.revID;
	}

	void SceneStore::CaptureCullHiddenBits()
	{
		cullHiddenBits.clear();
		for (auto* sceneNode : RE::BSShaderManager::State::GetSingleton().shadowSceneNode) {
			const auto* graph = sceneNode ? sceneNode->GetRuntimeData().portalGraph : nullptr;
			if (!graph)
				continue;
			for (const auto& child : graph->alwaysRenderChildren)
				if (child)
					cullHiddenBits.emplace_back(child.get(), IsHidden(child.get()));
			if (graph->portalSharedNode)
				cullHiddenBits.emplace_back(graph->portalSharedNode.get(), IsHidden(graph->portalSharedNode.get()));
		}
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			// The third-person skeleton as it is now. TESWaterReflections::Update (AE 0x140520570), on the frames a
			// cube-map reflection updates, hides the player's 3D while it renders the faces and then restores it.
			// Main::Draw calls it between the main cull jobs' Begin and Finish, so it runs alongside the walk.
			const RE::NiAVObject* thirdPerson = player->Get3D(false);
			if (thirdPerson)
				cullHiddenBits.emplace_back(thirdPerson, IsHidden(thirdPerson));
			// The first-person skeleton: Main::Draw (AE 0x1406444b0) hides it right after the call the walk is
			// kicked from, keeps it hidden through the main camera's cull and the sun's shadow casters, and shows it
			// only to draw the first-person view with its own camera. For every view the walk serves it is hidden.
			const RE::NiAVObject* firstPerson = player->Get3D(true);
			if (firstPerson && firstPerson != thirdPerson)
				cullHiddenBits.emplace_back(firstPerson, true);
		}
		std::sort(cullHiddenBits.begin(), cullHiddenBits.end());
	}

	bool SceneStore::HiddenForWalk(const RE::NiAVObject* a_object) const
	{
		const auto it = std::lower_bound(cullHiddenBits.begin(), cullHiddenBits.end(), a_object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHiddenBits.end() && it->first == a_object)
			return it->second;
		return IsHidden(a_object);
	}

	Ineligible SceneStore::ClassifyFrame(const Tracked& a_tracked, const AccumulatedPass* a_accumulated) const
	{
		const bool underSwitch = a_tracked.parentReason == Ineligible::Switch;
		if (underSwitch && !ActiveToggles().switchNodes)
			return Ineligible::Switch;
		if (a_tracked.parentReason != Ineligible::None && !underSwitch)
			return a_tracked.parentReason;

		// App-culled or hidden anywhere between the leaf and its category node; part of an actor; under a
		// switch node that does not draw this branch.
		const bool actors = ActiveToggles().actors;
		const RE::NiAVObject* child = nullptr;
		for (const RE::NiAVObject* object = a_tracked.geometry.get(); object; child = object, object = object->parent) {
			if (HiddenForWalk(object))
				return Ineligible::Hidden;
			if (object == a_tracked.categoryNode)
				break;
			if (underSwitch && child) {
				if (auto* switchNode = const_cast<RE::NiAVObject*>(object)->AsSwitchNode(); switchNode && !SwitchSelects(*switchNode, child))
					return Ineligible::Switch;
			}
			if (!actors)
				if (auto* ref = object->GetUserData(); ref && ref->IsActor())
					return Ineligible::Actor;
		}

		// Fading: as the pass was registered when there is one (the withholding decided on that same value),
		// else as the fade node stands now.
		auto* property = a_tracked.geometry->GetGeometryRuntimeData().shaderProperty.get();
		// Without the pass (the scene phase, for the shadow views) a fade is only the native loop's when fades
		// are not DCLF's at all; the shadow views skip faded casters themselves (ShadowReject::Faded).
		const bool fading = a_accumulated ? a_accumulated->fading :
		                                    !ActiveToggles().fading && property && property->fadeNode && property->fadeNode->GetRuntimeData().currentFade < 1.0f;
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
