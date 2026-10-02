#include "SceneLayerHeader.h"

#include <array>
#include <format>
#include <optional>
#include <span>
#include <vector>

#include "../../I18n/I18n.h"
#include "../Browser/BrowserWidgets.h"
#include "../EditorWindow.h"
#include "IconsFontAwesome5.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "SceneSettingsUI.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using SceneContextId = SceneSettingsManager::SceneContextId;
	using SceneContextType = SceneSettingsManager::SceneContextType;
	using WinningContext = SceneSettingsManager::WinningContext;

	/// Seconds the override summary is reused for; it walks every applied value of the feature.
	constexpr double kWinnerRefreshSeconds = 0.25;

	enum class Layer
	{
		Base,
		TimeOfDay,
		Interior,
		Weather,
		Location,
	};

	Layer LayerOf(SceneContextType type)
	{
		switch (type) {
		case SceneContextType::Interior:
			return Layer::Interior;
		case SceneContextType::Weather:
			return Layer::Weather;
		case SceneContextType::Location:
			return Layer::Location;
		default:
			return Layer::TimeOfDay;
		}
	}

	const char* LayerName(Layer layer)
	{
		switch (layer) {
		case Layer::Base:
			return T(TKEY("layer_base"), "Base");
		case Layer::TimeOfDay:
			return T(TKEY("layer_time_of_day"), "Time of day");
		case Layer::Interior:
			return T(TKEY("layer_interior"), "Interior");
		case Layer::Weather:
			return T(TKEY("layer_weather"), "Weather");
		default:
			return T(TKEY("layer_location"), "Location");
		}
	}

	const char* LayerTooltip(Layer layer)
	{
		switch (layer) {
		case Layer::Base:
			return T(TKEY("layer_base_tooltip"),
				"The feature's own settings, the same as its page in the Community Shaders menu. Edited in Base Settings.");
		case Layer::TimeOfDay:
			return T(TKEY("layer_time_of_day_tooltip"), "Overrides for each period of the day outdoors. Edited on the Scene Manager page.");
		case Layer::Interior:
			return T(TKEY("layer_interior_tooltip"), "Overrides for every interior. Edited on the Scene Manager page while indoors.");
		case Layer::Weather:
			return T(TKEY("layer_weather_tooltip"), "Overrides while one weather is active. Edited on that weather's Scene tab.");
		default:
			return T(TKEY("layer_location_tooltip"),
				"Overrides for a place on your Locations list. Edited in that place's window; the narrowest place wins.");
		}
	}

	/// The layers a value passes through, broadest first. Indoors there is no weather, and the
	/// interior layer stands in for time of day.
	std::span<const Layer> LayerStack(bool interior)
	{
		static constexpr std::array kOutdoor{ Layer::Base, Layer::TimeOfDay, Layer::Weather, Layer::Location };
		static constexpr std::array kIndoor{ Layer::Base, Layer::Interior, Layer::Location };
		return interior ? std::span<const Layer>(kIndoor) : std::span<const Layer>(kOutdoor);
	}

	SceneContextType ContextTypeOf(Layer layer)
	{
		switch (layer) {
		case Layer::Interior:
			return SceneContextType::Interior;
		case Layer::Weather:
			return SceneContextType::Weather;
		case Layer::Location:
			return SceneContextType::Location;
		default:
			return SceneContextType::TimeOfDay;
		}
	}

	/// Where clicking a layer leads.
	enum class Destination
	{
		None,        ///< The layer cannot hold this feature, or nothing of it applies here.
		Base,        ///< The Base Settings window.
		Page,        ///< The page of the layer's context that applies now.
		PlacesList,  ///< The Locations page: no listed place applies here yet.
	};

	struct LayerLink
	{
		Destination destination = Destination::None;
		std::optional<SceneContextId> context;
	};

	LayerLink ResolveLink(Layer layer, const std::string& featureShortName)
	{
		if (featureShortName.empty())
			return {};
		if (layer == Layer::Base)
			return { Destination::Base, std::nullopt };
		if (!SceneSettingsUI::LayerListsFeature(ContextTypeOf(layer), featureShortName))
			return {};
		if (auto context = SceneSettingsUI::ResolveCurrentLayer(ContextTypeOf(layer), featureShortName))
			return { Destination::Page, context };
		// A place can only be edited once it is on the user's list, so lead to where it is added.
		return layer == Layer::Location ? LayerLink{ Destination::PlacesList, std::nullopt } : LayerLink{};
	}

	void Follow(const LayerLink& link, const std::string& featureShortName)
	{
		switch (link.destination) {
		case Destination::Base:
			if (auto* editor = EditorWindow::GetSingleton())
				editor->OpenBaseSettings(featureShortName);
			break;
		case Destination::Page:
			SceneSettingsUI::OpenSceneContext(*link.context, featureShortName);
			break;
		case Destination::PlacesList:
			SceneSettingsUI::OpenLocationsPage();
			break;
		default:
			break;
		}
	}

	/// Tooltip of a layer in the stack: what it is, and where clicking it goes.
	void DrawLayerTooltip(Layer layer, Destination destination)
	{
		const char* hint = nullptr;
		switch (destination) {
		case Destination::Base:
			hint = T(TKEY("layer_open_base"), "Click to open this feature in Base Settings.");
			break;
		case Destination::Page:
			hint = T(TKEY("layer_open_page"), "Click to open the page for what applies right now.");
			break;
		case Destination::PlacesList:
			hint = T(TKEY("layer_open_places"), "None of your listed places applies here. Click to open Locations and add one.");
			break;
		default:
			hint = T(TKEY("layer_open_unavailable"), "This feature has nothing to set on this layer here right now.");
			break;
		}
		const std::string text = std::format("{}\n\n{}", LayerTooltip(layer), hint);
		Util::AddTooltip(text.c_str(), Util::kTooltipWhenDisabled);
	}

	/** @brief Breadcrumb of the stack: the current layer as an accent chip, the rest links to their pages. */
	void DrawStack(Layer current, bool interior, const std::string& featureShortName)
	{
		const auto& style = ImGui::GetStyle();
		const ImVec4 accent = Util::Colors::GetAccent();
		const Icons::GlyphRef separator = Icons::FA(ICON_FA_ANGLE_RIGHT);

		ImGui::PushID("SceneLayerStack");
		bool first = true;
		for (const Layer layer : LayerStack(interior)) {
			if (!first) {
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				ImGui::AlignTextToFramePadding();
				Icons::FontGuard font(separator);
				Util::Text::Secondary("%s", separator.utf8);
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			}
			first = false;

			ImGui::PushID(static_cast<int>(layer));
			if (layer == current) {
				BrowserUI::Chip(LayerName(layer), LayerName(layer), {}, nullptr, &accent);
				Util::AddTooltip(LayerTooltip(layer));
			} else {
				const LayerLink link = ResolveLink(layer, featureShortName);
				ImGui::AlignTextToFramePadding();
				if (link.destination == Destination::None)
					Util::Text::Disabled("%s", LayerName(layer));
				else if (BrowserUI::Link("##layer", LayerName(layer)))
					Follow(link, featureShortName);
				DrawLayerTooltip(layer, link.destination);
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	/** @brief The feature's overriding contexts, refreshed a few times a second rather than every frame. */
	const std::vector<WinningContext>& GetCachedWinners(SceneSettingsManager& manager, const std::string& featureShortName)
	{
		static std::string cachedFeature;
		static double cachedAt = -1.0;
		static std::vector<WinningContext> winners;

		const double now = ImGui::GetTime();
		if (cachedFeature != featureShortName || now - cachedAt > kWinnerRefreshSeconds) {
			cachedFeature = featureShortName;
			cachedAt = now;
			winners = manager.GetWinningContexts(featureShortName);
		}
		return winners;
	}

	/** @brief Lists the layers overriding the feature here, each a link to the page that edits it. */
	void DrawOverrides(SceneSettingsManager& manager, const std::string& featureShortName)
	{
		const auto& winners = GetCachedWinners(manager, featureShortName);
		if (winners.empty())
			return;

		ImGui::Spacing();
		Util::DrawInlineIndicatorDot(ImGui::GetColorU32(Util::Colors::GetInfo()), true);
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::TextUnformatted(T(TKEY("layer_overridden_by"), "Overridden here by:"));

		ImGui::Indent();
		for (const auto& winner : winners) {
			const auto label = I18n::GetSingleton()->Format(TKEY("layer_override_link"),
				{ { "layer", LayerName(LayerOf(winner.context.type)) },
					{ "context", manager.GetSceneContextDisplayName(winner.context) },
					{ "count", std::to_string(winner.settings) } },
				"{layer}: {context} ({count})");
			ImGui::PushID(&winner);
			if (BrowserUI::Link("##override", label.c_str()))
				SceneSettingsUI::OpenSceneContext(winner.context, featureShortName);
			ImGui::PopID();
			Util::AddTooltip(T(TKEY("layer_override_link_tooltip"), "Open this layer with the feature selected, to change the value it applies."));
		}
		ImGui::Unindent();

		Util::Text::WrappedSecondary("%s",
			T(TKEY("layer_override_hint"),
				"Blue settings show the value a scene layer applies. Changing one here only previews it until you leave "
				"this feature; change it on that layer to keep it."));
	}

	/** @brief What an edit on a scene layer does, naming the period, weather or place it applies to. */
	std::string DescribeSceneLayer(const SceneSettingsManager& manager, const SceneContextId& context)
	{
		const auto name = manager.GetSceneContextDisplayName(context);
		switch (LayerOf(context.type)) {
		case Layer::Interior:
			return T(TKEY("layer_note_interior"), "Overrides Base in every interior. Location overrides still win over it.");
		case Layer::Weather:
			return I18n::GetSingleton()->Format(TKEY("layer_note_weather"), { { "weather", name } },
				"Overrides Base and Time of day while the weather is {weather}. Location overrides still win over it.");
		case Layer::Location:
			return I18n::GetSingleton()->Format(TKEY("layer_note_location"), { { "location", name } },
				"Overrides every other layer while you are in {location}.");
		default:
			return I18n::GetSingleton()->Format(TKEY("layer_note_time_of_day"), { { "period", name } },
				"Overrides Base outdoors during {period}. Weather and location overrides still win over it.");
		}
	}
}

void SceneLayerHeader::DrawBase(const std::string& featureShortName)
{
	DrawStack(Layer::Base, Util::IsInterior(), featureShortName);
	Util::Text::WrappedSecondary("%s",
		T(TKEY("layer_note_base"),
			"Applies everywhere. The scene layers to the right override it while they match, and the narrowest one wins."));

	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager || featureShortName.empty())
		return;
	if (manager->IsFeatureSceneControlled(featureShortName))
		DrawOverrides(*manager, featureShortName);
	else if (manager->HasAnySceneEntriesForFeature(featureShortName))
		Util::Text::WrappedDisabled("%s",
			T(TKEY("layer_overrides_elsewhere"), "Scene layers hold settings for this feature, but none apply here right now."));
}

void SceneLayerHeader::DrawScene(const SceneContextId& context, const std::string& featureShortName)
{
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return;

	const Layer layer = LayerOf(context.type);
	// A location applies indoors and out, so it shows the stack the player is in now.
	const bool interior = layer == Layer::Interior || (layer == Layer::Location && Util::IsInterior());
	DrawStack(layer, interior, featureShortName);
	Util::Text::WrappedSecondary("%s", DescribeSceneLayer(*manager, context).c_str());
}

#undef I18N_KEY_PREFIX
