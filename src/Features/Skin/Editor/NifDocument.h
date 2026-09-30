#pragma once

#include "../Material/Material.h"

#include <filesystem>

namespace SkinEditor
{
	using Bytes = std::vector<uint8_t>;
	Bytes ReadResource(const std::string& a_path);
	uint64_t Fingerprint(std::span<const uint8_t> a_bytes);

	class NifDocument
	{
	public:
		struct Surface
		{
			uint32_t block = 0;
			std::string name;
			SkinMaterials::Definition definition;
			std::array<std::string, 9> textures;
			bool compatible = false;
		};
		struct Shape
		{
			uint32_t block;
			uint32_t material;
			std::string name;
		};

		void Open(const std::string& a_source);
		void Read(Bytes a_bytes);
		std::vector<Surface> Surfaces() const;
		std::vector<Shape> Shapes() const;
		uint32_t MakeIndependent(uint32_t a_shape);
		static NifDocument Template(const SkinMaterials::Material& a_material);
		void SetMaterial(uint32_t a_block, const SkinMaterials::Material& a_material);
		Bytes Serialize() const;
		Bytes VerifiedBytes() const;
		void Save(const std::filesystem::path& a_path);
		const std::string& Source() const { return source; }
		uint64_t SourceFingerprint() const { return Fingerprint(original); }
		bool Dirty() const { return !changed.empty() || !relinked.empty(); }

	private:
		struct Block
		{
			uint16_t type = 0;
			Bytes bytes;
		};
		struct Property
		{
			uint32_t feature = 0;
			uint32_t name = 0;
			std::vector<uint32_t> extras;
			size_t tail = 0;
			size_t textureOffset = 0;
			uint32_t texture = 0;
			uint64_t flags = 0;
		};
		std::string source;
		Bytes original;
		Bytes prefix;
		size_t blockCountOffset = 0;
		std::vector<std::string> types;
		std::vector<std::string> strings;
		std::vector<Block> blocks;
		Bytes footer;
		std::map<uint32_t, SkinMaterials::Material> changed;
		std::map<uint32_t, std::pair<size_t, uint32_t>> relinked;
		Property ReadProperty(uint32_t a_block) const;
		std::string String(uint32_t a_index) const;
		uint32_t AddBlock(const std::string& a_type, Bytes a_bytes);
		uint32_t AddString(const std::string& a_value);
	};
}
