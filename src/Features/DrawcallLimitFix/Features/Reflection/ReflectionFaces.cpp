// ReflectionFaces: the water reflection update, add-root and face render hooks (render thread).

// ReflectionView.h first (Epochs/ViewEpoch.h): volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

#include <array>
#include <utility>

namespace DCLF::Features::Reflection::ReflectionFaces
{
	static_assert(kLodRoots == Api::ViewRootBit(Api::ViewRoot::LodLandAndObjects) && kTreeRoot == Api::ViewRootBit(Api::ViewRoot::LodTrees),
		"ReflectionFaces' root bits are Api::ViewRootBit's");

	namespace
	{
		// AE 1.6.1170 and SE 1.5.97 (module offsets) of the engine state the update reads, and the hooks' call sites and their callees.
		namespace Address
		{
			inline constexpr Engine::Offset kReflectSky{ 0x20104e8, 0x1decfd0 };  // bool: the water reflection draws the sky
			inline constexpr Engine::Offset kUpdateCall{ 0x52289c, 0x4c5f00 };  // TESWaterReflections::Update's call
			inline constexpr Engine::Offset kUpdate{ 0x520570, 0x4c3e20 };
			inline constexpr Engine::Offset kOrientCall{ 0x14eda00, 0x1302455 };  // the face render's FUN_1414ed500(camera, face)
			inline constexpr Engine::Offset kOrient{ 0x14ed500, 0x1301fa0 };
			inline constexpr Engine::Offset kAccumulatorRenderCall{ 0x14edbf8, 0x1302646 };  // the face render's FUN_1414a90f0(camera, accumulator, 8)
			inline constexpr Engine::Offset kAccumulatorRender{ 0x14a90f0, 0x12c15c0 };
			// The update's LOD add-roots: land, objects, trees. AE's callee is FUN_1405275e0(camera, root), which appends to the camera's
			// roots (+0x188); SE calls the roots array's append itself (FUN_1401d05c0(camera + 0x188, root), shared, never detoured), so
			// AddLodRoot's first argument is the array there. It only forwards it.
			inline constexpr std::array<Engine::Offset, 3> kAddRootCalls{ { { 0x52080d, 0x4c40ae }, { 0x520826, 0x4c40ce }, { 0x52083f, 0x4c40ee } } };
			inline constexpr Engine::Offset kAddRoot{ 0x5275e0, 0x1d05c0 };
		}

		// Render thread only: the update and the face render run there, and so does everything that asks.
		const RE::NiCamera* camera = nullptr;
		std::uint32_t faceMask = 0;
		bool plain = false;
		FaceOriented faceOriented = nullptr;
		AfterFaceDraws afterFaceDraws = nullptr;
		RootsOwned rootsOwned = nullptr;
		// This update's roots DCLF draws (RootBits), decided at its start.
		std::uint32_t skipRoots = 0;
		RootStats rootStats;

		/** @brief TESWaterReflections::Update (0x140520570): the roots DCLF draws in its faces, decided before its add-roots. */
		struct Update
		{
			static void thunk(void* a_reflection)
			{
				// Its flags (+0x10): bit 12 renders without the LOD roots, bit 4 with silhouettes (render mode 0x1B); without bit 12 and
				// bReflectSky (0x1420104e8) the faces suppress the sky (0x19). Only mode 0 is plain.
				const auto flags = *reinterpret_cast<const std::uint16_t*>(static_cast<const std::byte*>(a_reflection) + 0x10);
				static const REL::Relocation<const bool*> reflectSky{ REL::Offset(Address::kReflectSky) };
				const bool silhouettes = (flags >> 4) & 1;
				const bool noSky = !((flags >> 12) & 1) && !*reflectSky.get();
				skipRoots = rootsOwned ? rootsOwned(!silhouettes && !noSky) : 0u;
				++rootStats.updates;
				rootStats.lodSkipped += (skipRoots & kLodRoots) ? 1u : 0u;
				rootStats.treeSkipped += (skipRoots & kTreeRoot) ? 1u : 0u;
				func(a_reflection);
				skipRoots = 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The add-root (FUN_1405275e0(camera, root)) at one of the LOD call sites: skipped when DCLF draws that root. */
		template <std::uint32_t Bit, std::uint32_t Site>
		struct AddLodRoot
		{
			static void thunk(void* a_camera, void* a_root)
			{
				if (!(skipRoots & Bit))
					func(a_camera, a_root);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The face's orientation (FUN_1414ed500(camera, face)), inside the face render: the face's camera is set. */
		struct Orient
		{
			static void thunk(RE::NiCamera* a_camera, std::uint32_t a_face)
			{
				func(a_camera, a_face);
				if (faceOriented && a_camera)
					faceOriented(*a_camera, a_face);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The face's accumulator render (FUN_1414a90f0(camera, accumulator, 8)), inside the face render. */
		struct AccumulatorRender
		{
			static void thunk(RE::NiCamera* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				func(a_camera, a_accumulator, a_flags);
				if (afterFaceDraws)
					afterFaceDraws();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSCubeMapCamera's face render (vfunc 0x35): a_faces' bits are the faces it renders. */
		struct FaceRender
		{
			static void thunk(RE::NiCamera* a_camera, std::uint32_t a_faces, bool a_silhouettes, bool a_clearRoots, bool a_noSky)
			{
				camera = a_camera;
				faceMask = a_faces;
				// The render mode the faces register under: 0x1B with silhouettes, 0x19 without the sky, else 0. Only mode 0's
				// registrations reach PassCapture's hooks (FUN_1414b2330), so only then can a member's passes be withheld.
				plain = !a_silhouettes && !a_noSky;
				auto& capture = PassCapture::Get();
				// BSCubeMapCamera's two accumulators (+0x1A0, +0x1A8; skyrim-engine-notes.md, "Water reflections: the cube map"): the faces
				// cull into the first. Their renderers are the view's own (OwnRenderers).
				if (a_camera) {
					const auto* bytes = reinterpret_cast<const std::byte*>(a_camera);
					Api::ViewAccumulators accumulators;
					accumulators.accumulators[0] = *reinterpret_cast<void* const*>(bytes + 0x1A0);
					accumulators.accumulators[1] = *reinterpret_cast<void* const*>(bytes + 0x1A8);
					accumulators.count = 2;
					capture.SetViewRenderers(kView, accumulators);
				}
				capture.SetViewWithholdActive(kView, plain);
				capture.SetViewRendering(kView, true);
				func(a_camera, a_faces, a_silhouettes, a_clearRoots, a_noSky);
				capture.SetViewRendering(kView, false);
				capture.SetViewWithholdActive(kView, false);
				camera = nullptr;
				faceMask = 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		using namespace Address;
		const auto base = REL::Module::get().base();
		const std::array<std::pair<Engine::Offset, Engine::Offset>, 6> calls{ { { kOrientCall, kOrient }, { kAccumulatorRenderCall, kAccumulatorRender },
			{ kUpdateCall, kUpdate }, { kAddRootCalls[0], kAddRoot }, { kAddRootCalls[1], kAddRoot }, { kAddRootCalls[2], kAddRoot } } };
		for (const auto& [site, callee] : calls)
			if (!kReflectSky.Known() || !site.Known() || !callee.Known() || !Engine::CallsTo(base + site, base + callee)) {
				logger::warn("[DCLF] reflection face hooks not installed: the instruction at {:#x} is not the expected call", 0x140000000 + site.Get());
				return;
			}
		stl::write_vfunc<0x35, FaceRender>(RE::VTABLE_BSCubeMapCamera[0]);
		stl::write_thunk_call<Orient>(base + kOrientCall);
		stl::write_thunk_call<AccumulatorRender>(base + kAccumulatorRenderCall);
		stl::write_thunk_call<Update>(base + kUpdateCall);
		// The LOD add-roots: LOD land (0x14315b898, under bReflectLODLand), LOD objects (0x14315b8a0, bReflectLODObjects), LOD trees
		// (0x14315b880, bReflectLODTrees).
		stl::write_thunk_call<AddLodRoot<kLodRoots, 0>>(base + kAddRootCalls[0]);
		stl::write_thunk_call<AddLodRoot<kLodRoots, 1>>(base + kAddRootCalls[1]);
		stl::write_thunk_call<AddLodRoot<kTreeRoot, 2>>(base + kAddRootCalls[2]);
		logger::info("[DCLF] reflection face hooks installed: the update, its LOD add-roots, the face render (vfunc 0x35), its orientation and draws");
	}

	bool InFace() { return camera != nullptr; }
	std::uint32_t FaceMask() { return faceMask; }
	bool Plain() { return plain; }
	void SetFaceOriented(FaceOriented a_callback) { faceOriented = a_callback; }
	void SetAfterFaceDraws(AfterFaceDraws a_callback) { afterFaceDraws = a_callback; }
	void SetRootsOwned(RootsOwned a_callback) { rootsOwned = a_callback; }

	RootStats TakeRootStats()
	{
		const auto out = rootStats;
		rootStats = {};
		return out;
	}
}
