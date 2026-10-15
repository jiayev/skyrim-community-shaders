#pragma once

// The draw family contract's Core side (Api/DrawFamilies.h): each family's entry, resolved once at InstallFeatures (ResolveDrawFamilies)
// from its registration (a family no feature takes has none: its passes and requirements are left out), the contexts its callbacks are
// handed, and the calls Core makes at the frame's start. Core's slot per family (its pipelines, pass names and ownership) is
// Core/Render/DrawFamilies/FamilySlots.h; the main passes and a trees view's faces call the entries' callbacks there.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org
{
	class RenderPassBuilder;
	struct FramePreparationContext;
	struct ResourceBindingToken;
}

namespace DCLF
{
	namespace Draws
	{
		struct SceneBuffers;
		struct ViewResources;
	}
	namespace TreeLod
	{
		struct DrawRow;
	}
}

namespace DCLF::Api
{
	class InstallContext;

	/** @brief The frame a family's frame callbacks run in (render thread): FrameView's number. */
	class DrawFamilyFrame
	{
	public:
		std::uint32_t frame = 0;
	};

	/**
	 * @brief A pass's declarations (the graph's build): the main pass's (view null) or a trees view's faces' draw pass (its resources),
	 * the scene's tables, and the family's indirect arguments the pass resolves at its recording (out: the main pass's one, a face's each).
	 */
	class DrawFamilyDeclare
	{
	public:
		org::RenderPassBuilder& builder;
		const Draws::SceneBuffers& scene;
		const Draws::ViewResources* view;
		std::span<org::ResourceBindingToken> arguments;
	};

	/**
	 * @brief A main pass execution's preparation: its revision (what moves it) and its prepare (out: what its record draws with, null
	 * for nothing this execution, and the draw row's address).
	 */
	class DrawFamilyPrepare
	{
	public:
		const org::FramePreparationContext& preparation;
		const Draws::SceneBuffers& scene;
		std::shared_ptr<const void> drawn;
		std::uint64_t row = 0;
	};

	/** @brief Which execution a family's draw is recorded in. */
	enum class DrawFamilyPass : std::uint8_t
	{
		Depth,          // the depth segment's phase 1 (z.depth)
		DepthPhaseTwo,  // its phase 2 (depth-phase2)
		Colour,         // the colour segment (main-opaque), under the Z-prepass layout
		Face,           // a trees view's face
	};

	/**
	 * @brief A family's draw (the pass's recording, inside its render pass): the commands, the execution, what prepare gave (main), the
	 * draw row's address, the declared arguments resolved, and for a face the view's forward pipeline and the family's draw signature.
	 */
	class DrawFamilyRecord
	{
	public:
		rhi::CommandList& commands;
		DrawFamilyPass pass;
		const std::shared_ptr<const void>& drawn;
		std::uint64_t row;
		rhi::ResourceHandle arguments;
		rhi::PipelineHandle pipeline;
		rhi::CommandSignatureHandle signature;

		/** @brief What prepare gave, as the family's type. */
		template <class T>
		const T& Drawn() const
		{
			return *static_cast<const T*>(drawn.get());
		}
	};

	/** @brief A trees view's graph extension (the graph's build): the view's resources, its faces' rows and lists among them. */
	class DrawFamilyViewContext
	{
	public:
		const std::shared_ptr<Draws::ViewResources>& resources;
	};

	/**
	 * @brief A trees view's epoch commit (render thread): the scene's tables, whether the view draws the family this execution (its
	 * shape's pipeline, its own pipeline and the faces' ownership: DecideViewTreeLod), the row the faces' latch takes (out), and what
	 * the execution holds (a texture the row binds).
	 */
	class DrawFamilyFaceRow
	{
	public:
		const Draws::SceneBuffers& scene;
		bool viewDraws;
		TreeLod::DrawRow& row;
		std::vector<std::shared_ptr<const void>>& owners;
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kDrawFamilies = static_cast<std::size_t>(Api::DrawFamily::Count);

	/** @brief A family's entry: its registration (taken: a feature's), with its strings owned. */
	struct DrawFamilyEntry
	{
		bool taken = false;
		Api::DrawFamilyContribution contribution;
		std::string colourRange;
	};
	using DrawFamilySet = std::array<DrawFamilyEntry, kDrawFamilies>;

	/** @brief Main thread, the end of InstallFeatures: the families' entries from their registrations (family Count: none). */
	void ResolveDrawFamilies(const std::array<Api::DrawFamilyContribution, kDrawFamilies>& a_registered);
	/** @brief Any thread, after InstallFeatures (none taken before): the families' entries, by family. */
	const DrawFamilySet& DrawFamilies();
	inline const DrawFamilyEntry& DrawFamilyOf(Api::DrawFamily a_family) { return DrawFamilies()[static_cast<std::size_t>(a_family)]; }
	/** @brief Whether a_family is DCLF's under a_toggles: taken, its class's toggle on. */
	bool DrawFamilyOn(const ToggleSet& a_toggles, Api::DrawFamily a_family);

	/** @brief Render thread, FrameView's frame requests (running or not): every taken family's serve, in family order. */
	void ServeDrawFamilies(std::uint32_t a_frame);
	/** @brief Render thread, the frame's start before the main camera's cull: every taken family's decide, in family order. */
	void DecideDrawFamilies(std::uint32_t a_frame);
}

namespace DCLF::Draws
{
	/**
	 * @brief Main thread, InstallFeatures, after the features' installs: the families Core draws itself until their features take them
	 * (Core/Render/DrawFamilies/TreeInstances.cpp: tree LOD's instances, with its stages), each only while its class has a toggle.
	 */
	void InstallCoreDrawFamilies(Api::InstallContext& a_context);
}
