// CS_DCLF_PERSISTENT_PARITY: tree LOD's mirror against the LOD trees root's shapes on parity frames: a shape it lacks, a shape it holds
// that the root does not, and records that differ from the group's instances packed again by the refill's rule (a group whose
// shaderPropertyUpToDate is clear is skipped: its refill is due). The refills run on the terrain manager's threads, so a group can be
// refilled between a drain and the check: a shape that differs counts as late when its Add arrives at the next drain, and as differing
// only when it does not.

#include "SceneObservers.h"
#include "SceneState.h"

#include "TreeLodEvents.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void CheckParity(const TreeLod::Mirror& a_mirror)
		{
			auto& stats = SceneDiagnostics().treeLod;
			auto& suspects = stats.suspects;
			const auto& groups = V::treeLodGroups(a_mirror);
			++stats.checks;
			auto* root = TreeLod::LodTreesRoot();
			ankerl::unordered_dense::set<const void*> live;
			std::vector<TreeLod::Instance> packed;
			auto note = [&](std::string&& a_text) {
				if (stats.first.empty())
					stats.first = std::move(a_text);
			};
			if (root) {
				for (const auto& child : root->GetChildren()) {
					auto* shape = child ? child->AsGeometry() : nullptr;
					if (!TreeLod::IsTreeLodShape(shape))
						continue;
					live.insert(shape);
					++stats.shapes;
					const auto it = groups.find(shape);
					if (it == groups.end()) {
						++stats.missing;
						note(fmt::format("'{}' under the root, not mirrored", shape->name.c_str() ? shape->name.c_str() : ""));
						continue;
					}
					const auto& block = *static_cast<const RE::BGSDistantTreeBlock*>(it->second.block);
					const auto& group = *static_cast<const RE::BGSDistantTreeBlock::TreeGroup*>(it->second.group);
					if (!group.shaderPropertyUpToDate) {
						++stats.unsettled;
						continue;
					}
					TreeLod::PackGroup(block.allVisible, group, packed);
					if (packed == it->second.instances)
						continue;
					std::size_t at = 0;
					while (at < packed.size() && at < it->second.instances.size() && packed[at] == it->second.instances[at])
						++at;
					std::string fields;
					if (at < packed.size() && at < it->second.instances.size())
						for (std::size_t f = 0; f < TreeLod::kInstanceShorts; ++f)
							if (packed[at].shorts[f] != it->second.instances[at].shorts[f])
								fields += fmt::format(" [{}] {:#06x} mirrored, {:#06x} packed", f, it->second.instances[at].shorts[f], packed[at].shorts[f]);
					suspects[shape] = fmt::format("{} records mirrored, {} packed now; record {}:{}", it->second.instances.size(), packed.size(), at, fields);
				}
			}
			// A mirrored shape no longer under the root: its removal was missed.
			for (const auto& [shape, group] : groups)
				if (!live.contains(shape)) {
					++stats.stale;
					note(fmt::format("a mirrored shape ({} records) is not under the root", group.instances.size()));
				}
		}

		void OnEventDrained(const TreeLodEventDrained& a_event)
		{
			auto& stats = SceneDiagnostics().treeLod;
			if (!stats.suspects.empty() && stats.suspects.erase(a_event.shape))
				++stats.late;
		}

		void OnDrained(const TreeLodDrained& a_event)
		{
			// Suspects with no event since the check: the mirror missed a refill.
			auto& stats = SceneDiagnostics().treeLod;
			for (auto& [shape, text] : stats.suspects) {
				++stats.differ;
				if (stats.first.empty())
					stats.first = std::move(text);
			}
			stats.suspects.clear();
			if (ParityDue(a_event.frame) && std::exchange(stats.frame, a_event.frame) != a_event.frame)
				CheckParity(a_event.mirror);
		}
	}

	void InstallTreeLodParity()
	{
		Subscribe<TreeLodEventDrained>(OnEventDrained);
		Subscribe<TreeLodDrained>(OnDrained);
	}
}
