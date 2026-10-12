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
	struct ScopeID
	{
		explicit ScopeID(int a_id) { ImGui::PushID(a_id); }
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

void Skin::PushUndo(const Editor::Draft& a_before)
{
	editor.undo.push_back(a_before);
	if (editor.undo.size() > 64)
		editor.undo.erase(editor.undo.begin());
	editor.redo.clear();
}

void Skin::DrawMaterialControls(Layer a_layer)
{
	const Editor::Draft before{ editor.material, editor.character };
	const auto& key = editor.key;
	SkinMaterials::Material inherited;
	switch (a_layer) {
	case Layer::Material:
		inherited = Resolve({}, settings.DefaultProfile);
		break;
	case Layer::CharacterAll:
		inherited = Resolve(BaseMaterial(key), settings.DefaultProfile);
		break;
	default:
		inherited = Resolve(Apply(BaseMaterial(key), ScalarsOnly(editor.character.all)), settings.DefaultProfile);
		break;
	}
	auto& changes = a_layer == Layer::Material ? editor.material : a_layer == Layer::CharacterAll ? editor.character.all :
	                                                                                                editor.character.materials[key];
	auto effective = Apply(inherited, changes);

	ImGui::TextWrapped("%s", a_layer == Layer::Material ? T("feature.skin.inherit_common_parameters", "Checked values are stored in this texture's material. Unchecked values follow the common skin settings.") : a_layer == Layer::CharacterAll ? T("feature.skin.inherit_character_all", "Checked values apply to every skin texture of this character. Unchecked values follow each texture's material.") :
																																																													T("feature.skin.inherit_character_material", "Checked values apply to this character while it shows this texture. Unchecked values follow the texture's material and the character's whole-skin adjustments."));
	const char* sections[] = { T("feature.skin.section_textures", "Textures"), T("feature.skin.section_specular", "Dual specular lobes"), T("feature.skin.section_fuzz", "Fuzz and edges"), T("feature.skin.section_transmission", "Transmission and SSS"), T("feature.skin.section_detail", "Detail normals"), T("feature.skin.section_wetness", "Wet response") };
	const bool wide = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 44;
	const bool columns = wide && ImGui::BeginTable("##skin_inspector", 2, ImGuiTableFlags_SizingStretchProp);
	{
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
			bool overridden = changes.parameters[i].has_value();
			if (ImGui::Checkbox(T("feature.skin.override", "Override"), &overridden))
				changes.parameters[i] = overridden ? std::optional(effective.parameters.values[i]) : std::nullopt;
			ImGui::SameLine();
			ImGui::BeginDisabled(!overridden);
			float value = changes.parameters[i].value_or(inherited.parameters.values[i]);
			bool changed;
			if (fields[i].integer) {
				bool flag = value != 0;
				changed = ImGui::Checkbox(T("feature.skin.enabled_value", "Enabled"), &flag);
				value = flag ? 1.0f : 0.0f;
			} else {
				ImGui::SetNextItemWidth(-FLT_MIN);
				changed = ImGui::SliderFloat("##value", &value, fields[i].minimum, fields[i].maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			}
			ImGui::EndDisabled();
			if (changed && overridden)
				changes.parameters[i] = value;
		}
		if (editor.materialSection == 0) {
			if (a_layer == Layer::CharacterAll) {
				ImGui::TextWrapped("%s", T("feature.skin.character_all_no_textures", "Textures are painted for one texture's UV layout, so they cannot apply to every skin of a character. Choose \"Only this skin texture\" to set them."));
			} else {
				for (size_t i = 0; i < TextureCount; ++i) {
					ScopeID scopedID(static_cast<int>(i + 100));
					ImGui::SeparatorText(TextureLabels()[i]);
					auto& edit = changes.textures[i];
					if (a_layer == Layer::CharacterMaterial) {
						int mode = static_cast<int>(edit.mode);
						const char* modes[] = { T("feature.skin.inherit_texture_material", "Use the texture's material"), T("feature.skin.use_custom_texture", "Use custom texture"), i == 2 ? T("feature.skin.use_global_detail", "Use global detail") : T("feature.skin.use_neutral_input", "Use neutral input") };
						if (ImGui::Combo(T("feature.skin.source", "Source"), &mode, modes, 3)) {
							edit.mode = static_cast<TextureMode>(mode);
							edit.path = mode == 1 ? Apply(inherited, changes).textures[i] : "";
							if (edit.mode == TextureMode::Resource && edit.path.empty())
								edit.mode = TextureMode::Inherit;
						}
					}
					const auto path = Apply(inherited, changes).textures[i];
					ImGui::TextWrapped("%s", path.empty() ? (i == 2 ? T("feature.skin.global_detail", "Global detail") : T("feature.skin.no_control_texture", "No texture (scalar roughness / flat wet normal)")) : path.c_str());
					if (ImGui::Button(T("feature.skin.choose_dds", "Choose DDS"))) {
						if (const auto file = ChooseFile(false, DDSFilter, L"dds")) {
							const auto imported = ImportTexture(*file);
							if (!imported.empty()) {
								edit = { TextureMode::Resource, imported };
								Invalidate(true);
							}
						}
					}
					ImGui::SameLine();
					if (ImGui::Button(i == 2 ? T("feature.skin.use_global_detail", "Use global detail") : T("feature.skin.use_neutral", "Use neutral")))
						edit = a_layer == Layer::Material ? TextureEdit{} : TextureEdit{ TextureMode::Default, {} };
					if (ImGui::TreeNode(T("feature.skin.resource_path_including_archived_dds", "Resource path (including archived DDS)"))) {
						ScopeExit treeScope{ [] { ImGui::TreePop(); } };
						std::string input = path;
						if (ImGui::InputText(T("feature.skin.dds_resource", "DDS resource"), &input))
							edit = input.empty() ? (a_layer == Layer::Material ? TextureEdit{} : TextureEdit{ TextureMode::Default, {} }) : TextureEdit{ TextureMode::Resource, input };
					}
				}
				ImGui::TextWrapped("%s", T("feature.skin.controls_r_roughness_g_fuzz_b_ambient_occlusion_a_reflectance", "RFAOS: R roughness uses separate multipliers for both specular lobes; G fuzz, B ambient occlusion, A specular. Without RFAOS, each lobe uses its own roughness value. Detail alpha stays in the main UV layout. Transmission uses the native skin texture."));
				if (ImGui::Button(T("feature.skin.open_texture_tools", "Build a packed texture..."))) {
					editor.returnWorkspace = editor.workspace;
					editor.workspace = Editor::Workspace::Textures;
				}
			}
		}
	}
	if (a_layer == Layer::CharacterMaterial && changes.Empty())
		editor.character.materials.erase(key);
	if (before != Editor::Draft{ editor.material, editor.character }) {
		editor.message.clear();
		if (!editor.editGesture) {
			editor.undo.push_back(before);
			if (editor.undo.size() > 64)
				editor.undo.erase(editor.undo.begin());
		}
		editor.redo.clear();
		editor.editGesture = ImGui::IsAnyItemActive();
		Invalidate();
	} else if (!ImGui::IsAnyItemActive()) {
		editor.editGesture = false;
	}
	if (editor.materialSection == 0 && a_layer != Layer::CharacterAll) {
		const auto* current = a_layer == Layer::Material ? &editor.material : nullptr;
		if (!current) {
			const auto found = editor.character.materials.find(key);
			current = found == editor.character.materials.end() ? nullptr : &found->second;
		}
		DrawTextureChannels(current ? Apply(inherited, *current) : inherited);
	}
}

void Skin::DrawTextureChannels(const SkinMaterials::Material& a_material)
{
	if (!ImGui::CollapsingHeader(T("feature.skin.texture_channels_and_dependencies", "Texture channels and dependencies")))
		return;
	ImGui::Combo(T("feature.skin.texture", "Texture"), &editor.texturePreview, TextureLabels().data(), static_cast<int>(TextureLabels().size()));
	const char* channels[] = { "RGB", "R", "G", "B", T("feature.skin.alpha_main_uv_mask_for_detail", "Alpha (main UV mask for detail)") };
	ImGui::Combo(T("feature.skin.channel", "Channel"), &editor.channel, channels, 5);
	auto path = a_material.textures[editor.texturePreview];
	if (editor.texturePreview == 2 && path.empty())
		path = settings.DetailTexture;
	const auto key = path + std::to_string(editor.channel);
	if (editor.thumbnailKey != key) {
		editor.thumbnail = {};
		editor.thumbnailKey = key;
		if (!path.empty())
			editor.thumbnail = ChannelPreview(path, editor.channel);
	}
	if (editor.thumbnail.view) {
		const float size = std::min(256.0f, ImGui::GetContentRegionAvail().x);
		ImGui::Image(reinterpret_cast<ImTextureID>(editor.thumbnail.view.get()), ImVec2(size, size));
	}
	for (size_t i = 0; i < a_material.textures.size(); ++i)
		ImGui::TextWrapped("%s: %s", TextureLabels()[i], a_material.textures[i].empty() ? T("feature.skin.no_per_material_dds_dependency", "No per-material DDS dependency") : a_material.textures[i].c_str());
	if (ImGui::Button(T("feature.skin.check_dependencies", "Check dependencies"))) {
		CheckTextures(a_material);
		editor.message = T("feature.skin.all_explicit_skin_dds_inputs_are_readable_single_2d_textures", "All explicit Skin DDS inputs are readable single 2D textures. Package them using the displayed paths.");
	}
}

void Skin::DrawPreviewOptions()
{
	ImGui::SeparatorText(T("feature.skin.preview_section_v2", "Preview"));
	ImGui::TextWrapped("%s", T("feature.skin.preview_automatic_v2", "While this editor is open, a texture material draft previews on everything that shows the texture, and a character draft previews on that character."));
	bool overrideWetness = editor.previewWetness >= 0;
	if (ImGui::Checkbox(T("feature.skin.preview_override_wetness", "Test a specific wetness level"), &overrideWetness))
		editor.previewWetness = overrideWetness ? 0.0f : -1.0f;
	if (overrideWetness)
		ImGui::SliderFloat(T("feature.skin.preview_wetness", "Preview wetness"), &editor.previewWetness, 0, 1);
	else
		ImGui::TextDisabled("%s", T("feature.skin.preview_live_wetness", "Using the character's current wetness."));
	if (ImGui::Button(T("feature.skin.reload_skin_resources", "Reload skin resources")))
		ReloadResources();
	ImGui::SameLine();
	ImGui::TextDisabled("%s", T("feature.skin.reload_skin_resources_help_v2", "Re-reads material files, character files and DDS inputs changed outside the game."));
}

void Skin::DrawTextureTools()
{
	ImGui::TextWrapped("%s", T("feature.skin.texture_tool_flow", "Choose a texture type, provide source images, then build a DDS. Return to Adjust material to assign it; your material draft is retained."));
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
	if (editor.generatedDDS.empty())
		return;
	ImGui::SeparatorText(T("feature.skin.texture_output", "Created texture"));
	ImGui::TextWrapped("%s", editor.generatedDDS.c_str());
	const bool character = editor.returnWorkspace == Editor::Workspace::Character;
	const bool hasDraft = character ? editor.draftTarget && !editor.characterAll && !editor.key.empty() : !editor.draftKey.empty();
	ImGui::BeginDisabled(!hasDraft);
	ScopeExit disabled{ [] { ImGui::EndDisabled(); } };
	if (ImGui::Button(T("feature.skin.assign_created_texture", "Use this texture in the current material"))) {
		const auto path = ImportTexture(std::filesystem::u8path(editor.generatedDDS));
		if (!path.empty()) {
			PushUndo({ editor.material, editor.character });
			const size_t slot = editor.generatedDetail ? 2 : 0;
			if (character)
				editor.character.materials[editor.key].textures[slot] = { TextureMode::Resource, path };
			else
				editor.material.textures[slot] = { TextureMode::Resource, path };
			editor.workspace = editor.returnWorkspace;
			editor.materialSection = 0;
			editor.message = T("feature.skin.texture_assigned_to_draft", "Texture assigned to the draft. Preview and save the material to finish.");
			Invalidate(true);
		}
	}
}

void Skin::DrawEditorHistory()
{
	const Editor::Draft current{ editor.material, editor.character };
	{
		ImGui::BeginDisabled(editor.undo.empty());
		ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.undo", "Undo"))) {
			editor.redo.push_back(current);
			editor.material = editor.undo.back().material;
			editor.character = editor.undo.back().character;
			editor.undo.pop_back();
			Invalidate();
		}
	}
	ImGui::SameLine();
	{
		ImGui::BeginDisabled(editor.redo.empty());
		ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.redo", "Redo"))) {
			editor.undo.push_back(current);
			editor.material = editor.redo.back().material;
			editor.character = editor.redo.back().character;
			editor.redo.pop_back();
			Invalidate();
		}
	}
	ImGui::SameLine();
	{
		ImGui::BeginDisabled(!EditorDirty());
		ScopeExit disabledScope{ [] { ImGui::EndDisabled(); } };
		if (ImGui::Button(T("feature.skin.discard_draft", "Discard draft"))) {
			PushUndo(current);
			editor.material = editor.materialBaseline;
			editor.character = editor.characterBaseline;
			Invalidate();
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%s", MaterialDirty() ? T("feature.skin.material_draft_unsaved", "Texture material: unsaved") : T("feature.skin.material_draft_clean", "Texture material: saved"));
	ImGui::SameLine();
	ImGui::TextDisabled("/ %s", CharacterDirty() ? T("feature.skin.character_draft_unsaved", "Character: unsaved") : T("feature.skin.character_draft_saved", "Character: saved"));
}

void Skin::DrawEditorDialogs()
{
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
}
