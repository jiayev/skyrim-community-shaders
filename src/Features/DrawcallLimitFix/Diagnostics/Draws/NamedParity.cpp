// CS_DCLF_PERSISTENT_PARITY: the sealed set, the only source of a versioned buffer's address, SRV index and capacity. Every address and SRV
// index a snapshot's draws embed against the set its revision was sealed with, and every shape's ranges and counts against the named
// versions of the buffers it indexes (the snapshot builder); every commit target named and current in the adopted set, and the
// recordings' resolves of buffers their set does not name (render thread).

#include "DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		// The snapshot builder's (one pass at a time); the counts read by the render thread's report line.
		struct SealedChecks
		{
			std::atomic<std::uint64_t> snapshots{ 0 }, payloads{ 0 }, differ{ 0 }, unnamed{ 0 };
			std::uint32_t logged = 0;
		} sealedChecks;

		// The snapshot builder's; the counts read by the render thread's report line.
		struct ShapeChecks
		{
			std::atomic<std::uint64_t> revisions{ 0 }, shapes{ 0 }, findings{ 0 };
			std::uint32_t logged = 0;
		} shapeChecks;

		// The render thread's.
		struct CommitChecks
		{
			std::uint64_t targets = 0, unnamed = 0, notCurrent = 0, resolves = 0;
			std::uint32_t logged = 0;
			const Draws::NamedVersions* set = nullptr;  // the adopted set whose misses were last read, and their count
			std::uint64_t setSerial = 0, setMisses = 0;
			std::uint32_t frames = 0;
		} commitChecks;

		constexpr std::uint32_t kLogged = 16;

		/** @brief The fields a build embeds of the versioned buffers both its kinds of resources hold (Impl::AddressesOf). */
		template <class Resources>
		void CheckAddresses(const char* a_payload, const Draws::NamedVersions& a_sealed, const Resources& a_resources, const Draws::ResourceAddresses& a_embedded)
		{
			auto& c = sealedChecks;
			++c.payloads;
			// An unnamed buffer's entry is empty (check reports it unnamed first).
			const auto entry = [&](const Draws::Versioned& a_buffer) -> const org::services::NamedEntry& {
				static const org::services::NamedEntry kUnnamed;
				return a_buffer && a_sealed.Names(*a_buffer) ? a_sealed.Of(*a_buffer) : kUnnamed;
			};
			const auto& scene = *a_resources.scene;
			const auto check = [&](const char* a_field, const Draws::Versioned& a_buffer, std::uint64_t a_value, std::uint64_t a_expected) {
				if (a_buffer && !a_sealed.Names(a_buffer)) {
					++c.unnamed;
					if (c.logged++ < kLogged)
						logger::warn("[DCLF] snapshot draws (set {}): the {} payload's {} is of {}, which the sealed set does not name <- NAMED ADDRESS", a_sealed.Serial(),
							a_payload, a_field, a_buffer->Get()->GetName());
					return;
				}
				if (a_value == a_expected)
					return;
				++c.differ;
				if (c.logged++ < kLogged)
					logger::warn("[DCLF] snapshot draws (set {}): the {} payload's {} is {:#x}, the sealed set names {:#x} <- NAMED ADDRESS", a_sealed.Serial(), a_payload,
						a_field, a_value, a_expected);
			};
			// The vertex stream's buffer (Features/Faces' "face positions").
			if (a_embedded.streamBuffer || Extensions::VertexStreamTaken())
				check(Extensions::VertexStream().name.c_str(), scene.vertexStream, a_embedded.streamBuffer, entry(scene.vertexStream).deviceAddress);
			// The features' frame bindings (Api::FrameBinding), each read by the frame the context was posted at: the frame's buffer or the
			// next one's (one alternating by frame: either parity's).
			const auto bindings = Extensions::RegisteredFrameBindings();
			for (std::size_t i = 0; i < bindings.size(); ++i) {
				static const Draws::Versioned kNone;
				const auto* even = Extensions::FrameBindingBuffer(i, scene, 1);
				const auto* odd = Extensions::FrameBindingBuffer(i, scene, 0);
				const std::uint64_t value = a_embedded.frameBindings[i];
				const std::uint64_t evenIndex = even ? entry(*even).srvIndex : 0, oddIndex = odd ? entry(*odd).srvIndex : 0;
				check(bindings[i].name.c_str(), even ? *even : kNone, value, value == oddIndex ? oddIndex : evenIndex);
			}
		}

		/**
		 * @brief A main shape's state class sub-ranges (PassFrame::classes): the classes the set version has, each class's keys its
		 * pipelines, each sub-range in its own region of the sequences (the main ones in the draw range, the terrain and forward ones in
		 * phase 2's before refraction, the decal ones past both), a count word per (class, range), the latch's table and map room.
		 */
		template <class Finding>
		void CheckClassRanges(const PassFrame& a_frame, std::uint64_t a_sequences, std::uint64_t a_classWords, Finding&& a_finding)
		{
			const auto& classes = a_frame.classes;
			if (classes.empty())
				return;
			if (classes.size() != a_frame.indirect.classes.size())
				a_finding(fmt::format("{} classes planned, the set version has {}", classes.size(), a_frame.indirect.classes.size()));
			if (std::uint64_t(classes.size()) * kClassRanges > a_classWords)
				a_finding(fmt::format("{} classes' count words past the class counts' {}", classes.size(), a_classWords));
			if (classes.size() > a_frame.latchLayout.classes || a_frame.indirect.classOf.size() > a_frame.latchLayout.classMapWords)
				a_finding(fmt::format("{} classes and {} set indices past the latch's {} and {}", classes.size(), a_frame.indirect.classOf.size(), a_frame.latchLayout.classes,
					a_frame.latchLayout.classMapWords));
			const std::uint64_t draws = a_frame.sequenceDraws, refraction = Draws::RefractionSlot(a_frame);
			for (std::size_t c = 0; c < classes.size(); ++c) {
				if (c < a_frame.indirect.classes.size() && classes[c].size != a_frame.indirect.classes[c].pipelines)
					a_finding(fmt::format("class {}'s keys {} for its {} pipelines", c, classes[c].size, a_frame.indirect.classes[c].pipelines));
				for (std::uint32_t r = 0; r < kClassRanges; ++r) {
					const auto& range = classes[c].ranges[r];
					if (!range.capacity)
						continue;
					const bool decal = r >= Draws::ClassRangeOfSlot(0) && r < Draws::ClassRangeOfSlot(kDecalGroups);
					const std::uint64_t end = std::uint64_t(range.first) + (r == Draws::kClassRangeForward ? 2ull : 1ull) * range.capacity;
					const std::uint64_t from = r == Draws::kClassRangeMain ? 0 : decal ? 2 * draws : draws;
					const std::uint64_t to = r == Draws::kClassRangeMain ? draws : decal ? a_sequences : refraction;
					if (range.first < from || end > to || end > a_sequences)
						a_finding(fmt::format("class {} range {} at {} of {} outside [{}, {}) (the sequences' {})", c, r, range.first, range.capacity, from, to, a_sequences));
				}
			}
		}
	}

	void CheckSnapshotAddresses(const SnapshotSealed& a_sealed)
	{
		auto& c = sealedChecks;
		++c.snapshots;
		const auto& context = a_sealed.context;
		const auto& draws = a_sealed.draws;
		// A payload built for other resources than the context's names theirs (the frame does not submit it: identity).
		for (const std::size_t j : { Draws::kAsyncZPrepass, Draws::kAsyncColour })
			if (const auto& payload = draws.payloads[j]; payload && context.build[j] && context.target && context.target->scene &&
														 payload->inputs.addresses.identity == context.target.get())
				CheckAddresses(j == Draws::kAsyncZPrepass ? "Z-prepass" : "colour", a_sealed.sealed, *context.target, payload->inputs.addresses);
		if (const auto& shadow = draws.shadow; shadow && context.shadow && context.shadowTarget && context.shadowTarget->scene &&
											   shadow->inputs.addresses.identity == context.shadowTarget.get())
			CheckAddresses("shadow", a_sealed.sealed, *context.shadowTarget, shadow->inputs.addresses);
	}

	void CheckCommitTarget(const CommitTargetStaged& a_staged)
	{
		auto& c = commitChecks;
		++c.targets;
		if (!a_staged.sealed.Names(a_staged.target)) {
			++c.unnamed;
			if (c.logged++ < kLogged)
				logger::warn("[DCLF] frame {}: a commit staged bytes into {}, which the adopted set (set {}) does not name: nothing staged <- NAMED PARITY",
					Scene::FrameView::Get().GetFrame(), a_staged.target->Get()->GetName(), a_staged.sealed.Serial());
		} else if (const auto& entry = a_staged.sealed.Of(a_staged.target); entry.version != a_staged.target->Current()) {
			++c.notCurrent;
			if (c.logged++ < kLogged)
				logger::warn("[DCLF] frame {}: a commit staged bytes into {} through the adopted set (set {}), whose version {} is not the current {} <- NAMED PARITY",
					Scene::FrameView::Get().GetFrame(), a_staged.target->Get()->GetName(), a_staged.sealed.Serial(), entry.version->number,
					a_staged.target->Current() ? a_staged.target->Current()->number : 0);
		}
	}

	void CheckShapeCapacities(const RevisionShapesMade& a_made)
	{
		auto& c = shapeChecks;
		++c.revisions;
		const auto& sealed = a_made.sealed;
		const auto& inputs = a_made.inputs;
		const auto finding = [&](const std::string& a_what) {
			++c.findings;
			if (c.logged++ < kLogged)
				logger::warn("[DCLF] revision shapes (set {}): {} <- SHAPE CAPACITY", sealed.Serial(), a_what);
		};
		const auto units = [&](const Draws::Versioned& a_buffer) { return Draws::NamedUnits(sealed, a_buffer); };
		const auto bytes = [&](const Draws::Versioned& a_buffer) -> std::uint64_t { return a_buffer && sealed.Names(*a_buffer) ? sealed.Of(*a_buffer).bytes : 0; };
		constexpr std::uint64_t kSequence = sizeof(DrawSequence);
		// The views: every face's range within the sequences, the plan (its buckets and forward range) within a face's range, its buckets'
		// count words within each face's bucket counts.
		for (std::size_t v = 0; v < Draws::kViews; ++v) {
			const auto& frame = a_made.made.views[v];
			const auto& view = inputs.views[v].resources;
			if (!frame || !view)
				continue;
			++c.shapes;
			const auto& traits = *view->traits;
			if (std::uint64_t(traits.faces) * frame->sequenceDraws * kSequence > bytes(view->sequences))
				finding(fmt::format("the {} view's {} faces of {} draws past its sequences' {} bytes", traits.name, traits.faces, frame->sequenceDraws, bytes(view->sequences)));
			std::uint64_t planned = 0;
			for (const auto& bucket : frame->buckets)
				planned = std::max<std::uint64_t>(planned, std::uint64_t(bucket.first) + bucket.capacity);
			for (const auto& range : frame->forwardRanges)
				planned = std::max<std::uint64_t>(planned, std::uint64_t(range.first) + 2ull * range.capacity);
			if (planned > frame->sequenceDraws)
				finding(fmt::format("the {} view's plan reaches {} draws past a face's {}", traits.name, planned, frame->sequenceDraws));
			const std::uint64_t words = frame->buckets.size() + frame->forwardRanges.size();
			for (std::uint32_t f = 0; f < traits.faces; ++f)
				if (words > units(view->bucketCounts[f]))
					finding(fmt::format("the {} view's {} bucket words past face {}'s {}", traits.name, words, f, units(view->bucketCounts[f])));
		}
		// The main segments: their ranges within the sequences, the Z-prepass buckets' table within phase 1's range and their count
		// words within the counts, the terrain, forward and refraction ranges within phase 2's.
		if (const auto& main = inputs.main) {
			for (const auto& frame : a_made.made.main) {
				if (!frame)
					continue;
				++c.shapes;
				if (Draws::SequenceSlots(frame->sequenceDraws, frame->sequenceDecals, 0) * kSequence > bytes(main->sequences))
					finding(fmt::format("the main ranges of {} draws past the sequences' {} bytes", frame->sequenceDraws, bytes(main->sequences)));
				CheckClassRanges(*frame, units(main->sequences), units(main->classCounts), finding);
				if (frame->zPlan) {
					std::uint64_t end = 0;
					for (const auto& call : frame->zPlan->calls)
						end = std::max<std::uint64_t>(end, std::uint64_t(call.first) + call.capacity);
					if (end > frame->sequenceDraws)
						finding(fmt::format("the Z-prepass buckets' table reaches {} draws past the range's {}", end, frame->sequenceDraws));
					for (const auto& counts : main->zBucketCounts)
						if (frame->zPlan->Buckets() > units(counts))
							finding(fmt::format("{} Z-prepass buckets past the count words' {}", frame->zPlan->Buckets(), units(counts)));
				}
				const std::uint64_t phaseTwo = Draws::ForwardStagingSlot(*frame) + 2ull * frame->forwardCapacity + frame->refractionCapacity;
				if (phaseTwo > 2ull * frame->sequenceDraws)
					finding(fmt::format("the terrain, forward and refraction ranges reach {} past phase 2's end {}", phaseTwo, 2ull * frame->sequenceDraws));
			}
		}
		// The shadow and occlusion shapes: each view's slot within the slots made, its classes' ranges within the slot's sequences, its
		// buckets within the ranges and the slot's count words, its slot within the view blocks.
		if (const auto& shadow = inputs.shadow) {
			const auto& slots = inputs.shadowSlots;
			for (const auto& kind : a_made.made.shadow)
				for (const auto& frame : kind) {
					if (!frame)
						continue;
					++c.shapes;
					for (const auto& view : frame->views) {
						if (!slots || view.slot >= slots->sequences.size() || view.slot >= slots->bucketCounts.size()) {
							finding(fmt::format("a shadow view at slot {} past the slots made", view.slot));
							continue;
						}
						const std::uint64_t range = std::uint64_t(kShadowClasses) * view.sequenceDraws;
						if (range * kSequence > bytes(slots->sequences[view.slot]))
							finding(fmt::format("shadow slot {}'s {} draws a class past its sequences' {} bytes", view.slot, view.sequenceDraws, bytes(slots->sequences[view.slot])));
						if (view.capacity > view.sequenceDraws)
							finding(fmt::format("shadow slot {}'s max count {} past its {} draws", view.slot, view.capacity, view.sequenceDraws));
						std::uint64_t end = 0;
						for (const auto& bucket : view.buckets)
							end = std::max<std::uint64_t>(end, std::uint64_t(bucket.first) + bucket.capacity);
						if (end > range)
							finding(fmt::format("shadow slot {}'s buckets reach {} past its {} sequences", view.slot, end, range));
						if (view.buckets.size() > units(slots->bucketCounts[view.slot]))
							finding(fmt::format("shadow slot {}'s {} buckets past its count words' {}", view.slot, view.buckets.size(), units(slots->bucketCounts[view.slot])));
						if (view.slot >= units(shadow->viewBlocks.buffer))
							finding(fmt::format("shadow slot {} past the view blocks' {} slots", view.slot, units(shadow->viewBlocks.buffer)));
					}
				}
		}
		// The index pool: a first index and a copy for every geometry slot of the tables.
		if (const auto& scene = inputs.scene; scene && scene->pool) {
			const auto& pool = *scene->pool;
			if (units(pool.firsts) < a_made.geometries || units(pool.copies) < a_made.geometries)
				finding(fmt::format("{} geometry slots past the index pool's {} first indices or {} copies", a_made.geometries, units(pool.firsts), units(pool.copies)));
		}
	}

	void CheckNamedResolves()
	{
		const auto* draws = DrawsView::Find();
		if (!draws)
			return;
		auto& c = commitChecks;
		// The recordings of the adopted revision resolved every buffer they bind from its set: Find's misses, counted by the set.
		const auto& adopted = draws->Adopted();
		if (&adopted != c.set || adopted.Serial() != c.setSerial) {
			c.set = &adopted;
			c.setSerial = adopted.Serial();
			c.setMisses = 0;
		}
		if (const std::uint64_t misses = adopted.Misses(); misses > c.setMisses) {
			c.resolves += misses - c.setMisses;
			if (c.logged++ < kLogged)
				logger::warn("[DCLF] frame {}: the adopted set (set {}) was asked {} times for a buffer it does not name: a recording resolved an unnamed buffer <- NAMED PARITY",
					Scene::FrameView::Get().GetFrame(), adopted.Serial(), misses - c.setMisses);
			c.setMisses = misses;
		}
		if (++c.frames % 600 == 0) {
			logger::info("[DCLF] named parity: {} commit targets checked ({} unnamed, {} not current), {} resolves of unnamed buffers{}", c.targets, c.unnamed, c.notCurrent,
				c.resolves, c.unnamed || c.notCurrent || c.resolves ? " <- NAMED PARITY" : "");
			const auto& s = sealedChecks;
			const std::uint64_t differ = s.differ.load(std::memory_order_relaxed), unnamed = s.unnamed.load(std::memory_order_relaxed);
			logger::info("[DCLF] named addresses: {} snapshots, {} payloads checked against their sealed sets: {} fields differ, {} of unnamed buffers{}",
				s.snapshots.load(std::memory_order_relaxed), s.payloads.load(std::memory_order_relaxed), differ, unnamed, differ || unnamed ? " <- NAMED ADDRESS" : "");
			const auto& shapes = shapeChecks;
			const std::uint64_t findings = shapes.findings.load(std::memory_order_relaxed);
			logger::info("[DCLF] shape capacities: {} revisions, {} shapes checked against their sets' buffers: {} findings{}", shapes.revisions.load(std::memory_order_relaxed),
				shapes.shapes.load(std::memory_order_relaxed), findings, findings ? " <- SHAPE CAPACITY" : " <- OK");
		}
	}
}
