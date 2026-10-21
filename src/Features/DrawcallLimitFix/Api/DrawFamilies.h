#pragma once

// The draw family contract (Api::InstallContext::RegisterDrawFamily): a feature's instanced draws in Core's main passes (the depth
// segment's two phases and the colour segment) and in a trees view's faces, at Core's fixed slot for the family. And its sibling, the
// program family contract (RegisterProgramFamily): a material port's keys drawn by the main pipeline set at Core's slot for the family.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/DrawFamilies.h), which the features inside DCLF include for them.
	class DrawFamilyFrame;
	class DrawFamilyDeclare;
	class DrawFamilyRevision;
	class DrawFamilyRecord;
	class DrawFamilyFaceRow;
	class DrawFamilyLists;
	class DrawFamilyFaceWrite;

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
		// Grass (the BSGrassShaderProperty instance shapes, BSMultiStreamInstanceTriShape), drawn from the mirrored instance records as
		// arrays of instanced draws with a GPU count (one per bucket and LOD tier), without faces.
		Grass,
		Count,
	};

	/** @brief How a family's main pipelines are built from its program (Core's named rules). */
	enum class DrawFamilyBuild : std::uint8_t
	{
		// A depth pipeline (depth write, LESS) of the program's depth stages, then a colour one (no depth write, EQUAL against it) of its
		// colour stages - and with two colour programs a second, alike, of its second colour stages - all with culling off; the colour
		// pipelines blend as the engine's write mode the program names (DrawFamilyProgram::writeMode).
		DepthThenEqualColour,
	};
	/** @brief The draw signature a family's draws execute with (Core's, created per slot). */
	enum class DrawFamilySignature : std::uint8_t
	{
		// One non-indexed draw's arguments (vertices, instances, first vertex, first instance). A family's record may execute one, or an
		// array of them with a GPU count (ExecuteIndirect with a count buffer: the array's capacity, the count read from the buffer).
		DrawInstanced,
	};

	/**
	 * @brief A draw family's program: what Core's pipeline lane builds its slot's pipelines of (the engine shader frame.programSource
	 * names), resolved once at install with its strings owned.
	 *
	 * - shaderType: the engine shader (RE::BSShader::Type) its programs' defines come from; none Lighting's or another draw or program
	 *   family's.
	 * - name: the logs' ("DistantTree": "SPIR-V build of DistantTree ..."); label: the reports' and pipelines' ("tree LOD": "tree LOD
	 *   colour"); keyName: its recipes' ("tree-lod": "dclf.tree-lod", "dclf.forward.tree-lod"); sourcePath: its HLSL, loaded with Core's
	 *   sources; nativeNote: what stays native without it ("tree LOD stays native").
	 * - descriptors: the engine descriptors of its colour and depth programs, of a view's forward program (0 without faces) and of its
	 *   second colour program (colour2, with colours 2: grass's PBR technique).
	 * - colours: its colour programs, 1 or 2 (the second's pipeline is FamilyPipelines::colour2; the feature picks per draw).
	 * - writeMode: the engine write mode (alphaBlendWriteMode, 0-12) whose blend state its colour pipelines take (the engine's opaque 1
	 *   by default: tree LOD's).
	 * - build, signature: its pipelines' rule and its draws' signature.
	 */
	struct DrawFamilyProgram
	{
		std::uint8_t shaderType = 0xFF;
		std::string_view name, label, keyName, sourcePath, nativeNote;
		struct Descriptors
		{
			std::uint32_t colour = 0, depth = 0, forward = 0, colour2 = 0;
		} descriptors;
		std::uint8_t colours = 1;
		std::uint8_t writeMode = 1;
		DrawFamilyBuild build = DrawFamilyBuild::DepthThenEqualColour;
		DrawFamilySignature signature = DrawFamilySignature::DrawInstanced;
	};

	/**
	 * @brief The engine's shapes a family's draws replace (PassCapture withholds their passes while the family is DCLF's). propertyVtable:
	 * called once at install: the address of their shader property's vtable.
	 */
	struct DrawFamilyEngineShapes
	{
		const void* (*propertyVtable)() = nullptr;
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
	 *   work), only while the family is set up, on (its class's toggle) and DCLF draws (not before its first covered frame), handed the
	 *   frame with the scene's tables: whether the family's draws are DCLF's this frame, which Core publishes (IndirectDraws::DrawFamilyOwned; then
	 *   PassCapture withholds the engine's passes they replace); else they are not. Its pipelines are Core's lane's
	 *   (DrawFamilyFrame::Pipelines).
	 * - frame.programSource: render thread, from DrawFamilyFrame::Pipelines: the engine shader its slot's pipelines are built from (null:
	 *   none yet), posted to the pipeline lane.
	 * - main.declare, main.revision, main.record: the main pass's executions (z.depth, depth-phase2, main-opaque), in the graph's build
	 *   (declare: its arguments and its draw row's address, fixed with its tables), each preparation (revision: what moves the recording
	 *   besides the slot's pipelines' words Core hands it; Core prepares the slot's pipelines and the row while the family is set up),
	 *   and each recording (record, only once the slot's pipelines are published: after the depth draws in the depth passes; in the
	 *   colour pass after the main draws, under the Z-prepass layout and colourRange).
	 * - faces.declare, faces.record, faces.row, faces.lists (optional, all or none): a trees view's faces (Api::ViewShape::trees): the
	 *   faces' draw pass's declarations and its draw after each face's buckets (record, for a face whose shape has the family's
	 *   pipeline), each epoch commit's row (row: the row the faces' latch takes, its texture held; true when the faces draw it; and the
	 *   faces' decision, without owners), and the units the faces' lists hold with the family's tables (lists: Core's view machinery's
	 *   lists, sized, required and counted with them). The lists' cull is the feature's own pass (Api::PassDef), which the schedule names
	 *   in the views' depth epochs.
	 * - program, engineShapes: what its slot's pipelines are built of, and the engine shapes its draws replace (read once at install).
	 * - faces' data (with its callbacks, all or none): rowName and listName (a face's row and list resources, the face's index
	 *   appended); rowBytes and headerBytes (a face's row and its list's header in the face's latch values), unitWords (a list's words
	 *   per unit its tables hold), cullGroup (its cull's thread group); root: the engine root a trees view's faces draw in the engine's
	 *   place while they draw the family (Api::ViewRoot, one owner). face: render thread, per face of each trees view's epoch commit: the
	 *   face's row and list header from the commit's row (Core's DrawFamilyFaceWrite).
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
			void (*declare)(DrawFamilyDeclare& a_declare) = nullptr;
			void (*record)(const DrawFamilyRecord& a_record) = nullptr;
			bool (*row)(DrawFamilyFaceRow& a_row) = nullptr;
			void (*lists)(DrawFamilyLists& a_lists) = nullptr;
			std::string_view rowName, listName;
			std::uint32_t rowBytes = 0, headerBytes = 0, unitWords = 0, cullGroup = 0;
			ViewRoot root = ViewRoot::Count;
			void (*face)(DrawFamilyFaceWrite& a_face) = nullptr;
		} faces;
		DrawFamilyProgram program;
		DrawFamilyEngineShapes engineShapes;
	};

	/**
	 * @brief Core's fixed slot per program family (Core/Gpu/Pipelines/ProgramFamilySlots.h): a material port's shader drawn by the main
	 * pipeline set beside Lighting's keys, whichever feature takes it - its key bit in the raster flags (KeyFamily), the three spare key
	 * bits it may use, its bit in the stage cache's key and in its programs' ids, its Z key's salt and its pipeline counters.
	 */
	enum class ProgramFamily : std::uint8_t
	{
		// The Effect port's keys (BSEffectShader; Effect.hlsl): key bit 17, spare bits 21-23, stage-key bit 37, program-id bit 63.
		Effect,
		Count,
	};

	/** @brief The stages a family's programs are built of. */
	enum class ProgramStages : std::uint8_t
	{
		VertexPixel,  // its source's vertex and pixel stages alone, never pulled
	};

	/** @brief How a family's pipelines are built from its program (Core's named rules). */
	enum class ProgramBuild : std::uint8_t
	{
		// A forward key's or a decal group's (a decal draws the DEFERRED pixel stage): its depth variant its colour stages, never a depth
		// segment input (no Z-prepass bucket) and its Z key never shared.
		ForwardOrDecal,
	};

	/**
	 * @brief A program family (Api::InstallContext::RegisterProgramFamily): a material port's keys (Api/ObjectContributors.h, section 4:
	 * the port with shaderType) drawn by the main pipeline set at Core's slot for the family. What Core reads per object per frame is
	 * resolved once at install, as data (Extensions::FamilyRules); programSource is called only by the pipeline lane's render-thread side.
	 *
	 * - family: the slot. shaderType: its port's engine shader (RE::BSShader::Type), the source of its programs' defines.
	 * - name: the logs' ("Effect": "SPIR-V build of Effect ..."); keyName: its keys' and recipes' ("effect": "dclf.effect...", "effect
	 *   color"); sourcePath: its HLSL ("Data/Shaders/Effect.hlsl"), loaded with Core's sources; nativeNote: what stays native without it
	 *   ("effect meshes stay native").
	 * - stages, build: its programs' stages and its pipelines' rule.
	 * - rowBlocks: false: its pipeline rows' technique and geometry blocks stay zero (its variants read the frame's and the object's).
	 * - rootViewsForwardOnly: a category root's view (first person) draws its keys only when forward (a decal stays the engine's).
	 * - turnedKeyBits: a billboard-turned leaf's key carries the billboard mode + 1 in the family's spare bits (Coordinator::BillboardModeOf).
	 * - records: the object record bits its keys set (BindlessObject::recordFlags): suppressEmittance for an object with External
	 *   Emittance suppressed; turned and the mode at turnedModeShift for a turned leaf; alphaRefPassBits: the pass descriptor bits whose
	 *   keys keep the alpha property's threshold as their reference whatever the alpha test.
	 * - programSource: render thread, each time the lane's inputs are posted: the engine shader its programs are built from (null: none
	 *   yet).
	 */
	struct ProgramFamilyContribution
	{
		ProgramFamily family = ProgramFamily::Count;
		std::uint8_t shaderType = 0xFF;
		std::string_view name, keyName, sourcePath, nativeNote;
		ProgramStages stages = ProgramStages::VertexPixel;
		ProgramBuild build = ProgramBuild::ForwardOrDecal;
		bool rowBlocks = true;
		bool rootViewsForwardOnly = false;
		bool turnedKeyBits = false;
		struct Records
		{
			std::uint32_t suppressEmittance = 0, turned = 0, turnedModeShift = 0, alphaRefPassBits = 0;
		} records;
		const void* (*programSource)() = nullptr;
	};
}
