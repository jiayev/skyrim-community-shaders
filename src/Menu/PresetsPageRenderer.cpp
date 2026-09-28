#include "PresetsPageRenderer.h"
#include "PCH.h"

#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "IconsFontAwesome5.h"
#include "Menu.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "Util.h"
#include "Utils/UI.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <vector>

std::optional<UnifiedPresetCatalog::PresetType> PresetsPageRenderer::typeFilter;
char PresetsPageRenderer::searchBuffer[128] = {};
std::string PresetsPageRenderer::selectedPackId;
bool PresetsPageRenderer::discovered = false;
std::string PresetsPageRenderer::heroPackId;
int PresetsPageRenderer::heroImageIndex = -1;

namespace
{
	const char* BackendLabelE11(bool compact)
	{
		return compact ? "E11" : "Effects 11";
	}

	const char* BackendLabelCS(bool compact)
	{
		return compact ? "CS" : "CS Presets";
	}

	float BadgeGap()
	{
		return ImGui::GetStyle().ItemInnerSpacing.x;
	}

	/** @brief Add a badge gap to width unless this is the first badge. */
	void AppendBadgeGap(float& width, bool& needGap)
	{
		if (needGap)
			width += BadgeGap();
		needGap = true;
	}

	void DrawRoundedImage(ImDrawList* dl, ImTextureID texture, const ImVec2& p0, const ImVec2& p1, float rounding)
	{
		dl->AddImageRounded(texture, p0, p1, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE, rounding);
	}

	/** @brief Circular translucent arrow button centered on center; returns true when clicked. */
	bool DrawCarouselArrow(const char* id, const char* icon, const ImVec2& center, float diameter)
	{
		ImGui::SetCursorScreenPos(ImVec2(center.x - diameter * 0.5f, center.y - diameter * 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, diameter * 0.5f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.45f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));

		ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * 0.85f);
		const float fontSize = ImGui::GetFontSize();
		const ImVec2 labelSize = ImGui::CalcTextSize(icon);
		const bool clicked = ImGui::InvisibleButton(id, ImVec2(diameter, diameter));
		const ImVec2 p0 = ImGui::GetItemRectMin();
		const ImVec2 p1 = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImU32 bg = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive :
				ImGui::IsItemHovered()                                                     ? ImGuiCol_ButtonHovered :
																							   ImGuiCol_Button);
		dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), diameter * 0.5f, bg);
		// Center the font's em-box on the circle, then shift by the unused space below the
		// ascent so the glyph ink (not the extra leading) lands on the circle's midpoint.
		const float unusedBelow = fontSize - ImGui::GetFontBaked()->Ascent;
		const float textX = (p0.x + p1.x - labelSize.x) * 0.5f;
		const float textY = (p0.y + p1.y - fontSize) * 0.5f + unusedBelow * 0.5f;
		dl->AddText(ImVec2(textX, textY), ImGui::GetColorU32(ImGuiCol_Text), icon);
		ImGui::PopFont();

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(3);
		return clicked;
	}
}

bool PresetsPageRenderer::FilterChip(const char* label, bool selected)
{
	const auto& theme = globals::menu->GetTheme();
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
	if (selected) {
		ImGui::PushStyleColor(ImGuiCol_Button, theme.StatusPalette.InfoColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.StatusPalette.InfoColor);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.StatusPalette.InfoColor);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
	}

	const bool clicked = ImGui::Button(label);
	if (selected)
		ImGui::PopStyleColor(4);
	ImGui::PopStyleVar();
	return clicked;
}

float PresetsPageRenderer::MeasureBackendBadgesWidth(bool hasE11, bool hasCSPresets, bool compact)
{
	const ImGuiStyle& style = ImGui::GetStyle();
	float width = 0.0f;
	bool needGap = false;
	if (hasE11) {
		AppendBadgeGap(width, needGap);
		width += ImGui::CalcTextSize(BackendLabelE11(compact)).x + style.FramePadding.x * 2.0f;
	}
	if (hasCSPresets) {
		AppendBadgeGap(width, needGap);
		width += ImGui::CalcTextSize(BackendLabelCS(compact)).x + style.FramePadding.x * 2.0f;
	}
	return width;
}

void PresetsPageRenderer::DrawBackendBadges(bool hasE11, bool hasCSPresets, bool compact)
{
	const auto& info = globals::menu->GetTheme().StatusPalette.InfoColor;
	const auto& warning = globals::menu->GetTheme().StatusPalette.Warning;
	const float gap = BadgeGap();

	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
	ImGui::BeginGroup();
	bool needGap = false;
	if (hasE11) {
		if (needGap)
			ImGui::SameLine(0.0f, gap);
		needGap = true;
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(info.x, info.y, info.z, 0.9f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, info);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, info);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));
		ImGui::SmallButton(BackendLabelE11(compact));
		ImGui::PopStyleColor(4);
	}
	if (hasCSPresets) {
		if (needGap)
			ImGui::SameLine(0.0f, gap);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(warning.x, warning.y, warning.z, 0.85f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, warning);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, warning);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.02f, 1.0f));
		ImGui::SmallButton(BackendLabelCS(compact));
		ImGui::PopStyleColor(4);
	}
	ImGui::EndGroup();
	ImGui::PopStyleVar();
}

void PresetsPageRenderer::RenderToolbar()
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const float scale = Util::GetUIScale();

	ImGui::SetNextItemWidth(220.0f * scale);
	ImGui::InputTextWithHint("##PresetSearch", T("menu.presets.search", "Search presets..."), searchBuffer, IM_ARRAYSIZE(searchBuffer));

	ImGui::SameLine();
	using PresetType = UnifiedPresetCatalog::PresetType;
	const auto typeChip = [](const char* label, std::optional<PresetType> type) {
		if (FilterChip(label, typeFilter == type))
			typeFilter = type;
	};
	typeChip(T("menu.presets.filter_all", "All"), std::nullopt);
	ImGui::SameLine();
	typeChip(T("menu.presets.filter_e11", "E11"), PresetType::E11);
	ImGui::SameLine();
	typeChip(T("menu.presets.filter_cs", "CS"), PresetType::CS);

	// A small divider keeps these maintenance actions from reading as more filter chips.
	Util::ToolbarDivider();

	{
		auto _style = Util::TransparentIconButtonStyle();
		if (ImGui::Button(ICON_FA_SYNC_ALT "##PresetsRefresh")) {
			catalog.Discover();
			discovered = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T("menu.presets.refresh_tooltip", "Rescan unified packs and Effects 11 library presets."));

		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_FOLDER_OPEN "##PresetsOpenFolder"))
			catalog.OpenPresetsFolder();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T("menu.presets.open_folder_tooltip", "Open the unified Presets library folder in Explorer."));
	}
}

void PresetsPageRenderer::RenderList(float width)
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto& theme = globals::menu->GetTheme();
	const float scale = Util::GetUIScale();

	const auto indices = catalog.Query(typeFilter, searchBuffer);

	ImGui::BeginChild("##PresetList", ImVec2(width, 0), true);
	{
		MenuFonts::FontRoleGuard heading(Menu::FontRole::Subheading);
		ImGui::TextUnformatted(T("menu.presets.installed", "Installed"));
	}
	ImGui::Separator();

	if (indices.empty()) {
		ImGui::TextDisabled("%s", T("menu.presets.empty", "No presets found. Drop packs into the Presets folder, install Effects 11 presets, or export from Scene Manager."));
	}

	const float rowPad = 6.0f * scale;
	const float logoSize = 40.0f * scale;
	const float rowGap = ImGui::GetStyle().ItemSpacing.y;
	const float rowHeight = logoSize + rowPad * 2.0f;

	for (size_t idx : indices) {
		const auto& packId = catalog.GetPacks()[idx].id;
		auto* packPtr = catalog.FindPack(packId);
		if (!packPtr)
			continue;
		auto& pack = *packPtr;
		const bool selected = selectedPackId == pack.id;
		const bool isActive = catalog.GetActivePackId() == pack.id;

		ImGui::PushID(pack.id.c_str());

		const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight));
		if (clicked) {
			selectedPackId = pack.id;
			heroPackId.clear();
			heroImageIndex = -1;
		}

		// Draw contents inside the selectable bounds, then park the cursor past the row so the
		// next selectable cannot overlap this highlight.
		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + 8.0f * scale, rowOrigin.y + rowPad));

		catalog.EnsureArtwork(pack);
		{
			ImGui::Dummy(ImVec2(logoSize, logoSize));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 p0 = ImGui::GetItemRectMin();
			const ImVec2 p1 = ImGui::GetItemRectMax();
			const float rounding = ImGui::GetStyle().FrameRounding;
			if (pack.logoSRV)
				DrawRoundedImage(dl, reinterpret_cast<ImTextureID>(pack.logoSRV.get()), p0, p1, rounding);
			else
				dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f, 0.15f, 0.18f, 1.0f)), rounding);
		}

		ImGui::SameLine(0.0f, 8.0f * scale);
		ImGui::BeginGroup();
		{
			MenuFonts::FontRoleGuard body(Menu::FontRole::Body);
			ImGui::TextUnformatted(pack.name.c_str());
			ImGui::SameLine(0.0f, 8.0f * scale);
			DrawBackendBadges(pack.IsE11(), pack.IsCS(), true);
			if (isActive) {
				ImGui::SameLine(0.0f, 6.0f * scale);
				ImGui::TextColored(theme.StatusPalette.SuccessColor, "%s", T("menu.presets.active", "Active"));
			}
			if (!pack.valid) {
				ImGui::SameLine(0.0f, 6.0f * scale);
				ImGui::TextColored(theme.StatusPalette.Error, "%s", T("menu.presets.invalid", "Invalid"));
			}
		}
		{
			MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
			std::string meta;
			if (!pack.author.empty() && !pack.version.empty())
				meta = std::format("{} · v{}", pack.author, pack.version);
			else if (!pack.author.empty())
				meta = pack.author;
			else if (!pack.version.empty())
				meta = std::format("v{}", pack.version);
			else if (pack.source == UnifiedPresetCatalog::SourceKind::Effects11Orphan)
				meta = T("menu.presets.source_e11", "Effects 11 library");
			else if (pack.IsE11())
				meta = T("menu.presets.source_enb", "ENB / Effects 11");
			else if (pack.IsCS())
				meta = T("menu.presets.source_cs", "CS Presets");
			if (!meta.empty())
				ImGui::TextDisabled("%s", meta.c_str());
		}
		ImGui::EndGroup();

		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y + rowHeight + rowGap));
		ImGui::Dummy(ImVec2(0, 0));

		ImGui::PopID();
	}

	ImGui::EndChild();
}

void PresetsPageRenderer::RenderDetail()
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	const auto& theme = globals::menu->GetTheme();
	const ImGuiStyle& style = ImGui::GetStyle();

	ImGui::BeginChild("##PresetDetail", ImVec2(0, 0), true);

	auto* pack = catalog.FindPack(selectedPackId);
	if (!pack) {
		ImGui::TextDisabled("%s", T("menu.presets.select_prompt", "Select a preset from the list to view details."));
		ImGui::EndChild();
		return;
	}

	catalog.EnsureArtwork(*pack);

	if (heroPackId != pack->id) {
		heroPackId = pack->id;
		heroImageIndex = -1;  // cover only until a screenshot is chosen
	}
	if (heroImageIndex >= static_cast<int>(pack->screenshotSRVs.size()))
		heroImageIndex = -1;

	const float badgesWidth = MeasureBackendBadgesWidth(pack->IsE11(), pack->IsCS(), false);
	const float posterH = ImGui::GetFrameHeight() * 6.0f;
	const float posterW = posterH * (2.0f / 3.0f);  // movie-poster portrait

	// Poster | title+author | badges in one table so pills share a baseline and spacing is style-driven.
	if (ImGui::BeginTable("##PresetDetailHeader", 3,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
		ImGui::TableSetupColumn("poster", ImGuiTableColumnFlags_WidthFixed, posterW);
		ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("badges", ImGuiTableColumnFlags_WidthFixed, badgesWidth);

		ImGui::TableNextRow(ImGuiTableRowFlags_None, posterH);
		ImGui::TableSetColumnIndex(0);
		{
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(posterW, posterH));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 p1(origin.x + posterW, origin.y + posterH);
			const float rounding = style.FrameRounding;
			// Deliberately the pack's own cover art only - never a screenshot, so this stays a
			// stable "poster" instead of jumping to whatever the filmstrip/hero viewer is showing.
			if (ID3D11ShaderResourceView* poster = pack->coverSRV.get()) {
				DrawRoundedImage(dl, reinterpret_cast<ImTextureID>(poster), origin, p1, rounding);
			} else {
				dl->AddRectFilled(origin, p1,
					ImGui::ColorConvertFloat4ToU32(ImVec4(0.12f, 0.12f, 0.14f, 1.0f)), rounding);
			}
		}

		// Top-aligned rather than centred in the poster's full height: the actions below now live
		// here too, so this column fills most of the row instead of floating in its middle.
		const float titleTopPad = 2.0f * Util::GetUIScale();

		ImGui::TableSetColumnIndex(1);
		{
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + titleTopPad);
			ImGui::BeginGroup();
			{
				MenuFonts::FontRoleGuard title(Menu::FontRole::Heading);
				ImGui::SetWindowFontScale(1.35f);
				ImGui::TextWrapped("%s", pack->name.c_str());
				ImGui::SetWindowFontScale(1.0f);
			}
			{
				MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
				std::string metaLine;
				if (!pack->author.empty())
					metaLine += pack->author;
				if (!pack->version.empty()) {
					if (!metaLine.empty())
						metaLine += "  ·  ";
					metaLine += "v" + pack->version;
				}
				if (!metaLine.empty())
					Util::Text::Secondary("%s", metaLine.c_str());
				if (!pack->nexusUrl.empty()) {
					ImGui::SameLine(0.0f, style.ItemSpacing.x);
					if (ImGui::SmallButton(ICON_FA_EXTERNAL_LINK_ALT "##NexusLink"))
						ShellExecuteA(NULL, "open", pack->nexusUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
					Util::AddTooltip(T("menu.presets.nexus_link_tooltip", "Open this preset's Nexus Mods page."));
				}
			}

			ImGui::Spacing();

			// Apply fills whatever is left of this column; the folder icon takes one frame
			// square so the pair never overflows when the CS window is narrowed.
			const float folderSize = ImGui::GetFrameHeight();
			const float applyWidth = std::max(0.0f, ImGui::GetContentRegionAvail().x - folderSize - style.ItemSpacing.x);
			const bool canApply = pack->valid && (pack->hasEffects11 || pack->hasCSPresets);
			ImGui::BeginDisabled(!canApply);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
			ImGui::PushStyleColor(ImGuiCol_Button, theme.StatusPalette.InfoColor);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
				std::min(1.0f, theme.StatusPalette.InfoColor.x + 0.1f),
				std::min(1.0f, theme.StatusPalette.InfoColor.y + 0.1f),
				std::min(1.0f, theme.StatusPalette.InfoColor.z + 0.1f),
				1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.StatusPalette.InfoColor);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.07f, 0.1f, 1.0f));
			if (Util::ButtonWithFlash(T("menu.presets.apply", "Apply Preset"), ImVec2(applyWidth, 0))) {
				if (catalog.ApplyPack(pack->id, true)) {
					logger::info("[Presets] Applied pack '{}'", pack->id);
				}
			}
			ImGui::PopStyleColor(4);
			ImGui::PopStyleVar();
			ImGui::EndDisabled();

			ImGui::SameLine(0.0f, style.ItemSpacing.x);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.0f);
			if (ImGui::Button(ICON_FA_FOLDER_OPEN "##OpenPack", ImVec2(folderSize, folderSize)))
				catalog.OpenPackFolder(pack->id);
			ImGui::PopStyleVar();
			Util::AddTooltip(T("menu.presets.open_pack", "Open Pack Folder"));

			if (catalog.GetActivePackId() == pack->id) {
				MenuFonts::FontRoleGuard sub(Menu::FontRole::Subtext);
				ImGui::TextColored(theme.StatusPalette.SuccessColor, "%s", T("menu.presets.active_pack", "This pack is currently active."));
			}

			ImGui::EndGroup();
		}

		ImGui::TableSetColumnIndex(2);
		if (badgesWidth > 0.0f) {
			// Aligned to the title line rather than centred in the whole poster height, now that
			// the title column reads top-down (title, author, actions) instead of floating in the middle.
			const float titleLineH = ImGui::GetTextLineHeight() * 1.35f;
			const float badgeH = ImGui::GetFrameHeight();
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + titleTopPad + std::max(0.0f, (titleLineH - badgeH) * 0.5f));
			DrawBackendBadges(pack->IsE11(), pack->IsCS(), false);
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();

	// Screenshot viewer only after a thumbnail click, not auto-filled with cover.
	const bool showViewer = heroImageIndex >= 0 && heroImageIndex < static_cast<int>(pack->screenshotSRVs.size()) &&
							pack->screenshotSRVs[static_cast<size_t>(heroImageIndex)];
	if (showViewer) {
		const float viewerW = ImGui::GetContentRegionAvail().x * 0.72f;
		const float viewerH = viewerW * (9.0f / 16.0f);
		const float arrowBtn = ImGui::GetFrameHeight() * 1.15f;
		const float viewerOffsetX = (ImGui::GetContentRegionAvail().x - viewerW) * 0.5f;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + viewerOffsetX);

		const ImVec2 heroOrigin = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(viewerW, viewerH));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		DrawRoundedImage(dl,
			reinterpret_cast<ImTextureID>(pack->screenshotSRVs[static_cast<size_t>(heroImageIndex)].get()),
			heroOrigin,
			ImVec2(heroOrigin.x + viewerW, heroOrigin.y + viewerH),
			style.FrameRounding);

		const int shotCount = static_cast<int>(pack->screenshotSRVs.size());
		if (shotCount > 1) {
			const float midY = heroOrigin.y + viewerH * 0.5f;
			const float inset = style.ItemSpacing.x;
			if (DrawCarouselArrow("##HeroPrev", ICON_FA_ANGLE_LEFT,
					ImVec2(heroOrigin.x + inset + arrowBtn * 0.5f, midY), arrowBtn)) {
				heroImageIndex = (heroImageIndex - 1 + shotCount) % shotCount;
			}
			if (DrawCarouselArrow("##HeroNext", ICON_FA_ANGLE_RIGHT,
					ImVec2(heroOrigin.x + viewerW - inset - arrowBtn * 0.5f, midY), arrowBtn)) {
				heroImageIndex = (heroImageIndex + 1) % shotCount;
			}
		}

		ImGui::SetCursorScreenPos(ImVec2(heroOrigin.x - viewerOffsetX, heroOrigin.y + viewerH));
		ImGui::Dummy(ImVec2(0, 0));
		ImGui::Spacing();
	}

	if (!pack->description.empty()) {
		MenuFonts::FontRoleGuard body(Menu::FontRole::Body);
		ImGui::TextWrapped("%s", pack->description.c_str());
	} else {
		ImGui::TextDisabled("%s", T("menu.presets.no_description", "No description provided."));
	}

	if (pack->hasCSPresets) {
		ImGui::Spacing();
		ImGui::TextDisabled("%s", T("menu.presets.cs_layer_note",
			"Applying makes this pack's CS Presets the active scene layer, replacing the previous pack's."));
	}

	if (!pack->valid && !pack->invalidReason.empty()) {
		ImGui::Spacing();
		ImGui::TextColored(theme.StatusPalette.Error, "%s", pack->invalidReason.c_str());
	}

	if (!pack->screenshotSRVs.empty()) {
		ImGui::Spacing();
		ImGui::SeparatorText(T("menu.presets.screenshots", "Screenshots"));
		const float thumb = ImGui::GetFrameHeight() * 4.0f;
		const float thumbH = thumb * (9.0f / 16.0f);
		for (size_t i = 0; i < pack->screenshotSRVs.size(); ++i) {
			if (i > 0)
				ImGui::SameLine(0.0f, style.ItemSpacing.x);
			ImGui::PushID(static_cast<int>(i));
			const bool isFeatured = static_cast<int>(i) == heroImageIndex;
			const bool clicked = ImGui::InvisibleButton("##shot", ImVec2(thumb, thumbH));
			const ImVec2 p0 = ImGui::GetItemRectMin();
			const ImVec2 p1 = ImGui::GetItemRectMax();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			if (pack->screenshotSRVs[i])
				DrawRoundedImage(dl, reinterpret_cast<ImTextureID>(pack->screenshotSRVs[i].get()), p0, p1, style.FrameRounding);
			if (isFeatured || ImGui::IsItemHovered()) {
				const ImU32 border = ImGui::ColorConvertFloat4ToU32(isFeatured ?
						theme.StatusPalette.InfoColor :
						ImGui::GetStyleColorVec4(ImGuiCol_Border));
				dl->AddRect(p0, p1, border, style.FrameRounding, 0, style.FrameBorderSize + (isFeatured ? 1.0f : 0.0f));
			}
			if (clicked)
				heroImageIndex = static_cast<int>(i);
			ImGui::PopID();
		}
	}

	ImGui::EndChild();
}

void PresetsPageRenderer::Render()
{
	auto& catalog = UnifiedPresetCatalog::GetSingleton();
	if (!discovered) {
		catalog.Discover();
		discovered = true;
		if (selectedPackId.empty() && !catalog.GetActivePackId().empty())
			selectedPackId = catalog.GetActivePackId();
		else if (selectedPackId.empty() && !catalog.GetPacks().empty())
			selectedPackId = catalog.GetPacks().front().id;
	}

	ImGui::BeginChild("PresetsPage", ImVec2(0, 0), false);

	RenderToolbar();
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	const float scale = Util::GetUIScale();
	const float listWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, 260.0f * scale, 380.0f * scale);
	RenderList(listWidth);
	ImGui::SameLine();
	RenderDetail();

	ImGui::EndChild();
}
