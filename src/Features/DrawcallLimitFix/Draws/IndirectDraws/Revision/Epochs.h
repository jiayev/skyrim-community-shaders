#pragma once

// A scene revision's epochs and their traits, and the views drawn by epochs of their own.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief A scene revision's epochs, by index: its shape and recording slots (SceneRevisions), the per-epoch counters and the
	 * coverage follow this order. The forward epoch records the colour shape's forward passes; the two first-person epochs share one
	 * first-person shape.
	 */
	namespace Epoch
	{
		enum : std::uint32_t
		{
			ZPrepass,
			Colour,
			Shadow,
			Occlusion,
			Reflection,
			Forward,
			FirstPersonDepth,
			FirstPersonColour,
			Count
		};
	}

	/** @brief The views drawn by epochs of their own (ViewEpoch.cpp): the reflection's faces and the first-person view. */
	enum class ViewKind : std::uint8_t
	{
		Reflection,
		FirstPerson,
	};
	constexpr std::size_t kViews = 2;
	constexpr std::size_t ViewIndex(ViewKind a_kind) { return static_cast<std::size_t>(a_kind); }

	// Index of a drawing segment's shape (Resources::frames): the main epochs'.
	constexpr std::size_t kDepthShape = Epoch::ZPrepass, kColourShape = Epoch::Colour;

	/** @brief What a revision's shape slot of an epoch holds: a main segment's shape, the shadow or occlusion variants, a view's. */
	enum class ShapeKind : std::uint8_t
	{
		Main,
		Shadow,
		View,
	};
	/**
	 * @brief An epoch's traits: its segment and name, the shape it records against (its kind and its index there: the main segment,
	 * shadow 0 or occlusion 1, the ViewKind), and the epoch whose shape it shares (Epoch::Count: none) - the forward epoch records the
	 * colour shape's forward passes, first person's colour epoch its depth epoch's shape.
	 */
	struct EpochTraits
	{
		RenderGraphRuntime::Segment segment;
		const char* name;
		ShapeKind shape;
		std::uint32_t index;
		std::uint32_t sharesWith = Epoch::Count;
	};
	inline constexpr std::array<EpochTraits, Epoch::Count> kEpochTraits = { {
		{ RenderGraphRuntime::Segment::ZPrepass, "Z-prepass", ShapeKind::Main, static_cast<std::uint32_t>(kDepthShape) },
		{ RenderGraphRuntime::Segment::MainOpaque, "colour", ShapeKind::Main, static_cast<std::uint32_t>(kColourShape) },
		{ RenderGraphRuntime::Segment::ShadowView, "shadow", ShapeKind::Shadow, 0 },
		{ RenderGraphRuntime::Segment::SkyOcclusion, "occlusion", ShapeKind::Shadow, 1 },
		{ RenderGraphRuntime::Segment::Reflection, "reflection", ShapeKind::View, static_cast<std::uint32_t>(ViewIndex(ViewKind::Reflection)) },
		{ RenderGraphRuntime::Segment::Forward, "forward", ShapeKind::Main, static_cast<std::uint32_t>(kColourShape), Epoch::Colour },
		{ RenderGraphRuntime::Segment::FirstPersonDepth, "first-person depth", ShapeKind::View, static_cast<std::uint32_t>(ViewIndex(ViewKind::FirstPerson)) },
		{ RenderGraphRuntime::Segment::FirstPerson, "first-person colour", ShapeKind::View, static_cast<std::uint32_t>(ViewIndex(ViewKind::FirstPerson)), Epoch::FirstPersonDepth },
	} };
}
