#include "CapturedScene.h"

#include <algorithm>
#include <stdexcept>
#include <limits>
#include <utility>

namespace DCLF::Published
{
	std::size_t ConsistencyGroupUpdate::OwnedBytes() const
	{
		std::size_t bytes = sizeof(*this);
		const auto add = [&](std::size_t count, std::size_t size) {
			if (count > (std::numeric_limits<std::size_t>::max() - bytes) / size)
				bytes = std::numeric_limits<std::size_t>::max();
			else bytes += count * size;
		};
		add(members.capacity(), sizeof(MemberInput));
		add(resources.capacity(), sizeof(ResourceLease));
		add(components.capacity(), sizeof(CapturedComponent));
		for (const auto& component : components) add(component.values.capacity(), sizeof(std::byte));
		return bytes;
	}

	bool ConsistencyGroupUpdate::CanRefresh(const ConsistencyGroupUpdate& previous) const
	{
		if (group != previous.group || lifecycle != previous.lifecycle || sourceUpdate <= previous.sourceUpdate ||
			members.size() != previous.members.size() || resources.size() != previous.resources.size() ||
			components.size() != previous.components.size()) return false;
		for (std::size_t i = 0; i < members.size(); ++i) {
			const auto& a = members[i];
			const auto& b = previous.members[i];
			if (a.identity != b.identity || a.skinned != b.skinned || a.face != b.face) return false;
		}
		for (std::size_t i = 0; i < resources.size(); ++i) {
			const auto& a = resources[i];
			const auto& b = previous.resources[i];
			if (a.identity != b.identity || a.version != b.version || a.owner != b.owner ||
				a.owner.owner_before(b.owner) || b.owner.owner_before(a.owner)) return false;
		}
		// Conservative: only transforms, pose and face values are replaceable.
		// Structural/material changes must remain ordered ReplaceGroup events.
		for (std::size_t i = 0; i < components.size(); ++i) {
			const auto& a = components[i];
			const auto& b = previous.components[i];
			if (a.member != b.member || a.kind != b.kind) return false;
			if (a.kind != ComponentKind::Geometry && a.kind != ComponentKind::Material) continue;
			if (a.values != b.values) return false;
		}
		return true;
	}

	namespace
	{
		bool Valid(Identity value) { return value.id && value.incarnation; }
	}

	std::shared_ptr<const ConsistencyGroupUpdate> ConsistencyGroupUpdate::Capture(Identity group, std::uint64_t lifecycle, std::uint64_t sourceUpdate,
		std::span<const MemberInput> members, std::span<const ComponentInput> components, std::span<const ResourceLease> resources)
	{
		if (!Valid(group) || !lifecycle || !sourceUpdate) throw std::invalid_argument("invalid captured group identity/update");
		std::map<std::uint64_t, std::pair<Identity, unsigned>> membership;
		for (const auto& member : members) {
			if (!Valid(member.identity) || !membership.emplace(member.identity.id, std::pair{ member.identity, 0u }).second)
				throw std::invalid_argument("invalid or duplicate captured member");
		}
		for (const auto& component : components) {
			const auto found = membership.find(component.member.id);
			const auto kind = static_cast<unsigned>(component.kind);
			if (found == membership.end() || found->second.first != component.member || kind > 3 || component.values.empty())
				throw std::invalid_argument("invalid captured component");
			const auto bit = 1u << kind;
			if (found->second.second & bit) throw std::invalid_argument("duplicate captured component");
			found->second.second |= bit;
		}
		for (const auto& member : members) {
			const auto required = 3u | (member.skinned ? 4u : 0u) | (member.face ? 8u : 0u);
			if (membership.at(member.identity.id).second != required)
				throw std::invalid_argument("incomplete or mismatched captured group components");
		}
		std::map<std::uint64_t, bool> resourceIds;
		for (const auto& resource : resources)
			if (!Valid(resource.identity) || !resource.version || !resource.owner || !resourceIds.emplace(resource.identity.id, true).second)
				throw std::invalid_argument("unleased captured resource");
		auto result = std::shared_ptr<ConsistencyGroupUpdate>(new ConsistencyGroupUpdate);
		result->group = group;
		result->lifecycle = lifecycle;
		result->sourceUpdate = sourceUpdate;
		result->members.assign(members.begin(), members.end());
		result->resources.assign(resources.begin(), resources.end());
		result->components.reserve(components.size());
		for (const auto& component : components)
			result->components.push_back({ component.member, component.kind, { component.values.begin(), component.values.end() } });
		std::sort(result->components.begin(), result->components.end(), [](const auto& a, const auto& b) {
			return a.member.id != b.member.id ? a.member.id < b.member.id : a.kind < b.kind;
		});
		return result;
	}

	CapturedSceneReducer::Result CapturedSceneReducer::Apply(CapturedSceneUpdate capture)
	{
		// Transactional allocation: failure leaves both the root and tombstones intact.
		auto next = std::make_shared<CapturedSceneState>(*current);
		auto nextHeads = heads;
		Result result;
		for (const auto& event : capture.events) {
			const auto reject = [&] { ++result.rejected; };
			if (!event.sequence || event.sequence <= next->sequence) { reject(); continue; }
			// Sequence is global across resets. Even invalid data consumes its sequence.
			next->sequence = event.sequence;
			if (event.kind == SceneEventKind::Reset) {
				if (event.lifecycle <= next->lifecycle || event.update || event.group != Identity{}) { reject(); continue; }
				next->lifecycle = event.lifecycle;
				next->groups.clear();
				nextHeads.clear();
				++result.applied;
				continue;
			}
			if (!event.lifecycle || event.lifecycle != next->lifecycle || !Valid(event.group)) { reject(); continue; }
			const auto found = nextHeads.find(event.group.id);
			const Head previous = found == nextHeads.end() ? Head{} : found->second;
			if (event.group.incarnation < previous.incarnation) { reject(); continue; }
			if (event.kind == SceneEventKind::DetachGroup) {
				if (event.update) { reject(); continue; }
				nextHeads[event.group.id] = { event.group.incarnation, 0, true };
				next->groups.erase(event.group.id);
			} else if (event.kind == SceneEventKind::ReplaceGroup || event.kind == SceneEventKind::RefreshGroup) {
				if (!event.update || event.update->Group() != event.group || event.update->Lifecycle() != event.lifecycle ||
					(event.group.incarnation == previous.incarnation &&
						(previous.detached || event.update->SourceUpdate() <= previous.source))) { reject(); continue; }
				if (event.kind == SceneEventKind::RefreshGroup) {
					const auto base = next->groups.find(event.group.id);
					if (base == next->groups.end() || !event.update->CanRefresh(*base->second)) { reject(); continue; }
				}
				nextHeads[event.group.id] = { event.group.incarnation, event.update->SourceUpdate(), false };
				next->groups[event.group.id] = event.update;
			} else { reject(); continue; }
			++result.applied;
		}
		heads.swap(nextHeads);
		current = std::move(next);
		return result;
	}
}
