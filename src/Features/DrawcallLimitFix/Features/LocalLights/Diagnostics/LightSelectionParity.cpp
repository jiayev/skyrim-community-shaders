// The light selection's parity (LightSelectionParity.h): at LightSelection's detour of CalculateActiveShadowCasterLights, render thread.

#include "Features/DrawcallLimitFix/Features/LocalLights/Diagnostics/LightSelectionParity.h"

#include "Features/DrawcallLimitFix/Features/LocalLights/LocalLights.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/Diagnostics/LightSelectionView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include <unordered_map>

namespace DCLF::Diagnostics::LightSelectionParity
{
	namespace
	{
		using Engine::At;
		using Engine::Global;
		using View = LightSelectionView;

		// Module offsets, AE 1.6.1170 and SE 1.5.97.
		constexpr Engine::Offset kLightCount{ 0x338c900, 0x32334d0 };   // the kept lights, the sun among them (the function's last store)
		constexpr Engine::Offset kSlotCounter{ 0x338c904, 0x32334d4 };  // the next shadowLightsAccum slot; the next mask index at +4
		constexpr Engine::Offset kFocusHost{ 0x33dcfb8, 0x3283b80 };    // last frame's focus host (non-sun)
		constexpr std::size_t kPortalEntry = 0x30190;       // a culling process's portal-graph entry

		RE::ShadowSceneNode* SceneNode() { return Global<RE::ShadowSceneNode*>(Engine::Address::kShadowSceneNode); }

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

		// The engine's entries at the function's entry, per light.
		struct EngineRooms
		{
			bool shares = false, visibleUnbound = false;
			const void* entry = nullptr;      // descriptor 0's process's (the room test's)
			const void* roomEntry = nullptr;  // the room process's (+0x128, the walk's)
			std::array<float, 3> walkedFrom{};
			std::vector<const void*> rooms;
		};
		std::unordered_map<const RE::BSShadowLight*, EngineRooms> engineRooms;
		const PortalViews::LightRooms kNotWalked{};
		Tally tally;
		Reports::Window window;

		void Sample(const On<EnginePoint::LightSelectionBegin>&)
		{
			// The engine's room test at the function's entry (FUN_140e14300 on the entries), and where the light's
			// entry was last walked from (its room process's camera, +0x128 -> +0x18).
			engineRooms.clear();
			auto* node = SceneNode();
			const auto* processes = Global<const std::byte* const*>(Engine::Address::kListProcesses);
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
				EngineRooms sample;
				sample.shares = entry && cameraEntry && SharesRoom(entry, cameraEntry);
				sample.entry = entry;
				if (const auto* roomProcess = At<const std::byte*>(light, 0x128)) {
					sample.roomEntry = At<const void*>(roomProcess, kPortalEntry);
					if (const auto* camera = At<const std::byte*>(roomProcess, 0x18))
						sample.walkedFrom = { At<float>(camera, 0xA0), At<float>(camera, 0xA4), At<float>(camera, 0xA8) };
				}
				if (entry) {
					sample.visibleUnbound = At<std::uint8_t>(entry, 0x130) != 0;
					const auto* list = At<const void* const*>(entry, 0x18);
					for (std::uint32_t i = 0, n = list ? At<std::uint32_t>(entry, 0x28) : 0; i < n; ++i)
						sample.rooms.push_back(list[i]);
				}
				engineRooms[light] = std::move(sample);
			}
		}

		void CheckRooms(bool& a_differ, std::string& a_detail)
		{
			// Every light DCLF tested for a room against the engine's verdict at the function's entry. The engine's light entry is
			// its last walk's: a light that moved since (its room process's camera elsewhere) is tallied apart.
			auto& p = tally;
			const auto& roomVerdicts = View::RoomVerdicts();
			const auto& lightRooms = View::LightRooms();
			for (const auto& [light, ours] : roomVerdicts) {
				const auto sample = engineRooms.find(light);
				if (sample == engineRooms.end())
					continue;
				const auto& engine = sample->second;
				++p.roomTests;
				const auto walkedAt = lightRooms.find(light);
				const auto& walked = walkedAt != lightRooms.end() ? walkedAt->second : kNotWalked;
				++p.roomWalks[std::min<std::uint32_t>(walked.walk, 2)];
				if (engine.entry != engine.roomEntry)
					++p.roomEntriesApart;
				if (walked.walk == PortalViews::kLightWalked) {
					std::string why;
					if (const auto* roomProcess = At<const void*>(light, 0x128); roomProcess && !PortalViews::Get().LightProcessAsAssumed(roomProcess, why)) {
						++p.roomProcessesOther;
						if (p.roomProcessFirst.empty())
							p.roomProcessFirst = why;
					}
				}
				const auto* niLight = const_cast<RE::BSShadowLight*>(light)->light.get();
				const float moved = niLight ? std::max({ std::abs(engine.walkedFrom[0] - niLight->world.translate.x), std::abs(engine.walkedFrom[1] - niLight->world.translate.y),
												  std::abs(engine.walkedFrom[2] - niLight->world.translate.z) }) :
											  0.0f;
				bool roomsEqual = walked.visibleUnbound == engine.visibleUnbound && walked.rooms.size() == engine.rooms.size();
				for (std::size_t i = 0; roomsEqual && i < walked.rooms.size(); ++i)
					roomsEqual = std::find(engine.rooms.begin(), engine.rooms.end(), walked.rooms[i]) != engine.rooms.end();
				if (!roomsEqual) {
					++(moved > 0.0f ? p.roomSetsMovedDiffer : p.roomSetsDiffer);
					if (moved == 0.0f && p.roomSetsFirst.empty())
				{
					const auto names = [](const std::vector<const void*>& a_rooms) {
						std::string out;
						for (const void* room : a_rooms) {
							const char* name = room ? static_cast<const RE::NiAVObject*>(room)->name.c_str() : nullptr;
							out += fmt::format("{}{}", out.empty() ? "" : ", ", name && *name ? name : "?");
						}
						return out;
					};
					const auto* position = niLight ? &niLight->world.translate : nullptr;
					p.roomSetsFirst = fmt::format("{} at ({:.0f} {:.0f} {:.0f}) r {:.0f} strict {} (walk {}, unbound {}, rooms [{}]), the engine's unbound {}, rooms [{}]",
						static_cast<const void*>(light), position ? position->x : 0.0f, position ? position->y : 0.0f, position ? position->z : 0.0f,
						niLight ? const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData().radius.x : 0.0f, At<std::uint8_t>(light, 0x47), walked.walk, walked.visibleUnbound,
						names(walked.rooms), engine.visibleUnbound, names(engine.rooms));
				}
				}
				if (ours == engine.shares)
					continue;
				if (moved > 0.0f) {
					++p.roomsMovedDiffer;
					continue;
				}
				++p.roomsDiffer;
				a_differ = true;
				if (a_detail.empty())
					a_detail = fmt::format("room test {} for {} (walk {}, unbound {}, {} rooms), the engine's {} (unbound {}, {} rooms)", ours, static_cast<const void*>(light), walked.walk,
						walked.visibleUnbound, walked.rooms.size(), engine.shares, engine.visibleUnbound, engine.rooms.size());
			}
		}

		void Check(const On<EnginePoint::LightSelectionMade>&)
		{
			auto& p = tally;
			const auto& frame = View::Frame();
			const auto& focusFlags = View::FocusFlags();
			const auto* lastFocusHost = View::LastFocusHost();
			const auto& roomVerdicts = View::RoomVerdicts();
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
							const auto room = roomVerdicts.find(a_light);
							return fmt::format("{} ({}{}, room {})", static_cast<const void*>(a_light),
								light->GetIsDirectionalLight() ? "directional" : light->GetIsFrustumLight() ? "spot" : light->GetIsParabolicLight() ? "point" : "other",
								niLight ? fmt::format(" at ({:.0f} {:.0f} {:.0f}) r {:.0f}", niLight->world.translate.x, niLight->world.translate.y, niLight->world.translate.z,
											  const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData().radius.x) :
										  "",
								room == roomVerdicts.end() ? "untested" : room->second ? "shared" : "not shared");
						};
						detail = fmt::format("slot {}: {}, the engine's {}", s, describe(ours), describe(engine));
					}
				}
			}
			CheckRooms(differ, detail);
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
			if (window.Tick())
				Features::LocalLights::LightSelectionLines();
		}
	}

	void Install()
	{
		Subscribe<EnginePoint::LightSelectionBegin>(Sample);
		Subscribe<EnginePoint::LightSelectionMade>(Check);
	}

	Tally Take()
	{
		return std::exchange(tally, {});
	}
}
