#include "Features/RemoteControl/Gpa.h"

#include "Features/RemoteControl/GpuCountersHost.h"

#include <format>
#include <optional>

/*
 * GPUPerfAPI's D3D11 backend: the game's immediate context on the system D3D11 runtime (AMD GPUs).
 *
 * Each frame is one pass of the sample: a GPA session counting one group of the capture's counters (gpa::Schedule), begun
 * after Present and ended before the next. The selected perf events' ranges are its samples; GPA's D3D11 samples do not
 * nest, so a selected range inside another one is not measured (one nesting level, as on the Vulkan backends). Once the
 * GPU finished a pass its samples are read, and the passes of a sample are merged by range name and occurrence.
 */
namespace GpuCounters::detail
{
	namespace
	{
		constexpr std::uint32_t kFramesWithoutRangeLimit = 8;
		constexpr std::uint32_t kResultFrameLimit = 240;

		Step Finished(std::string a_error)
		{
			return Step{ .finished = true, .error = std::move(a_error) };
		}

		class GpaD3D11Backend final : public Backend
		{
		public:
			const char* Name() const noexcept override { return "D3D11"; }
			const char* Profiler() const noexcept override { return "GPUPerfAPI"; }
			std::vector<MetricRequest> DefaultMetrics() const override { return gpa::DefaultMetrics(); }

			bool Probe(DeviceInfo& a_device) override
			{
				if (!Host::Device()) {
					a_device.error = "the D3D11 device is not created yet";
					return false;
				}
				a_device.probed = true;
				const auto* table = gpa::Table();
				if (!table) {
					a_device.error = gpa::Unavailable();
					return false;
				}
				a_device.available = true;
				// Clocks stay as the driver runs them: the counters describe the frame as it is played.
				if (!gpa::Check(table->GpaOpenContext(Host::Device(), kGpaOpenContextClockModeNoneBit, &context), "GpaOpenContext")) {
					a_device.error = "GPUPerfAPI could not open a context on the D3D11 device";
					return false;
				}
				GpaContextSampleTypeFlags types = 0;
				(void)table->GpaGetSupportedSampleTypes(context, &types);
				gpa::Describe(context, a_device);
				a_device.gpuSupported = (types & kGpaContextSampleTypeDiscreteCounter) != 0;
				if (!a_device.gpuSupported)
					a_device.error = "GPUPerfAPI has no discrete counters on this GPU";
				logger::info("[GPA] D3D11 context on '{}' ({}): discrete counters {}", a_device.deviceName, a_device.chipName, a_device.gpuSupported);
				return a_device.gpuSupported;
			}

			std::string Validate(const CaptureConfiguration& a_configuration) const override
			{
				if (a_configuration.nestingLevels != 1)
					return "GPUPerfAPI measures one nesting level; set nestingLevels to 1";
				return {};
			}

			bool Begin(Capture& a_capture, const std::string&, std::string& a_error) override
			{
				if (a_error = Validate(a_capture.configuration); !a_error.empty())
					return false;
				if (!gpa::BuildSchedule(context, a_capture.configuration.metrics, schedule, a_error))
					return false;
				a_capture.result.metrics = schedule.metrics;
				a_capture.result.unsupportedMetrics = schedule.unsupported;
				a_capture.passesPerSample = schedule.groups.size();
				merger.Reset(schedule.metrics.size());
				pass = 0;
				logger::info("[GPA] capture {}: {} metrics in {} single-pass groups ({} unsupported)", a_capture.id, schedule.metrics.size(), schedule.groups.size(),
					schedule.unsupported.size());
				if (!schedule.unsupported.empty()) {
					std::string list;
					for (const auto& metric : schedule.unsupported)
						list += std::format("{}{}", list.empty() ? "" : ", ", metric.name);
					logger::info("[GPA]   not measured (no such counter, or more than one pass alone): {}", list);
				}
				return true;
			}

			void PresentBegin(Capture* a_capture) override
			{
				if (a_capture && session && !ended)
					EndPass(*a_capture);
			}

			// After Present: the finished pass's samples once the GPU completed them, then the next pass.
			Step Advance(Capture& a_capture) override
			{
				const auto* table = gpa::Table();
				if (session && ended) {
					const GpaStatus status = table->GpaIsSessionComplete(session);
					if (status == kGpaStatusResultNotReady) {
						if (++waitFrames > kResultFrameLimit)
							return Finished("the GPU did not finish a GPUPerfAPI pass");
						return Step{ .state = CaptureState::Decoding };
					}
					std::string error;
					if (!gpa::Check(status, "GpaIsSessionComplete") || !merger.AddPass(session, schedule, pass, samples, error)) {
						gpa::DeleteSession(std::exchange(session, nullptr), false);
						return Finished(error.empty() ? "could not read a GPUPerfAPI pass" : error);
					}
					gpa::DeleteSession(std::exchange(session, nullptr), false);
					framesWithoutRange = samples.empty() ? framesWithoutRange + 1 : 0;
					samples.clear();
					if (framesWithoutRange >= kFramesWithoutRangeLimit)
						return Finished(std::format("no selected range was recorded within {} frames", kFramesWithoutRangeLimit));
					if (++pass >= schedule.groups.size()) {
						merger.Emit(a_capture.sample, a_capture.result);
						pass = 0;
						if (++a_capture.sample >= a_capture.configuration.samples)
							return Finished({});
					}
				}
				if (!session) {
					std::string error;
					session = gpa::BeginPassSession(context, schedule, pass, error);
					if (!session)
						return Finished(error);
					// GPA refuses an output id that still names a command list (it checks it before writing it).
					commandList = nullptr;
					if (!gpa::Check(table->GpaBeginCommandList(session, 0, GPA_NULL_COMMAND_LIST, kGpaCommandListNone, &commandList), "GpaBeginCommandList")) {
						gpa::DeleteSession(std::exchange(session, nullptr), true);
						return Finished("could not begin GPUPerfAPI's command list");
					}
					ended = false;
					waitFrames = 0;
					nextSample = 0;
					a_capture.passesSubmitted = pass;
					SetRangesActive(true);
				}
				return Step{ .state = CaptureState::Capturing };
			}

			void End(Capture&) override
			{
				SetRangesActive(false);
				if (session) {
					if (!ended) {
						CloseOpen();
						(void)gpa::Check(gpa::Table()->GpaEndCommandList(commandList), "GpaEndCommandList");
					}
					gpa::DeleteSession(std::exchange(session, nullptr), !ended);
				}
				samples.clear();
				stack.clear();
			}

			void PushRange(Capture& a_capture, std::string_view a_name) override
			{
				bool pushed = false;
				if (session && !ended && !open && PerfEventSelected(a_capture.configuration.ranges, a_name)) {
					const GpaUInt32 id = nextSample++;
					if (gpa::Table()->GpaBeginSample(id, commandList) == kGpaStatusOk) {
						samples.emplace_back(id, std::string(a_name));
						pushed = open = true;
					}
				}
				stack.push_back(pushed);
			}

			void PopRange() override
			{
				if (stack.empty())
					return;
				const bool pushed = stack.back();
				stack.pop_back();
				if (pushed && open) {
					(void)gpa::Check(gpa::Table()->GpaEndSample(commandList), "GpaEndSample");
					open = false;
				}
			}

		private:
			void CloseOpen()
			{
				if (open) {
					(void)gpa::Check(gpa::Table()->GpaEndSample(commandList), "GpaEndSample");
					open = false;
				}
				for (auto&& pushed : stack)
					pushed = false;
			}

			// Before Present: the pass's samples close (a perf event still open is closed here), its session ends.
			void EndPass(Capture& a_capture)
			{
				SetRangesActive(false);
				CloseOpen();
				const auto* table = gpa::Table();
				(void)gpa::Check(table->GpaEndCommandList(commandList), "GpaEndCommandList");
				(void)gpa::Check(table->GpaEndSession(session), "GpaEndSession");
				ended = true;
				a_capture.passesSubmitted = pass + 1;
			}

			GpaContextId context = nullptr;
			gpa::Schedule schedule;
			gpa::Merger merger;
			std::size_t pass = 0;  // the group the current session counts
			GpaSessionId session = nullptr;
			GpaCommandListId commandList = nullptr;
			bool ended = false;  // the session ended; its results are awaited
			bool open = false;   // a sample is open (they do not nest)
			GpaUInt32 nextSample = 0;
			std::vector<std::pair<GpaUInt32, std::string>> samples;  // the pass's, in order
			std::vector<bool> stack;                                 // per open perf event: whether it began a sample
			std::uint32_t waitFrames = 0;
			std::uint32_t framesWithoutRange = 0;
		};
	}

	std::unique_ptr<Backend> MakeGpaD3D11Backend()
	{
		return std::make_unique<GpaD3D11Backend>();
	}
}
