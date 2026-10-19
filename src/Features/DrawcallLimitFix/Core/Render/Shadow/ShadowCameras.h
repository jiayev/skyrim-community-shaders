#pragma once

// The cameras as DCLF computes them: the ports of the engine's camera, plane and viewport arithmetic, self-contained, for every view
// that derives its own camera (the shadow views' capture, the sources' views, the portals' and the light selection's planes, the
// reflection's and first person's blocks, the parities). The boards those cameras are written into are the features' that write them
// (Features/Shadows/Sun's SunViews, Features/Shadows/Local's LightViews and FocusViews), which Core reads through the shadow sources'
// cameras and the sun volume (Api/ShadowViews.h, Api/SunVolume.h); the light selection's slots are the views list's input
// (Core/Handoff/ShadowViews.h, LightSlotFrame).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
	class NiCamera;
	class NiFrustum;
	class NiTransform;
	class NiCullingProcess;
}

namespace DCLF::ShadowCameras
{
	/** @brief Six planes (normal, constant: a sphere is outside when n.c - d < -r) and the active ones. */
	struct Planes
	{
		std::array<std::array<float, 4>, 6> plane{};
		std::uint32_t mask = 0;
	};

	/**
	 * @brief A camera as DCLF derives it, with the engine's own arithmetic (operation for operation, so that it agrees to the bit):
	 * the view, projection and view-projection matrices BSGraphics::State::SetCameraData (0x140e56d70, FUN_140e58b80 -> FUN_140e57ae0)
	 * builds, the eye (posAdjust: the camera's world translation), the port, and the planes a culling process through it tests.
	 */
	struct Camera
	{
		// Row-major, as the engine's camera data holds them (VS_PerFrame holds their transposes: Block).
		std::array<float, 16> view{}, proj{}, viewProj{};
		std::array<float, 3> eye{};   // posAdjust
		std::array<float, 4> port{};  // NiRect<float>: left, right, top, bottom (y up)
		Planes frustum;               // the camera's, as a culling process through it has them
		Planes caster;                // UpdateCamera's caster volume (the culling process's customCullPlanes); mask 0: none
		Planes cull;                  // what the camera's cull tests (Process2): the caster volume when there is one, else the frustum
		// A portal-strict point light's view: its light's entry in the frame's light context atlas + 1 (BuildDrawsLatch::strictContext), whose
		// strict gates its casters must pass. 0: not gated.
		std::uint32_t portalContext = 0;
		bool valid = false;
	};

	/**
	 * @brief NiCullingProcess::SetFrustum's planes: FUN_140d32cc0 (frustum, the camera's world transform). The planes each culling
	 * process tests (BSCullingProcess::Process2, 0x140e28690) are these through its camera, replaced whole by its customCullPlanes
	 * when it has them (doCustomCullPlanes).
	 */
	void FrustumPlanes(const RE::NiFrustum& a_frustum, const RE::NiTransform& a_world, Planes& a_out);
	/** @brief What a culling process through a_camera tests (BSCullingProcess::Process2): its custom planes, else the frustum's. */
	void CullPlanes(const RE::NiCullingProcess* a_process, const RE::NiCamera& a_camera, Planes& a_out);
	/** @brief SetCameraData's matrices and eye for a_camera (FUN_140e57ae0), and its port. */
	void CameraMatrices(const RE::NiCamera& a_camera, Camera& a_out);
	/**
	 * @brief VS_PerFrame (b12) for a view drawn through a_camera: CameraView, CameraProj and CameraViewProj (c0-c11, the transposes),
	 * CameraViewProjUnjittered (c12-c15, the same), CameraPosAdjust (c40); everything else zero (the shadow modes of Utility.hlsl read
	 * only the view-projection and the eye). a_out holds kBlockBytes.
	 */
	inline constexpr std::uint32_t kBlockBytes = 45 * 16;
	void Block(const Camera& a_camera, std::byte* a_out);
	/** @brief Renderer::UpdateViewPort's (0x140e441c0) viewport for a port on a a_width x a_height target: x, y, width, height. */
	std::array<std::uint32_t, 4> Viewport(const std::array<float, 4>& a_port, std::uint32_t a_width, std::uint32_t a_height);

	/** @brief Within rounding: equal bits, or floats within 1e-5 relative (the planes' normalisation is a division). */
	bool Close(float a_a, float a_b);
	bool SamePlanes(const Planes& a_a, const Planes& a_b);
	/** @brief For a comparison's first difference: for each of a_engine's planes, the index of a_mine's that matches it (- none), and both sets. */
	std::string DescribePlanes(const Planes& a_mine, const Planes& a_engine);
}
