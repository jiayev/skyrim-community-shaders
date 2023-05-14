#include "Hooks.h"

#include <detours/Detours.h>

#include "ShaderCache.h"
#include "State.h"
#include "Menu.h"

#include "ShaderTools/BSShaderHooks.h"

void hk_BSShader_LoadShaders(RE::BSShader* shader, std::uintptr_t stream);

decltype(&hk_BSShader_LoadShaders) ptr_BSShader_LoadShaders;

void hk_BSShader_LoadShaders(RE::BSShader* shader, std::uintptr_t stream)
{
	(ptr_BSShader_LoadShaders)(shader, stream);
	auto& shaderCache = SIE::ShaderCache::Instance();
	for (const auto& entry : shader->pixelShaders) {
		shaderCache.GetPixelShader(*shader, entry->id);
	}
	for (const auto& entry : shader->vertexShaders) {
		shaderCache.GetVertexShader(*shader, entry->id);
	}
	BSShaderHooks::hk_LoadShaders((REX::BSShader*)shader, stream);
};

bool hk_BSShader_BeginTechnique(RE::BSShader* shader, int vertexDescriptor, int pixelDescriptor, bool skipPIxelShader);

decltype(&hk_BSShader_BeginTechnique) ptr_BSShader_BeginTechnique;

bool hk_BSShader_BeginTechnique(RE::BSShader* shader, int vertexDescriptor, int pixelDescriptor, bool skipPIxelShader)
{
	auto state = State::GetSingleton();
	state->currentShader = shader;
	state->currentVertexDescriptor = vertexDescriptor;
	state->currentPixelDescriptor = pixelDescriptor;
	return (ptr_BSShader_BeginTechnique)(shader, vertexDescriptor, pixelDescriptor, skipPIxelShader);
}

decltype(&IDXGISwapChain::Present) ptr_IDXGISwapChain_Present;

HRESULT WINAPI hk_IDXGISwapChain_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
	State::GetSingleton()->Reset();
	Menu::GetSingleton()->DrawOverlay();
	return (This->*ptr_IDXGISwapChain_Present)(SyncInterval, Flags);
}

void hk_BSGraphics_SetDirtyStates(bool isCompute);

decltype(&hk_BSGraphics_SetDirtyStates) ptr_BSGraphics_SetDirtyStates;

void hk_BSGraphics_SetDirtyStates(bool isCompute)
{
	(ptr_BSGraphics_SetDirtyStates)(isCompute);
	State::GetSingleton()->Draw();
}

namespace Hooks
{
	struct BSGraphics_Renderer_Init_InitD3D
	{
		static void thunk()
		{
			logger::info("Calling original Init3D");

			func();

			logger::info("Accessing render device information");

			auto manager = RE::BSRenderManager::GetSingleton();

			auto context = manager->GetRuntimeData().context;
			auto swapchain = manager->GetRuntimeData().swapChain;
			auto device = manager->GetRuntimeData().forwarder;

			logger::info("Detouring virtual function tables");

			*(uintptr_t*)&ptr_IDXGISwapChain_Present = Detours::X64::DetourClassVTable(*(uintptr_t*)swapchain, &hk_IDXGISwapChain_Present, 8);

			State::GetSingleton()->Setup();
			Menu::GetSingleton()->Init(swapchain, device, context);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Install()
	{
		logger::info("Hooking BSShader::LoadShaders");
		*(uintptr_t*)&ptr_BSShader_LoadShaders = Detours::X64::DetourFunction(REL::RelocationID(101339, 108326).address(), (uintptr_t)&hk_BSShader_LoadShaders);
		logger::info("Hooking BSShader::BeginTechnique");
		*(uintptr_t*)&ptr_BSShader_BeginTechnique = Detours::X64::DetourFunction(REL::RelocationID(101341, 108328).address(), (uintptr_t)&hk_BSShader_BeginTechnique);
		logger::info("Hooking BSGraphics::SetDirtyStates");
		*(uintptr_t*)&ptr_BSGraphics_SetDirtyStates = Detours::X64::DetourFunction(REL::RelocationID(75580, 77386).address(), (uintptr_t)&hk_BSGraphics_SetDirtyStates);
		logger::info("Hooking BSGraphics::Renderer::InitD3D");
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));
	}
}