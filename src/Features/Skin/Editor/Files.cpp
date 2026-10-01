#include "Files.h"

#include "../Runtime/Textures.h"
#include "TextureTools.h"

#include <commdlg.h>
#include <cwctype>
#include <fstream>
#include <set>

namespace SkinEditor
{
	std::string UTF8(const std::filesystem::path& a_path)
	{
		const auto text = a_path.u8string();
		return { text.begin(), text.end() };
	}

	std::optional<std::filesystem::path> ChooseFile(bool a_save, FileFilter a_filter, const wchar_t* a_extension, const std::filesystem::path& a_initial)
	{
		const char* label;
		const wchar_t* pattern;
		switch (a_filter) {
		case NifFilter:
			label = T("feature.skin.nif_filter", "Skyrim SE NIF");
			pattern = L"*.nif";
			break;
		case DDSFilter:
			label = T("feature.skin.dds_filter", "DDS texture");
			pattern = L"*.dds";
			break;
		case ImageFilter:
			label = T("feature.skin.image_filter", "Texture images");
			pattern = L"*.dds;*.png;*.tga;*.tif;*.tiff;*.bmp";
			break;
		case SchemeFilter:
			label = T("feature.skin.scheme_file_filter", "Skin character scheme");
			pattern = L"*.json";
			break;
		case ConfigurationFilter:
			label = T("feature.skin.configuration_file_filter", "Skin character configuration");
			pattern = L"*.json";
			break;
		default:
			label = T("feature.skin.legacy_filter", "Old material JSON");
			pattern = L"*.json";
			break;
		}
		std::wstring filter = winrt::to_hstring(label).c_str();
		filter.push_back(L'\0');
		filter.append(pattern);
		filter.append(2, L'\0');
		std::array<wchar_t, 32768> buffer{};
		const auto initial = a_initial.wstring();
		if (initial.size() >= buffer.size())
			throw std::runtime_error(T("feature.skin.file_path_is_too_long", "File path is too long."));
		std::copy(initial.begin(), initial.end(), buffer.begin());
		OPENFILENAMEW dialog{};
		dialog.lStructSize = static_cast<DWORD>(sizeof(dialog));
		dialog.hwndOwner = GetActiveWindow();
		dialog.lpstrFilter = filter.c_str();
		dialog.lpstrFile = buffer.data();
		dialog.nMaxFile = static_cast<DWORD>(buffer.size());
		dialog.lpstrDefExt = a_extension;
		dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
		               (a_save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
		if (a_save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))
			return std::filesystem::path(buffer.data());
		if (const auto error = CommDlgExtendedError())
			throw std::runtime_error(I18n::GetSingleton()->Format("feature.skin.file_dialog_failed", { { "code", std::format("{:08X}", error) } }, "File dialog failed ({code})."));
		return std::nullopt;
	}

	void Publish(std::span<const Output> a_outputs)
	{
		struct File
		{
			std::filesystem::path path, temporary, backup;
			Bytes before;
			const Output* output = nullptr;
			bool existed = false;
			bool replaced = false;
		};
		std::vector<File> files;
		std::set<std::wstring> destinations;
		try {
			for (const auto& output : a_outputs) {
				File file;
				file.path = std::filesystem::absolute(output.path).lexically_normal();
				auto key = file.path.wstring();
				std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
				if (!destinations.insert(key).second)
					throw std::runtime_error(T("feature.skin.more_than_one_output_uses_the_same_destination", "More than one output uses the same destination."));
				std::filesystem::create_directories(file.path.parent_path());
				file.output = &output;
				const auto temporary = file.path.wstring() + L".cs-skin." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
				file.temporary = temporary + L".tmp";
				for (uint32_t suffix = 1; std::filesystem::exists(file.temporary); ++suffix)
					file.temporary = temporary + L"." + std::to_wstring(suffix) + L".tmp";
				file.existed = std::filesystem::exists(file.path);
				if (file.existed) {
					file.before = ReadResource(UTF8(file.path));
					file.backup = file.path.wstring() + L".cs-skin.bak";
					for (uint32_t suffix = 1; std::filesystem::exists(file.backup); ++suffix)
						file.backup = file.path.wstring() + L".cs-skin." + std::to_wstring(suffix) + L".bak";
				}
				files.push_back(std::move(file));
				auto& staged = files.back();
				std::ofstream stream(staged.temporary, std::ios::binary);
				if (!stream.write(reinterpret_cast<const char*>(output.bytes.data()), static_cast<std::streamsize>(output.bytes.size())) || !stream.flush())
					throw std::runtime_error(T("feature.skin.could_not_stage", "Could not stage: ") + UTF8(staged.path));
				stream.close();
				if (ReadResource(UTF8(staged.temporary)) != output.bytes)
					throw std::runtime_error(T("feature.skin.staged_file_verification_failed", "Staged file verification failed: ") + UTF8(staged.path));
			}
			for (auto& file : files) {
				if (std::filesystem::exists(file.path) != file.existed || (file.existed && ReadResource(UTF8(file.path)) != file.before))
					throw std::runtime_error(T("feature.skin.output_changed_during_publication", "Output changed during publication: ") + UTF8(file.path));
				if (file.existed) {
					if (!CopyFileW(file.path.c_str(), file.backup.c_str(), TRUE)) {
						const auto code = GetLastError();
						throw std::runtime_error(I18n::GetSingleton()->Format("feature.skin.backup_write_failed", { { "path", UTF8(file.backup) }, { "code", std::to_string(code) } }, "Could not create backup: {path} (Windows error {code}). The original file has not been replaced."));
					}
					if (ReadResource(UTF8(file.backup)) != file.before)
						throw std::runtime_error(T("feature.skin.backup_verification_failed", "Backup verification failed: ") + UTF8(file.backup));
				}
				const auto unchanged = [&] {
					return std::filesystem::exists(file.path) == file.existed && (!file.existed || ReadResource(UTF8(file.path)) == file.before);
				};
				if (!unchanged())
					throw std::runtime_error(T("feature.skin.output_changed_during_publication", "Output changed during publication: ") + UTF8(file.path));
				const DWORD flags = MOVEFILE_WRITE_THROUGH | (file.existed ? MOVEFILE_REPLACE_EXISTING : 0);
				if (MoveFileExW(file.temporary.c_str(), file.path.c_str(), flags)) {
					file.replaced = true;
				} else {
					const auto code = GetLastError();
					if (!unchanged())
						throw std::runtime_error(T("feature.skin.output_changed_during_publication", "Output changed during publication: ") + UTF8(file.path));
					std::ofstream fallback(file.path, std::ios::binary | std::ios::trunc);
					if (fallback.is_open()) {
						file.replaced = true;
						fallback.write(reinterpret_cast<const char*>(file.output->bytes.data()), static_cast<std::streamsize>(file.output->bytes.size()));
						fallback.flush();
						fallback.close();
					}
					if (!file.replaced || fallback.fail())
						throw std::runtime_error(I18n::GetSingleton()->Format("feature.skin.publish_write_failed", { { "path", UTF8(file.path) }, { "code", std::to_string(code) } }, "Could not save: {path} (replacement failed with Windows error {code}; direct write also failed). Your draft is retained."));
					logger::warn("[Advanced Skin] Saved '{}' by direct write after replacement failed with Windows error {}", UTF8(file.path), code);
				}
				if (ReadResource(UTF8(file.path)) != file.output->bytes)
					throw std::runtime_error(T("feature.skin.saved_file_verification_failed", "Saved file verification failed: ") + UTF8(file.path));
				std::error_code ignored;
				std::filesystem::remove(file.temporary, ignored);
			}
		} catch (const std::exception& error) {
			std::string message = error.what();
			for (auto it = files.rbegin(); it != files.rend(); ++it) {
				if (!it->replaced)
					continue;
				bool restored = false;
				try {
					restored = it->existed ? CopyFileW(it->backup.c_str(), it->path.c_str(), FALSE) != FALSE && ReadResource(UTF8(it->path)) == it->before : DeleteFileW(it->path.c_str()) != FALSE;
				} catch (const std::exception&) {
				}
				if (!restored)
					message += T("feature.skin.recovery_required", " Recovery required: ") + UTF8(it->path) + T("feature.skin.backup", "; backup: ") + UTF8(it->backup);
			}
			for (const auto& file : files) {
				std::error_code ignored;
				std::filesystem::remove(file.temporary, ignored);
			}
			throw std::runtime_error(message);
		}
	}

	std::vector<Output> Package(const NifDocument& a_document, const std::filesystem::path& a_nifPath)
	{
		auto meshes = std::filesystem::absolute(a_nifPath).parent_path();
		while (!meshes.empty() && _wcsicmp(meshes.filename().c_str(), L"meshes") != 0) {
			const auto parent = meshes.parent_path();
			if (parent == meshes)
				break;
			meshes = parent;
		}
		if (_wcsicmp(meshes.filename().c_str(), L"meshes") != 0)
			throw std::runtime_error(T("feature.skin.package_meshes_folder", "Choose a NIF inside your mod's meshes folder. Its textures folder will be created alongside meshes."));
		std::vector<Output> outputs{ { a_nifPath, a_document.VerifiedBytes() } };
		std::set<std::string> included;
		for (const auto& surface : a_document.Surfaces()) {
			if (surface.definition.status != SkinMaterials::Status::Valid)
				continue;
			CheckTextures(surface.definition.material);
			for (const auto& path : surface.definition.material.textures) {
				const auto resource = SkinMaterials::NormalizeTexturePath(path);
				if (!resource.empty() && included.insert(resource).second)
					outputs.push_back({ meshes.parent_path() / std::filesystem::u8path(resource), ReadResource(resource) });
			}
		}
		return outputs;
	}

	std::string ImportTexture(const std::filesystem::path& a_source)
	{
		SkinTextures::Validate(UTF8(a_source));
		const auto root = std::filesystem::weakly_canonical("Data/textures");
		const auto relative = std::filesystem::weakly_canonical(a_source).lexically_relative(root);
		if (!relative.empty() && *relative.begin() != "..")
			return SkinMaterials::NormalizeTexturePath("textures/" + UTF8(relative));
		std::filesystem::create_directories(root / "CS/Skin");
		const auto destination = ChooseFile(true, DDSFilter, L"dds", root / "CS/Skin" / a_source.filename());
		if (!destination)
			return {};
		const auto target = std::filesystem::weakly_canonical(*destination).lexically_relative(root);
		if (target.empty() || *target.begin() == "..")
			throw std::runtime_error(T("feature.skin.for_game_preview_import_into_this_game_s_data_textures", "For game preview, import into this game's Data/textures folder (or a mod manager's redirected Data folder)."));
		const auto path = SkinMaterials::NormalizeTexturePath("textures/" + UTF8(target));
		const Output output{ *destination, ReadResource(UTF8(a_source)) };
		Publish(std::span(&output, 1));
		return path;
	}
}
