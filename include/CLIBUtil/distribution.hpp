#pragma once

#include <filesystem>
#include <ranges>
#include <variant>

#include "string.hpp"

namespace RE
{
	using FormID = std::uint32_t;
}

namespace clib_util::distribution
{
	using namespace std::literals;

	enum record_type : std::uint32_t
	{
		kNone = 0,
		kFormIDMod,
		kMod,
		kFormID,
		kEditorID
	};

	using formid_pair = std::pair<std::optional<RE::FormID>, std::optional<std::string>>;
	using record = std::variant<formid_pair, std::string>;

	inline std::vector<std::string> get_configs(std::string_view a_folder, std::string_view a_suffix, std::string_view a_extension = ".ini"sv)
	{
		std::vector<std::string> configs{};

		for (const auto iterator = std::filesystem::directory_iterator(a_folder); const auto& entry : iterator) {
			if (entry.exists()) {
				if (const auto& path = entry.path(); !path.empty() && path.extension() == a_extension) {
					if (const auto& fileName = entry.path().string(); fileName.rfind(a_suffix) != std::string::npos) {
						configs.push_back(fileName);
					}
				}
			}
		}

		std::ranges::sort(configs);

		return configs;
	}

	inline bool is_mod_name(std::string_view a_str)
	{
		return a_str.contains(".es");
	}

	inline bool is_valid_entry(const std::string& a_str)
	{
		return !a_str.empty() && !string::icontains(a_str, "NONE"sv);
	}

	inline std::vector<std::string> split_entry(const std::string& a_str, std::string_view a_delimiter = ",")
	{
		return is_valid_entry(a_str) ? string::split(a_str, a_delimiter) : std::vector<std::string>();
	}

	inline record_type get_record_type(const std::string& a_str)
	{
		if (a_str.contains("~"sv)) {
			return kFormIDMod;
		}
		if (is_mod_name(a_str)) {
			return kMod;
		}
		if (string::is_only_hex(a_str)) {
			return kFormID;
		}
		return kEditorID;
	}

	inline record get_record(const record_type a_type, const std::string& a_str)
	{
		switch (a_type) {
		case kFormIDMod:
			{
				auto splitID = string::split(a_str, "~");
				return std::make_pair(string::to_num<RE::FormID>(splitID[0], true), splitID[1]);
			}
		case kFormID:
			return std::make_pair(string::to_num<RE::FormID>(a_str, true), std::nullopt);
		case kMod:
			return std::make_pair(std::nullopt, a_str);
		case kEditorID:
			return a_str;
		default:
			return record{};
		}
	}

	inline record get_record(const std::string& a_str)
	{
		return get_record(get_record_type(a_str), a_str);
	}
}