#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class NiCamera;
	class NiFrustum;
	class NiTransform;
	class NiCullingProcess;
}

namespace DCLF
{
	/**
	 * @brief The sun's views as DCLF computes them (T2a, 2026-10-08): from the shadow cameras BSShadowDirectionalLight::UpdateCamera
	 * (0x141512230) has just set up, before any of the engine's culls or draws of them. Nothing here reads the engine's cascade culls
	 * (Accumulate's culling processes) or the renderer's state at a view's draw; those captures are the parity's alone.
	 *
	 * What UpdateCamera sets up and DCLF reads at its return: each cascade's camera (world transform, frustum and port), the caster
	 * volume it writes to the cascade's culling process (customCullPlanes), and the full-frustum camera. What DCLF derives, with the
	 * engine's own arithmetic (operation for operation, so that it agrees to the bit):
	 * - the view, projection and view-projection matrices BSGraphics::State::SetCameraData (0x140e56d70, FUN_140e58b80 ->
	 *   FUN_140e57ae0) builds, and the eye (posAdjust: the camera's world translation);
	 * - the planes each culling process tests (BSCullingProcess::Process2, 0x140e28690): NiCullingProcess::SetFrustum's (0x140d3fda0
	 *   -> FUN_140d32cc0) through its camera, replaced whole by its customCullPlanes when it has them (doCustomCullPlanes, which
	 *   UpdateCamera sets): each cascade's process (the sun bits' test) and each of the full-frustum cull's processes, which cull
	 *   through the full-frustum camera (FUN_141511f30; the sun's entry rule, kCullSunEntry);
	 * - the viewport Renderer::UpdateViewPort (0x140e441c0) makes of the port: its rectangle times the target's size.
	 *
	 * Render thread: written in the hook (CalculateActiveShadowCasterLights, before the full-frustum cull and Accumulate), read by
	 * the shadow views' capture and the inputs' preparation on the same thread; SunAccumulation's registration threads read the
	 * cascades only after its bitsReady release, which follows.
	 */
	class SunViews
	{
	public:
		static constexpr std::uint32_t kMaxCascades = 4;

		/** @brief Six planes (normal, constant: a sphere is outside when n.c - d < -r) and the active ones. */
		struct Planes
		{
			std::array<std::array<float, 4>, 6> plane{};
			std::uint32_t mask = 0;
		};

		struct Cascade
		{
			// Row-major, as the engine's camera data holds them (VS_PerFrame holds their transposes: Block).
			std::array<float, 16> view{}, proj{}, viewProj{};
			std::array<float, 3> eye{};   // posAdjust
			std::array<float, 4> port{};  // NiRect<float>: left, right, top, bottom (y up)
			Planes frustum;               // the camera's, as a culling process through it has them
			Planes caster;                // UpdateCamera's caster volume (the culling process's customCullPlanes); mask 0: none
			Planes cull;                  // what the cascade's cull tests (Process2): the caster volume when there is one, else the frustum
			bool valid = false;
		};

		struct Frame
		{
			std::uint32_t sceneFrame = ~0u;
			std::uint32_t count = 0;
			std::array<Cascade, kMaxCascades> cascades{};
			std::vector<Planes> fullFrusta;  // what each process of the full-frustum cull tests (Process2), by process
			bool valid = false;
		};

		static SunViews& Get();
		void Install();

		/** @brief This frame's views, once UpdateCamera has run for the sun (valid, sceneFrame the frame's). */
		const Frame& Current() const { return frame; }
		/** @brief The cascade a_descriptor of this frame's views, or null. */
		const Cascade* CascadeOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const;

		/** @brief NiCullingProcess::SetFrustum's planes: FUN_140d32cc0 (frustum, the camera's world transform). */
		static void FrustumPlanes(const RE::NiFrustum& a_frustum, const RE::NiTransform& a_world, Planes& a_out);
		/** @brief What a culling process through a_camera tests (BSCullingProcess::Process2): its custom planes, else the frustum's. */
		static void CullPlanes(const RE::NiCullingProcess* a_process, const RE::NiCamera& a_camera, Planes& a_out);
		/** @brief SetCameraData's matrices and eye for a_camera (FUN_140e57ae0). */
		static void CameraMatrices(const RE::NiCamera& a_camera, Cascade& a_out);
		/**
		 * @brief VS_PerFrame (b12) for a view drawn through a_cascade: CameraView, CameraProj and CameraViewProj (c0-c11, the
		 * transposes), CameraViewProjUnjittered (c12-c15, the same), CameraPosAdjust (c40); everything else zero (the shadow modes of
		 * Utility.hlsl read only the view-projection and the eye). a_out holds kBlockBytes.
		 */
		static constexpr std::uint32_t kBlockBytes = 45 * 16;
		static void Block(const Cascade& a_cascade, std::byte* a_out);
		/** @brief UpdateViewPort's viewport for a port on a a_width x a_height target: x, y, width, height. */
		static std::array<std::uint32_t, 4> Viewport(const std::array<float, 4>& a_port, std::uint32_t a_width, std::uint32_t a_height);

		/** @brief The parity's tallies (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY): DCLF's values against the engine's captures (LightViews keeps its own). */
		struct Parity
		{
			std::uint64_t views = 0, viewProj = 0, eye = 0, viewport = 0, casters = 0;   // differing views, by field
			std::uint64_t cascades = 0, cascadePlanes = 0, cascadeCasters = 0;           // the sun bits' cascades
			std::uint64_t fullFrusta = 0, fullFrustumPlanes = 0;                         // the full-frustum processes
			std::uint64_t missing = 0;                                                    // a sun view or cascade without DCLF's values
			float largest = 0.0f;
			std::string first;
			/** @brief A difference: the largest so far, and the first of each kind (up to four kinds) for the report. */
			void Note(const char* a_what, float a_size, std::string a_detail);
		};
		static bool ParityEnabled();
		Parity& GetParity() { return parity; }
		/** @brief Report thread (render): the parity's line, every a_every frames' worth; resets it. */
		void Report();
		/** @brief Within rounding: equal bits, or floats within 1e-5 relative (the planes' normalisation is a division). */
		static bool Close(float a_a, float a_b);
		static bool SamePlanes(const Planes& a_a, const Planes& a_b);
		/** @brief For the parity's first difference: for each of a_engine's planes, the index of a_mine's that matches it (- none), and both sets. */
		static std::string DescribePlanes(const Planes& a_mine, const Planes& a_engine);
		void NoteDifference(const char* a_what, float a_size, std::string a_detail);

	private:
		SunViews() = default;
		struct Hooks;
		friend struct Hooks;
		void Update(const void* a_light);

		Frame frame;
		Parity parity;
		bool installed = false;
		std::uint32_t reportFrames = 0;
	};
}
