#include "Features/DrawcallLimitFix/Published/CapturedScene.h"
#include "Features/DrawcallLimitFix/Published/CaptureAdmission.h"
#include "Features/DrawcallLimitFix/Published/CapturePreparation.h"
#include "Features/DrawcallLimitFix/Published/FaceCapture.h"
#include "Features/DrawcallLimitFix/Published/PublishedSceneExecutor.h"
#include "Features/DrawcallLimitFix/Published/SceneIdentity.h"

#include <algorithm>
#include <cassert>
#include <future>
#include <cstring>
#include <stdexcept>

using namespace DCLF::Published;

namespace
{
	void EngineIdentities()
	{
		DCLF::SceneIdentity identities;
		int actor = 0, otherActor = 0, body = 0, head = 0, prop = 0;
		const auto first = identities.Attach(&body, &actor);
		const auto sibling = identities.Attach(&head, &actor);
		assert(first.member && first.group && sibling.member != first.member && sibling.group == first.group);
		assert(identities.Attach(&body, &actor).member == first.member);
		identities.Detach(&body);
		const auto replacement = identities.Attach(&body, &actor);
		assert(replacement.member != first.member && replacement.group == sibling.group);
		const auto moved = identities.Attach(&body, &otherActor);
		assert(moved.replaced && moved.member != replacement.member && moved.group != replacement.group);
		identities.Detach(&head);
		const auto reincarnated = identities.Attach(&head, &actor);
		assert(reincarnated.group != first.group);
		const auto independent = identities.Attach(&prop, nullptr);
		assert(independent.group == independent.member && independent.group != reincarnated.group);
		identities.Reset();
		assert(identities.Attach(&body, &actor).member != moved.member);
		assert(identities.Attach(&head, &actor).group != first.group);
		identities.Detach(&prop); // stale detach after reset is harmless
	}

	template<class F> void Invalid(F&& call)
	{
		bool caught = false;
		try { call(); } catch (const std::invalid_argument&) { caught = true; }
		assert(caught);
	}

	std::shared_ptr<const ConsistencyGroupUpdate> Group(std::uint64_t incarnation, std::uint64_t source, std::uint64_t lifecycle = 1)
	{
		std::array members{ MemberInput{ { 10, incarnation } }, MemberInput{ { 11, incarnation } } };
		std::array bytes{ std::byte{ 7 } };
		std::array parts{
			ComponentInput{ members[0].identity, ComponentKind::Geometry, bytes },
			ComponentInput{ members[0].identity, ComponentKind::Material, bytes },
			ComponentInput{ members[1].identity, ComponentKind::Geometry, bytes },
			ComponentInput{ members[1].identity, ComponentKind::Material, bytes }
		};
		return ConsistencyGroupUpdate::Capture({ 1, incarnation }, lifecycle, source, members, parts, {});
	}

	void CaptureValues()
	{
		std::array members{ MemberInput{ { 10, 1 }, {}, true, true } };
		std::array bytes{ std::byte{ 7 }, std::byte{ 9 } };
		std::array parts{
			ComponentInput{ { 10, 1 }, ComponentKind::Geometry, bytes },
			ComponentInput{ { 10, 1 }, ComponentKind::Material, bytes },
			ComponentInput{ { 10, 1 }, ComponentKind::Pose, bytes },
			ComponentInput{ { 10, 1 }, ComponentKind::Face, bytes }
		};
		std::array order{ 0, 1, 2, 3 };
		do {
			std::vector<ComponentInput> reordered;
			for (int index : order) {
				if (!reordered.empty()) Invalid([&] { ConsistencyGroupUpdate::Capture({ 1, 1 }, 1, 1, members, reordered, {}); });
				reordered.push_back(parts[index]);
			}
			assert(ConsistencyGroupUpdate::Capture({ 1, 1 }, 1, 1, members, reordered, {})->Components().size() == 4);
		} while (std::next_permutation(order.begin(), order.end()));
		auto owner = std::make_shared<const int>(17);
		std::weak_ptr<const int> weak = owner;
		std::array resources{ ResourceLease{ { 20, 1 }, 3, owner } };
		auto page = ConsistencyGroupUpdate::Capture({ 1, 1 }, 1, 1, members, parts, resources);
		bytes[0] = std::byte{ 0 };
		members[0].world[0] = 42;
		owner.reset();
		resources[0].owner.reset();
		assert(page->Members()[0].world[0] == 0);
		assert(page->Components()[0].values[0] == std::byte{ 7 });
		assert(!weak.expired());
		page.reset();
		assert(weak.expired());
		Invalid([&] { ConsistencyGroupUpdate::Capture({ 1, 1 }, 1, 1, members, parts, resources); });
		parts[3] = parts[2];
		Invalid([&] { ConsistencyGroupUpdate::Capture({ 1, 1 }, 1, 1, members, parts, {}); });
		parts[3].member.incarnation = 2;
		Invalid([&] { ConsistencyGroupUpdate::Capture({ 1, 1 }, 1, 1, members, parts, {}); });
	}

	void FaceValues()
	{
		auto owner = std::make_shared<const std::vector<float>>(20, 3.5f);
		std::weak_ptr<const std::vector<float>> weak = owner;
		std::array views{
			DCLF::FaceSnapshots::ShapeView{ owner->data(), 2, 17, owner },
			DCLF::FaceSnapshots::ShapeView{ owner->data() + 8, 3, 17, owner }
		};
		auto captured = CaptureHeadFaceValues(views);
		assert(captured && captured->size() == 2);
		DCLF::FaceSnapshots::HeadView whole{ 5, 17, { { 0, 0, 2 }, { 1, 8, 3 } }, owner };
		assert(CaptureHeadFaceValues(whole) == captured);
		whole.shapes[1].offsetFloats = 9;
		assert(!CaptureHeadFaceValues(whole));
		whole.shapes[1].offsetFloats = 8;
		whole.shapes.pop_back();
		assert(!CaptureHeadFaceValues(whole));
		whole = {};
		FaceComponentHeader header;
		std::memcpy(&header, (*captured)[1].data(), sizeof(header));
		assert(header.generation == 17 && header.vertexCount == 3 && header.schema == 1);
		assert((*captured)[1].size() == sizeof(header) + 12 * sizeof(float));
		views[1].generation = 18;
		assert(!CaptureHeadFaceValues(views));
		views[1].generation = 17;
		views[1].vertexCount = 4;
		assert(!CaptureHeadFaceValues(views));
		views[1].vertexCount = 3;
		views[1].owner = std::make_shared<const std::vector<float>>(20, 1.f);
		assert(!CaptureHeadFaceValues(views));
		assert(!CaptureHeadFaceValues(std::span<const DCLF::FaceSnapshots::ShapeView>{}));
		views = {};
		owner.reset();
		assert(weak.expired());
		float position = 0;
		std::memcpy(&position, (*captured)[0].data() + sizeof(header), sizeof(position));
		assert(position == 3.5f); // independent of recyclable source storage
	}

	void Lifecycle()
	{
		CapturedSceneReducer reducer;
		auto first = Group(1, 1);
		auto result = reducer.Apply({ { { SceneEventKind::Reset, 1, 1, {}, {} },
			{ SceneEventKind::ReplaceGroup, 2, 1, { 1, 1 }, first } } });
		assert(result.applied == 2 && !result.rejected);
		auto retained = reducer.Snapshot();
		result = reducer.Apply({ { { SceneEventKind::ReplaceGroup, 3, 1, { 1, 1 }, Group(1, 2) } } });
		assert(result.applied == 1);
		assert(retained->groups.at(1) == first);
		assert(reducer.Snapshot()->groups.at(1)->SourceUpdate() == 2);
		result = reducer.Apply({ { { SceneEventKind::ReplaceGroup, 4, 1, { 1, 1 }, first },
			{ SceneEventKind::DetachGroup, 5, 1, { 1, 1 }, {} },
			{ SceneEventKind::ReplaceGroup, 6, 1, { 1, 1 }, Group(1, 3) },
			{ SceneEventKind::ReplaceGroup, 7, 1, { 1, 2 }, Group(2, 1) },
			{ SceneEventKind::DetachGroup, 8, 1, { 1, 1 }, {} } } });
		assert(result.applied == 2 && result.rejected == 3);
		assert(reducer.Snapshot()->groups.at(1)->Group().incarnation == 2);
		result = reducer.Apply({ { { SceneEventKind::Reset, 9, 2, {}, {} },
			{ SceneEventKind::ReplaceGroup, 10, 1, { 1, 2 }, Group(2, 2) },
			{ SceneEventKind::ReplaceGroup, 11, 2, { 1, 1 }, first },
			{ SceneEventKind::DetachGroup, 10, 2, { 1, 1 }, {} } } });
		assert(result.applied == 1 && result.rejected == 3);
		assert(reducer.Snapshot()->lifecycle == 2 && reducer.Snapshot()->groups.empty());
		result = reducer.Apply({ { { SceneEventKind::ReplaceGroup, 12, 2, { 1, 1 }, Group(1, 1, 2) } } });
		assert(result.applied == 1 && reducer.Snapshot()->groups.at(1)->Lifecycle() == 2);
		// Detach before an asynchronous first completion also installs a tombstone.
		result = reducer.Apply({ { { SceneEventKind::DetachGroup, 13, 2, { 2, 1 }, {} } } });
		assert(result.applied == 1);
		auto empty = ConsistencyGroupUpdate::Capture({ 2, 1 }, 2, 1, {}, {}, {});
		result = reducer.Apply({ { { SceneEventKind::ReplaceGroup, 14, 2, { 2, 1 }, empty } } });
		assert(result.rejected == 1);
	}

	void WorkerOwnership()
	{
		DCLF::PublishedSceneExecutor executor;
		auto scope = executor.CreateScope("capture reducer");
		std::promise<std::shared_ptr<const CapturedSceneState>> completed;
		auto result = completed.get_future();
		CapturedSceneUpdate capture{ { { SceneEventKind::Reset, 1, 1, {}, {} },
			{ SceneEventKind::ReplaceGroup, 2, 1, { 1, 1 }, Group(1, 1) } } };
		assert(executor.Dispatch(scope, DCLF::PublishedSceneExecutor::Coordinator, {}, "reduce",
			[capture = std::move(capture), &completed](const auto&) mutable {
				CapturedSceneReducer reducer;
				assert(reducer.Apply(std::move(capture)).applied == 2);
				completed.set_value(reducer.Snapshot());
			}));
		assert(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		auto state = result.get();
		scope->Wait();
		executor.Shutdown();
		assert(state->groups.at(1)->Members().size() == 2);
	}

	void Preparation()
	{
		using Result = CaptureAdmission::Result;
		CapturePreparation coordinator;
		assert(!coordinator.Begin(0));
		assert(coordinator.Post({ SceneEventKind::Reset, 1, 1, {}, {} }) == Result::Accepted);
		assert(coordinator.Post({ SceneEventKind::ReplaceGroup, 2, 1, { 1, 1 }, Group(1, 1) }) == Result::Accepted);
		auto first = coordinator.Begin(64);
		assert(first && first->scene->groups.at(1)->SourceUpdate() == 1);
		assert(!coordinator.Begin(64));
		assert(coordinator.Post({ SceneEventKind::RefreshGroup, 3, 1, { 1, 1 }, Group(1, 2) }) == Result::Accepted);
		assert(coordinator.Post({ SceneEventKind::RefreshGroup, 4, 1, { 1, 1 }, Group(1, 3) }) == Result::Coalesced);
		assert(first->scene->groups.at(1)->SourceUpdate() == 1);
		auto artifact = std::make_shared<const int>(42);
		assert(!coordinator.Complete(first->ticket + 1, artifact, 4));
		assert(coordinator.Building());
		assert(coordinator.Complete(first->ticket, artifact, 4));
		assert(!coordinator.Begin(64)); // ready must be acknowledged, never overwritten
		auto ready = coordinator.Ready();
		assert(ready && ready->request.ticket == first->ticket);
		assert(!coordinator.Acknowledge(first->ticket + 1, true));
		assert(coordinator.Acknowledge(first->ticket, true));
		auto next = coordinator.Begin(64);
		assert(next && next->scene->groups.at(1)->SourceUpdate() == 3);
		assert(next->ticket != first->ticket);
		assert(!coordinator.Fail(first->ticket));
		assert(coordinator.Fail(next->ticket));
		auto retry = coordinator.Begin(64);
		assert(retry && retry->scene == next->scene && retry->ticket != next->ticket);
		// Reset logically invalidates the job, but does not release its reservation
		// or permit an overlapping build until the worker returns its completion.
		assert(coordinator.Post({ SceneEventKind::Reset, 5, 2, {}, {} }) == Result::Accepted);
		assert(coordinator.Building() && !coordinator.Begin(0));
		assert(coordinator.Complete(retry->ticket, artifact, 4));
		assert(!coordinator.Ready());
		auto reset = coordinator.Begin(0);
		assert(reset && reset->scene->lifecycle == 2 && reset->scene->groups.empty());
		assert(coordinator.Complete(reset->ticket, artifact, 4)); // exceeds reservation: retry, no ready result
		assert(!coordinator.Ready());
		auto resized = coordinator.Begin(4);
		assert(resized && coordinator.Complete(resized->ticket, artifact, 4));
		assert(coordinator.Acknowledge(resized->ticket, false));
		auto rejected = coordinator.Begin(4);
		assert(rejected && coordinator.Complete(rejected->ticket, artifact, 4));
		assert(coordinator.Post({ SceneEventKind::Reset, 6, 3, {}, {} }) == Result::Accepted);
		assert(!coordinator.Ready());
		assert(!coordinator.Acknowledge(rejected->ticket, true));
		assert(coordinator.Post({ SceneEventKind::ReplaceGroup, 7, 2, { 1, 1 }, Group(1, 4, 2) }) == Result::Invalid);
		assert(ready->request.scene->lifecycle == 1); // externally retained immutable lease

		// Real delayed worker: its immutable input survives a coordinator reset.
		DCLF::PublishedSceneExecutor executor;
		auto scope = executor.CreateScope("delayed preparation");
		auto delayed = coordinator.Begin(4);
		assert(delayed);
		std::promise<void> release;
		auto gate = release.get_future().share();
		std::promise<std::uint64_t> completed;
		auto completion = completed.get_future();
		assert(executor.Dispatch(scope, DCLF::PublishedSceneExecutor::Preparation, {}, "build",
			[request = *delayed, gate, &completed](const auto&) {
				gate.wait(); // artificial delay on test worker only
				assert(request.scene->lifecycle == 3);
				completed.set_value(request.ticket);
			}));
		assert(coordinator.Post({ SceneEventKind::Reset, 8, 4, {}, {} }) == Result::Accepted);
		assert(!coordinator.Begin(4) && coordinator.Building());
		release.set_value();
		assert(completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		assert(coordinator.Complete(completion.get(), artifact, 4));
		assert(!coordinator.Ready());
		scope->Wait();
		executor.Shutdown();
	}

	void Admission()
	{
		using Result = CaptureAdmission::Result;
		auto first = Group(1, 1);
		auto second = Group(1, 2);
		auto third = Group(1, 3);
		const auto bytes = first->OwnedBytes();
		assert(bytes >= sizeof(ConsistencyGroupUpdate));
		CaptureAdmission admission(bytes * 2, 2);
		CapturedSceneEvent reset{ SceneEventKind::Reset, 1, 1, {}, {} };
		CapturedSceneEvent replace{ SceneEventKind::ReplaceGroup, 2, 1, { 1, 1 }, first };
		CapturedSceneEvent refresh{ SceneEventKind::RefreshGroup, 3, 1, { 1, 1 }, second };
		assert(admission.TryPush(reset) == Result::Accepted);
		assert(admission.TryPush(replace) == Result::Accepted);
		assert(admission.TryPush(refresh) == Result::Pressure);
		CapturedSceneReducer reducer;
		assert(reducer.Apply(admission.TakePending()).applied == 2);
		assert(!admission.PendingBytes() && !admission.PendingEvents());
		assert(admission.TryPush(refresh) == Result::Accepted);
		refresh.sequence = 4;
		refresh.update = third;
		assert(admission.TryPush(refresh) == Result::Coalesced);
		assert(admission.PendingEvents() == 1 && admission.PendingBytes() == third->OwnedBytes());
		CapturedSceneEvent detach{ SceneEventKind::DetachGroup, 5, 1, { 1, 1 }, {} };
		assert(admission.TryPush(detach) == Result::Accepted);
		refresh.sequence = 6;
		refresh.update = Group(1, 4);
		assert(admission.TryPush(refresh) == Result::Pressure);
		auto pending = admission.TakePending();
		assert(pending.events[0].sequence == 4 && pending.events[1].sequence == 5);
		assert(reducer.Apply(std::move(pending)).applied == 2);
		assert(reducer.Snapshot()->groups.empty());
		assert(admission.TryPush(refresh) == Result::Accepted);
		assert(reducer.Apply(admission.TakePending()).rejected == 1); // cannot resurrect after detach
		assert(admission.TryPush(detach) == Result::Invalid); // stale sequence

		CaptureAdmission small(bytes - 1, 4);
		assert(small.TryPush(replace, 1) == Result::Pressure);
		assert(small.TryPush(replace) == Result::Accepted); // one isolated oversized group
		assert(small.TryPush(detach) == Result::Pressure); // no growth until drained
		assert(small.TakePending().events.size() == 1);
		assert(small.TryPush(detach) == Result::Accepted); // rejection did not consume sequence

		CaptureAdmission regular(bytes * 2, 4);
		assert(regular.TryPush(replace, bytes + 1) == Result::Pressure);
		assert(regular.TryPush(replace, bytes) == Result::Accepted);
		replace.sequence = 3;
		replace.update = second;
		assert(regular.TryPush(replace) == Result::Accepted); // replacements never coalesce
		assert(regular.PendingEvents() == 2 && regular.PendingBytes() == bytes * 2);
		assert(!Group(2, 3)->CanRefresh(*second));
		assert(!Group(1, 3, 2)->CanRefresh(*second));
		assert(!first->CanRefresh(*second));

		// A falsely labelled structural change is not accepted as dynamic state.
		std::array members{ MemberInput{ { 10, 1 } }, MemberInput{ { 11, 1 } } };
		std::array changed{ std::byte{ 8 } };
		std::array parts{
			ComponentInput{ { 10, 1 }, ComponentKind::Geometry, changed },
			ComponentInput{ { 10, 1 }, ComponentKind::Material, changed },
			ComponentInput{ { 11, 1 }, ComponentKind::Geometry, changed },
			ComponentInput{ { 11, 1 }, ComponentKind::Material, changed }
		};
		auto structural = ConsistencyGroupUpdate::Capture({ 1, 1 }, 1, 4, members, parts, {});
		assert(!structural->CanRefresh(*third));
		CapturedSceneReducer validation;
		assert(validation.Apply({ { reset, { SceneEventKind::ReplaceGroup, 2, 1, { 1, 1 }, third } } }).applied == 2);
		assert(validation.Apply({ { { SceneEventKind::RefreshGroup, 3, 1, { 1, 1 }, structural } } }).rejected == 1);
		assert(validation.Snapshot()->groups.at(1) == third);
	}
}

int main()
{
	EngineIdentities();
	CaptureValues();
	FaceValues();
	Lifecycle();
	WorkerOwnership();
	Admission();
	Preparation();
}
