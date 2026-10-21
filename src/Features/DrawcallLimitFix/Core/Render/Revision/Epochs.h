#pragma once

// A scene revision's epochs (the schedule's, resolved) and their traits, and the views drawn by epochs of their own.
// Self-contained (features include it); Internal.h includes it in its order.

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DCLF::Draws
{
	/*
	 * A scene revision's epochs are the schedule's (Schedule/Frame.cpp: Api::EpochDef), resolved once at the end of InstallFeatures
	 * (ResolveSchedule): an epoch is its row's index. Its shape and recording slots (SceneRevisions), the per-epoch counters, the coverage
	 * and the reports follow that order; the fixed arrays they live in hold kMaxEpochs, and their loops run to EpochCount().
	 */
	constexpr std::uint32_t kMaxEpochs = 12;
	constexpr std::uint32_t kNoEpoch = ~0u;

	/**
	 * @brief The views drawn by epochs of their own (ViewEpoch.cpp): the reflection's faces and the first-person view. The engine views
	 * (Api/Views.h), in Core's slot order (Views/ViewSlots.h).
	 */
	using ViewKind = Api::EngineView;
	constexpr std::size_t kViews = static_cast<std::size_t>(ViewKind::Count);
	constexpr std::size_t ViewIndex(ViewKind a_kind) { return static_cast<std::size_t>(a_kind); }

	// Index of a drawing segment's shape (Resources::frames): the main epochs' (Api::MainShape).
	constexpr std::size_t kDepthShape = static_cast<std::size_t>(Api::MainShape::Depth), kColourShape = static_cast<std::size_t>(Api::MainShape::Colour);
	// Index of a shadow epoch's shape (Api::EpochDef::shapeIndex): the shadow views', the occlusion maps'.
	constexpr std::uint32_t kShadowShape = 0, kOcclusionShape = 1;

	/** @brief What a revision's shape slot of an epoch holds: a main segment's shape, the shadow or occlusion variants, a view's. */
	enum class ShapeKind : std::uint8_t
	{
		Main,
		Shadow,
		View,
	};
	/**
	 * @brief An epoch's traits: its segment and name, the shape it records against (its kind and its index there: the main segment,
	 * shadow 0 or occlusion 1, the ViewKind), the epoch whose shape it shares (kNoEpoch: none; always an earlier one), the class whose
	 * toggle gates the shared shape and the epoch's part in the claims (Count: none), and whether the frame's claims need its recordings.
	 * A hosted epoch (Api::EpochDef::hostedBy: the reflection's) names its host (hostedBy, an earlier Main epoch, whose segment it has), and
	 * the host it (hosts): the host's recordings carry the hosted epoch's passes, the hosted one has none of its own.
	 */
	struct EpochTraits
	{
		RenderGraphRuntime::Segment segment{};
		std::string_view name;
		ShapeKind shape = ShapeKind::Main;
		std::uint32_t index = 0;
		std::uint32_t sharesWith = kNoEpoch;
		Api::ObjectClass gate = Api::ObjectClass::Count;
		bool claims = false;
		std::uint32_t hostedBy = kNoEpoch, hosts = kNoEpoch;
	};
	/** @brief Any thread, after InstallFeatures (0 before): the schedule's epochs. */
	std::uint32_t EpochCount();
	/** @brief Any thread, after InstallFeatures: epoch a_epoch's traits (a_epoch < EpochCount()). */
	const EpochTraits& EpochTraitsOf(std::uint32_t a_epoch);
	/** @brief Any thread, after InstallFeatures: the epoch recording a_shape's a_index without sharing it (resolved at install). */
	std::uint32_t EpochOfShape(ShapeKind a_shape, std::uint32_t a_index);
	/** @brief Any thread, after InstallFeatures: the epoch whose id is a_id (kNoEpoch: none). A search: look it up once. */
	std::uint32_t EpochById(std::string_view a_id);
	/**
	 * @brief A view's epochs, from the schedule: the one recording its shape, then the one sharing it (kNoEpoch: none), and their segments
	 * (none: the first's again; a hosted epoch's, its host's).
	 */
	struct ViewEpochs
	{
		std::array<std::uint32_t, 2> epochs{ kNoEpoch, kNoEpoch };
		std::array<RenderGraphRuntime::Segment, 2> segments{};
	};
	/** @brief Any thread, after InstallFeatures: a_kind's epochs (resolved at install). */
	const ViewEpochs& ViewEpochsOf(ViewKind a_kind);
}
