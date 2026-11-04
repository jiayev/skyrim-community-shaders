#include "GpuEventTimers.h"

#include "Globals.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <d3dcompiler.h>
#include <tlhelp32.h>
#include <atomic>
#include <map>
#include <thread>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <unordered_map>
#include <vector>

#include <algorithm>
#include <chrono>
#include <map>

namespace GpuEventTimers
{
	namespace
	{
		// Frames in flight before a frame's queries are read back; a frame whose slot comes round again unread is dropped.
		// Nothing in the frame waits for the GPU (the upscaler's submissions are enqueued), so the render thread can run
		// several frames ahead: with 4, every frame was dropped with DCLF off.
		constexpr std::uint32_t kFrames = 8;
		// Queries per frame (two an event, two for the frame); events past it are not timed, and counted.
		constexpr std::uint32_t kMaxQueries = 4096;
		constexpr std::uint32_t kNone = ~0u;
		// Paths below this average inclusive time a frame are left out of the log.
		constexpr double kMinLoggedMs = 0.02;

		std::uint32_t ReadInterval()
		{
			char value[32] = {};
			const DWORD length = GetEnvironmentVariableA("CS_GPU_EVENT_TIMERS", value, sizeof(value));
			return length && length < sizeof(value) ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
		}

		struct Event
		{
			std::uint32_t path;
			std::uint32_t parent;  // the enclosing event's index in the frame, or kNone
			std::uint32_t begin, end = kNone;  // query indices
		};

		struct Frame
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			std::vector<winrt::com_ptr<ID3D11Query>> queries;
			std::uint32_t used = 0;
			std::uint32_t frameBegin = kNone, frameEnd = kNone;
			std::vector<Event> events;
			bool open = false, pending = false, overflow = false;
			std::uint32_t window = 0;  // the window it was recorded in (StartWindow)
			std::chrono::steady_clock::time_point cpuBegin{};
			double cpuMs = 0.0;  // Present to Present on the render thread
		};

		// The worst single stretch outside any event in a logged block.
		struct WorstGap
		{
			double ms = 0.0, frameMs = 0.0, cpuMs = 0.0;
			std::uint32_t after = kNone, before = kNone;  // the top-level paths around it; kNone: the frame's start or end
			std::uint64_t frame = 0;
		};

		struct Accumulated
		{
			double inclusive = 0.0, self = 0.0;
			std::uint64_t count = 0;
		};

		struct State
		{
			std::uint32_t interval = 0;
			bool initialized = false, failed = false;
			DWORD thread = 0;
			Frame frames[kFrames];
			std::uint32_t write = 0;
			// The open events, innermost last: an index into the recording frame's events, or kNone for one begun while
			// nothing was recorded (before the first frame, on the far side of a Present), whose end is then ignored.
			std::vector<std::uint32_t> stack;
			std::vector<std::string> paths;  // in the order they were first seen: the frame's order, near enough
			std::vector<std::uint32_t> depth;
			std::unordered_map<std::string, std::uint32_t> pathIndex;
			std::vector<Accumulated> accumulated;
			double frameMs = 0.0;
			std::uint32_t collected = 0, dropped = 0, disjoint = 0, overflowed = 0;
			// Time outside any event, by the top-level paths on either side of it.
			std::map<std::pair<std::uint32_t, std::uint32_t>, double> gaps;
			WorstGap worstGap;
			std::uint64_t framesSeen = 0;
			// Windows (StartWindow): the one frames open in, the one the totals are of, and each one's label ("-": not logged). Window 0
			// is the interval's; once a window is started, the windows alone are logged.
			std::uint32_t window = 0, collecting = 0;
			std::vector<std::string> labels{ std::string{} };
		};

		State& Get()
		{
			static State state;
			return state;
		}

		bool Initialize(State& a_state)
		{
			auto* device = globals::d3d::device;
			if (!device || !globals::d3d::context)
				return false;
			D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			for (auto& frame : a_state.frames) {
				if (FAILED(device->CreateQuery(&desc, frame.disjoint.put())))
					return false;
			}
			a_state.initialized = true;
			logger::info("[GpuEvents] GPU event timers on: every perf event's GPU time, logged every {} frames", a_state.interval);
			return true;
		}

		/** @brief A timestamp in the recording frame, or kNone when the frame has used all its queries. */
		std::uint32_t Timestamp(Frame& a_frame)
		{
			if (a_frame.used >= kMaxQueries) {
				a_frame.overflow = true;
				return kNone;
			}
			if (a_frame.used == a_frame.queries.size()) {
				D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
				winrt::com_ptr<ID3D11Query> query;
				if (FAILED(globals::d3d::device->CreateQuery(&desc, query.put()))) {
					a_frame.overflow = true;
					return kNone;
				}
				a_frame.queries.push_back(std::move(query));
			}
			const std::uint32_t index = a_frame.used++;
			globals::d3d::context->End(a_frame.queries[index].get());
			return index;
		}

		std::uint32_t PathOf(State& a_state, std::uint32_t a_parentPath, std::string_view a_name)
		{
			std::string path = a_parentPath == kNone ? std::string(a_name) : a_state.paths[a_parentPath] + " / " + std::string(a_name);
			const auto [at, inserted] = a_state.pathIndex.try_emplace(path, static_cast<std::uint32_t>(a_state.paths.size()));
			if (inserted) {
				a_state.paths.push_back(std::move(path));
				a_state.depth.push_back(a_parentPath == kNone ? 0u : a_state.depth[a_parentPath] + 1);
				a_state.accumulated.emplace_back();
			}
			return at->second;
		}

		void Log(State& a_state);

		void ResetTotals(State& a_state)
		{
			for (auto& total : a_state.accumulated)
				total = {};
			a_state.frameMs = 0.0;
			a_state.collected = a_state.dropped = a_state.disjoint = a_state.overflowed = 0;
			a_state.gaps.clear();
			a_state.worstGap = {};
		}

		/** @brief Adds the stretches of a frame that no top-level event covers, named by the events on either side. */
		void AccumulateGaps(State& a_state, const Frame& a_frame, const std::vector<std::uint64_t>& a_ticks, double a_toMs)
		{
			++a_state.framesSeen;
			std::vector<const Event*> tops;
			for (const auto& event : a_frame.events)
				if (event.parent == kNone && event.begin != kNone && event.end != kNone)
					tops.push_back(&event);
			std::ranges::sort(tops, [&](const Event* a, const Event* b) { return a_ticks[a->begin] < a_ticks[b->begin]; });

			const auto frameMs = static_cast<double>(a_ticks[a_frame.frameEnd] - a_ticks[a_frame.frameBegin]) * a_toMs;
			auto add = [&](std::uint64_t a_from, std::uint64_t a_to, std::uint32_t a_after, std::uint32_t a_before) {
				if (a_to <= a_from)
					return;
				const double ms = static_cast<double>(a_to - a_from) * a_toMs;
				a_state.gaps[{ a_after, a_before }] += ms;
				if (ms > a_state.worstGap.ms)
					a_state.worstGap = { ms, frameMs, a_frame.cpuMs, a_after, a_before, a_state.framesSeen };
			};
			std::uint64_t cursor = a_ticks[a_frame.frameBegin];
			std::uint32_t after = kNone;
			for (const auto* event : tops) {
				add(cursor, a_ticks[event->begin], after, event->path);
				cursor = std::max(cursor, a_ticks[event->end]);
				after = event->path;
			}
			add(cursor, a_ticks[a_frame.frameEnd], after, kNone);
		}

		/** @brief Reads a finished frame's queries into the totals; false while the GPU has not reached its end. */
		bool Collect(State& a_state, Frame& a_frame, bool a_flush)
		{
			auto* context = globals::d3d::context;
			const UINT flags = a_flush ? 0u : UINT(D3D11_ASYNC_GETDATA_DONOTFLUSH);
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (context->GetData(a_frame.disjoint.get(), &disjoint, sizeof(disjoint), flags) != S_OK)
				return false;
			std::vector<std::uint64_t> ticks(a_frame.used);
			for (std::uint32_t i = 0; i < a_frame.used; ++i)
				if (context->GetData(a_frame.queries[i].get(), &ticks[i], sizeof(std::uint64_t), flags) != S_OK)
					return false;
			a_frame.pending = false;
			// The first frame of a later window: the window before is complete (frames are read back in order), so it is logged.
			if (a_frame.window != a_state.collecting) {
				if (a_state.labels[a_state.collecting] != "-")
					Log(a_state);
				else
					ResetTotals(a_state);
				a_state.collecting = a_frame.window;
			}
			if (disjoint.Disjoint || !disjoint.Frequency || a_frame.frameBegin == kNone || a_frame.frameEnd == kNone) {
				++a_state.disjoint;
				return true;
			}
			const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
			auto span = [&](std::uint32_t a_begin, std::uint32_t a_end) {
				return ticks[a_end] >= ticks[a_begin] ? static_cast<double>(ticks[a_end] - ticks[a_begin]) * toMs : 0.0;
			};
			std::vector<double> inclusive(a_frame.events.size(), -1.0), children(a_frame.events.size(), 0.0);
			for (std::size_t e = 0; e < a_frame.events.size(); ++e) {
				const auto& event = a_frame.events[e];
				if (event.begin == kNone || event.end == kNone)
					continue;
				inclusive[e] = span(event.begin, event.end);
				if (event.parent != kNone)
					children[event.parent] += inclusive[e];
			}
			for (std::size_t e = 0; e < a_frame.events.size(); ++e) {
				if (inclusive[e] < 0.0)
					continue;
				auto& total = a_state.accumulated[a_frame.events[e].path];
				total.inclusive += inclusive[e];
				total.self += std::max(0.0, inclusive[e] - children[e]);
				++total.count;
			}
			a_state.frameMs += span(a_frame.frameBegin, a_frame.frameEnd);
			AccumulateGaps(a_state, a_frame, ticks, toMs);
			a_state.overflowed += a_frame.overflow ? 1u : 0u;
			++a_state.collected;
			return true;
		}

		void Log(State& a_state)
		{
			const double frames = std::max(1u, a_state.collected);
			double annotated = 0.0;
			for (std::size_t p = 0; p < a_state.paths.size(); ++p)
				if (a_state.depth[p] == 0)
					annotated += a_state.accumulated[p].inclusive;
			const auto& label = a_state.labels[a_state.collecting];
			std::string text = fmt::format("[GpuEvents] {}{} frames ({} dropped unread, {} disjoint, {} out of queries): GPU frame {:.2f} ms, {:.2f} ms outside any event; per frame, inclusive / self ms, calls:",
				label.empty() ? std::string{} : fmt::format("window '{}': ", label), a_state.collected, a_state.dropped, a_state.disjoint, a_state.overflowed, a_state.frameMs / frames, (a_state.frameMs - annotated) / frames);
			for (std::size_t p = 0; p < a_state.paths.size(); ++p) {
				const auto& total = a_state.accumulated[p];
				if (total.inclusive / frames < kMinLoggedMs)
					continue;
				const auto& path = a_state.paths[p];
				const auto leaf = path.rfind(" / ");
				text += fmt::format("\n    {:>6.2f} {:>6.2f} {:>5.1f}  {}{}", total.inclusive / frames, total.self / frames, static_cast<double>(total.count) / frames,
					std::string(std::size_t(a_state.depth[p]) * 2, ' '), leaf == std::string::npos ? path : path.substr(leaf + 3));
			}
			logger::info("{}", text);

			// Where the time outside any event is. It is not all GPU work: the GPU idles here too if the CPU is late.
			auto name = [&](std::uint32_t a_path, const char* a_none) { return a_path == kNone ? std::string(a_none) : a_state.paths[a_path]; };
			std::vector<std::pair<double, std::pair<std::uint32_t, std::uint32_t>>> gaps;
			for (const auto& [where, ms] : a_state.gaps)
				gaps.emplace_back(ms, where);
			std::ranges::sort(gaps, std::greater{}, [](const auto& a_gap) { return a_gap.first; });
			std::string gapText = fmt::format("[GpuEvents] outside any event: worst {:.2f} ms in frame {} (GPU frame {:.2f} ms, CPU frame {:.2f} ms), between {} and {}; per frame:",
				a_state.worstGap.ms, a_state.worstGap.frame, a_state.worstGap.frameMs, a_state.worstGap.cpuMs, name(a_state.worstGap.after, "the frame's start"), name(a_state.worstGap.before, "the frame's end"));
			for (std::size_t g = 0; g < std::min<std::size_t>(gaps.size(), 6); ++g)
				gapText += fmt::format("\n    {:>6.3f}  {} -> {}", gaps[g].first / frames, name(gaps[g].second.first, "start"), name(gaps[g].second.second, "end"));
			logger::info("{}", gapText);
			ResetTotals(a_state);
		}
	}

	bool Requested()
	{
		static const bool requested = ReadInterval() != 0;
		return requested;
	}

	// CS_PASS_SKIP=<event>|<event>...: from Present 3000, each event's draws and dispatches are dropped for 750 frames in turn, with 750 unchanged frames between.
	namespace PassSkip
	{
		std::vector<std::string> names;  // each one phase: events joined by '+' drop together
		std::vector<std::string> active;
		std::string currentName;
		std::uint32_t rounds = 2;
		bool activeSet = false, read = false;
		DWORD thread = 0;
		std::uint32_t depth = 0, dropAt = 0;
		std::uint64_t presents = 0, step = 0;

		std::string_view Trim(std::string_view a_name)
		{
			while (!a_name.empty() && a_name.back() == ' ')
				a_name.remove_suffix(1);
			return a_name;
		}

		void Read()
		{
			read = true;
			char roundsValue[16]{};
			if (GetEnvironmentVariableA("CS_PASS_ROUNDS", roundsValue, sizeof(roundsValue)))
				rounds = static_cast<std::uint32_t>(std::strtoul(roundsValue, nullptr, 10));
			char value[8192]{};
			const DWORD length = GetEnvironmentVariableA("CS_PASS_SKIP", value, sizeof(value));
			std::string text(value, length < sizeof(value) ? length : 0);
			for (std::size_t at = 0; at < text.size();) {
				const auto end = std::min(text.find('|', at), text.size());
				names.push_back(text.substr(at, end - at));
				at = end + 1;
			}
		}

		// CS_GPU_LOAD=<groups>: a fixed compute load before each Present, so that dropped passes do not run into the frame's floor.
		// The phase "~load" turns it off, which measures it on this path.
		std::uint32_t loadGroups = 0;
		bool loadReady = false;
		winrt::com_ptr<ID3D11ComputeShader> loadShader;
		winrt::com_ptr<ID3D11UnorderedAccessView> loadUav;

		void SetupLoad()
		{
			loadReady = true;
			char value[32]{};
			if (GetEnvironmentVariableA("CS_GPU_LOAD", value, sizeof(value)))
				loadGroups = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
			if (!loadGroups)
				return;
			using Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
			const auto compile = reinterpret_cast<Compile>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
			static const char source[] = R"(
RWTexture2D<float> Out : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	float v = id.x * 0.001 + id.y * 0.37;
	[loop] for (int i = 0; i < 1024; ++i)
		v = frac(sin(v * 12.9898 + i) * 43758.5453);
	Out[uint2(id.x % 256, id.y % 256)] = v;
}
)";
			winrt::com_ptr<ID3DBlob> blob, errors;
			if (!compile || FAILED(compile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, blob.put(), errors.put()))) {
				logger::error("[PassSkip] the load shader did not compile");
				loadGroups = 0;
				return;
			}
			auto* device = globals::d3d::device;
			device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, loadShader.put());
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = 256;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			winrt::com_ptr<ID3D11Texture2D> texture;
			device->CreateTexture2D(&desc, nullptr, texture.put());
			device->CreateUnorderedAccessView(texture.get(), nullptr, loadUav.put());
			logger::info("[PassSkip] GPU load: {} groups", loadGroups);
		}

		void RunLoad()
		{
			if (!loadReady)
				SetupLoad();
			if (!loadGroups || !loadShader || !loadUav || std::ranges::find(active, std::string_view("~load")) != active.end())
				return;
			auto* context = globals::d3d::context;
			winrt::com_ptr<ID3D11ComputeShader> shader;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
			context->CSGetShader(shader.put(), nullptr, nullptr);
			context->CSGetUnorderedAccessViews(0, 1, uav.put());
			context->CSSetShader(loadShader.get(), nullptr, 0);
			auto* loadView = loadUav.get();
			context->CSSetUnorderedAccessViews(0, 1, &loadView, nullptr);
			// Past the drops' hooks: the thunks only drop inside an event, and no event is open here.
			context->Dispatch(loadGroups, 4, 1);
			auto* restore = uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &restore, nullptr);
			context->CSSetShader(shader.get(), nullptr, 0);
		}

		// Each phase's last 120 frames: the wall time, this thread's CPU time, the time in Present, and the whole process's CPU time.
		using Clock = std::chrono::steady_clock;
		Clock::time_point lastPresent{}, presentBegin{};
		std::uint64_t lastThreadCpu = 0, lastProcessCpu = 0;
		double wallMs = 0.0, threadMs = 0.0, processMs = 0.0, presentMs = 0.0;
		std::uint32_t phaseFrames = 0, statFrames = 0;

		std::uint64_t Filetime(const FILETIME& a_time)
		{
			return (static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime;
		}

		void Measure()
		{
			const auto now = Clock::now();
			FILETIME create, exit, kernel, user;
			GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user);
			const auto threadCpu = Filetime(kernel) + Filetime(user);
			GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
			const auto processCpu = Filetime(kernel) + Filetime(user);
			if (++phaseFrames > 60 && lastPresent != Clock::time_point{}) {
				wallMs += std::chrono::duration<double, std::milli>(now - lastPresent).count();
				threadMs += static_cast<double>(threadCpu - lastThreadCpu) / 10000.0;
				processMs += static_cast<double>(processCpu - lastProcessCpu) / 10000.0;
				++statFrames;
			}
			lastPresent = now;
			lastThreadCpu = threadCpu;
			lastProcessCpu = processCpu;
			presentBegin = now;
		}

		void LogPhase(const std::string& a_name)
		{
			if (statFrames)
				logger::info("[PassSkip] stats wall {:.3f} thread {:.3f} present {:.3f} process {:.3f} frames {} name {}", wallMs / statFrames, threadMs / statFrames, presentMs / statFrames, processMs / statFrames, statFrames, a_name);
			wallMs = threadMs = processMs = presentMs = 0.0;
			phaseFrames = statFrames = 0;
		}

		// CS_PASS_SAMPLE=1: samples the main thread's stack by wall clock during the "Player View" phases: where its time goes, on or off the CPU.
		std::atomic<bool> sampling{ false };
		std::atomic<bool> samplerStarted{ false };

		struct Sampled
		{
			std::uint64_t frames[32];
			std::uint32_t count;
		};

		std::string ModuleOf(std::uint64_t a_address, std::uint64_t& a_base)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a_address), &module) || !module) {
				a_base = 0;
				return "?";
			}
			a_base = reinterpret_cast<std::uint64_t>(module);
			char path[MAX_PATH]{};
			GetModuleFileNameA(module, path, sizeof(path));
			std::string name(path);
			const auto slash = name.find_last_of("\\/");
			return slash == std::string::npos ? name : name.substr(slash + 1);
		}

		std::map<DWORD, std::uint64_t> SnapshotThreads()
		{
			std::map<DWORD, std::uint64_t> out;
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);
			for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
				if (entry.th32OwnerProcessID != GetCurrentProcessId())
					continue;
				HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
				if (!handle)
					continue;
				FILETIME create, exit, kernel, user;
				if (GetThreadTimes(handle, &create, &exit, &kernel, &user))
					out[entry.th32ThreadID] = Filetime(kernel) + Filetime(user);
				CloseHandle(handle);
			}
			CloseHandle(snapshot);
			return out;
		}

		std::string StartOf(DWORD a_thread)
		{
			using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
			static const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
			HANDLE handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, a_thread);
			std::uint64_t start = 0;
			if (handle && query)
				query(handle, 9, &start, sizeof(start), nullptr);
			if (handle)
				CloseHandle(handle);
			std::uint64_t moduleBase = 0;
			const auto name = ModuleOf(start, moduleBase);
			return fmt::format("{}+{:x}", name, start - moduleBase);
		}

		void SamplerThread(DWORD a_thread)
		{
			timeBeginPeriod(1);
			HANDLE target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_thread);
			std::vector<Sampled> samples;
			samples.reserve(200000);
			std::uint64_t lastLog = 0;
			std::map<DWORD, std::uint64_t> threadStart, threadTotal;
			Clock::time_point edge{};
			double sampledMs = 0.0;
			bool was = false;
			auto report = [&] {
				std::vector<std::pair<std::uint64_t, DWORD>> busiest;
				for (const auto& [id, total] : threadTotal)
					busiest.emplace_back(total, id);
				std::ranges::sort(busiest, std::greater{});
				for (std::size_t i = 0; i < std::min<std::size_t>(14, busiest.size()); ++i)
					logger::info("[PassSkip] thread {:5} {:6.1f}% of wall{} start {}", busiest[i].second, static_cast<double>(busiest[i].first) / 10000.0 / sampledMs * 100.0, busiest[i].second == a_thread ? " MAIN" : "", StartOf(busiest[i].second));
				std::map<std::string, std::uint64_t> leaf, first, chain;
				for (const auto& sample : samples) {
					std::uint64_t moduleBase = 0;
					std::string leafName = sample.count ? ModuleOf(sample.frames[0], moduleBase) : "?";
					++leaf[leafName];
					std::string firstName = "?";
					std::string text;
					int shown = 0;
					for (std::uint32_t f = 0; f < sample.count && shown < 9; ++f) {
						std::uint64_t mb = 0;
						const auto name = ModuleOf(sample.frames[f], mb);
						if (name == "ntdll.dll" || name == "KERNELBASE.dll" || name == "KERNEL32.DLL" || name == "win32u.dll" || name == "kernel32.dll")
							continue;
						if (!shown)
							firstName = fmt::format("{}+{:x}", name, sample.frames[f] - mb);
						text += fmt::format(" <- {}+{:x}", name, sample.frames[f] - mb);
						++shown;
					}
					++first[leafName == "ntdll.dll" || leafName == "win32u.dll" ? "wait/system in " + firstName : "run in " + firstName];
					++chain[text];
				}
				logger::info("[PassSkip] samples {}", samples.size());
				auto top = [&](const char* a_title, const std::map<std::string, std::uint64_t>& a_map, std::size_t a_count) {
					std::vector<std::pair<std::uint64_t, std::string>> rows;
					for (const auto& [name, count] : a_map)
						rows.emplace_back(count, name);
					std::ranges::sort(rows, std::greater{});
					for (std::size_t i = 0; i < std::min(a_count, rows.size()); ++i)
						logger::info("[PassSkip] {} {:6.2f}% {}", a_title, 100.0 * rows[i].first / samples.size(), rows[i].second);
				};
				top("leaf", leaf, 12);
				top("first", first, 25);
				top("chain", chain, 25);
			};
			for (;;) {
				Sleep(1);
				const bool now = sampling.load(std::memory_order_relaxed);
				if (now && !was) {
					threadStart = SnapshotThreads();
					edge = Clock::now();
				} else if (!now && was) {
					for (const auto& [id, cpu] : SnapshotThreads())
						if (const auto found = threadStart.find(id); found != threadStart.end())
							threadTotal[id] += cpu - found->second;
					sampledMs += std::chrono::duration<double, std::milli>(Clock::now() - edge).count();
				}
				was = now;
				if (!now) {
					// A phase ended: its totals so far, once a second's worth has come in.
					if (samples.size() >= lastLog + 1000) {
						lastLog = samples.size();
						report();
					}
					continue;
				}
				Sampled sample{};
				if (SuspendThread(target) == static_cast<DWORD>(-1))
					continue;
				CONTEXT context{};
				context.ContextFlags = CONTEXT_FULL;
				if (GetThreadContext(target, &context)) {
					// No heap and no locks of ours while the thread is stopped: it may hold them.
					for (; sample.count < 32 && context.Rip; ++sample.count) {
						sample.frames[sample.count] = context.Rip;
						DWORD64 imageBase = 0;
						auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
						if (!function) {
							context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
							context.Rsp += 8;
							continue;
						}
						PVOID handler = nullptr;
						DWORD64 frame = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handler, &frame, nullptr);
					}
				}
				ResumeThread(target);
				samples.push_back(sample);
			}
		}

		void StartSampler()
		{
			if (samplerStarted.exchange(true))
				return;
			char value[8]{};
			if (!GetEnvironmentVariableA("CS_PASS_SAMPLE", value, sizeof(value)))
				return;
			std::thread(SamplerThread, GetCurrentThreadId()).detach();
		}

		void Begin(std::string_view a_name)
		{
			if (!thread || GetCurrentThreadId() != thread)
				return;
			if (!dropAt && activeSet && std::ranges::find(active, Trim(a_name)) != active.end())
				dropAt = depth + 1;
			++depth;
		}

		void End()
		{
			if (!thread || GetCurrentThreadId() != thread)
				return;
			if (depth)
				--depth;
			if (dropAt && depth < dropAt)
				dropAt = 0;
		}

		void Present()
		{
			if (!read)
				Read();
			if (names.empty())
				return;
			thread = GetCurrentThreadId();
			depth = 0;
			dropAt = 0;
			++presents;
			// A run this long must not reach the idle camera: it changes what is drawn.
			if (auto* camera = RE::PlayerCamera::GetSingleton())
				camera->GetRuntimeData2().allowAutoVanityMode = false;
			if (presents == 2500) {
				auto* disabled = RE::GetINISetting("bDisableAutoVanityMode:Camera");
				auto* delay = RE::GetINISetting("fAutoVanityModeDelay:Camera");
				logger::info("[PassSkip] bDisableAutoVanityMode {} fAutoVanityModeDelay {}", disabled ? (disabled->GetBool() ? "1" : "0") : "missing",
					delay ? std::to_string(delay->GetFloat()) : std::string("missing"));
			}
			Measure();
			RunLoad();
			StartSampler();
			sampling.store(activeSet && currentName == "Player View" && phaseFrames > 60, std::memory_order_relaxed);
			if (presents < 1500 || (presents - 1500) % 180)
				return;
			LogPhase(activeSet ? currentName : std::string("none"));
			if (step >= names.size() * 2 * rounds) {
				logger::info("[PassSkip] done");
				sampling.store(false);
				Sleep(250);
				spdlog::default_logger()->flush();
				TerminateProcess(GetCurrentProcess(), 0);
			}
			const auto phase = step++;
			activeSet = phase % 2 == 1;
			active.clear();
			const std::string name = activeSet ? names[(phase / 2) % names.size()] : std::string("none");
			if (activeSet) {
				for (std::size_t at = 0; at < name.size();) {
					const auto end = std::min(name.find('+', at), name.size());
					active.push_back(name.substr(at, end - at));
					at = end + 1;
				}
			}
			currentName = name;
		}
	}

	void PassSkipAfterPresent()
	{
		if (PassSkip::names.empty() || PassSkip::phaseFrames <= 60)
			return;
		PassSkip::presentMs += std::chrono::duration<double, std::milli>(PassSkip::Clock::now() - PassSkip::presentBegin).count();
	}

	bool PassSkipRequested()
	{
		char value[8192]{};
		return GetEnvironmentVariableA("CS_PASS_SKIP", value, sizeof(value)) != 0;
	}

	bool PassSkipDropping()
	{
		return PassSkip::dropAt && GetCurrentThreadId() == PassSkip::thread;
	}

	void BeginEvent(std::string_view a_name)
	{
		PassSkip::Begin(a_name);
		if (!Requested())
			return;
		auto& state = Get();
		auto& frame = state.frames[state.write];
		if (!frame.open || GetCurrentThreadId() != state.thread) {
			if (GetCurrentThreadId() == state.thread || !state.initialized)
				state.stack.push_back(kNone);
			return;
		}
		const std::uint32_t parent = state.stack.empty() ? kNone : state.stack.back();
		const std::uint32_t parentPath = parent == kNone ? kNone : frame.events[parent].path;
		// An event under one begun outside the frame has no place in the tree: it is not timed.
		if (!state.stack.empty() && parent == kNone) {
			state.stack.push_back(kNone);
			return;
		}
		const auto index = static_cast<std::uint32_t>(frame.events.size());
		const std::uint32_t path = PathOf(state, parentPath, a_name);
		frame.events.push_back({ path, parent, Timestamp(frame) });
		state.stack.push_back(index);
	}

	void EndEvent()
	{
		PassSkip::End();
		if (!Requested())
			return;
		auto& state = Get();
		if (state.initialized && GetCurrentThreadId() != state.thread)
			return;
		if (state.stack.empty())
			return;
		const std::uint32_t index = state.stack.back();
		state.stack.pop_back();
		auto& frame = state.frames[state.write];
		if (index != kNone && frame.open && index < frame.events.size() && frame.events[index].begin != kNone) {
			frame.events[index].end = Timestamp(frame);
		}
	}

	void OnPresent()
	{
		PassSkip::Present();
		if (!Requested())
			return;
		auto& state = Get();
		if (state.failed)
			return;
		if (!state.initialized) {
			state.interval = ReadInterval();
			if (!Initialize(state)) {
				state.failed = true;
				return;
			}
		}
		state.thread = GetCurrentThreadId();
		auto* context = globals::d3d::context;

		// Close the recording frame. An event still open at Present is not timed, and its end is ignored.
		auto& current = state.frames[state.write];
		const auto now = std::chrono::steady_clock::now();
		if (current.open) {
			current.cpuMs = std::chrono::duration<double, std::milli>(now - current.cpuBegin).count();
			for (auto& open : state.stack)
				open = kNone;
			current.frameEnd = Timestamp(current);
			context->End(current.disjoint.get());
			current.open = false;
			current.pending = true;
		}

		// Read back every finished frame, oldest first. The oldest is the next to be reused, so its read may flush: without
		// it, a frame whose query ends DXVK has not submitted yet (nothing else flushed the context) is never readable, and
		// with DCLF off every frame was dropped unread.
		for (std::uint32_t i = 1; i <= kFrames; ++i) {
			auto& frame = state.frames[(state.write + i) % kFrames];
			if (frame.pending && !Collect(state, frame, i == 1))
				break;
		}
		if (state.window == 0 && state.collected >= state.interval)
			Log(state);

		// Open the next frame in the ring; one the GPU has still not finished is given up.
		state.write = (state.write + 1) % kFrames;
		auto& next = state.frames[state.write];
		if (next.pending) {
			next.pending = false;
			++state.dropped;
		}
		next.used = 0;
		next.events.clear();
		next.overflow = false;
		next.frameEnd = kNone;
		next.window = state.window;
		next.cpuBegin = now;
		context->Begin(next.disjoint.get());
		next.open = true;
		next.frameBegin = Timestamp(next);
	}

	void StartWindow(std::string_view a_label)
	{
		if (!Requested())
			return;
		auto& state = Get();
		state.labels.emplace_back(a_label);
		state.window = static_cast<std::uint32_t>(state.labels.size() - 1);
	}
}
