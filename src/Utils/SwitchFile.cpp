#include "SwitchFile.h"

#include <ShlObj.h>
#include <Windows.h>

#include <fstream>

#include <ankerl/unordered_dense.h>

namespace Util::SwitchFile
{
	namespace
	{
		constexpr const char* kFileName = "CommunityShaders-DCLF.ini";

		// NAME=value per line; blank lines and lines starting with ';' or '#' are ignored. Read once, on first use.
		const ankerl::unordered_dense::map<std::string, std::string>& File()
		{
			static const auto file = [] {
				ankerl::unordered_dense::map<std::string, std::string> values;
				const auto directory = Directory();
				if (directory.empty())
					return values;
				std::ifstream stream(directory / kFileName);
				if (!stream)
					return values;
				std::string line;
				while (std::getline(stream, line)) {
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
			}();
			return file;
		}

		std::string Environment(std::string_view a_name)
		{
			// Whatever its length (a test command list can run to kilobytes): the size first, then the value.
			const std::string name(a_name);
			const DWORD size = GetEnvironmentVariableA(name.c_str(), nullptr, 0);
			if (!size)
				return {};
			std::string value(size, char{});
			const DWORD length = GetEnvironmentVariableA(name.c_str(), value.data(), size);
			if (!length || length >= size)
				return {};
			value.resize(length);
			return value;
		}
	}

	std::filesystem::path Directory()
	{
		PWSTR documents = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)))
			return {};
		std::filesystem::path path(documents);
		CoTaskMemFree(documents);
		return path / L"My Games" / L"Skyrim Special Edition" / L"SKSE";
	}

	std::string Value(std::string_view a_name, bool a_environmentOnly)
	{
		auto value = Environment(a_name);
		if (value.empty() && !a_environmentOnly)
			if (const auto it = File().find(std::string(a_name)); it != File().end())
				value = it->second;
		return value;
	}

	std::vector<std::string> FileNames()
	{
		std::vector<std::string> names;
		names.reserve(File().size());
		for (const auto& [name, value] : File())
			names.push_back(name);
		return names;
	}
}
