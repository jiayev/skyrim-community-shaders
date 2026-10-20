// CS_DCLF_SET_PARITY: every Reports::kInterval-th colour epoch, BuildDraws' state class sub-ranges (the colour segment's sequences, the
// classCounts words and the count words) copied to staging and read a few colour epochs later, never waiting for the copies, against the
// shape the epoch recorded with: its class plan (the latch's class table and map) and its indirect state. Every sequence a call draws
// (index count > 0, below its sub-range's count) must name a pipeline its class's execution sets hold, at the index its own input's set
// index maps to in that class: a draw in another class's sub-range is drawn with that class's fixed-function state. Not the first-person
// view's forward range (its own sequences and bucket counts: a follow-up).

#include "DrawsState.h"
#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		constexpr std::uint32_t kReadDelay = 3;   // colour epochs before the first read
		constexpr std::uint32_t kReadTries = 60;  // colour epochs a copy still in flight is tried again before the check gives up

		/** @brief A sub-range's name for the report: its range (GpuLayouts.h, kClassRangeMain). */
		std::string RangeName(std::uint32_t a_range)
		{
			if (a_range == Draws::kClassRangeMain)
				return "main";
			if (a_range == Draws::kClassRangeForward)
				return "forward";
			const std::uint32_t slot = a_range - Draws::ClassRangeOfSlot(0);
			return slot < kDecalGroups ? fmt::format("decal group {}", slot + 1) : fmt::format("terrain group {}", slot - kDecalGroups + 1);
		}

		/** @brief A staging copy of a_source, after the epoch in D3D11 stream order (null: none made). */
		winrt::com_ptr<ID3D11Buffer> Staging(ID3D11Buffer* a_source)
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			if (!a_source)
				return buffer;
			D3D11_BUFFER_DESC desc{};
			a_source->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, buffer.put())))
				return {};
			globals::d3d::context->CopyResource(buffer.get(), a_source);
			return buffer;
		}

		std::uint32_t BufferBytes(ID3D11Buffer* a_buffer)
		{
			D3D11_BUFFER_DESC desc{};
			a_buffer->GetDesc(&desc);
			return desc.ByteWidth;
		}

		/** @brief The staged buffers, mapped together without waiting (false: one is still in flight, or a map failed; none stays mapped). */
		struct Mapped
		{
			std::array<D3D11_MAPPED_SUBRESOURCE, 3> maps{};
			std::array<ID3D11Buffer*, 3> buffers{};
			std::uint32_t mappedCount = 0;
			HRESULT failure = S_OK;

			bool Map(const DrawsState::SetClassReadback& a_readback)
			{
				buffers = { a_readback.sequences.get(), a_readback.count.get(), a_readback.classCounts.get() };
				for (; mappedCount < buffers.size(); ++mappedCount) {
					failure = globals::d3d::context->Map(buffers[mappedCount], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &maps[mappedCount]);
					if (FAILED(failure)) {
						Unmap();
						return false;
					}
				}
				return true;
			}
			void Unmap()
			{
				for (std::uint32_t b = 0; b < mappedCount; ++b)
					globals::d3d::context->Unmap(buffers[b], 0);
				mappedCount = 0;
			}
			~Mapped() { Unmap(); }
		};

		/** @brief A class's sub-range of a range as CheckSequences reads it: its pipelines, its drawn slots, whether it is in pipeline order. */
		struct SubRange
		{
			std::uint32_t c = 0, r = 0, pipelines = 0, drawn = 0;
			bool sorted = false;  // a main sub-range DrawSort wrote
		};

		/**
		 * @brief The drawn slots of a sub-range (a_sequences: its first; a_half: which forward half, for the report): each sequence with an
		 * index count names a pipeline its class's sets hold, at the index one of its object's inputs' set indices maps to in the class.
		 */
		void CheckSequences(const DrawsState::SetClassReadback& a_readback, std::span<const std::uint32_t> a_classOf, const DrawSequence* a_sequences, const SubRange& a_sub,
			const char* a_half, ParityCounter& a_parity, std::uint64_t& a_draws)
		{
			std::uint32_t previous = 0;
			for (std::uint32_t k = 0; k < a_sub.drawn; ++k) {
				const DrawSequence& sequence = a_sequences[k];
				if (sequence.indexCount == 0)
					continue;  // draws nothing: a culled decal, another class's decal's filler
				++a_draws;
				const std::uint32_t object = sequence.objectIndex & kObjectIndexMask;
				const auto where = [&] { return fmt::format("class {} {} sub-range{} slot {} (object {})", a_sub.c, RangeName(a_sub.r), a_half, k, object); };
				a_parity.Check(sequence.pipelineIndex < a_sub.pipelines, [&] {
					return fmt::format("{}: pipeline index {} past the class's {} pipelines", where(), sequence.pipelineIndex, a_sub.pipelines);
				});
				// Its object's inputs' set indices: one maps to this class at this index.
				const std::uint32_t word = (a_sub.c << kSetClassShift) | sequence.pipelineIndex;
				const auto found = a_readback.setsOf.find(object);
				const std::span<const std::uint32_t> sets = found != a_readback.setsOf.end() ? std::span<const std::uint32_t>(found->second) : std::span<const std::uint32_t>();
				const bool routed = std::ranges::any_of(sets, [&](std::uint32_t a_set) { return a_set < a_classOf.size() && a_classOf[a_set] == word; });
				a_parity.Check(routed, [&] {
					std::string inputs;
					for (const std::uint32_t set : sets)
						inputs += fmt::format("{}set {} -> {:#x}", inputs.empty() ? "" : ", ", set, set < a_classOf.size() ? a_classOf[set] : kNoSetClass);
					return fmt::format("{}: pipeline index {} (word {:#x}), its inputs': {}", where(), sequence.pipelineIndex, word,
						inputs.empty() ? std::string("none in the payload") : inputs);
				});
				// DrawSort: each class's main sub-range in pipeline order.
				if (a_sub.sorted) {
					a_parity.Check(sequence.pipelineIndex >= previous, [&] {
						return fmt::format("{}: pipeline index {} after {} (the class's main sub-range is sorted)", where(), sequence.pipelineIndex, previous);
					});
					previous = sequence.pipelineIndex;
				}
			}
		}

		/**
		 * @brief The shape's class plan against its sub-ranges and indirect state (the latch's plan is the shape's: its classes the shape's
		 * sub-ranges, its map the indirect state's class words). Returns the class words BuildDraws read: the plan's map (the latch's), else
		 * the indirect state's.
		 */
		std::span<const std::uint32_t> CheckPlan(const DrawsState::SetClassReadback& a_readback, ParityCounter& a_parity)
		{
			const auto* plan = a_readback.plan.get();
			const auto& classes = a_readback.classes;
			a_parity.Check(plan && plan->classes == classes && a_readback.pipelines.size() == classes.size(), [&] {
				return fmt::format("the shape's {} classes, its indirect state's {}, its class plan's {}", classes.size(), a_readback.pipelines.size(),
					plan ? fmt::format("{}", plan->classes.size()) : std::string("(none)"));
			});
			if (!plan || plan->map.size() < Draws::kClassHeaderWords) {
				a_parity.Check(false, [&] { return std::string("the shape has no class map"); });
				return a_readback.classOf;
			}
			// The set indices' class words (the forward buckets' words follow them: SetClassPlan::map).
			const auto words = std::span<const std::uint32_t>(plan->map).subspan(Draws::kClassHeaderWords).first(
				std::min<std::size_t>(plan->map[0], plan->map.size() - Draws::kClassHeaderWords));
			a_parity.Check(std::ranges::equal(words, a_readback.classOf), [&] {
				return fmt::format("the class plan's map ({} words) is not the indirect state's class words ({})", words.size(), a_readback.classOf.size());
			});
			return words;
		}

		/** @brief The readback against its shape; logs the verdict line. */
		void CheckReadback(const DrawsState::SetClassReadback& a_readback, const Mapped& a_mapped)
		{
			const auto* sequences = static_cast<const DrawSequence*>(a_mapped.maps[0].pData);
			const auto* count = static_cast<const std::uint32_t*>(a_mapped.maps[1].pData);
			const auto* classCounts = static_cast<const std::uint32_t*>(a_mapped.maps[2].pData);
			const std::uint64_t sequenceSlots = BufferBytes(a_readback.sequences.get()) / sizeof(DrawSequence);
			const std::uint32_t countWords = BufferBytes(a_readback.count.get()) / sizeof(std::uint32_t);
			const std::uint32_t classCountWords = BufferBytes(a_readback.classCounts.get()) / sizeof(std::uint32_t);
			const auto& classes = a_readback.classes;
			ParityCounter parity;
			std::uint64_t subRanges = 0, draws = 0, decalDraws = 0, forwardDraws = 0, otherDraws = 0;
			// The words BuildDraws counts a draw it dropped in: none may be (a dropped draw is an object not drawn).
			const std::uint32_t unplanned = kCountClassUnplannedWord < countWords ? count[kCountClassUnplannedWord] : 0;
			const std::uint32_t overflow = kCountClassOverflowWord < countWords ? count[kCountClassOverflowWord] : 0;
			parity.Check(unplanned == 0, [&] { return fmt::format("{} colour draws had no class sub-range (kCountClassUnplannedWord)", unplanned); });
			parity.Check(overflow == 0, [&] { return fmt::format("{} colour draws were past their class sub-range's capacity (kCountClassOverflowWord)", overflow); });
			const std::span<const std::uint32_t> classOf = CheckPlan(a_readback, parity);
			for (std::uint32_t c = 0; c < classes.size(); ++c) {
				const std::uint32_t pipelines = c < a_readback.pipelines.size() ? a_readback.pipelines[c] : 0;
				parity.Check(classes[c].size == pipelines,
					[&] { return fmt::format("class {}: {} sort keys planned, its sets hold {} pipelines", c, classes[c].size, pipelines); });
				for (std::uint32_t r = 0; r < Draws::kClassRanges; ++r) {
					const auto& range = classes[c].ranges[r];
					const bool decal = r >= Draws::ClassRangeOfSlot(0) && r < Draws::ClassRangeOfSlot(kDecalGroups);
					const bool forward = r == Draws::kClassRangeForward;
					// The count the calls read: a decal group's word (each of its classes' sub-ranges), else the class's classCounts word.
					const std::uint32_t countWord = decal ? Draws::DecalCountWord(r - Draws::ClassRangeOfSlot(0)) : Draws::ClassCountWord(c, r);
					const bool counted = decal ? countWord < countWords : countWord < classCountWords;
					const std::uint32_t drawn = !counted ? 0 : decal ? count[countWord] : classCounts[countWord];
					if (!range.capacity) {
						// No sub-range: nothing may have been routed there (a decal group's count is its other classes').
						if (!decal)
							parity.Check(drawn == 0, [&] { return fmt::format("class {} has no {} sub-range, yet counted {} draws there", c, RangeName(r), drawn); });
						continue;
					}
					++subRanges;
					if (decal)
						parity.Check(range.capacity == a_readback.sequenceDecals, [&] {
							return fmt::format("class {} {} sub-range: capacity {}, the decal sub-ranges' {}", c, RangeName(r), range.capacity, a_readback.sequenceDecals);
						});
					parity.Check(counted && drawn <= range.capacity, [&] {
						return fmt::format("class {} {} sub-range: count {} over its capacity {}{}", c, RangeName(r), drawn, range.capacity, counted ? "" : " (no count word)");
					});
					parity.Check(std::uint64_t(range.first) + (forward ? 2ull : 1ull) * range.capacity <= sequenceSlots, [&] {
						return fmt::format("class {} {} sub-range [{}, +{}) past the sequences' {} slots", c, RangeName(r), range.first, range.capacity, sequenceSlots);
					});
					if (std::uint64_t(range.first) + (forward ? 2ull : 1ull) * range.capacity > sequenceSlots)
						continue;
					// The forward range's staging half (BuildDraws') and sorted half (ForwardSortCS', which the forward epoch draws).
					const SubRange sub{ c, r, pipelines, std::min(drawn, range.capacity), r == Draws::kClassRangeMain && a_readback.sorted };
					const std::uint64_t drawsBefore = draws;
					CheckSequences(a_readback, classOf, sequences + range.first, sub, forward ? " (staging)" : "", parity, draws);
					if (forward)
						CheckSequences(a_readback, classOf, sequences + range.first + range.capacity, sub, " (sorted)", parity, draws);
					(decal ? decalDraws : forward ? forwardDraws : otherDraws) += draws - drawsBefore;
				}
			}
			// The plain draws' buckets (CS_DCLF_FORWARD_PLAIN, CS_DCLF_MAIN_PLAIN): each bucket's count within its capacity, and each drawn
			// sequence (a forward bucket's sorted half, a main bucket's range) its set index's (one of its object's inputs has it), its first
			// instance its slot (the plain draw finds it by its instance).
			const auto checkBuckets = [&](const char* a_kind, std::span<const SetClassPlan::ForwardBucket> a_buckets, auto&& a_countWord, std::uint32_t a_halves,
										   std::uint64_t& a_draws) {
				for (std::size_t b = 0; b < a_buckets.size(); ++b) {
					const auto& bucket = a_buckets[b];
					const std::uint32_t countWord = a_countWord(b);
					const std::uint32_t drawn = countWord < classCountWords ? classCounts[countWord] : 0;
					parity.Check(countWord < classCountWords && drawn <= bucket.capacity, [&] {
						return fmt::format("{} bucket {} (set {}): count {} over its capacity {}", a_kind, b, bucket.set, drawn, bucket.capacity);
					});
					if (std::uint64_t(bucket.first) + std::uint64_t(a_halves) * bucket.capacity > sequenceSlots)
						continue;
					const DrawSequence* drawnSequences = sequences + bucket.first + (a_halves - 1) * bucket.capacity;
					for (std::uint32_t k = 0; k < std::min(drawn, bucket.capacity); ++k) {
						if (drawnSequences[k].indexCount == 0)
							continue;
						++a_draws;
						++draws;
						const std::uint32_t object = drawnSequences[k].objectIndex & kObjectIndexMask;
						const auto found = a_readback.setsOf.find(object);
						const bool routed = found != a_readback.setsOf.end() && std::ranges::find(found->second, bucket.set) != found->second.end();
						parity.Check(routed, [&] { return fmt::format("{} bucket {} (set {}) slot {}: object {} has no input of that set index", a_kind, b, bucket.set, k, object); });
						parity.Check(drawnSequences[k].firstInstance == k, [&] {
							return fmt::format("{} bucket {} (set {}) slot {}: first instance {}", a_kind, b, bucket.set, k, drawnSequences[k].firstInstance);
						});
					}
				}
			};
			if (const auto* plan = a_readback.plan.get()) {
				checkBuckets("forward", plan->forward, [&](std::size_t a_b) { return plan->ForwardCountWord(a_b); }, 2, forwardDraws);
				checkBuckets("main", plan->main, [&](std::size_t a_b) { return plan->MainCountWord(a_b); }, 1, otherDraws);
			}
			const auto line = fmt::format("[DCLF] set classes (GPU, colour epoch {}): {} classes, {} sub-ranges, {} draws checked ({} main and terrain, {} decals, {} forward: both halves), "
										  "{} without a class sub-range, {} over capacity; {} checks, {} failed (not the first-person forward range)",
				a_readback.armedEpoch, classes.size(), subRanges, draws, otherDraws, decalDraws, forwardDraws, unplanned, overflow, parity.checks, parity.mismatches);
			if (!parity.mismatches)
				logger::info("{} <- OK", line);
			else
				logger::warn("{} <- SET CLASS; first: {}", line, parity.first);
		}
	}

	void CheckSetClassParity(const MainEpochDone& a_epoch)
	{
		auto& state = DrawsState::Get();
		auto& readback = state.setClass;
		if (a_epoch.depthOnly)
			return;
		if (readback) {
			if (readback->framesLeft && --readback->framesLeft)
				return;
			Mapped mapped;
			if (!mapped.Map(*readback)) {
				// Still in flight: the next colour epoch tries again (never a wait), for a while.
				if (mapped.failure != DXGI_ERROR_WAS_STILL_DRAWING || ++readback->tries >= kReadTries) {
					logger::warn("[DCLF] set classes (GPU, colour epoch {}): the readback could not be mapped ({:#x}, {} tries); not checked", readback->armedEpoch,
						static_cast<std::uint32_t>(mapped.failure), readback->tries);
					readback.reset();
				}
				return;
			}
			CheckReadback(*readback, mapped);
			mapped.Unmap();
			readback.reset();
			return;
		}
		// Half a window after the build parity's copies, so the two do not stage on one epoch.
		if ((state.setClassEpochs++ % Reports::kInterval) != Reports::kInterval / 2)
			return;
		const auto& resources = a_epoch.resources;
		if (!resources || !resources->countD3D11 || !resources->classCounts)
			return;
		// The shape the epoch was committed into (the adopted revision's), and its sub-ranges.
		const auto& active = DrawsView::Get().revisions.active;
		const auto epoch = Draws::EpochOfShape(Draws::ShapeKind::Main, static_cast<std::uint32_t>(kColourShape));
		const auto shape = active ? active->Get<PassFrame>(DrawsView::Impl::SceneRevisions::kShapeSlot + epoch) : nullptr;
		if (!shape || shape->classes.empty())
			return;
		// The versions the adopted set names (what the epoch's recording wrote), which need not be the buffers' current ones.
		const auto& adopted = DrawsView::Get().Adopted();
		auto namedSequences = adopted.Find(*resources->sequences);
		auto* const sequencesView = state.setClassSequences.Of(namedSequences ? std::move(namedSequences) : resources->sequences->Current());
		auto* const classCountsView = state.setClassCounts.Of(adopted.Find(*resources->classCounts));
		if (!sequencesView || !classCountsView)
			return;
		DrawsState::SetClassReadback armed;
		armed.classes = shape->classes;
		armed.plan = shape->classPlan;
		armed.classOf = shape->indirect.classOf;
		armed.pipelines.reserve(shape->indirect.classes.size());
		for (const auto& entry : shape->indirect.classes)
			armed.pipelines.push_back(entry.pipelines);
		armed.sequenceDraws = shape->sequenceDraws;
		armed.sequenceDecals = shape->sequenceDecals;
		armed.sorted = resources->sort != nullptr;
		// Each object's set indices, from the payload's inputs (the resident region's lead its list): its draws' (refraction's is a
		// plain draw, outside the classes).
		const Draws::MainPayload& payload = a_epoch.payload;
		auto note = [&](const Draws::DrawInput& a_input) {
			if (a_input.view.mask & Draws::kViewRefraction)
				return;
			auto& sets = armed.setsOf[a_input.objectIndex];
			if (std::ranges::find(sets, a_input.pipelineIndex) == sets.end())
				sets.push_back(a_input.pipelineIndex);
		};
		for (std::size_t i = 0; i < payload.resident.Count(); ++i)
			note(*payload.resident.At(i));
		for (const auto& input : payload.body->inputList)
			note(input);
		armed.sequences = Staging(sequencesView);
		armed.count = Staging(resources->countD3D11.get());
		armed.classCounts = Staging(classCountsView);
		armed.armedEpoch = state.setClassEpochs - 1;
		armed.framesLeft = kReadDelay;
		if (armed.sequences && armed.count && armed.classCounts)
			readback = std::move(armed);
	}
}
