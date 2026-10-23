// The portal views' parity (PortalParity.h): at PortalViews::Build's end, render thread.

#include "PortalParity.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "PortalViewsView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalShapes.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

namespace DCLF::Diagnostics::PortalParity
{
	namespace
	{
		using namespace PortalShapes;
		using Engine::Global;
		using View = PortalViewsView;
		using PlaneSet = PortalViews::PlaneSet;
		using Op = PortalViews::Op;
		using Program = PortalViews::Program;
		using Flags = std::array<std::uint8_t, 4>;

		// BSCullingProcess and its BSPortalGraphEntry, BSCompoundFrustum: the engine's entry the programs are compared with.
		constexpr std::size_t kProcessEntry = 0x30190;
		constexpr std::size_t kEntryAccum = 0x18, kEntryAccumSize = 0x28, kEntryMapCapacity = 0x44, kEntryMapEntries = 0x60,
							  kEntryUnbound = 0x68, kEntryVisibleUnbound = 0x130;
		constexpr std::size_t kFrustumSets = 0x0, kFrustumOps = 0x18, kFrustumFreePlane = 0xB8, kFrustumFreeOp = 0xBC,
							  kFrustumFirstOp = 0xC0, kFrustumSkipView = 0xC4, kFrustumFinalized = 0xC5;

		Tally tally;
		Reports::Window window;

		/** @brief A check's shared state: the tally, this frame's walk and camera, the engine's entry, and whether the camera moved since. */
		struct Walk
		{
			Tally& p;
			const PortalViews::Frame& frame;
			decltype(View::Camera()) camera;
			const std::byte* entry = nullptr;
			bool eyeMoved = false;
		};

		/** @brief A difference: on a frame whose camera moved after the engine's walk, counted apart; else the first one kept. */
		void Note(Walk& a_walk, std::string a_what)
		{
			auto& p = a_walk.p;
			if (a_walk.eyeMoved) {
				++p.movedDiffer;
				return;
			}
			if (p.first.empty())
				p.first = std::move(a_what);
		}

		std::string Name(const void* a_room)
		{
			const auto* object = static_cast<const RE::NiAVObject*>(a_room);
			return a_room ? fmt::format("'{}'", object->name.c_str() ? object->name.c_str() : "") : std::string("unbound space");
		}

		/**
		 * @brief Whether the camera moved after the engine's walk (its eye, or its view planes: turned), with the list process's view planes
		 * against DCLF's tallied.
		 */
		bool CameraMoved(Tally& p, const std::byte* a_listProcess, const std::byte* a_entry, decltype(View::Camera()) camera)
		{
			// The eye the engine's walk measured from (its unbound frustum's, Reset at the walk's start: +0xA0): Main::Update's camera,
			// which the frame's draw may since have moved. A frame whose eye differs is tallied apart: its programs are a later camera's.
			const float* engineEye = &At<float>(a_entry + kEntryUnbound, 0xA0);
			const float moved = std::max({ std::abs(engineEye[0] - camera.eye.x), std::abs(engineEye[1] - camera.eye.y), std::abs(engineEye[2] - camera.eye.z) });
			// And the camera's world view planes it Reset with (+0x30): a camera turned since gives other side planes, eye unmoved.
			const auto& engineView = At<const PlaneSet>(a_entry + kEntryUnbound, 0x30);
			bool turned = false;
			for (std::uint32_t q = 0; q < 6 && !turned; ++q)
				for (std::uint32_t c = 0; c < 4; ++c)
					turned = turned || engineView.plane[q][c] != camera.view.plane[q][c];
			p.turned += turned ? 1 : 0;
			// The list process's own view planes (+0x3C, SetFrustum before the jobs), which its sphere test reads: DCLF's must be them.
			{
				const auto& listPlanes = At<const PlaneSet>(a_listProcess, 0x3C);
				bool differ = listPlanes.mask != camera.view.mask;
				for (std::uint32_t q = 0; q < 6 && !differ; ++q)
					for (std::uint32_t c = 0; c < 4; ++c)
						differ = differ || listPlanes.plane[q][c] != camera.view.plane[q][c];
				if (differ && p.planesDiffer++ == 0)
					p.planesFirst = fmt::format("mask {:#x} (the list's {:#x}), plane 0 ({} {} {} {}) against ({} {} {} {})", camera.view.mask, listPlanes.mask, camera.view.plane[0][0],
						camera.view.plane[0][1], camera.view.plane[0][2], camera.view.plane[0][3], listPlanes.plane[0][0], listPlanes.plane[0][1], listPlanes.plane[0][2],
						listPlanes.plane[0][3]);
			}
			const bool eyeMoved = moved != 0.0f || turned;
			p.eyeMoved += eyeMoved ? 1 : 0;
			p.largestMove = std::max(p.largestMove, moved);
			return eyeMoved;
		}

		/** @brief A program's token stream against an engine BSCompoundFrustum's (same counts): the first difference, or empty. */
		std::string CompareOps(const Program& a_ours, const std::byte* a_engine, std::uint32_t freeOp, bool a_tokensOnly)
		{
			const bool finalized = At<std::uint8_t>(a_engine, kFrustumFinalized) != 0;
			const auto* ops = At<const Op*>(a_engine, kFrustumOps);
			std::string why;
			const bool branches = !a_tokensOnly && finalized && a_ours.finalized;
			if (branches && At<std::uint32_t>(a_engine, kFrustumFirstOp) != a_ours.firstOp)
				why = fmt::format("first op {}, the engine's {}", a_ours.firstOp, At<std::uint32_t>(a_engine, kFrustumFirstOp));
			// The tokens (with the tests' set words), the tests' branches, and the root, ACCEPT and REJECT records after them.
			const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(branches ? freeOp + 3 : freeOp, a_ours.ops.size()));
			for (std::uint32_t i = 0; why.empty() && i < count; ++i) {
				const auto& a = a_ours.ops[i];
				const auto& b = ops[i];
				const bool test = IsTest(a.word) && i < freeOp;
				if (a.word != b.word || (branches && test && (a.yes != b.yes || a.no != b.no))) {
					why = fmt::format("op {}: {} -> {}/{}, the engine's {} -> {}/{}", i, a.word, a.yes, a.no, b.word, b.yes, b.no);
				} else if (test && i + 1 < freeOp) {
					++i;
					if (a_ours.ops[i].word != ops[i].word)
						why = fmt::format("op {}'s set {}, the engine's {}", i - 1, a_ours.ops[i].word, ops[i].word);
				}
			}
			return why;
		}

		/** @brief A program's active sets' planes against an engine BSCompoundFrustum's, within rounding: the difference, or empty. */
		std::string CompareSets(Tally& p, const Program& a_ours, const PlaneSet* sets, std::uint32_t freePlane)
		{
			std::string why;
			for (std::uint32_t s = 0; why.empty() && s < freePlane && s < a_ours.sets.size(); ++s) {
				const auto& a = a_ours.sets[s];
				const auto& b = sets[s];
				if (a.mask != b.mask) {
					why = fmt::format("set {} mask {:#x}, the engine's {:#x}", s, a.mask, b.mask);
					break;
				}
				for (std::uint32_t q = 0; q < 6; ++q) {
					if (!((a.mask >> q) & 1))
						continue;
					for (std::uint32_t c = 0; c < 4; ++c) {
						const float d = std::abs(a.plane[q][c] - b.plane[q][c]);
						if (!ShadowCameras::Close(a.plane[q][c], b.plane[q][c])) {
							why = fmt::format("set {} plane {} [{}] {}, the engine's {}", s, q, c, a.plane[q][c], b.plane[q][c]);
							break;
						}
						p.largest = std::max(p.largest, d);
					}
				}
			}
			return why;
		}

		/** @brief A program against an engine BSCompoundFrustum: the structure exactly, the planes of the active sets within rounding. */
		std::string ProgramDifference(Tally& p, const Program& a_ours, const std::byte* a_engine, bool a_tokensOnly)
		{
			const std::uint32_t freeOp = At<std::uint32_t>(a_engine, kFrustumFreeOp), freePlane = At<std::uint32_t>(a_engine, kFrustumFreePlane);
			const auto* ops = At<const Op*>(a_engine, kFrustumOps);
			const auto* sets = At<const PlaneSet*>(a_engine, kFrustumSets);
			std::string why;
			if (freeOp != a_ours.freeOp || freePlane != a_ours.freePlane)
				why = fmt::format("{} ops {} sets, the engine's {} ops {} sets", a_ours.freeOp, a_ours.freePlane, freeOp, freePlane);
			else if ((At<std::uint8_t>(a_engine, kFrustumSkipView) != 0) != a_ours.skipView)
				why = fmt::format("skipViewFrustum {}, the engine's {}", a_ours.skipView, !a_ours.skipView);
			if (why.empty() && freeOp && ops)
				why = CompareOps(a_ours, a_engine, freeOp, a_tokensOnly);
			if (why.empty() && freePlane && sets)
				why = CompareSets(p, a_ours, sets, freePlane);
			return why;
		}

		/**
		 * @brief One program against an engine BSCompoundFrustum. Unbound space's of a walk from unbound space that differs is built again as
		 * the engine's walk built it: DCLF judges every graph plane before the walk as the actor culler does (PlaneJudged), while the
		 * engine's other occluder tests (FUN_1414a8ba0 and its siblings: HighActorCuller::Operate, FUN_140524a30) judge them lazily after
		 * Main::Update's reset (FUN_1401a4a00), and its walk tests a plane they left untested with WithinFrustumDistFirst, whose rectangle
		 * bug takes planes wholly outside the view. Matching then, the difference is the engine's job order and that bug, not DCLF's port.
		 */
		void CompareProgram(Walk& a_walk, const Program& a_ours, const std::byte* a_engine, const void* a_room, bool a_tokensOnly)
		{
			auto& p = a_walk.p;
			++p.programs;
			p.ops += a_ours.freeOp;
			p.sets += a_ours.freePlane;
			const std::string why = ProgramDifference(p, a_ours, a_engine, a_tokensOnly);
			if (!why.empty()) {
				if (!a_room && !a_walk.frame.cameraRoom && a_walk.frame.graph &&
					ProgramDifference(p, View::UnboundWithEngineStates(a_walk.frame.graph), a_engine, a_tokensOnly).empty()) {
					p.unboundEngineStates += a_walk.eyeMoved ? 0 : 1;
					return;
				}
				if (!a_walk.eyeMoved)
					++(a_room ? p.programsDiffer : p.unboundDiffer);
				Note(a_walk, fmt::format("{}: {}", Name(a_room), why));
			}
		}

		/** @brief The rooms the engine's map holds (its other keys are the portals' shared nodes) against the rooms DCLF's walk reached. */
		void CompareRooms(Walk& a_walk)
		{
			auto& p = a_walk.p;
			const auto& portals = PortalViews::Get();
			const auto* entry = a_walk.entry;
			const auto* graph = a_walk.frame.graph;
			const auto* graphRooms = graph ? At<const void* const*>(graph, kGraphRooms) : nullptr;
			const std::uint32_t graphRoomCount = graphRooms ? At<std::uint32_t>(graph, kGraphRoomCount) : 0;
			const auto isRoom = [&](const void* a_key) { return a_key && std::find(graphRooms, graphRooms + graphRoomCount, a_key) != graphRooms + graphRoomCount; };
			ankerl::unordered_dense::set<const void*> engineRooms;
			const std::uint32_t capacity = At<std::uint32_t>(entry, kEntryMapCapacity);
			const auto* entries = At<const std::byte*>(entry, kEntryMapEntries);
			for (std::uint32_t i = 0; entries && i < capacity; ++i) {
				const auto* slot = entries + std::size_t(i) * 0x18;
				if (!At<const void*>(slot, 0x10))
					continue;
				const void* key = At<const void*>(slot, 0);
				if (!isRoom(key) || !engineRooms.insert(key).second)
					continue;
				++p.rooms;
				const auto* value = At<const std::byte*>(slot, 8);
				const auto* ours = portals.RoomProgram(key);
				if (!ours) {
					p.roomsMissing += a_walk.eyeMoved ? 0 : 1;
					Note(a_walk, fmt::format("{}: in the engine's map, not reached", Name(key)));
				} else if (value) {
					CompareProgram(a_walk, *ours, value, key, false);
				}
			}
			for (const auto* key : a_walk.frame.roomKeys)
				if (!engineRooms.contains(key)) {
					p.roomsExtra += a_walk.eyeMoved ? 0 : 1;
					Note(a_walk, fmt::format("{}: reached, not in the engine's map", Name(key)));
				}
		}

		/**
		 * @brief Unbound space's program against the entry's own; with another operator count, each graph plane occluder, with the engine's
		 * state and edge flags and DCLF's test of it again (WithinFrustumDistFirst's parts).
		 */
		void CompareUnbound(Walk& a_walk)
		{
			auto& p = a_walk.p;
			const auto& frame = a_walk.frame;
			const auto& camera = a_walk.camera;
			const auto& states = View::States();
			const auto* entry = a_walk.entry;
			const auto* graph = frame.graph;
			const std::uint64_t differed = p.unboundDiffer;
			CompareProgram(a_walk, frame.unbound, entry + kEntryUnbound, nullptr, true);
			if (p.unboundDiffer != differed && graph && frame.unbound.freeOp != At<std::uint32_t>(entry + kEntryUnbound, kFrustumFreeOp) && p.occluderFirst.empty()) {
				ForList(graph, kGraphOccluders, [&](const void* a_occluder) {
					if (!a_occluder || !IsPlane(a_occluder))
						return;
					const auto c = View::Corners(a_occluder);
					const auto pl = PlaneFromPoints(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
					const auto& e = camera.eye;
					const float dP = ((e.x * pl[0] + e.y * pl[1]) + e.z * pl[2]) - pl[3];
					const auto& n0 = camera.view.plane[0];
					const float dN = ((n0[0] * e.x + n0[1] * e.y) + n0[2] * e.z) - n0[3];
					Flags scratch{};
					const bool quad = View::QuadVsSet(camera.view, c, scratch);
					const auto st = states.find(a_occluder);
					p.occluderFirst += fmt::format("[plane {} engine state {} flags {:#x}, DCLF's state {}; dP {} dN {} quad {} half {}x{}] ", a_occluder, At<std::uint32_t>(a_occluder, 0x40),
						At<std::uint32_t>(a_occluder, 0xF0), st == states.end() ? -1 : static_cast<int>(st->second), dP, dN, quad, At<float>(a_occluder, kShapeHalfX),
						At<float>(a_occluder, kShapeHalfY));
				});
			}
		}

		/** @brief The boxes this frame used: their corners (+0x134) and silhouette sets (+0x54, +0xC4) as the engine keeps them, against DCLF's. */
		void TallyBoxes(Walk& a_walk)
		{
			auto& p = a_walk.p;
			const auto& states = View::States();
			for (const auto& [box, scratch] : View::Boxes()) {
				const auto corners = BoxCorners(box);
				float cornerMove = 0.0f;
				for (std::uint32_t i = 0; i < 8; ++i) {
					const float* engineCorner = &At<float>(box, 0x134 + 12 * i);
					cornerMove = std::max({ cornerMove, std::abs(engineCorner[0] - corners[i].x), std::abs(engineCorner[1] - corners[i].y), std::abs(engineCorner[2] - corners[i].z) });
				}
				++p.boxChecks;
				if (cornerMove != 0.0f) {
					++p.boxCornersDiffer;
					p.largestCorner = std::max(p.largestCorner, cornerMove);
				}
				const auto st = states.find(box);
				if (st == states.end() || st->second != 1 || a_walk.eyeMoved)
					continue;
				for (std::uint32_t s = 0; s < 2; ++s) {
					const auto& ours = scratch.set[s];
					const auto& theirs = At<const PlaneSet>(box, s ? 0xC4 : 0x54);
					std::string why;
					if (ours.mask != theirs.mask)
						why = fmt::format("mask {:#x}, the engine's {:#x}", ours.mask, theirs.mask);
					for (std::uint32_t q = 0; q < 6 && why.empty(); ++q)
						if ((ours.mask >> q) & 1)
							for (std::uint32_t c = 0; c < 4 && why.empty(); ++c)
								if (ours.plane[q][c] != theirs.plane[q][c])
									why = fmt::format("plane {} [{}] {}, the engine's {} (hull {}, corners moved {})", q, c, ours.plane[q][c], theirs.plane[q][c], scratch.hullCount,
										cornerMove);
					if (!why.empty()) {
						++p.boxSetsDiffer;
						if (p.boxFirst.empty())
							p.boxFirst = fmt::format("box {} set {}: {}", box, s, why);
					}
				}
			}
		}

		/** @brief The rooms the walk popped, in order, and whether it reached unbound space. */
		void CompareAccumulated(Walk& a_walk)
		{
			auto& p = a_walk.p;
			const auto& frame = a_walk.frame;
			const auto* entry = a_walk.entry;
			const auto* accum = At<const void* const*>(entry, kEntryAccum);
			const std::uint32_t accumSize = At<std::uint32_t>(entry, kEntryAccumSize);
			const bool sameAccum = accumSize == frame.accumulated.size() && (accumSize == 0 || std::equal(frame.accumulated.begin(), frame.accumulated.end(), accum));
			if (!sameAccum || (At<std::uint8_t>(entry, kEntryVisibleUnbound) != 0) != frame.visibleUnbound) {
				p.accumDiffer += a_walk.eyeMoved ? 0 : 1;
				Note(a_walk, fmt::format("the walk popped {} rooms (unbound {}), the engine's {} (unbound {})", frame.accumulated.size(), frame.visibleUnbound, accumSize,
					At<std::uint8_t>(entry, kEntryVisibleUnbound) != 0));
			}
		}

		void Check(const On<EnginePoint::PortalWalkBuilt>&)
		{
			// The engine's entry as Main::Update's walk left it (the list jobs, which add the portals' shared-node frustums, have not run).
			auto& p = tally;
			++p.frames;
			const auto* processes = Global<const std::byte* const*>(Engine::Address::kListProcesses);
			const auto* entry = processes && processes[0] ? At<const std::byte*>(processes[0], kProcessEntry) : nullptr;
			if (!entry)
				return;
			Walk walk{ p, View::Frame(), View::Camera(), entry, CameraMoved(p, processes[0], entry, View::Camera()) };
			CompareRooms(walk);
			CompareUnbound(walk);
			TallyBoxes(walk);
			CompareAccumulated(walk);
		}

		void Walked(const On<EnginePoint::PortalWalkBuilt>& a_event)
		{
			Check(a_event);
			if (window.Tick())
				Features::Portals::PortalViewsLine();
		}
	}

	void Install()
	{
		Subscribe<EnginePoint::PortalWalkBuilt>(Walked);
	}

	Tally Take()
	{
		return std::exchange(tally, {});
	}
}
