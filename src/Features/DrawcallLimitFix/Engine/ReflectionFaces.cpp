#include "ReflectionFaces.h"

#include "Features/DrawcallLimitFix/Engine/PassCapture.h"

namespace DCLF::ReflectionFaces
{
	namespace
	{
		// Render thread only: the update and the face render run there, and so does everything that asks.
		const RE::NiCamera* camera = nullptr;
		std::uint32_t faceMask = 0;
		bool plain = false;
		AfterFaces afterFaces = nullptr;
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
				static const REL::Relocation<const bool*> reflectSky{ REL::Offset(0x20104e8) };
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
				capture.SetReflectionCamera(a_camera);
				capture.SetReflectionFace(plain);
				func(a_camera, a_faces, a_silhouettes, a_clearRoots, a_noSky);
				capture.SetReflectionFace(false);
				camera = nullptr;
				faceMask = 0;
				if (afterFaces)
					afterFaces(a_faces);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		stl::write_vfunc<0x35, FaceRender>(RE::VTABLE_BSCubeMapCamera[0]);
		stl::write_thunk_call<Orient>(REL::Offset(0x14eda00).address());
		stl::write_thunk_call<AccumulatorRender>(REL::Offset(0x14edbf8).address());
		stl::write_thunk_call<Update>(REL::Offset(0x52289c).address());
		// The LOD add-roots: LOD land (0x14315b898, under bReflectLODLand), LOD objects (0x14315b8a0, bReflectLODObjects), LOD trees
		// (0x14315b880, bReflectLODTrees).
		stl::write_thunk_call<AddLodRoot<kLodRoots, 0>>(REL::Offset(0x52080d).address());
		stl::write_thunk_call<AddLodRoot<kLodRoots, 1>>(REL::Offset(0x520826).address());
		stl::write_thunk_call<AddLodRoot<kTreeRoot, 2>>(REL::Offset(0x52083f).address());
		logger::info("[DCLF] reflection face hooks installed: the update, its LOD add-roots, the face render (vfunc 0x35), its orientation and draws");
	}

	bool InFace() { return camera != nullptr; }
	const RE::NiCamera* Camera() { return camera; }
	std::uint32_t FaceMask() { return faceMask; }
	bool Plain() { return plain; }
	void SetAfterFaces(AfterFaces a_callback) { afterFaces = a_callback; }
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
