#pragma once

// The shadow views' cameras as DCLF computes them: the ports of the engine's camera, plane and viewport arithmetic, and the
// render-thread boards the shadow sources' camera hooks write (the sun's cascades, the kept local lights' views, the focus views) and
// the shadow engine reads (the views' capture, the inputs' preparation, the sun's accumulation, the local shadows, the actors' rate).

#include <array>
#include <cstddef>
#include <cstdint>
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

	/**
	 * @brief The sun's views: from the shadow cameras BSShadowDirectionalLight::UpdateCamera (0x141512230) has just set up, before any
	 * of the engine's culls or draws of them: each cascade's camera (its caster volume, written to the cascade's culling process, the
	 * sun bits' test), and what each process of the full-frustum cull tests, through the full-frustum camera (FUN_141511f30; the sun's
	 * entry rule, kCullSunEntry). Nothing here reads the engine's cascade culls or the renderer's state at a view's draw; those captures
	 * are the observers' alone (Diagnostics::ShadowViewTally).
	 *
	 * Render thread: written by the sun's camera hook (CalculateActiveShadowCasterLights, before the full-frustum cull and Accumulate),
	 * read by the shadow views' capture and the inputs' preparation on the same thread; SunAccumulation's registration threads read the
	 * cascades only after its bitsReady release, which follows.
	 */
	struct SunFrame
	{
		static constexpr std::uint32_t kMaxCascades = 4;

		std::uint32_t sceneFrame = ~0u;
		std::uint32_t count = 0;
		std::array<Camera, kMaxCascades> cascades{};
		std::vector<Planes> fullFrusta;  // what each process of the full-frustum cull tests (Process2), by process
		bool valid = false;

		/** @brief Cascade a_descriptor of this frame's views (valid, sceneFrame the frame's), or null. */
		const Camera* CascadeOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (!valid || sceneFrame != a_sceneFrame || a_descriptor >= count || !cascades[a_descriptor].valid)
				return nullptr;
			return &cascades[a_descriptor];
		}
	};

	/**
	 * @brief The local shadow lights' views: spot (BSShadowFrustumLight) and point (BSShadowParabolicLight, one view per hemisphere)
	 * lights, from the cameras their UpdateCamera has set up, for the lights DCLF selects (LightSelection: the lights it kept, at
	 * CalculateActiveShadowCasterLights' return). Per descriptor: the camera, and the planes the light's culling process tests.
	 *
	 * Render thread: written as the selection keeps each light, read by the shadow views' capture and LocalShadowLights later in the
	 * frame; SideEffects' actor rate reads the last scene frame's.
	 */
	struct LightFrame
	{
		struct Light
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<Camera> views;      // by descriptor
			std::array<float, 4> sphere{};  // the light's position and radius (radius 0: none)
		};
		std::vector<Light> lights;  // this frame's, in the selection's order
		std::uint32_t sceneFrame = ~0u;

		/** @brief Descriptor a_descriptor of a_light's views this frame, or null (the light not selected this frame). */
		const Camera* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (a_sceneFrame != sceneFrame)
				return nullptr;
			for (const auto& entry : lights)
				if (entry.light == a_light)
					return a_descriptor < entry.views.size() && entry.views[a_descriptor].valid ? &entry.views[a_descriptor] : nullptr;
			return nullptr;
		}

		/**
		 * @brief For SideEffects' actor rate: every view of the lights kept in scene frame a_sceneFrame (none when the views are older), with
		 * its light's sphere (centre, radius: what a point light's paraboloid cull tests, its views having no planes).
		 */
		template <class F>
		void ForEachView(std::uint32_t a_sceneFrame, F&& a_visit) const
		{
			if (sceneFrame != a_sceneFrame)
				return;
			for (const auto& entry : lights)
				for (const auto& view : entry.views)
					if (view.valid)
						a_visit(view, entry.sphere);
		}
	};

	/**
	 * @brief The focus shadows' views: from the cameras BSShadowDirectionalLight::sub (0x1414f0480) has just aimed at the focus targets,
	 * for whichever light hosts them. Per focus descriptor below the target count: the camera (no caster volume: the focus accumulation,
	 * FUN_1414f0b90, culls nothing), and target i's node, whose subtree the engine registers whole (DCLF's casters for the view are the
	 * members whose fade root is that node: BuildDrawsLatch::focusRoot).
	 *
	 * Render thread: written by the focus camera hook (from CalculateAndDrawShadowCasterLights and CalculateActiveShadowCasterLights),
	 * read by the shadow views' capture later in the frame.
	 */
	struct FocusFrame
	{
		static constexpr std::uint32_t kMaxFocus = 4;  // focusShadowmapDescriptors

		struct Host
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<Camera> views;  // by focus descriptor
		};
		std::vector<Host> hosts;  // this frame's
		std::array<const RE::NiAVObject*, kMaxFocus> targets{};
		std::uint32_t targetCount = 0;
		std::uint32_t sceneFrame = ~0u;

		/** @brief a_light's focus view a_descriptor this frame, or null (its sub did not run this frame, or past the targets). */
		const Camera* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (a_sceneFrame != sceneFrame)
				return nullptr;
			for (const auto& host : hosts)
				if (host.light == a_light)
					return a_descriptor < host.views.size() && host.views[a_descriptor].valid ? &host.views[a_descriptor] : nullptr;
			return nullptr;
		}
		/** @brief Focus target a_descriptor's node this frame (the casters' root), or null. */
		const RE::NiAVObject* TargetOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			return a_sceneFrame == sceneFrame && a_descriptor < targetCount ? targets[a_descriptor] : nullptr;
		}
	};

	/**
	 * @brief The engine's current focus targets (0x14332a488: {float distance; NiAVObject* node} x the count at 0x14332a498): target
	 * a_index's node, or null past the count. What the focus camera hook records (FocusFrame::targets) and the focus parity compares
	 * the registrations with. Any thread that reads the engine's focus state.
	 */
	const RE::NiAVObject* EngineFocusTarget(std::uint32_t a_index);
	std::uint32_t EngineFocusTargetCount();

	/** @brief The boards (render thread; see each). Written in place by the sources' camera hooks: their storage is reused. */
	SunFrame& Sun();
	LightFrame& Lights();
	FocusFrame& Focus();
}
