#include "ReflectionFaces.h"

#include "Features/DrawcallLimitFix/Engine/PassCapture.h"

namespace DCLF::ReflectionFaces
{
	namespace
	{
		// Render thread only: the face render runs there, and so does everything that asks.
		const RE::NiCamera* camera = nullptr;
		std::uint32_t faceMask = 0;
		bool plain = false;
		AfterFaces afterFaces = nullptr;
		AfterFaceDraws afterFaceDraws = nullptr;

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
		stl::write_thunk_call<AccumulatorRender>(REL::Offset(0x14edbf8).address());
		logger::info("[DCLF] reflection face hook installed on BSCubeMapCamera's face render (vfunc 0x35)");
	}

	bool InFace() { return camera != nullptr; }
	const RE::NiCamera* Camera() { return camera; }
	std::uint32_t FaceMask() { return faceMask; }
	bool Plain() { return plain; }
	void SetAfterFaces(AfterFaces a_callback) { afterFaces = a_callback; }
	void SetAfterFaceDraws(AfterFaceDraws a_callback) { afterFaceDraws = a_callback; }
}
