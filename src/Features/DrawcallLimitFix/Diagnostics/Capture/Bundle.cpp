// The capture's worker side: the files the render thread left to it, the logs, the manifest, and the zip (Windows' own tar.exe).

#include "CaptureInternal.h"

#include "Utils/Clipboard.h"
#include "Utils/FileSystem.h"

#include <Psapi.h>

#include <algorithm>
#include <fstream>

namespace DCLF::Capture
{
	namespace
	{
		constexpr std::uintmax_t kLogHead = 1ull << 20;   // the start of a long log: the session's setup lines
		constexpr std::uintmax_t kLogTail = 32ull << 20;  // and its end: the lines leading up to the capture

		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		void ReplaceAll(std::string& a_text, const std::string& a_lowerNeedle, std::string_view a_with)
		{
			if (a_lowerNeedle.empty())
				return;
			auto lower = Lower(a_text);
			for (std::size_t at = lower.find(a_lowerNeedle); at != std::string::npos; at = lower.find(a_lowerNeedle, at + a_with.size())) {
				a_text.replace(at, a_lowerNeedle.size(), a_with);
				lower.replace(at, a_lowerNeedle.size(), a_with);
			}
		}

		bool Write(Session& a_session, const std::filesystem::path& a_path, std::string_view a_text, std::string_view a_section)
		{
			std::ofstream out(a_path, std::ios::binary);
			out.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
			const bool ok = static_cast<bool>(out);
			a_session.Note(std::string(a_section), ok ? "ok" : "failed: could not write the file");
			return ok;
		}

		/** @brief The text of a_path, its head and tail only past a_head + a_tail bytes. Empty when it cannot be read. */
		std::optional<std::string> ReadClipped(const std::filesystem::path& a_path, std::uintmax_t a_head, std::uintmax_t a_tail)
		{
			std::error_code error;
			const auto size = std::filesystem::file_size(a_path, error);
			if (error)
				return std::nullopt;
			std::ifstream in(a_path, std::ios::binary);
			if (!in)
				return std::nullopt;
			const auto read = [&](std::uintmax_t a_from, std::uintmax_t a_count) {
				std::string text(static_cast<std::size_t>(a_count), '\0');
				in.seekg(static_cast<std::streamoff>(a_from));
				in.read(text.data(), static_cast<std::streamsize>(a_count));
				text.resize(static_cast<std::size_t>(in.gcount()));
				in.clear();
				return text;
			};
			if (size <= a_head + a_tail)
				return read(0, size);
			return read(0, a_head) + fmt::format("\n\n[... {} bytes left out by the DCLF capture ...]\n\n", size - a_head - a_tail) + read(size - a_tail, a_tail);
		}

		/** @brief Copies a text log into logs/, clipped and redacted, or a binary file as it is. */
		void CopyLog(Session& a_session, const std::filesystem::path& a_from, bool a_text)
		{
			const auto name = a_from.filename().string();
			const auto section = "logs/" + name;
			if (!std::filesystem::exists(a_from))
				return;
			if (!a_text) {
				std::error_code error;
				std::filesystem::copy_file(a_from, a_session.dir / "logs" / a_from.filename(), std::filesystem::copy_options::overwrite_existing, error);
				a_session.Note(section, error ? "failed: " + error.message() : "ok");
				return;
			}
			if (auto text = ReadClipped(a_from, kLogHead, kLogTail))
				Write(a_session, a_session.dir / "logs" / a_from.filename(), Redact(std::move(*text)), section);
			else
				a_session.Note(section, "failed: could not read it");
		}

		std::string FileVersion(const std::wstring& a_path)
		{
			DWORD handle = 0;
			const auto size = GetFileVersionInfoSizeW(a_path.c_str(), &handle);
			if (!size)
				return {};
			std::vector<std::byte> data(size);
			VS_FIXEDFILEINFO* info = nullptr;
			UINT length = 0;
			if (!GetFileVersionInfoW(a_path.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
				return {};
			return fmt::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
		}

		/** @brief The process's loaded DLLs, game-folder paths shown relative to the game folder. */
		std::string Modules()
		{
			std::vector<HMODULE> modules(1024);
			DWORD needed = 0;
			const auto process = GetCurrentProcess();
			if (!EnumProcessModules(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
				return "EnumProcessModules failed\n";
			modules.resize(std::min<std::size_t>(modules.size(), needed / sizeof(HMODULE)));
			wchar_t exe[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			const auto gameDir = Lower(Util::PathHelpers::ToUtf8(std::filesystem::path(exe).parent_path())) + "\\";
			std::vector<std::string> lines;
			for (auto module : modules) {
				wchar_t path[MAX_PATH]{};
				if (!GetModuleFileNameW(module, path, MAX_PATH))
					continue;
				auto text = Util::PathHelpers::ToUtf8(path);
				if (Lower(text).rfind(gameDir, 0) == 0)
					text = text.substr(gameDir.size());
				lines.push_back(fmt::format("{}  {}", text, FileVersion(path)));
			}
			std::ranges::sort(lines);
			std::string out;
			for (const auto& line : lines)
				out += line + '\n';
			return out;
		}

		std::string FramesCsv(const Session& a_session)
		{
			std::string out = "frame,present_ms,dclf_render_thread_us,adopt_wait_us,scene_lag_frames,epochs_not_ready,queued_coordinator,queued_preparation,"
							  "queued_scene,queued_streaming,loading,running,cpu_pipeline,interior\n";
			for (const auto& f : a_session.frames)
				out += fmt::format("{},{:.3f},{},{},{},{},{},{},{},{},{},{},{},{}\n", f.frame, f.presentMs, f.dclfUs, f.adoptWaitUs, f.sceneLag, f.notReady, f.queued[0],
					f.queued[1], f.queued[2], f.queued[3], f.loading ? 1 : 0, f.running ? 1 : 0, f.cpuPipeline ? 1 : 0, f.interior ? 1 : 0);
			return out;
		}

		/** @brief Zips a_dir into a_zip with Windows' tar.exe (Windows 10 1803 and later). Empty when it worked, or why not. */
		std::string Zip(const std::filesystem::path& a_dir, const std::filesystem::path& a_zip)
		{
			wchar_t system[MAX_PATH]{};
			GetSystemDirectoryW(system, MAX_PATH);
			const auto tar = std::filesystem::path(system) / L"tar.exe";
			if (!std::filesystem::exists(tar))
				return "tar.exe is not on this system";
			std::wstring command = L"\"" + tar.wstring() + L"\" -a -c -f \"" + a_zip.wstring() + L"\" -C \"" + a_dir.wstring() + L"\" .";
			STARTUPINFOW startup{ sizeof(startup) };
			PROCESS_INFORMATION process{};
			if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
				return fmt::format("tar.exe did not start (error {})", GetLastError());
			const auto waited = WaitForSingleObject(process.hProcess, 120'000);
			DWORD exitCode = 1;
			GetExitCodeProcess(process.hProcess, &exitCode);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			if (waited != WAIT_OBJECT_0)
				return "tar.exe took longer than two minutes";
			return exitCode == 0 ? std::string{} : fmt::format("tar.exe failed (exit code {})", exitCode);
		}

		std::string Manifest(Session& a_session, std::string_view a_zipError)
		{
			json j;
			j["format"] = 1;
			j["capture"] = a_session.id;
			j["seconds"] = a_session.seconds;
			j["fromMenu"] = a_session.fromMenu;
			j["startFrame"] = a_session.startFrame;
			j["endFrame"] = a_session.endFrame;
			j["started"] = std::format("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(a_session.startWall));
			j["windowSeconds"] = std::chrono::duration<double>(a_session.end - a_session.start).count();
			j["frames"] = a_session.frames.size();
			j["framesDropped"] = a_session.framesDropped;
			json sections = json::object();
			{
				std::lock_guard lock(a_session.notesMutex);
				for (const auto& [name, state] : a_session.sections)
					sections[name] = state;
			}
			j["sections"] = std::move(sections);
			if (!a_zipError.empty())
				j["zip"] = a_zipError;
			return j.dump(1);
		}
	}

	std::string Redact(std::string a_text)
	{
		static const std::string profile = [] {
			wchar_t value[MAX_PATH]{};
			const auto length = GetEnvironmentVariableW(L"USERPROFILE", value, MAX_PATH);
			return length && length < MAX_PATH ? Lower(Util::PathHelpers::ToUtf8(std::filesystem::path(value))) : std::string{};
		}();
		if (profile.empty())
			return a_text;
		ReplaceAll(a_text, profile, "%USERPROFILE%");
		auto forward = profile;
		std::ranges::replace(forward, '\\', '/');
		ReplaceAll(a_text, forward, "%USERPROFILE%");
		return a_text;
	}

	std::filesystem::path Package(Session& a_session, std::string& a_error)
	{
		auto& s = a_session;
		Write(s, s.dir / "frames.csv", FramesCsv(s), "frames.csv");
		Write(s, s.dir / "environment.json", Redact(s.environment), "environment.json");
		Write(s, s.dir / "features.json", s.features, "features.json");
		Write(s, s.dir / "settings.json", Redact(s.settings), "settings.json");
		Write(s, s.dir / "modules.txt", Modules(), "modules.txt");
		if (s.mainThread.valid() && s.mainThread.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
			try {
				const auto info = s.mainThread.get();
				Write(s, s.dir / "loadorder.txt", info.loadOrder, "loadorder.txt");
				Write(s, s.dir / "location.json", info.location, "location.json");
			} catch (const std::exception& e) {
				s.Note("loadorder.txt", fmt::format("failed: {}", e.what()));
			}
		} else
			s.Note("loadorder.txt", "skipped: the main thread did not answer within 5 s");

		// The logs beside CommunityShaders.log: its own, DXVK's (and its device fault dump), the switch files.
		if (const auto logDir = logger::log_directory()) {
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(*logDir, error)) {
				if (!entry.is_regular_file())
					continue;
				const auto name = Lower(entry.path().filename().string());
				const bool text = name == "communityshaders.log" || name.ends_with("_d3d11.log") || name.ends_with("_dxgi.log") ||
				                  name == "communityshaders-dclf.ini" || name == "communityshaders.env";
				// The watchdog's hang reports (HangWatchdog.h): every thread's stack when the render thread stopped presenting.
				const bool hangReport = name.starts_with("communityshaders-hang-") && name.ends_with(".txt");
				if (text || hangReport)
					CopyLog(s, entry.path(), true);
				else if (name.ends_with("_device_fault.bin"))
					CopyLog(s, entry.path(), false);
			}
		}
		// capture.log was written by the tee: redact it in place.
		if (auto text = ReadClipped(s.dir / "capture.log", kLogHead * 64, 0))
			Write(s, s.dir / "capture.log", Redact(std::move(*text)), "capture.log");

		const auto zip = std::filesystem::path(s.dir).concat(".zip");
		Write(s, s.dir / "manifest.json", Manifest(s, {}), "manifest.json");
		if (const auto zipError = Zip(s.dir, zip); !zipError.empty()) {
			a_error = zipError + "; the capture is left as a folder";
			Write(s, s.dir / "manifest.json", Manifest(s, zipError), "manifest.json");
			return s.dir;
		}
		std::error_code error;
		std::filesystem::remove_all(s.dir, error);
		if (s.fromMenu) {
			// Ready to paste into a message, and shown in Explorer.
			Util::CopyFileToClipboard(zip);
			std::wstring arguments = L"/select,\"" + zip.wstring() + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
		}
		return zip;
	}
}
