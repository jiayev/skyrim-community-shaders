#pragma once

#include "NifDocument.h"

namespace SkinEditor
{
	std::string UTF8(const std::filesystem::path& a_path);
	enum FileFilter
	{
		NifFilter,
		DDSFilter,
		ImageFilter,
		SchemeFilter,
		ConfigurationFilter,
		LegacyFilter
	};
	std::optional<std::filesystem::path> ChooseFile(bool a_save, FileFilter a_filter, const wchar_t* a_extension, const std::filesystem::path& a_initial = {});
	struct Output
	{
		std::filesystem::path path;
		Bytes bytes;
	};
	void Publish(std::span<const Output> a_outputs);
	std::vector<Output> Package(const NifDocument& a_document, const std::filesystem::path& a_nifPath);
	std::string ImportTexture(const std::filesystem::path& a_source);
}
