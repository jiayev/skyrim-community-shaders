#include "PresetCompatibility.h"

#include "Feature.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Plugin.h"
#include "Utils/Format.h"

#include <cctype>
#include <charconv>
#include <format>
#include <system_error>

namespace PresetCompatibility
{
	namespace
	{
		bool ParseComponent(std::string_view text, size_t& cursor, int& out)
		{
			if (cursor >= text.size() || !std::isdigit(static_cast<unsigned char>(text[cursor])))
				return false;
			const size_t start = cursor;
			while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])))
				++cursor;
			const auto slice = text.substr(start, cursor - start);
			const auto* begin = slice.data();
			const auto* end = begin + slice.size();
			int value = 0;
			const auto result = std::from_chars(begin, end, value);
			if (result.ec != std::errc{} || result.ptr != end)
				return false;
			out = value;
			return true;
		}
	}

	bool ParseSemVer(std::string_view text, SemVer& out)
	{
		while (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
			text.remove_prefix(1);
		size_t cursor = 0;
		SemVer parsed{};
		if (!ParseComponent(text, cursor, parsed.major))
			return false;
		if (cursor >= text.size() || text[cursor] != '.')
			return false;
		++cursor;
		if (!ParseComponent(text, cursor, parsed.minor))
			return false;
		if (cursor >= text.size() || text[cursor] != '.')
			return false;
		++cursor;
		if (!ParseComponent(text, cursor, parsed.patch))
			return false;
		// Tolerate a trailing build segment (e.g. 0.8.0.0) by ignoring the rest.
		out = parsed;
		return true;
	}

	Feature::ReleaseStage ReleaseStageFromVersion(std::string_view version)
	{
		using enum Feature::ReleaseStage;
		SemVer parsed{};
		if (!ParseSemVer(version, parsed))
			return Release;
		if (parsed.major > 0)
			return Release;
		if (parsed.minor > 0)
			return Beta;
		return Alpha;
	}

	ImVec4 StageTagColor(Feature::ReleaseStage stage)
	{
		const auto& statusPalette = globals::menu->GetTheme().StatusPalette;
		return stage == Feature::ReleaseStage::Alpha ? statusPalette.Error : statusPalette.Warning;
	}

	VersionGap CompareRequiredCsVersion(std::string_view requiredCsVersion)
	{
		if (requiredCsVersion.empty())
			return VersionGap::None;

		SemVer required{};
		if (!ParseSemVer(requiredCsVersion, required))
			return VersionGap::None;

		const SemVer current{
			.major = static_cast<int>(Plugin::VERSION.major()),
			.minor = static_cast<int>(Plugin::VERSION.minor()),
			.patch = static_cast<int>(Plugin::VERSION.patch()),
		};

		if (current.major != required.major)
			return current.major < required.major ? VersionGap::Major : VersionGap::None;
		if (current.minor != required.minor)
			return current.minor < required.minor ? VersionGap::Minor : VersionGap::None;
		// Patch-only lag is intentionally silent.
		return VersionGap::None;
	}

	std::vector<std::string> MissingRequiredFeatures(const std::vector<std::string>& requiredFeatures)
	{
		std::vector<std::string> missing;
		for (const auto& shortName : requiredFeatures) {
			if (shortName.empty())
				continue;
			if (!Feature::FindFeatureByShortName(shortName))
				missing.push_back(shortName);
		}
		return missing;
	}

	std::string CurrentCsVersionString()
	{
		return Util::GetFormattedVersion(Plugin::VERSION);
	}

	Warning Evaluate(std::string_view requiredCsVersion, const std::vector<std::string>& requiredFeatures)
	{
		Warning warning;
		warning.versionGap = CompareRequiredCsVersion(requiredCsVersion);
		warning.missingFeatures = MissingRequiredFeatures(requiredFeatures);

		if (warning.versionGap == VersionGap::Major) {
			warning.versionMessage = I18n::GetSingleton()->Format("menu.presets.compat_cs_major",
				{ { "required", std::string{ requiredCsVersion } },
					{ "current", CurrentCsVersionString() } },
				"Made for Community Shaders {required}. This build ({current}) is a major version behind and may not work correctly.");
			warning.versionMessageCompact = I18n::GetSingleton()->Format("menu.presets.compat_cs_major_short",
				{ { "required", std::string{ requiredCsVersion } } },
				"Needs Community Shaders {required}+");
		} else if (warning.versionGap == VersionGap::Minor) {
			warning.versionMessage = I18n::GetSingleton()->Format("menu.presets.compat_cs_minor",
				{ { "required", std::string{ requiredCsVersion } },
					{ "current", CurrentCsVersionString() } },
				"Made for Community Shaders {required}. This build ({current}) is slightly older.");
			warning.versionMessageCompact = I18n::GetSingleton()->Format("menu.presets.compat_cs_minor_short",
				{ { "required", std::string{ requiredCsVersion } } },
				"Made for CS {required}");
		}

		if (!warning.missingFeatures.empty()) {
			std::string list;
			for (size_t i = 0; i < warning.missingFeatures.size(); ++i) {
				if (i > 0)
					list += ", ";
				list += warning.missingFeatures[i];
			}
			warning.featuresMessage = I18n::GetSingleton()->Format("menu.presets.compat_features_missing",
				{ { "features", list } },
				"Missing required feature(s): {features}");
		}
		return warning;
	}

	bool DrawWarnings(const Warning& warning, bool compact)
	{
		const auto& theme = Menu::GetSingleton()->GetTheme();
		bool drew = false;

		const auto& versionText = compact && !warning.versionMessageCompact.empty() ?
		                              warning.versionMessageCompact :
		                              warning.versionMessage;

		if (warning.versionGap == VersionGap::Major && !versionText.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.Error);
			if (compact)
				ImGui::TextUnformatted(versionText.c_str());
			else
				ImGui::TextWrapped("%s", versionText.c_str());
			ImGui::PopStyleColor();
			drew = true;
		} else if (warning.versionGap == VersionGap::Minor && !versionText.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.Warning);
			if (compact)
				ImGui::TextUnformatted(versionText.c_str());
			else
				ImGui::TextWrapped("%s", versionText.c_str());
			ImGui::PopStyleColor();
			drew = true;
		}

		if (!warning.featuresMessage.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.Warning);
			if (compact)
				ImGui::TextUnformatted(warning.featuresMessage.c_str());
			else
				ImGui::TextWrapped("%s", warning.featuresMessage.c_str());
			ImGui::PopStyleColor();
			drew = true;
		}
		return drew;
	}
}
