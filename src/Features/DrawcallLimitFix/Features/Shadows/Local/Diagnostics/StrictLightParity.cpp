// A portal-strict point light's native cull against DCLF's model of it (StrictLightParity.h): the AppendNonAccum trace, the
// model's walk, the comparison and its lines (render thread).

#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/StrictLightParity.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBytecode.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Features::Shadows::Local::StrictLightParity
{
	namespace
	{
		using Engine::At;

		// Offsets from the image base (AE 1.6.1170, SE 1.5.97). The process is Accumulate's static BSParabolicCullingProcess.
		constexpr DCLF::Engine::Offset kProcess{ 0x335bfa0, 0x3202ae0 };
		constexpr DCLF::Engine::Offset kAppendNonAccum{ 0x1519e60, 0x132c690 };  // vtable slot 0x19
		constexpr DCLF::Engine::Offset kRoomFrustum{ 0xe14460, 0xd3deb0 };       // FUN_140e14460(entry, room): the entry's visibility map's frustum
		// OnVisible implementations (NiAVObject vtable slot 0x34), as kinds: a kind is the AE function's offset (the switches' labels),
		// and kOnVisibleOffsets gives each kind's function on the running runtime.
		constexpr std::uintptr_t kNodeOnVisible = 0xd1e2c0, kFadeOnVisible = 0x1479f50, kLeafOnVisible = 0x147c9c0, kTreeOnVisible = 0x147d3c0;
		constexpr std::uintptr_t kMultiBoundOnVisible = 0xe2d710, kRoomOnVisible = 0xe089f0, kSwitchOnVisible = 0xd29700;
		constexpr std::uintptr_t kSharedOnVisible = 0xe24740, kGeometryOnVisible = 0xd37f30, kSubIndexOnVisible = 0xe312c0;
		constexpr std::uintptr_t kSegmentedOnVisible = 0xd59740, kNoOnVisible = 0xd1c5a0;
		constexpr std::uintptr_t kBillboardOnVisible = 0xd3f720;  // NiNode's without cameraRelatedUpdates
		struct OnVisibleOffset
		{
			std::uintptr_t kind;
			DCLF::Engine::Offset function;
		};
		constexpr std::array<OnVisibleOffset, 13> kOnVisibleOffsets{ {
			{ kNodeOnVisible, { kNodeOnVisible, 0xc58c70 } },
			{ kFadeOnVisible, { kFadeOnVisible, 0x128fe30 } },
			{ kLeafOnVisible, { kLeafOnVisible, 0x1292760 } },
			{ kTreeOnVisible, { kTreeOnVisible, 0x1293170 } },
			{ kMultiBoundOnVisible, { kMultiBoundOnVisible, 0xd56260 } },
			{ kRoomOnVisible, { kRoomOnVisible, 0xd33c70 } },
			{ kSwitchOnVisible, { kSwitchOnVisible, 0xc63ae0 } },
			{ kSharedOnVisible, { kSharedOnVisible, 0xd4cfe0 } },
			{ kGeometryOnVisible, { kGeometryOnVisible, 0xc71820 } },
			{ kSubIndexOnVisible, { kSubIndexOnVisible, 0xd59580 } },
			{ kSegmentedOnVisible, { kSegmentedOnVisible, 0xc90620 } },
			{ kNoOnVisible, { kNoOnVisible, 0xc57170 } },
			{ kBillboardOnVisible, { kBillboardOnVisible, 0xc78d90 } },
		} };
		// A function no kind names: its own offset, marked so that it never equals a kind (an SE function may sit at an AE kind's offset).
		constexpr std::uintptr_t kUnnamedOnVisible = std::uintptr_t(1) << 63;
		// Globals the node types read.
		constexpr DCLF::Engine::Offset kFadeGate{ 0x2032dfd, 0x1e0dc5d };   // byte: BSFadeNode::OnVisible's fades on
		constexpr DCLF::Engine::Offset kTreeGate{ 0x2032fb8, 0x1e0de18 };   // byte: BSTreeNode::OnVisible's height test on
		constexpr DCLF::Engine::Offset kTreeBase{ 0x332a2f0, 0x31d0df8 };   // float: the height test's base
		constexpr DCLF::Engine::Offset kTreeLimit{ 0x2032fa0, 0x1e0de00 };  // float: and its limit
		// The process's fields (BSCullingProcess / BSParabolicCullingProcess).
		constexpr std::size_t kIgnorePreprocess = 0x11e, kTreeTest = 0x121;
		constexpr std::size_t kGraphEntry = 0x30190, kCullMode = 0x30198, kCompound = 0x301a0;
		constexpr std::size_t kBackAccumulator = 0x30200, kPlane = 0x30208, kCentre = 0x30218, kRadius = 0x30224;
		// Node fields.
		constexpr std::size_t kFadeAmount = 0x100, kCurrentFade = 0x130, kSubIndexSegments = 0x16c;
		constexpr std::size_t kSwitchIndex = 0x12c;  // NiSwitchNode::OnVisible reads the active child here (CommonLib's member does not land on it)
		constexpr std::uint32_t kAlwaysDraw = 1u << 11, kPreprocessed = 1u << 12, kSettled = 1u << 15, kPreprocessHidden = 1u << 20;

		std::uintptr_t Base() { return REL::Module::get().base(); }
		using DCLF::Engine::Global;  // the module globals (an Engine::Offset argument finds it by ADL too)
		// CommonLib's casts are not const; the model reads only.
		const RE::NiNode* NodeOf(const RE::NiAVObject* a_object) { return const_cast<RE::NiAVObject*>(a_object)->AsNode(); }
		const RE::BSGeometry* GeometryOf(const RE::NiAVObject* a_object) { return const_cast<RE::NiAVObject*>(a_object)->AsGeometry(); }
		std::uintptr_t OnVisibleOf(const RE::NiAVObject* a_object) { return (*reinterpret_cast<const std::uintptr_t* const*>(a_object))[0x34] - Base(); }

		/**
		 * @brief The OnVisible a node runs, by its class's vtable: DCLF's own detours take some classes' slot (NiNode's category filter),
		 * and they forward to the engine's on the frames this parity runs. A class not listed is matched by its slot's engine function.
		 */
		std::uintptr_t OnVisibleKind(const RE::NiAVObject* a_object)
		{
			static const auto kinds = [] {
				ankerl::unordered_dense::map<std::uintptr_t, std::uintptr_t> map;
				const auto add = [&](const auto& a_vtable, std::uintptr_t a_kind) { map.emplace(REL::Relocation<std::uintptr_t>{ a_vtable }.address(), a_kind); };
				for (const auto& vtable : { RE::VTABLE_NiNode[0], RE::VTABLE_BSFaceGenNiNode[0], RE::VTABLE_BSMasterParticleSystem[0], RE::VTABLE_BSValueNode[0],
						 RE::VTABLE_BSNiNode[0], RE::VTABLE_NiBillboardNode[0], RE::VTABLE_BSDebrisNode[0], RE::VTABLE_BSBlastNode[0], RE::VTABLE_BSDamageStage[0], RE::VTABLE_BSFlattenedBoneTree[0] })
					add(vtable, kNodeOnVisible);
				add(RE::VTABLE_BSFadeNode[0], kFadeOnVisible);
				add(RE::VTABLE_BSLeafAnimNode[0], kLeafOnVisible);
				add(RE::VTABLE_BSTreeNode[0], kTreeOnVisible);
				add(RE::VTABLE_BSMultiBoundNode[0], kMultiBoundOnVisible);
				add(RE::VTABLE_BSMultiBoundRoom[0], kRoomOnVisible);
				add(RE::VTABLE_NiSwitchNode[0], kSwitchOnVisible);
				add(RE::VTABLE_BSPortalSharedNode[0], kSharedOnVisible);
				for (const auto& vtable : { RE::VTABLE_BSTriShape[0], RE::VTABLE_BSDynamicTriShape[0], RE::VTABLE_BSInstanceTriShape[0], RE::VTABLE_BSMultiIndexTriShape[0],
						 RE::VTABLE_BSLines[0], RE::VTABLE_NiParticleSystem[0], RE::VTABLE_BSGeometry[0] })
					add(vtable, kGeometryOnVisible);
				add(RE::VTABLE_BSSubIndexTriShape[0], kSubIndexOnVisible);
				add(RE::VTABLE_BSSegmentedTriShape[0], kSegmentedOnVisible);
				add(RE::VTABLE_BSLODTriShape[0], kNoOnVisible);
				return map;
			}();
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(a_object);
			if (const auto it = kinds.find(vtable); it != kinds.end())
				return it->second;
			const auto function = OnVisibleOf(a_object);
			for (const auto& entry : kOnVisibleOffsets)
				if (entry.function.Get() == function)
					return entry.kind;
			return function | kUnnamedOnVisible;
		}

		/** @brief A kind's function, as an address in the image's own numbering (0x140000000 based), for the logs. */
		std::uintptr_t KindAddress(std::uintptr_t a_kind)
		{
			if (a_kind & kUnnamedOnVisible)
				return (a_kind & ~kUnnamedOnVisible) + 0x140000000;
			for (const auto& entry : kOnVisibleOffsets)
				if (entry.kind == a_kind)
					return entry.function.Get() + 0x140000000;
			return a_kind + 0x140000000;
		}

		bool on = false;
		struct Trace
		{
			ankerl::unordered_dense::map<const void*, std::uint32_t> appended;  // geometry -> hemisphere code
			std::uint64_t repeats = 0;
			const void* firstRepeat = nullptr;
			std::uint32_t firstRepeatCodes[2]{};
			ankerl::unordered_dense::map<const void*, std::int32_t> switches;  // switch node -> its index at the cull
		};
		thread_local Trace* tracing = nullptr;
		Trace trace;  // render thread's

		struct AppendNonAccum
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_code)
			{
				if (tracing && a_object) {
					static const std::uintptr_t switchVtable = REL::Relocation<std::uintptr_t>{ RE::VTABLE_NiSwitchNode[0] }.address();
					for (const auto* node = a_object->parent; node; node = node->parent)
						if (*reinterpret_cast<const std::uintptr_t*>(node) == switchVtable)
							tracing->switches.try_emplace(node, At<std::int32_t>(node, kSwitchIndex));
					const auto [it, added] = tracing->appended.try_emplace(a_object, static_cast<std::uint32_t>(a_code));
					if (!added) {
						if (!tracing->repeats++) {
							tracing->firstRepeat = a_object;
							tracing->firstRepeatCodes[0] = it->second;
							tracing->firstRepeatCodes[1] = static_cast<std::uint32_t>(a_code);
						}
						it->second |= static_cast<std::uint32_t>(a_code);
					}
				}
				func(a_process, a_object, a_code);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The light's context: its rooms' compact programs (PortalPrograms.h's encoding, PortalViews::WalkLight's snapshot). */
		struct Programs : Portals::ProgramWords
		{
			using ProgramWords::ProgramWords;
			bool Pass(std::uint32_t a_program, const RE::NiBound& a_bound) const
			{
				const float centre[3]{ a_bound.center.x, a_bound.center.y, a_bound.center.z };
				return ProgramWords::Pass(a_program, centre, a_bound.radius);
			}
		};

		/** @brief FUN_14151a1e0 on the process's sphere and plane, as ParabolicCulling.h transcribes it. */
		struct Sphere
		{
			float centre[3]{}, radius = 0.0f, plane[4]{};
			bool back = false;

			std::uint32_t Mask(const RE::NiBound& a_bound) const
			{
				const float dx = a_bound.center.x - centre[0], dy = a_bound.center.y - centre[1], dz = a_bound.center.z - centre[2];
				if (!((std::sqrt(dx * dx + dy * dy + dz * dz) - a_bound.radius) - radius < 0.0f))
					return 0;
				const float side = ((a_bound.center.y * plane[1] + a_bound.center.x * plane[0]) + a_bound.center.z * plane[2]) - plane[3];
				if (side - a_bound.radius < 0.0f)
					return back ? 3u : 1u;
				return side > 0.0f ? 1u : (back ? 2u : 0u);
			}
		};

		enum Unmodelled : std::uint32_t
		{
			kSharedPortal,
			kOtherOnVisible,
			kMultiBoundMode,
			kUnmodelledKinds
		};
		constexpr std::array<const char*, kUnmodelledKinds> kUnmodelledNames{ "shared portal nodes", "other OnVisible", "multibound outside modes 1/3/4" };

		/** @brief The point process's walk from Process2's root (Process1, NiAVObject::Cull, the node types' OnVisible). */
		struct Model
		{
			const Programs& programs;
			const Sphere& sphere;
			const void* graphEntry = nullptr;
			bool ignorePreprocess = true, treeTest = false;
			std::uint32_t mode = 4, program = kPortalNoProgram, state = 0;
			const RE::NiAVObject* writer = nullptr;  // the last node whose test wrote state
			const ankerl::unordered_dense::map<const void*, std::int32_t>* switches = nullptr;  // the cull's switch indices
			const std::vector<const void*>* sharedNodes = nullptr;  // the context's crossed shared nodes

			bool SharedReached(const void* a_node) const
			{
				return sharedNodes && std::find(sharedNodes->begin(), sharedNodes->end(), a_node) != sharedNodes->end();
			}
			std::uint64_t switchesMoved = 0;  // switches whose index changed between the cull and the model

			std::int32_t SwitchIndex(const RE::NiAVObject* a_node)
			{
				const auto now = At<std::int32_t>(a_node, kSwitchIndex);
				if (switches)
					if (const auto it = switches->find(a_node); it != switches->end()) {
						switchesMoved += it->second != now;
						return it->second;
					}
				return now;
			}
			std::vector<const RE::NiAVObject*> path;
			bool uncertain = false;  // state since an unmodelled subtree, which may have written it

			struct Append
			{
				std::uint32_t code = 0;
				bool untested = false, inherited = false, uncertain = false;
			};
			ankerl::unordered_dense::map<const void*, Append> appended;
			ankerl::unordered_dense::set<const void*> excluded;  // geometries under an unmodelled subtree
			ankerl::unordered_dense::set<const void*> visited;   // every node Process1 reached
			ankerl::unordered_dense::map<const void*, const char*> rejected;  // why the walk stopped at a node
			std::array<std::uint64_t, kUnmodelledKinds> unmodelled{};
			std::uint64_t roomsReached = 0, roomsSkipped = 0, roomDisagree = 0, allPass = 0, untested = 0, inherited = 0, repeats = 0;
			std::uint64_t sharedReached = 0, sharedSkipped = 0;
			std::string firstRoomDisagree, firstUnmodelled;

			void Exclude(const RE::NiAVObject* a_object, Unmodelled a_kind)
			{
				++unmodelled[a_kind];
				if (firstUnmodelled.empty())
					firstUnmodelled = fmt::format("{} {} ({:#x})", kUnmodelledNames[a_kind], a_object->name.c_str(), OnVisibleOf(a_object) + 0x140000000);
				Engine::ForEachLive(a_object, [&](RE::NiAVObject& a_node, std::uint32_t) {
					if (a_node.AsGeometry())
						excluded.insert(&a_node);
				});
				uncertain = true;
			}

			void Write(const RE::NiAVObject* a_object, std::uint32_t a_mask)
			{
				state = a_mask;
				writer = a_object;
				uncertain = false;
			}

			void Cull(const RE::NiAVObject* a_object)
			{
				if (!a_object)
					return;
				if (Engine::IsHidden(a_object))
					rejected.emplace(a_object, "hidden");
				else
					Process1(a_object);
			}

			// BSParabolicCullingProcess::Process1 (0x141519a90).
			void Process1(const RE::NiAVObject* a_object)
			{
				visited.insert(a_object);
				if (mode == 2)
					return void(rejected.emplace(a_object, "mode 2"));
				const std::uint32_t flags = Engine::FlagsOf(a_object);
				if (mode == 1)
					return OnVisible(a_object, true);
				if ((flags & kPreprocessed) && !ignorePreprocess) {
					if (!(flags & kPreprocessHidden))
						OnVisible(a_object, true);
					return;
				}
				if (flags & kAlwaysDraw)
					return OnVisible(a_object, true);
				const auto& bound = a_object->worldBound;
				// The compound frustum: a room's with operators (freeOp), outside mode 3.
				if (program == kPortalNoProgram || !programs.HasOps(program) || mode == 3) {
					Write(a_object, sphere.Mask(bound));  // FUN_141519fe0 writes even a rejection
					if (state)
						OnVisible(a_object, false);
					else
						rejected.emplace(a_object, "sphere");
					return;
				}
				if (!programs.SkipView(program)) {
					const auto mask = sphere.Mask(bound);
					if (!mask || !programs.Pass(program, bound))
						return void(rejected.emplace(a_object, !mask ? "sphere (compound room)" : "compound frustum"));
					Write(a_object, mask);
					return OnVisible(a_object, false);
				}
				if (!programs.Pass(program, bound))
					return void(rejected.emplace(a_object, "compound frustum (skip view)"));
				Write(a_object, sphere.Mask(bound));
				if (state)
					OnVisible(a_object, false);
				else
					rejected.emplace(a_object, "sphere (skip view)");
			}

			void Children(const RE::NiAVObject* a_object)
			{
				const auto* node = NodeOf(a_object);
				if (!node)
					return;
				path.push_back(a_object);
				for (const auto& child : node->GetChildren())
					Cull(child.get());
				path.pop_back();
			}

			void NodeOnVisible(const RE::NiAVObject* a_object)
			{
				if (a_object->worldBound.radius != 0.0f || (Engine::FlagsOf(a_object) & kAlwaysDraw))
					Children(a_object);
				else
					rejected.emplace(a_object, "zero radius");
			}

			// BSFadeNode::OnVisible without cameraRelatedUpdates (a point light's process).
			void FadeOnVisible(const RE::NiAVObject* a_object)
			{
				const float fade = At<float>(a_object, kFadeAmount), current = At<float>(a_object, kCurrentFade);
				const bool settled = (Engine::FlagsOf(a_object) & kSettled) && fade == 1.0f && current == 1.0f;
				if (Global<std::uint8_t>(kFadeGate) && !settled && (current <= 0.0f || fade == 0.0f))
					return void(rejected.emplace(a_object, "fade"));
				NodeOnVisible(a_object);
			}

			void OnVisible(const RE::NiAVObject* a_object, bool a_untested)
			{
				switch (const auto function = OnVisibleKind(a_object)) {
				case kGeometryOnVisible:
				case kSubIndexOnVisible:
					if (function == kSubIndexOnVisible && !At<std::uint32_t>(a_object, kSubIndexSegments))
						return;
					if (const auto* geometry = GeometryOf(a_object); geometry && geometry->GetGeometryRuntimeData().shaderProperty)
						Add(a_object, a_untested);
					return;
				case kSegmentedOnVisible:  // calls NiAVObject::OnVisible: a point light appends nothing
				case kNoOnVisible:
					return;
				case kNodeOnVisible:
				case kBillboardOnVisible:
					return NodeOnVisible(a_object);
				case kFadeOnVisible:
				case kLeafOnVisible:  // BSFadeNode's after its leaf update, which needs cameraRelatedUpdates
					return FadeOnVisible(a_object);
				case kTreeOnVisible:
					if (!treeTest || !Global<std::uint8_t>(kTreeGate) || a_object->worldBound.center.z - Global<float>(kTreeBase) <= Global<float>(kTreeLimit))
						FadeOnVisible(a_object);
					return;
				case kSwitchOnVisible:
					if (const auto* node = NodeOf(a_object)) {
						const auto index = SwitchIndex(a_object);
						if (index >= 0 && static_cast<std::uint32_t>(index) < node->GetChildren().capacity()) {
							path.push_back(a_object);
							Cull(node->GetChildren()[static_cast<std::uint16_t>(index)].get());
							path.pop_back();
						}
					}
					return;
				case kMultiBoundOnVisible:
					return MultiBoundOnVisible(a_object);
				case kRoomOnVisible:
					return RoomOnVisible(a_object);
				case kSharedOnVisible:
					return SharedOnVisible(a_object);
				default:
					return Exclude(a_object, kOtherOnVisible);
				}
			}

			// BSMultiBoundNode::OnVisible (0x140e2d710). The point process's planes are inactive (Process2), so a mode 4 shape test passes.
			void MultiBoundOnVisible(const RE::NiAVObject* a_object)
			{
				const auto* node = static_cast<const RE::BSMultiBoundNode*>(a_object);
				if (node->GetChildren().empty())
					return;
				const auto& data = node->GetRuntimeData();
				if (!(Engine::FlagsOf(a_object) & kAlwaysDraw) && mode != 1 && mode != 3 && data.multiBound && mode != 4)
					return Exclude(a_object, kMultiBoundMode);
				const auto outer = mode;
				if (data.cullingMode == RE::BSCullingProcess::BSCPCullingType::kAllPass && mode != 3) {
					mode = 1;
					++allPass;
				}
				Children(a_object);
				mode = outer;
			}

			// BSMultiBoundRoom::OnVisible (0x140e089f0), the frustum from the light's visibility map (the context's reached rooms).
			void RoomOnVisible(const RE::NiAVObject* a_object)
			{
				const auto room = PortalViews::Get().ProgramOfRoom(a_object);
				const bool reached = room != kPortalNoProgram && programs.Reached(room);
				// The engine's own map: the context must agree on which rooms the walk reached.
				if (graphEntry) {
					using Lookup = const void* (*)(const void*, const void*, std::uint64_t, std::uint64_t);
					const bool native = reinterpret_cast<Lookup>(Base() + kRoomFrustum)(graphEntry, a_object, 0, 0) != nullptr;
					if (native != reached) {
						++roomDisagree;
						if (firstRoomDisagree.empty())
							firstRoomDisagree = fmt::format("room {} ({}): engine {}, context {}", a_object->name.c_str(), room, native ? "reached" : "not reached", reached ? "reached" : "not reached");
					}
				}
				if (!reached && mode != 1 && mode != 3) {
					++roomsSkipped;
					rejected.emplace(a_object, "room not reached");
					return;
				}
				++roomsReached;
				const auto outerMode = mode;
				const auto outerProgram = program;
				const auto roomMode = static_cast<std::uint32_t>(static_cast<const RE::BSMultiBoundNode*>(a_object)->GetRuntimeData().cullingMode);
				if (roomMode != 1 && roomMode != 3)
					program = reached ? room : kPortalNoProgram;
				if (mode != 3 && mode != 4)
					mode = roomMode;
				Children(a_object);
				mode = outerMode;
				if (outerMode != 1 && outerMode != 3)
					program = outerProgram;
			}

			// BSPortalSharedNode::OnVisible (0x140e24740): the map's entry for it is an empty frustum, so its subtree takes the sphere alone.
			void SharedOnVisible(const RE::NiAVObject* a_object)
			{
				if (NodeOf(a_object)->GetChildren().empty())
					return;
				const bool reached = SharedReached(a_object);
				if (graphEntry) {
					using Lookup = const void* (*)(const void*, const void*, std::uint64_t, std::uint64_t);
					const bool native = reinterpret_cast<Lookup>(Base() + kRoomFrustum)(graphEntry, a_object, 0, 0) != nullptr;
					if (native != reached) {
						++roomDisagree;
						if (firstRoomDisagree.empty())
							firstRoomDisagree = fmt::format("shared node {}: engine {}, context {}", a_object->name.c_str(), native ? "reached" : "not reached", reached ? "reached" : "not reached");
					}
				}
				if (!reached && mode != 1 && mode != 3) {
					++sharedSkipped;
					rejected.emplace(a_object, "shared node not reached");
					return;
				}
				++sharedReached;
				const auto outerProgram = program;
				program = kPortalNoProgram;
				Children(a_object);
				program = outerProgram;
			}

			void Add(const RE::NiAVObject* a_object, bool a_untested)
			{
				Append append{ state, a_untested, false, uncertain };
				if (a_untested) {
					++untested;
					// Native order: an untested visit takes the last state written, which an earlier sibling's subtree may have written.
					append.inherited = writer && std::find(path.begin(), path.end(), writer) == path.end();
					inherited += append.inherited;
				}
				// Reached again (overlapping roots): the trace ORs the codes, as both appends registered.
				if (const auto [it, added] = appended.try_emplace(a_object, append); !added) {
					it->second.code |= append.code;
					++repeats;
				}
			}
		};

		/**
		 * @brief The per-object predicate a GPU stage could evaluate from persistent ancestry: each geometry's structural gates (hidden,
		 * zero-radius nodes, inactive switch children, unsettled fades, the tree height test, unreached rooms, kAllPass below its room)
		 * and its own bound's test in its room's frustum, but no ancestor's bound test and no inherited state: an untested geometry
		 * takes its own sphere code. Measures how far that is from the engine's appends.
		 */
		struct Leaf
		{
			Model& model;
			ankerl::unordered_dense::map<const void*, std::uint32_t> appended;

			void Visit(const RE::NiAVObject* a_object, std::uint32_t a_mode, std::uint32_t a_program)
			{
				if (!a_object || Engine::IsHidden(a_object))
					return;
				const std::uint32_t flags = Engine::FlagsOf(a_object);
				const auto kind = OnVisibleKind(a_object);
				if (kind == kGeometryOnVisible || kind == kSubIndexOnVisible) {
					if (model.excluded.contains(a_object) || (kind == kSubIndexOnVisible && !At<std::uint32_t>(a_object, kSubIndexSegments)))
						return;
					const auto* geometry = GeometryOf(a_object);
					if (!geometry || !geometry->GetGeometryRuntimeData().shaderProperty)
						return;
					const auto& bound = a_object->worldBound;
					const auto mask = model.sphere.Mask(bound);
					if (a_mode == 1 || (flags & kAlwaysDraw)) {
						appended[a_object] = mask;
						return;
					}
					const bool compound = a_program != kPortalNoProgram && model.programs.HasOps(a_program) && a_mode != 3;
					if (!compound) {
						if (mask)
							appended[a_object] = mask;
					} else if (model.programs.SkipView(a_program) ? model.programs.Pass(a_program, bound) && mask : mask && model.programs.Pass(a_program, bound))
						appended[a_object] = mask;
					return;
				}
				const auto* node = NodeOf(a_object);
				if (!node)
					return;
				const auto children = [&](std::uint32_t a_childMode, std::uint32_t a_childProgram) {
					for (const auto& child : node->GetChildren())
						Visit(child.get(), a_childMode, a_childProgram);
				};
				const bool walks = a_object->worldBound.radius != 0.0f || (flags & kAlwaysDraw);
				const auto fadeSkips = [&] {
					const float fade = At<float>(a_object, kFadeAmount), current = At<float>(a_object, kCurrentFade);
					const bool settled = (flags & kSettled) && fade == 1.0f && current == 1.0f;
					return Global<std::uint8_t>(kFadeGate) && !settled && (current <= 0.0f || fade == 0.0f);
				};
				switch (kind) {
				case kNodeOnVisible:
				case kBillboardOnVisible:
					if (walks)
						children(a_mode, a_program);
					return;
				case kTreeOnVisible:
					if (model.treeTest && Global<std::uint8_t>(kTreeGate) && a_object->worldBound.center.z - Global<float>(kTreeBase) > Global<float>(kTreeLimit))
						return;
					[[fallthrough]];
				case kFadeOnVisible:
				case kLeafOnVisible:
					if (!fadeSkips() && walks)
						children(a_mode, a_program);
					return;
				case kSwitchOnVisible:
					if (const auto index = model.SwitchIndex(a_object); index >= 0 && static_cast<std::uint32_t>(index) < node->GetChildren().capacity())
						Visit(node->GetChildren()[static_cast<std::uint16_t>(index)].get(), a_mode, a_program);
					return;
				case kMultiBoundOnVisible:
					children(static_cast<const RE::BSMultiBoundNode*>(node)->GetRuntimeData().cullingMode == RE::BSCullingProcess::BSCPCullingType::kAllPass && a_mode != 3 ? 1u : a_mode,
						a_program);
					return;
				case kRoomOnVisible:
					{
						const auto room = PortalViews::Get().ProgramOfRoom(a_object);
						const bool reached = room != kPortalNoProgram && model.programs.Reached(room);
						if (!reached && a_mode != 1 && a_mode != 3)
							return;
						const auto roomMode = static_cast<std::uint32_t>(static_cast<const RE::BSMultiBoundNode*>(node)->GetRuntimeData().cullingMode);
						children(a_mode != 3 && a_mode != 4 ? roomMode : a_mode, roomMode != 1 && roomMode != 3 ? (reached ? room : kPortalNoProgram) : a_program);
						return;
					}
				case kSharedOnVisible:
					if (model.SharedReached(a_object) || a_mode == 1 || a_mode == 3)
						children(a_mode, kPortalNoProgram);
					return;
				default:
					return;  // unmodelled: its geometries are excluded
				}
			}
		};

		/** @brief Why the model did not append a geometry: the first node on its parent chain it rejected, or where its walk never went. */
		std::string Explain(const Model& a_model, const RE::NiAVObject* a_geometry)
		{
			std::uint32_t depth = 0;
			for (const auto* node = a_geometry; node; node = node->parent, ++depth) {
				if (const auto it = a_model.rejected.find(node); it != a_model.rejected.end())
					return fmt::format("{} at {} ({} up, {:#x}, radius {:.2f}, flags {:#x})", it->second, node->name.empty() ? "?" : node->name.c_str(), depth,
						KindAddress(OnVisibleKind(node)), node->worldBound.radius, Engine::FlagsOf(node));
				if (a_model.visited.contains(node)) {
					std::string children;
					if (OnVisibleKind(node) == kSwitchOnVisible)
						if (const auto* parent = NodeOf(node)) {
							const auto& list = parent->GetChildren();
							children = fmt::format("; switch index {} (vtable {:#x}), {} children of capacity {}:", At<std::int32_t>(parent, kSwitchIndex),
								*reinterpret_cast<const std::uintptr_t*>(node) - Base() + 0x140000000, list.size(), list.capacity());
							for (std::uint16_t c = 0; c < list.capacity(); ++c)
								if (const auto* child = list[c].get())
									children += fmt::format(" [{}] {}{}{}", c, child->name.empty() ? "?" : child->name.c_str(), Engine::IsHidden(child) ? " hidden" : "",
										a_model.visited.contains(child) ? " visited" : "");
						}
					return fmt::format("{} {} visited ({:#x}, radius {:.2f}, flags {:#x}) but its OnVisible did not reach the child{}", depth ? "ancestor" : "geometry",
						node->name.empty() ? "?" : node->name.c_str(), KindAddress(OnVisibleKind(node)), node->worldBound.radius, Engine::FlagsOf(node), children);
				}
			}
			return "no ancestor of it was visited (not under the request's roots)";
		}

		std::string PathOf(const RE::NiAVObject* a_object)
		{
			std::string path;
			for (std::uint32_t depth = 0; a_object && depth < 6; a_object = a_object->parent, ++depth)
				path += fmt::format("{}{}", depth ? " < " : "", a_object->name.empty() ? "?" : a_object->name.c_str());
			return path;
		}

		/**
		 * @brief DCLF's strict gates (Portals/StrictGates.h) as its consumers apply them, against the engine's appends: every geometry the
		 * structural gates (hidden, zero radius, switches, fades, the tree test) let through whose bound is in the light's sphere, rooms and
		 * shared nodes descended whatever their verdict (the gates' to decide). By geometry (the receivers' bits: its own chain) and by fade
		 * root (the GPU's casters and receivers: the root's chain, gate 0 without a listed root), each against whether the engine appended
		 * it with a hemisphere code.
		 */
		struct Coverage
		{
			Model& model;
			const Portals::Context& context;
			const Trace& trace;
			Api::RootListView roots;
			const ankerl::unordered_dense::map<const void*, std::uint32_t>& objects;  // geometry -> DCLF object slot
			Portals::GateChain chain;
			struct Tally
			{
				std::uint64_t agree = 0, engineOnly = 0, gatesOnly = 0;
				std::string first;
			};
			Tally geometry, root;
			std::uint64_t rootless = 0, notObjects = 0;

			void Count(Tally& a_tally, bool a_gates, bool a_engine, const RE::NiAVObject* a_object, std::string_view a_by)
			{
				if (a_gates == a_engine)
					return void(++a_tally.agree);
				++(a_engine ? a_tally.engineOnly : a_tally.gatesOnly);
				if (a_tally.first.empty()) {
					std::string links;
					for (const auto& link : chain)
						links += fmt::format(" {}{}", link.kind == Portals::GateLink::kRoom ? "room " : link.kind == Portals::GateLink::kShared ? "shared " : link.kind == Portals::GateLink::kAllPass ? "allpass " : "shape ",
							static_cast<const RE::NiAVObject*>(link.node)->name.empty() ? "?" : static_cast<const RE::NiAVObject*>(link.node)->name.c_str());
					a_tally.first = fmt::format("{}: engine {}, gates by {} {} (chain{})", PathOf(a_object), a_engine ? "appended" : "did not", a_by, a_gates ? "admit" : "refuse", links);
				}
			}

			void Geometry(const RE::NiAVObject* a_object)
			{
				const auto& bound = a_object->worldBound;
				if (!model.sphere.Mask(bound))
					return;  // DCLF's views and receivers test the sphere themselves
				const auto it = trace.appended.find(a_object);
				const bool engine = it != trace.appended.end() && it->second != 0;
				const float centre[3]{ bound.center.x, bound.center.y, bound.center.z };
				Portals::ChainOf(a_object, chain);
				Count(geometry, Portals::Admit(context, chain, centre, bound.radius), engine, a_object, "geometry");
				// The GPU's column: the object's fade root's chain, its own chain without a root. A geometry DCLF has no object for is drawn by
				// the engine (the receivers' bits alone).
				const auto object = objects.find(a_object);
				if (object == objects.end())
					return void(++notObjects);
				const auto* listed = static_cast<const RE::NiAVObject*>(roots.NodeOf(roots.RootOf(object->second)));
				if (!listed)
					++rootless;
				Portals::ChainOf(listed ? listed : a_object, chain);
				Count(root, Portals::Admit(context, chain, centre, bound.radius), engine, a_object, listed ? fmt::format("root {}", listed->name.empty() ? "?" : listed->name.c_str()) : std::string("its own chain"));
			}

			void Visit(const RE::NiAVObject* a_object)
			{
				if (!a_object || Engine::IsHidden(a_object))
					return;
				const std::uint32_t flags = Engine::FlagsOf(a_object);
				const auto kind = OnVisibleKind(a_object);
				if (kind == kGeometryOnVisible || kind == kSubIndexOnVisible) {
					if (model.excluded.contains(a_object) || (kind == kSubIndexOnVisible && !At<std::uint32_t>(a_object, kSubIndexSegments)))
						return;
					if (const auto* shape = GeometryOf(a_object); shape && shape->GetGeometryRuntimeData().shaderProperty)
						Geometry(a_object);
					return;
				}
				const auto* node = NodeOf(a_object);
				if (!node)
					return;
				const auto children = [&] {
					for (const auto& child : node->GetChildren())
						Visit(child.get());
				};
				const bool walks = a_object->worldBound.radius != 0.0f || (flags & kAlwaysDraw);
				const auto fadeSkips = [&] {
					const float fade = At<float>(a_object, kFadeAmount), current = At<float>(a_object, kCurrentFade);
					const bool settled = (flags & kSettled) && fade == 1.0f && current == 1.0f;
					return Global<std::uint8_t>(kFadeGate) && !settled && (current <= 0.0f || fade == 0.0f);
				};
				switch (kind) {
				case kNodeOnVisible:
				case kBillboardOnVisible:
					if (walks)
						children();
					return;
				case kTreeOnVisible:
					if (model.treeTest && Global<std::uint8_t>(kTreeGate) && a_object->worldBound.center.z - Global<float>(kTreeBase) > Global<float>(kTreeLimit))
						return;
					[[fallthrough]];
				case kFadeOnVisible:
				case kLeafOnVisible:
					if (!fadeSkips() && walks)
						children();
					return;
				case kSwitchOnVisible:
					if (const auto index = model.SwitchIndex(a_object); index >= 0 && static_cast<std::uint32_t>(index) < node->GetChildren().capacity())
						Visit(node->GetChildren()[static_cast<std::uint16_t>(index)].get());
					return;
				case kMultiBoundOnVisible:
				case kRoomOnVisible:
				case kSharedOnVisible:
					children();
					return;
				default:
					return;
				}
			}
		};

		/** @brief The report window's counts (render thread). */
		struct Window
		{
			std::uint64_t lights = 0, noContext = 0, noSelection = 0, notSelected = 0, unwalked = 0, noGraph = 0, arrays = 0, explicitRoots = 0, engine = 0, agree = 0, differ = 0, engineOnly = 0, modelOnly = 0, excluded = 0, uncertain = 0;
			std::uint64_t roomsReached = 0, roomsSkipped = 0, roomDisagree = 0, allPass = 0, untested = 0, inherited = 0, repeats = 0;
			std::uint64_t leafAgree = 0, leafDiffer = 0, leafEngineOnly = 0, leafOnly = 0, lightsWithRepeats = 0, modelRepeats = 0, switchesMoved = 0, sharedReached = 0, sharedSkipped = 0;
			std::uint64_t gateLights = 0, geometryAgree = 0, geometryEngineOnly = 0, geometryGatesOnly = 0, rootAgree = 0, rootEngineOnly = 0, rootGatesOnly = 0, rootless = 0, notObjects = 0;
			std::string firstNotSelected;
			std::string firstGeometryGate, firstRootGate;
			std::array<std::uint64_t, kUnmodelledKinds> unmodelled{};
			std::string firstDiffer, firstRoomDisagree, firstUnmodelled, firstLeafDiffer, firstRepeat;
		};
		Window window;
		constexpr std::uint64_t kReportLights = 10;  // traced lights a line

		void Report()
		{
			auto& w = window;
			const bool differs = w.differ || w.engineOnly || w.modelOnly || w.roomDisagree;
			logger::info("[DCLF] strict light parity: {} lights ({} without a context: {} no selection, {} not selected, {} no portal graph, {} not walked; {} root arrays, {} explicit roots), {} engine appends: {} agree, {} codes differ, {} engine only, {} model only, "
						 "{} excluded ({} {}, {} {}, {} {}), {} after an unmodelled subtree; rooms {} reached, {} skipped, shared nodes {} reached, {} skipped, {} membership differs; kAllPass {}; untested appends {} ({} inherited outside "
						 "their ancestry); repeated appends {} in {} lights (the model {}); switch indices moved since the cull {}{}",
				w.lights, w.noContext, w.noSelection, w.notSelected, w.noGraph, w.unwalked, w.arrays, w.explicitRoots, w.engine, w.agree, w.differ, w.engineOnly, w.modelOnly, w.excluded, w.unmodelled[0], kUnmodelledNames[0], w.unmodelled[1],
				kUnmodelledNames[1], w.unmodelled[2], kUnmodelledNames[2], w.uncertain, w.roomsReached, w.roomsSkipped, w.sharedReached, w.sharedSkipped, w.roomDisagree, w.allPass, w.untested, w.inherited,
				w.repeats, w.lightsWithRepeats, w.modelRepeats, w.switchesMoved, differs ? " <- STRICT LIGHT" : " <- OK");
			if (!w.firstDiffer.empty())
				logger::info("[DCLF] strict light parity: first difference {}", w.firstDiffer);
			if (!w.firstRepeat.empty())
				logger::info("[DCLF] strict light parity: first repeated engine append {}", w.firstRepeat);
			if (!w.firstRoomDisagree.empty())
				logger::info("[DCLF] strict light parity: first room membership difference {}", w.firstRoomDisagree);
			if (!w.firstUnmodelled.empty())
				logger::info("[DCLF] strict light parity: first unmodelled subtree {}", w.firstUnmodelled);
			logger::info("[DCLF] strict light parity: per-object predicate (own bound, structural gates) against the engine: {} agree, {} codes differ, {} engine only, {} predicate only{}",
				w.leafAgree, w.leafDiffer, w.leafEngineOnly, w.leafOnly, w.firstLeafDiffer.empty() ? "" : "; first " + w.firstLeafDiffer);
			// The coverage of DCLF's consumers: an engine-only geometry is a caster or receiver DCLF drops; a gates-only one, one DCLF adds.
			const bool gatesDiffer = w.geometryEngineOnly || w.geometryGatesOnly || w.rootEngineOnly || w.rootGatesOnly;
			logger::info("[DCLF] strict light parity: DCLF's strict gates against the engine in {} lights (geometries in the sphere under the structural gates): by geometry (receivers' "
						 "bits) {} agree, {} engine only, {} gates only; by object column (GPU casters and receivers) {} agree, {} engine only, {} gates only ({} without a root, by their own chain), {} not DCLF objects{}",
				w.gateLights, w.geometryAgree, w.geometryEngineOnly, w.geometryGatesOnly, w.rootAgree, w.rootEngineOnly, w.rootGatesOnly, w.rootless, w.notObjects,
				w.gateLights ? (gatesDiffer ? " <- STRICT GATES" : " <- OK") : "");
			if (!w.firstNotSelected.empty())
				logger::info("[DCLF] strict light parity: first light not in the selection {}", w.firstNotSelected);
			if (!w.firstGeometryGate.empty())
				logger::info("[DCLF] strict light parity: first gate difference by geometry {}", w.firstGeometryGate);
			if (!w.firstRootGate.empty())
				logger::info("[DCLF] strict light parity: first gate difference by root {}", w.firstRootGate);
			w = {};
		}
	}

	namespace
	{
		/** @brief A traced cull, compared once the selection has made the light's context. */
		struct Pending
		{
			const RE::BSShadowLight* light = nullptr;
			// Accumulate's roots: an explicit one, the light's sceneAccumArray (FUN_140e28f70: the first through Process2, the rest
			// straight to Process1, the process's state carried between them), or the scene graph's object root.
			std::vector<const RE::NiAVObject*> roots;
			bool array = false;
			std::uint32_t frame = 0, mode = 4;
			Trace trace;
			Sphere sphere;
			const void* graphEntry = nullptr;
			bool ignorePreprocess = true, treeTest = false, compound = false, explicitRoot = false;
		};
		std::vector<Pending> pendings;  // render thread
		bool enabled = false;
	}
	void Compare(const Pending& a_pending);

	bool Enabled() { return enabled; }

	void Configure(bool a_on)
	{
		enabled = a_on;
		if (enabled)
			Diagnostics::Subscribe<Diagnostics::EnginePoint::LightSelectionMade>([](const Diagnostics::On<Diagnostics::EnginePoint::LightSelectionMade>&) {
				for (const auto& pending : pendings)
					Compare(pending);
				pendings.clear();
			});
	}

	void Install()
	{
		static bool tried = false;
		if (std::exchange(tried, true) || !Enabled())
			return;
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSParabolicCullingProcess[0] };
		if (reinterpret_cast<const std::uintptr_t*>(vtable.address())[0x19] != Base() + kAppendNonAccum) {
			logger::warn("[DCLF] strict light parity: BSParabolicCullingProcess::AppendNonAccum is not the engine's; no parity");
			return;
		}
		stl::write_vfunc<0x19, AppendNonAccum>(RE::VTABLE_BSParabolicCullingProcess[0]);
		on = true;
		logger::info("[DCLF] strict light parity installed (the portal-strict point lights' culls on the parity frames)");
	}

	bool Begin(const RE::BSShadowLight& a_light, bool a_whole, std::uint32_t a_frame)
	{
		if (!on || !a_whole || !a_light.portalStrict || !Diagnostics::ParityDue(a_frame))
			return false;
		trace.appended.clear();
		trace.repeats = 0;
		tracing = &trace;
		return true;
	}

	void End(const RE::BSShadowLight& a_light, const RE::NiAVObject* a_root, std::uint32_t a_frame)
	{
		tracing = nullptr;
		// The process as the cull left it: its sphere and plane, back accumulator and request flags (the mode and frustum restored). The
		// light's context is the selection's, made after the engine's function returns: the comparison waits for LightSelectionMade
		// (the scene graph does not change in between; the next light's cull overwrites the process).
		auto& pending = pendings.emplace_back();
		pending.light = &a_light;
		pending.frame = a_frame;
		pending.trace = std::move(trace);
		trace = {};
		const auto& data = const_cast<RE::BSShadowLight&>(a_light).GetRuntimeData();
		const auto& shaderState = RE::BSShaderManager::State::GetSingleton();
		const auto* sceneNode = data.sceneGraphIndex < 4 ? shaderState.shadowSceneNode[data.sceneGraphIndex] : nullptr;
		pending.explicitRoot = a_root != nullptr;
		if (a_root)
			pending.roots.push_back(a_root);
		else if (!data.sceneAccumArray.empty()) {
			pending.array = true;
			for (const auto& object : data.sceneAccumArray)
				pending.roots.push_back(object.get());
		} else if (sceneNode && sceneNode->GetChildren().size() > 3)
			pending.roots.push_back(sceneNode->GetChildren()[3].get());
		const auto* process = reinterpret_cast<const std::byte*>(Base() + kProcess);
		auto& sphere = pending.sphere;
		std::memcpy(sphere.centre, &At<float>(process, kCentre), sizeof(sphere.centre));
		sphere.radius = At<float>(process, kRadius);
		std::memcpy(sphere.plane, &At<float>(process, kPlane), sizeof(sphere.plane));
		sphere.back = At<const void*>(process, kBackAccumulator) != nullptr;
		pending.graphEntry = At<const void*>(process, kGraphEntry);
		pending.ignorePreprocess = At<bool>(process, kIgnorePreprocess);
		pending.treeTest = At<bool>(process, kTreeTest);
		pending.mode = At<std::uint32_t>(process, kCullMode);
		pending.compound = At<const void*>(process, kCompound) != nullptr;
	}

	void Compare(const Pending& a_pending)
	{
		auto& w = window;
		const auto& traced = a_pending.trace;
		++w.lights;
		w.repeats += traced.repeats;
		w.lightsWithRepeats += traced.repeats != 0;
		if (traced.firstRepeat && w.firstRepeat.empty()) {
			std::uint32_t covering = 0;
			for (const auto* root : a_pending.roots)
				for (const auto* node = static_cast<const RE::NiAVObject*>(traced.firstRepeat); node; node = node->parent)
					covering += node == root;
			w.firstRepeat = fmt::format("{}: codes {} then {}, under {} of the request's {} roots", PathOf(static_cast<const RE::NiAVObject*>(traced.firstRepeat)),
				traced.firstRepeatCodes[0], traced.firstRepeatCodes[1], covering, a_pending.roots.size());
		}
		const auto* selection = LightSelection::Get().Current(a_pending.frame);
		// The frame's lights (Frame::FrameLight): every selection of the scene frame's. A light the walk did not walk (no rooms, hidden, on an
		// object node) has no context: no room is reached, and the engine's own visibility map checks that at every room the model meets.
		const auto* entry = selection ? selection->LightOf(a_pending.light) : nullptr;
		const bool selected = entry != nullptr;
		const Portals::ContextHold context = entry ? entry->portalContext : nullptr;
		const std::uint32_t walk = entry ? entry->portalWalk : PortalViews::kLightUntouched;
		if (!context) {
			++w.noContext;
			++(!selection ? w.noSelection : !selected ? w.notSelected : walk == PortalViews::kLightNoGraph ? w.noGraph : w.unwalked);
			if (selection && !selected && w.firstNotSelected.empty()) {
				std::string kept;
				for (const auto& light : selection->frameLights)
					kept += fmt::format(" {:p}", static_cast<const void*>(light.light));
				const auto* shadowScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
				bool active = false;
				if (shadowScene)
					for (const auto& candidate : shadowScene->GetRuntimeData().activeShadowLights)
						active = active || candidate.get() == a_pending.light;
				w.firstNotSelected = fmt::format("{:p} (traced frame {}, selection frame {} after {} selections, {} in activeShadowLights; the frame's selections kept{}{})",
					static_cast<const void*>(a_pending.light), a_pending.frame, selection->sceneFrame, selection->selections, active ? "" : "not", kept.empty() ? " none" : kept,
					selection->sun ? " after the sun" : "");
			}
		}
		w.arrays += a_pending.array;
		w.explicitRoots += !a_pending.array && a_pending.roots.size() == 1 && a_pending.explicitRoot;
		const auto& sphere = a_pending.sphere;
		static const std::vector<std::uint32_t> kNoPrograms;
		const Programs programs(context ? std::span<const std::uint32_t>(context->programs) : std::span<const std::uint32_t>(kNoPrograms));
		Model model{ programs, sphere };
		model.graphEntry = a_pending.graphEntry;
		model.switches = &traced.switches;
		model.sharedNodes = context ? &context->sharedNodes : nullptr;
		model.ignorePreprocess = a_pending.ignorePreprocess;
		model.treeTest = a_pending.treeTest;
		model.mode = a_pending.mode;
		for (std::size_t i = 0; i < a_pending.roots.size(); ++i) {
			const auto* root = a_pending.roots[i];
			if (!root)
				continue;
			if (a_pending.compound)
				model.Exclude(root, kOtherOnVisible);  // a request with its own frustum: not a point light's
			else if (!a_pending.array || i == 0)
				model.Cull(root);
			else if (Engine::IsHidden(root))
				model.Exclude(root, kOtherOnVisible);  // the request's hidden test of the later elements is not read
			else
				model.Process1(root);
		}
		// The comparison, both ways, the unmodelled subtrees' geometries left out.
		const auto note = [&](const void* a_geometry, std::string_view a_what) {
			if (w.firstDiffer.empty())
				w.firstDiffer = fmt::format("{}: {}", PathOf(static_cast<const RE::NiAVObject*>(a_geometry)), a_what);
		};
		for (const auto& [geometry, code] : traced.appended) {
			if (model.excluded.contains(geometry)) {
				++w.excluded;
				continue;
			}
			++w.engine;
			const auto it = model.appended.find(geometry);
			if (it == model.appended.end()) {
				++w.engineOnly;
				note(geometry, fmt::format("engine appended code {}, the model did not reach it: {}", code, Explain(model, static_cast<const RE::NiAVObject*>(geometry))));
			} else if (it->second.code != code) {
				++w.differ;
				note(geometry, fmt::format("engine code {}, model {}{}{}{}", code, it->second.code, it->second.untested ? " untested" : "", it->second.inherited ? " inherited" : "",
								   it->second.uncertain ? " after an unmodelled subtree" : ""));
			} else
				++w.agree;
		}
		for (const auto& [geometry, append] : model.appended)
			if (!traced.appended.contains(geometry)) {
				++w.modelOnly;
				note(geometry, fmt::format("model appended code {}{}, the engine did not", append.code, append.untested ? " untested" : ""));
			} else
				w.uncertain += append.uncertain;
		// The per-object predicate against the engine, on the same geometries.
		Leaf leaf{ model };
		if (!a_pending.compound)
			for (const auto* root : a_pending.roots)
				leaf.Visit(root, a_pending.mode, kPortalNoProgram);
		const auto noteLeaf = [&](const void* a_geometry, std::string_view a_what) {
			if (w.firstLeafDiffer.empty())
				w.firstLeafDiffer = fmt::format("{}: {}", PathOf(static_cast<const RE::NiAVObject*>(a_geometry)), a_what);
		};
		for (const auto& [geometry, code] : traced.appended) {
			if (model.excluded.contains(geometry))
				continue;
			const auto it = leaf.appended.find(geometry);
			if (it == leaf.appended.end()) {
				// An engine append of code 0 registers nothing: the predicate's omission agrees with it.
				if (code) {
					++w.leafEngineOnly;
					noteLeaf(geometry, fmt::format("engine code {}, the predicate culls it", code));
				} else
					++w.leafAgree;
			} else if (it->second != code) {
				++w.leafDiffer;
				noteLeaf(geometry, fmt::format("engine code {}, predicate {}", code, it->second));
			} else
				++w.leafAgree;
		}
		for (const auto& [geometry, code] : leaf.appended)
			if (code && !traced.appended.contains(geometry)) {
				++w.leafOnly;
				noteLeaf(geometry, fmt::format("predicate code {}, the engine did not reach it", code));
			}
		// DCLF's strict gates, as its consumers apply them, on the same cull.
		if (context && !a_pending.compound) {
			const auto& tables = Scene::FrameView::Get().GetTables();
			ankerl::unordered_dense::map<const void*, std::uint32_t> objects;
			for (std::size_t o = 0; o < tables.objectGeometry.size(); ++o)
				if (tables.objectGeometry[o])
					objects.emplace(tables.objectGeometry[o], static_cast<std::uint32_t>(o));
			Coverage coverage{ model, *context, traced, Extensions::RootListOf(tables), objects };
			for (const auto* root : a_pending.roots)
				coverage.Visit(root);
			++w.gateLights;
			w.geometryAgree += coverage.geometry.agree, w.geometryEngineOnly += coverage.geometry.engineOnly, w.geometryGatesOnly += coverage.geometry.gatesOnly;
			w.rootAgree += coverage.root.agree, w.rootEngineOnly += coverage.root.engineOnly, w.rootGatesOnly += coverage.root.gatesOnly;
			w.rootless += coverage.rootless;
			w.notObjects += coverage.notObjects;
			if (w.firstGeometryGate.empty())
				w.firstGeometryGate = coverage.geometry.first;
			if (w.firstRootGate.empty())
				w.firstRootGate = coverage.root.first;
		}
		for (std::uint32_t k = 0; k < kUnmodelledKinds; ++k)
			w.unmodelled[k] += model.unmodelled[k];
		w.roomsReached += model.roomsReached;
		w.roomsSkipped += model.roomsSkipped;
		w.roomDisagree += model.roomDisagree;
		w.allPass += model.allPass;
		w.untested += model.untested;
		w.inherited += model.inherited;
		w.modelRepeats += model.repeats;
		w.switchesMoved += model.switchesMoved;
		w.sharedReached += model.sharedReached;
		w.sharedSkipped += model.sharedSkipped;
		if (w.firstRoomDisagree.empty())
			w.firstRoomDisagree = model.firstRoomDisagree;
		if (w.firstUnmodelled.empty())
			w.firstUnmodelled = model.firstUnmodelled;
		if (w.lights >= kReportLights)
			Report();
	}
}
