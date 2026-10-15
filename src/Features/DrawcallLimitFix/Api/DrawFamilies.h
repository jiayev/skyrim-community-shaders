#pragma once

// The draw family contract (Api::InstallContext::RegisterDrawFamily): a feature's instanced draws in Core's main passes (the depth
// segment's two phases and the colour segment) and in a trees view's faces, at Core's fixed slot for the family.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace org
{
	class RenderPass;
}

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/DrawFamilies.h), which the features inside DCLF include for them.
	class DrawFamilyFrame;
	class DrawFamilyDeclare;
	class DrawFamilyRevision;
	class DrawFamilyRecord;
	class DrawFamilyViewContext;
	class DrawFamilyFaceRow;
	class DrawFamilyLists;

	/**
	 * @brief Core's fixed slot per draw family (Core/Render/DrawFamilies/FamilySlots.h): what Core keeps for each, whichever feature
	 * takes it - its catalog pipelines (depth, colour, a view's forward) on Core's pipeline lane, built from the program source the
	 * feature names, their draw signature, its points in the main passes and a trees view's faces (ViewShape::trees draws TreeInstances
	 * per face), its part of the revisions' shapes key, and PassCapture's ownership of the engine's passes it replaces.
	 */
	enum class DrawFamily : std::uint8_t
	{
		// The tree LOD instances (the BSDistantTreeShaderProperty groups under the LOD trees root), one instanced draw of a culled list.
		TreeInstances,
		Count,
	};

	/**
	 * @brief A draw family: the feature's per-frame decisions and its part of each pass execution. None is called per object; a family
	 * no feature takes adds no passes and no requirement.
	 *
	 * - family: the slot. objectClass: the class whose toggle gates it (ObjectClassOn, read as data; registered before). resources: its
	 *   tables (Api/SceneResources.h; registered before): the family is set up while their state is made (Draws::FamilySetUp: its
	 *   decision, its draws, a trees view's lists). colourRange: the nvperf range around its colour draw.
	 * - frame.serve (optional): render thread, at the frame's start where the frame requests are served (running or not: FrameView's
	 *   ServeFrameRequests, at its end), handed the frame: what the family mirrors of the engine, drained.
	 * - frame.decide: render thread, once a frame before the main camera's cull (FrameDriver, before the FrameStartAfterTreeLod view
	 *   work), only while the family is set up, on (its class's toggle) and the frame's set not withdrawn, handed the frame with the
	 *   scene's tables: whether the family's draws are DCLF's this frame, which Core publishes (IndirectDraws::DrawFamilyOwned; then
	 *   PassCapture withholds the engine's passes they replace); else they are not. Its pipelines are Core's lane's
	 *   (DrawFamilyFrame::Pipelines).
	 * - frame.programSource: render thread, from DrawFamilyFrame::Pipelines: the engine shader its slot's pipelines are built from (null:
	 *   none yet), posted to the pipeline lane.
	 * - main.declare, main.revision, main.record: the main pass's executions (z.depth, depth-phase2, main-opaque), in the graph's build
	 *   (declare: its arguments and its draw row's address, fixed with its tables), each preparation (revision: what moves the recording
	 *   besides the slot's pipelines' words Core hands it; Core prepares the slot's pipelines and the row while the family is set up),
	 *   and each recording (record, only once the slot's pipelines are published: after the depth draws in the depth passes; in the
	 *   colour pass after the main draws, under the Z-prepass layout and colourRange).
	 * - faces.makeCull, faces.declare, faces.record, faces.row, faces.lists (optional, all or none): a trees view's faces
	 *   (Api::ViewShape::trees): its cull pass in the view's depth epoch (makeCull at the graph's build; null: none), the faces' draw
	 *   pass's declarations and its draw after each face's buckets (record, for a face whose shape has the family's pipeline), each epoch
	 *   commit's row (row: the row the faces' latch takes, its texture held; true when the faces draw it; and the faces' decision, without
	 *   owners), and the units the faces' lists hold with the family's tables (lists: Core's view machinery's lists, sized, required and
	 *   counted with them).
	 */
	struct DrawFamilyContribution
	{
		DrawFamily family = DrawFamily::Count;
		ObjectClass objectClass = ObjectClass::Count;
		SceneResourcesId resources;
		std::string_view colourRange;
		struct Frame
		{
			void (*serve)(const DrawFamilyFrame& a_frame) = nullptr;
			bool (*decide)(const DrawFamilyFrame& a_frame) = nullptr;
			const void* (*programSource)() = nullptr;
		} frame;
		struct Main
		{
			void (*declare)(DrawFamilyDeclare& a_declare) = nullptr;
			void (*revision)(const DrawFamilyRevision& a_revision, std::vector<std::uint64_t>& a_out) = nullptr;
			void (*record)(const DrawFamilyRecord& a_record) = nullptr;
		} main;
		struct Faces
		{
			std::shared_ptr<org::RenderPass> (*makeCull)(const DrawFamilyViewContext& a_view) = nullptr;
			void (*declare)(DrawFamilyDeclare& a_declare) = nullptr;
			void (*record)(const DrawFamilyRecord& a_record) = nullptr;
			bool (*row)(DrawFamilyFaceRow& a_row) = nullptr;
			void (*lists)(DrawFamilyLists& a_lists) = nullptr;
		} faces;
	};
}
