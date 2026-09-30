#pragma once

#include "../Persistence/ActorState.h"
#include "Sources.h"

namespace SkinActors
{
	struct Target
	{
		Part part = Part::Count;
		Guard guard;
		std::string error;
		bool character = false;
		bool persistent = false;
		RE::ActorHandle actor;
		RE::FormID armor = 0, addon = 0;
		std::string key, name;
	};
	bool Current(const Target& a_target, RE::BSGeometry* a_geometry);
	RE::Actor* Owner(RE::BSGeometry* a_geometry);
	bool Belongs(RE::BSGeometry* a_geometry, RE::Actor* a_actor);
	Target Describe(RE::BSGeometry* a_geometry, const SkinSources::Snapshot& a_source,
		const std::function<uint64_t(const std::string&)>& a_resourceRevision, RE::Actor* a_actor = nullptr);
}
