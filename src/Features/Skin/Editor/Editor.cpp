#include "Features/Skin.h"

#include "Files.h"
#include "Legacy.h"

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
	struct ScopeID
	{
		explicit ScopeID(int a_id) { ImGui::PushID(a_id); }
		explicit ScopeID(const void* a_id) { ImGui::PushID(a_id); }
		explicit ScopeID(const char* a_id) { ImGui::PushID(a_id); }
		~ScopeID() { ImGui::PopID(); }
	};
	std::string SurfaceName(RE::BSGeometry* a_geometry)
	{
		return a_geometry && a_geometry->name.c_str() && *a_geometry->name.c_str() ? a_geometry->name.c_str() : T("feature.skin.unnamed_surface", "Unnamed surface");
	}
	std::string MaterialName(const NifDocument::Surface& a_surface)
	{
		return I18n::GetSingleton()->Format("feature.skin.material_block", { { "name", a_surface.name.empty() ? T("feature.skin.skin_material", "Skin material") : a_surface.name }, { "block", std::to_string(a_surface.block) } }, "{name} [block {block}]");
	}
	void PathPicker(const char* a_label, std::string& a_path)
	{
		ScopeID scopedID(a_label);
		ImGui::TextUnformatted(a_label);
		ImGui::TextWrapped("%s", a_path.empty() ? T("feature.skin.constant_not_selected", "Constant / not selected") : a_path.c_str());
		if (ImGui::Button(T("feature.skin.choose_image", "Choose image"))) {
			if (const auto path = ChooseFile(false, ImageFilter, nullptr))
				a_path = UTF8(*path);
		}
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.clear", "Clear")))
			a_path.clear();
	}
}

bool Skin::EditorDirty() const
{
	return editor.character ? editor.changes != editor.changesBaseline :
	                          editor.document && (editor.document->Dirty() || editor.draft != editor.baseline);
}

void Skin::ChangeEditor(std::function<void()> a_action)
{
	if (EditorDirty() && !editor.discardConfirmed)
		editor.pendingAction = std::move(a_action);
	else
		a_action();
}

void Skin::SelectReference(RE::TESObjectREFR* a_reference, bool a_firstPerson)
{
	if (!a_reference)
		throw std::runtime_error(T("feature.skin.select_a_reference_in_the_console_or_choose_player", "Select a reference in the console, or choose Player."));
	RE::NiAVObject* root = a_reference->Get3D();
	if (a_reference == RE::PlayerCharacter::GetSingleton())
		root = RE::PlayerCharacter::GetSingleton()->Get3D(a_firstPerson);
	if (!root)
		throw std::runtime_error(T("feature.skin.this_reference_has_no_loaded_3d_in_the_selected_view", "This reference has no loaded 3D in the selected view."));
	std::vector<RE::NiPointer<RE::BSGeometry>> surfaces;
	RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
		if (IsCompatible(geometry->GetGeometryRuntimeData().shaderProperty.get()))
			surfaces.emplace_back(geometry);
		return RE::BSVisit::BSVisitControl::kContinue;
	});
	if (surfaces.empty())
		throw std::runtime_error(T("feature.skin.no_compatible_skin_surfaces_were_found", "No compatible skin surfaces were found."));
	const auto characters = SkinActors::Get();
	for (const auto& surface : surfaces) {
		if (std::find(editor.surfaces.begin(), editor.surfaces.end(), surface) == editor.surfaces.end())
			editor.surfaces.push_back(surface);
		auto& entry = Observe(surface.get(), surface->GetGeometryRuntimeData().shaderProperty.get());
		if (auto* actor = a_reference->As<RE::Actor>())
			entry.actor = actor->GetHandle();
		entry.inspect = true;
		Prepare(entry, characters);
	}
	if (!editor.geometry || geometries.at(editor.geometry.get()).target.actor.get().get() != a_reference)
		ChangeEditor([this, surface = surfaces.front()] { SelectSurface(surface.get()); });
}

void Skin::SelectSurface(RE::BSGeometry* a_geometry)
{
	const auto cached = geometries.find(a_geometry);
	if (cached != geometries.end() && cached->second.actor && !SkinActors::Belongs(a_geometry, cached->second.actor.get().get()))
		throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
	auto* property = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
	if (!IsCompatible(property))
		throw std::runtime_error(T("feature.skin.this_surface_is_no_longer_compatible", "This surface is no longer compatible."));
	auto& entry = Observe(a_geometry, property);
	const auto characters = SkinActors::Get();
	entry.inspect = true;
	Prepare(entry, characters);
	editor.transfer.reset();
	editor.geometry = RE::NiPointer(a_geometry);
	editor.targets = { editor.geometry };
	editor.character = entry.target.character;
	editor.document.reset();
	editor.materials.clear();
	editor.selectedBlock = UINT32_MAX;
	editor.preview = false;
	editor.previewWetness = -1;
	editor.undo.clear();
	editor.redo.clear();
	editor.characterUndo.clear();
	editor.characterRedo.clear();
	editor.draft = editor.baseline = entry.base;
	editor.changes = {};
	if (editor.character) {
		const auto* binding = SkinActors::Find(*characters, entry.target.key);
		if (const auto* savedSurface = SkinActors::Find(binding, entry.target.part, entry.target.guard))
			editor.changes = savedSurface->changes;
		else if (binding && entry.target.part < SkinActors::Part::Count) {
			const auto& part = binding->parts[static_cast<size_t>(entry.target.part)];
			if (part.size() == 1)
				editor.changes = part.front().changes;
			else
				for (const auto& surface : part)
					if (surface.guard.source == entry.target.guard.source && surface.guard.materialBlock == entry.target.guard.materialBlock &&
						surface.guard.firstPerson == entry.target.guard.firstPerson)
						editor.changes = surface.changes;
		}
	}
	editor.changesBaseline = editor.changes;
	editor.message = T("feature.skin.surface_selected_preview_changes_affect_only_the_explicitly_selected_surface", "Surface selected. Preview changes affect only the explicitly selected surface targets.");
	editor.error.clear();
	Invalidate();
}

void Skin::OpenDocument(const std::string& a_path, uint32_t a_block)
{
	NifDocument document;
	document.Open(a_path);
	const auto materials = document.Surfaces();
	if (materials.empty())
		throw std::runtime_error(T("feature.skin.this_nif_contains_no_lighting_shader_properties", "This NIF contains no lighting shader properties."));
	editor.transfer.reset();
	editor.document = std::move(document);
	editor.materials = materials;
	editor.sourceInput = a_path;
	editor.character = false;
	editor.preview = false;
	editor.selectedBlock = UINT32_MAX;
	if (a_block == UINT32_MAX) {
		const auto first = std::find_if(materials.begin(), materials.end(), [](const auto& item) { return item.compatible && item.definition.status != Status::Invalid; });
		if (first != materials.end())
			a_block = first->block;
	}
	if (a_block != UINT32_MAX)
		SelectMaterial(a_block);
	Invalidate();
}

void Skin::SelectMaterial(uint32_t a_block)
{
	const auto materials = editor.document ? editor.document->Surfaces() : editor.materials;
	const auto surface = std::find_if(materials.begin(), materials.end(), [a_block](const auto& item) { return item.block == a_block; });
	if (surface == materials.end() || !surface->compatible || surface->definition.status == Status::Invalid)
		throw std::runtime_error(T("feature.skin.select_a_compatible_skin_material_with_valid_extension_data", "Select a compatible skin material with valid extension data."));
	if (editor.document && editor.selectedBlock != UINT32_MAX && editor.selectedBlock != a_block && editor.draft != editor.baseline)
		editor.document->SetMaterial(editor.selectedBlock, editor.draft);
	editor.selectedBlock = a_block;
	editor.draft = editor.baseline = surface->definition.material;
	editor.undo.clear();
	editor.redo.clear();
	editor.characterUndo.clear();
	editor.characterRedo.clear();
	editor.preview = false;
	editor.error.clear();
	Invalidate();
}

void Skin::DrawMaterialEditor()
{
	std::lock_guard lock(mutex);
	ImGui::TextWrapped("%s", T("feature.skin.edit_asset_or_character", "Edit a NIF for a mod, or customize a player or NPC. Character settings save immediately to the external configuration."));
	try {
		DrawCharacterStorage();
		if (ImGui::Button(T("feature.skin.player", "Player")))
			SelectReference(RE::PlayerCharacter::GetSingleton());
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.first_person", "First person")))
			SelectReference(RE::PlayerCharacter::GetSingleton(), true);
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.console_selection", "Console selection")))
			SelectReference(RE::Console::GetSelectedRef().get());
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.open_nif", "Open NIF")))
			if (const auto path = ChooseFile(false, NifFilter, L"nif"))
				ChangeEditor([this, path = UTF8(*path)] { OpenDocument(path); });
		ImGui::SameLine();
		if (ImGui::Button(T("feature.skin.new_template", "New material template"))) {
			ChangeEditor([this] {
				Material material;
				material.enabled = true;
				editor.document = NifDocument::Template(material);
				editor.materials = editor.document->Surfaces();
				editor.selectedBlock = UINT32_MAX;
				editor.character = false;
				editor.sourceInput.clear();
				SelectMaterial(0);
			});
		}
		if (ImGui::CollapsingHeader(T("feature.skin.game_resource_archive_input", "Game resource / archive input"))) {
			ImGui::InputText(T("feature.skin.resource_path", "Resource path"), &editor.sourceInput);
			if (ImGui::Button(T("feature.skin.open_resource", "Open resource")))
				ChangeEditor([this, path = editor.sourceInput] { OpenDocument(path); });
		}
		if (!editor.surfaces.empty() && ImGui::BeginCombo(T("feature.skin.preview_surface", "Preview surface"), SurfaceName(editor.geometry.get()).c_str())) {
			ScopeExit comboScope{ [] { ImGui::EndCombo(); } };
			for (const auto& surface : editor.surfaces) {
				ScopeID scopedID(surface.get());
				if (ImGui::Selectable(SurfaceName(surface.get()).c_str(), editor.geometry == surface)) {
					if (!editor.character && editor.document) {
						if (!IsCompatible(surface->GetGeometryRuntimeData().shaderProperty.get()))
							throw std::runtime_error(T("feature.skin.this_surface_is_no_longer_compatible", "This surface is no longer compatible."));
						editor.geometry = surface;
						Invalidate();
					} else {
						ChangeEditor([this, surface] { SelectSurface(surface.get()); });
					}
				}
			}
		}
		if (editor.geometry) {
			const auto cached = geometries.find(editor.geometry.get());
			if (cached != geometries.end() && cached->second.actor && !SkinActors::Belongs(editor.geometry.get(), cached->second.actor.get().get()))
				throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
			auto* property = editor.geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!IsCompatible(property))
				throw std::runtime_error(T("feature.skin.this_surface_is_no_longer_compatible", "This surface is no longer compatible."));
			auto& entry = Observe(editor.geometry.get(), property);
			if (!entry.ready || entry.revision != revision || entry.actorRevision != SkinActors::Revision())
				Prepare(entry, SkinActors::Get());
			if (entry.source.asset) {
				ImGui::TextWrapped(T("feature.skin.source_s_material_block_u_txst_08x", "Source: %s | material block %u | TXST %08X"), entry.source.asset->path.c_str(), entry.source.asset->block, entry.source.txst);
				if (ImGui::Button(T("feature.skin.edit_source_nif", "Edit source NIF"))) {
					NifDocument document;
					document.Open(entry.source.asset->path);
					if (!entry.source.asset->fingerprint || document.SourceFingerprint() != entry.source.asset->fingerprint)
						throw std::runtime_error(T("feature.skin.source_snapshot_changed_or_is_unavailable_open_the_file_explicitly", "Source snapshot changed or is unavailable. Open the file explicitly and select its material."));
					ChangeEditor([this, source = entry.source.asset] { OpenDocument(source->path, source->block); });
				}
			} else {
				ImGui::TextWrapped("%s", T("feature.skin.source_provenance_unavailable_open_the_intended_nif_explicitly_for_asset", "Source provenance unavailable. Open the intended NIF explicitly for asset editing."));
			}
			if (entry.target.character) {
				ImGui::SameLine();
				if (ImGui::Button(T("feature.skin.customize_character_part", "Customize this character part")))
					ChangeEditor([this, surface = editor.geometry] { SelectSurface(surface.get()); });
			}
			if (!entry.diagnostic.empty())
				ImGui::TextWrapped("%s", entry.diagnostic.c_str());
			for (size_t i = 0; i < entry.textures.size(); ++i)
				if (entry.textures[i] && !entry.textures[i]->resource.error.empty())
					ImGui::TextWrapped("%s: %s", TextureLabels()[i], entry.textures[i]->resource.error.c_str());
		}
		if (!editor.character)
			DrawAssetActions();
		const bool editable = editor.character ? bool(editor.geometry) : editor.document && editor.selectedBlock != UINT32_MAX;
		if (editable) {
			ImGui::BeginDisabled(bool(editor.transfer));
			ScopeExit controlsScope{ [] { ImGui::EndDisabled(); } };
			DrawMaterialControls();
			if (editor.geometry) {
				if (ImGui::Checkbox(T("feature.skin.preview_draft_on_selected_surface", "Preview draft on selected surface"), &editor.preview))
					Invalidate();
				ImGui::SameLine();
				if (ImGui::Button(T("feature.skin.use_live_wetness", "Use live wetness"))) {
					editor.previewWetness = -1;
					Invalidate();
				}
				if (ImGui::SliderFloat(T("feature.skin.preview_wetness_1_live", "Preview wetness (-1 = live)"), &editor.previewWetness, -1, 1))
					Invalidate();
			}
			if (editor.character)
				DrawCharacterActions();
			else if (ImGui::Button(T("feature.skin.save_nif_as", "Save NIF as..."))) {
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
			if (!editor.character && ImGui::Button(T("feature.skin.prepare_package", "Save NIF with Skin textures..."))) {
				if (const auto output = ChooseFile(true, NifFilter, L"nif", std::filesystem::u8path(editor.sourceInput).filename())) {
					auto document = *editor.document;
					document.SetMaterial(editor.selectedBlock, editor.draft);
					editor.package = Package(document, *output);
					editor.packageDocument = std::move(document);
				}
			}
			DrawPresets();
			DrawBatch();
		}
		DrawCharacterTransfer();
		DrawTextureTools();
		if (ImGui::Button(T("feature.skin.reload_skin_resources", "Reload skin resources"))) {
			Invalidate(true);
			editor.thumbnailKey.clear();
		}
		if (editor.pendingAction)
			ImGui::OpenPopup("###skin_discard_confirmation");
		bool discard = false;
		if (ImGui::BeginPopupModal("###skin_discard_confirmation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ScopeExit popupScope{ [] { ImGui::EndPopup(); } };
			ImGui::TextWrapped("%s", T("feature.skin.unsaved_draft", "This editor has unsaved changes. Save or apply them first, or discard them to continue."));
			if (ImGui::Button(T("feature.skin.keep_editing", "Keep editing"))) {
				editor.pendingAction = {};
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
	} catch (const std::exception& e) {
		editor.error = e.what();
		logger::warn("[Advanced Skin Editor] {}", e.what());
	}
	if (!editor.message.empty())
		ImGui::TextWrapped("%s", editor.message.c_str());
	if (!editor.error.empty()) {
		ImGui::TextWrapped("%s", editor.error.c_str());
		if (ImGui::SmallButton(T("feature.skin.dismiss_error", "Dismiss error")))
			editor.error.clear();
	}
}

void Skin::DrawAssetActions()
{
	if (!editor.document)
		return;
	ImGui::Separator();
	ImGui::TextWrapped(T("feature.skin.asset_s", "Asset: %s"), editor.document->Source().empty() ? T("feature.skin.new_material_template", "New material template") : editor.document->Source().c_str());
	const auto current = std::find_if(editor.materials.begin(), editor.materials.end(), [&](const auto& item) { return item.block == editor.selectedBlock; });
	if (ImGui::BeginCombo(T("feature.skin.nif_material", "NIF material"), current == editor.materials.end() ? T("feature.skin.select_material", "Select material") : MaterialName(*current).c_str())) {
		ScopeExit comboScope{ [] { ImGui::EndCombo(); } };
		for (const auto& material : editor.materials) {
			const bool supported = material.compatible && material.definition.status != Status::Invalid;
			{
				ImGui::BeginDisabled(!supported);
				ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
				if (ImGui::Selectable(MaterialName(material).c_str(), material.block == editor.selectedBlock))
					SelectMaterial(material.block);
			}
			if (!material.definition.diagnostic.empty())
				ImGui::TextWrapped("%s", material.definition.diagnostic.c_str());
		}
	}
	if (ImGui::CollapsingHeader(T("feature.skin.shapes_using_this_material", "Shapes using this material"))) {
		ImGui::TextWrapped("%s", T("feature.skin.editing_a_shared_material_affects_all_of_its_shapes_make", "Editing a shared material affects all of its shapes. Make a copy to change only one shape."));
		const auto shapes = editor.document->Shapes();
		for (const auto& shape : shapes) {
			if (shape.material != editor.selectedBlock)
				continue;
			ScopeID scopedID(static_cast<int>(shape.block));
			ImGui::Text(T("feature.skin.s_block_u", "%s [block %u]"), shape.name.c_str(), shape.block);
			ImGui::SameLine();
			if (ImGui::SmallButton(T("feature.skin.make_independent", "Make independent"))) {
				auto next = *editor.document;
				const auto block = next.MakeIndependent(shape.block);
				next.SetMaterial(block, editor.draft);
				editor.document = std::move(next);
				editor.materials = editor.document->Surfaces();
				editor.selectedBlock = UINT32_MAX;
				SelectMaterial(block);
				editor.message = T("feature.skin.independent_material_prepared_save_nif_to_commit_the_new_shape", "Independent material prepared. Save NIF to commit the new shape link.");
			}
		}
	}
}

void Skin::DrawMaterialControls()
{
	const auto before = editor.draft;
	const auto changesBefore = editor.changes;
	const auto base = editor.character ? geometries.at(editor.geometry.get()).base : editor.draft;
	auto effective = editor.character ? Apply(base, editor.changes) : editor.draft;
	if (!editor.character)
		ImGui::Checkbox(T("feature.skin.enable_advanced_skin_on_this_material", "Enable Advanced Skin on this material"), &editor.draft.enabled);
	else
		ImGui::TextWrapped("%s", T("feature.skin.unchecked_values_inherit_the_current_nif_txst_material_applying_enables", "Unchecked values inherit the current NIF/TXST material. Applying enables Advanced Skin for this player part."));
	const auto fields = ParameterTable();
	for (size_t i = 0; i < fields.size(); ++i) {
		ScopeID scopedID(static_cast<int>(i));
		bool overridden = !editor.character || editor.changes.parameters[i].has_value();
		if (editor.character) {
			if (ImGui::Checkbox(T("feature.skin.override", "Override"), &overridden))
				editor.changes.parameters[i] = overridden ? std::optional(effective.parameters.values[i]) : std::nullopt;
			ImGui::SameLine();
		}
		const bool globalDetail = effective.textures[2].empty() &&
		                          (i == static_cast<size_t>(Parameter::DetailStrength) || i == static_cast<size_t>(Parameter::DetailTiling));
		{
			ImGui::BeginDisabled(!overridden || globalDetail);
			ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
			float value = effective.parameters.values[i];
			bool changed;
			if (fields[i].integer) {
				bool flag = value != 0;
				changed = ImGui::Checkbox(ParameterLabel(i), &flag);
				value = flag ? 1.0f : 0.0f;
			} else {
				changed = ImGui::SliderFloat(ParameterLabel(i), &value, fields[i].minimum, fields[i].maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			}
			if (changed) {
				if (editor.character)
					editor.changes.parameters[i] = value;
				else
					editor.draft.parameters.values[i] = value;
			}
		}
		if (globalDetail && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", T("feature.skin.this_material_follows_global_detail_strength_and_tiling_assign_a", "This material follows global detail strength and tiling. Assign a custom detail texture to use these values."));
	}
	ImGui::Separator();
	for (size_t i = 0; i < TextureLabels().size(); ++i) {
		ScopeID scopedID(static_cast<int>(i + 100));
		ImGui::TextUnformatted(TextureLabels()[i]);
		if (editor.character) {
			int mode = static_cast<int>(editor.changes.textures[i].mode);
			const char* modes[] = { T("feature.skin.inherit_current_material", "Inherit current material"), T("feature.skin.use_custom_texture", "Use custom texture"), i == 2 ? T("feature.skin.use_global_detail", "Use global detail") : T("feature.skin.use_neutral_input", "Use neutral input") };
			if (ImGui::Combo(T("feature.skin.source", "Source"), &mode, modes, 3)) {
				auto& edit = editor.changes.textures[i];
				edit.mode = static_cast<TextureMode>(mode);
				edit.path = mode == 1 ? effective.textures[i] : "";
			}
		}
		const auto path = editor.character ? Apply(base, editor.changes).textures[i] : editor.draft.textures[i];
		ImGui::TextWrapped("%s", path.empty() ? (i == 2 ? T("feature.skin.global_detail", "Global detail") : T("feature.skin.neutral_white_multipliers_flat_wet_normal", "Neutral (white multipliers / flat wet normal)")) : path.c_str());
		if (ImGui::Button(T("feature.skin.choose_dds", "Choose DDS"))) {
			if (const auto file = ChooseFile(false, DDSFilter, L"dds")) {
				const auto imported = ImportTexture(*file);
				if (!imported.empty()) {
					if (editor.character)
						editor.changes.textures[i] = { TextureMode::Resource, imported };
					else
						editor.draft.textures[i] = imported;
					Invalidate(true);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(i == 2 ? T("feature.skin.use_global_detail", "Use global detail") : T("feature.skin.use_neutral", "Use neutral"))) {
			if (editor.character)
				editor.changes.textures[i] = { TextureMode::Default, {} };
			else
				editor.draft.textures[i].clear();
		}
		if (ImGui::TreeNode(T("feature.skin.resource_path_including_archived_dds", "Resource path (including archived DDS)"))) {
			ScopeExit treeScope{ [] { ImGui::TreePop(); } };
			std::string input = path;
			if (ImGui::InputText(T("feature.skin.dds_resource", "DDS resource"), &input)) {
				if (editor.character)
					editor.changes.textures[i] = { input.empty() ? TextureMode::Default : TextureMode::Resource, input };
				else
					editor.draft.textures[i] = input;
			}
		}
	}
	ImGui::TextWrapped("%s", T("feature.skin.controls_r_roughness_g_fuzz_b_ambient_occlusion_a_reflectance", "Controls: R roughness, G fuzz, B ambient occlusion, A reflectance. Detail RGB tiles; its alpha mask stays in the main UV layout. Transmission uses the native skin texture, not the SSS mask."));
	if (before != editor.draft || changesBefore != editor.changes) {
		if (!editor.editGesture) {
			editor.undo.push_back(before);
			editor.characterUndo.push_back(changesBefore);
			if (editor.undo.size() > 64) {
				editor.undo.erase(editor.undo.begin());
				editor.characterUndo.erase(editor.characterUndo.begin());
			}
		}
		editor.redo.clear();
		editor.characterRedo.clear();
		editor.editGesture = ImGui::IsAnyItemActive();
		Invalidate();
	} else if (!ImGui::IsAnyItemActive()) {
		editor.editGesture = false;
	}
	{
		ImGui::BeginDisabled(editor.undo.empty());
		ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.undo", "Undo"))) {
			editor.redo.push_back(editor.draft);
			editor.characterRedo.push_back(editor.changes);
			editor.draft = editor.undo.back();
			editor.undo.pop_back();
			editor.changes = editor.characterUndo.back();
			editor.characterUndo.pop_back();
			Invalidate();
		}
	}
	ImGui::SameLine();
	{
		ImGui::BeginDisabled(editor.redo.empty());
		ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.redo", "Redo"))) {
			editor.undo.push_back(editor.draft);
			editor.characterUndo.push_back(editor.changes);
			editor.draft = editor.redo.back();
			editor.redo.pop_back();
			editor.changes = editor.characterRedo.back();
			editor.characterRedo.pop_back();
			Invalidate();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.discard_draft", "Discard draft"))) {
		editor.draft = editor.baseline;
		editor.changes = editor.changesBaseline;
		editor.preview = false;
		editor.previewWetness = -1;
		editor.undo.clear();
		editor.redo.clear();
		editor.characterUndo.clear();
		editor.characterRedo.clear();
		Invalidate();
	}
	if (ImGui::CollapsingHeader(T("feature.skin.texture_channels_and_dependencies", "Texture channels and dependencies"))) {
		ImGui::Combo(T("feature.skin.texture", "Texture"), &editor.texturePreview, TextureLabels().data(), static_cast<int>(TextureLabels().size()));
		const char* channels[] = { "RGB", "R", "G", "B", T("feature.skin.alpha_main_uv_mask_for_detail", "Alpha (main UV mask for detail)") };
		ImGui::Combo(T("feature.skin.channel", "Channel"), &editor.channel, channels, 5);
		effective = editor.character ? Apply(base, editor.changes) : editor.draft;
		auto path = effective.textures[editor.texturePreview];
		if (editor.texturePreview == 2 && path.empty())
			path = settings.DetailTexture;
		const auto key = path + std::to_string(editor.channel);
		if (editor.thumbnailKey != key) {
			editor.thumbnail = {};
			editor.thumbnailKey = key;
			if (!path.empty())
				editor.thumbnail = ChannelPreview(path, editor.channel);
		}
		if (editor.thumbnail.view)
			ImGui::Image(reinterpret_cast<ImTextureID>(editor.thumbnail.view.get()), ImVec2(256, 256));
		for (size_t i = 0; i < effective.textures.size(); ++i)
			ImGui::TextWrapped("%s: %s", TextureLabels()[i], effective.textures[i].empty() ? T("feature.skin.no_per_material_dds_dependency", "No per-material DDS dependency") : effective.textures[i].c_str());
		if (ImGui::Button(T("feature.skin.check_dependencies", "Check dependencies"))) {
			CheckTextures(effective);
			editor.message = T("feature.skin.all_explicit_skin_dds_inputs_are_readable_single_2d_textures", "All explicit Skin DDS inputs are readable single 2D textures. Package them using the displayed paths.");
		}
	}
}

void Skin::DrawPresets()
{
	if (!ImGui::CollapsingHeader(T("feature.skin.material_presets", "Material presets")))
		return;
	if (ImGui::Button(T("feature.skin.save_material_preset_nif", "Save material preset NIF"))) {
		const auto material = editor.character ? Apply(geometries.at(editor.geometry.get()).base, editor.changes) : editor.draft;
		CheckTextures(material);
		if (const auto output = ChooseFile(true, NifFilter, L"nif", "SkinMaterial.nif")) {
			auto document = NifDocument::Template(material);
			document.Save(*output);
			editor.message = T("feature.skin.saved_a_material_only_template_nif_include_its_dds_dependencies", "Saved a material-only template NIF. Include its DDS dependencies when sharing it.");
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.import_preset_nif", "Import preset NIF"))) {
		if (const auto input = ChooseFile(false, NifFilter, L"nif")) {
			NifDocument document;
			document.Open(UTF8(*input));
			std::vector<Material> candidates;
			for (const auto& surface : document.Surfaces())
				if (surface.definition.status == Status::Valid)
					candidates.push_back(surface.definition.material);
			if (candidates.size() != 1)
				throw std::runtime_error(T("feature.skin.choose_a_preset_nif_with_exactly_one_advanced_skin_material", "Choose a preset NIF with exactly one Advanced Skin material."));
			editor.imported = candidates.front();
			editor.importParameters.fill(true);
			editor.importTextures.fill(true);
		}
	}
	if (!editor.imported) {
		if (ImGui::Button(T("feature.skin.import_legacy", "Import old material file (one time)"))) {
			if (const auto input = ChooseFile(false, LegacyFilter, L"json"))
				editor.legacy = ImportLegacy(UTF8(*input));
		}
		if (!editor.legacy.empty()) {
			ImGui::TextWrapped("%s", T("feature.skin.legacy_warning", "Import is approximate. RFAOS keeps its RGBA channels and adjusts the reflectance coefficient. Old wetness encoding, detail AO and assignment rules are not imported. Preview and review before saving."));
			for (size_t i = 0; i < editor.legacy.size(); ++i) {
				ScopeID scopedID(static_cast<int>(i));
				if (ImGui::Selectable(editor.legacy[i].first.c_str())) {
					editor.imported = editor.legacy[i].second;
					editor.importParameters.fill(true);
					editor.importTextures.fill(true);
				}
			}
		}
		if (!editor.imported)
			return;
	}
	ImGui::TextWrapped("%s", T("feature.skin.choose_what_to_receive_from_the_preset_nothing_is_applied", "Choose what to receive from the preset. Nothing is applied until you accept it."));
	for (size_t i = 0; i < ParameterCount; ++i) {
		ScopeID scopedID(static_cast<int>(i));
		ImGui::Checkbox(ParameterLabel(i), &editor.importParameters[i]);
	}
	for (size_t i = 0; i < TextureLabels().size(); ++i)
		ImGui::Checkbox(TextureLabels()[i], &editor.importTextures[i]);
	if (ImGui::Button(T("feature.skin.accept_selected_preset_values", "Accept selected preset values"))) {
		editor.undo.push_back(editor.draft);
		editor.characterUndo.push_back(editor.changes);
		editor.redo.clear();
		editor.characterRedo.clear();
		for (size_t i = 0; i < ParameterCount; ++i) {
			if (!editor.importParameters[i])
				continue;
			if (editor.character)
				editor.changes.parameters[i] = editor.imported->parameters.values[i];
			else
				editor.draft.parameters.values[i] = editor.imported->parameters.values[i];
		}
		for (size_t i = 0; i < TextureLabels().size(); ++i) {
			if (!editor.importTextures[i])
				continue;
			const auto& path = editor.imported->textures[i];
			if (editor.character)
				editor.changes.textures[i] = { path.empty() ? TextureMode::Default : TextureMode::Resource, path };
			else
				editor.draft.textures[i] = path;
		}
		editor.draft.enabled = true;
		editor.imported.reset();
		Invalidate();
	}
}

void Skin::DrawBatch()
{
	if (!ImGui::CollapsingHeader(T("feature.skin.batch_asset_output", "Batch asset output")))
		return;
	ImGui::TextWrapped("%s", T("feature.skin.add_explicit_bodyslide_facegen_hands_feet_or_first_person_nifs", "Add explicit BodySlide, FaceGen, hands, feet or first-person NIFs. Choose a material and output file for each. The current draft is copied to those materials only."));
	if (ImGui::Button(T("feature.skin.add_batch_nif", "Add batch NIF"))) {
		if (const auto input = ChooseFile(false, NifFilter, L"nif")) {
			Editor::BatchItem item;
			item.document.Open(UTF8(*input));
			const auto materials = item.document.Surfaces();
			for (const auto& material : materials)
				if (material.compatible && material.definition.status != Status::Invalid) {
					item.block = material.block;
					break;
				}
			if (item.block == UINT32_MAX)
				throw std::runtime_error(T("feature.skin.no_editable_skin_material_in_this_batch_input", "No editable skin material in this batch input."));
			if (const auto output = ChooseFile(true, NifFilter, L"nif", input->filename())) {
				item.output = *output;
				editor.batch.push_back(std::move(item));
			}
		}
	}
	std::optional<size_t> remove;
	for (size_t i = 0; i < editor.batch.size(); ++i) {
		auto& item = editor.batch[i];
		ScopeID scopedID(static_cast<int>(i));
		ImGui::TextWrapped("%s -> %s", item.document.Source().c_str(), UTF8(item.output).c_str());
		const auto blockLabel = I18n::GetSingleton()->Format("feature.skin.block_number", { { "block", std::to_string(item.block) } }, "Block {block}");
		if (ImGui::BeginCombo(T("feature.skin.material", "Material"), blockLabel.c_str())) {
			ScopeExit comboScope{ [] { ImGui::EndCombo(); } };
			for (const auto& material : item.document.Surfaces())
				if (material.compatible && material.definition.status != Status::Invalid && ImGui::Selectable(MaterialName(material).c_str(), item.block == material.block))
					item.block = material.block;
		}
		if (ImGui::SmallButton(T("feature.skin.remove", "Remove")))
			remove = i;
	}
	if (remove)
		editor.batch.erase(editor.batch.begin() + *remove);
	{
		ImGui::BeginDisabled(editor.batch.empty());
		ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.publish_draft_to_listed_nifs", "Publish draft to listed NIFs"))) {
			const auto material = editor.character ? Apply(geometries.at(editor.geometry.get()).base, editor.changes) : editor.draft;
			CheckTextures(material);
			std::vector<Output> outputs;
			for (const auto& item : editor.batch) {
				auto document = item.document;
				document.SetMaterial(item.block, material);
				outputs.push_back({ item.output, document.VerifiedBytes() });
			}
			Publish(outputs);
			editor.batch.clear();
			editor.message = T("feature.skin.published_all_listed_nifs_with_verified_staged_output_and_recovery", "Published all listed NIFs with verified staged output and recovery backups. Regenerating BodySlide or FaceGen may overwrite generated assets.");
			Invalidate(true);
		}
	}
}

void Skin::DrawTextureTools()
{
	if (!ImGui::CollapsingHeader(T("feature.skin.build_a_texture", "Build a texture")))
		return;
	ImGui::Checkbox(T("feature.skin.detail_normal_region_mask", "Detail normal + region mask"), &editor.tools.detail);
	if (editor.tools.detail) {
		PathPicker(T("feature.skin.rgb_tangent_space_normal", "RGB tangent-space normal"), editor.tools.normal);
		PathPicker(T("feature.skin.main_uv_region_mask_white_by_default", "Main-UV region mask (white by default)"), editor.tools.mask);
		ImGui::TextWrapped("%s", T("feature.skin.rgb_repeats_with_detail_tiling_the_mask_uses_the_model", "RGB repeats with detail tiling. The mask uses the model's main UV. Empty mask writes white; old detail AO is discarded."));
	} else {
		const char* labels[] = { T("feature.skin.r_roughness_multiplier", "R: roughness multiplier"), T("feature.skin.g_fuzz_multiplier", "G: fuzz multiplier"), T("feature.skin.b_ambient_occlusion", "B: ambient occlusion"), T("feature.skin.a_reflectance_multiplier", "A: reflectance multiplier") };
		for (size_t i = 0; i < 4; ++i) {
			ScopeID scopedID(static_cast<int>(i));
			PathPicker(labels[i], editor.tools.channels[i]);
			if (editor.tools.channels[i].empty())
				ImGui::SliderFloat(T("feature.skin.constant", "Constant"), &editor.tools.defaults[i], 0, 1);
		}
		ImGui::TextWrapped("%s", T("feature.skin.use_grayscale_images_of_equal_dimensions_the_red_channel_of", "Use grayscale images of equal dimensions. The red channel of each input is copied into the selected output channel."));
	}
	ImGui::Checkbox(T("feature.skin.compress_as_bc7_slower", "Compress as BC7 (slower)"), &editor.tools.compress);
	if (ImGui::Button(T("feature.skin.build_and_save_dds", "Build and save DDS"))) {
		if (const auto output = ChooseFile(true, DDSFilter, L"dds", editor.tools.detail ? "detail_normal_mask.dds" : "skin_controls.dds")) {
			const Output file{ *output, editor.tools.detail ? PackDetail(editor.tools) : PackControls(editor.tools) };
			Publish(std::span(&file, 1));
			editor.message = T("feature.skin.dds_saved_with_linear_channels_alpha_and_mipmaps_choose_dds", "DDS saved with linear channels, alpha and mipmaps. Choose DDS on the material to assign it.");
		}
	}
}
