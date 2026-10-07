#include "FrameGlobals.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/EngineStates.h"
#include "Features/DrawcallLimitFix/Engine/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "Utils/Game.h"

#include <algorithm>
#include <atomic>

namespace DCLF
{
	namespace
	{
		using Engine::Global;

		// AE 1.6.1170, module offsets (the readers' notes name each).
		constexpr std::uintptr_t kTechniqueByte12 = 0x2032fdb;
		constexpr std::uintptr_t kTechniqueByte7 = 0x2035500;
		constexpr std::uintptr_t kShadowGlobal = 0x2033498;
		constexpr std::uintptr_t kMainAccumulator = 0x338c830;  // BSShaderAccumulator*, render mode 0
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;
		constexpr std::uintptr_t kNoSunShadowDir = 0x20330a4;
		constexpr std::uintptr_t kScreenDoorFades = 0x2033468;
		constexpr std::uintptr_t kFadesOn = 0x2032dfd;
		constexpr std::uintptr_t kFadeLodUpdates = 0x2032dfc;
		constexpr std::uintptr_t kFadeSpecialA = 0x332a254;
		constexpr std::uintptr_t kFadeSpecialB = 0x1769578;
		constexpr std::uintptr_t kFadeDistanceMult = 0x2032e48;
		constexpr std::uintptr_t kFadeOutThreshold = 0x2032e38;
		constexpr std::uintptr_t kFadeDefaultScale = 0x1ad2840;
		constexpr std::uintptr_t kFadeTypeDivisors = 0x2032e00;
		constexpr std::uintptr_t kMetricScale = 0x1aa6300;
		constexpr std::uintptr_t kLodRadiusBase = 0x2032e40;
		constexpr std::uintptr_t kLodExponentScale = 0x2032e54;
		constexpr std::uintptr_t kLodPowBase = 0x2032e44;
		constexpr std::uintptr_t kTreeWindSource = 0x2033060;  // a pointer; the magnitude at +0x304
		constexpr std::uintptr_t kTreeWindFadeStart = 0x2033100;
		constexpr std::uintptr_t kTreeWindFadeEnd = 0x2033104;
		constexpr std::uintptr_t kTreeWindTimerScale = 0x1ad28bc;

		// A setting's value: the setting itself looked up once (GetINISetting walks the collection by name), its value every frame.
		struct CachedSetting
		{
			const char* name;
			float fallback;
			RE::Setting* setting = nullptr;
			bool looked = false;
			float Get()
			{
				if (!std::exchange(looked, true))
					setting = RE::GetINISetting(name);
				return setting ? setting->GetFloat() : fallback;
			}
		};
		CachedSetting iniSpecularStart{ "fSpecularLODFadeStart:LightingShader", 0.09f };
		CachedSetting iniSpecularEnd{ "fSpecularLODFadeEnd:LightingShader", 0.10f };
		CachedSetting iniEnvmapStart{ "fEnvmapLODFadeStart:LightingShader", 0.09f };
		CachedSetting iniEnvmapEnd{ "fEnvmapLODFadeEnd:LightingShader", 0.10f };

		bool Hidden(const RE::NiAVObject* a_object)
		{
			return a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		}

		// The render thread's latest capture (written by the render thread alone), and the ones before it: an unscoped reader (a
		// defect, counted) holds a reference to one for its read, so a few frames' are kept alive past their replacement.
		std::shared_ptr<const FrameGlobals> latest = std::make_shared<FrameGlobals>();
		std::array<std::shared_ptr<const FrameGlobals>, 8> recent;
		std::uint32_t recentNext = 0;
		std::atomic<std::uint32_t> renderThread{ 0 };
		std::atomic<std::uint64_t> unscopedReads{ 0 };
		thread_local const FrameGlobals* scoped = nullptr;
	}

	std::shared_ptr<const FrameGlobals> FrameGlobals::Capture()
	{
		ZoneScopedN("CS.DCLF.FrameGlobals.Capture");
		renderThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		auto g = std::make_shared<FrameGlobals>();
		g->loading = SceneStore::IsLoadingScreenUp();
		g->interior = Util::IsInterior();
		g->decalBias = { 0u, DecalDepthBiasMode(1), DecalDepthBiasMode(2), DecalDepthBiasMode(3) };
		g->specularStart = iniSpecularStart.Get();
		g->specularEnd = iniSpecularEnd.Get();
		g->envmapStart = iniEnvmapStart.Get();
		g->envmapEnd = iniEnvmapEnd.Get();
		g->techniqueByte12 = Global<std::uint8_t>(kTechniqueByte12);
		g->techniqueByte7 = Global<std::uint8_t>(kTechniqueByte7);
		g->shadowGlobal = Global<std::uint8_t>(kShadowGlobal);
		if (const auto* accumulator = Global<std::uint8_t*>(kMainAccumulator)) {
			g->accumulator = true;
			g->accumulatorDeferredShadow = accumulator[kAccumulatorDeferredShadow];
		}
		g->noSunShadowDir = Global<std::uint8_t>(kNoSunShadowDir);
		g->screenDoorFades = Global<std::uint8_t>(kScreenDoorFades);
		g->fadesOn = Global<std::uint8_t>(kFadesOn);
		g->fadeLodUpdates = Global<std::uint8_t>(kFadeLodUpdates);
		g->fadeSpecialA = Global<float>(kFadeSpecialA);
		g->fadeSpecialB = Global<float>(kFadeSpecialB);
		g->fadeDistanceMult = Global<float>(kFadeDistanceMult);
		g->fadeOutThreshold = Global<float>(kFadeOutThreshold);
		g->fadeDefaultScale = Global<float>(kFadeDefaultScale);
		g->metricScale = Global<float>(kMetricScale);
		for (std::uint32_t type = 0; type < g->fadeTypeDivisors.size(); ++type)
			g->fadeTypeDivisors[type] = Global<float>(kFadeTypeDivisors + type * 4);
		g->lodRadiusBase = Global<float>(kLodRadiusBase);
		g->lodExponentScale = Global<float>(kLodExponentScale);
		g->lodPowBase = Global<float>(kLodPowBase);
		if (const auto* camera = RE::Main::WorldRootCamera()) {
			g->eye = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z };
			g->lodAdjust = camera->GetRuntimeData2().lodAdjust;
		}
		if (const auto source = Global<std::uintptr_t>(kTreeWindSource))
			g->treeWindMagnitude = *reinterpret_cast<const float*>(source + 0x304);
		g->treeWindFadeStart = Global<float>(kTreeWindFadeStart);
		g->treeWindFadeEnd = Global<float>(kTreeWindFadeEnd);
		g->treeWindTimerScale = Global<float>(kTreeWindTimerScale);
		// The roots whose hidden bit the frame changes around the scene's views (SceneStore::HiddenForWalk), as the views see it,
		// at the point the scene work is kicked from (after Main::Draw's early call).
		if (!g->loading) {
			for (auto* sceneNode : RE::BSShaderManager::State::GetSingleton().shadowSceneNode) {
				const auto* graph = sceneNode ? sceneNode->GetRuntimeData().portalGraph : nullptr;
				if (!graph)
					continue;
				for (const auto& child : graph->alwaysRenderChildren)
					if (child)
						g->cullHidden.emplace_back(child.get(), Hidden(child.get()));
				if (graph->portalSharedNode)
					g->cullHidden.emplace_back(graph->portalSharedNode.get(), Hidden(graph->portalSharedNode.get()));
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				// The third-person skeleton as it is now. TESWaterReflections::Update (AE 0x140520570), on the frames a cube-map
				// reflection updates, hides the player's 3D while it renders the faces and then restores it; Main::Draw calls it
				// between the main cull jobs' Begin and Finish.
				const RE::NiAVObject* thirdPerson = player->Get3D(false);
				if (thirdPerson)
					g->cullHidden.emplace_back(thirdPerson, Hidden(thirdPerson));
				// The first-person skeleton: Main::Draw (AE 0x1406444b0) hides it right after the call the scene work is kicked from,
				// keeps it hidden through the main camera's cull and the sun's shadow casters, and shows it only to draw the
				// first-person view with its own camera. For every view the scene serves it is hidden.
				const RE::NiAVObject* firstPerson = player->Get3D(true);
				if (firstPerson && firstPerson != thirdPerson)
					g->cullHidden.emplace_back(firstPerson, true);
			}
			std::sort(g->cullHidden.begin(), g->cullHidden.end());
		}
		g->membershipWitness = PrimaryCull::MembershipWitness(*g);
		recent[recentNext++ % recent.size()] = latest;
		latest = g;
		return g;
	}

	const FrameGlobals& FrameGlobals::Current()
	{
		if (scoped)
			return *scoped;
		if (::GetCurrentThreadId() != renderThread.load(std::memory_order_relaxed) && unscopedReads.fetch_add(1, std::memory_order_relaxed) == 0)
			logger::error("[DCLF] the frame's engine globals were read off the render thread with no frame's capture bound (step 6e F2); it got the render thread's latest");
		return *latest;
	}

	std::uint64_t FrameGlobals::TakeUnscopedReads()
	{
		return unscopedReads.exchange(0, std::memory_order_relaxed);
	}

	FrameGlobals::Scope::Scope(std::shared_ptr<const FrameGlobals> a_globals) :
		held(std::move(a_globals)),
		previous(scoped)
	{
		scoped = held.get();
	}

	FrameGlobals::Scope::~Scope()
	{
		scoped = previous;
	}
}
