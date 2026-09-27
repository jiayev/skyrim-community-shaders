#include "Switches.h"

#include <ShlObj.h>
#include <Windows.h>

#include <fstream>
#include <mutex>

#include <ankerl/unordered_dense.h>

namespace DCLF
{
	namespace
	{
		constexpr const char* kFileName = "CommunityShaders-DCLF.ini";

		std::filesystem::path SwitchesPath()
		{
			const auto directory = SwitchesDirectory();
			return directory.empty() ? directory : directory / kFileName;
		}

		// NAME=value per line; blank lines and lines starting with ';' or '#' are ignored.
		ankerl::unordered_dense::map<std::string, std::string> LoadFile()
		{
			ankerl::unordered_dense::map<std::string, std::string> values;
			const auto path = SwitchesPath();
			if (path.empty())
				return values;
			std::ifstream file(path);
			if (!file)
				return values;
			std::string line;
			while (std::getline(file, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
					line.pop_back();
				const auto first = line.find_first_not_of(" \t");
				if (first == std::string::npos || line[first] == ';' || line[first] == '#')
					continue;
				const auto equals = line.find('=', first);
				if (equals == std::string::npos)
					continue;
				auto name = line.substr(first, equals - first);
				while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
					name.pop_back();
				values[name] = line.substr(equals + 1);
			}
			return values;
		}

		const ankerl::unordered_dense::map<std::string, std::string>& FileValues()
		{
			static const auto values = LoadFile();
			return values;
		}

		ankerl::unordered_dense::map<std::string, std::string>& Resolved()
		{
			static ankerl::unordered_dense::map<std::string, std::string> resolved;
			return resolved;
		}

		std::mutex& ResolvedMutex()
		{
			static std::mutex mutex;
			return mutex;
		}
	}

	std::filesystem::path SwitchesDirectory()
	{
		PWSTR documents = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)))
			return {};
		std::filesystem::path path(documents);
		CoTaskMemFree(documents);
		return path / L"My Games" / L"Skyrim Special Edition" / L"SKSE";
	}

	std::string SwitchValue(const char* a_name)
	{
		const std::lock_guard lock(ResolvedMutex());
		auto& resolved = Resolved();
		if (const auto it = resolved.find(a_name); it != resolved.end())
			return it->second;
		std::string value;
		char buffer[1024] = {};
		if (const auto length = GetEnvironmentVariableA(a_name, buffer, sizeof(buffer)); length && length < sizeof(buffer)) {
			value.assign(buffer, length);
		} else if (const auto it = FileValues().find(a_name); it != FileValues().end()) {
			value = it->second;
		}
		resolved.emplace(a_name, value);
		return value;
	}

	bool SwitchEnabled(const char* a_name)
	{
		return SwitchValue(a_name) == "1";
	}

	std::string ReducedFeatures()
	{
		struct Feature
		{
			const char* name;
			bool (*reduced)(const std::string&);
		};
		// Each feature's switch and the values that turn it off or narrow it, as its own reader interprets them.
		constexpr auto offUnlessOne = [](const std::string& a_value) { return !a_value.empty() && a_value != "1"; };
		constexpr auto zero = [](const std::string& a_value) { return a_value == "0"; };
		constexpr auto offWord = [](const std::string& a_value) { return a_value == "off"; };
		static constexpr Feature features[] = {
			{ "CS_DCLF_ASYNC", [](const std::string& a_value) { return a_value == "off" || a_value == "0" || a_value == "probe"; } },
			{ "CS_DCLF_ASYNC_JOBS", [](const std::string& a_value) { return !a_value.empty(); } },
			{ "CS_ORG_EPOCHS", zero },
			{ "CS_ORG_ASYNC_EPOCHS", zero },
			{ "CS_ORG_CLOSED", zero },
			{ "CS_ORG_BATCH_SUBMIT", zero },
			{ "CS_ORG_EARLY_FLUSH", zero },
			{ "CS_DCLF_EARLY_SCENE", zero },
			{ "CS_DCLF_HYBRID", offUnlessOne },
			{ "CS_DCLF_SHADOWS", offUnlessOne },
			{ "CS_DCLF_SUN_SKIP", offUnlessOne },
			{ "CS_DCLF_SUN_EXCLUDE", offUnlessOne },  // `probe` runs it dry
			{ "CS_DCLF_PRIMARY_EXCLUDE", offUnlessOne },  // `probe` is the census
			{ "CS_DCLF_SKYLIGHT", offUnlessOne },
			{ "CS_DCLF_SKINNED", offUnlessOne },
			{ "CS_DCLF_SKIN_PARTITIONS", offUnlessOne },
			{ "CS_DCLF_ACTORS", offUnlessOne },
			{ "CS_DCLF_TREES", offUnlessOne },
			{ "CS_DCLF_DECALS", offUnlessOne },
			{ "CS_DCLF_FADING", offUnlessOne },
			{ "CS_DCLF_LOD_CROSSFADE", offUnlessOne },
			{ "CS_DCLF_MTLAND", offUnlessOne },
			{ "CS_DCLF_PROJECTED_UV", offUnlessOne },
			{ "CS_DCLF_SWITCH_NODES", offUnlessOne },
			{ "CS_DCLF_FACEGEN", zero },
			{ "CS_DCLF_SHADOW_ONLY", zero },
			{ "CS_DCLF_BINDLESS", zero },
			{ "CS_DCLF_BINDLESS_DRAW", zero },
			{ "CS_DCLF_BUILD_CACHE", zero },
			{ "CS_DCLF_CLASSIFY_CACHE", offWord },
			{ "CS_DCLF_DERIVED_CACHE", offWord },
			{ "CS_DCLF_MATERIAL_CACHE", offWord },
			{ "CS_DCLF_CULL", [](const std::string& a_value) { return a_value == "off" || a_value == "frustum"; } },
			{ "CS_DCLF_EVAL", [](const std::string& a_value) { return a_value == "off" || a_value == "material"; } },
			{ "CS_DCLF_OBJECT_SLOTS", zero },
			{ "CS_DCLF_SCENE_DELTA", zero },
			{ "CS_DCLF_FRAME_PUSH", zero },
			{ "CS_DCLF_COLOUR_EQUAL", zero },
			{ "CS_DCLF_DECAL_DEPTH", zero },
			{ "CS_DCLF_DGC_PREPROCESS", zero },
		};
		std::string text;
		for (const auto& feature : features) {
			const auto value = SwitchValue(feature.name);
			if (!feature.reduced(value))
				continue;
			if (!text.empty())
				text += ", ";
			text += std::string(feature.name) + "=" + value;
		}
		return text;
	}

	std::string SwitchSummary()
	{
		std::string text;
		// The file's own entries, plus whatever the environment adds on top of them.
		auto add = [&](const std::string& a_name) {
			const auto value = SwitchValue(a_name.c_str());
			if (value.empty())
				return;
			if (!text.empty())
				text += ", ";
			text += a_name + "=" + value;
		};
		for (const auto& [name, value] : FileValues())
			add(name);
		for (const char* name : { "CS_DCLF", "CS_DCLF_HYBRID", "CS_DCLF_CULL", "CS_DCLF_STATS", "CS_DCLF_DEBUG_VIEW",
				 "CS_DCLF_BUILD_PARITY", "CS_DCLF_CAPTURE_PARITY", "CS_DCLF_HYBRID_NOSKIP", "CS_DCLF_ONLY_ELIGIBLE",
				 "CS_DCLF_SHADER_DEBUG", "CS_DCLF_SHADER_SOURCE_DIR", "CS_DCLF_ASYNC", "CS_DCLF_ASYNC_JOBS",
				 "CS_DCLF_ASYNC_WAIT_MS", "CS_DCLF_ASYNC_PRIORITY" }) {
			if (!FileValues().contains(name))
				add(name);
		}
		return text.empty() ? std::string("none set") : text;
	}
}
