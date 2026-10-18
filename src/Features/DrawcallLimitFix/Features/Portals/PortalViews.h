#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

namespace RE
{
	class NiAVObject;
	class NiNode;
}

namespace DCLF::Diagnostics
{
	struct PortalViewsView;
}

namespace DCLF
{
	/**
	 * @brief The main camera's portal and occluder visibility: a port of the engine's walk (FUN_1401a43c0 -> Traverse FUN_1414a41e0)
	 * and its BSCompoundFrustum builders (skyrim-engine-notes.md, "Portals, rooms and occluders"). The engine builds, in Main::Update,
	 * a compound frustum per visible room (its children are culled through it; an unreached room is not culled into) and one for
	 * unbound space. DCLF builds the same programs with its own copies of the shapes' scratch: it writes no engine object and reads
	 * nothing the engine's culls wrote. Render thread, right after Main::Update's walk (the feature's PortalWalk thunk, PortalsFeature.cpp).
	 */
	class PortalViews
	{
	public:
		/** @brief NiFrustumPlanes' layout: six planes {n, d} (dot(n, p) - d >= 0 inside), active mask, base mask. */
		struct PlaneSet
		{
			std::array<std::array<float, 4>, 6> plane{};
			std::uint32_t mask = 0;
			std::uint32_t base = 0x3F;
			std::uint32_t extra[2]{};
		};
		static_assert(sizeof(PlaneSet) == 0x70);

		/** @brief A BSCompoundFrustum operator record: token (4 AND, 5 OR, 6 close, 7/8 a test whose next record is its set), branches. */
		struct Op
		{
			std::uint32_t word = 0, yes = 0, no = 0;
		};

		/** @brief A compound frustum: the token stream (Finalize makes it a branch program) and its plane sets. */
		struct Program
		{
			std::vector<Op> ops;          // freeOp tokens, then (finalized) root, ACCEPT and REJECT
			std::vector<PlaneSet> sets;   // freePlane in use
			std::uint32_t freeOp = 0, freePlane = 0, firstOp = 0;
			bool skipView = false, finalized = false;
			std::array<float, 3> eye{};  // the camera's position (Reset)
		};

		struct Frame
		{
			std::uint32_t sceneFrame = ~0u;
			const void* graph = nullptr;    // the ShadowSceneNode's portal graph (+0x228); null: nothing built
			bool valid = false;
			bool visibleUnbound = false;    // a walk reached unbound space
			Program unbound;                // the portal-graph entry's own (+0x68): unbound space
			std::vector<const void*> roomKeys;  // the visibility map's rooms, as inserted
			std::vector<Program> rooms;          // by roomKeys
			std::vector<const void*> accumulated;  // rooms the walk popped (multiBoundRoomAccumList), in order
			const void* cameraRoom = nullptr;      // first room holding the camera's near-plane centre
		};

		static PortalViews& Get();

		/** @brief Render thread, after Main::Update's walk: this frame's programs. */
		void Build();
		const Frame& Current() const { return frame; }
		/**
		 * @brief Whether this frame's walk reached unbound space (visibleUnboundSpace; DrawWorld_BuildSceneLists' branch). Any thread; a
		 * frame with no graph keeps the last walk's, as the engine does.
		 */
		bool VisibleUnbound() const { return visibleUnbound.load(std::memory_order_acquire); }
		/** @brief a_room's program this frame, or null (not reached: not culled into). */
		const Program* RoomProgram(const void* a_room) const;
		/** @brief Portal tests, occlusion planes and boxes the built programs held. */
		struct ProgramCounts
		{
			std::uint64_t portals = 0, planes = 0, boxes = 0;
		};
		/** @brief Walks since the last Take: built, skipped (no graph or camera), and their programs' contents. */
		struct BuildCounts
		{
			std::uint64_t built = 0, skipped = 0;
			ProgramCounts programs;
		};
		BuildCounts TakeBuildCounts();

		/**
		 * @brief This frame's programs as FadeStateCS and BuildDraws read them (PortalPrograms.h, kPortal*): view planes, directory (unbound
		 * space, then rooms in order) and records; a header of no programs without a walk.
		 */
		void Encode(std::vector<std::uint32_t>& a_words) const;
		/** @brief This frame's Encode, made at Build (the GPU copy and CPU checks read the same words). */
		const std::vector<std::uint32_t>& Encoded() const { return encoded; }
		/**
		 * @brief PortalPrograms.hlsli's PortalTest on the CPU (a_nodeFlags: always-draw bit 11, preprocessed 12, preprocess-hidden 20).
		 * 1 in view, 0 culled, -1 no program; a_why names the deciding test.
		 */
		static int Visible(std::span<const std::uint32_t> a_words, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags,
			std::string& a_why);
		/**
		 * @brief The program a node is culled through: 1 + its nearest BSMultiBoundRoom ancestor's room index, 0 (unbound space) with
		 * none, kPortalNoProgram with no graph. Render thread.
		 */
		std::uint32_t ProgramOf(const RE::NiAVObject* a_node) const;
		/** @brief Changes when ProgramOf's answers may have (room child attached/detached, new graph). */
		std::uint64_t ProgramsVersion() const { return structure.load(std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull ^ graphVersion; }
		/** @brief Any thread (attach/detach detours): a room's child changed. */
		void NoteStructure(const RE::NiNode* a_parent);

		/**
		 * @brief What the main view sees as Main::IsRoomVisible's (0x140649f20) keys: the walk's rooms, and the portal shared nodes the
		 * engine's entry holds after the scene lists (crossed portals' and every portal of a popped room: DrawWorld_BuildSceneLists'
		 * AddVisibilityToMap). Immutable once published; built at Build only after PublishVisibleSets.
		 */
		struct VisibleSet
		{
			/** @brief Per graph: its rooms, and each portal's shared node with the portal's two rooms. */
			struct GraphKeys
			{
				const void* graph = nullptr;
				std::uint64_t signature = 0;  // the room and portal counts it was built from
				ankerl::unordered_dense::set<const void*> rooms;
				ankerl::unordered_dense::map<const void*, std::array<const void*, 2>> shared;  // a null room: unbound space
			};
			struct Keys
			{
				ankerl::unordered_dense::set<const void*> rooms;
				ankerl::unordered_dense::set<const void*> shared;
			};
			enum Kind : std::uint8_t
			{
				kNoKey,  // a null key: true
				kRoom,
				kShared,
				kOther,  // not a room or shared node (or no graph): true, as the engine's
			};
			struct Answer
			{
				bool visible = true;
				Kind kind = kNoKey;
			};

			std::uint64_t generation = 0;
			std::uint32_t sceneFrame = ~0u;
			std::shared_ptr<const GraphKeys> graph;  // null: no walk this frame
			std::uint32_t graphAge = 0;              // publishes since the graph last changed
			Keys current;
			Keys previous;  // the previous set's current (for observers' frame alignment)

			/**
			 * @brief Main::IsRoomVisible on keys alone: a room when walked; a shared node when held or either of its portal's rooms is.
			 * a_previous: from the previous walk's keys.
			 */
			Answer Visible(const void* a_key, bool a_previous = false) const;
		};

		/**
		 * @brief A lock-free hold on the newest VisibleSet (reader count + atomic pointer; MSVC's atomic shared_ptr locks). Any thread;
		 * not freed while held; null before the first publish.
		 */
		class VisibleSetHold
		{
		public:
			VisibleSetHold();
			~VisibleSetHold();
			VisibleSetHold(const VisibleSetHold&) = delete;
			VisibleSetHold& operator=(const VisibleSetHold&) = delete;
			const VisibleSet* get() const { return set; }
			const VisibleSet* operator->() const { return set; }
			explicit operator bool() const { return set != nullptr; }

		private:
			const VisibleSet* set = nullptr;
		};
		/** @brief From now on Build publishes a VisibleSet every frame (RoomVisibility's install). */
		void PublishVisibleSets() { publishVisible.store(true, std::memory_order_release); }
		/** @brief The newest set's generation (any thread): an older hold raced a publish. */
		std::uint64_t VisibleGeneration() const { return visibleGeneration.load(std::memory_order_acquire); }

		/**
		 * @brief A shadow light's rooms as its own portal-graph entry holds them (FUN_1414a2530 -> FUN_1414a6400 ported; the cases are
		 * in dclf-architecture.md, "PortalViews::WalkLight"). Kept per light by the caller; an unwalked light keeps its entry. The
		 * camera is the light's room process's (+0x128 -> +0x18, or the NiCamera constructor's before one exists); +0x30200 (a shadow
		 * light's process) is read with it. Occluder states and boxes are the main walk's; edge flags persist only for a kept light.
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
		/** @brief Render thread, after Build: a_light's entry as FUN_1414a2530 would leave it this frame. */
		void WalkLight(const void* a_light, LightRooms& a_rooms);
		/** @brief The last WalkLight's light is kept: its edge flags become the shapes' (the engine walks only kept lights). */
		void CommitLightWalk();
		/** @brief FUN_140e14300 (the light's entry, the world camera's): both see unbound space, or a room in common. */
		bool SharesRoom(const LightRooms& a_light) const;
		/**
		 * @brief Whether a light's room process (+0x128) is what WalkLight assumes (BSParabolicCullingProcess portal test, no view
		 * planes, plane 0 zero); a_why names the first mismatch.
		 */
		bool LightProcessAsAssumed(const void* a_roomProcess, std::string& a_why) const;

	private:
		PortalViews() = default;
		friend struct Diagnostics::PortalViewsView;

		struct Vec3
		{
			float x = 0.0f, y = 0.0f, z = 0.0f;
		};
		using Quad = std::array<Vec3, 4>;
		using Flags = std::array<std::uint8_t, 4>;
		/** @brief The engine's scratch on an occlusion box while its state is 1 (BoxSilhouette's sets, hull, face slots). */
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
			PlaneSet relative;                   // FUN_140e16f70's camera-relative copy (box refinement)
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

		/** @brief The end of every Build: the encoded words and, after PublishVisibleSets, the VisibleSet and its report. */
		void AfterBuild();
		void PublishVisible();

		Frame frame;
		Camera camera;
		// DCLF's copies of the engine's shape scratch: edge flags (last test wins, across frames), per-frame states (0 untested,
		// 1 accepted, 2 rejected: FUN_1401a4a00 resets them before each walk) and box sets.
		ankerl::unordered_dense::map<const void*, Flags> edgeFlags;
		ankerl::unordered_dense::map<const void*, std::uint32_t> states;
		ankerl::unordered_dense::map<const void*, BoxScratch> boxes;
		ankerl::unordered_dense::set<const void*> crossed;  // portals keyed this frame
		Program temp;                                       // the engine's temp frustum (0x14332a710)
		const void* edgeGraph = nullptr;                    // the graph edgeFlags are for
		ankerl::unordered_dense::map<const void*, std::uint32_t> roomIndex;  // room -> index (ProgramOf)
		std::uint64_t graphVersion = 0;                     // bumped with every new graph
		std::atomic<std::uint64_t> structure{ 0 };          // NoteStructure
		std::vector<std::uint32_t> encoded;                 // Encoded
		std::atomic<bool> visibleUnbound{ true };           // VisibleUnbound (the engine entry's initial value)

		// The published VisibleSet and replaced ones, freed at a publish that sees no reader (readers count themselves before loading).
		std::atomic<bool> publishVisible{ false };
		std::atomic<const VisibleSet*> visibleSet{ nullptr };
		std::atomic<std::uint32_t> visibleReaders{ 0 };
		std::atomic<std::uint64_t> visibleGeneration{ 0 };
		std::shared_ptr<const VisibleSet> visibleOwned;
		std::vector<std::shared_ptr<const VisibleSet>> visibleRetired;
		std::shared_ptr<const VisibleSet::GraphKeys> graphKeys;  // reused while the graph's counts hold
		Reports::Window visibleReports;  // RoomVisibility's line every 300 sets

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
		// A light walk's own frame, camera, crossed portals (swapped in) and edge flags.
		Frame lightFrame;
		ankerl::unordered_dense::set<const void*> lightCrossed;
		ankerl::unordered_dense::map<const void*, Flags> lightEdgeFlags;  // the last light walk's copy of edgeFlags
		bool lightFlagsPending = false;
		std::array<std::byte, 0x188> lightCamera{};

		ProgramCounts programCounts;
		std::uint64_t built = 0, skipped = 0;
	};
}
