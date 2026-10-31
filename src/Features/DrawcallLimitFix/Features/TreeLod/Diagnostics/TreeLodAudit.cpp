// CS_DCLF_TREE_LOD_AUDIT: the open defect's probe (dclf-open-defects.md, "Tree LOD instances the engine leaves hidden after allVisible"),
// main thread at the scene frame's start, with or without DCLF: the attached tree LOD blocks' instances against their references (TreeLodMirror.h).

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::TreeLod
{
	namespace
	{
		Api::SwitchId auditSwitch;  // install

		// Reference form ID -> the frame it was first seen stuck (AuditTreeLod's; main thread); and first seen faded out in view.
		ankerl::unordered_dense::map<RE::FormID, std::uint32_t> stuckSince, fadedOutSince;

		/** @brief Whether a_object's world bound is roughly in the world camera's view (a cone of 60 degrees half angle, its radius added). */
		bool InView(const RE::NiAVObject& a_object)
		{
			const auto* camera = RE::Main::WorldRootCamera();
			if (!camera)
				return true;
			const auto& eye = camera->world.translate;
			const auto& r = camera->world.rotate;
			const RE::NiPoint3 forward{ r.entry[0][0], r.entry[1][0], r.entry[2][0] };  // NiCamera looks down its +X
			const RE::NiPoint3 to = a_object.worldBound.center - eye;
			const float distance = to.Length();
			const float radius = a_object.worldBound.radius;
			if (distance <= radius)
				return true;
			const float along = to.Dot(forward);
			return along + radius > 0.0f && along >= distance * 0.5f - radius;
		}

		/** @brief One audit's counts, the references stuck in it and its first samples. */
		struct Audit
		{
			std::uint32_t frame = 0;
			std::uint32_t fadedOut = 0, fadedOutInView = 0, fadedOutInViewUnlisted = 0, fadedOutInViewLong = 0;
			std::string fadedOutSamples;
			ankerl::unordered_dense::set<RE::FormID> fadedOutNow;
			std::uint32_t exact = 0, overlapped = 0, blocks = 0, mismatched = 0, loading = 0, instances = 0, hidden = 0, noRef = 0, loaded = 0, stuck = 0, holes = 0;
			ankerl::unordered_dense::set<RE::FormID> stuckNow;
			std::string samples;
		};

		/** @brief DCLF's fade root row for a_node in the accepted tables (the frame's thread), or that none lists it. */
		std::string RootOf(const RE::NiAVObject* a_node)
		{
			const auto& tables = Scene::FrameView::Get().AcceptedTables();
			if (!tables)
				return "no tables";
			const auto list = Extensions::RootListOf(*tables);
			for (std::size_t r = 0; r < list.nodes.size() && r < list.rows.size(); ++r)
				if (list.nodes[r] == a_node) {
					const auto bits = list.rows[r].bits;
					return fmt::format("root {} gen {} plan {} written {} stood in {} owned {} tree LOD {} initial fade {}", r, list.rows[r].generation, bits & kFadeRootPlanMask,
						(bits & kFadeRootWritten) != 0, (bits & kFadeRootStoodIn) != 0, (bits & kFadeRootOwned) != 0, (bits & kFadeRootTreeLod) != 0,
						list.rows[r].initial.currentFade);
				}
			return "not a listed root";
		}

		/** @brief A block's instance against its reference: counted, and an instance shown over its visible full tree sampled. */
		void AuditInstance(Audit& a_audit, const RE::BGSTerrainNode& a_node, const RE::BGSDistantTreeBlock& a_block, const RE::BGSDistantTreeBlock::TreeGroup& a_group,
			const RE::BGSDistantTreeBlock::InstanceData& a_instance)
		{
			++a_audit.instances;
			// AE's byte, not a bool: bit 0 hidden, bit 1 the instance's form ID is exact (no scan of the files).
			const std::uint8_t flags = std::bit_cast<std::uint8_t>(a_instance.hidden);
			const bool instanceHidden = (flags & 1) != 0;
			a_audit.exact += (flags & 2) ? 1 : 0;
			a_audit.hidden += instanceHidden ? 1 : 0;
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>((flags & 2) ? a_instance.id : a_instance.id & 0x00FFFFFF);
			if (!ref) {
				++a_audit.noRef;
				return;
			}
			auto* full = ref->Get3D();
			const bool visible = full && !full->GetFlags().any(RE::NiAVObject::Flag::kHidden) && !ref->IsDisabled();
			a_audit.loaded += visible ? 1 : 0;
			if (instanceHidden && !visible && full)
				++a_audit.holes;
			if (instanceHidden || !visible)
				return;
			++a_audit.stuck;
			const auto* fadeNode = full->AsFadeNode();
			const float fullFade = fadeNode ? *reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(fadeNode) + 0x130) : 1.0f;
			if (!(fullFade > 0.0f) || fullFade < 0.05f) {
				// The full tree is faded out (or stuck at a first step) on its node: the LOD stands in for it, unless DCLF draws the tree from
				// its own fade state. Only one in view can show both; how long it has been so.
				++a_audit.fadedOut;
				if (!InView(*full))
					return;
				++a_audit.fadedOutInView;
				a_audit.fadedOutNow.insert(ref->GetFormID());
				const auto [since, first] = fadedOutSince.try_emplace(ref->GetFormID(), a_audit.frame);
				const std::uint32_t frames = a_audit.frame - since->second;
				a_audit.fadedOutInViewLong += frames >= 120 ? 1 : 0;
				const std::string root = RootOf(full);
				a_audit.fadedOutInViewUnlisted += root == "not a listed root" ? 1 : 0;
				if (frames >= 120 && a_audit.fadedOutInViewLong <= 6)
					a_audit.fadedOutSamples += fmt::format("; {:08X} '{}' fade {} for {} frames, {:.0f} units away ({})", ref->GetFormID(),
						ref->GetBaseObject() ? ref->GetBaseObject()->GetName() : "?", fullFade, frames,
						RE::Main::WorldRootCamera() ? full->worldBound.center.GetDistance(RE::Main::WorldRootCamera()->world.translate) : 0.0f, root);
				return;
			}
			++a_audit.overlapped;
			a_audit.stuckNow.insert(ref->GetFormID());
			const auto [it, first] = stuckSince.try_emplace(ref->GetFormID(), a_audit.frame);
			if (a_audit.overlapped <= 8) {
				const auto* fade = full->AsFadeNode();
				auto* cell = ref->GetParentCell();
				const auto* coords = cell && cell->IsExteriorCell() ? cell->GetCoordinates() : nullptr;
				a_audit.samples += fmt::format("; {:08X} '{}' cell ({}, {}) block node ({}, {}) level {} alpha {} fade {} allVisible {} upToDate {} stuck {} frames ({})",
					ref->GetFormID(), ref->GetBaseObject() ? ref->GetBaseObject()->GetName() : "?", coords ? coords->cellX : 0, coords ? coords->cellY : 0,
					a_node.baseCellX, a_node.baseCellY, a_node.GetLODLevel(), a_instance.alpha,
					fade ? *reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(fade) + 0x130) : -1.0f, a_block.allVisible,
					a_group.shaderPropertyUpToDate, a_audit.frame - it->second, RootOf(full));
			}
		}

		/** @brief The terrain manager's quadtree from a_root: each node's attached tree block's instances (AuditInstance). */
		void AuditBlocks(Audit& a_audit, const RE::BGSTerrainNode* a_root)
		{
			std::vector<const RE::BGSTerrainNode*> stack{ a_root };
			while (!stack.empty()) {
				const auto* node = stack.back();
				stack.pop_back();
				// The four children are stored together, the field pointing at the first (CommonLib types it as an array of pointers).
				if (const auto* children = reinterpret_cast<const RE::BGSTerrainNode*>(node->children))
					for (std::uint32_t c = 0; c < 4; ++c)
						stack.push_back(children + c);
				const auto* block = node->trees ? node->trees->block : nullptr;
				if (!block)
					continue;
				if (block->node != node) {
					++a_audit.mismatched;  // the layer's layout is not CommonLib's
					continue;
				}
				if (!block->doneLoading || !block->attached) {
					++a_audit.loading;
					continue;
				}
				++a_audit.blocks;
				for (const auto* group : block->treeGroups) {
					if (!group)
						continue;
					for (const auto& instance : group->instances)
						AuditInstance(a_audit, *node, *block, *group, instance);
				}
			}
		}

		/** @brief The LOD trees root's tree LOD shapes: how many, with their instances, are hidden by their own flag or an ancestor's. */
		std::string AuditShapesHidden()
		{
			auto* root = LodTreesRoot();
			if (!root)
				return "no LOD trees root";
			std::uint32_t shapes = 0, hidden = 0, hiddenBySelf = 0, hiddenInstances = 0, shownInstances = 0;
			float minDistance = 1e30f, maxDistance = 0.0f;
			std::vector<std::pair<RE::NiAVObject*, bool>> stack{ { root, root->GetFlags().any(RE::NiAVObject::Flag::kHidden) } };
			while (!stack.empty()) {
				const auto [object, parentHidden] = stack.back();
				stack.pop_back();
				const bool self = object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
				if (auto* node = object->AsNode()) {
					for (const auto& child : node->GetChildren())
						if (child)
							stack.push_back({ child.get(), parentHidden || self });
					continue;
				}
				auto* geometry = object->AsGeometry();
				if (!geometry || !TreeInstances::IsEngineShape(geometry))
					continue;
				++shapes;
				auto* group = const_cast<RE::BSMultiStreamInstanceTriShape*>(static_cast<const RE::BSMultiStreamInstanceTriShape*>(geometry));
				const auto& data = group->GetMultiStreamTrishapeRuntimeData();
				const std::uint32_t instances = data.instanceCount;
				minDistance = std::min(minDistance, data.renderDistance);
				maxDistance = std::max(maxDistance, data.renderDistance);
				if (self || parentHidden) {
					++hidden;
					hiddenBySelf += self ? 1 : 0;
					hiddenInstances += instances;
				} else
					shownInstances += instances;
			}
			return fmt::format("{} shapes under the LOD trees root, {} hidden ({} by their own flag, the rest by an ancestor's) with {} instances; {} instances in shown shapes; render distance {} - {}",
				shapes, hidden, hiddenBySelf, hiddenInstances, shownInstances, minDistance, maxDistance);
		}

		void AuditTreeLod()
		{
			static std::uint32_t frame = 0;
			// The stuck set reported last.
			static std::size_t reported = ~std::size_t{};
			if (!SwitchEnabled(auditSwitch) || (++frame % 60) != 0)
				return;
			auto* tes = RE::TES::GetSingleton();
			const auto* world = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
			const auto* manager = world ? world->GetTerrainManager() : nullptr;
			if (!manager || !manager->rootNode)
				return;
			Audit audit{ .frame = frame };
			AuditBlocks(audit, manager->rootNode);
			std::erase_if(stuckSince, [&](const auto& a_entry) { return !audit.stuckNow.contains(a_entry.first); });
			std::erase_if(fadedOutSince, [&](const auto& a_entry) { return !audit.fadedOutNow.contains(a_entry.first); });
			if (audit.overlapped == reported && audit.overlapped == 0)
				return;
			reported = audit.overlapped;
			logger::info("[DCLF] tree LOD audit: {} blocks attached ({} loading, {} not their node's), {} instances ({} exact), {} hidden; {} without a loaded reference, {} with a visible full tree; {} shown over it ({} with its fade above 0), {} hidden over a hidden full tree{}",
				audit.blocks, audit.loading, audit.mismatched, audit.instances, audit.exact, audit.hidden, audit.noRef, audit.loaded, audit.stuck, audit.overlapped, audit.holes,
				audit.samples);
			logger::info("[DCLF] tree LOD audit: {}", AuditShapesHidden());
			if (audit.fadedOut)
				logger::info("[DCLF] tree LOD audit: {} shown over a full tree faded out on its node (fade below 0.05); in view {} ({} not a listed root), {} of them for 120+ frames{}",
					audit.fadedOut, audit.fadedOutInView, audit.fadedOutInViewUnlisted, audit.fadedOutInViewLong, audit.fadedOutSamples);
		}
	}

	void SubscribeAudit(Api::SwitchId a_switch)
	{
		auditSwitch = a_switch;
		Diagnostics::Subscribe<Diagnostics::FrameBegin>([](const Diagnostics::FrameBegin&) { AuditTreeLod(); });
	}
}
