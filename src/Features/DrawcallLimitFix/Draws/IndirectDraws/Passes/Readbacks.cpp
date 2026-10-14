// The GPU-to-host copies: tree LOD's list counts and the fade and visibility event rings.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Passes/PassesInternal.h"

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
}

#endif
