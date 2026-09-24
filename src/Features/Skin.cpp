#include "Skin.h"
#include <DirectXTex.h>
#include <imgui_stdlib.h>

#include "Globals.h"
#include "State.h"
#include "Utils/UI.h"

#include <cmath>
#include <fstream>

using namespace SkinMaterials;

namespace
{
	std::array<const char*, ParameterCount> ParameterLabels()
	{
		return {
			T("feature.skin.primary_roughness", "Primary Roughness"),
			T("feature.skin.secondary_roughness", "Secondary Roughness"),
			T("feature.skin.specular_texture_multiplier", "Specular Texture Multiplier"),
			T("feature.skin.secondary_specular_strength", "Secondary Specular Strength"),
			T("feature.skin.fresnel_f0", "Fresnel F0"),
			T("feature.skin.base_color_multiplier", "Base Color Multiplier"),
			T("feature.skin.physical_main_roughness_multiplier", "Physical Main Roughness Multiplier"),
			T("feature.skin.physical_second_roughness_multiplier", "Physical Second Roughness Multiplier"),
			T("feature.skin.physical_specular_multiplier", "Physical Specular Multiplier"),
			T("feature.skin.extra_edge_roughness", "Extra Edge Roughness"),
			T("feature.skin.enable_skin_detail", "Enable Skin Detail"),
			T("feature.skin.skin_detail_strength", "Skin Detail Strength"),
			T("feature.skin.skin_detail_tiling", "Skin Detail Tiling"),
			T("feature.skin.body_tiling_multiplier", "Body Tiling Multiplier"),
			T("feature.skin.translucency", "Translucency"),
			T("feature.skin.sss_width", "SSS Width"),
			T("feature.skin.enable_sss_transmission", "Enable SSS Transmission"),
			T("feature.skin.fuzz_strength", "Fuzz Strength"),
			T("feature.skin.fuzz_roughness", "Fuzz Roughness"),
			T("feature.skin.fuzz_f0", "Fuzz F0")
		};
	}
	/** @brief Returns the player's world-space NiCamera, or null. */
	RE::NiCamera* GetPlayerNiCamera()
	{
		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		if (!playerCamera || !playerCamera->cameraRoot)
			return nullptr;
		for (auto& child : playerCamera->cameraRoot->GetChildren()) {
			if (auto* camera = netimmerse_cast<RE::NiCamera*>(child.get()))
				return camera;
		}
		return nullptr;
	}

	/** @brief Raycasts through the screen center and returns the hit reference, or null. */
	RE::TESObjectREFR* PickCrosshairRef()
	{
		auto* niCamera = GetPlayerNiCamera();
		if (!niCamera)
			return nullptr;

		const auto display = ImGui::GetIO().DisplaySize;
		if (display.x <= 0.0f || display.y <= 0.0f)
			return nullptr;

		RE::NiPoint3 origin, dir;
		if (!niCamera->WindowPointToRay(static_cast<std::int32_t>(display.x * 0.5f), static_cast<std::int32_t>(display.y * 0.5f),
				origin, dir, display.x, display.y))
			return nullptr;
		dir.Unitize();

		constexpr float kSkyrimToHavok = 0.0142875f;
		constexpr float kRayLength = 100000.0f;
		const RE::NiPoint3 end = origin + dir * kRayLength;

		RE::bhkPickData pick{};
		pick.rayInput.from = RE::hkVector4(origin.x * kSkyrimToHavok, origin.y * kSkyrimToHavok, origin.z * kSkyrimToHavok, 0.0f);
		pick.rayInput.to = RE::hkVector4(end.x * kSkyrimToHavok, end.y * kSkyrimToHavok, end.z * kSkyrimToHavok, 0.0f);

		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return nullptr;
		tes->Pick(pick);

		if (!pick.rayOutput.rootCollidable)
			return nullptr;
		return RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
	}
}

void Skin::DrawParameters(SkinProfile& a_profile, Patch* a_patch)
{
	const auto labels = ParameterLabels();
	const auto fields = ParameterTable();
	for (size_t i = 0; i < fields.size(); ++i) {
		const auto& field = fields[i];
		ImGui::PushID(field.name);
		bool enabled = !a_patch || (*a_patch)[i].has_value();
		if (a_patch) {
			ImGui::Checkbox("##override", &enabled);
			ImGui::SameLine();
		}
		ImGui::BeginDisabled(!enabled);
		if (field.toggle)
			ImGui::Checkbox(labels[i], &(a_profile.*field.toggle));
		else
			ImGui::SliderFloat(labels[i], &(a_profile.*field.number), static_cast<float>(field.minimum), static_cast<float>(field.maximum), "%.4f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndDisabled();
		if (a_patch) {
			if (enabled)
				(*a_patch)[i] = field.toggle ? double(a_profile.*field.toggle) : double(a_profile.*field.number);
			else
				(*a_patch)[i].reset();
		}
		ImGui::PopID();
	}
}

void Skin::DrawSettings()
{
	std::lock_guard lock(mutex);
	ImGui::Checkbox(T("feature.skin.enable_advanced_skin", "Enable Advanced Skin"), &settings.EnableSkin);
	ImGui::TextWrapped("%s", T("feature.skin.asset_workflow", "Materials belong to NIF surfaces or confirmed texture-set records. Legacy path rules and automatic texture discovery are inactive."));
	if (ImGui::CollapsingHeader(T("feature.skin.default_material", "Default Material"))) {
		const auto previous = ToJSON(settings.DefaultProfile);
		ImGui::PushID("default");
		DrawParameters(settings.DefaultProfile);
		ImGui::PopID();
		if (previous != ToJSON(settings.DefaultProfile))
			Invalidate();
	}
	if (ImGui::CollapsingHeader(T("feature.skin.global_section", "Wetness (Global)")))
		DrawGlobalSettings();
	if (ImGui::CollapsingHeader(T("feature.skin.material_editor", "Surface Material Editor"), ImGuiTreeNodeFlags_DefaultOpen))
		DrawMaterialEditor();
	if (ImGui::CollapsingHeader(T("feature.skin.saved_edits", "Saved User Edits"))) {
		std::unordered_set<std::string> observed;
		for (const auto& [geometry, entry] : geometries) {
			if (entry.surface.HasSurface())
				observed.insert(entry.surface.Key());
			if (auto npc = GetFormKey(RE::TESForm::LookupByID(entry.npc))) {
				Target target;
				target.npc = npc;
				observed.insert(target.Key());
				if (entry.surface.HasSurface()) {
					target = entry.surface;
					target.npc = npc;
					observed.insert(target.Key());
				}
			}
		}
		std::optional<Target> removed;
		for (const auto& [key, edit] : materials.UserEdits()) {
			ImGui::PushID(key.c_str());
			ImGui::TextWrapped("%s", key.c_str());
			if (!observed.contains(key))
				ImGui::TextDisabled("%s", T("feature.skin.edit_not_observed", "No matching target observed in recent draws. Asset identity changes require explicit reassignment."));
			ImGui::BeginDisabled(HasUnsavedEdit());
			if (ImGui::Button(T("feature.skin.delete_user_edit", "Delete This User Edit")))
				removed = edit.target;
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		if (removed) {
			try {
				materials.DeleteEdit(*removed);
				BeginEdit();
			} catch (const std::exception& e) {
				editor.error = e.what();
			}
		}
	}
	if (ImGui::Button(T("feature.skin.reload_material_assets", "Reload Materials and Textures"))) {
		materials.Refresh(true);
		ReloadSkinDetail();
	}
	for (const auto& diagnostic : materials.Diagnostics())
		ImGui::TextWrapped("%s", diagnostic.c_str());
}

void Skin::SelectReference(RE::TESObjectREFR* a_ref)
{
	editor.preview = false;
	++editor.revision;
	editor.geometry.reset();
	editor.surfaces.clear();
	if (!a_ref || !a_ref->Get3D())
		return;
	RE::BSVisit::TraverseScenegraphGeometries(a_ref->Get3D(), [&](RE::BSGeometry* geometry) {
		auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
		auto* material = property ? property->GetBaseMaterial() : nullptr;
		if (material && material->GetType() == RE::BSShaderMaterial::Type::kLighting &&
			(material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGen || material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint))
			editor.surfaces.emplace_back(geometry);
		return RE::BSVisit::BSVisitControl::kContinue;
	});
	if (!editor.surfaces.empty())
		SelectSurface(editor.surfaces.front().get());
}

void Skin::SelectSurface(RE::BSGeometry* a_geometry)
{
	editor.geometry = RE::NiPointer(a_geometry);
	BeginEdit();
}

namespace
{
	std::string EditContents(const UserEdit& a_edit)
	{
		return ToJSON(a_edit.parameters).dump() + (a_edit.material ? ToJSON(*a_edit.material).dump() : "null");
	}
}

UserEdit Skin::DraftEdit() const
{
	auto result = editor.edit;
	if (editor.replacing)
		result.material = editor.draft;
	return result;
}

bool Skin::HasUnsavedEdit() const
{
	return editor.geometry && EditContents(DraftEdit()) != editor.baseline;
}

Target Skin::EditTarget(const GeometryEntry& a_entry) const
{
	Target target;
	const auto npc = GetFormKey(RE::TESForm::LookupByID(a_entry.npc));
	if (editor.scope == 0)
		target = a_entry.surface;
	else if (editor.scope == 1 && npc)
		target.npc = npc;
	else if (editor.scope == 2 && npc && a_entry.surface.HasSurface()) {
		target = a_entry.surface;
		target.npc = npc;
	}
	return target;
}

void Skin::BeginEdit()
{
	editor.preview = false;
	editor.valid = true;
	editor.error.clear();
	++editor.revision;
	if (!editor.geometry)
		return;
	auto* property = editor.geometry->GetGeometryRuntimeData().shaderProperty.get();
	auto* material = property ? property->GetBaseMaterial() : nullptr;
	if (!material || material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
		(material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGen && material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint)) {
		editor.geometry.reset();
		return;
	}
	auto& entry = ResolveGeometry(editor.geometry.get(), property);
	editor.edit = {};
	editor.edit.target = EditTarget(entry);
	if (const auto* saved = materials.FindEdit(editor.edit.target))
		editor.edit = *saved;
	editor.draft = editor.edit.material.value_or(entry.resolved.base);
	editor.adjusted = entry.resolved.parameters;
	Apply(editor.adjusted, editor.edit.parameters);
	editor.replacing = false;
	editor.baseline = EditContents(editor.edit);
	editor.preview = true;
	++editor.revision;
}

void Skin::DrawMaterialEditor()
{
	std::lock_guard lock(mutex);
	if (!loaded) {
		ImGui::TextWrapped("%s", T("feature.skin.editor_requires_feature", "Load Advanced Skin to edit and preview skin materials."));
		return;
	}
	if (!settings.EnableSkin && ImGui::Button(T("feature.skin.enable_skin_preview", "Enable Skin Rendering")))
		settings.EnableSkin = true;
	const bool dirty = HasUnsavedEdit();
	if (dirty)
		ImGui::TextWrapped("%s", T("feature.skin.unsaved_edit", "Unsaved changes. Save or cancel before selecting another surface or scope."));
	ImGui::BeginDisabled(dirty);
	if (ImGui::Button(T("feature.skin.pick_crosshair", "Pick Crosshair")))
		SelectReference(PickCrosshairRef());
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.pick_console_ref", "Use Console Selection")))
		SelectReference(RE::Console::GetSelectedRef().get());
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.pick_player", "Use Player")))
		SelectReference(RE::PlayerCharacter::GetSingleton());
	ImGui::EndDisabled();
	if (!editor.geometry) {
		ImGui::TextWrapped("%s", T("feature.skin.pick_surface_help", "Select an actor with loaded skin geometry to edit a surface."));
		return;
	}
	ImGui::BeginDisabled(dirty);
	if (ImGui::BeginCombo(T("feature.skin.selected_surface", "Surface"), editor.geometry->name.c_str())) {
		for (size_t i = 0; i < editor.surfaces.size(); ++i) {
			auto& geometry = editor.surfaces[i];
			ImGui::PushID(static_cast<int>(i));
			if (ImGui::Selectable(geometry->name.c_str(), geometry.get() == editor.geometry.get()))
				SelectSurface(geometry.get());
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	const char* scopes[] = { T("feature.skin.scope_surface", "This material surface"),
		T("feature.skin.scope_npc", "This NPC (all actors with this base)"), T("feature.skin.scope_npc_surface", "This NPC's surface") };
	if (ImGui::Combo(T("feature.skin.edit_scope", "Apply to"), &editor.scope, scopes, 3))
		BeginEdit();
	ImGui::EndDisabled();
	if (!editor.geometry)
		return;
	auto* property = editor.geometry->GetGeometryRuntimeData().shaderProperty.get();
	auto* material = property ? property->GetBaseMaterial() : nullptr;
	if (!material || material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
		(material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGen && material->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGenRGBTint)) {
		ImGui::TextUnformatted(T("feature.skin.surface_changed", "This surface changed. Select it again."));
		if (ImGui::Button(T("feature.skin.cancel_material_preview", "Cancel Unsaved Changes")))
			BeginEdit();
		return;
	}
	auto& entry = ResolveGeometry(editor.geometry.get(), property);
	if (EditTarget(entry).Key() != editor.edit.target.Key()) {
		editor.preview = false;
		++editor.revision;
		ImGui::TextUnformatted(T("feature.skin.surface_changed", "This surface changed. Select it again."));
		if (ImGui::Button(T("feature.skin.cancel_material_preview", "Cancel Unsaved Changes")))
			BeginEdit();
		return;
	}
	ImGui::Text("%s: %s", T("feature.skin.material_source", "Material source"), entry.resolved.source.c_str());
	ImGui::TextWrapped("%s", entry.surface.Key().c_str());
	if (entry.surface.txst)
		ImGui::TextWrapped("%s", T("feature.skin.txst_scope_notice", "This surface identity includes every supported skin surface using this texture-set record."));
	if (!entry.txst)
		ImGui::TextWrapped("%s", T("feature.skin.unconfirmed_txst", "No confirmed applied texture-set record for this surface. NIF materials and actor adjustments remain available."));
	if (!entry.nif.diagnostic.empty())
		ImGui::TextWrapped("%s", entry.nif.diagnostic.c_str());
	for (const auto& adjustment : entry.resolved.adjustments)
		ImGui::BulletText("%s", adjustment.c_str());
	if (!entry.resolved.base.rfaos.empty() && !entry.textures.hasExtraTexture)
		ImGui::TextWrapped("%s", T("feature.skin.rfaos_failed", "RFAOS texture failed to load; this channel is disabled."));
	if (!entry.resolved.base.wetness.empty() && !entry.textures.hasWetnessTexture)
		ImGui::TextWrapped("%s", T("feature.skin.wetness_failed", "Wetness texture failed to load; this channel is disabled."));
	const bool stable = editor.edit.target.HasSurface() || editor.edit.target.npc.has_value();
	if (!stable)
		ImGui::TextWrapped("%s", T("feature.skin.preview_only", "Preview only: publish a NIF surface identity or establish a confirmed record before saving this scope."));
	ImGui::BeginDisabled(editor.scope == 1);
	const bool modeChanged = ImGui::Checkbox(T("feature.skin.replace_material", "Replace complete material"), &editor.replacing);
	if (modeChanged)
		++editor.revision;
	ImGui::EndDisabled();
	const auto before = ToJSON(editor.draft).dump() + ToJSON(editor.edit.parameters).dump();
	if (editor.replacing) {
		ImGui::InputText(T("feature.skin.material_name", "Material name"), &editor.draft.name);
		ImGui::InputText(T("feature.skin.rfaos_path", "RFAOS DDS (empty disables)"), &editor.draft.rfaos);
		ImGui::InputText(T("feature.skin.wetness_path", "Wetness DDS (empty disables)"), &editor.draft.wetness);
		ImGui::TextWrapped("%s", T("feature.skin.base_before_adjustments", "Editing the base material. Race, NPC and saved parameter adjustments still apply after it."));
		DrawParameters(editor.draft.parameters);
	} else {
		ImGui::TextWrapped("%s", T("feature.skin.adjustment_fields", "Check only the parameters you want to change. Unchecked parameters follow the material."));
		auto candidate = DraftEdit();
		editor.adjusted = materials.Resolve(entry.nif, entry.txst, entry.race, entry.npc, entry.surface, settings.DefaultProfile, &candidate).parameters;
		Apply(editor.adjusted, editor.edit.parameters);
		DrawParameters(editor.adjusted, &editor.edit.parameters);
	}
	const bool changed = before != ToJSON(editor.draft).dump() + ToJSON(editor.edit.parameters).dump();
	if (changed || modeChanged) {
		++editor.revision;
		editor.error.clear();
		try {
			if (editor.replacing)
				editor.draft = ReadMaterial(ToJSON(editor.draft));
			ReadParameters(ToJSON(editor.edit.parameters), false);
			editor.valid = true;
		} catch (const std::exception& e) {
			editor.valid = false;
			editor.error = e.what();
		}
	}
	if (ImGui::Checkbox(T("feature.skin.preview_material", "Preview unsaved changes"), &editor.preview))
		++editor.revision;
	ImGui::TextWrapped("%s", T("feature.skin.preview_scope_help", "Preview uses the selected scope's priority, exactly as saving will. More specific edits can override it."));
	ImGui::BeginDisabled(!stable || !editor.valid);
	if (ImGui::Button(T("feature.skin.save_material_edit", "Save User Edit"))) {
		try {
			materials.SaveEdit(DraftEdit());
			BeginEdit();
		} catch (const std::exception& e) {
			editor.error = e.what();
		}
	}
	ImGui::SameLine();
	ImGui::EndDisabled();
	if (ImGui::Button(T("feature.skin.restore_parameter_values", "Restore Parameter Values"))) {
		editor.edit.parameters = {};
		++editor.revision;
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.use_author_material", "Use Author Material"))) {
		editor.edit.material.reset();
		editor.replacing = false;
		editor.valid = true;
		editor.error.clear();
		editor.draft = materials.Resolve(entry.nif, entry.txst, entry.race, entry.npc, entry.surface, settings.DefaultProfile, &editor.edit).base;
		++editor.revision;
	}
	if (ImGui::Button(T("feature.skin.cancel_material_preview", "Cancel Unsaved Changes")))
		BeginEdit();
	ImGui::InputText(T("feature.skin.export_material_name", "Export filename"), &editor.exportName);
	if (ImGui::Button(T("feature.skin.export_material_draft", "Export Material Draft"))) {
		try {
			if (editor.exportName.empty() || editor.exportName.size() > 64 ||
				!std::all_of(editor.exportName.begin(), editor.exportName.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }))
				throw std::runtime_error("Export filename must contain 1-64 letters, digits, underscores or hyphens");
			auto base = ReadMaterial(ToJSON(editor.draft));
			Apply(base.parameters, editor.edit.parameters);
			const auto path = std::filesystem::path("Data/Shaders/Skin/Authoring") / (editor.exportName + ".skinmaterial.json");
			WriteJSON(path, { { "schemaVersion", 1 }, { "material", ToJSON(base) } });
			editor.error = path.string();
		} catch (const std::exception& e) {
			editor.error = e.what();
		}
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(editor.scope == 1);
	if (ImGui::Button(T("feature.skin.import_material_draft", "Import Material Draft"))) {
		try {
			if (editor.exportName.empty() || editor.exportName.size() > 64 ||
				!std::all_of(editor.exportName.begin(), editor.exportName.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }))
				throw std::runtime_error("Invalid draft filename");
			const auto path = std::filesystem::path("Data/Shaders/Skin/Authoring") / (editor.exportName + ".skinmaterial.json");
			if (std::filesystem::file_size(path) > 65536)
				throw std::runtime_error("Material draft exceeds 64 KiB");
			std::ifstream stream(path, std::ios::binary);
			const auto data = Parse(std::string(std::istreambuf_iterator<char>(stream), {}));
			if (!data.is_object() || data.size() != 2 || !data.contains("schemaVersion") || !data["schemaVersion"].is_number_integer() || data["schemaVersion"] != 1 || !data.contains("material"))
				throw std::runtime_error("Expected a schemaVersion 1 material draft");
			editor.draft = ReadMaterial(data.at("material"));
			editor.replacing = true;
			editor.valid = true;
			editor.error.clear();
			++editor.revision;
		} catch (const std::exception& e) {
			editor.error = e.what();
		}
	}
	ImGui::EndDisabled();
	ImGui::TextWrapped("%s", T("feature.skin.draft_export_help", "Drafts use Shaders/Skin/Authoring. Export includes this base and checked parameter changes, without baking race or NPC adjustments."));
	if (!editor.error.empty())
		ImGui::TextWrapped("%s", editor.error.c_str());
}

void Skin::DrawGlobalSettings()
{
	ImGui::SliderFloat(T("feature.skin.extra_skin_wetness", "Extra Skin Wetness"), &settings.ExtraSkinWetness, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.adds_a_constant_layer_of_wetness_to_all", "Adds a constant layer of wetness to all skin, making it look slightly damp or sweaty at all times, even when not in water or exerting effort."));
	}

	ImGui::SliderFloat(T("feature.skin.wetness_fade_out_time", "Wetness Fade Out Time"), &settings.WetFadeTime, 0.0f, 50.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.how_many_seconds_it_takes_for_skin_to", "How many seconds it takes for skin to fully dry after leaving water. Higher values mean wetness lingers longer."));
	}

	ImGui::SliderFloat(T("feature.skin.stamina_threshold_for_sweat", "Stamina Threshold for Sweat"), &settings.StartSweat, 0.0f, 1.0f, "%.2f",
		ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(T("feature.skin.the_character_starts_sweating_when_their_stamina_drops", "The character starts sweating when their stamina drops below this percentage. For example, 0.75 means sweat appears below 75%% stamina."));
	}
	ImGui::SliderFloat(T("feature.skin.full_sweat_threshold", "Full Sweat Threshold"), &settings.FullSweat, 0.0f, 1.0f, "%.2f",
		ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(T("feature.skin.the_character_reaches_maximum_sweat_when_stamina_drops", "The character reaches maximum sweat when stamina drops below this percentage. For example, 0.15 means full sweat below 15%% stamina."));
	}

	ImGui::SliderFloat(T("feature.skin.wetness_perlin_noise_scale", "Wetness Perlin Noise Scale"), &settings.WetParams.x, 0.0f, 1024.0f, "%1.f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_the_size_of_the_wet_dry_pattern", "Controls the size of the wet/dry pattern on skin. Higher values create a finer, more detailed pattern; lower values produce larger, broader wet patches."));
	}
	ImGui::SliderFloat(T("feature.skin.wetness_perlin_noise_lacunarity", "Wetness Perlin Noise Lacunarity"), &settings.WetParams.y, 0.0f, 2.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_how_much_fine_detail_is_added_to", "Controls how much fine detail is added to the wetness pattern. Higher values add more small-scale variation on top of the base pattern."));
	}
	ImGui::SliderFloat(T("feature.skin.wetness_perlin_noise_persistence", "Wetness Perlin Noise Persistence"), &settings.WetParams.z, 0.0f, 20.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_the_overall_contrast_and_roughness_of_the", "Controls the overall contrast and roughness of the wetness pattern. Higher values make the pattern more pronounced and varied."));
	}
	ImGui::SliderFloat(T("feature.skin.wetness_normal_scale", "Wetness Normal Scale"), &settings.WetParams.w, 0.0f, 20.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.skin.controls_how_bumpy_wet_skin_appears_higher_values", "Controls how bumpy wet skin appears. Higher values create more visible surface ripples and distortion on wet areas."));
	}
}

void Skin::LoadSkinDetailTexture()
{
	auto device = globals::d3d::device;

	DirectX::ScratchImage image;
	try {
		std::filesystem::path path{ "Data\\Shaders\\Skin\\skin_detail_n.dds" };
		DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
	} catch (const DX::com_exception& e) {
		logger::error("{}", e.what());
		return;
	}

	ID3D11Resource* pResource = nullptr;
	try {
		DX::ThrowIfFailed(CreateTexture(device,
			image.GetImages(), image.GetImageCount(),
			image.GetMetadata(), &pResource));
	} catch (const DX::com_exception& e) {
		logger::error("{}", e.what());
		return;
	}

	texSkinDetail = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "Skin::DetailNormal");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texSkinDetail->desc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = static_cast<UINT>(image.GetMetadata().mipLevels) }
	};
	texSkinDetail->CreateSRV(srvDesc);
}

Skin::SkinData Skin::MakeProfileData(const SkinProfile& a_profile) const
{
	SkinData data{};
	data.skinParams = float4(a_profile.SkinMainRoughness, a_profile.SkinSecondRoughness, a_profile.SkinSpecularTexMultiplier, float(settings.EnableSkin));
	data.skinParams2 = float4(a_profile.SecondarySpecularStrength, settings.ExtraSkinWetness, a_profile.F0, a_profile.BaseColorMultiplier);
	data.skinDetailParams = float4(a_profile.SkinDetailTiling, a_profile.BodyTilingMultiplier, a_profile.SkinDetailStrength, float(a_profile.EnableSkinDetail && settings.EnableSkin));
	data.sssParams = float4(a_profile.Translucency, a_profile.sssWidth, 0.0f, float(a_profile.UseSSS));
	data.fuzzParams = float4(a_profile.FuzzStrength, a_profile.FuzzRoughness, a_profile.FuzzF0, a_profile.ExtraEdgeRoughness);
	data.physicalParams = float4(a_profile.PhysicalMainRoughnessMultiplier, a_profile.PhysicalSecondRoughnessMultiplier, a_profile.PhysicalSpecularStrength, 0.0f);
	data.wetParams = settings.WetParams;
	return data;
}

Skin::SkinData Skin::GetCommonBufferData()
{
	std::lock_guard lock(mutex);
	return MakeProfileData(settings.DefaultProfile);
}

// By PO3
// https://github.com/powerof3/Splashes-of-Skyrim/blob/master/src/Manager.cpp
float Skin::GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos)
{
	float waterHeight = -RE::NI_INFINITY;

	if (const auto waterManager = RE::TESWaterSystem::GetSingleton()) {
		waterHeight = a_ref->GetWaterHeight();

		if (waterHeight != -RE::NI_INFINITY) {
			return waterHeight;
		}

		const auto get_nearest_water_object_height = [&]() {
			for (const auto& waterObject : waterManager->waterObjects) {
				if (waterObject) {
					for (const auto& bound : waterObject->multiBounds) {
						if (bound) {
							if (auto size{ bound->size }; size.z <= 10.0f) {  //avoid sloped water
								auto center{ bound->center };
								const auto boundMin = center - size;
								const auto boundMax = center + size;
								if (!(a_pos.x < boundMin.x || a_pos.x > boundMax.x || a_pos.y < boundMin.y || a_pos.y > boundMax.y)) {
									return center.z;
								}
							}
						}
					}
				}
			}

			return -RE::NI_INFINITY;
		};

		waterHeight = get_nearest_water_object_height();
	}

	return waterHeight;
}

float4 Skin::GetWetness(RE::BSGeometry* geometry)
{
	float4 wetness = float4(0.0f, 0.0f, 0.0f, 0.0f);
	auto userData = geometry->GetUserData();
	if (userData && userData->formType == RE::FormType::ActorCharacter) {
		auto actor = static_cast<RE::Character*>(userData);
		const uint32_t actorFormID = userData->formID;
		const uint currentFrame = globals::state->frameCount;

		if (actorWetnessMap.size() > 1024) {
			actorWetnessMap.clear();
		}

		auto [it, inserted] = actorWetnessMap.try_emplace(actorFormID);
		auto& cached = it->second;
		if (!inserted && cached.frameCount == currentFrame) {
			return cached.wetness;
		}
		cached.frameCount = currentFrame;

		const float positionZ = actor->GetPositionZ();
		wetness.z = positionZ;
		const float stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		const float permanentStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		const float temporaryStamina = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);
		const float maxStamina = std::max(permanentStamina + temporaryStamina, 1.0f);
		const float staminaPercentage = actor->IsDead() ? 1.0f : (stamina / maxStamina);
		const float sweatRange = settings.StartSweat - settings.FullSweat;
		wetness.x = (std::abs(sweatRange) < 1e-5f)             ? 0.0f :
		            (staminaPercentage >= settings.StartSweat) ? 0.0f :
		            (staminaPercentage <= settings.FullSweat)  ? 1.0f :
		                                                         (settings.StartSweat - staminaPercentage) / sweatRange;
		if (actor->IsInWater()) {
			wetness.y = 2.0f;
			const float waterHeight = GetWaterHeight(userData, actor->GetPosition());
			wetness.w = std::max(0.0f, waterHeight - positionZ);
		} else {
			wetness.y = 0.0f;
			wetness.w = 0.0f;
		}

		if (inserted) {
			cached.wetness = wetness;
		} else {
			const float fadeTime = std::max(settings.WetFadeTime, 0.001f);
			if (cached.wetness.x < wetness.x) {
				cached.wetness.x = wetness.x;
			} else if (cached.wetness.x > wetness.x) {
				cached.wetness.x -= *globals::game::deltaTime / fadeTime;
				cached.wetness.x = std::max(cached.wetness.x, 0.0f);
			}
			wetness.x = cached.wetness.x;

			if (cached.wetness.y < wetness.y) {
				cached.wetness.y = wetness.y;
				if (cached.wetness.w < wetness.w) {
					cached.wetness.w = wetness.w;
				} else {
					wetness.w = cached.wetness.w;
				}
			} else if (cached.wetness.y > wetness.y) {
				cached.wetness.y -= *globals::game::deltaTime / fadeTime;
				cached.wetness.y = std::max(cached.wetness.y, 0.0f);
				wetness.y = cached.wetness.y;
				if (wetness.y == 0.0f) {
					wetness.w = 0.0f;
					cached.wetness.w = 0.0f;
				} else if (cached.wetness.w < wetness.w) {
					cached.wetness.w = wetness.w;
				} else {
					wetness.w = cached.wetness.w;
				}
			} else if (cached.wetness.w < wetness.w) {
				cached.wetness.w = wetness.w;
			} else {
				wetness.w = cached.wetness.w;
			}
			cached.wetness = wetness;
		}
	}
	return wetness;
}
