#include "SceneSettingsUI.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "../../I18n/I18n.h"
#include "../EditorWindow.h"
#include "../LocationTypeIcons.h"
#include "../Weather/WeatherWidget.h"
#include "Features/CSEditor.h"
#include "IconsFontAwesome5.h"
#include "Menu.h"
#include "SceneFeatureReplica.h"
#include "ScenePageToolbar.h"
#include "SceneSettingsLocationTargets.h"
#include "SceneSettingsManager.h"
#include "Utils/Form.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include "../WeatherUtils.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using TimeOfDayPeriod = SceneSettingsManager::TimeOfDayPeriod;
	constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;

	/// Game-hour delta that counts as a deliberate scrub. Deltas below it accumulate rather than
	/// reset the baseline, so time running at any timescale still re-couples the bar.
	constexpr float kScrubEpsilon = 1e-3f;

	/// Starting width of the weather tab's feature column at the baseline font size; the user can drag it.
	constexpr float kFeatureListWidth = 180.0f;

	/// The divider doubles as the resize grip, so the feature column and the panel share one border.
	constexpr ImGuiTableFlags kFeatureLayoutFlags =
		ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;

	/// The objects window nests its list inside the category list, so it reads as a sub-level.
	constexpr float kNestedFeatureFontScale = 0.85f;

	/// Name and editor ID share the slack; the type and the trailing action are fixed and narrow.
	constexpr float kLocationTypeColumnWidth = 90.0f;
	constexpr float kLocationAddColumnWidth = 32.0f;
	constexpr float kLocationRemoveColumnWidth = 40.0f;
	/// Glyphs differ in advance, so each sits centred in a box this many font sizes wide to keep names aligned.
	constexpr float kLocationIconBoxScale = 1.4f;
	/// Floor for the alternating row tint, so long lists stay scannable under themes that leave it clear.
	constexpr float kMinRowShadeAlpha = 0.04f;
	/// Rows the picker shows before it scrolls.
	constexpr float kLocationPickerVisibleRows = 10.0f;
	/// Matches the weather list's JSON delete icon, which sits a little inside the row height.
	constexpr float kRemoveIconScale = 0.85f;
	/// Outlined like the weather editor's lists, which frame their rows the same way.
	constexpr ImGuiTableFlags kLocationTableFlags =
		ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
	/// Only the user's list sorts: the chain's order is its hierarchy, outermost first.
	constexpr ImGuiTableFlags kAuthoredLocationTableFlags = kLocationTableFlags | ImGuiTableFlags_Sortable;

	/// Identifies the clicked header in the sort specs, which go by user ID rather than position.
	enum LocationColumnId
	{
		LocationColumnName,
		LocationColumnEditorId,
		LocationColumnType,
		LocationColumnAction
	};

	/// Location windows share their size with each other, like the form widgets do per type.
	constexpr const char* kLocationWidgetType = "SceneLocation";

	/// One bar shared by every panel: it tracks live time, which is global.
	struct PeriodBarState
	{
		int selected = -1;       // -1 follows the live period
		float lastHour = -1.0f;  // game hour the coupling was last judged against
	};
	PeriodBarState periodBar;

	/// The Scene Manager panel edits interior and time of day, and only one of them resolves at a
	/// time, so it follows the player instead of being matched to the cell by hand.
	bool interiorEnabled = false;
	bool lastInterior = false;

	/// Re-arms the interior layer on a cell transition.
	void SyncSceneToggles()
	{
		const bool interior = Util::IsInterior();
		if (interior != lastInterior) {
			lastInterior = interior;
			interiorEnabled = interior;
		}
	}

	/// A selectable feature, with its label built once because the list is fixed after boot.
	struct FeatureListEntry
	{
		std::string shortName;
		std::string label;
	};

	/// Each panel keeps its own selection: several can be on screen at once.
	std::string weatherSelectedFeature;
	std::string panelSelectedFeature;

	/// A location the user opened for editing. Locations are not forms in the widget system, so the
	/// editor tracks its own windows instead of going through Widget.
	struct LocationWindow
	{
		SceneSettingsManager::LocationTarget target;
		std::string selectedFeature;
		bool open = true;
		bool pendingFocus = false;
	};
	std::vector<LocationWindow> locationWindows;

	struct LocationPickerState
	{
		char search[256]{};
		/// Catalog indices passing the search, rebuilt only when the query changes.
		std::vector<size_t> matches;
		bool matchesValid = false;
	};
	LocationPickerState locationPicker;

	// Latch driving the automatic time pause: raised by any page editing a period this frame.
	bool periodEditingThisFrame = false;
	bool wasEditingTimeOfDay = false;
	bool pausedByPanel = false;

	const char* GetPeriodLabel(int period)
	{
		switch (static_cast<TimeOfDayPeriod>(period)) {
		case TimeOfDayPeriod::Dawn:
			return T(TKEY("tod_dawn"), "Dawn");
		case TimeOfDayPeriod::Sunrise:
			return T(TKEY("tod_sunrise"), "Sunrise");
		case TimeOfDayPeriod::Day:
			return T(TKEY("tod_day"), "Day");
		case TimeOfDayPeriod::Sunset:
			return T(TKEY("tod_sunset"), "Sunset");
		case TimeOfDayPeriod::Dusk:
			return T(TKEY("tod_dusk"), "Dusk");
		default:
			return T(TKEY("tod_night"), "Night");
		}
	}

	/// The context one page edits: the selected period's set while editing periods, else the flat set.
	SceneSettingsManager::SceneContextId ResolvePageContext(
		const SceneSettingsManager::SceneContextId& baseContext, TimeOfDayPeriod period, bool periodEditing)
	{
		auto context = baseContext;
		context.period = periodEditing && SceneSettingsManager::IsPeriodicContext(context.type) ?
		                     period :
		                     TimeOfDayPeriod::Count;
		assert(context.type != SceneSettingsManager::SceneContextType::TimeOfDay || context.period != TimeOfDayPeriod::Count);
		return context;
	}

	/// Whether a page edits one period at a time. The Scene Manager panel has no flat mode, so it
	/// always does, except indoors where the aperiodic interior layer takes the panel over. Weather
	/// and location pages follow the saved set their scene has active.
	bool ResolvePeriodEditing(const SceneSettingsManager::SceneContextId& baseContext, bool sceneManagerPanel)
	{
		if (sceneManagerPanel)
			return !interiorEnabled;
		return SceneSettingsManager::GetSingleton()->IsSceneTimeOfDayEnabled(baseContext);
	}

	/// Resolves which period is currently active, updating the follow/pin state as a side effect.
	/// Call exactly once per panel per frame - the title row's toolbar and the period bar itself
	/// both need this value, and running the scrub/pin logic twice would double-apply it.
	int ResolveActivePeriod(bool editing)
	{
		const int live = static_cast<int>(SceneSettingsManager::GetCurrentPeriod());

		if (editing) {
			const float hour = SceneSettingsManager::GetCurrentGameHour();
			if (std::abs(hour - periodBar.lastHour) > kScrubEpsilon) {
				periodBar.selected = -1;
				periodBar.lastHour = hour;
			}
		} else if (periodBar.selected < 0) {
			// Pin the follow state so a disabled bar stops reacting to time entirely.
			periodBar.selected = live;
		}

		return periodBar.selected < 0 ? live : periodBar.selected;
	}

	/// Draws the period segmented control and its companion checkbox (the interior indicator, or
	/// the Time of Day toggle), and returns the period the panel below it should edit. `active` is
	/// the value ResolveActivePeriod already computed for this panel this frame; a click here may
	/// advance it further. The page toolbar used to share this row, but now sits on the title row
	/// above so navigation (this bar) and actions stay visually distinct.
	int DrawPeriodBarRow(const SceneSettingsManager::SceneContextId& baseContext, bool editing,
		bool sceneManagerPanel, int active)
	{
		const bool interior = sceneManagerPanel && interiorEnabled;
		const int live = static_cast<int>(SceneSettingsManager::GetCurrentPeriod());

		std::array<const char*, kPeriodCount> labels{};
		for (int period = 0; period < kPeriodCount; ++period)
			labels[period] = GetPeriodLabel(period);

		ImGui::BeginDisabled(!editing);
		// Dot the live period only when the selection has left it, so the bar always shows where time actually is.
		if (Util::SegmentedControl("PeriodBar", labels.data(), kPeriodCount, active, editing ? live : -1)) {
			periodBar.selected = active;
			// Jump time into the middle of the period so the scene shows what is being edited.
			// The baseline moves with it, or the scrub check would drop straight back to following.
			periodBar.lastHour = SceneSettingsManager::GetPeriodMidHour(static_cast<TimeOfDayPeriod>(active));
			SceneSettingsManager::SetGameHour(periodBar.lastHour);
		}
		ImGui::EndDisabled();

		// The state line explains the bar rather than changing what it does, so it lives on hover.
		char periodStatus[256];
		if (interior)
			std::snprintf(periodStatus, sizeof(periodStatus), "%s", T(TKEY("period_bar_interior"), "Time of day editing is unavailable indoors."));
		else if (!editing)
			std::snprintf(periodStatus, sizeof(periodStatus), "%s", T(TKEY("period_bar_off"), "Time of day is off, so one set of settings applies all day. The bar does not follow game time."));
		else if (periodBar.selected < 0)
			std::snprintf(periodStatus, sizeof(periodStatus), "%s", T(TKEY("period_bar_following"), "Following the time of day. Click a period to jump to it and edit it on its own."));
		else
			std::snprintf(periodStatus, sizeof(periodStatus), T(TKEY("period_bar_manual"), "Editing %s. Scrub the time of day to follow it again."), labels[active]);
		Util::AddTooltip(periodStatus, Util::kTooltipWhenDisabled);

		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 3.0f);
		if (sceneManagerPanel) {
			// Indicator only: interior always follows the cell the player is actually in, so
			// toggling it by hand would silently do nothing when the layer can't resolve.
			ImGui::BeginDisabled();
			ImGui::Checkbox(T(TKEY("interior_toggle"), "Interior"), &interiorEnabled);
			ImGui::EndDisabled();
			Util::AddTooltip(T(TKEY("interior_toggle_tooltip"),
				"Shows whether interior settings are being edited. Follows the cell the player is in."),
				Util::kTooltipWhenDisabled);
		} else {
			// Enabling re-couples the bar to live time, so it always lands on the current period.
			bool timeOfDayEnabled = editing;
			if (ImGui::Checkbox(T(TKEY("time_of_day_toggle"), "Time of Day"), &timeOfDayEnabled)) {
				SceneSettingsManager::GetSingleton()->SetSceneTimeOfDayEnabled(baseContext, timeOfDayEnabled);
				if (timeOfDayEnabled) {
					periodBar.selected = -1;
					periodBar.lastHour = SceneSettingsManager::GetCurrentGameHour();
				}
			}
			Util::AddTooltip(T(TKEY("time_of_day_toggle_tooltip"),
				"Use a separate set of settings for each period instead of one set for the whole day. "
				"Both sets are kept, so switching back restores the other. Game time pauses while a period is being edited."),
				ImGuiHoveredFlags_DelayNormal);
		}

		return active;
	}

	/// Scene-capable features, resolved once: loaded features and the catalog are fixed after boot.
	/// Transitionable-only is the weather and time-of-day set; the rest also covers interior and location.
	const std::vector<FeatureListEntry>& GetFeatureEntries(bool transitionableOnly)
	{
		auto build = [](const std::vector<std::string>& names) {
			std::vector<FeatureListEntry> entries;
			entries.reserve(names.size());
			for (const auto& name : names)
				entries.push_back({ name, std::format("{}##{}", SceneSettingsManager::GetFeatureDisplayName(name), name) });
			return entries;
		};
		static const std::vector<FeatureListEntry> transitionable = build(SceneSettingsManager::GetExteriorRelevantFeatureNames());
		static const std::vector<FeatureListEntry> all = build(SceneSettingsManager::GetLocationRelevantFeatureNames());
		return transitionableOnly ? transitionable : all;
	}

	/// Draws the feature selectables and returns the feature the panel below should edit.
	const std::string& DrawFeatureList(std::string& selected, const std::vector<FeatureListEntry>& entries)
	{
		if (entries.empty()) {
			selected.clear();
			Util::Text::WrappedSecondary("%s",
				T(TKEY("scene_feature_list_empty"), "No loaded feature exposes scene settings."));
			return selected;
		}

		if (std::ranges::none_of(entries, [&](const auto& entry) { return entry.shortName == selected; }))
			selected = entries.front().shortName;

		for (const auto& entry : entries) {
			if (ImGui::Selectable(entry.label.c_str(), entry.shortName == selected))
				selected = entry.shortName;
		}
		return selected;
	}

	/// Period bar, intro, and the replicated feature UI bound to one scene context.
	void DrawPanel(const char* intro, const std::string& selectedFeature,
		const SceneSettingsManager::SceneContextId& baseContext, bool withPeriodBar = true,
		bool sceneManagerPanel = false)
	{
		auto context = baseContext;
		bool periodEditing = false;
		if (withPeriodBar) {
			periodEditing = ResolvePeriodEditing(baseContext, sceneManagerPanel);
			periodEditingThisFrame |= periodEditing;
			int active = ResolveActivePeriod(periodEditing);

			// Title row: the panel's identity (Scene Manager panel only) shares a row with its
			// actions, like a window header with its buttons beside the title, rather than the
			// actions crowding the period bar's navigation row below.
			if (sceneManagerPanel) {
				EditorWindow::GetSingleton()->DrawActiveWeatherIndicator(false);
				ImGui::SameLine();
			}
			ScenePageToolbar::Draw(ResolvePageContext(baseContext, static_cast<TimeOfDayPeriod>(active), periodEditing));
			ImGui::Separator();

			active = DrawPeriodBarRow(baseContext, periodEditing, sceneManagerPanel, active);
			context = ResolvePageContext(baseContext, static_cast<TimeOfDayPeriod>(active), periodEditing);
			ImGui::Separator();
		} else {
			// A page without the bar has no toggle row to share, so the actions get a row of their own.
			ScenePageToolbar::Draw(context);
		}
		Util::Text::WrappedSecondary("%s", intro);

		if (selectedFeature.empty())
			return;
		SceneFeatureReplica::Draw(selectedFeature, context);
	}

	/// Feature column beside the panel body, split by a divider the user can drag.
	/// Transitionable features are the weather set; the rest also covers interior and location.
	void DrawFeatureLayout(std::string& selectedFeature, bool transitionableOnly, const char* intro,
		bool withPeriodBar, const SceneSettingsManager::SceneContextId& baseContext)
	{
		if (!ImGui::BeginTable("SceneFeatureLayout", 2, kFeatureLayoutFlags))
			return;

		ImGui::TableSetupColumn("##Features", ImGuiTableColumnFlags_WidthFixed, kFeatureListWidth * Util::GetUIScale());
		ImGui::TableSetupColumn("##Body", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();

		// Each column scrolls on its own, so a long feature list never drags the panel with it.
		ImGui::TableSetColumnIndex(0);
		if (ImGui::BeginChild("##SceneFeatureList")) {
			Util::Text::Secondary("%s", T(TKEY("scene_feature_list_title"), "Features"));
			DrawFeatureList(selectedFeature, GetFeatureEntries(transitionableOnly));
		}
		ImGui::EndChild();

		ImGui::TableSetColumnIndex(1);
		if (ImGui::BeginChild("##SceneFeatureBody"))
			DrawPanel(intro, selectedFeature, baseContext, withPeriodBar);
		ImGui::EndChild();

		ImGui::EndTable();
	}

	const char* GetLocationTypeLabel(SceneSettingsManager::LocationTargetType type)
	{
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Worldspace:
			return T(TKEY("location_type_worldspace"), "Worldspace");
		case SceneSettingsManager::LocationTargetType::LocationType:
			return T(TKEY("location_type_location_type"), "Location Type");
		case SceneSettingsManager::LocationTargetType::Region:
			return T(TKEY("location_type_region"), "Region");
		case SceneSettingsManager::LocationTargetType::Cell:
			return T(TKEY("location_type_cell"), "Cell");
		default:
			return T(TKEY("location_type_location"), "Location");
		}
	}

	const char* GetLocationCategoryIcon(SceneSettingsManager::LocationTargetType type)
	{
		switch (type) {
		case SceneSettingsManager::LocationTargetType::Worldspace:
			return ICON_FA_GLOBE_EUROPE;
		case SceneSettingsManager::LocationTargetType::LocationType:
			return ICON_FA_TAG;
		case SceneSettingsManager::LocationTargetType::Region:
			return ICON_FA_MAP;
		case SceneSettingsManager::LocationTargetType::Cell:
			return ICON_FA_BORDER_ALL;
		default:
			return ICON_FA_MAP_MARKER_ALT;
		}
	}

	/** @brief LocType FA glyph when the target is (or resolves to) a location-type keyword. */
	const char* ResolveLocTypeGlyph(const SceneSettingsManager::LocationTarget& target)
	{
		using LocationTargetType = SceneSettingsManager::LocationTargetType;

		if (target.type == LocationTargetType::LocationType && !target.editorId.empty())
			return LocationTypeIcons::LookupLocationTypeIcon(target.editorId);

		// Concrete locations: pick the most specific LocType keyword they carry.
		if (target.type != LocationTargetType::Location)
			return nullptr;

		RE::BGSLocation* location = nullptr;
		if (target.formId != 0) {
			location = RE::TESForm::LookupByID<RE::BGSLocation>(target.formId);
		} else if (!target.formKey.empty()) {
			// Quiet resolve — SpidToFormId logs on miss, and authored lists redraw every frame.
			const auto components = Util::ParseSpid(target.formKey);
			if (auto* handler = RE::TESDataHandler::GetSingleton();
				handler && components.localFormId != 0 && !components.pluginName.empty())
				location = handler->LookupForm<RE::BGSLocation>(components.localFormId, components.pluginName);
		}
		if (!location)
			return nullptr;

		std::vector<std::string> locTypeIds;
		for (auto* keyword : location->GetKeywords()) {
			if (!SceneSettingsLocationTargets::IsLocationTypeKeyword(keyword))
				continue;
			auto editorId = Util::GetFormEditorID(keyword);
			if (!editorId.empty())
				locTypeIds.push_back(std::move(editorId));
		}
		return LocationTypeIcons::ResolvePrimaryLocationTypeIcon(locTypeIds);
	}

	const char* GetLocationTargetIcon(const SceneSettingsManager::LocationTarget& target)
	{
		if (const char* locTypeIcon = ResolveLocTypeGlyph(target))
			return locTypeIcon;
		return GetLocationCategoryIcon(target.type);
	}

	/// The type glyph leading a row's name, muted so the name stays the thing read first.
	void DrawLocationTypeIcon(const SceneSettingsManager::LocationTarget& target)
	{
		const char* icon = GetLocationTargetIcon(target);
		const float box = ImGui::GetFontSize() * kLocationIconBoxScale;
		const float offset = std::max(0.0f, (box - ImGui::CalcTextSize(icon).x) * 0.5f);
		const float start = ImGui::GetCursorPosX();
		ImGui::SetCursorPosX(start + offset);
		Util::Text::Secondary("%s", icon);
		ImGui::SameLine(start + box);
	}

	void PushLocationRowShade()
	{
		ImVec4 shade = ImGui::GetStyleColorVec4(ImGuiCol_TableRowBgAlt);
		if (shade.w < kMinRowShadeAlpha) {
			shade = ImGui::GetStyleColorVec4(ImGuiCol_Text);
			shade.w = kMinRowShadeAlpha;
		}
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, shade);
	}

	/// Opens a location's editor window, focusing the existing one rather than opening a second.
	LocationWindow& OpenLocationWindow(const SceneSettingsManager::LocationTarget& target)
	{
		auto existing = std::ranges::find_if(locationWindows, [&](const auto& window) {
			return window.target.type == target.type && window.target.formKey == target.formKey;
		});
		if (existing != locationWindows.end()) {
			existing->open = true;
			existing->pendingFocus = true;
			return *existing;
		}
		return locationWindows.emplace_back(LocationWindow{ .target = target });
	}

	/// Both location tables identify a target the same way; only the trailing action differs.
	void SetupLocationColumns(float actionWidth)
	{
		const float scale = Util::GetUIScale();
		ImGui::TableSetupColumn(T(TKEY("location_column_name"), "Name"), ImGuiTableColumnFlags_WidthStretch, 0.0f, LocationColumnName);
		ImGui::TableSetupColumn(T(TKEY("location_column_editor_id"), "Editor ID"), ImGuiTableColumnFlags_WidthStretch, 0.0f, LocationColumnEditorId);
		ImGui::TableSetupColumn(T(TKEY("location_column_type"), "Type"), ImGuiTableColumnFlags_WidthFixed, kLocationTypeColumnWidth * scale, LocationColumnType);
		ImGui::TableSetupColumn("##Action", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, actionWidth * scale, LocationColumnAction);
		ImGui::TableHeadersRow();
	}

	/// The form key is the fallback identity for targets whose editor ID the game does not expose.
	const std::string& GetLocationIdentityText(const SceneSettingsManager::LocationTarget& target)
	{
		return target.editorId.empty() ? target.formKey : target.editorId;
	}

	/// Editor ID and type: what tells apart two links of a chain that share a display name.
	void DrawLocationDetailColumns(const SceneSettingsManager::LocationTarget& target)
	{
		ImGui::TableNextColumn();
		Util::Text::Secondary("%s", GetLocationIdentityText(target).c_str());
		if (target.editorId.empty())
			Util::AddTooltip(T(TKEY("location_form_key_tooltip"), "The game exposes no editor ID for this place, so its form key is shown."));

		ImGui::TableNextColumn();
		Util::Text::Secondary("%s", GetLocationTypeLabel(target.type));
	}

	/// Trailing icon action for an addable row: quiet until hovered, a check once the place is listed.
	bool DrawLocationAddButton(bool authored)
	{
		const char* label = authored ? ICON_FA_CHECK "##add" : ICON_FA_PLUS "##add";
		const float padding = ImGui::GetStyle().FramePadding.x * 0.5f;
		const float width = ImGui::CalcTextSize(label, nullptr, true).x + padding * 2.0f;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { padding, 0.0f });
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		auto _style = Util::TransparentIconButtonStyle();
		ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetSecondary());
		ImGui::BeginDisabled(authored);
		const bool clicked = ImGui::Button(label);
		ImGui::EndDisabled();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
		return clicked;
	}

	/// Orders the user's list by the clicked header; ImGui owns the direction cycling and the arrow.
	void SortLocationTargets(std::vector<SceneSettingsManager::LocationTarget>& targets)
	{
		const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
		if (!specs || specs->SpecsCount <= 0)
			return;

		const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
		std::ranges::sort(targets, [&spec](const auto& lhs, const auto& rhs) {
			int comparison = 0;
			switch (spec.ColumnUserID) {
			case LocationColumnName:
				comparison = _stricmp(lhs.name.c_str(), rhs.name.c_str());
				break;
			case LocationColumnEditorId:
				comparison = _stricmp(GetLocationIdentityText(lhs).c_str(), GetLocationIdentityText(rhs).c_str());
				break;
			case LocationColumnType:
				comparison = _stricmp(GetLocationTypeLabel(lhs.type), GetLocationTypeLabel(rhs.type));
				break;
			default:
				break;
			}
			// The form key breaks ties: the sort is unstable, so equal keys would otherwise swap rows per frame.
			if (comparison == 0)
				comparison = _stricmp(lhs.formKey.c_str(), rhs.formKey.c_str());
			return spec.SortDirection == ImGuiSortDirection_Ascending ? comparison < 0 : comparison > 0;
		});
	}

	/// One row of a table the user adds targets from, disabled once the target is on their list.
	void DrawLocationAddRow(SceneSettingsManager& manager, const SceneSettingsManager::LocationTarget& target)
	{
		ImGui::TableNextRow();
		ImGui::PushID(target.formKey.c_str());

		ImGui::TableNextColumn();
		DrawLocationTypeIcon(target);
		ImGui::TextUnformatted(target.name.c_str());
		DrawLocationDetailColumns(target);

		ImGui::TableNextColumn();
		const bool authored = manager.IsLocationTargetAuthored(target.type, target.formKey);
		if (DrawLocationAddButton(authored))
			manager.AddLocationTarget(target);
		Util::AddTooltip(authored ? T(TKEY("location_already_added"), "Already on your list.") :
									T(TKEY("location_add_tooltip"), "Add to your locations"),
			Util::kTooltipWhenDisabled);

		ImGui::PopID();
	}

	/// The chain the player is standing in, outermost first, each link addable on its own.
	void DrawLocationChain()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		if (!manager)
			return;
		const auto& targets = manager->GetCurrentLocationTargets();
		if (targets.empty()) {
			Util::Text::WrappedSecondary("%s",
				T(TKEY("location_chain_unavailable"), "Waiting for the player's location."));
			return;
		}

		PushLocationRowShade();
		if (ImGui::BeginTable("LocationChain", 4, kLocationTableFlags)) {
			SetupLocationColumns(kLocationAddColumnWidth);
			for (const auto& target : targets)
				DrawLocationAddRow(*manager, target);
			ImGui::EndTable();
		}
		ImGui::PopStyleColor();
	}

	bool LocationTargetMatchesSearch(const SceneSettingsManager::LocationTarget& target, const std::string& query)
	{
		if (Util::StringMatchesSearch(target.name, query) ||
			Util::StringMatchesSearch(GetLocationIdentityText(target), query) ||
			Util::StringMatchesSearch(GetLocationTypeLabel(target.type), query))
			return true;
		if (target.type == SceneSettingsManager::LocationTargetType::LocationType) {
			if (const auto label = LocationTypeIcons::LookupLocationTypeLabel(target.editorId); !label.empty())
				return Util::StringMatchesSearch(std::string(label), query);
		}
		return false;
	}

	/// Every place the game defines, searchable, so targets away from the player can be added too.
	void DrawLocationPicker()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		if (!manager)
			return;
		const auto& catalog = manager->GetLocationCatalog();

		ImGui::SetNextItemWidth(-FLT_MIN);
		const bool searchChanged = ImGui::InputTextWithHint("##LocationSearch",
			T(TKEY("location_search"), "Search by name, editor ID, or type..."), locationPicker.search,
			IM_ARRAYSIZE(locationPicker.search));
		// The catalog runs to thousands of forms, so it is filtered only when the query changes.
		if (searchChanged || !locationPicker.matchesValid) {
			const std::string query = locationPicker.search;
			locationPicker.matches.clear();
			for (size_t index = 0; index < catalog.size(); ++index)
				if (LocationTargetMatchesSearch(catalog[index], query))
					locationPicker.matches.push_back(index);
			locationPicker.matchesValid = true;
		}

		if (locationPicker.matches.empty()) {
			Util::Text::WrappedSecondary("%s", T(TKEY("location_search_empty"), "No places match the search."));
			return;
		}

		const ImVec2 tableSize{ 0.0f, ImGui::GetFrameHeightWithSpacing() * kLocationPickerVisibleRows };
		PushLocationRowShade();
		if (ImGui::BeginTable("LocationCatalog", 4, kLocationTableFlags | ImGuiTableFlags_ScrollY, tableSize)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			SetupLocationColumns(kLocationAddColumnWidth);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(locationPicker.matches.size()));
			while (clipper.Step())
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
					DrawLocationAddRow(*manager, catalog[locationPicker.matches[row]]);
			ImGui::EndTable();
		}
		ImGui::PopStyleColor();
	}

	/// The editor's delete icon, matching the weather list's JSON removal action.
	bool DrawLocationRemoveButton()
	{
		// The row selectable spans every column, so the button has to claim the clicks over it.
		ImGui::SetNextItemAllowOverlap();

		const float iconSize = ImGui::GetFrameHeight() * kRemoveIconScale;
		auto* menu = globals::menu;
		if (!menu || !menu->uiIcons.deleteSettings.texture)
			return Util::ErrorTextButton(T(TKEY("remove"), "Remove"));
		return Util::ErrorImageButton("##remove", menu->uiIcons.deleteSettings.texture, { iconSize, iconSize });
	}

	/// The user's list: double-click a row to edit it, or drop it and its settings.
	void DrawAuthoredLocations()
	{
		auto* manager = SceneSettingsManager::GetSingleton();
		if (!manager)
			return;
		auto targets = manager->GetAuthoredLocationTargets();
		if (targets.empty()) {
			Util::Text::WrappedSecondary("%s", T(TKEY("location_list_empty"),
				"No locations yet. Add one from where you are standing, or search for any place."));
			return;
		}

		PushLocationRowShade();
		if (!ImGui::BeginTable("AuthoredLocations", 4, kAuthoredLocationTableFlags)) {
			ImGui::PopStyleColor();
			return;
		}

		SetupLocationColumns(kLocationRemoveColumnWidth);
		// The list is rebuilt from the manager each frame, so it is re-sorted each frame too.
		SortLocationTargets(targets);

		// Removal mutates the manager's map, so it waits until the rows are submitted.
		const SceneSettingsManager::LocationTarget* pendingRemoval = nullptr;
		for (const auto& target : targets) {
			ImGui::TableNextRow();
			ImGui::PushID(target.formKey.c_str());

			ImGui::TableNextColumn();
			const bool opened = std::ranges::any_of(locationWindows, [&](const auto& window) {
				return window.open && window.target.type == target.type && window.target.formKey == target.formKey;
			});
			DrawLocationTypeIcon(target);
			if (Util::TableRowSelectable(target.name.c_str(), opened,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap) &&
				ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				OpenLocationWindow(target);
			DrawLocationDetailColumns(target);

			ImGui::TableNextColumn();
			if (DrawLocationRemoveButton())
				pendingRemoval = &target;
			Util::AddTooltip(T(TKEY("location_remove_tooltip"),
				"Drops the location from the list along with the settings authored for it."));

			ImGui::PopID();
		}
		ImGui::EndTable();
		ImGui::PopStyleColor();

		if (pendingRemoval) {
			std::erase_if(locationWindows, [&](const auto& window) {
				return window.target.type == pendingRemoval->type && window.target.formKey == pendingRemoval->formKey;
			});
			manager->RemoveLocationTarget(pendingRemoval->type, pendingRemoval->formKey);
		}
	}
}

void SceneSettingsUI::DrawWeatherSceneTab(RE::FormID weatherId)
{
	if (weatherId == 0) {
		Util::Text::WrappedDisabled("%s",
			T(TKEY("scene_weather_unresolved"), "This weather has no form, so it cannot hold overrides."));
		return;
	}

	const SceneSettingsManager::SceneContextId context{
		.type = SceneSettingsManager::SceneContextType::Weather,
		.weatherId = weatherId,
	};
	DrawFeatureLayout(weatherSelectedFeature, true,
		T(TKEY("scene_manager_weather_intro"), "Settings overridden while this weather is active."), true, context);
}

void SceneSettingsUI::DrawSceneManagerPanel()
{
	// DrawPanel below draws the active-weather indicator itself, on the same row as the page
	// toolbar, since this is the only caller that passes sceneManagerPanel = true.

	// The interior layer takes the panel over indoors, and it has no periods.
	SyncSceneToggles();
	const bool interior = interiorEnabled;
	const SceneSettingsManager::SceneContextId context{
		.type = interior ? SceneSettingsManager::SceneContextType::Interior :
						   SceneSettingsManager::SceneContextType::TimeOfDay,
		.period = interior ? TimeOfDayPeriod::Count : TimeOfDayPeriod::Dawn,
	};

	DrawPanel(T(TKEY("scene_manager_panel_intro"), "Settings overridden by interior and time of day."),
		panelSelectedFeature, context, true, true);
}

void SceneSettingsUI::DrawSceneManagerCategoryFeatures()
{
	ImGui::Separator();

	ImGui::Indent();
	ImGui::SetWindowFontScale(kNestedFeatureFontScale);
	DrawFeatureList(panelSelectedFeature, GetFeatureEntries(false));
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Unindent();
}

void SceneSettingsUI::DrawLocationBrowser()
{
	EditorWindow::GetSingleton()->DrawActiveWeatherIndicator();

	Util::Explainer(T(TKEY("location_browser_intro_label"), "How locations resolve"),
		T(TKEY("location_browser_intro"),
			"Locations resolve last, so they win over interior, time of day, and weather. Narrower places win over broader ones: "
			"a cell over its location, a location over its region, location types, and worldspace."));

	ImGui::SeparatorText(T(TKEY("location_add_from_here"), "Add from where you are"));
	DrawLocationChain();

	ImGui::SeparatorText(T(TKEY("location_add_any"), "Add any place"));
	DrawLocationPicker();

	ImGui::SeparatorText(T(TKEY("location_list_title"), "Your locations"));
	Util::AddTooltip(T(TKEY("location_list_tooltip"), "Double-click a location to edit its settings."));
	DrawAuthoredLocations();
}

void SceneSettingsUI::DrawLocationWindows()
{
	for (auto& window : locationWindows) {
		if (!window.open)
			continue;

		if (window.pendingFocus) {
			ImGui::SetNextWindowFocus();
			window.pendingFocus = false;
		}

		SetupWidgetWindowDefaults(kLocationWidgetType);
		// FA LocType / category glyph left of the title (same pattern as weather popouts).
		const char* typeIcon = GetLocationTargetIcon(window.target);
		const auto title = std::format("{}  {} ({})###SceneLocation_{}", typeIcon, window.target.name,
			GetLocationTypeLabel(window.target.type), window.target.formKey);
		const bool visible = Util::BeginWithCustomHeader(title.c_str(), &window.open, nullptr, ImGuiWindowFlags_NoSavedSettings | kStickyHeaderFlags);
		UpdateWidgetTypeSize(kLocationWidgetType);
		if (visible) {
			const SceneSettingsManager::SceneContextId context{
				.type = SceneSettingsManager::SceneContextType::Location,
				.period = TimeOfDayPeriod::Count,
				.locationType = window.target.type,
				.locationFormKey = window.target.formKey,
			};
			// Both sets take location features; a per-period set greys whatever cannot blend.
			DrawFeatureLayout(window.selectedFeature, false,
				T(TKEY("scene_manager_location_intro"), "Settings overridden while the player is in this location."),
				true, context);
		}
		ImGui::End();
	}

	std::erase_if(locationWindows, [](const auto& window) { return !window.open; });
}

void SceneSettingsUI::OpenSceneContext(const SceneSettingsManager::SceneContextId& context,
	const std::string& featureShortName)
{
	CSEditor::OpenEditorWindow();
	auto* editorWindow = EditorWindow::GetSingleton();
	if (!editorWindow->open)
		return;

	// Pinned without moving the clock, so a mid-blend jump lands on the incoming period as it is now.
	if (context.period != TimeOfDayPeriod::Count)
		periodBar = { static_cast<int>(context.period), SceneSettingsManager::GetCurrentGameHour() };

	switch (context.type) {
	case SceneSettingsManager::SceneContextType::Location:
		{
			const auto& targets = SceneSettingsManager::GetSingleton()->GetCurrentLocationTargets();
			const auto target = std::ranges::find_if(targets, [&](const auto& candidate) {
				return candidate.type == context.locationType && candidate.formKey == context.locationFormKey;
			});
			if (target != targets.end())
				OpenLocationWindow(*target).selectedFeature = featureShortName;
			break;
		}
	case SceneSettingsManager::SceneContextType::Weather:
		for (const auto& widget : editorWindow->weatherWidgets)
			if (widget->form && widget->form->GetFormID() == context.weatherId) {
				static_cast<WeatherWidget*>(widget.get())->OpenSceneManagerTab();
				weatherSelectedFeature = featureShortName;
				break;
			}
		break;
	default:
		editorWindow->SelectCategory("Scene Manager");
		panelSelectedFeature = featureShortName;
		break;
	}
}

bool SceneSettingsUI::OpenCurrentLocationForSetting(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	CSEditor::OpenEditorWindow();
	auto* editorWindow = EditorWindow::GetSingleton();
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!editorWindow || !editorWindow->open || !manager)
		return false;

	const auto& targets = manager->GetCurrentLocationTargets();
	if (targets.empty()) {
		editorWindow->ShowNotification(
			T(TKEY("scene_override_location_unknown"),
				"Can't tell where you are yet. Step into the world, then try again."),
			Util::Colors::GetWarning(), 4.0f);
		return false;
	}

	// Flat Location is the layer that accepts non-blendable settings Weather/TOD grey out.
	if (!SceneSettingsManager::IsSettingAllowedForType(SceneSettingsManager::SceneType::Location,
			featureShortName, settingPath, settingKey, false)) {
		editorWindow->ShowNotification(
			T(TKEY("scene_override_location_unsupported"),
				"This place can't hold that setting — Location scenes don't support it either."),
			Util::Colors::GetWarning(), 4.0f);
		return false;
	}

	// Chain is outermost-first; the last link is the place you're actually standing in.
	const auto& target = targets.back();
	const bool alreadyAuthored = manager->IsLocationTargetAuthored(target.type, target.formKey);
	if (!alreadyAuthored && !manager->AddLocationTarget(target)) {
		editorWindow->ShowNotification(
			T(TKEY("scene_override_location_add_failed"),
				"Couldn't add an override for where you are. Check the log."),
			Util::Colors::GetWarning(), 4.0f);
		return false;
	}

	const SceneSettingsManager::SceneContextId locationContext{
		.type = SceneSettingsManager::SceneContextType::Location,
		.period = TimeOfDayPeriod::Count,
		.locationType = target.type,
		.locationFormKey = target.formKey,
	};
	// Period sets require blendable values; turn Time of Day off so this setting is editable here.
	bool turnedOffTod = false;
	if (manager->IsSceneTimeOfDayEnabled(locationContext)) {
		manager->SetSceneTimeOfDayEnabled(locationContext, false);
		turnedOffTod = true;
	}

	auto& window = OpenLocationWindow(target);
	window.selectedFeature = featureShortName;
	window.pendingFocus = true;

	if (!alreadyAuthored) {
		editorWindow->ShowNotification(
			I18n::GetSingleton()->Format("cs_editor.scene_override_location_added",
				{ { "place", target.name } },
				"Added {place} — edit this setting here."),
			Util::Colors::GetInfo(), 3.0f);
	} else if (turnedOffTod) {
		editorWindow->ShowNotification(
			I18n::GetSingleton()->Format("cs_editor.scene_override_location_flat",
				{ { "place", target.name } },
				"Opened {place}. Time of Day is off here so this setting can be edited."),
			Util::Colors::GetInfo(), 3.5f);
	}

	return true;
}

void SceneSettingsUI::SyncTimePause()
{
	// Time only stops for a panel that is actually editing a period.
	const bool editing = periodEditingThisFrame;
	periodEditingThisFrame = false;
	if (editing == wasEditingTimeOfDay)
		return;
	wasEditingTimeOfDay = editing;

	auto* editorWindow = EditorWindow::GetSingleton();
	if (editing) {
		// A pause the user set themselves is theirs to release, so only track our own.
		pausedByPanel = !editorWindow->IsTimePaused();
		if (pausedByPanel)
			editorWindow->PauseTime();
	} else if (pausedByPanel) {
		editorWindow->ResumeTime();
		pausedByPanel = false;
	}
}
