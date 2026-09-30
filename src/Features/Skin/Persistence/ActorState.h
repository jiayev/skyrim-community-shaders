#pragma once

#include "../Material/Material.h"

#include <map>
#include <memory>

namespace SkinActors
{
	enum class Part : uint32_t
	{
		Face,
		Body,
		Hands,
		Feet,
		Count
	};
	std::array<const char*, 4> PartNames();

	struct Guard
	{
		std::string race, headPart, armor, addon;
		uint32_t sex = 0;
		uint32_t materialBlock = UINT32_MAX;
		bool firstPerson = false;
		bool resolved = true;
		std::string layout;
		std::string source;
		std::array<std::string, 3> baseTextures;
		std::array<uint64_t, 3> resourceRevisions{};
		std::array<float, 4> uvTransform{};
		bool operator==(const Guard&) const = default;
	};
	struct Surface
	{
		SkinMaterials::Changes changes;
		Guard guard;
	};
	using Parts = std::array<std::vector<Surface>, 4>;
	struct Binding
	{
		std::string name;
		bool persistent = true;
		Parts parts;
	};
	struct State
	{
		std::map<std::string, Binding> actors;
		std::map<std::string, Binding> undo;
		uint64_t session = 1;
		uint64_t revision = 1;
		bool available = false;
		std::filesystem::path path;
		std::string message;
	};
	using Snapshot = std::shared_ptr<const State>;
	struct Sample
	{
		SkinMaterials::Material material;
		std::string layout;
		std::array<float, 4> uvTransform{};
		bool firstPerson = false;
	};
	struct Scheme
	{
		std::array<std::vector<Sample>, 4> parts;
		std::array<bool, 4> selected{};
	};

	std::string FormKey(const RE::TESForm* a_form);
	std::string ActorKey(RE::Actor* a_actor);
	bool PersistentKey(const std::string& a_key);
	std::filesystem::path Directory();
	void Install();
	void InstallLifecycle();
	void Reset();
	Snapshot Get();
	uint64_t Revision();
	uint64_t Session();
	const Binding* Find(const State& a_state, const std::string& a_key);
	const Surface* Find(const Binding* a_binding, Part a_part, const Guard& a_guard);
	void Commit(const std::string& a_key, Binding a_binding, uint64_t a_revision, uint64_t a_session);
	void Clear(const std::string& a_key, Part a_part, uint64_t a_revision, uint64_t a_session);
	void Undo(const std::string& a_key, uint64_t a_revision, uint64_t a_session);
	void OpenStorage(const std::filesystem::path& a_path, bool a_copy, bool a_empty = false);
	void Recover();
	void SaveScheme(const std::filesystem::path& a_path, const Scheme& a_scheme);
	Scheme LoadScheme(const std::filesystem::path& a_path);
	SkinMaterials::Changes Capture(const SkinMaterials::Material& a_material);
	bool Compatible(const Sample& a_sample, const Guard& a_guard);
}
