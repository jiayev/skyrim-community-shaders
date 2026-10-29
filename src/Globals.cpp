#include "Globals.h"

#include "CSEditor/SceneManager/SceneManager.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "Deferred.h"
#include "Features/CSEditor.h"
#include "Features/CloudShadows.h"
#include "Features/DeveloperTools.h"
#include "Features/DynamicCubemaps.h"
#include "Features/Effects11.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/GrassCollision.h"
#include "Features/GrassLighting.h"
#include "Features/GrassOptimizations.h"
#include "Features/HDRDisplay.h"
#include "Features/HairSpecular.h"
#include "Features/HorizonFix.h"
#include "Features/IBL.h"
#include "Features/InteriorSun.h"
#include "Features/LODBlending.h"
#include "Features/DrawcallLimitFix.h"
#include "Utils/SwitchFile.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PerformanceOverlay.h"
#include "Features/PostProcessing.h"
#include "Features/RainRefraction.h"
#include "Features/RemoteControl.h"
#include "Features/RenderDoc.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Skin.h"
#include "Features/SkySync.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainHelper.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/VolumetricLighting.h"
#include "Features/VolumetricShadows.h"
#include "Features/VRAMBudget.h"
#include "Features/WaterEffects.h"
#include "Features/WetnessEffects.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/Game.h"

#include <array>
#include <atomic>
#include <intrin.h>
#include <mutex>

namespace globals
{
	namespace d3d
	{
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		IDXGISwapChain* swapChain = nullptr;
	}

	namespace features
	{
		CloudShadows cloudShadows{};
		DynamicCubemaps dynamicCubemaps{};
		VolumetricShadows volumetricShadows{};
		ExtendedMaterials extendedMaterials{};
		GrassCollision grassCollision{};
		GrassLighting grassLighting{};
		DrawcallLimitFix drawcallLimitFix{};
		GrassOptimizations grassOptimizations{};
		IBL ibl{};
		LightLimitFix lightLimitFix{};
		LinearLighting linearLighting{};
		LODBlending lodBlending{};
		HairSpecular hairSpecular{};
		HorizonFix horizonFix{};
		InteriorSun interiorSun{};
		RainRefraction rainRefraction{};
		ScreenSpaceGI screenSpaceGI{};
		ScreenSpaceShadows screenSpaceShadows{};
		Skylighting skylighting{};
		TerrainVariation terrainVariation{};
		SkySync skySync{};
		SubsurfaceScattering subsurfaceScattering{};
		TerrainBlending terrainBlending{};
		TerrainHelper terrainHelper{};
		TerrainShadows terrainShadows{};
		UnifiedWater unifiedWater{};
		VolumetricLighting volumetricLighting{};
		VRAMBudget vramBudget{};
		WaterEffects waterEffects{};
		PerformanceOverlay performanceOverlay{};
		WetnessEffects wetnessEffects{};
		ExtendedTranslucency extendedTranslucency{};
		Upscaling upscaling{};
		HDRDisplay hdrDisplay{};
		Effects11 effects11{};
		DeveloperTools developerTools{};
		RenderDoc renderDoc{};
		RemoteControl remoteControl{};
		ScreenshotFeature screenshotFeature{};
		CSEditor csEditor{};
		ExponentialHeightFog exponentialHeightFog{};
		TruePBR truePBR{};
		Skin skin{};
		SceneManager sceneManager{};
		PostProcessing postProcessing{};

		namespace llf
		{
		}
	}

	namespace game
	{
		RE::BSGraphics::RendererShadowState* shadowState = nullptr;
		RE::BSGraphics::State* graphicsState = nullptr;
		RE::BSGraphics::Renderer* renderer = nullptr;
		RE::BSShaderManager::State* smState = nullptr;
		RE::TES* tes = nullptr;
		RE::MemoryManager* memoryManager = nullptr;
		RE::INISettingCollection* iniSettingCollection = nullptr;
		RE::INIPrefSettingCollection* iniPrefSettingCollection = nullptr;
		RE::GameSettingCollection* gameSettingCollection = nullptr;
		float* cameraNear = nullptr;
		float* cameraFar = nullptr;
		float* deltaTime = nullptr;
		RE::BSUtilityShader* utilityShader = nullptr;
		RE::PlayerCharacter* player = nullptr;
		RE::Sky* sky = nullptr;
		RE::UI* ui = nullptr;
		RE::Calendar* calendar = nullptr;
		RE::ImageSpaceManager* imageSpaceManager = nullptr;
		bool* bEnableVolumetricLighting = nullptr;
		std::atomic<bool> quitGame{ false };

		RE::BSGraphics::PixelShader** currentPixelShader = nullptr;
		RE::BSGraphics::VertexShader** currentVertexShader = nullptr;
		REX::EnumSet<RE::BSGraphics::ShaderFlags, uint32_t>* stateUpdateFlags = nullptr;

		RE::Setting* bEnableLandFade = nullptr;
		RE::Setting* bShadowsOnGrass = nullptr;
		RE::Setting* shadowMaskQuarter = nullptr;

		REL::Relocation<ID3D11Buffer**> perFrame;
		REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> currentAccumulator;

		D3D11_MAPPED_SUBRESOURCE* mappedFrameBuffer = nullptr;
		FrameBufferCache frameBufferCached{};
	}

	static void RefreshTES()
	{
		if (auto tes = RE::TES::GetSingleton())
			game::tes = tes;
	}

	namespace rtti
	{
		REL::Relocation<const RE::NiRTTI*> NiIntegerExtraDataRTTI;
		REL::Relocation<const RE::NiRTTI*> BSLightingShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSEffectShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSWaterShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> NiParticleSystemRTTI;
		REL::Relocation<const RE::NiRTTI*> NiBillboardNodeRTTI;
		REL::Relocation<const RE::NiRTTI*> NiAlphaPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> NiSourceTextureRTTI;
		REL::Relocation<const RE::NiRTTI*> BSGrassShaderPropertyRTTI;
		REL::Relocation<const RE::NiRTTI*> BSMultiStreamInstanceTriShapeRTTI;
	}

	State* state = nullptr;
	Deferred* deferred = nullptr;
	Menu* menu = nullptr;
	SIE::ShaderCache* shaderCache = nullptr;
	SceneSettingsManager* sceneSettingsManager = nullptr;

	static Profiler profilerInstance;
	Profiler* profiler = &profilerInstance;

	void OnInit()
	{
		shaderCache = &SIE::ShaderCache::Instance();
		state = State::GetSingleton();
		menu = Menu::GetSingleton();
		deferred = Deferred::GetSingleton();
		sceneSettingsManager = SceneSettingsManager::GetSingleton();
	}

	void ReInit()
	{
		{
			using namespace game;

			shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			graphicsState = RE::BSGraphics::State::GetSingleton();
			renderer = RE::BSGraphics::Renderer::GetSingleton();
			smState = &RE::BSShaderManager::State::GetSingleton();
			iniSettingCollection = RE::INISettingCollection::GetSingleton();
			iniPrefSettingCollection = RE::INIPrefSettingCollection::GetSingleton();
			gameSettingCollection = RE::GameSettingCollection::GetSingleton();
			RefreshTES();
			cameraNear = (float*)(REL::RelocationID(517032, 403540).address() + 0x40);
			cameraFar = (float*)(REL::RelocationID(517032, 403540).address() + 0x44);
			deltaTime = (float*)REL::RelocationID(523660, 410199).address();

			currentPixelShader = &(shadowState->GetRuntimeData().currentPixelShader);
			currentVertexShader = &(shadowState->GetRuntimeData().currentVertexShader);
			stateUpdateFlags = &(shadowState->GetRuntimeData().stateUpdateFlags);

			ui = RE::UI::GetSingleton();
			calendar = RE::Calendar::GetSingleton();
			perFrame = { REL::RelocationID(524768, 411384) };

			currentAccumulator = { REL::RelocationID(527650, 414600) };
		}

		{
			using namespace rtti;
			NiIntegerExtraDataRTTI = { RE::NiIntegerExtraData::Ni_RTTI };
			BSLightingShaderPropertyRTTI = { RE::BSLightingShaderProperty::Ni_RTTI };
			BSEffectShaderPropertyRTTI = { RE::BSEffectShaderProperty::Ni_RTTI };
			BSWaterShaderPropertyRTTI = { RE::BSWaterShaderProperty::Ni_RTTI };
			NiParticleSystemRTTI = { RE::NiParticleSystem::Ni_RTTI };
			NiBillboardNodeRTTI = { RE::NiBillboardNode::Ni_RTTI };
			NiAlphaPropertyRTTI = { RE::NiAlphaProperty::Ni_RTTI };
			NiSourceTextureRTTI = { RE::NiSourceTexture::Ni_RTTI };
			BSGrassShaderPropertyRTTI = { RE::BSGrassShaderProperty::Ni_RTTI };
			BSMultiStreamInstanceTriShapeRTTI = { RE::BSMultiStreamInstanceTriShape::Ni_RTTI };
		}

		d3d::device = reinterpret_cast<ID3D11Device*>(game::renderer->GetRuntimeData().forwarder);
		d3d::context = reinterpret_cast<ID3D11DeviceContext*>(game::renderer->GetRuntimeData().context);
		d3d::swapChain = reinterpret_cast<IDXGISwapChain*>(game::renderer->GetRuntimeData().renderWindows->swapChain);
	}

	void OnDataLoaded()
	{
		using namespace game;
		RefreshTES();
		player = RE::PlayerCharacter::GetSingleton();
		sky = RE::Sky::GetSingleton();
		utilityShader = RE::BSUtilityShader::GetSingleton();
		imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		bEnableVolumetricLighting = reinterpret_cast<bool*>(REL::RelocationID(527940, 414913).address());

		bEnableLandFade = iniSettingCollection->GetSetting("bEnableLandFade:Display");

		bShadowsOnGrass = RE::GetINISetting("bShadowsOnGrass:Display");
		shadowMaskQuarter = RE::GetINISetting("iShadowMaskQuarter:Display");
	}

	void OnGameWindowClose()
	{
		game::quitGame = true;
		if (shaderCache)
			shaderCache->StopCompilation();
	}

	/**
 * @brief Caches the current frame buffer data and clears the mapped pointer.
 *
 * Copies the contents of the mapped frame buffer into an internal cache and resets the mapped frame buffer pointer.
 */
	void CacheFramebuffer()
	{
		using namespace game;
		static_assert(sizeof(FrameBuffer) % sizeof(__m128i) == 0);

		auto* source = static_cast<__m128i*>(mappedFrameBuffer->pData);
		auto* destination = reinterpret_cast<__m128i*>(&frameBufferCached.data);

		if (reinterpret_cast<uintptr_t>(source) % alignof(__m128i) == 0) {
			for (size_t i = 0; i < sizeof(FrameBuffer) / sizeof(__m128i); ++i)
				_mm_storeu_si128(destination + i, _mm_stream_load_si128(source + i));
		} else {
			frameBufferCached.data = *reinterpret_cast<FrameBuffer*>(source);
		}

		mappedFrameBuffer = nullptr;
	}

	/**
 * @brief Hooks the ID3D11DeviceContext::Map method to track mapping of the per-frame resource.
 *
 * Calls the original Map function and, if the mapped resource matches the current per-frame buffer, stores the mapped subresource pointer for later use.
 *
 * @return HRESULT Result of the original Map call.
 */
	struct ID3D11DeviceContext_Map
	{
		static HRESULT thunk(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
		{
			HRESULT hr = func(This, pResource, Subresource, MapType, MapFlags, pMappedResource);
			if (hr == S_OK) {
				if (*globals::game::perFrame.get() == pResource)
					globals::game::mappedFrameBuffer = pMappedResource;
				DrawcallLimitFix::OnMap(pResource, pMappedResource->pData);
			}
			return hr;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
 * @brief Hooked implementation of ID3D11DeviceContext::Unmap that caches the frame buffer if applicable.
 *
 * If the resource being unmapped matches the current per-frame buffer and a mapped frame buffer is present, caches the frame buffer data before calling the original Unmap function.
 */
	struct ID3D11DeviceContext_Unmap
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11Resource* pResource, UINT Subresource)
		{
			if (*globals::game::perFrame.get() == pResource && globals::game::mappedFrameBuffer) {
				CacheFramebuffer();
			}
			DrawcallLimitFix::OnUnmap(pResource);
			func(This, pResource, Subresource);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Feeds Drawcall Limit Fix's constant buffer mirror (buffers updated without Map). */
	struct ID3D11DeviceContext_UpdateSubresource
	{
		static void thunk(ID3D11DeviceContext* This, ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch,
			UINT SrcDepthPitch)
		{
			DrawcallLimitFix::OnUpdateSubresource(pDstResource, pDstBox, pSrcData);
			func(This, pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// CS_DCLF_DEPTH_TRACE=1: only the five selected interior frames incur these hooks' work.
	// Keep the calls in order and log after leaving the interval, so logging cannot change it.
	namespace
	{
		struct DepthEvent
		{
			const char* action = nullptr;
			const void* caller = nullptr;
			const void* resource = nullptr;
			std::uint32_t detail = 0;
			bool mainDepth = false;
		};

		std::atomic<bool> depthTraceActive = false;
		std::mutex depthTraceMutex;
		ID3D11Texture2D* depthTraceTarget = nullptr;
		std::array<DepthEvent, 256> depthEvents{};
		std::size_t depthEventCount = 0;
		std::uint32_t depthEventOverflow = 0;
		std::uint32_t depthCandidateFrames = 0;
		std::uint32_t depthTracedFrames = 0;

		bool IsMainDepth(ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv || !depthTraceTarget)
				return false;
			ID3D11Resource* resource = nullptr;
			a_dsv->GetResource(&resource);
			const bool match = resource == static_cast<ID3D11Resource*>(depthTraceTarget);
			if (resource)
				resource->Release();
			return match;
		}

		void AppendDepthEvent(const char* a_action, const void* a_caller, const void* a_resource, std::uint32_t a_detail, bool a_main)
		{
			if (!depthTraceActive.load(std::memory_order_relaxed))
				return;
			std::scoped_lock lock(depthTraceMutex);
			if (!depthTraceActive.load(std::memory_order_relaxed))
				return;
			if (depthEventCount < depthEvents.size())
				depthEvents[depthEventCount++] = { a_action, a_caller, a_resource, a_detail, a_main };
			else
				++depthEventOverflow;
		}

		void TraceNativeDraw(ID3D11DeviceContext* a_context, const char* a_action, const void* a_caller)
		{
			if (!depthTraceActive.load(std::memory_order_relaxed))
				return;
			ID3D11DepthStencilView* dsv = nullptr;
			a_context->OMGetRenderTargets(0, nullptr, &dsv);
			ID3D11DepthStencilState* depthState = nullptr;
			UINT stencilRef = 0;
			a_context->OMGetDepthStencilState(&depthState, &stencilRef);
			D3D11_DEPTH_STENCIL_DESC desc{};
			if (depthState) {
				depthState->GetDesc(&desc);
				depthState->Release();
			}
			const auto detail = std::uint32_t(desc.DepthEnable) | (std::uint32_t(desc.DepthWriteMask) << 1) | (std::uint32_t(desc.DepthFunc) << 8);
			const bool main = IsMainDepth(dsv);
			AppendDepthEvent(a_action, a_caller, dsv, detail, main);
			if (dsv)
				dsv->Release();
		}
	}

	struct ID3D11DeviceContext_DepthClearTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_dsv, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			if (depthTraceActive.load(std::memory_order_relaxed))
				AppendDepthEvent("clear", _ReturnAddress(), a_dsv, a_flags, IsMainDepth(a_dsv));
			func(a_context, a_dsv, a_flags, a_depth, a_stencil);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DepthCopyTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a_dst, ID3D11Resource* a_src)
		{
			const auto* target = static_cast<ID3D11Resource*>(depthTraceTarget);
			if (depthTraceActive.load(std::memory_order_relaxed) && target && (a_dst == target || a_src == target)) {
				AppendDepthEvent("copy", _ReturnAddress(), a_dst, a_dst == target ? 1u : 2u, true);
			}
			func(a_context, a_dst, a_src);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DepthBindTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_count, ID3D11RenderTargetView* const* a_rtvs, ID3D11DepthStencilView* a_dsv)
		{
			if (depthTraceActive.load(std::memory_order_relaxed))
				AppendDepthEvent("bind", _ReturnAddress(), a_dsv, a_count, IsMainDepth(a_dsv));
			func(a_context, a_count, a_rtvs, a_dsv);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DepthDrawIndexedTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_count, UINT a_start, INT a_base)
		{
			TraceNativeDraw(a_context, "draw-indexed", _ReturnAddress());
			func(a_context, a_count, a_start, a_base);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DrawCallerTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_count, UINT a_start, INT a_base)
		{
			if (a_count == 8991) {
				using SetDrawCaller = void(__stdcall*)(const void*, const void* const*, std::uint32_t);
				static const auto setCaller = []() -> SetDrawCaller {
					const auto module = ::GetModuleHandleW(L"dxvk_d3d11.dll");
					return module ? reinterpret_cast<SetDrawCaller>(::GetProcAddress(module, "dxvkSetDrawCaller")) : nullptr;
				}();
				if (setCaller) {
					void* stack[16]{};
					const void* addresses[16]{};
					const auto count = ::RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(stack)), stack, nullptr);
					for (std::uint32_t i = 0; i < count; ++i)
						addresses[i] = stack[i];
					setCaller(_ReturnAddress(), addresses, count);
				}
			}
			func(a_context, a_count, a_start, a_base);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DepthDrawTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_count, UINT a_start)
		{
			TraceNativeDraw(a_context, "draw", _ReturnAddress());
			func(a_context, a_count, a_start);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DepthDrawIndexedInstancedTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_instances, UINT a_startIndex, INT a_base, UINT a_startInstance)
		{
			TraceNativeDraw(a_context, "draw-indexed-instanced", _ReturnAddress());
			func(a_context, a_indexCount, a_instances, a_startIndex, a_base, a_startInstance);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DepthDrawInstancedTrace
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_vertices, UINT a_instances, UINT a_startVertex, UINT a_startInstance)
		{
			TraceNativeDraw(a_context, "draw-instanced", _ReturnAddress());
			func(a_context, a_vertices, a_instances, a_startVertex, a_startInstance);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void BeginDCLFDepthTrace(ID3D11Texture2D* a_depth)
	{
		static const bool depthTrace = Util::SwitchFile::Enabled("CS_DCLF_DEPTH_TRACE");
		if (!a_depth || !depthTrace || depthTracedFrames >= 5 || ++depthCandidateFrames < 40)
			return;
		std::scoped_lock lock(depthTraceMutex);
		depthTraceTarget = a_depth;
		depthEventCount = 0;
		depthEventOverflow = 0;
		depthTraceActive.store(true, std::memory_order_release);
	}

	void EndDCLFDepthTrace()
	{
		if (!depthTraceActive.load(std::memory_order_acquire))
			return;
		std::array<DepthEvent, 256> events;
		std::size_t count;
		std::uint32_t overflow;
		{
			std::scoped_lock lock(depthTraceMutex);
			depthTraceActive.store(false, std::memory_order_release);
			events = depthEvents;
			count = depthEventCount;
			overflow = depthEventOverflow;
			++depthTracedFrames;
		}
		logger::info("[DCLF] depth trace frame {}: {} D3D11 calls, {} omitted; target {}", depthTracedFrames, count, overflow, fmt::ptr(depthTraceTarget));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& event = events[i];
			logger::info("[DCLF] depth trace {:03}: {} resource={} main={} detail={:#x} caller={}", i, event.action, fmt::ptr(event.resource), event.mainDepth, event.detail,
				fmt::ptr(event.caller));
		}
	}

	void InstallD3DHooks(ID3D11DeviceContext* a_context)
	{
		stl::detour_vfunc<14, ID3D11DeviceContext_Map>(a_context);
		stl::detour_vfunc<15, ID3D11DeviceContext_Unmap>(a_context);
		stl::detour_vfunc<48, ID3D11DeviceContext_UpdateSubresource>(a_context);
		// CS_DCLF_DRAW_TRACE / CS_DCLF_DEPTH_TRACE (Drawcall Limit Fix's diagnostics).
		const bool depthTrace = Util::SwitchFile::Enabled("CS_DCLF_DEPTH_TRACE");
		if (Util::SwitchFile::Enabled("CS_DCLF_DRAW_TRACE") && !depthTrace)
			stl::detour_vfunc<12, ID3D11DeviceContext_DrawCallerTrace>(a_context);
		if (depthTrace) {
			stl::detour_vfunc<53, ID3D11DeviceContext_DepthClearTrace>(a_context);
			stl::detour_vfunc<47, ID3D11DeviceContext_DepthCopyTrace>(a_context);
			stl::detour_vfunc<33, ID3D11DeviceContext_DepthBindTrace>(a_context);
			stl::detour_vfunc<12, ID3D11DeviceContext_DepthDrawIndexedTrace>(a_context);
			stl::detour_vfunc<13, ID3D11DeviceContext_DepthDrawTrace>(a_context);
			stl::detour_vfunc<20, ID3D11DeviceContext_DepthDrawIndexedInstancedTrace>(a_context);
			stl::detour_vfunc<21, ID3D11DeviceContext_DepthDrawInstancedTrace>(a_context);
		}
	}
}
