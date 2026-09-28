#pragma once

#include "CapturePreparation.h"

#include <functional>
#include <memory>

namespace org::async { class GraphScheduler; }

namespace DCLF::Published
{
	// Offline asynchronous host. No engine pointers, native suppression or GPU
	// readiness are interpreted here. Builder runs only on preparation workers.
	class CaptureService
	{
	public:
		struct Output { std::shared_ptr<const void> artifact; std::size_t bytes = 0; };
		using Builder = std::function<Output(const CaptureBuildRequest&)>;
		using Completion = std::function<void(Output)>;
		struct Backend
		{
			std::function<bool(const CaptureBuildRequest&, Completion)> start;
			std::function<void()> shutdown;
		};
		using BackendFactory = std::function<Backend(std::shared_ptr<org::async::GraphScheduler>)>;
		CaptureService(Builder, std::size_t outputReservation,
			std::size_t byteBudget = 128u * 1024u * 1024u, std::size_t eventLimit = 4096);
		CaptureService(BackendFactory, std::size_t outputReservation,
			std::size_t byteBudget = 128u * 1024u * 1024u, std::size_t eventLimit = 4096);
		~CaptureService();
		CaptureService(const CaptureService&) = delete;
		CaptureService& operator=(const CaptureService&) = delete;
		// Short mailbox mutex only; never waits for preparation. False retains the
		// caller's event for retry. Caller must preserve source/lifecycle order.
		// Ingress has a separate byte/count bound, not a unified pipeline budget.
		bool TryPost(const CapturedSceneEvent&);
		// Single consumer. Destination must be empty; release leases on a cleanup
		// lane. This is not render-thread publication selection or a frame lease.
		// Delivered results may age across resets: consumers must validate their
		// lifecycle before use, then acknowledge the delivered ticket exactly once.
		bool TryTakeReady(std::shared_ptr<const PreparedCapture>& destination);
		bool TryAcknowledge(std::uint64_t ticket, bool selected);
		void Retry();
		bool Faulted() const;
		// Only teardown may block. Owner must outlive callbacks; not worker-callable.
		void Shutdown();
	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
