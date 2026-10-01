#include "NifDocument.h"
#include "Files.h"

#include <cmath>
#include <fstream>
#include <set>

namespace SkinEditor
{
	namespace
	{
		constexpr size_t MaxBytes = 512 * 1024 * 1024;
		constexpr uint32_t MaxBlocks = 262144;
		constexpr uint32_t NoLink = 0xFFFFFFFF;

		struct Reader
		{
			std::span<const uint8_t> bytes;
			size_t pos = 0;
			std::span<const uint8_t> Take(size_t size)
			{
				if (size > bytes.size() - pos)
					throw std::runtime_error(T("feature.skin.truncated_nif_data", "Truncated NIF data."));
				const auto result = bytes.subspan(pos, size);
				pos += size;
				return result;
			}
			template <class ValueType>
			ValueType Get()
			{
				ValueType value;
				const auto data = Take(sizeof(ValueType));
				std::memcpy(&value, data.data(), sizeof(ValueType));
				return value;
			}
			std::string Text()
			{
				const auto size = Get<uint32_t>();
				if (size > 1024 * 1024)
					throw std::runtime_error(T("feature.skin.nif_string_exceeds_the_supported_limit", "NIF string exceeds the supported limit."));
				const auto text = Take(size);
				return { reinterpret_cast<const char*>(text.data()), text.size() };
			}
		};
		template <class ValueType>
		void Put(Bytes& a_bytes, ValueType a_value)
		{
			const auto* data = reinterpret_cast<const uint8_t*>(&a_value);
			a_bytes.insert(a_bytes.end(), data, data + sizeof(ValueType));
		}
		template <class ValueType>
		void Set(Bytes& a_bytes, size_t a_offset, ValueType a_value)
		{
			if (a_offset > a_bytes.size() || sizeof(ValueType) > a_bytes.size() - a_offset)
				throw std::runtime_error(T("feature.skin.invalid_nif_field_offset", "Invalid NIF field offset."));
			std::memcpy(a_bytes.data() + a_offset, &a_value, sizeof(ValueType));
		}
		void PutText(Bytes& a_bytes, const std::string& a_value)
		{
			Put(a_bytes, static_cast<uint32_t>(a_value.size()));
			a_bytes.insert(a_bytes.end(), a_value.begin(), a_value.end());
		}
	}

	uint64_t Fingerprint(std::span<const uint8_t> a_bytes)
	{
		uint64_t value = 14695981039346656037ULL;
		for (auto byte : a_bytes)
			value = (value ^ byte) * 1099511628211ULL;
		return value;
	}

	Bytes ReadResource(const std::string& a_path)
	{
		const auto path = std::filesystem::u8path(a_path);
		if (path.is_absolute() || std::filesystem::is_regular_file(path)) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			const auto size = input.tellg();
			if (!input || size < 0 || size > static_cast<std::streamoff>(MaxBytes))
				throw std::runtime_error(T("feature.skin.asset_read_limit", "Cannot read asset or asset exceeds 512 MiB: ") + a_path);
			Bytes bytes(static_cast<size_t>(size));
			input.seekg(0);
			if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
				throw std::runtime_error(T("feature.skin.could_not_read_the_complete_asset", "Could not read the complete asset: ") + a_path);
			return bytes;
		}
		std::string resourcePath = a_path;
		std::replace(resourcePath.begin(), resourcePath.end(), '/', '\\');
		if (resourcePath.size() >= 5 && _strnicmp(resourcePath.c_str(), "data\\", 5) == 0)
			resourcePath.erase(0, 5);
		RE::BSResourceNiBinaryStream input(resourcePath);
		if (!input.good() || !input.stream || input.stream->totalSize > MaxBytes)
			throw std::runtime_error(T("feature.skin.game_resource_is_unavailable", "Game resource is unavailable: ") + a_path);
		Bytes bytes(input.stream->totalSize);
		if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<uint32_t>(bytes.size())))
			throw std::runtime_error(T("feature.skin.could_not_read_the_complete_game_resource", "Could not read the complete game resource: ") + a_path);
		return bytes;
	}

	void NifDocument::Open(const std::string& a_source)
	{
		NifDocument next;
		next.Read(ReadResource(a_source));
		next.source = a_source;
		*this = std::move(next);
	}

	void NifDocument::Read(Bytes a_bytes)
	{
		NifDocument next;
		Reader reader{ a_bytes };
		constexpr std::string_view magic = "Gamebryo File Format, Version 20.2.0.7\n";
		const auto header = reader.Take(magic.size());
		if (!std::equal(header.begin(), header.end(), magic.begin()) || reader.Get<uint32_t>() != 0x14020007 ||
			reader.Get<uint8_t>() != 1 || reader.Get<uint32_t>() != 12)
			throw std::runtime_error(T("feature.skin.only_little_endian_skyrim_se_nif_20_2_0_7", "Only little-endian Skyrim SE NIF 20.2.0.7 / user version 12 is editable."));
		next.blockCountOffset = reader.pos;
		const auto count = reader.Get<uint32_t>();
		if (count > MaxBlocks || reader.Get<uint32_t>() != 100)
			throw std::runtime_error(T("feature.skin.only_skyrim_se_stream_version_100_is_editable", "Only Skyrim SE stream version 100 is editable."));
		for (size_t i = 0; i < 3; ++i)
			reader.Take(reader.Get<uint8_t>());
		next.prefix.assign(a_bytes.begin(), a_bytes.begin() + reader.pos);
		const auto typeCount = reader.Get<uint16_t>();
		for (uint32_t i = 0; i < typeCount; ++i)
			next.types.push_back(reader.Text());
		next.blocks.resize(count);
		for (auto& block : next.blocks) {
			block.type = reader.Get<uint16_t>();
			if (block.type >= next.types.size())
				throw std::runtime_error(T("feature.skin.invalid_nif_block_type", "Invalid NIF block type."));
		}
		std::vector<uint32_t> sizes(count);
		for (auto& size : sizes)
			size = reader.Get<uint32_t>();
		const auto stringCount = reader.Get<uint32_t>();
		reader.Get<uint32_t>();
		if (stringCount > MaxBlocks)
			throw std::runtime_error(T("feature.skin.too_many_nif_strings", "Too many NIF strings."));
		for (uint32_t i = 0; i < stringCount; ++i)
			next.strings.push_back(reader.Text());
		if (reader.Get<uint32_t>() != 0)
			throw std::runtime_error(T("feature.skin.nif_files_with_block_groups_are_not_editable", "NIF files with block groups are not editable."));
		for (size_t i = 0; i < sizes.size(); ++i) {
			const auto block = reader.Take(sizes[i]);
			next.blocks[i].bytes.assign(block.begin(), block.end());
		}
		const auto footerStart = reader.pos;
		const auto roots = reader.Get<uint32_t>();
		if (roots > count)
			throw std::runtime_error(T("feature.skin.invalid_nif_root_count", "Invalid NIF root count."));
		for (uint32_t i = 0; i < roots; ++i) {
			const auto root = reader.Get<uint32_t>();
			if (root != NoLink && root >= count)
				throw std::runtime_error(T("feature.skin.invalid_nif_root_reference", "Invalid NIF root reference."));
		}
		if (reader.pos != a_bytes.size())
			throw std::runtime_error(T("feature.skin.unexpected_data_after_the_nif_footer", "Unexpected data after the NIF footer."));
		next.footer.assign(a_bytes.begin() + footerStart, a_bytes.end());
		next.original = std::move(a_bytes);
		*this = std::move(next);
	}

	std::string NifDocument::String(uint32_t a_index) const
	{
		if (a_index == NoLink)
			return {};
		if (a_index >= strings.size())
			throw std::runtime_error(T("feature.skin.invalid_nif_string_reference", "Invalid NIF string reference."));
		return strings[a_index];
	}

	NifDocument::Property NifDocument::ReadProperty(uint32_t a_block) const
	{
		if (a_block >= blocks.size() || types[blocks[a_block].type] != "BSLightingShaderProperty")
			throw std::runtime_error(T("feature.skin.the_selected_block_is_not_a_lighting_shader_property", "The selected block is not a lighting shader property."));
		Reader reader{ blocks[a_block].bytes };
		Property property;
		property.feature = reader.Get<uint32_t>();
		property.name = reader.Get<uint32_t>();
		const auto count = reader.Get<uint32_t>();
		if (count > 4096)
			throw std::runtime_error(T("feature.skin.too_many_property_extra_data_references", "Too many property extra-data references."));
		for (uint32_t i = 0; i < count; ++i) {
			const auto link = reader.Get<uint32_t>();
			if (link >= blocks.size())
				throw std::runtime_error(T("feature.skin.invalid_property_extra_data_reference", "Invalid property extra-data reference."));
			property.extras.push_back(link);
		}
		property.tail = reader.pos;
		reader.Get<uint32_t>();
		property.flags = reader.Get<uint64_t>();
		reader.Take(16);
		property.textureOffset = reader.pos;
		property.texture = reader.Get<uint32_t>();
		if (property.feature == 4 || property.feature == 5) {
			reader.Take(property.feature == 5 ? 68 : 56);
			if (reader.pos != reader.bytes.size())
				throw std::runtime_error(T("feature.skin.skin_property_payload", "Unexpected native skin shader-property payload."));
		}
		return property;
	}

	std::vector<NifDocument::Surface> NifDocument::Surfaces() const
	{
		using namespace SkinMaterials;
		std::vector<Surface> result;
		for (uint32_t id = 0; id < blocks.size(); ++id) {
			if (types[blocks[id].type] != "BSLightingShaderProperty")
				continue;
			Surface surface;
			surface.block = id;
			try {
				const auto property = ReadProperty(id);
				surface.name = String(property.name);
				surface.compatible = (property.feature == 4 || property.feature == 5) &&
				                     (property.flags & ((1ULL << 7) | (1ULL << 26) | (1ULL << 27) | (1ULL << 55) | (1ULL << 56))) == 0;
				std::map<std::string, uint32_t> fields;
				for (auto extra : property.extras) {
					Reader reader{ blocks[extra].bytes };
					const auto name = String(reader.Get<uint32_t>());
					if (name.starts_with("CS_Skin") && !fields.emplace(name, extra).second)
						throw std::runtime_error(T("feature.skin.duplicate_skin_field", "Duplicate Skin field: ") + name);
				}
				if (property.texture != NoLink) {
					if (property.texture >= blocks.size() || types[blocks[property.texture].type] != "BSShaderTextureSet")
						throw std::runtime_error(T("feature.skin.invalid_shader_texture_set_reference", "Invalid shader texture-set reference."));
					Reader reader{ blocks[property.texture].bytes };
					if (reader.Get<uint32_t>() != 9)
						throw std::runtime_error(T("feature.skin.expected_nine_skyrim_se_texture_slots", "Expected nine Skyrim SE texture slots."));
					for (auto& path : surface.textures)
						path = reader.Text();
					if (reader.pos != reader.bytes.size())
						throw std::runtime_error(T("feature.skin.unexpected_texture_set_payload", "Unexpected texture-set payload."));
				}
				if (const auto marker = fields.find("CS_SkinVersion"); marker != fields.end()) {
					Reader reader{ blocks[marker->second].bytes };
					reader.Get<uint32_t>();
					if (types[blocks[marker->second].type] != "NiIntegerExtraData" || reader.Get<uint32_t>() != Version || reader.pos != reader.bytes.size())
						throw std::runtime_error(T("feature.skin.unsupported_cs_skinversion", "Unsupported CS_SkinVersion."));
					if (!surface.compatible)
						throw std::runtime_error(T("feature.skin.skin_extension_conflicts_with_the_native_shader_configuration", "Skin extension conflicts with the native shader configuration."));
					auto& material = surface.definition.material;
					const auto table = ParameterTable();
					for (size_t i = 0; i < table.size(); ++i) {
						if (const auto found = fields.find(table[i].name); found != fields.end()) {
							const auto& block = blocks[found->second];
							if (types[block.type] != (table[i].integer ? "NiIntegerExtraData" : "NiFloatExtraData"))
								throw std::runtime_error(std::string(T("feature.skin.wrong_field_type", "Wrong field type: ")) + table[i].name);
							Reader field{ block.bytes };
							field.Get<uint32_t>();
							material.specified[i] = true;
							material.parameters.values[i] = table[i].integer ? static_cast<float>(field.Get<int32_t>()) : field.Get<float>();
							if (field.pos != field.bytes.size())
								throw std::runtime_error(T("feature.skin.unexpected_extra_data_payload", "Unexpected extra-data payload."));
						}
					}
					for (size_t i = 0; i < TextureSlots.size(); ++i)
						material.textures[i] = NormalizeTexturePath(surface.textures[TextureSlots[i]]);
					Validate(material);
					material.enabled = true;
					surface.definition.status = Status::Valid;
				}
			} catch (const std::exception& e) {
				surface.definition.status = Status::Invalid;
				surface.definition.diagnostic = e.what();
			}
			result.push_back(std::move(surface));
		}
		return result;
	}

	uint32_t NifDocument::AddString(const std::string& a_value)
	{
		const auto found = std::find(strings.begin(), strings.end(), a_value);
		if (found != strings.end())
			return static_cast<uint32_t>(found - strings.begin());
		strings.push_back(a_value);
		return static_cast<uint32_t>(strings.size() - 1);
	}

	std::vector<NifDocument::Shape> NifDocument::Shapes() const
	{
		std::vector<Shape> result;
		for (uint32_t id = 0; id < blocks.size(); ++id) {
			const auto& type = types[blocks[id].type];
			if (type != "BSTriShape" && type != "BSDynamicTriShape" && type != "BSSubIndexTriShape" && type != "BSMeshLODTriShape")
				continue;
			Reader reader{ blocks[id].bytes };
			const auto name = String(reader.Get<uint32_t>());
			const auto extras = reader.Get<uint32_t>();
			if (extras > 4096)
				throw std::runtime_error(T("feature.skin.too_many_shape_extra_data_references", "Too many shape extra-data references."));
			reader.Take(static_cast<size_t>(extras) * 4 + 84);
			result.push_back({ id, reader.Get<uint32_t>(), name });
		}
		return result;
	}

	uint32_t NifDocument::MakeIndependent(uint32_t a_shape)
	{
		const auto shapes = Shapes();
		const auto shape = std::find_if(shapes.begin(), shapes.end(), [a_shape](const auto& item) { return item.block == a_shape; });
		if (shape == shapes.end())
			throw std::runtime_error(T("feature.skin.select_a_supported_shape_before_making_its_material_independent", "Select a supported shape before making its material independent."));
		ReadProperty(shape->material);
		NifDocument next = *this;
		const auto material = next.AddBlock("BSLightingShaderProperty", blocks[shape->material].bytes);
		Reader reader{ blocks[a_shape].bytes };
		reader.Get<uint32_t>();
		const auto extras = reader.Get<uint32_t>();
		const size_t offset = 92 + static_cast<size_t>(extras) * 4;
		Set(next.blocks[a_shape].bytes, offset, material);
		next.relinked[a_shape] = { offset, material };
		*this = std::move(next);
		return material;
	}

	NifDocument NifDocument::Template(const SkinMaterials::Material& a_material)
	{
		NifDocument document;
		constexpr std::string_view magic = "Gamebryo File Format, Version 20.2.0.7\n";
		document.prefix.assign(magic.begin(), magic.end());
		Put(document.prefix, uint32_t{ 0x14020007 });
		Put(document.prefix, uint8_t{ 1 });
		Put(document.prefix, uint32_t{ 12 });
		document.blockCountOffset = document.prefix.size();
		Put(document.prefix, uint32_t{ 1 });
		Put(document.prefix, uint32_t{ 100 });
		for (int i = 0; i < 3; ++i) {
			Put(document.prefix, uint8_t{ 1 });
			Put(document.prefix, uint8_t{ 0 });
		}
		Bytes property;
		Put(property, uint32_t{ 5 });
		Put(property, document.AddString("Advanced Skin Material"));
		Put(property, uint32_t{ 0 });
		Put(property, NoLink);
		Put(property, uint32_t{ 0x80200001 });
		Put(property, uint32_t{ 1 });
		for (float value : { 0.f, 0.f, 1.f, 1.f })
			Put(property, value);
		Put(property, NoLink);
		for (float value : { 0.f, 0.f, 0.f, 1.f })
			Put(property, value);
		Put(property, uint32_t{ 3 });
		for (float value : { 1.f, 0.f, 20.f, 1.f, 1.f, 1.f, 1.f, 0.3f, 2.f, 1.f, 1.f, 1.f })
			Put(property, value);
		document.AddBlock("BSLightingShaderProperty", std::move(property));
		Put(document.footer, uint32_t{ 1 });
		Put(document.footer, uint32_t{ 0 });
		document.original = document.Serialize();
		document.SetMaterial(0, a_material);
		return document;
	}

	uint32_t NifDocument::AddBlock(const std::string& a_type, Bytes a_bytes)
	{
		auto found = std::find(types.begin(), types.end(), a_type);
		if (found == types.end()) {
			if (types.size() >= UINT16_MAX)
				throw std::runtime_error(T("feature.skin.too_many_nif_block_types", "Too many NIF block types."));
			types.push_back(a_type);
			found = types.end() - 1;
		}
		if (blocks.size() >= MaxBlocks)
			throw std::runtime_error(T("feature.skin.too_many_nif_blocks", "Too many NIF blocks."));
		blocks.push_back({ static_cast<uint16_t>(found - types.begin()), std::move(a_bytes) });
		return static_cast<uint32_t>(blocks.size() - 1);
	}

	void NifDocument::SetMaterial(uint32_t a_block, const SkinMaterials::Material& a_material)
	{
		using namespace SkinMaterials;
		Validate(a_material);
		const auto surfaces = Surfaces();
		const auto selected = std::find_if(surfaces.begin(), surfaces.end(), [a_block](const auto& surface) { return surface.block == a_block; });
		if (selected == surfaces.end() || !selected->compatible || selected->definition.status == Status::Invalid)
			throw std::runtime_error(T("feature.skin.select_a_compatible_valid_skin_shader_property", "Select a compatible, valid skin shader property."));
		NifDocument next = *this;
		const auto property = ReadProperty(a_block);
		std::vector<uint32_t> extras;
		for (const auto id : property.extras) {
			Reader reader{ blocks[id].bytes };
			const auto name = String(reader.Get<uint32_t>());
			const auto fields = ParameterTable();
			if (name != "CS_SkinVersion" && std::none_of(fields.begin(), fields.end(), [&](const auto& field) { return name == field.name; }))
				extras.push_back(id);
		}
		if (a_material.enabled) {
			Bytes marker;
			Put(marker, next.AddString("CS_SkinVersion"));
			Put(marker, Version);
			extras.push_back(next.AddBlock("NiIntegerExtraData", std::move(marker)));
			const auto table = ParameterTable();
			for (size_t i = 0; i < table.size(); ++i) {
				if (!a_material.specified[i])
					continue;
				Bytes field;
				Put(field, next.AddString(table[i].name));
				if (table[i].integer)
					Put(field, static_cast<int32_t>(a_material.parameters.values[i]));
				else
					Put(field, a_material.parameters.values[i]);
				extras.push_back(next.AddBlock(table[i].integer ? "NiIntegerExtraData" : "NiFloatExtraData", std::move(field)));
			}
		}
		Bytes replacement;
		Put(replacement, property.feature);
		Put(replacement, property.name);
		Put(replacement, static_cast<uint32_t>(extras.size()));
		for (const auto extra : extras)
			Put(replacement, extra);
		const auto tailOffset = replacement.size();
		replacement.insert(replacement.end(), blocks[a_block].bytes.begin() + property.tail, blocks[a_block].bytes.end());
		if (a_material.enabled) {
			auto textures = selected->textures;
			for (size_t i = 0; i < TextureSlots.size(); ++i)
				textures[TextureSlots[i]] = NormalizeTexturePath(a_material.textures[i]);
			Bytes textureSet;
			Put(textureSet, uint32_t{ 9 });
			for (const auto& path : textures)
				PutText(textureSet, path);
			Set(replacement, tailOffset + property.textureOffset - property.tail, next.AddBlock("BSShaderTextureSet", std::move(textureSet)));
		}
		next.blocks[a_block].bytes = std::move(replacement);
		next.changed[a_block] = a_material;
		for (auto& path : next.changed[a_block].textures)
			path = NormalizeTexturePath(path);
		*this = std::move(next);
	}

	Bytes NifDocument::Serialize() const
	{
		Bytes bytes = prefix;
		Set(bytes, blockCountOffset, static_cast<uint32_t>(blocks.size()));
		Put(bytes, static_cast<uint16_t>(types.size()));
		for (const auto& type : types)
			PutText(bytes, type);
		for (const auto& block : blocks)
			Put(bytes, block.type);
		for (const auto& block : blocks)
			Put(bytes, static_cast<uint32_t>(block.bytes.size()));
		Put(bytes, static_cast<uint32_t>(strings.size()));
		size_t longest = 0;
		for (const auto& string : strings)
			longest = std::max(longest, string.size());
		Put(bytes, static_cast<uint32_t>(longest));
		for (const auto& string : strings)
			PutText(bytes, string);
		Put(bytes, uint32_t{ 0 });
		for (const auto& block : blocks)
			bytes.insert(bytes.end(), block.bytes.begin(), block.bytes.end());
		bytes.insert(bytes.end(), footer.begin(), footer.end());
		if (bytes.size() > MaxBytes)
			throw std::runtime_error(T("feature.skin.edited_nif_exceeds_the_supported_size", "Edited NIF exceeds the supported size."));
		return bytes;
	}

	Bytes NifDocument::VerifiedBytes() const
	{
		if (!source.empty() && ReadResource(source) != original)
			throw std::runtime_error(T("feature.skin.source_changed_on_disk_reopen_it_before_saving", "Source changed on disk. Reopen it before saving."));
		const auto bytes = Serialize();
		NifDocument verified;
		verified.Read(bytes);
		const auto surfaces = verified.Surfaces();
		for (const auto& [id, material] : changed) {
			const auto surface = std::find_if(surfaces.begin(), surfaces.end(), [id](const auto& item) { return item.block == id; });
			if (surface == surfaces.end() || surface->definition.status == SkinMaterials::Status::Invalid ||
				(material.enabled && surface->definition.material != material) ||
				(!material.enabled && surface->definition.status != SkinMaterials::Status::Unmarked))
				throw std::runtime_error(T("feature.skin.nif_material_read_back_verification_failed", "NIF material read-back verification failed."));
		}
		NifDocument before;
		before.Read(original);
		for (uint32_t i = 0; i < before.blocks.size(); ++i) {
			if (changed.contains(i)) {
				const auto oldProperty = before.ReadProperty(i);
				const auto newProperty = verified.ReadProperty(i);
				Bytes tail(before.blocks[i].bytes.begin() + oldProperty.tail, before.blocks[i].bytes.end());
				Set(tail, oldProperty.textureOffset - oldProperty.tail, newProperty.texture);
				const Bytes savedTail(verified.blocks[i].bytes.begin() + newProperty.tail, verified.blocks[i].bytes.end());
				if (oldProperty.feature != newProperty.feature || oldProperty.name != newProperty.name || tail != savedTail)
					throw std::runtime_error(T("feature.skin.native_property_changed", "Save would change native shader-property settings."));
			}
			auto expected = before.blocks[i].bytes;
			if (const auto link = relinked.find(i); link != relinked.end())
				Set(expected, link->second.first, link->second.second);
			if (!changed.contains(i) && expected != verified.blocks[i].bytes)
				throw std::runtime_error(T("feature.skin.save_would_change_an_unrelated_nif_block", "Save would change an unrelated NIF block."));
		}
		return bytes;
	}

	void NifDocument::Save(const std::filesystem::path& a_path)
	{
		const Output output{ a_path, VerifiedBytes() };
		Publish(std::span(&output, 1));
		Open(UTF8(a_path));
	}
}
