#include "Features/DrawcallLimitFix/Runtime/SceneScheduler.h"

#include "Features/DrawcallLimitFix/Runtime/Switches.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineReadWindow.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <Windows.h>

#include <thread>

namespace DCLF
{
	unsigned SceneScheduler::PreparationWorkers()
	{
		static const unsigned workers = [] {
			const auto value = SwitchValue(Switch::Workers);
			if (const unsigned asked = value.empty() ? 0u : static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 10)))
				return asked;
			const unsigned hardware = std::thread::hardware_concurrency();
			return hardware > 3 ? hardware - 2 : 1u;
		}();
		return workers;
	}

	SceneExecutor& SceneScheduler::Executor()
	{
		// Never destroyed: joining threads in a DLL's static destruction (under the loader lock, after ExitProcess has ended
		// them) is not something to rely on.
		static auto* executor = new SceneExecutor(PreparationWorkers(), [](org::async::TaskClass a_class, unsigned a_index) {
			// T6b3e: one of DCLF's threads, for the release guard (CS_DCLF_RELEASE_GUARD): the scene lane's, or the others'.
			ReleaseGuard::MarkThread(a_class.domain == SceneExecutor::Scene.domain ? 1 : 2);
			if (a_class.domain == SceneExecutor::Scene.domain) {
				SetThreadDescription(GetCurrentThread(), L"CS DCLF scene");
				// The render thread waits for it only at Present; above the engine's job threads, as the coordinator.
				if (SwitchValue(Switch::AsyncPriority) != "normal")
					SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
				return;
			}
			if (a_class.domain == SceneExecutor::Coordinator.domain) {
				SetThreadDescription(GetCurrentThread(), L"CS DCLF coordinator");
				// The engine's job threads run at normal priority; the frame's jobs are joined by the render thread, so the
				// coordinator goes above them unless asked not to.
				if (SwitchValue(Switch::AsyncPriority) != "normal")
					SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
				return;
			}
			SetThreadDescription(GetCurrentThread(), (L"CS DCLF worker " + std::to_wstring(a_index)).c_str());
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		});
		return *executor;
	}

	const org::async::Scope& SceneScheduler::SceneLaneScope()
	{
		static const org::async::Scope scope = Executor().CreateScope("DCLF scene");
		return scope;
	}

	Runtime::SceneGraph& SceneScheduler::Graph()
	{
		// Never destroyed, as the executor it runs on; the graph holds it without owning it.
		static auto* graph = new Runtime::SceneGraph(std::shared_ptr<org::async::GraphScheduler>(&Executor(), [](org::async::GraphScheduler*) {}));
		return *graph;
	}

	org::async::SerializedTaskPump* SceneScheduler::MakeLanePump(SceneExecutor& a_executor, const org::async::Scope& a_scope, org::async::TaskClass a_class,
		org::async::TaskDispatch a_dispatch, std::string_view a_name, std::function<void()> a_drain, std::function<void()> a_refused)
	{
		auto* pump = new org::async::SerializedTaskPump;
		pump->Configure(
			[&a_executor, &a_scope, a_class, a_dispatch, a_name](org::async::SerializedTaskPump::Task a_task) {
				return a_executor.Dispatch(a_scope, a_class, a_dispatch, a_name, [task = std::move(a_task)](const org::async::TaskContext&) { task(); });
			},
			std::move(a_drain), std::move(a_refused));
		return pump;
	}

	const org::async::Scope& SceneScheduler::Scope()
	{
		static const org::async::Scope scope = Executor().CreateScope("DCLF");
		return scope;
	}
}
