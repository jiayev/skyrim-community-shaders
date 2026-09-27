#pragma once

#include <cstdint>
#include <exception>
#include <unordered_map>

namespace DCLF
{
	// Coordinator-only lifecycle IDs. Raw keys never escape this tracker; a
	// publication carries only the returned IDs. Reset drops keys but not the
	// allocation counter, so a new world cannot reuse an old publication ID.
	class SceneIdentity
	{
	public:
		struct Assignment
		{
			std::uint64_t member = 0, group = 0;
			bool replaced = false;
		};
		Assignment Attach(const void* a_member, const void* a_actor)
		{
			if (!a_member)
				return {};
			bool replaced = false;
			if (const auto it = members.find(a_member); it != members.end()) {
				if (it->second.actor == a_actor)
					return { it->second.identity, it->second.group, false };
				ReleaseGroup(it->second.actor);
				members.erase(it);
				replaced = true;
			}
			const auto identity = Next();
			std::uint64_t group = identity;
			if (a_actor) {
				auto [it, inserted] = groups.try_emplace(a_actor);
				if (inserted)
					it->second.identity = Next();
				++it->second.members;
				group = it->second.identity;
			}
			members.emplace(a_member, Member{ identity, group, a_actor });
			return { identity, group, replaced };
		}
		void Detach(const void* a_member)
		{
			const auto it = members.find(a_member);
			if (it == members.end())
				return;
			ReleaseGroup(it->second.actor);
			members.erase(it);
		}
		void Reset()
		{
			members.clear();
			groups.clear();
		}
	private:
		struct Member { std::uint64_t identity, group; const void* actor; };
		struct Group { std::uint64_t identity = 0; std::uint32_t members = 0; };
		std::uint64_t next = 1;
		std::unordered_map<const void*, Member> members;
		std::unordered_map<const void*, Group> groups;
		std::uint64_t Next()
		{
			const auto value = next++;
			// Wrapping would violate ABA prevention; fail closed long before it
			// could let a publication refer to an unrelated object.
			if (!value)
				std::terminate();
			return value;
		}
		void ReleaseGroup(const void* a_actor)
		{
			if (!a_actor)
				return;
			const auto it = groups.find(a_actor);
			if (it == groups.end())
				return;
			if (it->second.members <= 1)
				groups.erase(it);
			else
				--it->second.members;
		}
	};
}
