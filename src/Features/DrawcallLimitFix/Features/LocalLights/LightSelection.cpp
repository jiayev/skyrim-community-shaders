// LightSelection: the CalculateActiveShadowCasterLights detour (render thread) and its port of the engine's selection.

#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContextAtlas.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		// Module offsets, AE 1.6.1170 and SE 1.5.97.
		constexpr Engine::Offset kCalculateActive{ 0x14cc570, 0x12e2f60 };  // CalculateActiveShadowCasterLights
		constexpr Engine::Offset kLodFadeCutoff{ 0x33dcfac, 0x3283b88 };    // UpdateCamera's lodFade cutoff
		constexpr Engine::Offset kFocusShadows{ 0x2032fd0, 0x1e0de43 };     // the focus shadows' setting byte
		constexpr std::size_t kPortalEntry = 0x30190;           // a culling process's portal-graph entry
		constexpr std::size_t kShadowMapCount = 0x140;          // BSShadowLight: its descriptors in use
		constexpr std::uint32_t kMaxLights = 4;

		using Corners = std::array<std::array<float, 3>, 5>;

		/** @brief FUN_14150aba0's corners of a camera's frustum: the eye, then the far rectangle's (+up+right, +up-right, -up+right, -up-right). */
		Corners FrustumCorners(const RE::NiCamera& a_camera)
		{
			const auto& r = a_camera.world.rotate.entry;
			const auto& t = a_camera.world.translate;
			const auto& f = const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().viewFrustum;
			const float farDistance = f.fFar;
			const float eye[3] = { t.x, t.y, t.z };
			float dir[3], up[3], right[3];
			for (std::uint32_t i = 0; i < 3; ++i) {
				dir[i] = r[i][0] * farDistance;
				up[i] = r[i][1] * f.fTop * farDistance;
				right[i] = r[i][2] * f.fRight * farDistance;
			}
			Corners out;
			out[0] = { eye[0], eye[1], eye[2] };
			for (std::uint32_t i = 0; i < 3; ++i) {
				const float centreUp = eye[i] + dir[i] + up[i];
				const float centreDown = eye[i] + dir[i] - up[i];
				out[1][i] = centreUp + right[i];
				out[2][i] = centreUp - right[i];
				out[3][i] = centreDown + right[i];
				out[4][i] = centreDown - right[i];
			}
			return out;
		}

		/** @brief FUN_14150aba0's test of one frustum's corners and edges against the other's six planes (all six, whatever the mask). */
		bool CornersOrEdgesInside(const Corners& a_points, const ShadowCameras::Planes& a_planes)
		{
			const auto& planes = a_planes.plane;
			// A corner strictly inside all six.
			for (const auto& p : a_points) {
				std::uint32_t k = 0;
				while (0.0f < (p[0] * planes[k][0] + p[1] * planes[k][1] + p[2] * planes[k][2]) - planes[k][3])
					if (++k > 5)
						return true;
			}
			// An edge crossing a plane at a point inside the other five (or lying in it).
			static constexpr std::uint8_t kEdges[8][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 } };
			const auto side = [](float a_value) -> std::uint8_t { return 0.0f <= a_value ? (0.0f < a_value ? 1 : 0) : 2; };
			for (const auto& edge : kEdges) {
				const auto& a = a_points[edge[0]];
				const auto& b = a_points[edge[1]];
				for (std::uint32_t j = 0; j < 6; ++j) {
					const auto& n = planes[j];
					const std::uint8_t sa = side((a[0] * n[0] + a[1] * n[1] + a[2] * n[2]) - n[3]);
					const std::uint8_t sb = side((b[0] * n[0] + b[1] * n[1] + b[2] * n[2]) - n[3]);
					if (sa == sb) {
						if (sa == 0)
							return true;
						continue;
					}
					const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
					const float t = ((n[0] * n[3] - a[0]) * n[0] + (n[3] * n[1] - a[1]) * n[1] + (n[3] * n[2] - a[2]) * n[2]) / (dx * n[0] + dy * n[1] + dz * n[2]);
					if (!(0.0f <= t) || !(t <= 1.0f))
						continue;
					std::uint32_t k = 0;
					while (k == j || 0.0f <= ((dy * t + a[1]) * planes[k][1] + (dx * t + a[0]) * planes[k][0] + (dz * t + a[2]) * planes[k][2]) - planes[k][3])
						if (++k > 5)
							return true;
				}
			}
			return false;
		}

		/** @brief FUN_14150aba0 (light, world camera, the light's camera): whether the two frusta intersect. */
		bool FrustaIntersect(const RE::NiCamera& a_world, const RE::NiCamera& a_light)
		{
			ShadowCameras::Planes worldPlanes, lightPlanes;
			ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera&>(a_world).GetRuntimeData2().viewFrustum, a_world.world, worldPlanes);
			ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera&>(a_light).GetRuntimeData2().viewFrustum, a_light.world, lightPlanes);
			return CornersOrEdgesInside(FrustumCorners(a_world), lightPlanes) || CornersOrEdgesInside(FrustumCorners(a_light), worldPlanes);
		}

		/** @brief BSMultiBoundSphere::Func41: a sphere inside a frustum's active planes (outside when n.c - d <= -r, or unordered). */
		bool SphereInside(const float a_centre[3], float a_radius, const ShadowCameras::Planes& a_planes)
		{
			for (std::uint32_t k = 0; k < 6; ++k) {
				if (!((a_planes.mask >> k) & 1))
					continue;
				const auto& n = a_planes.plane[k];
				const float value = (n[1] * a_centre[1] + n[0] * a_centre[0] + n[2] * a_centre[2]) - n[3];
				if (!(value > -a_radius))
					return false;
			}
			return true;
		}

		/** @brief vfunc 0x10's verdict (BSShadowFrustumLight 0x14151ac50, BSShadowParabolicLight 0x14151b620; a directional one's is true). */
		bool Visible(const RE::BSShadowLight& a_light, const RE::NiCamera& a_camera, const ShadowCameras::Planes& a_cameraPlanes)
		{
			auto& light = const_cast<RE::BSShadowLight&>(a_light);
			const auto* niLight = light.light.get();
			if (light.GetIsDirectionalLight())
				return true;
			if (!niLight || (niLight->GetFlags().underlying() & 1))
				return false;
			const bool frustum = light.GetIsFrustumLight();
			if (!frustum && !light.GetIsParabolicLight())
				return false;
			const auto& position = niLight->world.translate;
			const float radius = const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData().radius.x;
			bool visible;
			if (frustum) {
				const auto& descriptors = light.GetRuntimeData().shadowmapDescriptors;
				const auto* camera = descriptors.empty() ? nullptr : descriptors[0].camera.get();
				visible = camera && FrustaIntersect(a_camera, *camera);
			} else {
				const float centre[3] = { position.x, position.y, position.z };
				visible = SphereInside(centre, radius, a_cameraPlanes);
			}
			if (visible && light.lodFade) {
				const float dx = position.x - a_camera.world.translate.x;
				const float dy = position.y - a_camera.world.translate.y;
				const float dz = position.z - a_camera.world.translate.z;
				// Each class's own order of the sum.
				const float squared = frustum ? (dy * dy + dx * dx) + dz * dz : (dx * dx + dy * dy) + dz * dz;
				if (Global<float>(kLodFadeCutoff) <= (squared - radius * radius) * At<float>(&a_camera, 0x184))
					visible = false;
			}
			return visible;
		}

		RE::ShadowSceneNode* SceneNode() { return Global<RE::ShadowSceneNode*>(Engine::Address::kShadowSceneNode); }
		const RE::NiCamera* WorldCamera()
		{
			const auto* graph = Global<const std::byte*>(Engine::Address::kWorldSceneGraph);
			return graph ? At<const RE::NiCamera*>(graph, 0x128) : nullptr;
		}
	}

	LightSelection& LightSelection::Get()
	{
		static LightSelection instance;
		return instance;
	}

	bool LightSelection::SharesRoomWithCamera(const RE::BSShadowLight* a_light)
	{
		// The engine's condition first: descriptor 0's culling process and its entry exist (allocation, not a cull); then the
		// light's rooms as DCLF walks them this frame (PortalViews::WalkLight) against the world camera's.
		auto* light = const_cast<RE::BSShadowLight*>(a_light);
		const auto& descriptors = light->GetRuntimeData().shadowmapDescriptors;
		const auto* process = descriptors.empty() ? nullptr : descriptors[0].cullingProcess;
		if (!process || !At<const void*>(process, kPortalEntry))
			return false;
		auto& portals = PortalViews::Get();
		auto& rooms = lightRooms[a_light];
		portals.WalkLight(a_light, rooms);
		const bool shares = portals.SharesRoom(rooms);
		roomVerdicts[a_light] = shares;
		return shares;
	}

	void LightSelection::Select()
	{
		// The slots' board first: an early return leaves it invalid (no views this frame), as the frame below.
		ShadowViews::LightSlots().Invalidate();
		// A second selection in the scene frame keeps the first's lights and atlas entries (Frame::FrameLight).
		const std::uint32_t sceneFrame = Scene::FrameView::Get().GetFrame();
		std::vector<FrameLight> frameLights = frame.sceneFrame == sceneFrame ? std::move(frame.frameLights) : std::vector<FrameLight>{};
		const std::uint32_t selections = frame.sceneFrame == sceneFrame ? frame.selections + 1 : 1;
		frame = {};
		frame.sceneFrame = sceneFrame;
		frame.selections = selections;
		frame.frameLights = std::move(frameLights);
		auto* node = SceneNode();
		const auto* camera = WorldCamera();
		if (!node || !camera)
			return;
		auto& runtime = node->GetRuntimeData();
		std::uint32_t kept = 0, slot = 0, mask = 0;
		// The focus shadows: on when the setting is and there is a target; the host found so far (0x14338c912).
		const bool focusOn = Global<std::uint8_t>(kFocusShadows) != 0 && Global<std::uint32_t>(Engine::Address::kFocusTargetCount) != 0;
		bool focusFound = false;
		if (!Global<std::uint8_t>(Engine::Address::kSunOff) && runtime.sunShadowDirLight) {
			frame.sun = runtime.sunShadowDirLight;
			frame.sunSlots = At<std::uint32_t>(frame.sun, kShadowMapCount);
			frame.slots.assign(frame.sunSlots, frame.sun);
			kept = 1;
			slot = frame.sunSlots;
			mask = 1;
			// CalculateAndDrawShadowCasterLights sets the sun's flag to the setting; CalculateActiveShadowCasterLights makes it the host.
			focusFlags[frame.sun] = focusOn;
			if (focusOn) {
				focusFound = true;
				lastFocusHost = nullptr;
			}
		}
		// No host yet: last frame's keeps it while it is a candidate - each candidate's flag is whether it is that one, and the
		// running verdict is the last candidate's (the engine's loop, as it is).
		if (focusOn && !focusFound && !runtime.activeShadowLights.empty()) {
			for (const auto& pointer : runtime.activeShadowLights) {
				if (const auto* light = pointer.get()) {
					focusFound = light == lastFocusHost && !focusFound;
					focusFlags[light] = focusFound;
				}
			}
		}
		lastFocusHost = nullptr;
		roomVerdicts.clear();
		ShadowCameras::Planes cameraPlanes;
		ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, cameraPlanes);
		for (const auto& pointer : runtime.activeShadowLights) {
			const auto* light = pointer.get();
			if (!light || kept >= kMaxLights)
				continue;
			if (!Visible(*light, *camera, cameraPlanes) || !SharesRoomWithCamera(light))
				continue;
			// Kept: the engine walks it here (FUN_1414a2530), its walk's edge flags left on the shapes.
			PortalViews::Get().CommitLightWalk();
			const auto& rooms = lightRooms.at(light);
			frame.locals.push_back({ light, slot, mask, rooms.context, ~0u, rooms.walk });
			// The focus host: a light with the flag, or the first that can host (vfunc 0x20: directional, spot) while none has.
			{
				auto& flag = focusFlags[light];
				auto* candidate = const_cast<RE::BSShadowLight*>(light);
				if (flag || (!focusFound && (candidate->GetIsDirectionalLight() || candidate->GetIsFrustumLight()))) {
					flag = true;
					focusFound = true;
					lastFocusHost = light;
				}
			}
			// FUN_1414a3fb0 writes the light into one slot; its Accumulate takes one per hemisphere (BSShadowParabolicLight with two
			// shadow maps: 2), one for a spot light, one per cascade for a directional one. The slots past the first keep what they
			// held (not this frame's: a gap here).
			auto* shadowLight = const_cast<RE::BSShadowLight*>(light);
			const std::uint32_t maps = At<std::uint32_t>(light, kShadowMapCount);
			const std::uint32_t taken = shadowLight->GetIsParabolicLight() ? (maps == 2 ? 2u : 1u) : shadowLight->GetIsDirectionalLight() ? maps :
			                                                                                                                                1u;
			frame.slots.push_back(light);
			frame.slots.resize(frame.slots.size() + (taken ? taken - 1 : 0), nullptr);
			slot += taken;
			++mask;
			++kept;
		}
		// The drawn lights' focus flags: their Render draws focus views 0..count-1.
		frame.focusCount = std::min<std::uint32_t>(Global<std::uint32_t>(Engine::Address::kFocusTargetCount), 4u);
		if (frame.sun && focusFlags[frame.sun])
			frame.focusHosts.push_back(frame.sun);
		for (const auto& selected : frame.locals)
			if (selected.light != frame.sun && focusFlags[selected.light])
				frame.focusHosts.push_back(selected.light);
		// Lights that are not candidates leave the map (a freed light's address may be reused).
		if (focusFlags.size() > 64) {
			std::unordered_map<const RE::BSShadowLight*, bool> live;
			for (const auto& pointer : runtime.activeShadowLights)
				if (const auto it = focusFlags.find(pointer.get()); it != focusFlags.end())
					live.insert(*it);
			if (frame.sun)
				live.insert_or_assign(frame.sun, focusFlags[frame.sun]);
			focusFlags = std::move(live);
		}
		if (lightRooms.size() > 64) {
			std::unordered_map<const RE::BSShadowLight*, PortalViews::LightRooms> live;
			for (const auto& pointer : runtime.activeShadowLights)
				if (const auto it = lightRooms.find(pointer.get()); it != lightRooms.end())
					live.insert(std::move(*it));
			lightRooms = std::move(live);
		}
		// The frame's lights: this selection's join the earlier ones' (a light kept again takes this walk's context), then the atlas of the
		// camera and every frame light's context, entries in first-kept order.
		for (const auto& selected : frame.locals) {
			auto it = std::find_if(frame.frameLights.begin(), frame.frameLights.end(), [&](const auto& a_entry) { return a_entry.light == selected.light; });
			if (it == frame.frameLights.end())
				it = frame.frameLights.insert(frame.frameLights.end(), FrameLight{ selected.light });
			it->portalContext = selected.portalContext;
			it->portalWalk = selected.portalWalk;
		}
		std::vector<Portals::ContextHold> contexts;
		if (auto cameraContext = PortalViews::Get().MainContext(); cameraContext && cameraContext->frame == frame.sceneFrame)
			contexts.push_back(std::move(cameraContext));
		for (auto& entry : frame.frameLights) {
			entry.portalContextIndex = ~0u;
			if (entry.portalContext) {
				entry.portalContextIndex = static_cast<std::uint32_t>(contexts.size());
				contexts.push_back(entry.portalContext);
			}
		}
		for (auto& selected : frame.locals)
			if (const auto* entry = frame.LightOf(selected.light))
				selected.portalContextIndex = entry->portalContextIndex;
		// Render thread; identity also changes if selection runs twice in one scene frame. Zero is never an identity.
		static std::uint64_t publication = 0;
		if (++publication == 0)
			++publication;
		// Inside the engine's detour: an atlas that cannot be encoded is not published (the commits then publish none).
		try {
			frame.portalContexts = Portals::EncodeContexts(contexts, publication);
		} catch (const std::exception& e) {
			frame.portalContexts.clear();
			for (auto& selected : frame.locals)
				selected.portalContextIndex = ~0u;
			for (auto& entry : frame.frameLights)
				entry.portalContextIndex = ~0u;
			logger::error("[DCLF][structural-gap] point-light context atlas not encoded at frame {}: {}", frame.sceneFrame, e.what());
		}
		frame.valid = true;
		// The slots and focus hosts to the shadow views (the LightSlots board: spans into frame, stable until the next Select).
		ShadowViews::LightSlots().Publish(frame.sceneFrame, frame.slots, frame.focusHosts, frame.focusCount);
		TellKeptLights(frame);
		Diagnostics::Notify<Diagnostics::EnginePoint::LightSelectionMade>();
	}

	void LightSelection::TellKeptLights(const Frame& a_frame)
	{
		// Their views, from the cameras their UpdateCamera has set up: each kept spot light to the Spot source, each point light to the
		// Point source (Api::ShadowSourceContribution::lightKept), in the selection's order.
		const auto& spot = Extensions::ShadowSourceOf(Api::ShadowSource::Spot);
		const auto& point = Extensions::ShadowSourceOf(Api::ShadowSource::Point);
		for (const auto& selected : a_frame.locals) {
			auto* light = const_cast<RE::BSShadowLight*>(selected.light);
			const auto* source = light->GetIsFrustumLight() ? &spot : light->GetIsParabolicLight() ? &point :
			                                                                                         nullptr;
			if (source && source->lightKept)
				source->lightKept({ selected.light, a_frame.sceneFrame });
		}
	}

	struct LightSelection::Hooks
	{
		struct CalculateActive
		{
			static void thunk()
			{
				auto& selection = LightSelection::Get();
				Diagnostics::Notify<Diagnostics::EnginePoint::LightSelectionBegin>();
				func();
				selection.Select();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void LightSelection::Install()
	{
		if (installed)
			return;
		if (!kCalculateActive.Known()) {
			logger::warn("[DCLF] light selection: CalculateActiveShadowCasterLights is not known for this runtime; the engine selects the lights");
			return;
		}
		stl::detour_thunk<Hooks::CalculateActive>(REL::Offset(kCalculateActive.Get()).address());
		installed = true;
		logger::info("[DCLF] light selection: CalculateActiveShadowCasterLights detoured (T3b)");
	}
}
