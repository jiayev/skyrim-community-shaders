#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

/// Shared Community Shaders / feature compatibility checks for unified packs and Scene Manager exports.
namespace PresetCompatibility
{
	struct SemVer
	{
		int major = 0;
		int minor = 0;
		int patch = 0;
	};

	/// How far the installed CS build lags behind a preset's required CS version.
	enum class VersionGap : std::uint8_t
	{
		None,   ///< Current is newer/equal, or only a patch behind — no UI warning
		Minor,  ///< Same major, lower minor — soft warning
		Major   ///< Lower major — strong warning
	};

	bool ParseSemVer(std::string_view text, SemVer& out);

	/** @return Gap when the running CS build is older than required; None when equal/newer or unparsable. */
	VersionGap CompareRequiredCsVersion(std::string_view requiredCsVersion);

	/// Feature short names from the pack that are not loaded in this session.
	std::vector<std::string> MissingRequiredFeatures(const std::vector<std::string>& requiredFeatures);

	/// Running Community Shaders version as MAJOR.MINOR.PATCH (no build suffix).
	std::string CurrentCsVersionString();

	struct Warning
	{
		VersionGap versionGap = VersionGap::None;
		std::string versionMessage;
		std::string versionMessageCompact;
		std::vector<std::string> missingFeatures;
		std::string featuresMessage;
	};

	Warning Evaluate(std::string_view requiredCsVersion, const std::vector<std::string>& requiredFeatures);

	/** @brief Draws compact coloured warning lines; returns whether anything was drawn. */
	bool DrawWarnings(const Warning& warning, bool compact);
}
