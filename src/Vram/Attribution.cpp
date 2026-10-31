#include "Vram/Attribution.h"

#include "Feature.h"

#include <cctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Vram
{
	namespace
	{
		std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			for (auto& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		// Name prefixes that are not a feature's short name, by the owner they name.
		const std::unordered_map<std::string, std::string>& Aliases()
		{
			static const std::unordered_map<std::string, std::string> aliases = {
				// D3D11 "Prefix::" names
				{ "ssgi", "ScreenSpaceGI" },
				{ "llf", "LightLimitFix" },
				{ "sss", "SubsurfaceScattering" },
				{ "hdr", "HDRDisplay" },
				{ "ibl", "ImageBasedLighting" },
				{ "state", "Core" },
				{ "dof", "PostProcessing" },
				{ "physicalglare", "PostProcessing" },
				{ "motionblur", "PostProcessing" },
				{ "lensflare", "PostProcessing" },
				{ "texturemanager", "Effects11" },
				{ "enbdepthoffield", "Effects11" },
				{ "enbadaptation", "Effects11" },
				{ "enbeffectpostpass", "Effects11" },
				// Render-graph "cs.<x>." names
				{ "dclf", "DrawcallLimitFix" },
				{ "org", "RenderGraph" },
			};
			return aliases;
		}

		// A prefix's owner: the feature it names (by short name, case-insensitive, or an alias), else the prefix itself (CS's own
		// subsystems: Main, Deferred, Menu, ...).
		std::string Resolve(std::string_view a_prefix)
		{
			const std::string lower = Lower(a_prefix);
			if (const auto alias = Aliases().find(lower); alias != Aliases().end())
				return "CS/" + alias->second;
			for (auto* feature : Feature::GetFeatureList())
				if (Lower(feature->GetShortName()) == lower)
					return "CS/" + feature->GetShortName();
			return "CS/" + std::string(a_prefix);
		}
	}

	std::string OwnerFromD3DName(std::string_view a_name)
	{
		const auto separator = a_name.find("::");
		if (separator == std::string_view::npos || separator == 0)
			return {};
		const auto prefix = a_name.substr(0, separator);
		for (char c : prefix)
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
				return {};
		return Resolve(prefix);
	}

	std::string OwnerFromOrgName(std::string_view a_name)
	{
		// The graph's own ("org.staged-uploads", "UploadManager page").
		if (a_name.starts_with("org.") || a_name.starts_with("UploadManager"))
			return "ORG/Internal";
		if (!a_name.starts_with("cs."))
			return {};
		const auto rest = a_name.substr(3);
		const auto end = rest.find_first_of(".-");
		const auto prefix = rest.substr(0, end);
		return prefix.empty() ? std::string() : Resolve(prefix);
	}

	const char* FeatureOwner(std::string_view a_shortName)
	{
		static std::mutex mutex;
		static std::unordered_set<std::string> owners;
		std::string owner = "CS/" + std::string(a_shortName);
		std::lock_guard lock(mutex);
		return owners.insert(std::move(owner)).first->c_str();
	}
}
