// The GPU-to-host copies: tree LOD's list counts and the fade and visibility event rings.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	struct TreeLodReadbackBindings
	{
		org::ResourceBindingToken visible;
	};

	struct TreeLodReadbackPrepared
	{
		std::shared_ptr<SceneBuffers::TreeLodCounts> counts;
	};

	/** @brief Copies tree LOD's list header to the frame slot's host buffer after phase 2, first summing what the slot held. */
	class TreeLodReadbackPass final : public org::TypedRenderGraphPass<TreeLodReadbackPass, TreeLodReadbackPrepared, TreeLodReadbackBindings>
	{
	public:
		explicit TreeLodReadbackPass(std::shared_ptr<SceneBuffers> a_scene) :
			scene(std::move(a_scene)) {}

		TreeLodReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			return { a_builder.CopySource(*scene->treeLodVisible) };
		}

		void InvocationRevision([[maybe_unused]] const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(NamedSerial(a_preparation, scene->treeLodVisible));
		}

		TreeLodReadbackPrepared Prepare(const TreeLodReadbackBindings&, [[maybe_unused]] const org::PassPrepareContext& a_preparation) const
		{
			return { scene->treeLodCounts };
		}

		static void Record(const TreeLodReadbackBindings& a_bindings, const TreeLodReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.counts)
				return;
			auto& counts = *a_frame.counts;
			const std::uint32_t slot = a_recording.FrameSlot();
			if (slot >= counts.readback.size())
				return;
			if (counts.filled[slot]) {
				auto resource = counts.readback[slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					TreeLod::VisibleHeader header;
					std::memcpy(&header, mapped, sizeof(header));
					counts.frames.fetch_add(1, std::memory_order_relaxed);
					counts.phaseOne.fetch_add(header.phaseOne[1], std::memory_order_relaxed);
					counts.retests.fetch_add(header.retests, std::memory_order_relaxed);
					counts.phaseTwo.fetch_add(header.phaseTwo[1], std::memory_order_relaxed);
					resource.Unmap(0, 0);
				}
			}
			a_recording.Commands().CopyBufferRegion(counts.readback[slot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.visible).GetHandle(), 0,
				sizeof(TreeLod::VisibleHeader));
			counts.filled[slot] = 1;
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
	};

	std::shared_ptr<org::RenderPass> MakeTreeLodReadbackPass(std::shared_ptr<SceneBuffers> a_scene)
	{
		return std::make_shared<TreeLodReadbackPass>(std::move(a_scene));
	}

	struct EventReadbackBindings
	{
		org::ResourceBindingToken events;
	};
	struct EventReadbackPrepared
	{
		std::shared_ptr<HostEventRing> ring;              // holds the staging set
		const HostEventRing::StagingSet* set = nullptr;  // the bound list's (its version the preparation's revision names)
	};

	/**
	 * @brief Copies an event list (fade write-back, visibility events) whole into the frame slot's host buffer: a fixed copy, so
	 * the recording depends on the slot alone. The depth commit harvests it when the slot comes round again.
	 */
	class EventReadbackPass final : public org::TypedRenderGraphPass<EventReadbackPass, EventReadbackPrepared, EventReadbackBindings>
	{
	public:
		EventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<HostEventRing> a_ring) :
			scene(std::move(a_scene)), events(std::move(a_events)), ring(std::move(a_ring)) {}

		EventReadbackBindings Declare(org::PassBuilder& a_builder) { return { a_builder.CopySource(*events) }; }

		void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const { a_out.push_back(NamedSerial(a_preparation, events)); }

		EventReadbackPrepared Prepare(const EventReadbackBindings&, const org::PassPrepareContext& a_preparation) const
		{
			return { ring, ring->SetFor(static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, events))) };
		}

		static void Record(const EventReadbackBindings& a_bindings, const EventReadbackPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			const std::uint32_t frameSlot = a_recording.FrameSlot();
			if (!a_prepared.set || frameSlot >= a_prepared.set->buffers.size())
				return;
			// The header and the list's full capacity.
			a_recording.Commands().CopyBufferRegion(a_prepared.set->buffers[frameSlot]->GetAPIResource().GetHandle(), 0, a_recording.Resolve(a_bindings.events).GetHandle(), 0,
				a_prepared.ring->bytesFor(a_prepared.set->events));
		}

	private:
		std::shared_ptr<SceneBuffers> scene;
		Versioned events;
		std::shared_ptr<HostEventRing> ring;
	};

	std::shared_ptr<org::RenderPass> MakeEventReadbackPass(std::shared_ptr<SceneBuffers> a_scene, Versioned a_events, std::shared_ptr<HostEventRing> a_ring)
	{
		return std::make_shared<EventReadbackPass>(std::move(a_scene), std::move(a_events), std::move(a_ring));
	}

	namespace
	{
		// Each view's readback as its pass last declared it: weak, so the graph's pass alone keeps it (and its host buffers).
		std::array<std::atomic<std::weak_ptr<CounterReadback>>, static_cast<std::size_t>(CullCounterView::kCount)> counterReadbacks;
	}

	std::shared_ptr<CounterReadback> CullCounterReadback(CullCounterView a_view)
	{
		return counterReadbacks[static_cast<std::size_t>(a_view)].load(std::memory_order_acquire).lock();
	}

	struct CounterReadbackBindings
	{
		std::vector<org::ResourceBindingToken> counts;  // by source
	};
	struct CounterReadbackPrepared
	{
		std::shared_ptr<CounterReadback> readback;
		CounterSource source;
	};

	/**
	 * @brief Copies each drawn source's count words and its culling latch's stamp into the frame slot's host buffer (CounterReadback):
	 * a fixed copy, so the recording depends on the shape and the slot alone. The epochs harvest it (CullCounters.cpp, the colour
	 * commit; IndirectDraws::Impl::ReadShadowCullCounters).
	 */
	class CounterReadbackPass final : public org::TypedRenderGraphPass<CounterReadbackPass, CounterReadbackPrepared, CounterReadbackBindings>
	{
	public:
		CounterReadbackPass(CullCounterView a_view, std::vector<std::shared_ptr<org::Buffer>> a_counts, std::function<CounterSource(const org::PassPrepareContext&)> a_source) :
			view(a_view), counts(std::move(a_counts)), source(std::move(a_source))
		{
			auto* host = RenderGraphRuntime::Get().Host();
			readback = std::make_shared<CounterReadback>(host ? host->FrameSlots() : 1u, static_cast<std::uint32_t>(counts.size()));
		}

		CounterReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			CounterReadbackBindings bindings{};
			for (const auto& count : counts)
				bindings.counts.push_back(a_builder.CopySource(count));
			counterReadbacks[static_cast<std::size_t>(view)].store(readback, std::memory_order_release);
			return bindings;
		}

		void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const { a_out.push_back(source(a_preparation).generation); }

		CounterReadbackPrepared Prepare(const CounterReadbackBindings&, const org::PassPrepareContext& a_preparation) const
		{
			CounterReadbackPrepared prepared{ readback, source(a_preparation) };
			std::erase_if(prepared.source.copies, [&](const CounterSource::Copy& a_copy) { return a_copy.source >= counts.size(); });
			return prepared;
		}

		static void Record(const CounterReadbackBindings& a_bindings, const CounterReadbackPrepared& a_prepared, org::PassRecordContext& a_recording)
		{
			const auto& readback = a_prepared.readback;
			const auto& latch = a_prepared.source.latch;
			const std::uint32_t frameSlot = a_recording.FrameSlot();
			if (!readback || !latch || frameSlot >= readback->buffers.size())
				return;
			auto& commands = a_recording.Commands();
			const auto host = readback->buffers[frameSlot]->GetAPIResource().GetHandle();
			const auto latchBuffer = latch->Resource()->GetAPIResource().GetHandle();
			const std::uint64_t region = latch->Offset(frameSlot);
			constexpr std::uint64_t kWordBytes = sizeof(std::uint32_t);
			for (const auto& copy : a_prepared.source.copies) {
				const std::uint64_t at = std::uint64_t(copy.source) * CounterReadback::kSourceWords * kWordBytes;
				commands.CopyBufferRegion(host, at, a_recording.Resolve(a_bindings.counts[copy.source]).GetHandle(), 0, kCountWords * kWordBytes);
				commands.CopyBufferRegion(host, at + kCountWords * kWordBytes, latchBuffer, region + copy.latchOffset + offsetof(BuildDrawsLatch, visibilityStamp), kWordBytes);
			}
		}

	private:
		CullCounterView view;
		std::vector<std::shared_ptr<org::Buffer>> counts;
		std::function<CounterSource(const org::PassPrepareContext&)> source;
		std::shared_ptr<CounterReadback> readback;
	};

	std::shared_ptr<org::RenderPass> MakeCounterReadbackPass(CullCounterView a_view, std::vector<std::shared_ptr<org::Buffer>> a_counts,
		std::function<CounterSource(const org::PassPrepareContext&)> a_source)
	{
		return std::make_shared<CounterReadbackPass>(a_view, std::move(a_counts), std::move(a_source));
	}

	std::shared_ptr<org::RenderPass> MakeShadowCullCounterPass(std::shared_ptr<ShadowResources> a_resources)
	{
		// Every view slot's count buffer; each drawn view's culling latch at its slot within the region (as ShadowBuildDrawsPass reads it).
		return MakeCounterReadbackPass(CullCounterView::kShadow, a_resources->count, [](const org::PassPrepareContext& a_preparation) {
			CounterSource out;
			const auto* shapes = RevisionShapesOf(a_preparation);
			const auto frame = shapes ? shapes->shadow : nullptr;
			if (!frame)
				return out;
			out.generation = frame->generation;
			out.latch = frame->latch;
			for (const auto& view : frame->views)
				out.copies.push_back({ view.slot, view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)) });
			return out;
		});
	}
}
