// The diagnostic capture's state machine (see Capture.h). The render thread owns the session; the scene lane writes its sections at a
// pass's end; Bundle.cpp's worker writes the rest and zips it.

#include "CaptureInternal.h"

#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/PlayerParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/FrameSummary.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Diagnostics/TelemetryProfile.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeCensus.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Integrations/IntegrationList.h"

#include "DxvkLoader.h"
#include "Feature.h"
#include "Features/ScreenshotFeature.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/MenuIcons.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/LogTee.h"
#include "Utils/Notifications.h"
#include "Utils/UI.h"
#include "Vram/VramLedger.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <ctime>
#include <optional>
#include <thread>

namespace DCLF::Capture
{
	namespace
	{
		constexpr auto kMenuClosedDelay = std::chrono::seconds(2);
		constexpr auto kFinishTimeout = std::chrono::seconds(3);
		constexpr std::size_t kMaxFrames = 8192;

		std::atomic<Phase> phase{ Phase::Idle };
		std::atomic<float> progress{ 0.0f };
		std::mutex statusMutex;
		std::filesystem::path lastResult;
		std::string lastError;
		std::atomic<bool> packagingDone{ false };

		// Render thread only.
		std::shared_ptr<Session> session;
		std::uint32_t nextId = 1;
		float armedSeconds = 10.0f;
		bool armedImmediate = false;
		std::optional<std::chrono::steady_clock::time_point> menuClosedSince;
		LARGE_INTEGER lastPresent{};
		std::uint32_t presentCount = 0;
		int menuSeconds = 10;

		// The scene lane's requests.
		struct LaneRequest
		{
			std::weak_ptr<Session> session;
			std::string label;
		};
		std::mutex laneMutex;
		std::vector<LaneRequest> laneRequests;
		std::atomic<bool> laneWanted{ false };

		DrawcallLimitFix::CaptureInfo Dclf() { return globals::features::drawcallLimitFix.GetCaptureInfo(); }

		std::string Stamp(std::chrono::system_clock::time_point a_time)
		{
			const auto t = std::chrono::system_clock::to_time_t(a_time);
			std::tm local{};
			localtime_s(&local, &t);
			char text[32]{};
			std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
			return text;
		}

		void WriteText(Session& a_session, const std::filesystem::path& a_path, std::string_view a_text, std::string_view a_section)
		{
			const bool written = Util::FileHelpers::WriteFileAtomically(a_path, a_text, "DCLF capture");
			a_session.Note(std::string(a_section), written ? "ok" : "failed: could not write the file");
		}

		/** @brief Render thread: the start's or the end's snapshot.txt and vram.json, under a_label. */
		void WriteRenderSnapshot(Session& a_session, std::string_view a_label)
		{
			const auto dir = a_session.dir / a_label;
			std::string text = fmt::format("DCLF capture {} snapshot, frame {}\n", a_label, Scene::FrameView::Get().GetFrame());
			const auto section = [&](std::string_view a_name, const std::vector<std::string>& a_lines) {
				text += fmt::format("\n[{}]\n", a_name);
				for (const auto& line : a_lines)
					text += line + '\n';
			};
			const auto dclf = Dclf();
			section("integrations", Integrations::StatusLines());
			if (dclf.installed) {
				section("this frame", Reports::FrameSummary());
				std::vector<std::string> executor;
				const auto stats = SceneScheduler::Executor().GetStatistics();
				executor.push_back(fmt::format("jobs: accepted {}, completed {}, cancelled {}, rejected {}, failed {}", stats.accepted, stats.completed,
					stats.cancelled, stats.rejected, stats.failed));
				for (std::size_t d = 0; d < stats.queued.size(); ++d)
					executor.push_back(fmt::format("domain {}: queued {}, active {}, high water {}", d, stats.queued[d], stats.active[d], stats.highWater[d]));
				for (const auto& loop : SceneScheduler::Executor().GetParallelForStatistics(false))
					executor.push_back(fmt::format("parallel for {}: {} calls, {} chunks ({} by helpers), {} blocked for {:.3f} ms in all (longest {:.3f} ms)", loop.name,
						loop.calls, loop.chunks, loop.helperChunks, loop.blocked, loop.blockedNanoseconds / 1e6, loop.maxBlockedNanoseconds / 1e6));
				section("scene executor (cumulative since start)", executor);
				auto growth = Fades::RenderGrowthLines();
				for (auto& line : Integrations::AdvancedSkin::GrowthLines())
					growth.push_back(std::move(line));
				section("render-thread growth", growth);
			} else
				text += "\nDCLF is not installed: no frame state.\n";
			WriteText(a_session, dir / "snapshot.txt", text, fmt::format("{}/snapshot.txt", a_label));
			try {
				const auto written = Vram::WriteJson(dir);
				a_session.Note(fmt::format("{}/vram", a_label), written.empty() ? "failed: not written" : "ok: " + written.filename().string());
			} catch (const std::exception& e) {
				a_session.Note(fmt::format("{}/vram", a_label), fmt::format("failed: {}", e.what()));
			}
		}

		/** @brief Render thread: asks the scene lane for its sections (the fade census, the growth census) under a_label. */
		void AskLane(const std::shared_ptr<Session>& a_session, std::string a_label)
		{
			const auto dclf = Dclf();
			if (!dclf.installed || !dclf.unavailableReason.empty()) {
				a_session->Note(fmt::format("{}/scene", a_label), "skipped: DCLF is not running its scene work this session");
				return;
			}
			{
				std::lock_guard lock(laneMutex);
				laneRequests.push_back({ a_session, a_label });
			}
			a_session->laneAsked.fetch_add(1, std::memory_order_acq_rel);
			laneWanted.store(true, std::memory_order_release);
			Scene::Coordinator::Get().RequestActorProbe("capture-" + a_label);
			WakeScenePass(SceneWake::FrameInput);
		}

		std::string ToggleText(const ToggleSet& a_set)
		{
			std::string text = fmt::format("cull {}, pipeline {}, interiors {}, exteriors {}, cpu record {}", a_set.cullMode,
				a_set.pipelineMode == PipelineMode::Manual ? "manual" : "auto", CpuPipelineFor(a_set, true) ? "cpu" : "gpu",
				CpuPipelineFor(a_set, false) ? "cpu" : "gpu", a_set.cpuRecord);
			for (const auto& toggle : ToggleTable())
				text += fmt::format(", {} {}", toggle.name, ToggleValue(a_set, toggle) ? 1 : 0);
			return text;
		}

		/** @brief Render thread: the environment section (environment.json). */
		std::string BuildEnvironment()
		{
			const auto dclf = Dclf();
			json j;
			j["cs"] = { { "version", Plugin::VERSION.string() }, { "build", std::string(Plugin::BUILD_DESCRIBE) }, { "label", Util::GetBuildLabel() } };
			j["game"] = { { "runtime", std::string(magic_enum::enum_name(REL::Module::GetRuntime())) }, { "version", REL::Module::get().version().string() } };
			j["gpu"] = globals::state ? globals::state->adapterDescription : std::string{};
			j["dxvk"] = { { "loaded", DxvkLoader::IsLoaded() }, { "nativeRequested", DxvkLoader::NativeModeRequested() } };
			j["dclf"] = { { "installed", dclf.installed }, { "switchedOn", dclf.switchedOn }, { "running", dclf.running }, { "faulted", dclf.faulted },
				{ "pumpLagging", dclf.pumpLagging }, { "unavailableReason", dclf.unavailableReason } };
			j["switches"] = SwitchSummary();
			j["reducedFeatures"] = ReducedFeatures();
			j["reportInterval"] = Reports::Interval();
			const auto& o = Diagnostics::Observe();
			const std::pair<const char*, bool> observers[] = { { "CS_DCLF_PERSISTENT_PARITY", o.scene }, { "CS_DCLF_SET_PARITY", o.claims },
				{ "CS_DCLF_WALK_PARITY", o.walk }, { "CS_DCLF_CHANGE_LOG_PARITY", o.changeLog }, { "CS_DCLF_RESIDENT_PARITY", o.residents },
				{ "CS_DCLF_BUILD_PARITY", o.builds }, { "CS_DCLF_MIRROR_PARITY", o.mirror }, { "CS_DCLF_CAPTURE_PARITY", o.capture },
				{ "CS_DCLF_PARITY_BOTH", o.engineDrawsAll }, { "CS_DCLF_CAPTURE_POINT_PARITY", o.capturePoint },
				{ "CS_DCLF_ACCUMULATED_PARITY", o.accumulated }, { "CS_DCLF_FOLIAGE_PARITY", o.foliage }, { "CS_DCLF_STATS", o.stats },
				{ "CS_DCLF_PASS_STATS", o.passStats }, { "CS_DCLF_PROFILE", o.profile }, { "CS_DCLF_RELEASE_GUARD", o.releaseGuard },
				{ "CS_DCLF_FRAME_TRACE", o.frameTrace }, { "CS_DCLF_TARGET_PROBE", !o.targetProbe.empty() } };
			json set = json::array();
			for (const auto& [name, on] : observers)
				if (on)
					set.push_back(name);
			j["observers"] = set;
			if (AnyObserverSwitch())
				j["warning"] = "developer switches are set; several make the normal path slower";
			j["compatibilityMonitor"] = Diagnostics::PlayerParity::Active();
			if (dclf.installed) {
				j["toggles"] = { { "requested", ToggleText(Toggles::Get().Requested()) }, { "active", ToggleText(ActiveToggles()) } };
				const auto& conditions = Draws::CurrentGraphConditions();
				j["graph"] = { { "cpuPipeline", conditions.cpuPipeline }, { "interior", conditions.interior }, { "cpuForced", Draws::CpuPipelineForced() } };
				j["frame"] = Scene::FrameView::Get().GetFrame();
			}
			FILETIME created{}, exited{}, kernel{}, user{};
			if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
				FILETIME now{};
				GetSystemTimeAsFileTime(&now);
				const auto ticks = [](FILETIME a_time) { return (static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime; };
				j["uptimeSeconds"] = (ticks(now) - ticks(created)) / 10'000'000ull;
				j["cpuSeconds"] = { { "user", ticks(user) / 1e7 }, { "kernel", ticks(kernel) / 1e7 } };
			}
			return j.dump(1);
		}

		/** @brief Render thread: every feature, loaded or not (features.json). */
		std::string BuildFeatures()
		{
			json list = json::array();
			for (auto* feature : Feature::GetFeatureList()) {
				if (!feature)
					continue;
				const auto shortName = feature->GetShortName();
				list.push_back({ { "name", feature->GetName() }, { "shortName", shortName }, { "loaded", feature->loaded }, { "version", feature->version },
					{ "core", feature->IsCore() }, { "disabled", globals::state && globals::state->IsFeatureDisabled(shortName) } });
			}
			json j;
			j["features"] = std::move(list);
			j["dclfIntegrations"] = Integrations::StatusLines();
			return j.dump(1);
		}

		/** @brief Main thread (an SKSE task): the load order and the player's place. */
		MainThreadInfo ReadMainThread()
		{
			MainThreadInfo info;
			if (auto* data = RE::TESDataHandler::GetSingleton()) {
				const auto list = [&](RE::TESFile** a_files, std::uint32_t a_count, std::string_view a_kind) {
					for (std::uint32_t i = 0; a_files && i < a_count; ++i)
						if (a_files[i])
							info.loadOrder += fmt::format("{} {:3} {}\n", a_kind, i, a_files[i]->fileName);
				};
				list(data->GetLoadedMods(), data->GetLoadedModCount(), "full ");
				list(data->GetLoadedLightMods(), data->GetLoadedLightModCount(), "light");
			}
			json j = json::object();
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				const auto& position = player->GetPosition();
				j["position"] = { position.x, position.y, position.z };
				if (auto* cell = player->GetParentCell())
					j["cell"] = { { "formId", fmt::format("{:08X}", cell->GetFormID()) }, { "editorId", cell->GetFormEditorID() ? cell->GetFormEditorID() : "" },
						{ "interior", cell->IsInteriorCell() } };
				if (auto* world = player->GetWorldspace())
					j["worldspace"] = { { "formId", fmt::format("{:08X}", world->GetFormID()) }, { "editorId", world->GetFormEditorID() ? world->GetFormEditorID() : "" } };
			}
			info.location = j.dump(1);
			return info;
		}

		void Start()
		{
			const auto dclf = Dclf();
			auto s = std::make_shared<Session>();
			s->id = nextId++;
			s->seconds = armedSeconds;
			s->fromMenu = !armedImmediate;
			s->startWall = std::chrono::system_clock::now();
			s->start = std::chrono::steady_clock::now();
			s->startFrame = dclf.installed ? Scene::FrameView::Get().GetFrame() : presentCount;
			const auto logDir = logger::log_directory();
			if (!logDir) {
				std::lock_guard lock(statusMutex);
				lastError = "no log directory";
				phase = Phase::Idle;
				return;
			}
			s->dir = *logDir / ("DCLF-Capture-" + Stamp(s->startWall));
			std::error_code error;
			for (const auto* sub : { "start", "end", "telemetry", "logs" })
				std::filesystem::create_directories(s->dir / sub, error);
			if (error) {
				std::lock_guard lock(statusMutex);
				lastError = fmt::format("could not create {}: {}", s->dir.string(), error.message());
				phase = Phase::Idle;
				return;
			}
			// The tee first, so the flushed window before the capture (the time leading up to the click) is in the bundle too.
			try {
				auto tee = std::make_shared<spdlog::sinks::basic_file_sink_mt>((s->dir / "capture.log").string(), true);
				tee->set_pattern(Util::LogTee::kPattern);
				Util::LogTee::AddSink(tee);
				s->tee = std::move(tee);
			} catch (const std::exception& e) {
				s->Note("capture.log", fmt::format("failed: {}", e.what()));
			}
			logger::info("[DCLF] capture {}: started for {:.0f} s at frame {}; the next report window is the one before the capture", s->id, s->seconds, s->startFrame);
			Reports::captureStatistics.store(true, std::memory_order_relaxed);
			Reports::Flush();
			Diagnostics::TelemetryPhase("capture", s->dir / "telemetry");
			WriteRenderSnapshot(*s, "start");
			AskLane(s, "start");
			globals::features::screenshotFeature.RequestCapture(s->dir / "screenshot.png");
			s->frames.reserve(kMaxFrames);
			QueryPerformanceCounter(&lastPresent);
			progress = 0.0f;
			session = std::move(s);
			phase = Phase::Recording;
		}

		void Sample(Session& a_session)
		{
			static const double msPerTick = Reports::MsPerTick();
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			FrameSample f;
			f.presentMs = static_cast<float>((now.QuadPart - lastPresent.QuadPart) * msPerTick);
			lastPresent = now;
			const auto dclf = Dclf();
			f.running = dclf.running;
			if (dclf.installed) {
				f.frame = Scene::FrameView::Get().GetFrame();
				f.loading = Scene::FrameView::IsLoadingScreenUp();
				const auto& conditions = Draws::CurrentGraphConditions();
				f.cpuPipeline = conditions.cpuPipeline;
				f.interior = conditions.interior;
				const auto done = IndirectDraws::Get().ScenePassDoneFrame();
				f.sceneLag = f.frame > done ? f.frame - done : 0u;
				f.notReady = IndirectDraws::Get().GetStats().notReady;
				f.adoptWaitUs = IndirectDraws::Get().LastAdoptWaitUs();
				f.dclfUs = static_cast<std::uint32_t>(RenderThreadBudget::Get().LastFrame().totalUs);
				const auto stats = SceneScheduler::Executor().GetStatistics();
				for (std::size_t d = 0; d < f.queued.size() && d < stats.queued.size(); ++d)
					f.queued[d] = static_cast<std::uint32_t>(stats.queued[d]);
			} else
				f.frame = presentCount;
			if (a_session.frames.size() < a_session.frames.capacity())
				a_session.frames.push_back(f);
			else
				++a_session.framesDropped;
		}

		void EndWindow(const std::shared_ptr<Session>& a_session)
		{
			auto& s = *a_session;
			const auto dclf = Dclf();
			s.end = std::chrono::steady_clock::now();
			s.endFrame = dclf.installed ? Scene::FrameView::Get().GetFrame() : presentCount;
			logger::info("[DCLF] capture {}: the window ended at frame {} after {} Presents; the next report window covers the capture", s.id, s.endFrame,
				s.frames.size() + s.framesDropped);
			Reports::Flush();
			WriteRenderSnapshot(s, "end");
			AskLane(a_session, "end");
			Diagnostics::TelemetryPhase("off");
			s.environment = BuildEnvironment();
			s.features = BuildFeatures();
			if (globals::state) {
				json settings;
				globals::state->SaveToJson(settings);
				s.settings = settings.dump(1);
			}
			if (Diagnostics::PlayerParity::Active()) {
				const auto result = Diagnostics::PlayerParity::WriteReport(s.dir / "compat.txt");
				s.Note("compat.txt", "ok: " + result);
			} else
				s.Note("compat.txt", "skipped: the compatibility monitor is off");
			// The load order and the player's place are the main thread's to read.
			auto promise = std::make_shared<std::promise<MainThreadInfo>>();
			s.mainThread = promise->get_future().share();
			if (auto* tasks = SKSE::GetTaskInterface())
				tasks->AddTask([promise] {
					try {
						promise->set_value(ReadMainThread());
					} catch (...) {
						promise->set_exception(std::current_exception());
					}
				});
			else
				promise->set_value({});
			s.finishDeadline = std::chrono::steady_clock::now() + kFinishTimeout;
			s.finishFrames = 0;
			phase = Phase::Finishing;
		}

		void DetachTee(Session& a_session)
		{
			if (!a_session.tee)
				return;
			Util::LogTee::RemoveSink(a_session.tee);
			a_session.tee->flush();
			a_session.tee.reset();
		}

		void Finish(std::shared_ptr<Session> a_session)
		{
			auto& s = *a_session;
			const auto asked = s.laneAsked.load(std::memory_order_acquire), done = s.laneDone.load(std::memory_order_acquire);
			if (done < asked)
				s.Note("scene", fmt::format("timed out: {} of {} scene sections written", done, asked));
			// The flushed reports have run (the render thread's at the frame after the end, the scene lane's at its pass), so the
			// statistics go quiet again.
			Reports::captureStatistics.store(false, std::memory_order_relaxed);
			logger::info("[DCLF] capture {}: packaging {}", s.id, s.dir.string());
			DetachTee(s);
			phase = Phase::Packaging;
			std::thread([a_session] {
				std::string error;
				std::filesystem::path result;
				try {
					result = Package(*a_session, error);
				} catch (const std::exception& e) {
					error = fmt::format("packaging failed: {}", e.what());
					result = a_session->dir;
				}
				{
					std::lock_guard lock(statusMutex);
					lastResult = result;
					lastError = error;
				}
				logger::info("[DCLF] capture {}: written to {}{}", a_session->id, result.string(), error.empty() ? "" : " (" + error + ")");
				packagingDone.store(true, std::memory_order_release);
				phase = Phase::Idle;
			}).detach();
			session.reset();
		}
	}

	void Request(float a_seconds, bool a_immediate)
	{
		if (phase.load() != Phase::Idle)
			return;
		armedSeconds = std::clamp(a_seconds, 1.0f, 120.0f);
		armedImmediate = a_immediate;
		menuClosedSince.reset();
		{
			std::lock_guard lock(statusMutex);
			lastError.clear();
		}
		phase = Phase::Armed;
		logger::info("[DCLF] capture: armed for {:.0f} s{}", armedSeconds, a_immediate ? "" : "; it starts once the menu is closed");
	}

	void Cancel()
	{
		const auto now = phase.load();
		if (now == Phase::Armed) {
			phase = Phase::Idle;
			return;
		}
		if ((now != Phase::Recording && now != Phase::Finishing) || !session)
			return;
		logger::info("[DCLF] capture {}: cancelled", session->id);
		Reports::captureStatistics.store(false, std::memory_order_relaxed);
		if (now == Phase::Recording)
			Diagnostics::TelemetryPhase("off");
		DetachTee(*session);
		std::error_code error;
		std::filesystem::remove_all(session->dir, error);
		session.reset();
		phase = Phase::Idle;
	}

	Status GetStatus()
	{
		Status status;
		status.phase = phase.load();
		status.progress = progress.load();
		std::lock_guard lock(statusMutex);
		status.lastResult = lastResult;
		status.lastError = lastError;
		return status;
	}

	bool Recording() { return phase.load(std::memory_order_relaxed) == Phase::Recording; }

	void OnPresent()
	{
		++presentCount;
		if (packagingDone.exchange(false, std::memory_order_acq_rel)) {
			const auto status = GetStatus();
			Util::Notifications::Show(fmt::format("{} {}", T("feature.dclf.capture.written", "DCLF capture written:"), status.lastResult.filename().string()),
				status.lastError.empty() ? Util::Colors::GetSuccess() : Util::Colors::GetWarning());
		}
		switch (phase.load(std::memory_order_relaxed)) {
		case Phase::Armed:
			{
				if (armedImmediate) {
					Start();
					break;
				}
				const auto now = std::chrono::steady_clock::now();
				if (globals::menu && globals::menu->IsOpen())
					menuClosedSince.reset();
				else if (!menuClosedSince)
					menuClosedSince = now;
				else if (now - *menuClosedSince >= kMenuClosedDelay)
					Start();
				break;
			}
		case Phase::Recording:
			{
				if (!session)
					break;
				Sample(*session);
				const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - session->start).count();
				progress = std::min(1.0f, elapsed / session->seconds);
				if (elapsed >= session->seconds)
					EndWindow(session);
				break;
			}
		case Phase::Finishing:
			{
				if (!session)
					break;
				const bool laneDone = session->laneDone.load(std::memory_order_acquire) >= session->laneAsked.load(std::memory_order_acquire);
				if ((laneDone && ++session->finishFrames >= 2) || std::chrono::steady_clock::now() > session->finishDeadline)
					Finish(session);
				break;
			}
		default:
			break;
		}
	}

	void RunSceneLaneWork()
	{
		if (!laneWanted.exchange(false, std::memory_order_acq_rel))
			return;
		std::vector<LaneRequest> requests;
		{
			std::lock_guard lock(laneMutex);
			requests.swap(laneRequests);
		}
		for (auto& request : requests) {
			const auto s = request.session.lock();
			if (!s)
				continue;
			const auto dir = s->dir / request.label;
			try {
				auto& coordinator = Scene::Coordinator::Get();
				WriteText(*s, dir / "fades.json", coordinator.FadeCensus().dump(1), request.label + "/fades.json");
				std::string growth;
				for (const auto& line : coordinator.SceneGrowthLines())
					growth += line + '\n';
				WriteText(*s, dir / "scene-growth.txt", growth, request.label + "/scene-growth.txt");
			} catch (const std::exception& e) {
				s->Note(request.label + "/scene", fmt::format("failed: {}", e.what()));
			}
			s->laneDone.fetch_add(1, std::memory_order_acq_rel);
		}
	}

	void DrawControls()
	{
		const auto status = GetStatus();
		switch (status.phase) {
		case Phase::Idle:
			{
				if (Util::LabeledButton("##DclfCapture", Icons::TI(ICON_TI_FILE_EXPORT), T("feature.dclf.capture.button", "Create diagnostic capture")))
					Request(static_cast<float>(menuSeconds), false);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(T("feature.dclf.capture.button_tooltip",
						"Records what DCLF is doing for a few seconds and writes one zip to send to the developer. Press it while the problem is "
						"visible (slow frames, see-through characters), then close the menu: the capture starts two seconds later. The zip "
						"holds your logs, settings, load order and a screenshot."));
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
				ImGui::SliderInt("##DclfCaptureSeconds", &menuSeconds, 5, 30, T("feature.dclf.capture.seconds", "%d s"));
				break;
			}
		case Phase::Armed:
			ImGui::TextWrapped("%s", T("feature.dclf.capture.armed", "Close the menu: the capture starts two seconds after it closes."));
			if (Util::Button(T("common.cancel", "Cancel")))
				Cancel();
			break;
		case Phase::Recording:
			ImGui::ProgressBar(status.progress, ImVec2(ImGui::GetFontSize() * 16.0f, 0.0f), T("feature.dclf.capture.recording", "Recording"));
			ImGui::SameLine();
			if (Util::Button(T("common.cancel", "Cancel")))
				Cancel();
			break;
		case Phase::Finishing:
		case Phase::Packaging:
			ImGui::TextUnformatted(T("feature.dclf.capture.packaging", "Writing the capture..."));
			break;
		}
		if (status.phase == Phase::Idle && !status.lastResult.empty()) {
			ImGui::TextWrapped("%s %s", T("feature.dclf.capture.last", "Last capture:"), Util::PathHelpers::ToUtf8(status.lastResult).c_str());
			if (Util::Button(T("feature.dclf.capture.show", "Show in folder")))
				Util::FileHelpers::OpenWithShell(status.lastResult.parent_path());
		}
		if (!status.lastError.empty())
			ImGui::TextWrapped("%s", status.lastError.c_str());
	}
}
