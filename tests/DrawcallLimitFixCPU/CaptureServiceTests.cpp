#include "Features/DrawcallLimitFix/CaptureService.h"
#include "Features/DrawcallLimitFix/CaptureGraph.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace DCLF::Published;
using namespace std::chrono_literals;

auto Backend(CaptureService::Builder builder)
{
#ifdef DCLF_TEST_GRAPH_BACKEND
	return MakeGraphCaptureBackend(std::move(builder));
#else
	return builder;
#endif
}

template<class Predicate> void Until(Predicate test)
{
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (!test()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
}

int main()
{
	const auto caller = std::this_thread::get_id();
	std::promise<void> started, release;
	auto entered = started.get_future();
	auto gate = release.get_future().share();
	std::atomic_uint calls{ 0 };
	CaptureService service(Backend([&](const CaptureBuildRequest& request) {
		assert(std::this_thread::get_id() != caller);
		if (calls.fetch_add(1) == 0) { started.set_value(); gate.wait(); }
		return CaptureService::Output{ std::make_shared<const std::uint64_t>(request.scene->lifecycle), sizeof(std::uint64_t) };
	}), sizeof(std::uint64_t), 1024 * 1024, 2);
	assert(service.TryPost({ SceneEventKind::Reset, 1, 1, {}, {} }));
	assert(entered.wait_for(5s) == std::future_status::ready);
	auto group = ConsistencyGroupUpdate::Capture({ 1, 1 }, 2, 1, {}, {}, {});
	assert(service.TryPost({ SceneEventKind::Reset, 2, 2, {}, {} }));
	assert(service.TryPost({ SceneEventKind::ReplaceGroup, 3, 2, { 1, 1 }, group }));
	std::shared_ptr<const PreparedCapture> ready;
	assert(!service.TryTakeReady(ready));
	release.set_value();
	Until([&] { assert(!service.Faulted()); return service.TryTakeReady(ready); });
	assert(ready->request.scene->lifecycle == 2);
	assert(ready->request.scene->groups.at(1) == group);
	assert(!service.TryAcknowledge(ready->request.ticket + 1, true));
	assert(service.TryAcknowledge(ready->request.ticket, true));
	assert(!service.TryAcknowledge(ready->request.ticket, true));
	// An externally held CPU lease survives reset/shutdown. Production callers
	// must release such references on cleanup lanes, not the render thread.
	assert(service.TryPost({ SceneEventKind::Reset, 4, 3, {}, {} }));
	std::shared_ptr<const PreparedCapture> newer;
	Until([&] { assert(!service.Faulted()); return service.TryTakeReady(newer); });
	assert(newer->request.scene->lifecycle == 3 && newer->request.scene->groups.empty());
	assert(service.TryAcknowledge(newer->request.ticket, true));
	service.Shutdown();
	service.Shutdown();
	assert(!service.TryPost({ SceneEventKind::Reset, 5, 4, {}, {} }));
	assert(ready->request.scene->groups.at(1) == group);

	std::atomic_uint attempts{ 0 };
	CaptureService failure(Backend([&](const auto&) -> CaptureService::Output {
		if (attempts.fetch_add(1) == 0) throw std::runtime_error("synthetic preparation failure");
		return { std::make_shared<const int>(9), sizeof(int) };
	}), sizeof(int));
	assert(failure.TryPost({ SceneEventKind::Reset, 1, 1, {}, {} }));
	std::shared_ptr<const PreparedCapture> recovered;
	Until([&] {
		assert(!failure.Faulted());
		failure.Retry();
		return failure.TryTakeReady(recovered);
	});
	assert(attempts.load() == 2);
	assert(failure.TryAcknowledge(recovered->request.ticket, true));
	failure.Shutdown();

	CaptureService invalid(Backend([](const auto&) { return CaptureService::Output{}; }), 0);
	assert(invalid.TryPost({ SceneEventKind::DetachGroup, 1, 1, { 1, 1 }, {} }));
	Until([&] { return invalid.Faulted(); });
	assert(!invalid.TryPost({ SceneEventKind::Reset, 2, 1, {}, {} }));
	invalid.Shutdown();
}
