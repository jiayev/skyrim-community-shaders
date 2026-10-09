#include "LightSelection.h"

#include "EngineAccess.h"
#include "LightViews.h"
#include "SunViews.h"

#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include "State.h"

namespace DCLF
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		// AE 1.6.1170 module offsets.
		constexpr std::uintptr_t kCalculateActive = 0x14cc570;  // CalculateActiveShadowCasterLights
		constexpr std::uintptr_t kShadowSceneNode = 0x2033060;  // the ShadowSceneNode it reads (BSShaderManager::State's first)
		constexpr std::uintptr_t kWorldSceneGraph = 0x338c8e8;  // the world scene graph; its camera at +0x128
		constexpr std::uintptr_t kListProcesses = 0x338c8a0;    // the scene lists' culling processes (an array, by pointer)
		constexpr std::uintptr_t kSunOff = 0x338c911;           // set: the sun does not draw
		constexpr std::uintptr_t kLightCount = 0x338c900;       // the kept lights, the sun among them (the function's last store)
		constexpr std::uintptr_t kSlotCounter = 0x338c904;      // the next shadowLightsAccum slot; the next mask index at +4
		constexpr std::uintptr_t kLodFadeCutoff = 0x33dcfac;    // UpdateCamera's lodFade cutoff
		constexpr std::uintptr_t kFocusShadows = 0x2032fd0;     // the focus shadows' setting byte
		constexpr std::uintptr_t kFocusTargets = 0x332a498;     // the focus targets' count
		constexpr std::uintptr_t kFocusHost = 0x33dcfb8;        // last frame's focus host (non-sun)
		constexpr std::size_t kPortalEntry = 0x30190;           // a culling process's portal-graph entry
		constexpr std::size_t kShadowMapCount = 0x140;          // BSShadowLight: its descriptors in use
		constexpr std::uint32_t kMaxLights = 4;

		/** @brief FUN_140e14300: whether two portal-graph entries share a room, or both see every room (+0x130). */
		bool SharesRoom(const void* a_light, const void* a_camera)
		{
			if (At<std::uint8_t>(a_light, 0x130) && At<std::uint8_t>(a_camera, 0x130))
				return true;
			const std::uint32_t lightRooms = At<std::uint32_t>(a_light, 0x28), cameraRooms = At<std::uint32_t>(a_camera, 0x28);
			if (!lightRooms || !cameraRooms)
				return false;
			const auto* light = At<const void* const*>(a_light, 0x18);
			const auto* camera = At<const void* const*>(a_camera, 0x18);
			for (std::uint32_t i = 0; i < lightRooms; ++i)
				for (std::uint32_t j = 0; j < cameraRooms; ++j)
					if (camera[j] == light[i])
						return true;
			return false;
		}

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
		bool CornersOrEdgesInside(const Corners& a_points, const SunViews::Planes& a_planes)
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
			SunViews::Planes worldPlanes, lightPlanes;
			SunViews::FrustumPlanes(const_cast<RE::NiCamera&>(a_world).GetRuntimeData2().viewFrustum, a_world.world, worldPlanes);
			SunViews::FrustumPlanes(const_cast<RE::NiCamera&>(a_light).GetRuntimeData2().viewFrustum, a_light.world, lightPlanes);
			return CornersOrEdgesInside(FrustumCorners(a_world), lightPlanes) || CornersOrEdgesInside(FrustumCorners(a_light), worldPlanes);
		}

		/** @brief BSMultiBoundSphere::Func41: a sphere inside a frustum's active planes (outside when n.c - d <= -r, or unordered). */
		bool SphereInside(const float a_centre[3], float a_radius, const SunViews::Planes& a_planes)
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
		bool Visible(const RE::BSShadowLight& a_light, const RE::NiCamera& a_camera, const SunViews::Planes& a_cameraPlanes)
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

		RE::ShadowSceneNode* SceneNode() { return Global<RE::ShadowSceneNode*>(kShadowSceneNode); }
		const RE::NiCamera* WorldCamera()
		{
			const auto* graph = Global<const std::byte*>(kWorldSceneGraph);
			return graph ? At<const RE::NiCamera*>(graph, 0x128) : nullptr;
		}
	}

	LightSelection& LightSelection::Get()
	{
		static LightSelection instance;
		return instance;
	}

	void LightSelection::SampleRooms()
	{
		rooms.clear();
		auto* node = SceneNode();
		const auto* processes = Global<const std::byte* const*>(kListProcesses);
		const auto* cameraEntry = processes && processes[0] ? At<const void*>(processes[0], kPortalEntry) : nullptr;
		if (!node)
			return;
		for (const auto& pointer : node->GetRuntimeData().activeShadowLights) {
			auto* light = pointer.get();
			if (!light)
				continue;
			const auto& descriptors = light->GetRuntimeData().shadowmapDescriptors;
			const auto* process = descriptors.empty() ? nullptr : descriptors[0].cullingProcess;
			const auto* entry = process ? At<const void*>(process, kPortalEntry) : nullptr;
			rooms[light] = entry && cameraEntry && SharesRoom(entry, cameraEntry);
		}
	}

	void LightSelection::Select()
	{
		frame = {};
		frame.sceneFrame = SceneStore::Get().GetFrame();
		auto* node = SceneNode();
		const auto* camera = WorldCamera();
		if (!node || !camera)
			return;
		auto& runtime = node->GetRuntimeData();
		std::uint32_t kept = 0, slot = 0, mask = 0;
		// The focus shadows (T3c): on when the setting is and there is a target; the host found so far (0x14338c912).
		const bool focusOn = Global<std::uint8_t>(kFocusShadows) != 0 && Global<std::uint32_t>(kFocusTargets) != 0;
		bool focusFound = false;
		if (!Global<std::uint8_t>(kSunOff) && runtime.sunShadowDirLight) {
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
		SunViews::Planes cameraPlanes;
		SunViews::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, cameraPlanes);
		for (const auto& pointer : runtime.activeShadowLights) {
			const auto* light = pointer.get();
			if (!light || kept >= kMaxLights)
				continue;
			const auto room = rooms.find(light);
			if (room == rooms.end() || !room->second || !Visible(*light, *camera, cameraPlanes))
				continue;
			frame.locals.push_back({ light, slot, mask });
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
			const std::uint32_t taken = shadowLight->GetIsParabolicLight() ? (maps == 2 ? 2u : 1u) : shadowLight->GetIsDirectionalLight() ? maps : 1u;
			frame.slots.push_back(light);
			frame.slots.resize(frame.slots.size() + (taken ? taken - 1 : 0), nullptr);
			slot += taken;
			++mask;
			++kept;
		}
		// The drawn lights' focus flags: their Render draws focus views 0..count-1.
		frame.focusCount = std::min<std::uint32_t>(Global<std::uint32_t>(kFocusTargets), 4u);
		if (frame.sun && focusFlags[frame.sun])
			frame.focusHosts.push_back(frame.sun);
		for (const auto& selected : frame.locals)
			if (selected.light != frame.sun && focusFlags[selected.light])
				frame.focusHosts.push_back(selected.light);
		// Lights no longer candidates leave the map (a freed light's address may be reused).
		if (focusFlags.size() > 64) {
			std::unordered_map<const RE::BSShadowLight*, bool> live;
			for (const auto& pointer : runtime.activeShadowLights)
				if (const auto it = focusFlags.find(pointer.get()); it != focusFlags.end())
					live.insert(*it);
			if (frame.sun)
				live.insert_or_assign(frame.sun, focusFlags[frame.sun]);
			focusFlags = std::move(live);
		}
		frame.valid = true;
		// Their views, from the cameras their UpdateCamera has set up (T3a).
		for (const auto& selected : frame.locals) {
			auto* light = const_cast<RE::BSShadowLight*>(selected.light);
			if (light->GetIsFrustumLight() || light->GetIsParabolicLight())
				LightViews::Get().Update(selected.light);
		}
		if (SunViews::ParityEnabled())
			CheckParity();
	}

	void LightSelection::CheckParity()
	{
		auto& p = parity;
		++p.frames;
		p.kept += frame.locals.size();
		auto* node = SceneNode();
		const auto& accum = node->GetRuntimeData().shadowLightsAccum;
		const std::uint32_t engineSlots = Global<std::uint32_t>(kSlotCounter);
		const std::uint32_t engineCount = Global<std::uint32_t>(kLightCount);
		std::string detail;
		const std::uint32_t mine = (frame.sun ? 1u : 0u) + static_cast<std::uint32_t>(frame.locals.size());
		bool differ = false;
		if (engineCount != mine || engineSlots != frame.slots.size()) {
			++p.countsDiffer;
			differ = true;
			detail = fmt::format("{} lights in {} slots, the engine's {} in {}", mine, frame.slots.size(), engineCount, engineSlots);
		}
		for (std::uint32_t s = 0; s < std::max<std::uint32_t>(engineSlots, static_cast<std::uint32_t>(frame.slots.size())); ++s) {
			const RE::BSShadowLight* engine = s < engineSlots && s < accum.size() ? accum[s] : nullptr;
			const RE::BSShadowLight* ours = s < frame.slots.size() ? frame.slots[s] : nullptr;
			// A gap is not this frame's: whatever the slot held.
			if (s < frame.slots.size() && !ours)
				continue;
			if (engine != ours) {
				++p.slotsDiffer;
				differ = true;
				if (detail.empty()) {
					const auto describe = [&](const RE::BSShadowLight* a_light) -> std::string {
						if (!a_light)
							return "none";
						auto* light = const_cast<RE::BSShadowLight*>(a_light);
						const auto* niLight = light->light.get();
						const auto room = rooms.find(a_light);
						return fmt::format("{} ({}{}, room {})", static_cast<const void*>(a_light),
							light->GetIsDirectionalLight() ? "directional" : light->GetIsFrustumLight() ? "spot" : light->GetIsParabolicLight() ? "point" : "other",
							niLight ? fmt::format(" at ({:.0f} {:.0f} {:.0f}) r {:.0f}", niLight->world.translate.x, niLight->world.translate.y, niLight->world.translate.z,
										  const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData().radius.x) :
									  "",
							room == rooms.end() ? "unsampled" : room->second ? "shared" : "not shared");
					};
					detail = fmt::format("slot {}: {}, the engine's {}", s, describe(ours), describe(engine));
				}
			}
		}
		// The focus flags of the drawn lights, and last frame's host as the engine left it.
		{
			p.focusHosts += frame.focusHosts.size();
			const auto checkFocus = [&](const RE::BSShadowLight* a_light) {
				const bool engine = const_cast<RE::BSShadowLight*>(a_light)->GetRuntimeData().drawFocusShadows;
				const auto it = focusFlags.find(a_light);
				const bool ours = it != focusFlags.end() && it->second;
				if (engine != ours) {
					++p.focusDiffer;
					differ = true;
					if (detail.empty())
						detail = fmt::format("focus flag {} for {}, the engine's {}", ours, static_cast<const void*>(a_light), engine);
				}
			};
			if (frame.sun)
				checkFocus(frame.sun);
			for (const auto& selected : frame.locals)
				checkFocus(selected.light);
			if (Global<const void*>(kFocusHost) != lastFocusHost) {
				++p.focusDiffer;
				differ = true;
				if (detail.empty())
					detail = fmt::format("focus host {}, the engine's {}", static_cast<const void*>(lastFocusHost), Global<const void*>(kFocusHost));
			}
		}
		for (const auto& selected : frame.locals) {
			const std::uint32_t engineMask = const_cast<RE::BSShadowLight*>(selected.light)->GetRuntimeData().maskIndex;
			if (engineMask != selected.maskIndex) {
				++p.masksDiffer;
				differ = true;
				if (detail.empty())
					detail = fmt::format("mask index {} for {}, the engine's {}", selected.maskIndex, static_cast<const void*>(selected.light), engineMask);
			}
		}
		if (differ) {
			++p.framesDiffer;
			if (p.first.empty())
				p.first = fmt::format("frame {}: {}", frame.sceneFrame, detail);
		}
		if (++reportFrames >= 300) {
			reportFrames = 0;
			Report();
		}
	}

	void LightSelection::Report()
	{
		auto& p = parity;
		logger::info("[DCLF] light selection (T3b, T3c: DCLF's choice of shadow lights and focus hosts against CalculateActiveShadowCasterLights'): {} frames, {} local "
					 "lights kept, {} focus hosts; {} frames differ ({} slots, {} mask indices, {} counts, {} focus){}{}",
			p.frames, p.kept, p.focusHosts, p.framesDiffer, p.slotsDiffer, p.masksDiffer, p.countsDiffer, p.focusDiffer, p.framesDiffer ? " <- LIGHT SELECTION" : " <- OK",
			p.first.empty() ? "" : "; first: " + p.first);
		p = {};
	}

	struct LightSelection::Hooks
	{
		struct CalculateActive
		{
			static void thunk()
			{
				auto& selection = LightSelection::Get();
				selection.SampleRooms();
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
		stl::detour_thunk<Hooks::CalculateActive>(REL::Offset(kCalculateActive).address());
		installed = true;
		logger::info("[DCLF] light selection: CalculateActiveShadowCasterLights detoured (T3b)");
	}
}
