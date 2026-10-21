#pragma once

// The draw family contract's Core side (Api/DrawFamilies.h): each family's entry, resolved once at InstallFeatures (ResolveDrawFamilies)
// from its registration (a family no feature takes has none: its passes and requirements are left out), the contexts its callbacks are
// handed (their Core service: DrawFamilyFrame::Pipelines, Core/Render/DrawFamilies/FamilyDecisions.cpp), and the serve Core makes at the
// frame requests. Core's slot per family (its pipelines, pass names and ownership) is Core/Render/DrawFamilies/FamilySlots.h; the frame's
// decisions are IndirectDraws::DecideDrawFamilies'; the main passes and a trees view's faces call the entries' callbacks there.

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
		struct UnitGroup;
	}
}

namespace DCLF::Api
{
	/**
	 * @brief The frame a family's frame callbacks run in (render thread): FrameView's number; for decide, the scene's tables (null at
	 * serve) and the family.
	 */
	class DrawFamilyFrame
	{
	public:
		std::uint32_t frame = 0;
		Draws::SceneBuffers* scene = nullptr;
		DrawFamily family = DrawFamily::Count;

		/**
		 * @brief decide's: Core's pipeline lane for the family's slot - posts its program source (frame.programSource) and asks for its
		 * pipelines, counting the frame's wait for them; true once the frame's catalog has them and a_drawable (what else its draws need,
		 * the feature's), and then they are the slot's for the passes (published: Draws::FamilyPipelinesOf).
		 */
		bool Pipelines(bool a_drawable) const;
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
		std::uint64_t row = 0;  // the main pass's (out): the draw row's address its records read, fixed with the family's tables
	};

	/**
	 * @brief A main pass execution's revision (each preparation): the preparation, the scene's tables, and the slot's pipelines as the
	 * lane published them (null: none yet), which the execution draws with.
	 */
	class DrawFamilyRevision
	{
	public:
		const org::FramePreparationContext& preparation;
		const Draws::SceneBuffers& scene;
		const std::shared_ptr<const void>& pipelines;
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
	 * @brief A family's draw (the pass's recording, inside its render pass): the commands, the execution, the slot's pipelines it draws
	 * with (main), the draw row's address, the declared arguments resolved, and for a face the view's forward pipeline and the family's
	 * draw signature.
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

		/**
		 * @brief The slot's pipelines (main), as the lane's type (FamilyPipelines: depth, colour and, with two colour programs, colour2;
		 * the feature binds the colour pipeline of each draw it executes in the colour pass).
		 */
		template <class T>
		const T& Drawn() const
		{
			return *static_cast<const T*>(drawn.get());
		}
	};

	/**
	 * @brief A trees view's epoch commit (render thread): the scene's tables, whether the view draws the family this execution (its
	 * shape's pipeline, its own pipeline and the faces' ownership: DecideViewFaces), the row the faces' latch takes (out, rowBytes,
	 * zeroed), and what the execution holds (a texture the row binds). Without owners, the faces' decision (DecideViewFaces, viewDraws
	 * true): only whether the last depth commit's row draws, nothing bound.
	 */
	class DrawFamilyFaceRow
	{
	public:
		const Draws::SceneBuffers& scene;
		bool viewDraws;
		std::span<std::byte> row;
		std::vector<std::shared_ptr<const void>>* owners;
	};

	/**
	 * @brief A trees view's faces' lists (Core's ViewResources, one per face), whose units are the family's tables' (render thread or the
	 * snapshot builder): the scene's tables and the words the family's scene resources posted with the revision's inputs (the
	 * requirement's; empty elsewhere); out: the tables the lists hold units with (into tables, the producer's: the shapes are made at the
	 * least any of them holds), the units the revision requires of the lists (from inputs) and what the tables' current versions hold (a
	 * new list's first units).
	 */
	class DrawFamilyLists
	{
	public:
		const Draws::SceneBuffers& scene;
		std::span<const std::uint32_t> inputs;
		Draws::UnitGroup* tables = nullptr;
		std::uint64_t required = 0;
		std::uint32_t current = 0;
	};

	/**
	 * @brief A face of a trees view's epoch commit (render thread, per face): the commit's row (faces.row's, rowBytes), the face's list's
	 * device address (its adopted version) and whether the face is drawn; out: the face's row (rowBytes) and its list's header
	 * (headerBytes), which the face's latch values take.
	 */
	class DrawFamilyFaceWrite
	{
	public:
		std::span<const std::byte> commitRow;
		std::uint64_t listAddress = 0;
		bool drawn = false;
		std::span<std::byte> row;
		std::span<std::byte> header;
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kDrawFamilies = static_cast<std::size_t>(Api::DrawFamily::Count);

	/**
	 * @brief Where a family's face values stand in a trees view's face latch values (ViewLatchLayout): its row and its list's header, past
	 * the face's block and the earlier taken families' (in family order).
	 */
	struct DrawFamilyFaceLayout
	{
		std::uint32_t rowInFace = 0, headerInFace = 0;
	};

	/** @brief A family's entry: its registration (taken: a feature's), with its strings owned. */
	struct DrawFamilyEntry
	{
		bool taken = false;
		Api::DrawFamilyContribution contribution;
		std::string colourRange;
		// The program's and the faces' strings (contribution's views point into them).
		std::string name, label, keyName, sourcePath, nativeNote;
		std::string rowName, listName;
		// The engine shapes' property vtable (engineShapes.propertyVtable, called once at install).
		std::uintptr_t engineShapeVtable = 0;
		DrawFamilyFaceLayout faceLayout;
	};
	using DrawFamilySet = std::array<DrawFamilyEntry, kDrawFamilies>;

	/** @brief Main thread, the end of InstallFeatures: the families' entries from their registrations (family Count: none). */
	void ResolveDrawFamilies(const std::array<Api::DrawFamilyContribution, kDrawFamilies>& a_registered);
	/** @brief Any thread, after InstallFeatures (none taken before): the families' entries, by family. */
	const DrawFamilySet& DrawFamilies();
	inline const DrawFamilyEntry& DrawFamilyOf(Api::DrawFamily a_family) { return DrawFamilies()[static_cast<std::size_t>(a_family)]; }
	/** @brief Any thread, after InstallFeatures: the family whose program a_shaderType (RE::BSShader::Type) is (Count: none). */
	Api::DrawFamily DrawFamilyOfShader(std::uint8_t a_shaderType);
	/**
	 * @brief Any thread, after InstallFeatures: a trees view's face latch values' bytes - the face's block and every taken family's faces'
	 * row and header (DrawFamilyFaceLayout), rounded to 256.
	 */
	std::uint32_t DrawFamilyFaceBytes();
	/** @brief Whether a_family is DCLF's under a_toggles: taken, its class's toggle on. */
	bool DrawFamilyOn(const ToggleSet& a_toggles, Api::DrawFamily a_family);
	/**
	 * @brief Whether a_family draws in a trees view's faces: taken, with faces (Api::DrawFamilyContribution::faces). A family without
	 * them (grass) has no forward pipeline, rows, lists or faces' draws.
	 */
	inline bool DrawFamilyHasFaces(Api::DrawFamily a_family)
	{
		const auto& entry = DrawFamilyOf(a_family);
		return entry.taken && entry.contribution.faces.face;
	}

	/**
	 * @brief Render thread, FrameView's frame requests (running or not): every taken family's serve, in family order. (Their decide is
	 * IndirectDraws::DecideDrawFamilies', which publishes its result.)
	 */
	void ServeDrawFamilies(std::uint32_t a_frame);
}
