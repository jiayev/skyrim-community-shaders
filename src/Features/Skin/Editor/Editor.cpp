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
	int ParameterSection(Parameter parameter)
	{
		switch (parameter) {
		case Parameter::Fuzz:
		case Parameter::FuzzRoughness:
		case Parameter::FuzzF0:
		case Parameter::ExtraEdgeRoughness:
			return 2;
		case Parameter::SSSAmount:
		case Parameter::Transmission:
		case Parameter::TransmissionDepth:
		case Parameter::TransmissionEnabled:
			return 3;
		case Parameter::DetailEnabled:
		case Parameter::DetailStrength:
		case Parameter::DetailTiling:
		case Parameter::BodyTilingMultiplier:
			return 4;
		case Parameter::WetResponse:
			return 5;
		default:
			return 1;
		}
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
	if ((EditorDirty() || editor.transfer) && !editor.discardConfirmed)
		editor.pendingAction = std::move(a_action);
	else
		a_action();
}

void Skin::SelectReference(RE::TESObjectREFR* a_reference, bool a_firstPerson, bool a_previewOnly)
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
		auto& entry = Observe(surface.get(), surface->GetGeometryRuntimeData().shaderProperty.get());
		if (auto* actor = a_reference->As<RE::Actor>())
			entry.actor = actor->GetHandle();
		entry.inspect = true;
		Prepare(entry, characters);
	}
	if (!a_previewOnly)
		if (auto* actor = a_reference->As<RE::Actor>())
			for (const auto& surface : DiscoverActor(actor))
				if (std::find(surfaces.begin(), surfaces.end(), surface) == surfaces.end())
					surfaces.push_back(surface);
	if (a_previewOnly) {
		editor.surfaces = std::move(surfaces);
		editor.geometry = editor.surfaces.front();
		editor.previewWetness = -1;
		Invalidate();
	} else {
		ChangeEditor([this, surfaces = std::move(surfaces)] {
			editor.surfaces = surfaces;
			SelectSurface(surfaces.front().get());
		});
	}
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
	editor.workspace = Editor::Workspace::Character;
	editor.step = 0;
	editor.document.reset();
	editor.materials.clear();
	editor.selectedBlock = UINT32_MAX;
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
	editor.workspace = Editor::Workspace::Asset;
	editor.step = 0;
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
	editor.error.clear();
	Invalidate();
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
	auto effective = editor.character ? Apply(base, editor.changes) : Resolve(editor.draft, settings.DefaultProfile);
	if (!editor.character)
		ImGui::Checkbox(T("feature.skin.customize_material", "Customize this material"), &editor.draft.enabled);
	ImGui::TextWrapped("%s", T("feature.skin.inherit_material_parameters", "Unchecked values inherit the current material and common skin settings."));
	const char* sections[] = { T("feature.skin.section_textures", "Textures"), T("feature.skin.section_specular", "Dual specular lobes"), T("feature.skin.section_fuzz", "Fuzz and edges"), T("feature.skin.section_transmission", "Transmission and SSS"), T("feature.skin.section_detail", "Detail normals"), T("feature.skin.section_wetness", "Wet response") };
	const bool wide = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 44;
	const bool columns = wide && ImGui::BeginTable("##skin_inspector", 2, ImGuiTableFlags_SizingStretchProp);
	ScopeExit inspector{ [columns] { if (columns) ImGui::EndTable(); } };
	if (columns) {
		ImGui::TableSetupColumn("##sections", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12);
		ImGui::TableSetupColumn("##parameters", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextColumn();
		for (int i = 0; i < 6; ++i)
			if (ImGui::Selectable(sections[i], editor.materialSection == i))
				editor.materialSection = i;
		ImGui::TableNextColumn();
	} else {
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::Combo("##skin_section", &editor.materialSection, sections, 6);
	}
	ImGui::SeparatorText(sections[editor.materialSection]);
	if (editor.materialSection == 1)
		ImGui::TextWrapped("%s", effective.textures[0].empty() ? T("feature.skin.specular_scalar_mode", "No RFAOS: the two lobes use their own roughness values and the native specular-map control. RFAOS multipliers are retained for use when a controls texture is assigned.") : T("feature.skin.specular_texture_mode", "RFAOS assigned: its red channel is multiplied separately for each lobe. Absolute roughness values are retained for use without RFAOS."));
	if (editor.materialSection == 3)
		ImGui::TextWrapped("%s", T("feature.skin.transmission_sss_help", "Transmission controls backlighting through the native thickness texture. SSS strength controls the separate post-process effect."));
	if (editor.materialSection == 4)
		ImGui::TextWrapped("%s", T("feature.skin.detail_section_help", "Detail RGB repeats; the alpha strength mask stays in the main UV layout. Empty texture slots use the common detail texture. Unchecked parameters follow common settings."));
	if (editor.materialSection == 5)
		ImGui::TextWrapped("%s", T("feature.skin.wet_section_help", "Set how strongly this skin responds to wetness. Overall wetness, sweat and noise controls remain in the Advanced Skin feature settings."));
	const auto fields = ParameterTable();
	for (const auto parameter : ParameterOrder) {
		const auto i = static_cast<size_t>(parameter);
		if (ParameterSection(parameter) != editor.materialSection)
			continue;
		ScopeID scopedID(static_cast<int>(i));
		ImGui::TextWrapped("%s", ParameterLabel(i));
		bool overridden = editor.character ? editor.changes.parameters[i].has_value() : editor.draft.specified[i];
		if (ImGui::Checkbox(T("feature.skin.override", "Override"), &overridden)) {
			if (editor.character) {
				editor.changes.parameters[i] = overridden ? std::optional(effective.parameters.values[i]) : std::nullopt;
			} else {
				editor.draft.enabled = true;
				editor.draft.specified[i] = overridden;
				editor.draft.parameters.values[i] = overridden ? effective.parameters.values[i] : fields[i].initial;
			}
		}
		ImGui::SameLine();
		{
			ImGui::BeginDisabled(!overridden || (!editor.character && !editor.draft.enabled));
			ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
			float value = effective.parameters.values[i];
			bool changed;
			if (fields[i].integer) {
				bool flag = value != 0;
				changed = ImGui::Checkbox(T("feature.skin.enabled_value", "Enabled"), &flag);
				value = flag ? 1.0f : 0.0f;
			} else {
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed = ImGui::SliderFloat("##value", &value, fields[i].minimum, fields[i].maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			}
			if (changed) {
				if (editor.character)
					editor.changes.parameters[i] = value;
				else
					editor.draft.parameters.values[i] = value;
			}
		}
	}
	if (editor.materialSection == 0) {
		ImGui::Separator();
		for (size_t i = 0; i < TextureLabels().size(); ++i) {
			ScopeID scopedID(static_cast<int>(i + 100));
			ImGui::SeparatorText(TextureLabels()[i]);
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
			ImGui::TextWrapped("%s", path.empty() ? (i == 2 ? T("feature.skin.global_detail", "Global detail") : T("feature.skin.no_control_texture", "No texture (scalar roughness / flat wet normal)")) : path.c_str());
			if (ImGui::Button(T("feature.skin.choose_dds", "Choose DDS"))) {
				if (const auto file = ChooseFile(false, DDSFilter, L"dds")) {
					const auto imported = ImportTexture(*file);
					if (!imported.empty()) {
						if (editor.character)
							editor.changes.textures[i] = { TextureMode::Resource, imported };
						else {
							editor.draft.textures[i] = imported;
							editor.draft.enabled = true;
						}
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
					else {
						editor.draft.textures[i] = input;
						editor.draft.enabled = true;
					}
				}
			}
		}
		ImGui::TextWrapped("%s", T("feature.skin.controls_r_roughness_g_fuzz_b_ambient_occlusion_a_reflectance", "RFAOS: R roughness uses separate multipliers for both specular lobes; G fuzz, B ambient occlusion, A specular. Without RFAOS, each lobe uses its own roughness value. Detail alpha stays in the main UV layout. Transmission uses the native skin texture."));
		if (ImGui::Button(T("feature.skin.open_texture_tools", "Build a packed texture..."))) {
			editor.workspace = Editor::Workspace::Textures;
			Invalidate();
		}
	}
	if (before != editor.draft || changesBefore != editor.changes) {
		editor.message.clear();
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
	if (editor.materialSection == 0 && ImGui::CollapsingHeader(T("feature.skin.texture_channels_and_dependencies", "Texture channels and dependencies"))) {
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
			ImGui::Image(reinterpret_cast<ImTextureID>(editor.thumbnail.view.get()), ImVec2(std::min(256.0f, ImGui::GetContentRegionAvail().x), std::min(256.0f, ImGui::GetContentRegionAvail().x)));
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
			ImGui::TextWrapped("%s", T("feature.skin.legacy_warning", "Imports skin parameters and RFAOS channels. Old wetness encoding, detail AO and assignment rules are not imported. Preview and review before saving."));
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
				editor.changes.parameters[i] = editor.imported->specified[i] ? std::optional(editor.imported->parameters.values[i]) : std::nullopt;
			else {
				editor.draft.parameters.values[i] = editor.imported->parameters.values[i];
				editor.draft.specified[i] = editor.imported->specified[i];
			}
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
	ImGui::SeparatorText(T("feature.skin.build_a_texture", "Build a texture"));
	ImGui::Checkbox(T("feature.skin.detail_normal_region_mask", "Detail normal + region mask"), &editor.tools.detail);
	if (editor.tools.detail) {
		PathPicker(T("feature.skin.rgb_tangent_space_normal", "RGB tangent-space normal"), editor.tools.normal);
		PathPicker(T("feature.skin.main_uv_region_mask_white_by_default", "Main-UV region mask (white by default)"), editor.tools.mask);
		ImGui::TextWrapped("%s", T("feature.skin.rgb_repeats_with_detail_tiling_the_mask_uses_the_model", "RGB repeats with detail tiling. The mask uses the model's main UV. Empty mask writes white; old detail AO is discarded."));
	} else {
		const char* labels[] = { T("feature.skin.r_roughness", "R: roughness"), T("feature.skin.g_fuzz_multiplier", "G: fuzz multiplier"), T("feature.skin.b_ambient_occlusion", "B: ambient occlusion"), T("feature.skin.a_specular", "A: specular") };
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
			editor.generatedDDS = UTF8(*output);
			editor.generatedDetail = editor.tools.detail;
			editor.message = T("feature.skin.dds_saved_with_linear_channels_alpha_and_mipmaps_choose_dds", "DDS saved with linear channels, alpha and mipmaps. Choose DDS on the material to assign it.");
		}
	}
	if (!editor.generatedDDS.empty()) {
		ImGui::SeparatorText(T("feature.skin.texture_output", "Created texture"));
		ImGui::TextWrapped("%s", editor.generatedDDS.c_str());
		const bool hasDraft = editor.character ? bool(editor.geometry) : editor.document && editor.selectedBlock != UINT32_MAX;
		ImGui::BeginDisabled(!hasDraft || bool(editor.transfer));
		ScopeExit disabled{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.assign_created_texture", "Use this texture in the current material"))) {
			const auto path = ImportTexture(std::filesystem::u8path(editor.generatedDDS));
			if (!path.empty()) {
				editor.undo.push_back(editor.draft);
				editor.characterUndo.push_back(editor.changes);
				editor.redo.clear();
				editor.characterRedo.clear();
				const size_t slot = editor.generatedDetail ? 2 : 0;
				if (editor.character)
					editor.changes.textures[slot] = { TextureMode::Resource, path };
				else {
					editor.draft.enabled = true;
					editor.draft.textures[slot] = path;
				}
				editor.workspace = editor.character ? Editor::Workspace::Character : Editor::Workspace::Asset;
				editor.step = 1;
				editor.materialSection = 0;
				editor.message = T("feature.skin.texture_assigned_to_draft", "Texture assigned to the draft. Preview and save the material to finish.");
				Invalidate(true);
			}
		}
	}
}

void Skin::DrawEditorHistory()
{
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
		editor.previewWetness = -1;
		editor.undo.clear();
		editor.redo.clear();
		editor.characterUndo.clear();
		editor.characterRedo.clear();
		Invalidate();
	}
}
