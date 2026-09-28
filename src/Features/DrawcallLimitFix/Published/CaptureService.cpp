#include "CaptureService.h"
#include "PublishedSceneExecutor.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace DCLF::Published
{
	struct CaptureService::Impl
	{
		struct Completion { std::uint64_t ticket, lifecycle; Output output; };
		struct Ack { std::uint64_t ticket; bool selected; };
		std::shared_ptr<PublishedSceneExecutor> executor = std::make_shared<PublishedSceneExecutor>();
		org::async::Scope scope = executor->CreateScope("DCLF capture service");
		org::async::SerializedTaskPump pump;
		CapturePreparation preparation;
		Builder builder;
		Backend backend;
		std::size_t reservation, ingressBudget, ingressBytes = 0;
		std::mutex mailbox;
		std::vector<CapturedSceneEvent> ingress;
		std::size_t head = 0, count = 0;
		std::optional<Completion> completion;
		std::optional<Ack> acknowledgement;
		std::shared_ptr<const PreparedCapture> outgoing;
		std::atomic_bool faulted{ false }, stopping{ false }, retryRequested{ false };
		std::uint64_t offered = 0;
		std::uint64_t delivered = 0; // mailbox mutex, single-consumer acknowledgement identity
		bool retryBlocked = false;

		Impl(Builder fn, BackendFactory factory, std::size_t outputReservation, std::size_t budget, std::size_t limit) :
			preparation(budget, limit), builder(std::move(fn)), reservation(outputReservation), ingressBudget(budget), ingress(limit)
		{
			if (factory) backend = factory(executor);
			if (!builder && (!backend.start || !backend.shutdown)) throw std::invalid_argument("capture service needs a builder/backend");
			pump.Configure([this](auto task) {
				return executor->Dispatch(scope, PublishedSceneExecutor::Coordinator, {}, "capture mailbox",
					[task = std::move(task)](const auto&) { task(); });
			}, [this] { Drain(); }, [this] { faulted.store(true); });
		}

		void Ingest()
		{
			for (;;) {
				CapturedSceneEvent event;
				{
					std::lock_guard lock(mailbox);
					if (!count) return;
					event = ingress[head];
				}
				const auto result = preparation.Post(event);
				if (result == CaptureAdmission::Result::Pressure) return;
				// Invalid accepted input is a visible service fault, not a silently
				// discarded lifecycle record. Integration must validate ordered input.
				if (result == CaptureAdmission::Result::Invalid) { faulted.store(true); return; }
				std::shared_ptr<const PreparedCapture> retired;
				{
					std::lock_guard lock(mailbox);
					if (outgoing && outgoing->request.scene->lifecycle != preparation.Lifecycle()) {
						retired = std::move(outgoing);
						offered = 0;
					}
					ingress[head] = {}; // event still owns its page outside the lock
					if (event.update) ingressBytes -= event.update->OwnedBytes();
					head = (head + 1) % ingress.size();
					--count;
				}
				retryBlocked = false;
			}
		}

		void Drain()
		{
			if (stopping.load() || faulted.load()) return;
			std::optional<Ack> ack;
			std::optional<Completion> done;
			{
				std::lock_guard lock(mailbox);
				ack.swap(acknowledgement);
				done.swap(completion);
			}
			if (ack && ack->ticket == offered) {
				preparation.Acknowledge(ack->ticket, ack->selected);
				offered = 0;
			}
			Ingest(); // process accepted resets before deciding readiness
			if (faulted.load()) return;
			if (done) {
				preparation.Complete(done->ticket, std::move(done->output.artifact), done->output.bytes);
				retryBlocked = !preparation.Ready() && done->lifecycle == preparation.Lifecycle();
				Ingest(); // completion may have released a pressure reservation
			}
			if (retryRequested.exchange(false)) retryBlocked = false;
			if (faulted.load()) return;
			if (auto ready = preparation.Ready(); ready && !offered) {
				std::lock_guard lock(mailbox);
				if (!outgoing) { offered = ready->request.ticket; outgoing = std::move(ready); }
			}
			if (retryBlocked) return;
			const auto request = preparation.Begin(reservation);
			if (!request) return;
			auto finished = [this, ticket = request->ticket, lifecycle = request->scene->lifecycle](Output output) {
					{
						std::lock_guard lock(mailbox);
						completion.emplace(Completion{ ticket, lifecycle, std::move(output) });
					}
					(void)pump.Notify();
				};
			const bool dispatched = backend.start ? backend.start(*request, std::move(finished)) :
				executor->Dispatch(scope, PublishedSceneExecutor::Preparation, {}, "capture preparation",
					[this, request = *request, finished = std::move(finished)](const auto&) {
						Output output;
						try { output = builder(request); } catch (...) { /* returned as failure */ }
						finished(std::move(output));
					});
			if (!dispatched) {
				preparation.Fail(request->ticket);
				retryBlocked = true;
			}
		}
	};

	CaptureService::CaptureService(Builder builder, std::size_t reservation, std::size_t budget, std::size_t limit) :
		impl(std::make_unique<Impl>(std::move(builder), BackendFactory{}, reservation, budget, limit)) {}
	CaptureService::CaptureService(BackendFactory factory, std::size_t reservation, std::size_t budget, std::size_t limit) :
		impl(std::make_unique<Impl>(Builder{}, std::move(factory), reservation, budget, limit)) {}
	CaptureService::~CaptureService() { Shutdown(); }

	bool CaptureService::TryPost(const CapturedSceneEvent& event)
	{
		const auto bytes = event.update ? event.update->OwnedBytes() : 0;
		{
			std::lock_guard lock(impl->mailbox);
			if (impl->stopping.load() || impl->faulted.load() || impl->count == impl->ingress.size()) return false;
			// Independently bounded ingress, with one isolated oversized page. Total
			// pipeline accounting still needs to include this mailbox's retained bytes.
			if (impl->count && (impl->ingressBytes > impl->ingressBudget || bytes > impl->ingressBudget - impl->ingressBytes)) return false;
			impl->ingress[(impl->head + impl->count) % impl->ingress.size()] = event;
			impl->ingressBytes += bytes;
			++impl->count;
		}
		// An accepted envelope remains owned even if scheduling subsequently faults.
		(void)impl->pump.Notify();
		return true;
	}
	bool CaptureService::TryTakeReady(std::shared_ptr<const PreparedCapture>& destination)
	{
		std::lock_guard lock(impl->mailbox);
		if (destination || impl->delivered || impl->stopping.load() || impl->faulted.load() || !impl->outgoing) return false;
		destination = std::move(impl->outgoing);
		impl->delivered = destination->request.ticket;
		return true;
	}
	bool CaptureService::TryAcknowledge(std::uint64_t ticket, bool selected)
	{
		{
			std::lock_guard lock(impl->mailbox);
			if (!ticket || ticket != impl->delivered || impl->stopping.load() || impl->faulted.load() || impl->acknowledgement) return false;
			impl->acknowledgement = Impl::Ack{ ticket, selected };
			impl->delivered = 0;
		}
		(void)impl->pump.Notify();
		return true;
	}
	void CaptureService::Retry() { impl->retryRequested.store(true); (void)impl->pump.Notify(); }
	bool CaptureService::Faulted() const { return impl->faulted.load(); }
	void CaptureService::Shutdown()
	{
		impl->stopping.store(true);
		impl->pump.Stop();
		// Quiesce the service's coordinator before shutting down its backend. Worker
		// completions can still post to the stopped pump; mailboxes outlive both drains.
		impl->scope->Wait();
		if (impl->backend.shutdown) impl->backend.shutdown();
		impl->executor->Shutdown();
	}
}
