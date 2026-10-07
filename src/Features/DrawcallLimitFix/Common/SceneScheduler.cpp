#include "SceneScheduler.h"

#include "Switches.h"

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

	PublishedSceneExecutor& SceneScheduler::Executor()
	{
		// Never destroyed: joining threads in a DLL's static destruction (under the loader lock, after ExitProcess has ended
		// them) is not something to rely on.
		static auto* executor = new PublishedSceneExecutor(PreparationWorkers(), [](org::async::TaskClass a_class, unsigned a_index) {
			if (a_class.domain == PublishedSceneExecutor::Coordinator.domain) {
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

	PublishedSceneExecutor& SceneScheduler::SceneLane()
	{
		// Never destroyed, as Executor(). The executor takes at least one preparation worker; the lane's is idle (the walk's parallel
		// loops use Executor()'s).
		static auto* lane = new PublishedSceneExecutor(1, [](org::async::TaskClass a_class, unsigned) {
			if (a_class.domain != PublishedSceneExecutor::Coordinator.domain) {
				SetThreadDescription(GetCurrentThread(), L"CS DCLF scene (idle)");
				return;
			}
			SetThreadDescription(GetCurrentThread(), L"CS DCLF scene");
			// The render thread waits for it only at Present; above the engine's job threads, as the coordinator.
			if (SwitchValue(Switch::AsyncPriority) != "normal")
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		});
		return *lane;
	}

	const org::async::Scope& SceneScheduler::SceneLaneScope()
	{
		static const org::async::Scope scope = SceneLane().CreateScope("DCLF scene");
		return scope;
	}

	Published::SceneGraph& SceneScheduler::Graph()
	{
		// Never destroyed, as the executor it runs on; the graph holds it without owning it.
		static auto* graph = new Published::SceneGraph(std::shared_ptr<org::async::GraphScheduler>(&Executor(), [](org::async::GraphScheduler*) {}));
		return *graph;
	}

	const org::async::Scope& SceneScheduler::Scope()
	{
		static const org::async::Scope scope = Executor().CreateScope("DCLF");
		return scope;
	}
}
