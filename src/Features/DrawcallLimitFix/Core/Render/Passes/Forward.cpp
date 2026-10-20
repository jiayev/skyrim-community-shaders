// The forward sort (the colour epoch's core.forward-sort and the views' view.forward-sort), and the frame slots' addresses a draw pass
// outside IndirectDraws' implementation pushes (DrawHelpers.h). The forward epoch's draws are Features/Forward's (ForwardPasses.cpp).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	/*
	 * The forward epoch's passes (dclf-architecture.md, "The forward epoch"). The colour BuildDraws appends visible forward draws (kObjectForward) with
	 * their view depth to their state class's staging half; ForwardSortPass (colour epoch) sorts each class's back to front into its sorted half;
	 * Features/Forward's ForwardDrawPass draws it in the forward epoch (the engine's blended draw, FUN_1414b39d0, after the deferred
	 * composite) on the colour pass's attachments; forward pipelines write the main target alone.
	 */
	namespace
	{
		struct ForwardSortBindings
		{
			org::DeclaredViewToken sequences, count;
		};

		// A dispatch per state class with a forward sub-range (ForwardSortSource::ranges).
		struct ForwardSortFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			std::vector<ForwardSortConstants> constants;
			std::vector<std::uint32_t> groups;
		};

		class ForwardSortPass final : public org::TypedRenderGraphPass<ForwardSortPass, ForwardSortFrame, ForwardSortBindings>
		{
		public:
			ForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source) :
				sequences(std::move(a_sequences)), counts(std::move(a_counts)), source(std::move(a_source)) {}

			ForwardSortBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.UnorderedAccess(*sequences).View(),
					counts.versioned ? a_builder.ShaderResource(*counts.versioned).View() : a_builder.ShaderResource(counts.buffer).View() };
			}

			void InvocationRevision(const org::PassPrepareContext& a_preparation, std::vector<std::uint64_t>& a_out) const { a_out.push_back(source(a_preparation).generation); }

			ForwardSortFrame Prepare(const ForwardSortBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				ForwardSortFrame prepared{};
				const auto sorted = source(a_preparation);
				if (!sorted.program || sorted.ranges.empty())
					return prepared;
				prepared.program = sorted.program;
				const std::uint32_t sequencesIndex = CaptureViewIndex(a_preparation, a_bindings.sequences);
				const std::uint32_t countIndex = CaptureViewIndex(a_preparation, a_bindings.count);
				for (const auto& range : sorted.ranges) {
					if (!range.capacity)
						continue;
					ForwardSortConstants constants{};
					constants.sequencesIndex = sequencesIndex;
					constants.countIndex = countIndex;
					constants.stagingBase = range.stagingBase;
					constants.capacity = range.capacity;
					constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
					// The class's count word (its classCounts word, a view's forward entry's bucket count).
					constants.countOffset = range.countOffset;
					constants.options = ForwardSortConstants::kCountOffset | range.options;
					prepared.constants.push_back(constants);
					// Full capacity; threads past the count return.
					prepared.groups.push_back((range.capacity + kForwardSortGroup - 1) / kForwardSortGroup);
				}
				return prepared;
			}

			static void Record(const ForwardSortBindings&, const ForwardSortFrame& a_frame, org::PassRecordContext& a_recording)
			{
				for (std::size_t r = 0; a_frame.program && r < a_frame.constants.size(); ++r)
					RecordDispatch(a_recording.Commands(), *a_frame.program, a_frame.constants[r], a_frame.groups[r]);
			}

		private:
			Versioned sequences;
			LatchedTarget counts;
			std::function<ForwardSortSource(const org::PassPrepareContext&)> source;
		};
	}

	std::shared_ptr<org::RenderPass> MakeForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source)
	{
		return std::make_shared<ForwardSortPass>(std::move(a_sequences), std::move(a_counts), std::move(a_source));
	}

	std::array<std::uint32_t, kFramePushWords> MainFramePushWords(std::uint64_t a_frameConstants)
	{
		return FramePushWords(a_frameConstants);
	}

	void MainShadowLayoutPush(std::uint64_t a_frameConstants, std::uint32_t (&a_out)[kShadowPushWords])
	{
		auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
			a_words[0] = static_cast<std::uint32_t>(a_value);
			a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
		};
		const std::uint64_t constants = a_frameConstants;
		split(a_out + kShadowPushFrameRecord, constants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
		split(a_out + kShadowPushViewBlock, constants + FrameSlotOffset(false, 0));
		split(a_out + kShadowPushPerFrame, constants + FrameSlotOffset(false, kPerFrameVertexRegister));
		split(a_out + kShadowPushZeros, constants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		split(a_out + kShadowPushSharedData, constants + FrameSlotOffset(false, kSharedDataRegister));
		split(a_out + kShadowPushFeatureData, constants + FrameSlotOffset(false, kFeatureDataRegister));
	}

	std::uint64_t LodFadeFrameAddress(std::uint64_t a_frameConstants)
	{
		return a_frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting);
	}
}
