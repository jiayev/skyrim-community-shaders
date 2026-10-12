#include "Features/Skin.h"

#include "Globals.h"
#include "State.h"

#include <imgui_stdlib.h>
#include <set>
#include <shellapi.h>

using namespace SkinMaterials;
using namespace SkinEditor;

namespace
{
	struct ScopeExit
	{
		std::function<void()> leave;
		~ScopeExit() { leave(); }
	};
	Changes ToChanges(const SkinMaterials::Material& a_material)
	{
		Changes result;
		if (!a_material.enabled)
			return result;
		for (size_t i = 0; i < ParameterCount; ++i)
			if (a_material.specified[i])
				result.parameters[i] = a_material.parameters.values[i];
		for (size_t i = 0; i < TextureCount; ++i)
			if (!a_material.textures[i].empty())
				result.textures[i] = { TextureMode::Resource, a_material.textures[i] };
		return result;
	}
	// Accepts the mod folder itself, its textures folder, or any folder below textures.
	std::filesystem::path ModRoot(std::filesystem::path a_folder)
	{
		for (auto current = a_folder; current.has_parent_path() && current != current.parent_path(); current = current.parent_path())
			if (_wcsicmp(current.filename().c_str(), L"textures") == 0)
				return current.parent_path();
		return a_folder;
	}
	const char* OriginLabel(SkinLibrary::Origin a_origin)
	{
		switch (a_origin) {
		case SkinLibrary::Origin::Loose:
			return T("feature.skin.material_origin_loose", "Material file (loose)");
		case SkinLibrary::Origin::Archive:
			return T("feature.skin.material_origin_archive", "Material file (archive)");
		case SkinLibrary::Origin::Session:
			return T("feature.skin.material_origin_session", "Material file saved in this session");
		default:
			return T("feature.skin.material_origin_none", "No material file - common settings");
		}
	}
}

bool Skin::MaterialDirty() const
{
	return !editor.draftKey.empty() && editor.material != editor.materialBaseline;
}

bool Skin::CharacterDirty() const
{
	return editor.draftTarget && editor.character != editor.characterBaseline;
}

bool Skin::EditorDirty() const
{
	return MaterialDirty() || CharacterDirty();
}

void Skin::ChangeEditor(bool a_discards, std::function<void()> a_action)
{
	if (a_discards && !editor.discardConfirmed)
		editor.pendingAction = std::move(a_action);
	else
		a_action();
}

void Skin::SelectActor(RE::Actor* a_actor)
{
	if (!a_actor || !a_actor->Get3D(false))
		throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
	const auto handle = a_actor->GetHandle();
	ChangeEditor(CharacterDirty() && !(SkinCharacters::TargetOf(a_actor) == editor.draftTarget), [this, handle] {
		const auto actor = handle.get();
		if (!actor)
			throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
		editor.actor = handle;
		editor.actorName = actor->GetName() ? actor->GetName() : "";
		editor.draftTarget = SkinCharacters::TargetOf(actor.get());
		const auto* saved = editor.draftTarget ? SkinCharacters::Find(*SkinCharacters::Get(), editor.draftTarget.key) : nullptr;
		editor.character = editor.characterBaseline = saved ? saved->customization : SkinCharacters::Customization{};
		if (!saved)
			editor.character.name = editor.characterBaseline.name = editor.actorName;
		editor.undo.clear();
		editor.redo.clear();
		RefreshSurfaces();
		const bool shown = std::any_of(editor.surfaces.begin(), editor.surfaces.end(), [this](const auto& surface) { return surface.key == editor.key; });
		if (!shown && !MaterialDirty()) {
			// Prefer the body: it is usually what an author is working on and what armor shares.
			const auto body = std::find_if(editor.surfaces.begin(), editor.surfaces.end(), [](const auto& surface) { return !surface.key.empty() && surface.description.part == SkinSurfaces::Part::Body; });
			const auto any = std::find_if(editor.surfaces.begin(), editor.surfaces.end(), [](const auto& surface) { return !surface.key.empty(); });
			const auto chosen = body != editor.surfaces.end() ? body : any;
			if (chosen != editor.surfaces.end()) {
				editor.key = chosen->key;
				LoadMaterialDraft(chosen->key);
			}
		}
		editor.message.clear();
		Invalidate();
	});
}

void Skin::RefreshSurfaces()
{
	editor.lastRefresh = globals::state->frameCount;
	const auto actor = editor.actor.get();
	if (!actor) {
		editor.surfaces.clear();
		return;
	}
	std::vector<Editor::Surface> surfaces;
	std::set<RE::NiAVObject*> roots;
	for (bool first : { false, true }) {
		if (first && actor.get() != RE::PlayerCharacter::GetSingleton())
			continue;
		auto* root = actor->Get3D(first);
		if (!root || !roots.insert(root).second)
			continue;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (IsCompatible(property)) {
				const auto& entry = Observe(geometry, property);
				surfaces.push_back({ RE::NiPointer(geometry), entry.key, SkinSurfaces::Describe(geometry, actor.get()) });
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}
	std::stable_sort(surfaces.begin(), surfaces.end(), [](const auto& a, const auto& b) {
		return std::tie(a.description.firstPerson, a.description.part) < std::tie(b.description.firstPerson, b.description.part);
	});
	editor.surfaces = std::move(surfaces);
}

void Skin::SelectKey(const std::string& a_key)
{
	if (a_key.empty() || (a_key == editor.key && a_key == editor.draftKey))
		return;
	ChangeEditor(MaterialDirty() && a_key != editor.draftKey, [this, a_key] {
		editor.key = a_key;
		if (a_key != editor.draftKey)
			LoadMaterialDraft(a_key);
	});
}

void Skin::LoadMaterialDraft(const std::string& a_key)
{
	const auto entry = FindMaterial(a_key);
	editor.draftKey = a_key;
	editor.material = editor.materialBaseline = entry->definition.status == Status::Valid ? ToChanges(entry->definition.material) : Changes{};
	editor.recordFingerprint = entry->definition.status != Status::Valid || entry->definition.authoredFor.has_value();
	editor.fingerprintWarning.clear();
	if (entry->definition.authoredFor) {
		try {
			if (ResourceFingerprint(a_key) != *entry->definition.authoredFor)
				editor.fingerprintWarning = T("feature.skin.material_authored_for_other_texture", "This material was made for a different version of this texture. Its painted masks may not line up; check the result or save again to accept it.");
		} catch (const std::exception& e) {
			editor.fingerprintWarning = e.what();
		}
	}
	editor.undo.clear();
	editor.redo.clear();
	editor.thumbnailKey.clear();
	Invalidate();
}

void Skin::SaveMaterial(const std::filesystem::path& a_root)
{
	if (editor.draftKey.empty())
		throw std::runtime_error(T("feature.skin.select_skin_texture", "Select a skin texture first."));
	Validate(editor.material);
	const auto material = Apply({}, editor.material);
	CheckTextures(material);
	std::optional<SkinMaterials::Fingerprint> fingerprint;
	if (editor.recordFingerprint) {
		// Advisory data only: a diffuse that cannot be read back must not block saving the material.
		try {
			fingerprint = ResourceFingerprint(editor.draftKey);
		} catch (const std::exception& e) {
			logger::warn("[Advanced Skin Editor] {} was not fingerprinted: {}", editor.draftKey, e.what());
		}
	}
	const auto text = Encode(material, fingerprint).dump(2) + "\n";
	const Bytes bytes(text.begin(), text.end());
	const auto relative = SidecarPath(editor.draftKey);
	const auto destination = a_root / std::filesystem::u8path(relative);
	const Output output{ destination, bytes };
	Publish(std::span(&output, 1), false);
	SkinLibrary::Entry entry;
	entry.key = editor.draftKey;
	entry.path = relative;
	entry.definition = Parse(bytes);
	entry.origin = SkinLibrary::Origin::Session;
	entry.savedTo = UTF8(destination);
	library[editor.draftKey] = std::make_shared<const SkinLibrary::Entry>(std::move(entry));
	editor.materialBaseline = editor.material;
	editor.fingerprintWarning.clear();
	editor.message = I18n::GetSingleton()->Format("feature.skin.material_saved", { { "path", UTF8(destination) } },
		"Saved {path}. It applies to everything showing this texture. If another mod provides the same file with higher priority, the game will use that one after Reload skin resources or a restart.");
	Invalidate();
}

void Skin::SaveCharacter()
{
	if (!editor.draftTarget)
		throw std::runtime_error(T("feature.skin.character_no_identity", "This character was generated at runtime and has no record to save adjustments for."));
	if (editor.character.name.empty())
		editor.character.name = editor.actorName;
	std::erase_if(editor.character.materials, [](const auto& item) { return item.second.Empty(); });
	const bool removed = editor.character.Empty();
	const auto file = SkinCharacters::Save(editor.draftTarget, editor.character);
	editor.characterBaseline = editor.character;
	editor.message = removed ? I18n::GetSingleton()->Format("feature.skin.character_file_removed", { { "path", UTF8(file) } }, "No adjustments left; removed {path}.") :
	                           I18n::GetSingleton()->Format("feature.skin.character_saved_file", { { "path", UTF8(file) } }, "Saved {path}. It applies in every save and can be edited or shared like any other file.");
	Invalidate();
}

void Skin::DrawEditorHeader()
{
	using Workspace = Editor::Workspace;
	const char* workspaces[] = { T("feature.skin.workspace_material", "Skin texture materials"), T("feature.skin.workspace_character_v2", "This character"), T("feature.skin.workspace_textures", "Texture tools") };
	int workspace = static_cast<int>(editor.workspace);
	float workspaceWidth = 0;
	for (const auto* label : workspaces)
		workspaceWidth = std::max(workspaceWidth, ImGui::CalcTextSize(label).x + ImGui::GetStyle().CellPadding.x * 2 + ImGui::GetStyle().ItemSpacing.x);
	if (ImGui::GetContentRegionAvail().x < workspaceWidth * 3) {
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::Combo("##skin_workspace", &workspace, workspaces, 3);
	} else if (ImGui::BeginTable("##skin_workspaces", 3, ImGuiTableFlags_SizingStretchSame)) {
		for (int i = 0; i < 3; ++i) {
			ImGui::TableNextColumn();
			if (ImGui::Selectable(workspaces[i], workspace == i))
				workspace = i;
		}
		ImGui::EndTable();
	}
	if (workspace != static_cast<int>(editor.workspace)) {
		editor.workspace = static_cast<Workspace>(workspace);
		if (editor.workspace != Workspace::Textures)
			editor.returnWorkspace = editor.workspace;
	}
	ImGui::Separator();
	if (!editor.error.empty()) {
		ImGui::TextWrapped("%s", editor.error.c_str());
		if (ImGui::SmallButton(T("feature.skin.dismiss_error", "Dismiss error")))
			editor.error.clear();
	} else if (!editor.message.empty()) {
		ImGui::TextWrapped("%s", editor.message.c_str());
	}
}

void Skin::DrawTargets()
{
	if (ImGui::Button(T("feature.skin.player", "Player")))
		SelectActor(RE::PlayerCharacter::GetSingleton());
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.console_selection", "Console selection"))) {
		const auto reference = RE::Console::GetSelectedRef();
		auto* actor = reference ? reference->As<RE::Actor>() : nullptr;
		if (!actor)
			throw std::runtime_error(T("feature.skin.select_a_reference_in_the_console_or_choose_player", "Select a reference in the console, or choose Player."));
		SelectActor(actor);
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.refresh_surfaces", "Refresh")))
		RefreshSurfaces();
	if (!editor.actor.get()) {
		ImGui::TextWrapped("%s", T("feature.skin.choose_target_first_v2", "Choose Player, or select an NPC in the console, to see which skin textures it shows."));
		return;
	}
	// Equipment and RaceMenu changes rebuild the model; keep the list current without user action.
	if (globals::state->frameCount - editor.lastRefresh > 30)
		RefreshSurfaces();
	ImGui::SameLine();
	ImGui::TextUnformatted(editor.actorName.empty() ? T("feature.skin.unnamed_character", "Unnamed character") : editor.actorName.c_str());
}

void Skin::DrawSurfaces()
{
	if (!editor.actor.get())
		return;
	ImGui::TextWrapped("%s", T("feature.skin.texture_identity_help", "Each skin texture has its own material. Everything that shows the same texture shares it: other characters, other races, and skin inside armor and clothing meshes."));
	std::vector<std::string> keys;
	std::map<std::string, std::vector<std::string>> users;
	for (const auto& surface : editor.surfaces) {
		if (!users.contains(surface.key))
			keys.push_back(surface.key);
		users[surface.key].push_back(SkinSurfaces::Label(surface.description));
	}
	if (keys.empty()) {
		ImGui::TextWrapped("%s", T("feature.skin.no_compatible_skin_surfaces_were_found", "No compatible skin surfaces were found."));
		return;
	}
	if (!ImGui::BeginTable("##skin_surfaces", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable))
		return;
	ScopeExit table{ [] { ImGui::EndTable(); } };
	ImGui::TableSetupColumn(T("feature.skin.column_texture", "Skin texture"), ImGuiTableColumnFlags_WidthStretch, 3);
	ImGui::TableSetupColumn(T("feature.skin.column_material", "Material"), ImGuiTableColumnFlags_WidthStretch, 2);
	ImGui::TableSetupColumn(T("feature.skin.column_shown_on", "Shown on"), ImGuiTableColumnFlags_WidthStretch, 2);
	ImGui::TableHeadersRow();
	for (const auto& key : keys) {
		ImGui::PushID(key.c_str());
		ScopeExit id{ [] { ImGui::PopID(); } };
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		if (key.empty()) {
			ImGui::TextDisabled("%s", T("feature.skin.unidentified_texture", "Unidentified texture (cannot hold a material)"));
		} else if (ImGui::Selectable(key.c_str(), key == editor.key, ImGuiSelectableFlags_SpanAllColumns)) {
			SelectKey(key);
		}
		ImGui::TableNextColumn();
		if (!key.empty()) {
			const auto entry = FindMaterial(key);
			if (entry->definition.status == Status::Invalid)
				ImGui::TextWrapped("%s", T("feature.skin.material_file_invalid", "Invalid material file - ignored"));
			else
				ImGui::TextWrapped("%s", OriginLabel(entry->origin));
			if (editor.draftTarget) {
				const auto* customization = &editor.character;
				if (customization->materials.contains(key) || !customization->all.Empty())
					ImGui::TextDisabled("%s", T("feature.skin.character_adjusted", "+ character adjustments"));
			}
		}
		ImGui::TableNextColumn();
		std::string shown;
		for (const auto& user : users[key])
			shown += (shown.empty() ? "" : ", ") + user;
		ImGui::TextWrapped("%s", shown.c_str());
	}
}

void Skin::DrawMaterialWorkspace()
{
	if (editor.draftKey.empty()) {
		ImGui::TextWrapped("%s", T("feature.skin.select_skin_texture", "Select a skin texture first."));
		return;
	}
	const auto entry = FindMaterial(editor.draftKey);
	ImGui::SeparatorText(T("feature.skin.texture_material", "Texture material"));
	ImGui::TextWrapped("%s", editor.draftKey.c_str());
	ImGui::TextDisabled("%s", entry->path.c_str());
	ImGui::TextWrapped("%s", OriginLabel(entry->origin));
	if (!entry->savedTo.empty())
		ImGui::TextDisabled("%s", entry->savedTo.c_str());
	if (entry->definition.status == Status::Invalid)
		ImGui::TextWrapped("%s %s", T("feature.skin.material_file_invalid_detail", "The existing file is ignored and the common settings apply. Saving replaces it:"), entry->definition.diagnostic.c_str());
	if (!editor.fingerprintWarning.empty())
		ImGui::TextWrapped("%s", editor.fingerprintWarning.c_str());
	if (editor.draftKey != editor.key)
		ImGui::TextWrapped("%s", T("feature.skin.draft_for_other_texture", "This draft belongs to a texture other than the one selected above. Save or discard it to edit the selected texture."));
	ImGui::TextWrapped("%s", T("feature.skin.material_scope", "Shared by every character, race and armor that shows this texture. Saving writes the material file beside the texture, ready to ship with your skin mod."));
	DrawMaterialControls(Layer::Material);
	DrawPreviewOptions();

	ImGui::SeparatorText(T("feature.skin.save_destination", "Save destination"));
	const auto root = settings.MaterialOutputFolder.empty() ? std::filesystem::path("Data") : std::filesystem::u8path(settings.MaterialOutputFolder);
	ImGui::TextWrapped("%s", UTF8(root / std::filesystem::u8path(SidecarPath(editor.draftKey))).c_str());
	ImGui::Checkbox(T("feature.skin.record_fingerprint", "Record which version of the diffuse texture this was made for"), &editor.recordFingerprint);
	if (ImGui::Button(T("feature.skin.save_material", "Save material file"), ImVec2(-FLT_MIN, 0)))
		SaveMaterial(root);
	if (ImGui::Button(T("feature.skin.save_material_to_folder", "Save into a mod folder..."))) {
		if (const auto folder = ChooseFolder()) {
			const auto modRoot = ModRoot(*folder);
			settings.MaterialOutputFolder = UTF8(modRoot);
			SaveMaterial(modRoot);
		}
	}
	if (!settings.MaterialOutputFolder.empty()) {
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.save_material_to_data", "Use the game's Data folder")))
			settings.MaterialOutputFolder.clear();
	}
	if (ImGui::Button(T("feature.skin.import_material_file", "Copy values from another material file..."))) {
		if (const auto path = ChooseFile(false, MaterialFilter, L"json")) {
			const auto definition = Parse(ReadResource(UTF8(*path)));
			if (definition.status != Status::Valid)
				throw std::runtime_error(definition.diagnostic);
			// Painted inputs belong to the other texture's UV layout, so only the numeric values are copied.
			PushUndo({ editor.material, editor.character });
			editor.material.parameters = ToChanges(definition.material).parameters;
			Invalidate();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.clear_material_overrides", "Clear all overrides"))) {
		PushUndo({ editor.material, editor.character });
		editor.material = {};
		Invalidate();
	}
}

void Skin::DrawCharacterWorkspace()
{
	const auto actor = editor.actor.get();
	if (!actor)
		return;
	ImGui::SeparatorText(editor.actorName.empty() ? T("feature.skin.unnamed_character", "Unnamed character") : editor.actorName.c_str());
	if (!editor.draftTarget) {
		ImGui::TextWrapped("%s", T("feature.skin.character_no_identity", "This character was generated at runtime and has no record to save adjustments for."));
		return;
	}
	const auto* saved = SkinCharacters::Find(*SkinCharacters::Get(), editor.draftTarget.key);
	ImGui::TextWrapped("%s", T("feature.skin.character_scope_v2", "Adjustments are saved as a file for this character. They apply in every save, to every actor using this record, and the file can be edited by hand or shared. A change of race or skin texture simply switches to that texture's material."));
	ImGui::TextDisabled("%s", editor.draftTarget.text.c_str());
	ImGui::TextDisabled("%s", saved ? UTF8(saved->file).c_str() : T("feature.skin.character_no_file", "No character file yet"));
	if (ImGui::RadioButton(T("feature.skin.character_scope_material", "Only this skin texture"), !editor.characterAll))
		editor.characterAll = false;
	ImGui::SameLine();
	if (ImGui::RadioButton(T("feature.skin.character_scope_all", "All skin of this character"), editor.characterAll))
		editor.characterAll = true;
	if (!editor.characterAll) {
		if (editor.key.empty()) {
			ImGui::TextWrapped("%s", T("feature.skin.select_skin_texture", "Select a skin texture first."));
		} else {
			ImGui::TextWrapped("%s", editor.key.c_str());
			if (std::none_of(editor.surfaces.begin(), editor.surfaces.end(), [this](const auto& surface) { return surface.key == editor.key; }))
				ImGui::TextWrapped("%s", T("feature.skin.character_texture_not_shown", "This character does not currently show this texture. Select one of its textures in the list above."));
			DrawMaterialControls(Layer::CharacterMaterial);
		}
	} else {
		DrawMaterialControls(Layer::CharacterAll);
	}
	DrawPreviewOptions();

	ImGui::SeparatorText(T("feature.skin.character_save_section", "Save"));
	{
		ImGui::BeginDisabled(!CharacterDirty());
		ScopeExit disabled{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.save_character", "Save character file"), ImVec2(-FLT_MIN, 0)))
			SaveCharacter();
	}
	if (ImGui::Button(T("feature.skin.remove_character_adjustments", "Remove all adjustments of this character"))) {
		PushUndo({ editor.material, editor.character });
		editor.character = {};
		Invalidate();
	}

	std::set<std::string> shown;
	for (const auto& surface : editor.surfaces)
		shown.insert(surface.key);
	std::vector<std::string> inactive;
	for (const auto& [key, changes] : editor.character.materials)
		if (!shown.contains(key))
			inactive.push_back(key);
	if (!inactive.empty() && ImGui::CollapsingHeader(T("feature.skin.inactive_character_materials", "Adjustments for textures not currently shown"))) {
		ImGui::TextWrapped("%s", T("feature.skin.inactive_character_materials_help", "These apply again when the character shows the texture, for example after switching back to an earlier race. They never apply to a different texture."));
		for (const auto& key : inactive) {
			ImGui::PushID(key.c_str());
			ScopeExit id{ [] { ImGui::PopID(); } };
			ImGui::TextWrapped("%s", key.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(T("feature.skin.remove", "Remove"))) {
				PushUndo({ editor.material, editor.character });
				editor.character.materials.erase(key);
				Invalidate();
				break;
			}
			if (!editor.key.empty() && shown.contains(editor.key)) {
				ImGui::SameLine();
				// Numeric values only: the painted inputs belong to the other texture's UV layout.
				if (ImGui::SmallButton(T("feature.skin.copy_values_to_selected_texture", "Copy values to the selected texture"))) {
					PushUndo({ editor.material, editor.character });
					const auto parameters = editor.character.materials.at(key).parameters;
					editor.character.materials[editor.key].parameters = parameters;
					Invalidate();
					break;
				}
			}
		}
	}

	if (ImGui::CollapsingHeader(T("feature.skin.character_files_section", "Files and sharing"))) {
		ImGui::TextWrapped("%s", T("feature.skin.character_files_help", "Character files are ordinary JSON in the folder below; a mod can ship them too. Use another character's file to start from its look: its adjustments are loaded into this draft and apply wherever the same textures are shown."));
		ImGui::TextDisabled("%s", UTF8(SkinCharacters::Directory()).c_str());
		if (ImGui::Button(T("feature.skin.open_character_folder_v2", "Open folder"))) {
			std::error_code ignored;
			std::filesystem::create_directories(SkinCharacters::Directory(), ignored);
			ShellExecuteW(nullptr, L"open", std::filesystem::absolute(SkinCharacters::Directory()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.import_character_file", "Load from another character file..."))) {
			std::error_code ignored;
			std::filesystem::create_directories(SkinCharacters::Directory(), ignored);
			if (const auto path = ChooseFile(false, CharacterFilter, L"json", std::filesystem::absolute(SkinCharacters::Directory()) / "")) {
				auto imported = SkinCharacters::Import(*path);
				PushUndo({ editor.material, editor.character });
				imported.name = editor.character.name.empty() ? editor.actorName : editor.character.name;
				size_t unmatched = 0;
				for (const auto& [key, changes] : imported.materials)
					unmatched += shown.contains(key) ? 0 : 1;
				editor.character = std::move(imported);
				editor.message = I18n::GetSingleton()->Format("feature.skin.character_file_loaded", { { "count", std::to_string(unmatched) } },
					"Loaded into the draft. {count} texture-specific entries do not match a texture this character currently shows. Save to keep it.");
				Invalidate(true);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.export_character_file", "Save a copy..."))) {
			if (const auto path = ChooseFile(true, CharacterFilter, L"json", std::filesystem::path(editor.actorName.empty() ? "Skin character" : editor.actorName).concat(".json"))) {
				SkinCharacters::Export(*path, editor.draftTarget, editor.character);
				editor.message = T("feature.skin.character_copy_saved", "Copy saved. Share any custom DDS files it refers to alongside it.");
			}
		}
		const auto state = SkinCharacters::Get();
		for (const auto& problem : state->problems)
			ImGui::TextWrapped("%s: %s", UTF8(problem.file.filename()).c_str(), problem.diagnostic.c_str());
	}
}

void Skin::DrawMaterialEditor()
{
	std::lock_guard lock(mutex);
	try {
		DrawEditorHeader();
		const bool textureTools = editor.workspace == Editor::Workspace::Textures;
		{
			const float footer = ImGui::GetFrameHeightWithSpacing() * 1.5f;
			const bool visible = ImGui::BeginChild("##skin_workspace_body", ImVec2(0, -footer));
			ScopeExit body{ [] { ImGui::EndChild(); } };
			if (visible) {
				if (textureTools) {
					DrawTextureTools();
				} else {
					DrawTargets();
					DrawSurfaces();
					if (editor.workspace == Editor::Workspace::Material)
						DrawMaterialWorkspace();
					else
						DrawCharacterWorkspace();
				}
			}
		}
		ImGui::Separator();
		DrawEditorHistory();
		DrawEditorDialogs();
	} catch (const std::exception& error) {
		editor.error = error.what();
		logger::warn("[Advanced Skin Editor] {}", error.what());
	}
}
