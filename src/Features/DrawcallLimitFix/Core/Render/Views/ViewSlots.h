#pragma once

// Core's fixed slot per engine view (Api::EngineView): what Core reserves for each, whichever feature takes the view. The per-view loops
// (the revision's latches, plans, shapes and key groups, the coverage, the views' setup) run over it, in its order. A new view is the
// enum value, its row here, its epochs (the schedule's: Schedule/Frame.cpp, each a RenderGraphRuntime segment, a CS enum), then the
// feature's registration.
// Part of IndirectDraws' implementation: included by Internal.h after Impl.h (it names the producer's key groups).

namespace DCLF::Draws
{
	/**
	 * @brief A view's slot: the revision key group its inputs mix into (ImplProducer::ShapesKeyGroup; the shapes-made report names it)
	 * and its graph extension's id. Its epochs and their segments are the schedule's (ViewEpochsOf: the View-shape epochs of the view, the
	 * one recording its shape first, then the one sharing it).
	 */
	struct ViewSlot
	{
		ViewKind view;
		std::size_t keyGroup;
		const char* extensionId;
	};
	inline constexpr std::array<ViewSlot, kViews> kViewSlots = { {
		{ ViewKind::Reflection, ImplProducer::kKeyReflection, kReflectionExtensionId },
		{ ViewKind::FirstPerson, ImplProducer::kKeyFirstPerson, kFirstPersonExtensionId },
	} };
	static_assert(kViewSlots[ViewIndex(ViewKind::Reflection)].view == ViewKind::Reflection && kViewSlots[ViewIndex(ViewKind::FirstPerson)].view == ViewKind::FirstPerson,
		"kViewSlots is by view");
}
