#pragma once

#include <utility>

#include "string.hpp"
#include "detail/SimpleIni.h"

namespace clib_util::ini
{
	template <class T>
	T& get_value(CSimpleIniA& a_ini, T& a_value, const char* a_section, const char* a_key, const char* a_comment, const char* a_delimiter = R"(|)")
	{
		if constexpr (std::is_same_v<T, bool>) {
			a_value = a_ini.GetBoolValue(a_section, a_key, a_value);
			a_ini.SetBoolValue(a_section, a_key, a_value, a_comment);
		} else if constexpr (std::is_floating_point_v<T>) {
			a_value = static_cast<float>(a_ini.GetDoubleValue(a_section, a_key, a_value));
			a_ini.SetDoubleValue(a_section, a_key, a_value, a_comment);
		} else if constexpr (std::is_enum_v<T>) {
			a_value = string::to_num<T>(a_ini.GetValue(a_section, a_key, std::to_string(std::to_underlying(a_value)).c_str()));
			a_ini.SetValue(a_section, a_key, std::to_string(std::to_underlying(a_value)).c_str(), a_comment);
		} else if constexpr (std::is_arithmetic_v<T>) {
			a_value = string::to_num<T>(a_ini.GetValue(a_section, a_key, std::to_string(a_value).c_str()));
			a_ini.SetValue(a_section, a_key, std::to_string(a_value).c_str(), a_comment);
		} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
			a_value = string::split(a_ini.GetValue(a_section, a_key, string::join(a_value, a_delimiter).c_str()), a_delimiter);
			a_ini.SetValue(a_section, a_key, string::join(a_value, a_delimiter).c_str(), a_comment);
		} else {
			a_value = a_ini.GetValue(a_section, a_key, a_value.c_str());
			a_ini.SetValue(a_section, a_key, a_value.c_str(), a_comment);
		}
		return a_value;
	}
}