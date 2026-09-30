#pragma once

#include "../Material/Material.h"

namespace SkinSources
{
	struct Asset
	{
		SkinMaterials::Definition definition;
		std::string path;
		uint32_t block = UINT32_MAX;
		uint64_t fingerprint = 0;
		std::array<std::string, 9> textures;
	};
	struct Snapshot
	{
		std::shared_ptr<const Asset> asset;
		RE::FormID txst = 0;
		RE::FormID headPart = 0;
		RE::FormID npc = 0;
		uint64_t revision = 0;
	};
	void Install();
	void Prune();
	Snapshot Get(RE::BSShaderProperty* a_property);
	std::array<std::string, 9> Paths(RE::BSTextureSet* a_textureSet);
	SkinMaterials::Material AssignedMaterial(const Snapshot& a_source);
}
