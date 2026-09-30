#include "ActorState.h"

#include "../Editor/Files.h"
#include "Utils/FileSystem.h"

#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>

namespace SkinActors
{
	std::array<const char*, 4> PartNames()
	{
		return { T("feature.skin.face", "Face"), T("feature.skin.body", "Body"),
			T("feature.skin.hands", "Hands"), T("feature.skin.feet", "Feet") };
	}
	namespace
	{
		constexpr size_t MaxBytes = 64 * 1024 * 1024;
		constexpr size_t MaxLayout = 4 * 1024 * 1024;
		constexpr size_t MaxActors = 1024;
		constexpr size_t MaxSurfaces = 32;
		std::mutex mutex;
		std::shared_ptr<const State> state = std::make_shared<State>();
		std::atomic<uint64_t> revision{ 1 }, session{ 1 };
		std::optional<std::vector<uint8_t>> disk;
		std::optional<std::vector<uint8_t>> locationDisk;

		[[noreturn]] void Invalid()
		{
			throw std::runtime_error(T("feature.skin.invalid_character_file", "Invalid or unsupported Skin character file."));
		}
		std::filesystem::path DefaultDirectory()
		{
			return Util::PathHelpers::GetCommunityShaderPath() / "AdvancedSkin";
		}
		std::optional<std::vector<uint8_t>> Read(const std::filesystem::path& path)
		{
			if (!std::filesystem::exists(path))
				return std::nullopt;
			const auto size = std::filesystem::file_size(path);
			if (size > MaxBytes)
				Invalid();
			std::vector<uint8_t> bytes(static_cast<size_t>(size));
			std::ifstream input(path, std::ios::binary);
			if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
				throw std::runtime_error(T("feature.skin.cannot_read_character_file", "Cannot read Skin character configuration."));
			return bytes;
		}
		json Parse(const std::vector<uint8_t>& bytes)
		{
			return json::parse(bytes.begin(), bytes.end(), [](int depth, json::parse_event_t, json&) {
				if (depth > 32)
					Invalid();
				return true;
			});
		}
		std::vector<uint8_t> Bytes(const json& value)
		{
			const auto text = value.dump(2);
			if (text.size() > MaxBytes)
				Invalid();
			return { text.begin(), text.end() };
		}
		bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			return _wcsicmp(std::filesystem::weakly_canonical(a).c_str(), std::filesystem::weakly_canonical(b).c_str()) == 0;
		}
		void CheckDestination(const std::filesystem::path& path, bool scheme)
		{
			if (SamePath(path, DefaultDirectory() / "Location.json") ||
				(scheme && SamePath(path, state->path)))
				throw std::runtime_error(T("feature.skin.reserved_character_file", "Choose a separate file; this path is used by the active character configuration."));
		}
		template <class Value>
		Value Unsigned(const json& value)
		{
			if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<Value>::max())
				Invalid();
			return value.get<Value>();
		}
		void Array(const json& value, size_t size)
		{
			if (!value.is_array() || value.size() != size)
				Invalid();
		}
		std::string Text(const json& value, size_t limit = 1024)
		{
			auto result = value.get<std::string>();
			if (result.size() > limit || result.find('\0') != std::string::npos)
				Invalid();
			return result;
		}
		void Write(const std::filesystem::path& path, const json& value, const std::optional<std::vector<uint8_t>>& expected)
		{
			const auto bytes = Bytes(value);
			if (Parse(bytes) != value || Read(path) != expected)
				throw std::runtime_error(T("feature.skin.configuration_conflict", "The configuration changed on disk. Reopen it before saving."));
			const SkinEditor::Output output{ path, bytes };
			SkinEditor::Publish(std::span(&output, 1));
			if (Read(path) != std::optional(bytes))
				throw std::runtime_error(T("feature.skin.configuration_readback_failed", "Configuration read-back failed. Check the displayed output location."));
		}
		json Encode(const SkinMaterials::Changes& changes)
		{
			json values = json::array(), textures = json::array();
			for (const auto& value : changes.parameters)
				values.push_back(value ? json(*value) : json(nullptr));
			for (const auto& texture : changes.textures)
				textures.push_back({ { "mode", static_cast<uint32_t>(texture.mode) }, { "path", texture.path } });
			return { { "parameters", values }, { "textures", textures } };
		}
		SkinMaterials::Changes DecodeChanges(const json& value)
		{
			SkinMaterials::Changes result;
			const auto& numbers = value.at("parameters");
			const auto& textures = value.at("textures");
			if (!numbers.is_array() || numbers.size() != result.parameters.size() || !textures.is_array() || textures.size() != result.textures.size())
				Invalid();
			for (size_t i = 0; i < numbers.size(); ++i)
				if (!numbers[i].is_null())
					result.parameters[i] = numbers[i].get<float>();
			for (size_t i = 0; i < textures.size(); ++i) {
				result.textures[i].mode = static_cast<SkinMaterials::TextureMode>(Unsigned<uint32_t>(textures[i].at("mode")));
				result.textures[i].path = SkinMaterials::NormalizeTexturePath(Text(textures[i].at("path")));
			}
			SkinMaterials::Validate(result);
			return result;
		}
		bool ValidLayout(const std::string& layout)
		{
			return !layout.empty() && layout.size() <= MaxLayout && layout.size() % 2 == 0 &&
			       std::all_of(layout.begin(), layout.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
		}
		bool ValidTransform(const std::array<float, 4>& values)
		{
			return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
		}
		void Validate(Part part, const Guard& guard)
		{
			if (!guard.resolved || !PersistentKey(guard.race) || guard.sex > 1 || guard.materialBlock == UINT32_MAX ||
				!ValidLayout(guard.layout) || guard.source.empty() || guard.source.size() > 1024 || !ValidTransform(guard.uvTransform) ||
				(part == Part::Face ? !PersistentKey(guard.headPart) || !guard.armor.empty() || !guard.addon.empty() :
									  !guard.headPart.empty() || !PersistentKey(guard.armor) || !PersistentKey(guard.addon)))
				Invalid();
		}
		json Encode(const Guard& guard)
		{
			return { { "race", guard.race }, { "headPart", guard.headPart }, { "armor", guard.armor }, { "addon", guard.addon },
				{ "sex", guard.sex }, { "block", guard.materialBlock }, { "firstPerson", guard.firstPerson },
				{ "layout", guard.layout }, { "source", guard.source }, { "baseTextures", guard.baseTextures },
				{ "resources", guard.resourceRevisions }, { "uv", guard.uvTransform } };
		}
		Guard DecodeGuard(const json& value, Part part)
		{
			Guard guard;
			guard.race = Text(value.at("race"));
			guard.headPart = Text(value.at("headPart"));
			guard.armor = Text(value.at("armor"));
			guard.addon = Text(value.at("addon"));
			guard.sex = Unsigned<uint32_t>(value.at("sex"));
			guard.materialBlock = Unsigned<uint32_t>(value.at("block"));
			guard.firstPerson = value.at("firstPerson").get<bool>();
			guard.layout = Text(value.at("layout"), MaxLayout);
			guard.source = Text(value.at("source"));
			Array(value.at("baseTextures"), guard.baseTextures.size());
			Array(value.at("resources"), guard.resourceRevisions.size());
			Array(value.at("uv"), guard.uvTransform.size());
			guard.baseTextures = value.at("baseTextures").get<decltype(guard.baseTextures)>();
			for (auto& path : guard.baseTextures) {
				if (path.size() > 1024)
					Invalid();
				path = SkinMaterials::NormalizeTexturePath(path);
			}
			for (size_t i = 0; i < guard.resourceRevisions.size(); ++i)
				guard.resourceRevisions[i] = Unsigned<uint64_t>(value.at("resources")[i]);
			guard.uvTransform = value.at("uv").get<decltype(guard.uvTransform)>();
			Validate(part, guard);
			return guard;
		}
		json Encode(const State& value)
		{
			json actors = json::array();
			for (const auto& [key, binding] : value.actors) {
				if (!binding.persistent)
					continue;
				json parts = json::array();
				for (const auto& part : binding.parts) {
					json surfaces = json::array();
					for (const auto& surface : part)
						surfaces.push_back({ { "changes", Encode(surface.changes) }, { "guard", Encode(surface.guard) } });
					parts.push_back(std::move(surfaces));
				}
				actors.push_back({ { "target", key }, { "name", binding.name }, { "parts", parts } });
			}
			return { { "format", "CommunityShaders.Skin.Characters" }, { "version", 1 }, { "skinVersion", SkinMaterials::Version }, { "actors", actors } };
		}
		void Header(const json& value, const char* format)
		{
			if (value.at("format") != format || value.at("version") != 1 || value.at("skinVersion") != SkinMaterials::Version)
				Invalid();
		}
		void Decode(State& next, const json& value)
		{
			Header(value, "CommunityShaders.Skin.Characters");
			const auto& actors = value.at("actors");
			if (!actors.is_array() || actors.size() > MaxActors)
				Invalid();
			for (const auto& actor : actors) {
				const auto key = Text(actor.at("target"));
				if (key != "player" && !PersistentKey(key))
					Invalid();
				Binding binding;
				binding.name = Text(actor.at("name"));
				const auto& parts = actor.at("parts");
				if (!parts.is_array() || parts.size() != binding.parts.size())
					Invalid();
				for (size_t i = 0; i < parts.size(); ++i) {
					if (!parts[i].is_array() || parts[i].size() > MaxSurfaces)
						Invalid();
					for (const auto& surface : parts[i]) {
						auto guard = DecodeGuard(surface.at("guard"), static_cast<Part>(i));
						if (Find(&binding, static_cast<Part>(i), guard))
							Invalid();
						binding.parts[i].push_back({ DecodeChanges(surface.at("changes")), std::move(guard) });
					}
				}
				if (!next.actors.emplace(key, std::move(binding)).second)
					Invalid();
			}
		}
		void Publish(State next)
		{
			next.revision = state->revision + 1;
			state = std::make_shared<State>(std::move(next));
			revision.store(state->revision);
			session.store(state->session);
		}
		void CheckRevision(uint64_t expectedRevision, uint64_t expectedSession)
		{
			if (state->revision != expectedRevision || state->session != expectedSession)
				throw std::runtime_error(T("feature.skin.character_changed", "The character configuration or game session changed. Select the current target again."));
		}
		void Submit(State next)
		{
			if (!state->available)
				throw std::runtime_error(T("feature.skin.character_storage_unavailable", "Character storage is unavailable. Reopen a valid configuration or recover a backup."));
			const auto value = Encode(next);
			Write(state->path, value, disk);
			disk = Bytes(value);
			next.message = T("feature.skin.character_saved", "Character configuration saved. Loading a game does not roll it back.");
			Publish(std::move(next));
		}
		struct MenuEvents : RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (event && event->opening && event->menuName == RE::MainMenu::MENU_NAME)
					Reset();
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	std::string FormKey(const RE::TESForm* form)
	{
		if (!form || !form->GetFile(0) || (form->GetFormID() >> 24) == 0xff)
			return {};
		std::string plugin(form->GetFile(0)->GetFilename());
		for (auto& c : plugin)
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
		return std::format("{:x}~{}", form->GetLocalFormID(), plugin);
	}
	bool PersistentKey(const std::string& key)
	{
		const auto split = key.find('~');
		if (split == std::string::npos || split == 0 || split > 6 || split + 1 == key.size() || key.size() > 1024)
			return false;
		return std::all_of(key.begin(), key.begin() + split, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) &&
		       key.find_first_of("/\\:\0", split + 1, 4) == std::string::npos;
	}
	std::string ActorKey(RE::Actor* actor)
	{
		if (!actor)
			return {};
		if (actor == RE::PlayerCharacter::GetSingleton())
			return "player";
		const auto key = FormKey(actor);
		return key.empty() ? std::format("session:{}:{}", Session(), actor->GetHandle().native_handle()) : key;
	}
	std::filesystem::path Directory()
	{
		return Get()->path.empty() ? DefaultDirectory() : Get()->path.parent_path();
	}
	void Install()
	{
		std::lock_guard lock(mutex);
		State next;
		next.path = DefaultDirectory() / "State.json";
		try {
			locationDisk = Read(DefaultDirectory() / "Location.json");
			if (locationDisk)
				next.path = std::filesystem::u8path(Text(Parse(*locationDisk).at("path"), 32768));
			next.path = std::filesystem::absolute(next.path).lexically_normal();
			disk = Read(next.path);
			if (disk)
				Decode(next, Parse(*disk));
			next.available = true;
		} catch (const std::exception& e) {
			next.actors.clear();
			next.message = e.what();
			logger::error("[Advanced Skin] {}", next.message);
		}
		Publish(std::move(next));
	}
	void InstallLifecycle()
	{
		static MenuEvents events;
		if (auto* ui = RE::UI::GetSingleton())
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&events);
	}
	Snapshot Get()
	{
		std::lock_guard lock(mutex);
		return state;
	}
	uint64_t Revision() { return revision.load(); }
	uint64_t Session() { return session.load(); }
	const Binding* Find(const State& value, const std::string& key)
	{
		const auto found = value.actors.find(key);
		return found == value.actors.end() ? nullptr : &found->second;
	}
	const Surface* Find(const Binding* binding, Part part, const Guard& guard)
	{
		if (!binding || part >= Part::Count || !guard.resolved)
			return nullptr;
		const auto& surfaces = binding->parts[static_cast<size_t>(part)];
		const auto found = std::find_if(surfaces.begin(), surfaces.end(), [&](const auto& surface) { return surface.guard == guard; });
		return found == surfaces.end() ? nullptr : &*found;
	}
	void Commit(const std::string& key, Binding binding, uint64_t expectedRevision, uint64_t expectedSession)
	{
		binding.persistent = !key.starts_with("session:");
		if (key.empty() || binding.name.size() > 1024 || (binding.persistent && key != "player" && !PersistentKey(key)))
			Invalid();
		for (size_t i = 0; i < binding.parts.size(); ++i) {
			const auto& part = binding.parts[i];
			if (part.size() > MaxSurfaces)
				Invalid();
			for (const auto& surface : part) {
				if (std::count_if(part.begin(), part.end(), [&](const auto& other) { return surface.guard == other.guard; }) != 1)
					Invalid();
				SkinMaterials::Validate(surface.changes);
				Validate(static_cast<Part>(i), surface.guard);
			}
		}
		std::lock_guard lock(mutex);
		CheckRevision(expectedRevision, expectedSession);
		State next = *state;
		const auto old = Find(*state, key);
		next.undo[key] = old ? *old : Binding{};
		if (std::all_of(binding.parts.begin(), binding.parts.end(), [](const auto& part) { return part.empty(); }))
			next.actors.erase(key);
		else
			next.actors[key] = std::move(binding);
		if (next.actors.size() > MaxActors)
			Invalid();
		if (key.starts_with("session:")) {
			next.message = T("feature.skin.character_session_only", "Applied for this game session only. This actor has no persistent plugin reference.");
			Publish(std::move(next));
		} else {
			Submit(std::move(next));
		}
	}
	void Clear(const std::string& key, Part part, uint64_t expectedRevision, uint64_t expectedSession)
	{
		const auto current = Get();
		const auto old = Find(*current, key);
		Binding next = old ? *old : Binding{};
		next.parts.at(static_cast<size_t>(part)).clear();
		Commit(key, std::move(next), expectedRevision, expectedSession);
	}
	void Undo(const std::string& key, uint64_t expectedRevision, uint64_t expectedSession)
	{
		const auto current = Get();
		const auto found = current->undo.find(key);
		if (found == current->undo.end())
			return;
		Commit(key, found->second, expectedRevision, expectedSession);
	}
	void Reset()
	{
		std::lock_guard lock(mutex);
		State next = *state;
		++next.session;
		next.undo.clear();
		std::erase_if(next.actors, [](const auto& actor) { return !actor.second.persistent; });
		Publish(std::move(next));
	}
	void OpenStorage(const std::filesystem::path& path, bool copy, bool empty)
	{
		std::lock_guard lock(mutex);
		State next;
		next.session = state->session + 1;
		next.path = std::filesystem::absolute(path).lexically_normal();
		CheckDestination(next.path, false);
		auto bytes = Read(next.path);
		if (copy) {
			if (!state->available && !empty)
				throw std::runtime_error(T("feature.skin.character_storage_unavailable", "Character storage is unavailable. Reopen a valid configuration or recover a backup."));
			if (!empty)
				next.actors = state->actors;
			std::erase_if(next.actors, [](const auto& actor) { return !actor.second.persistent; });
			const auto value = Encode(next);
			Write(next.path, value, bytes);
			bytes = Bytes(value);
		} else if (bytes) {
			Decode(next, Parse(*bytes));
		} else {
			throw std::runtime_error(T("feature.skin.cannot_read_character_file", "Cannot read Skin character configuration."));
		}
		const json location{ { "path", SkinEditor::UTF8(next.path) } };
		Write(DefaultDirectory() / "Location.json", location, Read(DefaultDirectory() / "Location.json"));
		locationDisk = Bytes(location);
		disk = std::move(bytes);
		next.available = true;
		next.message = T("feature.skin.character_configuration_opened", "Character configuration opened.");
		Publish(std::move(next));
	}
	void Recover()
	{
		std::lock_guard lock(mutex);
		std::vector<std::filesystem::directory_entry> candidates;
		if (std::filesystem::exists(state->path.parent_path()))
			for (const auto& file : std::filesystem::directory_iterator(state->path.parent_path())) {
				const auto name = file.path().filename().wstring();
				if (file.is_regular_file() && name.starts_with(state->path.filename().wstring() + L".cs-skin") && name.ends_with(L".bak"))
					candidates.push_back(file);
			}
		std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.last_write_time() > b.last_write_time(); });
		for (const auto& file : candidates) {
			State next;
			next.path = state->path;
			next.session = state->session + 1;
			try {
				const auto backup = Read(file.path());
				if (!backup)
					continue;
				Decode(next, Parse(*backup));
			} catch (const std::exception&) {
				continue;
			}
			const auto value = Encode(next);
			Write(next.path, value, Read(next.path));
			disk = Bytes(value);
			next.available = true;
			next.message = T("feature.skin.character_backup_restored", "The latest valid character configuration backup was restored.");
			Publish(std::move(next));
			return;
		}
		throw std::runtime_error(T("feature.skin.no_character_backup", "No valid character configuration backup was found."));
	}
	SkinMaterials::Changes Capture(const SkinMaterials::Material& material)
	{
		SkinMaterials::Validate(material);
		SkinMaterials::Changes result;
		for (size_t i = 0; i < result.parameters.size(); ++i)
			result.parameters[i] = material.parameters.values[i];
		for (size_t i = 0; i < result.textures.size(); ++i)
			result.textures[i] = { material.textures[i].empty() ? SkinMaterials::TextureMode::Default : SkinMaterials::TextureMode::Resource, material.textures[i] };
		return result;
	}
	bool Compatible(const Sample& sample, const Guard& guard)
	{
		return guard.resolved && sample.firstPerson == guard.firstPerson && !sample.layout.empty() &&
		       sample.layout == guard.layout && sample.uvTransform == guard.uvTransform;
	}
	void SaveScheme(const std::filesystem::path& path, const Scheme& scheme)
	{
		std::lock_guard lock(mutex);
		CheckDestination(path, true);
		json parts = json::array();
		for (size_t i = 0; i < scheme.parts.size(); ++i) {
			if (scheme.parts[i].size() > MaxSurfaces || (scheme.selected[i] && scheme.parts[i].empty()))
				throw std::runtime_error(T("feature.skin.scheme_part_unavailable", "A selected source part is unavailable or paused. Restore its appearance before saving the scheme."));
			json samples = json::array();
			for (const auto& sample : scheme.parts[i])
				samples.push_back({ { "material", Encode(Capture(sample.material)) }, { "layout", sample.layout },
					{ "uv", sample.uvTransform }, { "firstPerson", sample.firstPerson } });
			parts.push_back({ { "selected", scheme.selected[i] }, { "surfaces", samples } });
		}
		const json value{ { "format", "CommunityShaders.Skin.Scheme" }, { "version", 1 }, { "skinVersion", SkinMaterials::Version }, { "parts", parts } };
		Write(path, value, Read(path));
	}
	Scheme LoadScheme(const std::filesystem::path& path)
	{
		const auto bytes = Read(path);
		if (!bytes)
			Invalid();
		const auto value = Parse(*bytes);
		Header(value, "CommunityShaders.Skin.Scheme");
		Scheme result;
		const auto& parts = value.at("parts");
		if (!parts.is_array() || parts.size() != result.parts.size())
			Invalid();
		for (size_t i = 0; i < parts.size(); ++i) {
			result.selected[i] = parts[i].at("selected").get<bool>();
			const auto& samples = parts[i].at("surfaces");
			if (!samples.is_array() || samples.size() > MaxSurfaces)
				Invalid();
			for (const auto& value : samples) {
				Sample sample;
				const auto changes = DecodeChanges(value.at("material"));
				if (std::any_of(changes.parameters.begin(), changes.parameters.end(), [](const auto& v) { return !v; }) ||
					std::any_of(changes.textures.begin(), changes.textures.end(), [](const auto& t) { return t.mode == SkinMaterials::TextureMode::Inherit; }))
					Invalid();
				sample.material = SkinMaterials::Apply({}, changes);
				sample.layout = Text(value.at("layout"), MaxLayout);
				Array(value.at("uv"), sample.uvTransform.size());
				sample.uvTransform = value.at("uv").get<decltype(sample.uvTransform)>();
				sample.firstPerson = value.at("firstPerson").get<bool>();
				if (!ValidLayout(sample.layout) || !ValidTransform(sample.uvTransform))
					Invalid();
				result.parts[i].push_back(std::move(sample));
			}
		}
		return result;
	}
}
