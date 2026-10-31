#pragma once

#include <string>
#include <string_view>

/**
 * @brief Owners from resource names, for VRAM attribution (Vram/VramLedger.h).
 *
 * CS names its D3D11 resources "Prefix::Description" (Util::SetResourceName, the Buffer.h wrappers) and its render-graph
 * resources "cs.<feature>.<name>". The prefixes are not always a feature's short name ("SSGI", "LLF", "DoF"), so they go through
 * an alias table before they are matched against the feature list.
 */
namespace Vram
{
	/** @brief "CS/<owner>" for a CS D3D11 name ("SSGI::Radiance" -> "CS/ScreenSpaceGI"); empty for a name without a prefix. */
	std::string OwnerFromD3DName(std::string_view a_name);

	/**
	 * @brief "CS/<owner>" for a CS render-graph name ("cs.dclf.grass-instances" -> "CS/DrawcallLimitFix"), "ORG/Internal" for the
	 * graph's own ("org.staged-uploads"); empty otherwise.
	 */
	std::string OwnerFromOrgName(std::string_view a_name);

	/** @brief "CS/<feature short name>" for a_feature's owner scopes; the string lives as long as the process. */
	const char* FeatureOwner(std::string_view a_shortName);
}
