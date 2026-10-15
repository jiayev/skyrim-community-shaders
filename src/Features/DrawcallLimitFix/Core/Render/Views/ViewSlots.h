#pragma once

// Core's fixed slot per engine view (Api::EngineView): what Core reserves for each, whichever feature takes the view. The per-view loops
// (the revision's latches, plans, shapes and key groups, the coverage, the views' setup) run over it, in its order. A new view is the
// enum value, a RenderGraphRuntime segment (a CS enum) and its row here, then the feature's registration.
// Part of IndirectDraws' implementation: included by Internal.h after Impl.h (it names the producer's key groups).

namespace DCLF::Draws
{
	/**
	 * @brief A view's slot: its epochs (the depth or only one, then the colour one; Epoch::Count: none) and their segments, the revision
	 * key group its inputs mix into (ImplProducer::ShapesKeyGroup; the shapes-made report names it) and its graph extension's id.
	 */
	struct ViewSlot
	{
		ViewKind view;
		std::array<std::uint32_t, 2> epochs;
		std::array<RenderGraphRuntime::Segment, 2> segments;
		std::size_t keyGroup;
		const char* extensionId;
	};
	inline constexpr std::array<ViewSlot, kViews> kViewSlots = { {
		{ ViewKind::Reflection, { Epoch::Reflection, Epoch::Count }, { RenderGraphRuntime::Segment::Reflection, RenderGraphRuntime::Segment::Reflection },
			ImplProducer::kKeyReflection, kReflectionExtensionId },
		{ ViewKind::FirstPerson, { Epoch::FirstPersonDepth, Epoch::FirstPersonColour },
			{ RenderGraphRuntime::Segment::FirstPersonDepth, RenderGraphRuntime::Segment::FirstPerson }, ImplProducer::kKeyFirstPerson, kFirstPersonExtensionId },
	} };
	static_assert(kViewSlots[ViewIndex(ViewKind::Reflection)].view == ViewKind::Reflection && kViewSlots[ViewIndex(ViewKind::FirstPerson)].view == ViewKind::FirstPerson,
		"kViewSlots is by view");
}
