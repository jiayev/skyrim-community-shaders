#pragma once

// The frame's schedule (Schedule/Frame.cpp): the engine points the epochs are submitted at (the triggers); each epoch DCLF adds passes to,
// its definition (its trigger, its shape, the shape it shares, its gate and whether the frame's claims need it) and its passes' keys
// (Api/Passes.h) in order; the epochs outside DCLF; the frame and async orders.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Which extension's resources an epoch's passes are made from: the main view's, the shadow views', a view's (Api::EngineView). */
	enum class EpochShape : std::uint8_t
	{
		Main,
		Shadow,
		View,
	};

	/** @brief The main view's shapes (EpochDef::shapeIndex of a Main epoch): the depth (Z-prepass) segment's, the colour segment's. */
	enum class MainShape : std::uint32_t
	{
		Depth,
		Colour,
	};

	/**
	 * @brief An engine point an epoch is submitted at: its owner's hook marks it active on the render thread for the duration of the
	 * hook's DCLF work (Core's FrameDriver points; a feature's, InstallContext::FindTrigger and Extensions::TriggerScope). An epoch
	 * submitted while another (or none) is active is counted and logged once.
	 */
	struct TriggerDef
	{
		std::string_view id;     // "shadow-maps.after", "depth.committed", ...
		std::string_view owner;  // whose hook fires it: "Core", "Features/Forward", ...
		std::string_view where;  // the engine point, and the hook
	};

	/** @brief A schedule's trigger (InstallContext::FindTrigger's result): its row in FrameSchedule::triggers. */
	struct TriggerId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
		bool operator==(const TriggerId&) const = default;
	};

	/**
	 * @brief An epoch: what Core records it against, and its passes' keys, added in this order. Its row's index in FrameSchedule::epochs
	 * is its index in the scene revisions (their shape and recording slots, the per-epoch counters, the coverage, the reports).
	 */
	struct EpochDef
	{
		std::string_view id;                       // "main.depth", "main.colour", "shadow", "reflection", "forward", "fp.depth", "fp.colour"
		std::string_view label;                    // the reports' name
		std::string_view trigger;                  // the TriggerDef id of the engine point it is submitted at
		std::uint32_t segment = 0;                 // RenderGraphRuntime::Segment, as its value
		EpochShape shape = EpochShape::Main;
		// Its index in its shape: Main, MainShape; Shadow, 0 the shadow views' and occlusion maps'; View, its Api::EngineView. As its value.
		std::uint32_t shapeIndex = 0;
		// The id of the epoch whose shape this one records against (the forward epoch, the colour shape's forward passes; first person's
		// colour epoch, its depth epoch's shape). Empty: its own shape.
		std::string_view sharesWith;
		// While this class is off, the shared shape is not taken and the frame's claims do not need the epoch (Count: none).
		ObjectClass gate = ObjectClass::Count;
		bool claims = false;    // the frame's claims need the epoch's recordings (while its gate is on)
		bool revision = true;   // revision-driven: each submission carries the scene revision's recording, no live ticket is prepared
		// The id of an earlier Main epoch that runs this one's passes (the reflection's faces and first person's depth, in the depth
		// epoch): its passes are keys of the host's list, prefixed with this epoch's id and kHostedKey ("reflection/view.faces"), made
		// from this epoch's own shape's resources (a View epoch's: its view's); its own passes list is empty. It has a revision shape slot
		// as any epoch (another epoch may share it: first person's colour epoch), but no segment (its host's), recording, order or
		// submission of its own: the host's recordings carry its passes, and the host's commit commits it. A host may host several.
		// Empty: its own epoch.
		std::string_view hostedBy;
		std::span<const std::string_view> passes;  // pass keys, in order
	};
	/** @brief Between a hosted epoch's id and its pass key, in its host's list (EpochDef::hostedBy). */
	inline constexpr char kHostedKey = '/';

	/** @brief An epoch outside DCLF (another feature's segment): only in the frame and async orders, never revision-driven. */
	struct ForeignEpoch
	{
		std::string_view id;
		std::uint32_t segment = 0;  // RenderGraphRuntime::Segment, as its value
	};

	/**
	 * @brief The frame's triggers (each named by an epoch); its epochs, in the revisions' slot order (within an epoch, its passes' order
	 * is the graph's; across epochs ORG orders them by frameOrder); the foreign ones; the frame's order and the host's async order, by id,
	 * every epoch and foreign epoch once but a hosted epoch (EpochDef::hostedBy), which runs as its host.
	 */
	struct FrameSchedule
	{
		std::span<const TriggerDef> triggers;
		std::span<const EpochDef> epochs;
		std::span<const ForeignEpoch> foreign;
		std::span<const std::string_view> frameOrder;  // the order a frame runs them (PersistentGraphHost::Desc::epochOrder)
		std::span<const std::string_view> asyncOrder;  // the host's async epochs, in its ticket loop's order (the revision-driven ones: those with revision)
	};
}
