// CS_DCLF_TREE_LOD_AUDIT: the open defect's probe (dclf-open-defects.md, "Tree LOD instances the engine leaves hidden after allVisible"),
// main thread at the scene frame's start, with or without DCLF: the attached tree LOD blocks' instances against their references (TreeLod.h).

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::TreeLod
{
	namespace
	{
		Api::SwitchId auditSwitch;  // install

		// Reference form ID -> the frame it was first seen stuck (AuditTreeLod's; main thread).
		ankerl::unordered_dense::map<RE::FormID, std::uint32_t> stuckSince;

		/** @brief One audit's counts, the references stuck in it and its first samples. */
		struct Audit
		{
			std::uint32_t frame = 0;
			std::uint32_t exact = 0, overlapped = 0, blocks = 0, mismatched = 0, loading = 0, instances = 0, hidden = 0, noRef = 0, loaded = 0, stuck = 0, holes = 0;
			ankerl::unordered_dense::set<RE::FormID> stuckNow;
			std::string samples;
		};

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
			if (!(fullFade > 0.0f))
				return;  // the full tree is faded out: the LOD stands in for it
			++a_audit.overlapped;
			a_audit.stuckNow.insert(ref->GetFormID());
			const auto [it, first] = stuckSince.try_emplace(ref->GetFormID(), a_audit.frame);
			if (a_audit.overlapped <= 8) {
				const auto* fade = full->AsFadeNode();
				auto* cell = ref->GetParentCell();
				const auto* coords = cell && cell->IsExteriorCell() ? cell->GetCoordinates() : nullptr;
				a_audit.samples += fmt::format("; {:08X} '{}' cell ({}, {}) block node ({}, {}) level {} alpha {} fade {} allVisible {} upToDate {} stuck {} frames",
					ref->GetFormID(), ref->GetBaseObject() ? ref->GetBaseObject()->GetName() : "?", coords ? coords->cellX : 0, coords ? coords->cellY : 0,
					a_node.baseCellX, a_node.baseCellY, a_node.GetLODLevel(), a_instance.alpha,
					fade ? *reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(fade) + 0x130) : -1.0f, a_block.allVisible,
					a_group.shaderPropertyUpToDate, a_audit.frame - it->second);
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
			if (audit.overlapped == reported && audit.overlapped == 0)
				return;
			reported = audit.overlapped;
			logger::info("[DCLF] tree LOD audit: {} blocks attached ({} loading, {} not their node's), {} instances ({} exact), {} hidden; {} without a loaded reference, {} with a visible full tree; {} shown over it ({} with its fade above 0), {} hidden over a hidden full tree{}",
				audit.blocks, audit.loading, audit.mismatched, audit.instances, audit.exact, audit.hidden, audit.noRef, audit.loaded, audit.stuck, audit.overlapped, audit.holes,
				audit.samples);
		}
	}

	void SubscribeAudit(Api::SwitchId a_switch)
	{
		auditSwitch = a_switch;
		Diagnostics::Subscribe<Diagnostics::FrameBegin>([](const Diagnostics::FrameBegin&) { AuditTreeLod(); });
	}
}
