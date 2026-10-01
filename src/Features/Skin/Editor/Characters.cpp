#include "Features/Skin.h"

#include <shellapi.h>

using namespace SkinMaterials;
using namespace SkinEditor;

namespace
{
	struct UIScope
	{
		std::function<void()> end;
		~UIScope() { end(); }
	};
	std::string SurfaceLabel(RE::BSGeometry* geometry)
	{
		return geometry && geometry->name.c_str() && *geometry->name.c_str() ?
		           geometry->name.c_str() :
		           T("feature.skin.unnamed_surface", "Unnamed surface");
	}
	bool SameSlot(const SkinActors::Guard& a, const SkinActors::Guard& b)
	{
		return a.firstPerson == b.firstPerson && a.source == b.source && a.materialBlock == b.materialBlock;
	}
	void Merge(SkinActors::Binding& binding, SkinActors::Part part, const std::vector<SkinActors::Surface>& incoming)
	{
		auto& items = binding.parts.at(static_cast<size_t>(part));
		const auto previous = items;
		std::erase_if(items, [&](const auto& old) {
			return std::any_of(incoming.begin(), incoming.end(), [&](const auto& surface) {
				if (!SameSlot(old.guard, surface.guard))
					return false;
				const auto count = std::count_if(previous.begin(), previous.end(), [&](const auto& item) { return SameSlot(item.guard, surface.guard); });
				return count == 1 || (old.guard.layout == surface.guard.layout && old.guard.baseTextures == surface.guard.baseTextures &&
										 old.guard.uvTransform == surface.guard.uvTransform && old.guard.headPart == surface.guard.headPart &&
										 old.guard.armor == surface.guard.armor && old.guard.addon == surface.guard.addon);
			});
		});
		for (const auto& surface : incoming) {
			if (const auto* saved = SkinActors::Find(&binding, part, surface.guard)) {
				if (saved->changes != surface.changes)
					throw std::runtime_error(T("feature.skin.shared_character_surface", "These surfaces share a persistent identity. Include them together with identical settings, or give them independent source materials."));
			} else {
				items.push_back(surface);
			}
		}
	}
}

std::vector<RE::NiPointer<RE::BSGeometry>> Skin::DiscoverActor(RE::Actor* actor)
{
	std::vector<RE::NiPointer<RE::BSGeometry>> result;
	if (!actor)
		throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
	const auto current = SkinActors::Get();
	for (bool first : { false, true }) {
		if (first && (actor != RE::PlayerCharacter::GetSingleton() || actor->Get3D(true) == actor->Get3D(false)))
			continue;
		auto* root = actor->Get3D(first);
		if (!root)
			continue;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (IsCompatible(property)) {
				auto& entry = Observe(geometry, property);
				entry.actor = actor->GetHandle();
				entry.inspect = true;
				Prepare(entry, current);
				result.emplace_back(geometry);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}
	if (result.empty())
		throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
	return result;
}

SkinActors::Scheme Skin::CaptureScheme(RE::Actor* actor)
{
	const auto surfaces = DiscoverActor(actor);
	const auto current = SkinActors::Get();
	const auto* binding = SkinActors::Find(*current, SkinActors::ActorKey(actor));
	SkinActors::Scheme scheme;
	if (binding)
		for (size_t i = 0; i < scheme.selected.size(); ++i)
			scheme.selected[i] = !binding->parts[i].empty();
	const bool customized = std::any_of(scheme.selected.begin(), scheme.selected.end(), [](bool value) { return value; });
	for (const auto& geometry : surfaces) {
		const auto& entry = geometries.at(geometry.get());
		if (!entry.target.error.empty() || entry.target.part >= SkinActors::Part::Count ||
			(entry.source.asset && entry.source.asset->definition.status == Status::Invalid))
			continue;
		const auto part = static_cast<size_t>(entry.target.part);
		auto material = entry.base;
		if (const auto* saved = SkinActors::Find(binding, entry.target.part, entry.target.guard))
			material = Apply(material, saved->changes);
		else if (binding && !binding->parts[part].empty())
			continue;
		if (!material.enabled)
			continue;
		CheckTextures(material);
		scheme.parts[part].push_back({ std::move(material), entry.target.guard.layout, entry.target.guard.uvTransform, entry.target.guard.firstPerson });
		if (!customized)
			scheme.selected[part] = true;
	}
	return scheme;
}

const SkinActors::Sample* Skin::TransferSample(const GeometryEntry& entry) const
{
	if (!editor.transfer)
		return nullptr;
	const auto& transfer = *editor.transfer;
	if (transfer.session != SkinActors::Session() || transfer.revision != SkinActors::Revision() || entry.target.key != transfer.key)
		return nullptr;
	for (const auto& row : transfer.rows) {
		const auto part = static_cast<size_t>(row.part);
		if (row.geometry != entry.geometry || !row.include || !transfer.scheme.selected[part] ||
			!row.error.empty() || !entry.target.error.empty() || row.guard != entry.target.guard ||
			row.sample < 0 || static_cast<size_t>(row.sample) >= transfer.scheme.parts[part].size())
			continue;
		return &transfer.scheme.parts[part][row.sample];
	}
	return nullptr;
}

void Skin::BeginTransfer(SkinActors::Scheme scheme, RE::Actor* actor, bool apply)
{
	const auto surfaces = DiscoverActor(actor);
	const auto current = SkinActors::Get();
	Editor::Transfer transfer;
	transfer.scheme = std::move(scheme);
	transfer.actor = actor->GetHandle();
	transfer.key = SkinActors::ActorKey(actor);
	transfer.revision = current->revision;
	transfer.session = current->session;
	std::array<bool, 4> seen{};
	for (const auto& geometry : surfaces) {
		const auto& entry = geometries.at(geometry.get());
		if (entry.target.part >= SkinActors::Part::Count)
			continue;
		const auto part = static_cast<size_t>(entry.target.part);
		if (transfer.scheme.parts[part].empty() && !transfer.scheme.selected[part])
			continue;
		Editor::TransferRow row;
		row.geometry = geometry;
		row.part = entry.target.part;
		row.guard = entry.target.guard;
		row.error = entry.target.error;
		if (entry.source.asset && entry.source.asset->definition.status == Status::Invalid)
			row.error = entry.source.asset->definition.diagnostic;
		seen[part] = true;
		for (size_t i = 0; i < transfer.scheme.parts[part].size(); ++i) {
			const auto& sample = transfer.scheme.parts[part][i];
			if (!SkinActors::Compatible(sample, row.guard))
				continue;
			if (row.sample >= 0 && sample.material != transfer.scheme.parts[part][row.sample].material) {
				row.sample = -1;
				break;
			}
			row.sample = static_cast<int>(i);
		}
		transfer.rows.push_back(std::move(row));
	}
	for (size_t i = 0; i < seen.size(); ++i)
		if (!seen[i] && (transfer.scheme.selected[i] || !transfer.scheme.parts[i].empty())) {
			Editor::TransferRow row;
			row.part = static_cast<SkinActors::Part>(i);
			row.error = T("feature.skin.receiver_part_unavailable", "This receiving part has no verified, loaded surface.");
			transfer.rows.push_back(std::move(row));
		}
	editor.changes = editor.changesBaseline;
	editor.characterUndo.clear();
	editor.characterRedo.clear();
	editor.preview = false;
	editor.transfer = std::move(transfer);
	Invalidate();
	bool ready = std::any_of(editor.transfer->scheme.selected.begin(), editor.transfer->scheme.selected.end(), [](bool value) { return value; });
	for (const auto& row : editor.transfer->rows)
		if (editor.transfer->scheme.selected[static_cast<size_t>(row.part)] && (!row.error.empty() || row.sample < 0))
			ready = false;
	if (apply && ready)
		CommitTransfer();
	else
		editor.message = T("feature.skin.review_character_transfer", "Review the selected parts and surface mappings before saving. Nothing has been applied.");
}

void Skin::CommitTransfer()
{
	if (!editor.transfer)
		return;
	const auto transfer = *editor.transfer;
	const auto actor = transfer.actor.get();
	const auto current = SkinActors::Get();
	if (!actor || current->session != transfer.session || current->revision != transfer.revision || SkinActors::ActorKey(actor.get()) != transfer.key)
		throw std::runtime_error(T("feature.skin.character_changed", "The character configuration or game session changed. Select the current target again."));
	const auto* old = SkinActors::Find(*current, transfer.key);
	SkinActors::Binding next = old ? *old : SkinActors::Binding{};
	next.name = actor->GetName() ? actor->GetName() : "";
	next.persistent = transfer.key == "player" || SkinActors::PersistentKey(transfer.key);
	std::array<size_t, 4> counts{};
	SkinActors::Parts incoming;
	for (const auto& row : transfer.rows) {
		const auto part = static_cast<size_t>(row.part);
		if (!transfer.scheme.selected[part] || !row.include)
			continue;
		if (!row.geometry || !row.error.empty() || row.sample < 0 || static_cast<size_t>(row.sample) >= transfer.scheme.parts[part].size())
			throw std::runtime_error(T("feature.skin.transfer_mapping_required", "Every included surface needs a supported target and an explicit source mapping."));
		const auto& sample = transfer.scheme.parts[part][row.sample];
		if (!SkinActors::Compatible(sample, row.guard) && !(row.confirmed && transfer.preview))
			throw std::runtime_error(T("feature.skin.transfer_preview_required", "Preview and confirm manually mapped textures, or exclude their parts."));
		if (!SkinActors::Belongs(row.geometry.get(), actor.get()))
			throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
		auto* property = row.geometry->GetGeometryRuntimeData().shaderProperty.get();
		if (!IsCompatible(property))
			throw std::runtime_error(T("feature.skin.this_surface_is_no_longer_compatible", "This surface is no longer compatible."));
		auto& entry = Observe(row.geometry.get(), property);
		Prepare(entry, current);
		if (entry.target.key != transfer.key || entry.target.part != row.part || !entry.target.error.empty() || entry.target.guard != row.guard)
			throw std::runtime_error(T("feature.skin.character_changed", "The character configuration or game session changed. Select the current target again."));
		CheckTextures(sample.material);
		const auto changes = SkinActors::Capture(sample.material);
		for (const auto& other : transfer.rows)
			if (other.geometry != row.geometry && other.part == row.part && other.guard == row.guard &&
				(!other.include || other.sample < 0 || transfer.scheme.parts[part][other.sample].material != sample.material))
				throw std::runtime_error(T("feature.skin.shared_character_surface", "These surfaces share a persistent identity. Include them together with identical settings, or give them independent source materials."));
		incoming[part].push_back({ changes, row.guard });
		++counts[part];
	}
	bool any = false;
	for (size_t i = 0; i < counts.size(); ++i)
		if (transfer.scheme.selected[i]) {
			if (!counts[i])
				throw std::runtime_error(T("feature.skin.transfer_empty_part", "A selected part has no receiving surfaces. Exclude it or load and map its surfaces."));
			any = true;
		}
	if (!any)
		throw std::runtime_error(T("feature.skin.select_transfer_parts", "Select at least one part to copy."));
	for (size_t part = 0; part < incoming.size(); ++part)
		Merge(next, static_cast<SkinActors::Part>(part), incoming[part]);
	SkinActors::Commit(transfer.key, std::move(next), transfer.revision, transfer.session);
	editor.transfer.reset();
	editor.preview = false;
	editor.previewWetness = -1;
	if (editor.geometry && SkinActors::Belongs(editor.geometry.get(), actor.get()))
		SelectSurface(editor.geometry.get());
	editor.message = SkinActors::Get()->message;
	Invalidate();
}

void Skin::DrawCharacterActions()
{
	auto& entry = geometries.at(editor.geometry.get());
	const auto current = SkinActors::Get();
	ImGui::Separator();
	ImGui::TextWrapped("%s", entry.target.name.c_str());
	if (!entry.target.error.empty())
		ImGui::TextWrapped("%s", entry.target.error.c_str());
	if (entry.target.part >= SkinActors::Part::Count) {
		DrawCharacterSchemeActions();
		return;
	}
	const auto part = entry.target.part;
	ImGui::TextUnformatted(SkinActors::PartNames()[static_cast<size_t>(part)]);
	if (!entry.target.persistent)
		ImGui::TextWrapped("%s", T("feature.skin.character_session_only", "Applied for this game session only. This actor has no persistent plugin reference."));
	if (ImGui::TreeNode(T("feature.skin.explicit_surface_targets", "Explicit surface targets"))) {
		UIScope tree{ [] { ImGui::TreePop(); } };
		ImGui::TextWrapped("%s", T("feature.skin.character_surface_selection", "Select the surfaces of this character and part that should receive the edit. Discover the player's first-person view separately when needed."));
		for (const auto& surface : editor.surfaces) {
			auto found = geometries.find(surface.get());
			if (found == geometries.end() || found->second.target.key != entry.target.key || found->second.target.part != part)
				continue;
			bool selected = std::find(editor.targets.begin(), editor.targets.end(), surface) != editor.targets.end();
			ImGui::PushID(surface.get());
			UIScope id{ [] { ImGui::PopID(); } };
			const auto label = SurfaceLabel(surface.get()) + (found->second.target.guard.firstPerson ?
																	 T("feature.skin.suffix_first_person", " (first person)") :
																	 T("feature.skin.suffix_third_person", " (third person)"));
			ImGui::BeginDisabled(!found->second.target.error.empty());
			UIScope disabled{ [] { ImGui::EndDisabled(); } };
			if (ImGui::Checkbox(label.c_str(), &selected)) {
				if (selected)
					editor.targets.push_back(surface);
				else
					std::erase(editor.targets, surface);
				Invalidate();
			}
		}
	}
	const auto* binding = SkinActors::Find(*current, entry.target.key);
	const bool rebind = binding && !binding->parts[static_cast<size_t>(part)].empty() && !SkinActors::Find(binding, part, entry.target.guard);
	if (rebind)
		ImGui::TextWrapped("%s", T("feature.skin.character_rebind_notice", "This appearance does not match a saved surface. Review the draft before saving it for this appearance."));
	{
		ImGui::BeginDisabled((entry.target.persistent && !current->available) || !entry.target.error.empty() || editor.targets.empty() || bool(editor.transfer));
		UIScope disabled{ [] { ImGui::EndDisabled(); } };
		const auto* label = entry.target.persistent ? (rebind ?
															  T("feature.skin.save_for_this_appearance", "Save for this appearance") :
															  T("feature.skin.save_apply_character", "Save and apply")) :
		                                              T("feature.skin.apply_character_session", "Apply for this session");
		if (ImGui::Button(label)) {
			SkinActors::Binding next = binding ? *binding : SkinActors::Binding{};
			next.name = entry.target.name;
			next.persistent = entry.target.persistent;
			const auto key = entry.target.key;
			const auto loadedSurfaces = DiscoverActor(entry.target.actor.get().get());
			std::vector<SkinActors::Surface> edits;
			Validate(editor.changes);
			for (const auto& geometry : editor.targets) {
				if (!SkinActors::Belongs(geometry.get(), entry.target.actor.get().get()))
					throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
				auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				if (!IsCompatible(property))
					throw std::runtime_error(T("feature.skin.this_surface_is_no_longer_compatible", "This surface is no longer compatible."));
				auto& target = Observe(geometry.get(), property);
				Prepare(target, current);
				if (target.target.key != key || !target.target.error.empty() || target.target.part != part)
					throw std::runtime_error(T("feature.skin.character_changed", "The character configuration or game session changed. Select the current target again."));
				CheckTextures(Apply(target.base, editor.changes));
				for (const auto& other : loadedSurfaces)
					if (std::find(editor.targets.begin(), editor.targets.end(), other) == editor.targets.end() &&
						geometries.at(other.get()).target.guard == target.target.guard)
						throw std::runtime_error(T("feature.skin.shared_character_surface", "These surfaces share a persistent identity. Include them together with identical settings, or give them independent source materials."));
				edits.push_back({ editor.changes, target.target.guard });
			}
			Merge(next, part, edits);
			SkinActors::Commit(key, std::move(next), current->revision, current->session);
			editor.changesBaseline = editor.changes;
			editor.preview = false;
			editor.previewWetness = -1;
			editor.message = SkinActors::Get()->message;
			Invalidate();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.clear_this_part_s_customization", "Clear this part's customization"))) {
		const auto key = entry.target.key;
		ChangeEditor([this, key, part] {
			const auto state = SkinActors::Get();
			SkinActors::Clear(key, part, state->revision, state->session);
			editor.changes = editor.changesBaseline = {};
			editor.preview = false;
			editor.transfer.reset();
			Invalidate();
		});
	}
	if (current->undo.contains(entry.target.key) && ImGui::Button(T("feature.skin.undo_saved_character_edit", "Undo last saved character edit"))) {
		const auto key = entry.target.key;
		ChangeEditor([this, key] {
			const auto state = SkinActors::Get();
			SkinActors::Undo(key, state->revision, state->session);
			if (editor.geometry)
				SelectSurface(editor.geometry.get());
			Invalidate();
		});
	}
	DrawCharacterSchemeActions();
}

void Skin::DrawCharacterSchemeActions()
{
	const auto& entry = geometries.at(editor.geometry.get());
	ImGui::TextWrapped("%s", T("feature.skin.external_character_contract", "Settings are independent of game saves. The Player slot is shared across playthroughs using this configuration; select a scheme to change it."));
	ImGui::TextWrapped("%s", SkinActors::Get()->message.c_str());
	const auto receiver = entry.target.actor;
	if (ImGui::Button(T("feature.skin.copy_player_skin", "Copy Skin settings from player")))
		ChangeEditor([this, receiver] {
			const auto actor = receiver.get();
			if (!actor)
				throw std::runtime_error(T("feature.skin.select_loaded_character", "Select a character with loaded 3D."));
			BeginTransfer(CaptureScheme(RE::PlayerCharacter::GetSingleton()), actor.get(), true);
		});
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.capture_character_source", "Use as copy source")))
		editor.clipboard = CaptureScheme(receiver.get().get());
	if (editor.clipboard && ImGui::Button(T("feature.skin.paste_character_scheme", "Apply copied scheme...")))
		ChangeEditor([this, receiver, scheme = *editor.clipboard] {
			BeginTransfer(scheme, receiver.get().get(), true);
		});
	if (ImGui::Button(T("feature.skin.save_character_scheme", "Save character scheme..."))) {
		const auto scheme = CaptureScheme(receiver.get().get());
		std::filesystem::create_directories(SkinActors::Directory() / "Presets");
		if (const auto path = ChooseFile(true, SchemeFilter, L"json", SkinActors::Directory() / "Presets" / "Skin.json")) {
			SkinActors::SaveScheme(*path, scheme);
			editor.message = T("feature.skin.character_scheme_saved", "Character scheme saved from committed settings. Its DDS files remain dependencies.");
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.open_character_scheme", "Choose character scheme...")))
		if (const auto path = ChooseFile(false, SchemeFilter, L"json", SkinActors::Directory() / "Presets" / "Skin.json")) {
			const auto scheme = SkinActors::LoadScheme(*path);
			ChangeEditor([this, receiver, scheme] { BeginTransfer(scheme, receiver.get().get(), false); });
		}
}

void Skin::DrawCharacterStorage()
{
	if (!ImGui::CollapsingHeader(T("feature.skin.character_configuration", "Character configuration")))
		return;
	const auto current = SkinActors::Get();
	ImGui::TextWrapped("%s", UTF8(current->path).c_str());
	ImGui::TextWrapped("%s", current->message.c_str());
	if (ImGui::Button(T("feature.skin.open_character_folder", "Open configuration folder"))) {
		std::filesystem::create_directories(SkinActors::Directory());
		ShellExecuteW(nullptr, L"open", SkinActors::Directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
	if (ImGui::Button(T("feature.skin.open_character_configuration", "Open configuration...")))
		if (const auto path = ChooseFile(false, ConfigurationFilter, L"json", current->path))
			ChangeEditor([path = *path] { SkinActors::OpenStorage(path, false); });
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.save_character_configuration_as", "Save configuration as...")))
		if (const auto path = ChooseFile(true, ConfigurationFilter, L"json", current->path))
			ChangeEditor([path = *path] { SkinActors::OpenStorage(path, true); });
	if (ImGui::Button(T("feature.skin.new_character_configuration", "New configuration...")))
		if (const auto path = ChooseFile(true, ConfigurationFilter, L"json", SkinActors::Directory() / "NewSkin.json"))
			ChangeEditor([path = *path] { SkinActors::OpenStorage(path, true, true); });
	if (ImGui::Button(T("feature.skin.restore_character_backup", "Restore latest valid backup")))
		ChangeEditor([] { SkinActors::Recover(); });
	for (const auto& [key, binding] : current->actors) {
		ImGui::PushID(key.c_str());
		UIScope id{ [] { ImGui::PopID(); } };
		if (!ImGui::TreeNode(binding.name.empty() ? key.c_str() : binding.name.c_str()))
			continue;
		UIScope tree{ [] { ImGui::TreePop(); } };
		ImGui::TextWrapped("%s", key.c_str());
		for (size_t i = 0; i < binding.parts.size(); ++i) {
			if (binding.parts[i].empty())
				continue;
			ImGui::PushID(static_cast<int>(i));
			UIScope partID{ [] { ImGui::PopID(); } };
			ImGui::TextUnformatted(SkinActors::PartNames()[i]);
			ImGui::SameLine();
			if (ImGui::SmallButton(T("feature.skin.clear", "Clear")))
				ChangeEditor([this, key, part = static_cast<SkinActors::Part>(i)] {
					const auto state = SkinActors::Get();
					SkinActors::Clear(key, part, state->revision, state->session);
					if (editor.geometry && geometries.at(editor.geometry.get()).target.key == key)
						SelectSurface(editor.geometry.get());
					Invalidate();
				});
		}
	}
}

void Skin::DrawCharacterTransfer()
{
	if (!editor.transfer)
		return;
	ImGui::Separator();
	ImGui::TextUnformatted(T("feature.skin.transfer_review", "Copy Skin settings"));
	auto& transfer = *editor.transfer;
	const auto actor = transfer.actor.get();
	ImGui::TextWrapped("%s", actor ? actor->GetName() : T("feature.skin.target_unloaded", "Target unloaded"));
	ImGui::TextWrapped("%s", T("feature.skin.transfer_scope", "Included surfaces receive independent Skin settings. Native color, normal and thickness textures remain unchanged. Unselected parts keep their settings."));
	for (size_t part = 0; part < transfer.scheme.selected.size(); ++part) {
		ImGui::PushID(static_cast<int>(part));
		UIScope partID{ [] { ImGui::PopID(); } };
		if (transfer.scheme.parts[part].empty() && !transfer.scheme.selected[part])
			continue;
		if (ImGui::Checkbox(SkinActors::PartNames()[part], &transfer.scheme.selected[part]))
			Invalidate();
		if (!transfer.scheme.selected[part])
			continue;
		for (size_t index = 0; index < transfer.rows.size(); ++index) {
			auto& row = transfer.rows[index];
			if (static_cast<size_t>(row.part) != part)
				continue;
			ImGui::PushID(static_cast<int>(index));
			UIScope rowID{ [] { ImGui::PopID(); } };
			if (ImGui::Checkbox(SurfaceLabel(row.geometry.get()).c_str(), &row.include))
				Invalidate();
			if (!row.error.empty()) {
				ImGui::TextWrapped("%s", row.error.c_str());
				continue;
			}
			const auto label = row.sample >= 0 ? std::to_string(row.sample + 1) : std::string(T("feature.skin.choose_source_surface", "Choose source surface"));
			if (ImGui::BeginCombo(T("feature.skin.source_surface", "Source surface"), label.c_str())) {
				UIScope combo{ [] { ImGui::EndCombo(); } };
				for (size_t i = 0; i < transfer.scheme.parts[part].size(); ++i) {
					const auto& sample = transfer.scheme.parts[part][i];
					if (sample.firstPerson != row.guard.firstPerson)
						continue;
					if (ImGui::Selectable(std::to_string(i + 1).c_str(), row.sample == static_cast<int>(i))) {
						row.sample = static_cast<int>(i);
						row.confirmed = false;
						Invalidate();
					}
				}
			}
			if (row.sample >= 0) {
				const auto& sample = transfer.scheme.parts[part][row.sample];
				for (const auto& path : sample.material.textures)
					if (!path.empty())
						ImGui::TextWrapped("%s", path.c_str());
				if (!SkinActors::Compatible(sample, row.guard)) {
					ImGui::TextWrapped("%s", T("feature.skin.manual_uv_mapping", "UV compatibility could not be established. Inspect the texture regions on this receiver before confirming."));
					ImGui::BeginDisabled(!transfer.preview);
					UIScope disabled{ [] { ImGui::EndDisabled(); } };
					ImGui::Checkbox(T("feature.skin.confirm_uv_mapping", "I checked the texture mapping on this surface"), &row.confirmed);
				}
			}
		}
	}
	if (ImGui::Checkbox(T("feature.skin.preview_character_transfer", "Preview mapped surfaces"), &transfer.preview))
		Invalidate();
	if (ImGui::Button(T("feature.skin.save_selected_character_parts", "Save and apply selected parts")))
		CommitTransfer();
	ImGui::SameLine();
	if (ImGui::Button(T("feature.skin.cancel", "Cancel"))) {
		editor.transfer.reset();
		Invalidate();
	}
}
