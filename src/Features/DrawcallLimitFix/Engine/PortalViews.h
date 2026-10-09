#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class NiAVObject;
	class NiNode;
}

namespace DCLF
{
	/**
	 * @brief The main camera's portal and occluder visibility as DCLF computes it (T5a, 2026-10-09): a port of the engine's walk
	 * (FUN_1401a43c0 -> Traverse FUN_1414a41e0) and of the BSCompoundFrustum builders it uses, from the main camera and the
	 * portal graph's rooms, portals and occluders (skyrim-engine-notes.md, "Portals, rooms and occluders").
	 *
	 * The engine runs it in Main::Update into the portal-graph entry the scene lists' processes share: per visible room its
	 * compound frustum (the room's children are culled through it, BSMultiBoundRoom::OnVisible; a room not reached is not culled
	 * into at all), and one for unbound space (the graph's occluders; every list's copy). DCLF builds the same programs into its
	 * own Frame, with its own scratch for what the engine keeps on the shapes (their corners, edge flags, per-frame states, the
	 * boxes' silhouette sets): nothing is written to an engine object, and nothing the engine's culls wrote is read.
	 *
	 * Render thread, right after Main::Update's walk (T5b: from the camera it walked from, before the scene lists are built and
	 * culled; PrimaryCull's PortalWalk hook).
	 */
	class PortalViews
	{
	public:
		/** @brief NiFrustumPlanes' layout: six planes {normal, d} (dist = dot(n, p) - d, inside >= 0), the active mask, the base mask. */
		struct PlaneSet
		{
			std::array<std::array<float, 4>, 6> plane{};
			std::uint32_t mask = 0;
			std::uint32_t base = 0x3F;
			std::uint32_t extra[2]{};
		};
		static_assert(sizeof(PlaneSet) == 0x70);

		/** @brief A BSCompoundFrustum's operator record: the token (4 AND, 5 OR, 6 close, 7/8 a test whose next record is its set), then the branches. */
		struct Op
		{
			std::uint32_t word = 0, yes = 0, no = 0;
		};

		/** @brief A compound frustum: the token stream, finalized into a branch program (Finalize), and its plane sets. */
		struct Program
		{
			std::vector<Op> ops;          // freeOp tokens, then (finalized) the root, ACCEPT and REJECT records
			std::vector<PlaneSet> sets;   // freePlane in use
			std::uint32_t freeOp = 0, freePlane = 0, firstOp = 0;
			bool skipView = false, finalized = false;
			std::array<float, 3> eye{};  // the camera's position (Reset)
		};

		struct Frame
		{
			std::uint32_t sceneFrame = ~0u;
			const void* graph = nullptr;    // the ShadowSceneNode's portal graph (+0x228); null: nothing built (the engine keeps its map)
			bool valid = false;
			bool visibleUnbound = false;    // a walk reached unbound space
			Program unbound;                // the portal-graph entry's own (+0x68): unbound space
			std::vector<const void*> roomKeys;  // the visibility map's rooms, as inserted
			std::vector<Program> rooms;          // by roomKeys
			std::vector<const void*> accumulated;  // the rooms the walk popped (multiBoundRoomAccumList), in order
			const void* cameraRoom = nullptr;      // the first room holding the camera's near-plane centre
		};

		static PortalViews& Get();

		/** @brief Render thread, after Main::Update's walk: this frame's programs (and, under the parities, the comparison). */
		void Build();
		const Frame& Current() const { return frame; }
		/**
		 * @brief Whether this frame's walk reached unbound space (the entry's visibleUnboundSpace): DrawWorld_BuildSceneLists'
		 * branch. Any thread (the lists' build job); a frame with no graph keeps the last walk's, as the engine's entry does.
		 */
		bool VisibleUnbound() const { return visibleUnbound.load(std::memory_order_acquire); }
		/** @brief The program of a_room this frame, or null (not reached: the engine does not cull into it). */
		const Program* RoomProgram(const void* a_room) const;
		/** @brief The parity's line (`<- PORTAL VIEW`); resets it. */
		void Report();

		/**
		 * @brief This frame's programs as FadeStateCS and BuildDraws read them (Records.h, kPortal*): the view planes, the directory
		 * (unbound space, then the graph's rooms in order) and the records. With no walk this frame, a header of no programs.
		 */
		void Encode(std::vector<std::uint32_t>& a_words) const;
		/** @brief This frame's Encode, made at Build: the GPU's copy and the CPU checks read the same words. */
		const std::vector<std::uint32_t>& Encoded() const { return encoded; }
		/**
		 * @brief PortalPrograms.hlsli's PortalTest on the CPU: program a_program of a_words for a bound (a_nodeFlags: the node's flags,
		 * always-draw bit 11, preprocessed 12, preprocess-hidden 20). 1 in view, 0 culled, -1 no program; a_why names the deciding test.
		 */
		static int Visible(const std::vector<std::uint32_t>& a_words, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags,
			std::string& a_why);
		/**
		 * @brief The program a node is culled through: 1 + its nearest BSMultiBoundRoom ancestor's index among the graph's rooms, 0
		 * (unbound space) with none, kPortalNoProgram with no graph. Render thread.
		 */
		std::uint32_t ProgramOf(const RE::NiAVObject* a_node) const;
		/** @brief Changes when ProgramOf's answers may have (a room's child attached or detached, another graph). */
		std::uint64_t ProgramsVersion() const { return structure.load(std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull ^ graphVersion; }
		/** @brief Any thread, the attach and detach detours: a child of a room changed. */
		void NoteStructure(const RE::NiNode* a_parent);

		/**
		 * @brief A shadow light's rooms, as its own portal-graph entry holds them (T5a3): FUN_1414a2530 -> FUN_1414a6400 ported.
		 * Kept per light by the caller: a light the engine does not walk keeps its entry as it was (the constructor's: unbound
		 * space seen, no rooms).
		 * - No graph, or a graph with no rooms; the NiLight hidden or its fade under 0.05; a light on an object node (BSLight
		 *   +0x130): not walked.
		 * - Not portal-strict (+0x47): unbound space seen, and the graph's rooms whose bound meets the light's sphere
		 *   (CheckBound2, vfunc 0x41), in the graph's order.
		 * - Portal-strict: Traverse with frustums (a shadow light) from every room holding the light's camera's near point, each
		 *   as a first walk, else from unbound space. The camera is the light's room process's (+0x128 -> +0x18: a shadow light's
		 *   own orthographic one, its frustum, viewport and world-to-camera matrix read as set up; the NiCamera constructor's
		 *   before the engine has made one) at the light with the NiLight's rotation; no view planes; the portals' view test
		 *   BSParabolicCullingProcess's (the light's sphere). Its occluder states and boxes are this frame's
		 *   main walk's, as the engine's are; its edge flags start from the shapes' and stay theirs only for a kept light
		 *   (CommitLightWalk). +0x30200 (set on a shadow light's process) is read with the camera.
		 */
		struct LightRooms
		{
			bool visibleUnbound = true;      // +0x130
			std::vector<const void*> rooms;  // multiBoundRoomAccumList (+0x18)
			std::uint32_t walk = 0;          // how the last call left it: LightWalk
		};
		enum LightWalk : std::uint32_t
		{
			kLightUntouched,
			kLightBounds,
			kLightWalked,
		};
		/** @brief Render thread, after Build this frame: a_light's entry, as FUN_1414a2530 would leave it now. */
		void WalkLight(const void* a_light, LightRooms& a_rooms);
		/** @brief The last WalkLight's light is kept: its walk's edge flags become the shapes' (the engine walks only a kept light). */
		void CommitLightWalk();
		/** @brief FUN_140e14300 (the light's entry, the world camera's): both see unbound space, or a room in common. */
		bool SharesRoom(const LightRooms& a_light) const;
		/**
		 * @brief The parity's: whether a light's room process (+0x128) holds what WalkLight assumes of it (its
		 * portal test BSParabolicCullingProcess's, no view planes and plane 0 zero); a_why names the first that does not.
		 */
		bool LightProcessAsAssumed(const void* a_roomProcess, std::string& a_why) const;

	private:
		PortalViews() = default;

		struct Vec3
		{
			float x = 0.0f, y = 0.0f, z = 0.0f;
		};
		using Quad = std::array<Vec3, 4>;
		using Flags = std::array<std::uint8_t, 4>;
		/** @brief What the engine keeps on an occlusion box while its state is 1 (BoxSilhouette's sets, hull and face slots). */
		struct BoxScratch
		{
			PlaneSet set[2];
			std::array<std::uint32_t, 8> hull{};
			std::uint32_t hullCount = 0;
			std::array<std::int32_t, 6> face{ -1, -1, -1, -1, -1, -1 };
		};
		struct Camera
		{
			const std::byte* camera = nullptr;  // the NiCamera
			Vec3 eye;
			PlaneSet view;                       // NiCullingProcess::SetFrustum's planes
			PlaneSet relative;                   // FUN_140e16f70's camera-relative copy (the boxes' refinement)
		};

		// The walk (FUN_1414a41e0) and its builders.
		void Walk(const void* a_graph, const void* a_startRoom, bool a_isFirst);
		void BuildRoom(const void* a_room, std::uint32_t a_parent, bool a_parentUnbound, const void* a_portal, const void* a_startRoom);
		void BuildUnbound(std::uint32_t a_parent, bool a_parentUnbound, const void* a_portal, const void* a_graph);
		Program& ProgramAt(std::uint32_t a_index, bool a_unbound) { return a_unbound ? frame.unbound : frame.rooms[a_index]; }
		std::int32_t FindRoom(const void* a_room) const;
		std::uint32_t AddRoom(const void* a_room);

		void Reset(Program& a_program) const;
		static void Emit(Program& a_program, std::uint32_t a_word);
		static void EmitTest(Program& a_program, std::uint32_t a_type, std::uint32_t a_set);
		static void Finalize(Program& a_program);
		static void ReopenAsOr(Program& a_program);
		static void CompactEmptyGroups(Program& a_program);
		static void Merge(Program& a_destination, const Program& a_source);
		void CopyMasked(Program& a_out, const Program& a_parent, const void* a_portal);
		void AddPortal(Program& a_program, const void* a_portal);
		void AddOccluderTop(Program& a_program, const void* a_occluder, bool& a_needOpen);
		void AddOccluderRoom(Program& a_program, const void* a_occluder, const Program* a_parent);
		bool AddBox(Program& a_program, const void* a_box);
		void AddPlane(Program& a_program, const void* a_plane);

		// The shapes' tests.
		static Quad Corners(const void* a_quad);
		static PlaneSet& Grow(Program& a_program, std::uint32_t a_index);
		PlaneSet PortalPlanes(const void* a_quad, const Vec3& a_eye);
		bool TestQuadProgram(const Program& a_program, const void* a_quad);
		bool TestShapeProgram(const Program& a_program, const void* a_shape) const;
		static bool QuadVsSet(const PlaneSet& a_set, const Quad& a_quad, Flags& a_flags);
		static bool AllCornersInside(const Quad& a_quad, const PlaneSet& a_set);
		bool PortalVsSetEdges(const void* a_portal, const PlaneSet& a_set);
		bool WithinFrustumDistFirst(const void* a_quad, const PlaneSet& a_planes, const Vec3& a_eye);
		bool BoxInView(const void* a_box) const;
		bool BoxSilhouette(const void* a_box, BoxScratch& a_scratch);
		bool BoxRefine(const void* a_box, BoxScratch& a_scratch) const;

		/** @brief Under the parities: this frame's programs against the engine's portal-graph entry (before the list jobs). */
		void CheckParity();

		Frame frame;
		Camera camera;
		// DCLF's copies of what the engine keeps on the shapes: the edge flags (last test wins, across frames, as the engine's),
		// the per-frame states (0 untested, 1 accepted, 2 rejected: FUN_1401a4a00 resets them before each walk) and box sets.
		ankerl::unordered_dense::map<const void*, Flags> edgeFlags;
		ankerl::unordered_dense::map<const void*, std::uint32_t> states;
		ankerl::unordered_dense::map<const void*, BoxScratch> boxes;
		ankerl::unordered_dense::set<const void*> crossed;  // portals keyed this frame (the map's portal-shared-node entries)
		Program temp;                                       // the engine's temp frustum (0x14332a710)
		const void* edgeGraph = nullptr;                    // the graph edgeFlags are for
		ankerl::unordered_dense::map<const void*, std::uint32_t> roomIndex;  // the graph's rooms, by their index (ProgramOf)
		std::uint64_t graphVersion = 0;                     // bumped with every new graph
		std::atomic<std::uint64_t> structure{ 0 };          // NoteStructure
		std::vector<std::uint32_t> encoded;                 // Encoded
		std::atomic<bool> visibleUnbound{ true };           // VisibleUnbound (the entry's constructor's)

		/** @brief BSParabolicCullingProcess's view test (TestBaseVisibility3, 0x14151a050) for a light's walk. */
		struct Parabolic
		{
			Vec3 centre;
			float radius = 0.0f;
			std::array<float, 4> plane{};  // through the light, along its camera's forward (FUN_141519d20)
			bool bothSides = false;        // +0x30200 set
		};
		static bool ParabolicVisible(const Parabolic& a_parabolic, const void* a_portal);
		const Parabolic* parabolic = nullptr;  // set during a light's walk
		// A light's walk's own frame, camera and crossed portals (swapped in), and its edge flags.
		Frame lightFrame;
		ankerl::unordered_dense::set<const void*> lightCrossed;
		ankerl::unordered_dense::map<const void*, Flags> lightEdgeFlags;  // the last light walk's copy of edgeFlags
		bool lightFlagsPending = false;
		std::array<std::byte, 0x188> lightCamera{};

		struct Parity
		{
			std::uint64_t frames = 0, rooms = 0, roomsMissing = 0, roomsExtra = 0, programs = 0, programsDiffer = 0;
			std::uint64_t unboundDiffer = 0, flagDiffer = 0, accumDiffer = 0;
			std::uint64_t ops = 0, sets = 0, boxes = 0, planes = 0, portals = 0;  // what the built programs held
			float largest = 0.0f;  // the largest plane difference among the programs that matched in structure
			std::uint64_t eyeMoved = 0, movedDiffer = 0;  // frames whose camera moved after the engine's walk, and their differences
			float largestMove = 0.0f;
			std::uint64_t turned = 0, boxChecks = 0, boxCornersDiffer = 0, boxSetsDiffer = 0;
			float largestCorner = 0.0f;
			std::string boxFirst;
			std::uint64_t planesDiffer = 0;
			std::string planesFirst;
			std::string occluderFirst;
			std::string first;
		};
		Parity parity;
		std::uint32_t reportFrames = 0;
		std::uint64_t built = 0, skipped = 0;
	};
}
