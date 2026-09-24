#include "SkinMaterial.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>

namespace SkinMaterials
{
	namespace
	{
		constexpr ParameterInfo parameterTable[] = {
#define SKIN_FLOAT(name, value, min, max) { #name, &Parameters::name, nullptr, min, max },
#define SKIN_BOOL(name, value) { #name, nullptr, &Parameters::name, 0.0f, 1.0f },
#include "SkinParameters.def"
#undef SKIN_FLOAT
#undef SKIN_BOOL
		};
		static_assert(std::size(parameterTable) == ParameterCount);
		const std::filesystem::path packageDirectory = "Data/Shaders/Skin/Materials";
		const std::filesystem::path userFile = "Data/Shaders/Skin/User/SkinMaterials.user.json";

		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		void Fields(const json& a_json, std::initializer_list<std::string_view> a_allowed,
			std::initializer_list<std::string_view> a_required)
		{
			if (!a_json.is_object())
				throw std::runtime_error("Expected an object");
			for (const auto& [key, value] : a_json.items()) {
				if (std::find(a_allowed.begin(), a_allowed.end(), key) == a_allowed.end())
					throw std::runtime_error("Unknown field: " + key);
			}
			for (auto key : a_required) {
				if (!a_json.contains(key))
					throw std::runtime_error("Missing field: " + std::string(key));
			}
		}

		std::string String(const json& a_json, size_t a_limit = 260)
		{
			if (!a_json.is_string())
				throw std::runtime_error("Expected a string");
			auto value = a_json.get<std::string>();
			if (value.empty() || value.size() > a_limit || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32; }))
				throw std::runtime_error("Empty, oversized or invalid string");
			return value;
		}

		void Version(const json& a_json)
		{
			if (!a_json.at("schemaVersion").is_number_integer() || a_json.at("schemaVersion") != 1)
				throw std::runtime_error("Unsupported Skin material schemaVersion");
		}

		void Array(const json& a_json)
		{
			if (!a_json.is_array() || a_json.size() > MaxEntries)
				throw std::runtime_error("Expected an array of at most 4096 entries");
		}

		Target ReadTarget(const json& a_json)
		{
			Fields(a_json, { "surfaceId", "txst", "npc" }, {});
			Target result;
			if (a_json.contains("surfaceId")) {
				result.surfaceID = Lower(String(a_json.at("surfaceId"), 36));
				if (!IsSurfaceID(result.surfaceID))
					throw std::runtime_error("Invalid surfaceId");
			}
			if (a_json.contains("txst"))
				result.txst = ReadFormKey(a_json.at("txst"));
			if (a_json.contains("npc"))
				result.npc = ReadFormKey(a_json.at("npc"));
			if ((!result.surfaceID.empty() && result.txst) || (!result.HasSurface() && !result.npc))
				throw std::runtime_error("A target needs one surface identity, an NPC, or both");
			return result;
		}

		UserEdit ReadEdit(const json& a_json)
		{
			Fields(a_json, { "target", "material", "parameters" }, { "target", "parameters" });
			UserEdit result;
			result.target = ReadTarget(a_json.at("target"));
			result.parameters = ReadParameters(a_json.at("parameters"), false);
			if (a_json.contains("material")) {
				if (!result.target.HasSurface())
					throw std::runtime_error("NPC-only edits cannot replace a material");
				result.material = ReadMaterial(a_json.at("material"));
			}
			return result;
		}

		json ReadFile(const std::filesystem::path& a_path)
		{
			std::ifstream input(a_path, std::ios::binary);
			if (!input)
				throw std::runtime_error("Cannot open file");
			std::string text(MaxPackageBytes + 1, '\0');
			input.read(text.data(), static_cast<std::streamsize>(text.size()));
			text.resize(static_cast<size_t>(input.gcount()));
			if (text.size() > MaxPackageBytes || input.bad())
				throw std::runtime_error("File exceeds 4 MiB or could not be read");
			return Parse(text);
		}

		void ValidateRoot(const json& a_data, bool a_user)
		{
			if (a_user) {
				Fields(a_data, { "schemaVersion", "edits" }, { "schemaVersion", "edits" });
				Array(a_data.at("edits"));
			} else {
				Fields(a_data, { "schemaVersion", "ownerPlugin", "recordMaterials", "raceAdjustments", "npcAdjustments" },
					{ "schemaVersion", "ownerPlugin", "recordMaterials", "raceAdjustments", "npcAdjustments" });
				String(a_data.at("ownerPlugin"));
				for (const auto key : { "recordMaterials", "raceAdjustments", "npcAdjustments" })
					Array(a_data.at(key));
			}
			Version(a_data);
		}
	}

	std::span<const ParameterInfo, ParameterCount> ParameterTable() { return parameterTable; }

	json Parse(std::string_view a_text)
	{
		std::vector<std::set<std::string>> objects;
		return json::parse(a_text, [&](int a_depth, json::parse_event_t a_event, json& a_value) {
			if (a_depth > 16)
				throw std::runtime_error("Skin JSON nesting exceeds 16 levels");
			if (a_event == json::parse_event_t::object_start)
				objects.emplace_back();
			else if (a_event == json::parse_event_t::object_end)
				objects.pop_back();
			else if (a_event == json::parse_event_t::key && !objects.back().insert(a_value.get<std::string>()).second)
				throw std::runtime_error("Duplicate JSON key: " + a_value.get<std::string>());
			return true;
		});
	}

	json ToJSON(const Parameters& a_parameters)
	{
		json result = json::object();
		for (const auto& field : parameterTable) {
			if (field.toggle)
				result[field.name] = a_parameters.*field.toggle;
			else
				result[field.name] = a_parameters.*field.number;
		}
		return result;
	}

	json ToJSON(const Patch& a_patch)
	{
		json result = json::object();
		for (size_t i = 0; i < ParameterCount; ++i) {
			if (a_patch[i]) {
				if (parameterTable[i].toggle)
					result[parameterTable[i].name] = *a_patch[i] != 0;
				else
					result[parameterTable[i].name] = *a_patch[i];
			}
		}
		return result;
	}

	Patch ReadParameters(const json& a_json, bool a_complete)
	{
		if (!a_json.is_object() || (a_complete && a_json.size() != ParameterCount))
			throw std::runtime_error("Complete materials require all 20 parameters");
		Patch result{};
		for (const auto& [name, value] : a_json.items()) {
			auto field = std::find_if(std::begin(parameterTable), std::end(parameterTable), [&](const auto& f) { return name == f.name; });
			if (field == std::end(parameterTable))
				throw std::runtime_error("Unknown parameter: " + name);
			double number;
			if (field->toggle) {
				if (!value.is_boolean())
					throw std::runtime_error("Expected a boolean: " + name);
				number = value.get<bool>() ? 1.0 : 0.0;
			} else {
				if (!value.is_number())
					throw std::runtime_error("Expected a number: " + name);
				number = value.get<double>();
				if (!std::isfinite(number) || number < field->minimum || number > field->maximum)
					throw std::runtime_error("Parameter out of range: " + name);
			}
			result[static_cast<size_t>(field - parameterTable)] = number;
		}
		return result;
	}

	void Apply(Parameters& a_parameters, const Patch& a_patch)
	{
		for (size_t i = 0; i < ParameterCount; ++i) {
			if (!a_patch[i])
				continue;
			const auto& field = parameterTable[i];
			if (field.toggle)
				a_parameters.*field.toggle = *a_patch[i] != 0;
			else
				a_parameters.*field.number = static_cast<float>(*a_patch[i]);
		}
	}

	std::string NormalizeTexturePath(std::string a_path)
	{
		std::replace(a_path.begin(), a_path.end(), '\\', '/');
		a_path = Lower(a_path);
		if (a_path.size() > 260 || !a_path.starts_with("textures/") || !a_path.ends_with(".dds") ||
			a_path.find_first_of(":*?\"<>|") != std::string::npos ||
			std::any_of(a_path.begin(), a_path.end(), [](unsigned char c) { return c < 32; }))
			throw std::runtime_error("Texture must be a Data-relative textures/...dds path");
		for (const auto& component : std::filesystem::path(a_path)) {
			if (component.empty() || component == "." || component == ".." || component.string().back() == ' ' || component.string().back() == '.')
				throw std::runtime_error("Invalid texture path component");
		}
		if (a_path.find("//") != std::string::npos)
			throw std::runtime_error("Invalid texture path separator");
		return a_path;
	}

	Material ReadMaterial(const json& a_json)
	{
		Fields(a_json, { "name", "parameters", "textures" }, { "name", "parameters", "textures" });
		Material result;
		result.name = String(a_json.at("name"), 128);
		Apply(result.parameters, ReadParameters(a_json.at("parameters"), true));
		const auto& textures = a_json.at("textures");
		Fields(textures, { "rfaos", "wetness" }, { "rfaos", "wetness" });
		if (!textures.at("rfaos").is_null())
			result.rfaos = NormalizeTexturePath(String(textures.at("rfaos")));
		if (!textures.at("wetness").is_null())
			result.wetness = NormalizeTexturePath(String(textures.at("wetness")));
		return result;
	}

	json ToJSON(const Material& a_material)
	{
		return { { "name", a_material.name }, { "parameters", ToJSON(a_material.parameters) },
			{ "textures", { { "rfaos", a_material.rfaos.empty() ? json(nullptr) : json(a_material.rfaos) },
							  { "wetness", a_material.wetness.empty() ? json(nullptr) : json(a_material.wetness) } } } };
	}

	bool IsSurfaceID(std::string_view a_id)
	{
		if (a_id.size() != 36 || a_id == "00000000-0000-0000-0000-000000000000")
			return false;
		for (size_t i = 0; i < a_id.size(); ++i) {
			if (i == 8 || i == 13 || i == 18 || i == 23) {
				if (a_id[i] != '-')
					return false;
			} else if (!std::isxdigit(static_cast<unsigned char>(a_id[i]))) {
				return false;
			}
		}
		return true;
	}

	std::string FormKey::Key() const { return std::format("{}|{:08x}", Lower(plugin), localID); }
	json ToJSON(const FormKey& a_key) { return { { "plugin", a_key.plugin }, { "localFormId", std::format("{:08X}", a_key.localID) } }; }

	FormKey ReadFormKey(const json& a_json)
	{
		Fields(a_json, { "plugin", "localFormId" }, { "plugin", "localFormId" });
		FormKey result;
		result.plugin = Lower(String(a_json.at("plugin")));
		if (result.plugin.find_first_of("/\\:|*?\"<>") != std::string::npos ||
			!(result.plugin.ends_with(".esm") || result.plugin.ends_with(".esp") || result.plugin.ends_with(".esl")))
			throw std::runtime_error("Invalid plugin filename");
		auto id = String(a_json.at("localFormId"), 8);
		auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), result.localID, 16);
		if (error != std::errc{} || end != id.data() + id.size() || !result.localID || result.localID > 0xFFFFFF)
			throw std::runtime_error("Invalid localFormId; load-order prefixes are not allowed");
		return result;
	}

	std::optional<FormKey> GetFormKey(const RE::TESForm* a_form)
	{
		if (!a_form || (a_form->GetFormID() >> 24) == 0xFF)
			return {};
		auto* file = a_form->GetFile(0);
		if (!file || file->compileIndex == 0xFF)
			return {};
		return FormKey{ Lower(std::string(file->GetFilename())), a_form->GetLocalFormID() };
	}

	RE::TESForm* ResolveForm(const FormKey& a_key, RE::FormType a_type)
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler)
			return nullptr;
		auto* file = handler->LookupModByName(a_key.plugin);
		if (!file || file->compileIndex == 0xFF || (file->IsLight() && a_key.localID > 0xFFF))
			return nullptr;
		auto* form = handler->LookupForm(a_key.localID, a_key.plugin);
		return form && form->GetFormType() == a_type && GetFormKey(form) == std::optional(a_key) ? form : nullptr;
	}

	std::string Target::Key() const
	{
		return (!surfaceID.empty() ? "surface:" + surfaceID : txst ? "txst:" + txst->Key() :
																	 "") +
		       (npc ? "@npc:" + npc->Key() : "");
	}

	json ToJSON(const Target& a_target)
	{
		json result = json::object();
		if (!a_target.surfaceID.empty())
			result["surfaceId"] = a_target.surfaceID;
		if (a_target.txst)
			result["txst"] = ToJSON(*a_target.txst);
		if (a_target.npc)
			result["npc"] = ToJSON(*a_target.npc);
		return result;
	}

	NifMaterial ReadNif(RE::BSGeometry* a_geometry)
	{
		NifMaterial result;
		try {
			RE::NiStringExtraData* payload = nullptr;
			for (uint16_t i = 0; i < a_geometry->GetExtraDataSize(); ++i) {
				auto* extra = a_geometry->GetExtraDataAt(i);
				if (!extra || extra->name != "CSSkinMaterial")
					continue;
				if (payload)
					throw std::runtime_error("Duplicate CSSkinMaterial blocks");
				payload = netimmerse_cast<RE::NiStringExtraData*>(extra);
				if (!payload || !payload->value)
					throw std::runtime_error("CSSkinMaterial must be NiStringExtraData");
			}
			if (!payload)
				return result;
			const size_t length = strnlen_s(payload->value, MaxPayloadBytes + 1);
			if (length > MaxPayloadBytes)
				throw std::runtime_error("CSSkinMaterial exceeds 64 KiB");
			const auto data = Parse(std::string_view(payload->value, length));
			Fields(data, { "schemaVersion", "surfaceId", "material" }, { "schemaVersion", "surfaceId", "material" });
			Version(data);
			auto id = Lower(String(data.at("surfaceId"), 36));
			if (!IsSurfaceID(id))
				throw std::runtime_error("Invalid surfaceId");
			auto material = ReadMaterial(data.at("material"));
			result.surfaceID = std::move(id);
			result.material = std::move(material);
		} catch (const std::exception& e) {
			result.diagnostic = e.what();
			logger::warn("[Advanced Skin] {}: {}", a_geometry->name.c_str(), e.what());
		}
		return result;
	}

	void WriteJSON(const std::filesystem::path& a_path, const json& a_json)
	{
		std::filesystem::create_directories(a_path.parent_path());
		auto temporary = a_path;
		temporary += ".tmp";
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			output << a_json.dump(2) << '\n';
			output.flush();
			if (!output)
				throw std::runtime_error("Cannot write " + temporary.string());
		}
		if (!MoveFileExW(temporary.c_str(), a_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			throw std::runtime_error("Cannot replace " + a_path.string());
	}

	void Store::Refresh(bool a_force)
	{
		const auto previousFiles = files;
		bool changed = a_force;
		std::set<std::string> present;
		try {
			std::vector<std::filesystem::path> paths;
			if (std::filesystem::exists(packageDirectory)) {
				for (const auto& entry : std::filesystem::directory_iterator(packageDirectory)) {
					if (entry.is_regular_file() && Lower(entry.path().filename().string()).ends_with(".skin.json"))
						paths.push_back(entry.path());
				}
			}
			if (std::filesystem::exists(userFile))
				paths.push_back(userFile);
			if (paths.size() > 4096)
				throw std::runtime_error("Too many Skin material packages");
			uintmax_t totalBytes = 0;
			for (const auto& path : paths) {
				totalBytes += std::filesystem::file_size(path);
				if (totalBytes > 64 * 1024 * 1024)
					throw std::runtime_error("Skin material files exceed the 64 MiB aggregate limit");
			}
			for (const auto& path : paths) {
				const auto key = path.generic_string();
				present.insert(key);
				const auto time = std::filesystem::last_write_time(path);
				auto found = files.find(key);
				if (!a_force && found != files.end() && found->second.time == time)
					continue;
				changed = true;
				auto& snapshot = files[key];
				snapshot.time = time;
				try {
					auto data = ReadFile(path);
					ValidateRoot(data, path == userFile);
					if (path != userFile && Lower(path.filename().string()) != Lower(String(data.at("ownerPlugin"))) + ".skin.json")
						throw std::runtime_error("Filename must equal ownerPlugin + .skin.json");
					snapshot.data = std::move(data);
					snapshot.error.clear();
				} catch (const std::exception& e) {
					snapshot.error = std::string(e.what()) + (snapshot.data.is_null() ? "; not loaded" : "; previous snapshot retained");
				}
			}
			for (auto it = files.begin(); it != files.end();) {
				if (!present.contains(it->first)) {
					changed = true;
					it = files.erase(it);
				} else {
					++it;
				}
			}
		} catch (const std::exception& e) {
			files = previousFiles;
			logger::warn("[Advanced Skin] Material scan failed: {}", e.what());
			return;
		}
		if (changed)
			Rebuild();
	}

	void Store::Rebuild()
	{
		records.clear();
		races.clear();
		npcs.clear();
		userEdits.clear();
		diagnostics.clear();
		++revision;
		std::vector<std::pair<uint32_t, const FileSnapshot*>> packages;
		auto* handler = RE::TESDataHandler::GetSingleton();
		std::unordered_map<const RE::TESFile*, uint32_t> loadOrder;
		if (handler) {
			uint32_t order = 0;
			// FormID indices split light/full plugins; the record loader traverses this list.
			for (auto* plugin : handler->files) {
				if (plugin && plugin->compileIndex != 0xFF && plugin != handler->activeFile)
					loadOrder.emplace(plugin, order++);
			}
			if (handler->activeFile && handler->activeFile->compileIndex != 0xFF)
				loadOrder.insert_or_assign(handler->activeFile, order);
		}
		for (const auto& [path, file] : files) {
			if (!file.error.empty())
				diagnostics.push_back(path + ": " + file.error);
			if (file.data.is_null())
				continue;
			if (path == userFile.generic_string()) {
				std::set<std::string> seen;
				for (const auto& entry : file.data.at("edits")) {
					try {
						const auto key = ReadTarget(entry.at("target")).Key();
						if (!seen.insert(key).second) {
							userEdits.erase(key);
							throw std::runtime_error("Duplicate user target: " + key);
						}
						auto edit = ReadEdit(entry);
						if ((edit.target.txst && !ResolveForm(*edit.target.txst, RE::FormType::TextureSet)) ||
							(edit.target.npc && !ResolveForm(*edit.target.npc, RE::FormType::NPC)))
							diagnostics.push_back("Unresolved user target: " + key);
						userEdits.emplace(key, std::move(edit));
					} catch (const std::exception& e) {
						diagnostics.push_back(path + ": " + e.what());
					}
				}
			} else if (handler) {
				auto owner = file.data.at("ownerPlugin").get<std::string>();
				auto* plugin = handler->LookupModByName(owner);
				if (!plugin || !loadOrder.contains(plugin))
					diagnostics.push_back(owner + ": owner plugin is not loaded");
				else
					packages.emplace_back(loadOrder.at(plugin), &file);
			}
		}
		std::sort(packages.begin(), packages.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [order, file] : packages) {
			const std::string source = file->data.at("ownerPlugin");
			auto load = [&](const char* a_array, const char* a_target, RE::FormType a_type, auto& a_destination) {
				using Value = typename std::decay_t<decltype(a_destination)>::mapped_type;
				std::unordered_map<RE::FormID, Value> local;
				std::unordered_set<RE::FormID> seen;
				for (const auto& entry : file->data.at(a_array)) {
					try {
						auto key = ReadFormKey(entry.at(a_target));
						auto* form = ResolveForm(key, a_type);
						if (!form)
							throw std::runtime_error("Missing or wrong-type target: " + key.Key());
						const auto id = form->GetFormID();
						if (!seen.insert(id).second) {
							local.erase(id);
							throw std::runtime_error("Duplicate target: " + key.Key());
						}
						if constexpr (std::is_same_v<Value, RecordMaterial>) {
							Fields(entry, { a_target, "material" }, { a_target, "material" });
							local.emplace(id, Value{ ReadMaterial(entry.at("material")), source });
						} else {
							Fields(entry, { a_target, "parameters" }, { a_target, "parameters" });
							local.emplace(id, Value{ ReadParameters(entry.at("parameters"), false), source });
						}
					} catch (const std::exception& e) {
						diagnostics.push_back(source + "/" + a_array + ": " + e.what());
					}
				}
				for (auto& [id, value] : local)
					a_destination.insert_or_assign(id, std::move(value));
			};
			load("recordMaterials", "txst", RE::FormType::TextureSet, records);
			load("raceAdjustments", "race", RE::FormType::Race, races);
			load("npcAdjustments", "npc", RE::FormType::NPC, npcs);
		}
		for (const auto& message : diagnostics)
			logger::warn("[Advanced Skin] {}", message);
	}

	const UserEdit* Store::FindEdit(const Target& a_target) const
	{
		auto it = userEdits.find(a_target.Key());
		return it == userEdits.end() ? nullptr : &it->second;
	}

	Resolution Store::Resolve(const NifMaterial& a_nif, RE::FormID a_txst, RE::FormID a_race,
		RE::FormID a_npc, const Target& a_surface, const Parameters& a_default, const UserEdit* a_preview) const
	{
		Resolution result{ Material{ "Default", a_default, {}, {} }, a_default, "Default", {} };
		if (a_nif.material) {
			result.base = *a_nif.material;
			result.source = "NIF";
		}
		if (auto record = records.find(a_txst); record != records.end()) {
			result.base = record->second.material;
			result.source = record->second.source;
		}
		Target npc;
		npc.npc = GetFormKey(RE::TESForm::LookupByID(a_npc));
		Target combined = a_surface;
		combined.npc = npc.npc;
		auto findEdit = [&](const Target& a_target) {
			return a_preview && a_preview->target.Key() == a_target.Key() ? a_preview : FindEdit(a_target);
		};
		const UserEdit* surfaceEdit = a_surface.HasSurface() ? findEdit(a_surface) : nullptr;
		const UserEdit* npcEdit = npc.npc ? findEdit(npc) : nullptr;
		const UserEdit* combinedEdit = a_surface.HasSurface() && npc.npc ? findEdit(combined) : nullptr;
		for (auto* edit : { surfaceEdit, combinedEdit }) {
			if (edit && edit->material) {
				result.base = *edit->material;
				result.source = (edit == a_preview ? "Preview: " : "User: ") + edit->target.Key();
			}
		}
		const bool isolatedPreview = a_preview && !a_preview->target.HasSurface() && !a_preview->target.npc;
		if (isolatedPreview && a_preview->material) {
			result.base = *a_preview->material;
			result.source = "Preview";
		}
		result.parameters = result.base.parameters;
		auto adjust = [&](const auto& a_map, RE::FormID a_id) {
			if (auto it = a_map.find(a_id); it != a_map.end()) {
				Apply(result.parameters, it->second.parameters);
				result.adjustments.push_back(it->second.source + std::format(" [{:08X}]", a_id));
			}
		};
		adjust(races, a_race);
		adjust(npcs, a_npc);
		for (auto* edit : { surfaceEdit, npcEdit, combinedEdit }) {
			if (edit) {
				Apply(result.parameters, edit->parameters);
				result.adjustments.push_back((edit == a_preview ? "Preview: " : "User: ") + edit->target.Key());
			}
		}
		if (isolatedPreview) {
			Apply(result.parameters, a_preview->parameters);
			result.adjustments.push_back("Preview");
		}
		return result;
	}

	void Store::SaveUsers(const std::map<std::string, UserEdit>& a_edits)
	{
		json entries = json::array();
		for (const auto& [key, edit] : a_edits) {
			json entry = { { "target", ToJSON(edit.target) }, { "parameters", ToJSON(edit.parameters) } };
			if (edit.material)
				entry["material"] = ToJSON(*edit.material);
			ReadEdit(entry);
			entries.push_back(std::move(entry));
		}
		if (entries.size() > MaxEntries)
			throw std::runtime_error("Too many user edits");
		json data = { { "schemaVersion", 1 }, { "edits", std::move(entries) } };
		if (data.dump(2).size() > MaxPackageBytes)
			throw std::runtime_error("User edits exceed 4 MiB");
		WriteJSON(userFile, data);
		Refresh(true);
	}

	void Store::SaveEdit(const UserEdit& a_edit)
	{
		auto copy = userEdits;
		copy.insert_or_assign(a_edit.target.Key(), a_edit);
		SaveUsers(copy);
	}

	void Store::DeleteEdit(const Target& a_target)
	{
		auto copy = userEdits;
		copy.erase(a_target.Key());
		SaveUsers(copy);
	}

	void Store::RemoveEdit(const Target& a_target, bool a_material)
	{
		auto copy = userEdits;
		if (auto it = copy.find(a_target.Key()); it != copy.end()) {
			if (a_material)
				it->second.material.reset();
			else
				it->second.parameters = {};
			if (!it->second.material && ToJSON(it->second.parameters).empty())
				copy.erase(it);
		}
		SaveUsers(copy);
	}
}
