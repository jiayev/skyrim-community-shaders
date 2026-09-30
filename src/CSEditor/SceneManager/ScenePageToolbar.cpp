#include "ScenePageToolbar.h"

#include <format>
#include <optional>
#include <string>

#include "../../I18n/I18n.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/Icons/helpers/SceneActionIcons.h"
#include "SceneCopyModal.h"
#include "ScenePresetExport.h"
#include "SceneTransitionField.h"
#include "Utils/UI.h"

#include <imgui_internal.h>

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using SceneContextId = SceneSettingsManager::SceneContextId;
	using SceneContextType = SceneSettingsManager::SceneContextType;

	/// Keeps the toolbar off the window's scrollbar, like the widget gutter does.
	constexpr float kRightMargin = 8.0f;

	/// Divider that keeps the transition label from reading as part of the control before the toolbar.
	constexpr float kDividerThickness = 1.0f;

	/// Clear is page-local, so each page keys its own confirmation and only that page draws it.
	struct PageConfirmation
	{
		void Request(const SceneContextId& a_page)
		{
			context = a_page;
			requested = true;
			popup.Request();
		}

		/** @brief Runs a_confirmed on the requesting page once accepted, and drops the request as
		 *  soon as the popup is gone, however it went. */
		template <typename Action>
		void Draw(const SceneContextId& a_page, Action&& a_confirmed)
		{
			if (!requested || context != a_page)
				return;
			if (popup.Draw())
				a_confirmed();
			else if (popup.IsOpen())
				return;
			requested = false;
		}

		Util::ConfirmationPopup popup;
		SceneContextId context;
		bool requested = false;
	};
	PageConfirmation clearConfirmation;

	/// Width one text button occupies, so the toolbar can right-align before drawing anything.
	float ButtonWidth(const char* label)
	{
		return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	}

	/// Compact FA / PNG icon button width (glyph + frame pad).
	float IconButtonWidth()
	{
		return ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.0f;
	}
}

void ScenePageToolbar::Draw(const SceneContextId& context)
{
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return;

	const auto summary = manager->GetContextUserEntrySummary(context);
	const bool hasEntries = summary.total != 0;
	// A mixed page pauses rather than resumes: the button is a way out of that state, not into it.
	const bool pauseTarget = !summary.AllPaused();

	const char* toggleLabel = pauseTarget ? T(TKEY("scene_page_pause_all"), "Pause All") :
	                                        T(TKEY("scene_page_resume_all"), "Resume All");
	const char* copyLabel = T(TKEY("scene_page_copy"), "Copy");
	const char* exportLabel = T(TKEY("scene_page_export"), "Export");
	const char* clearLabel = T(TKEY("scene_page_clear"), "Clear");
	const char* transitionLabel = T(TKEY("scene_page_transition"), "Transition");

	const auto& style = ImGui::GetStyle();
	const float clearWidth = IconButtonWidth();
	// The global duration only governs the location layer, so it is absent everywhere else.
	const bool hasTransitionField = context.type == SceneContextType::Location;
	const float transitionWidth = hasTransitionField ?
	                                  kDividerThickness + style.ItemSpacing.x +
	                                      ImGui::CalcTextSize(transitionLabel).x + style.ItemInnerSpacing.x +
	                                      SceneTransitionField::GetWidth() + style.ItemSpacing.x :
	                                  0.0f;
	// Pause, the page's copy/export actions, and clear are three groups.
	const float width = ButtonWidth(toggleLabel) + ButtonWidth(copyLabel) + ButtonWidth(exportLabel) +
	                    clearWidth + transitionWidth + style.ItemSpacing.x * 2.0f +
	                    Util::GetToolbarDividerWidth() * 2.0f;
	const float margin = kRightMargin * Util::GetUIScale();
	// Sharing a row it cannot fit on would push the actions past the panel edge.
	if (ImGui::GetCurrentWindowRead()->DC.IsSameLine && ImGui::GetContentRegionAvail().x < width + margin)
		ImGui::NewLine();
	if (const auto avail = ImGui::GetContentRegionAvail().x; avail > width + margin)
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width - margin);

	ImGui::PushID("ScenePageToolbar");

	if (hasTransitionField) {
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical, kDividerThickness);
		ImGui::SameLine();

		// Bare text is top-aligned, which would float it above the framed row it labels.
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(transitionLabel);
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

		// The global has no layer above it, so it is always set: emptying restores the default
		// rather than clearing to nullopt, which the resolver could not use.
		std::optional<float> seconds = manager->GetLocationTransitionSeconds();
		if (SceneTransitionField::Draw("##ScenePageTransition", seconds,
				SceneSettingsManager::kDefaultLocationTransitionSeconds, true)) {
			manager->SetLocationTransitionSeconds(
				seconds.value_or(SceneSettingsManager::kDefaultLocationTransitionSeconds));
		}
		Util::AddTooltip(T(TKEY("scene_page_transition_tooltip"),
			"Seconds every value on this page takes to ease in and out when the location changes.\n"
			"A single setting can override this from its own transition field."));

		ImGui::SameLine();
	}

	ImGui::BeginDisabled(!hasEntries);
	if (ImGui::Button(toggleLabel))
		manager->SetContextEntriesPaused(context, pauseTarget);
	ImGui::EndDisabled();
	Util::AddTooltip(pauseTarget ?
						 T(TKEY("scene_page_pause_all_tooltip"),
							 "Holds back every override on this page without losing its value.") :
						 T(TKEY("scene_page_resume_all_tooltip"), "Applies every override on this page again."),
		Util::kTooltipWhenDisabled);

	Util::ToolbarDivider();
	// A page holding entries always has somewhere to offer them, so destinations are never walked just to grey the button.
	ImGui::BeginDisabled(!hasEntries && !SceneCopyModal::HasSources(context));
	if (ImGui::Button(copyLabel))
		SceneCopyModal::Open(context);
	ImGui::EndDisabled();
	Util::AddTooltip(T(TKEY("scene_page_copy_tooltip"), "Copies settings between this page and another context."),
		Util::kTooltipWhenDisabled);

	ImGui::SameLine();
	ImGui::BeginDisabled(!ScenePresetExport::CanExport());
	if (ImGui::Button(exportLabel))
		ScenePresetExport::Open();
	ImGui::EndDisabled();
	Util::AddTooltip(T(TKEY("scene_page_export_tooltip"),
						  "Export scene settings as a preset, or update an existing pack's metadata and artwork."),
		Util::kTooltipWhenDisabled);

	Util::ToolbarDivider();
	ImGui::BeginDisabled(!hasEntries);
	{
		Icons::FontGuard font(SceneActionIcons::kDelete);
		if (Util::ErrorTextButton(std::format("{}##ScenePageClear", SceneActionIcons::kDelete.utf8).c_str())) {
			auto count = summary.total;
			auto pageName = manager->GetSceneContextDisplayName(context);
			clearConfirmation.popup.title = T(TKEY("scene_page_clear_title"), "Clear page");
			clearConfirmation.popup.message = std::vformat(T(TKEY("scene_page_clear_message"),
															  "Remove all {} settings from {}? Mod overrides are left alone.\n\n"
															  "Settings you removed come back too."),
				std::make_format_args(count, pageName));
			clearConfirmation.popup.confirmLabel = clearLabel;
			clearConfirmation.popup.cancelLabel = T(TKEY("cancel"), "Cancel");
			clearConfirmation.Request(context);
		}
	}
	ImGui::EndDisabled();
	Util::AddTooltip(T(TKEY("scene_page_clear_tooltip"), "Removes every override this page holds."),
		Util::kTooltipWhenDisabled);

	clearConfirmation.Draw(context, [&] { manager->ClearContextEntries(context); });
	SceneCopyModal::Draw(context);

	ImGui::PopID();
}

#undef I18N_KEY_PREFIX
