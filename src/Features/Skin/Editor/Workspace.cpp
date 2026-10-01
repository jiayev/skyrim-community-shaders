#include "Features/Skin.h"

#include <imgui_stdlib.h>

using namespace SkinMaterials;
using namespace SkinEditor;

namespace
{
	struct ScopeExit
	{
		std::function<void()> leave;
		~ScopeExit() { leave(); }
	};
}

void Skin::DrawEditorDialogs()
{
	if (editor.pendingSave && editor.saveConflict)
		ImGui::OpenPopup("###skin_save_conflict");
	if (ImGui::BeginPopupModal("###skin_save_conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ScopeExit popupScope{ [] { ImGui::EndPopup(); } };
		if (editor.saveConflict) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 32);
			ImGui::TextWrapped("%s", editor.saveConflict->what());
			if (!editor.error.empty())
				ImGui::TextWrapped("%s", editor.error.c_str());
			ImGui::TextWrapped("%s", UTF8(editor.saveConflict->path).c_str());
			ImGui::PopTextWrapPos();
			if (ImGui::Button(T("feature.skin.backup_and_save_edits", "Back up disk file and save my edits"))) {
				editor.confirmSave = true;
				ScopeExit confirmation{ [this] { editor.confirmSave = false; } };
				if (RunCharacterSave(editor.pendingSave))
					ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(T("feature.skin.keep_editing", "Keep editing"))) {
				editor.pendingSave = {};
				editor.saveConflict.reset();
				ImGui::CloseCurrentPopup();
			}
		} else {
			ImGui::CloseCurrentPopup();
		}
	}
	if (editor.pendingAction)
		ImGui::OpenPopup("###skin_discard_confirmation");
	bool discard = false;
	if (ImGui::BeginPopupModal("###skin_discard_confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ScopeExit popupScope{ [] { ImGui::EndPopup(); } };
		ImGui::TextWrapped("%s", T("feature.skin.unsaved_draft", "This editor has unsaved changes. Save or apply them first, or discard them to continue."));
		if (ImGui::Button(T("feature.skin.keep_editing", "Keep editing"))) {
			editor.pendingAction = {};
			editor.workspace = editor.character ? Editor::Workspace::Character : Editor::Workspace::Asset;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.discard_and_continue", "Discard and continue"))) {
			discard = true;
			ImGui::CloseCurrentPopup();
		}
	}
	if (discard) {
		auto action = std::move(editor.pendingAction);
		editor.pendingAction = {};
		editor.discardConfirmed = true;
		ScopeExit confirmationScope{ [this] { editor.discardConfirmed = false; } };
		action();
	}
	if (!editor.package.empty())
		ImGui::OpenPopup("###skin_package_outputs");
	if (ImGui::BeginPopupModal("###skin_package_outputs", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ScopeExit popupScope{ [] { ImGui::EndPopup(); } };
		ImGui::TextWrapped("%s", T("feature.skin.package_review", "Write these files? Existing files receive recovery backups. This includes the NIF and its explicit Skin textures; native textures remain dependencies of the original mod."));
		for (const auto& output : editor.package)
			ImGui::TextWrapped("%s", UTF8(output.path).c_str());
		if (ImGui::Button(T("feature.skin.write_package", "Write listed files"))) {
			editor.packageDocument->VerifiedBytes();
			Publish(editor.package);
			editor.packageDocument->Open(UTF8(editor.package.front().path));
			editor.document = std::move(editor.packageDocument);
			editor.materials = editor.document->Surfaces();
			editor.sourceInput = editor.document->Source();
			editor.baseline = editor.draft;
			editor.package.clear();
			editor.message = T("feature.skin.package_saved", "NIF and explicit Skin textures saved to the listed mod paths.");
			ImGui::CloseCurrentPopup();
			Invalidate(true);
		}
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.cancel", "Cancel"))) {
			editor.package.clear();
			editor.packageDocument.reset();
			ImGui::CloseCurrentPopup();
		}
	}
}

void Skin::DrawEditorSave()
{
	if (editor.character) {
		DrawCharacterActions();
		return;
	}
	if (ImGui::Button(T("feature.skin.save_nif_as", "Save NIF as..."), ImVec2(-FLT_MIN, 0))) {
		CheckTextures(editor.draft);
		if (const auto output = ChooseFile(true, NifFilter, L"nif", std::filesystem::u8path(editor.sourceInput).filename())) {
			auto next = *editor.document;
			next.SetMaterial(editor.selectedBlock, editor.draft);
			for (const auto& surface : next.Surfaces())
				if (surface.definition.status == Status::Valid)
					CheckTextures(surface.definition.material);
			next.Save(*output);
			editor.document = std::move(next);
			editor.baseline = editor.draft;
			editor.materials = editor.document->Surfaces();
			editor.sourceInput = editor.document->Source();
			editor.message = T("feature.skin.nif_saved_and_read_back_existing_loaded_models_update_when", "NIF saved and read back. Existing loaded models update when their 3D is rebuilt. Preview remains temporary.");
			Invalidate(true);
		}
	}
	if (ImGui::Button(T("feature.skin.prepare_package", "Save NIF with Skin textures..."))) {
		if (const auto output = ChooseFile(true, NifFilter, L"nif", std::filesystem::u8path(editor.sourceInput).filename())) {
			auto document = *editor.document;
			document.SetMaterial(editor.selectedBlock, editor.draft);
			editor.package = Package(document, *output);
			editor.packageDocument = std::move(document);
		}
	}
	DrawBatch();
}

void Skin::DrawMaterialEditor()
{
	std::lock_guard lock(mutex);
	UpdateEditorPreview();
	using Workspace = Editor::Workspace;
	try {
		const char* workspaces[] = { T("feature.skin.workspace_asset", "NIF authoring"), T("feature.skin.workspace_character", "Character customization"), T("feature.skin.workspace_textures", "Texture tools"), T("feature.skin.workspace_storage", "Configuration") };
		int workspace = static_cast<int>(editor.workspace);
		float workspaceWidth = 0;
		for (const auto* label : workspaces)
			workspaceWidth = std::max(workspaceWidth, ImGui::CalcTextSize(label).x + ImGui::GetStyle().CellPadding.x * 2 + ImGui::GetStyle().ItemSpacing.x);
		if (ImGui::GetContentRegionAvail().x < workspaceWidth * 4) {
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::Combo("##skin_workspace", &workspace, workspaces, 4);
		} else if (ImGui::BeginTable("##skin_workspaces", 4, ImGuiTableFlags_SizingStretchSame)) {
			for (int i = 0; i < 4; ++i) {
				ImGui::TableNextColumn();
				if (ImGui::Selectable(workspaces[i], workspace == i))
					workspace = i;
			}
			ImGui::EndTable();
		}
		if (workspace != static_cast<int>(editor.workspace)) {
			editor.workspace = static_cast<Workspace>(workspace);
			Invalidate();
		}
		ImGui::Separator();
		const bool asset = editor.workspace == Workspace::Asset;
		const bool character = editor.workspace == Workspace::Character;
		const bool editing = (asset && !editor.character && editor.document && editor.selectedBlock != UINT32_MAX) ||
		                     (character && editor.character && editor.geometry);
		if (editing) {
			if (asset) {
				ImGui::TextWrapped("%s", editor.document->Source().empty() ? T("feature.skin.new_material_template", "New material template") : editor.document->Source().c_str());
				const auto material = std::find_if(editor.materials.begin(), editor.materials.end(), [this](const auto& value) { return value.block == editor.selectedBlock; });
				if (material != editor.materials.end())
					ImGui::TextWrapped("%s", I18n::GetSingleton()->Format("feature.skin.material_block", { { "name", material->name.empty() ? T("feature.skin.skin_material", "Skin material") : material->name }, { "block", std::to_string(material->block) } }, "{name} [block {block}]").c_str());
			} else {
				const auto& target = geometries.at(editor.geometry.get()).target;
				ImGui::TextWrapped("%s", target.name.c_str());
				if (target.part < SkinActors::Part::Count) {
					ImGui::SameLine();
					ImGui::TextDisabled("/ %s", SkinActors::PartNames()[static_cast<size_t>(target.part)]);
				}
			}
			ImGui::TextDisabled("%s", EditorDirty() ? T("feature.skin.draft_unsaved", "Unsaved draft") : T("feature.skin.draft_clean", "No unsaved changes"));
			ImGui::SameLine();
			ImGui::TextDisabled("/ %s", editor.preview || (editor.transfer && editor.transfer->preview) ? T("feature.skin.preview_active", "Temporary preview active") : T("feature.skin.preview_waiting", "Waiting for a loaded preview surface"));
		} else if (EditorDirty()) {
			ImGui::TextWrapped("%s", T("feature.skin.draft_retained", "Your draft is retained in its editing workspace. Opening a different target will ask before discarding it."));
		}
		if ((!editing && (editor.document || editor.character)) && ImGui::SmallButton(T("feature.skin.return_to_draft", "Return to material draft"))) {
			editor.workspace = editor.character ? Workspace::Character : Workspace::Asset;
			editor.step = 1;
		}
		if (!editor.error.empty()) {
			ImGui::TextWrapped("%s", editor.error.c_str());
			if (ImGui::SmallButton(T("feature.skin.dismiss_error", "Dismiss error")))
				editor.error.clear();
		} else if (!editor.message.empty()) {
			ImGui::TextWrapped("%s", editor.message.c_str());
		}
		if (asset || character) {
			const char* steps[] = { T("feature.skin.step_target", "1  Select target"), T("feature.skin.step_material", "2  Adjust material"), T("feature.skin.step_finish", "3  Review and save") };
			float stepWidth = 0;
			for (const auto* label : steps)
				stepWidth = std::max(stepWidth, ImGui::CalcTextSize(label).x + ImGui::GetStyle().CellPadding.x * 2 + ImGui::GetStyle().ItemSpacing.x);
			if (ImGui::GetContentRegionAvail().x < stepWidth * 3) {
				int step = editing ? editor.step : 0;
				ImGui::BeginDisabled(!editing || bool(editor.transfer));
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::Combo("##skin_step", &step, steps, 3))
					editor.step = step;
				ImGui::EndDisabled();
			} else if (ImGui::BeginTable("##skin_steps", 3, ImGuiTableFlags_SizingStretchSame)) {
				for (int i = 0; i < 3; ++i) {
					ImGui::TableNextColumn();
					ImGui::BeginDisabled((i > 0 && !editing) || bool(editor.transfer));
					if (ImGui::Selectable(steps[i], (editing ? editor.step : 0) == i))
						editor.step = i;
					ImGui::EndDisabled();
				}
				ImGui::EndTable();
			}
		}
		{
			const float footer = (asset || character) ? ImGui::GetFrameHeightWithSpacing() * 2.5f : 0;
			const bool visible = ImGui::BeginChild("##skin_workspace_body", ImVec2(0, -footer));
			ScopeExit body{ [] { ImGui::EndChild(); } };
			if (visible) {
				if (editor.workspace == Workspace::Textures) {
					ImGui::TextWrapped("%s", T("feature.skin.texture_tool_flow", "Choose a texture type, provide source images, then build a DDS. Return to Adjust material to assign it; your material draft is retained."));
					DrawTextureTools();
				} else if (editor.workspace == Workspace::Storage) {
					ImGui::TextWrapped("%s", T("feature.skin.storage_flow", "Manage the external character configuration and backups. NIF authoring saves directly to assets; it does not use this file."));
					DrawCharacterStorage();
				} else if (editor.transfer && character) {
					DrawCharacterTransfer();
				} else if (!editing || editor.step == 0) {
					DrawEditorTarget();
				} else if (editor.step == 1) {
					DrawMaterialControls();
					DrawPresets();
				} else {
					DrawEditorPreview();
					ImGui::SeparatorText(T("feature.skin.save_destination", "Save destination"));
					ImGui::TextWrapped("%s", asset ? T("feature.skin.save_asset_scope", "Save the edited NIF for your mod. This affects every user of that asset after their model is rebuilt. Preview is temporary.") : T("feature.skin.save_character_scope", "Save only the selected character surfaces to the external configuration. This does not change the NIF or follow game saves."));
					if (character)
						ImGui::TextWrapped("%s", UTF8(SkinActors::Get()->path).c_str());
					DrawEditorSave();
				}
			}
		}
		if (asset || character) {
			ImGui::Separator();
			if (editing && !editor.transfer) {
				DrawEditorHistory();
				ImGui::BeginDisabled(editor.step == 0);
				if (ImGui::Button(T("feature.skin.previous_step", "Previous")))
					--editor.step;
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(editor.step == 2);
				if (ImGui::Button(editor.step == 0 ? T("feature.skin.continue_material", "Continue to material") : T("feature.skin.continue_preview", "Continue to review and save")))
					++editor.step;
				ImGui::EndDisabled();
			} else if (!editing) {
				ImGui::TextWrapped("%s", T("feature.skin.choose_target_first", "Choose a target above to begin. Common settings already apply to all compatible skin."));
			}
		}
		DrawEditorDialogs();
	} catch (const std::exception& error) {
		editor.error = error.what();
		logger::warn("[Advanced Skin Editor] {}", error.what());
	}
}

void Skin::DrawEditorSurfacePicker(bool a_previewOnly)
{
	if (ImGui::Button(T("feature.skin.player", "Player")))
		SelectReference(RE::PlayerCharacter::GetSingleton(), false, a_previewOnly);
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.first_person", "First person")))
		SelectReference(RE::PlayerCharacter::GetSingleton(), true, a_previewOnly);
	if (ImGui::Button(T("feature.skin.console_selection", "Console selection")))
		SelectReference(RE::Console::GetSelectedRef().get(), false, a_previewOnly);
	const auto name = editor.geometry && editor.geometry->name.c_str() ? editor.geometry->name.c_str() : T("feature.skin.select_material", "Select material");
	if (ImGui::BeginCombo(a_previewOnly ? T("feature.skin.preview_surface", "Preview surface") : T("feature.skin.edit_surface", "Surface to customize"), name)) {
		ScopeExit combo{ [] { ImGui::EndCombo(); } };
		for (const auto& surface : editor.surfaces) {
			ImGui::PushID(surface.get());
			ScopeExit id{ [] { ImGui::PopID(); } };
			const auto found = geometries.find(surface.get());
			if (found == geometries.end())
				continue;
			const auto& target = found->second.target;
			const auto label = std::string(surface->name.c_str() ? surface->name.c_str() : T("feature.skin.unnamed_surface", "Unnamed surface")) +
			                   (target.guard.firstPerson ? T("feature.skin.suffix_first_person", " (first person)") : T("feature.skin.suffix_third_person", " (third person)"));
			if (ImGui::Selectable(label.c_str(), editor.geometry == surface)) {
				if (a_previewOnly) {
					editor.geometry = surface;
					Invalidate();
				} else {
					ChangeEditor([this, surface] { SelectSurface(surface.get()); });
				}
			}
		}
	}
}

void Skin::DrawEditorTarget()
{
	if (editor.workspace == Editor::Workspace::Asset) {
		ImGui::SeparatorText(T("feature.skin.choose_asset", "Choose a NIF to author"));
		ImGui::TextWrapped("%s", T("feature.skin.asset_flow", "Open the final body, face, hands or feet NIF. Choose its material, adjust only the overrides you need, then preview and save."));
		if (ImGui::Button(T("feature.skin.open_nif", "Open NIF")))
			if (const auto path = ChooseFile(false, NifFilter, L"nif"))
				ChangeEditor([this, path = UTF8(*path)] { OpenDocument(path); });
		if (ImGui::Button(T("feature.skin.new_template", "New material template")))
			ChangeEditor([this] {
				Material material;
				material.enabled = true;
				editor.document = NifDocument::Template(material);
				editor.materials = editor.document->Surfaces();
				editor.selectedBlock = UINT32_MAX;
				editor.character = false;
				editor.transfer.reset();
				editor.workspace = Editor::Workspace::Asset;
				editor.step = 0;
				editor.sourceInput.clear();
				SelectMaterial(0);
			});
		if (ImGui::CollapsingHeader(T("feature.skin.game_resource_archive_input", "Game resource / archive input"))) {
			ImGui::InputText(T("feature.skin.resource_path", "Resource path"), &editor.sourceInput);
			if (ImGui::Button(T("feature.skin.open_resource", "Open resource")))
				ChangeEditor([this, path = editor.sourceInput] { OpenDocument(path); });
		}
		if (!editor.character)
			DrawAssetActions();
	} else {
		ImGui::SeparatorText(T("feature.skin.choose_character", "Choose a character and part"));
		ImGui::TextWrapped("%s", T("feature.skin.character_flow", "Choose Player or an NPC selected in the console, then select the surface and the parts affected by this edit."));
		DrawEditorSurfacePicker(false);
		if (editor.character && editor.geometry) {
			DrawCharacterTargets();
			if (ImGui::CollapsingHeader(T("feature.skin.character_schemes_section", "Copy or reuse a character scheme")))
				DrawCharacterSchemeActions();
			const auto& entry = geometries.at(editor.geometry.get());
			if (entry.source.asset && ImGui::CollapsingHeader(T("feature.skin.edit_asset_instead", "Edit the shared NIF instead"))) {
				ImGui::TextWrapped("%s", T("feature.skin.shared_asset_scope", "Use this when making a mod for everyone using this mesh. It changes the shared asset, not just this character."));
				if (ImGui::Button(T("feature.skin.edit_source_nif", "Edit source NIF"))) {
					NifDocument document;
					document.Open(entry.source.asset->path);
					if (!entry.source.asset->fingerprint || document.SourceFingerprint() != entry.source.asset->fingerprint)
						throw std::runtime_error(T("feature.skin.source_snapshot_changed_or_is_unavailable_open_the_file_explicitly", "Source snapshot changed or is unavailable. Open the file explicitly and select its material."));
					ChangeEditor([this, source = entry.source.asset] { OpenDocument(source->path, source->block); });
				}
			}
		}
	}
}

void Skin::DrawEditorPreview()
{
	ImGui::SeparatorText(T("feature.skin.preview_section", "Preview on a loaded character"));
	if (!editor.character) {
		ImGui::TextWrapped("%s", T("feature.skin.preview_asset_target", "Choose a loaded surface for a temporary preview. This keeps the NIF you are authoring open."));
		DrawEditorSurfacePicker(true);
	}
	if (!editor.geometry) {
		ImGui::TextWrapped("%s", T("feature.skin.preview_optional", "No preview surface selected. You can still save the NIF without a game preview."));
		return;
	}
	const auto cached = geometries.find(editor.geometry.get());
	auto* property = editor.geometry->GetGeometryRuntimeData().shaderProperty.get();
	if (cached == geometries.end() || (cached->second.actor && !SkinActors::Belongs(editor.geometry.get(), cached->second.actor.get().get())) || !IsCompatible(property)) {
		editor.previewWetness = -1;
		ImGui::TextWrapped("%s", T("feature.skin.preview_unavailable", "The preview surface is no longer available. Select its current model again. The NIF draft can still be saved."));
		if (!editor.character)
			editor.geometry = nullptr;
		Invalidate();
		return;
	}
	auto& entry = Observe(editor.geometry.get(), property);
	if (!entry.ready || entry.revision != revision || entry.actorRevision != SkinActors::Revision())
		Prepare(entry, SkinActors::Get());
	if (!entry.diagnostic.empty())
		ImGui::TextWrapped("%s", entry.diagnostic.c_str());
	for (size_t i = 0; i < entry.textures.size(); ++i)
		if (entry.textures[i] && !entry.textures[i]->resource.error.empty())
			ImGui::TextWrapped("%s: %s", TextureLabels()[i], entry.textures[i]->resource.error.c_str());
	if (editor.character) {
		ImGui::Text(T("feature.skin.selected_surface_count", "Selected surfaces: %u"), static_cast<unsigned>(editor.targets.size()));
		DrawCharacterTargets();
	}
	ImGui::TextWrapped("%s", T("feature.skin.preview_automatic", "Edits preview automatically while the editor is open. Save to keep them; discard to restore the draft's starting values."));
	bool overrideWetness = editor.previewWetness >= 0;
	if (ImGui::Checkbox(T("feature.skin.preview_override_wetness", "Test a specific wetness level"), &overrideWetness)) {
		editor.previewWetness = overrideWetness ? 0.0f : -1.0f;
		Invalidate();
	}
	if (overrideWetness) {
		if (ImGui::SliderFloat(T("feature.skin.preview_wetness", "Preview wetness"), &editor.previewWetness, 0, 1))
			Invalidate();
	} else {
		ImGui::TextDisabled("%s", T("feature.skin.preview_live_wetness", "Using the character's current wetness."));
	}
	if (ImGui::CollapsingHeader(T("feature.skin.source_details", "Source details and resource reload"))) {
		if (entry.source.asset)
			ImGui::TextWrapped(T("feature.skin.source_s_material_block_u_txst_08x", "Source: %s | material block %u | TXST %08X"), entry.source.asset->path.c_str(), entry.source.asset->block, entry.source.txst);
		if (ImGui::Button(T("feature.skin.reload_skin_resources", "Reload skin resources"))) {
			Invalidate(true);
			editor.thumbnailKey.clear();
		}
	}
}
