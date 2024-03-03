#include "Hooks.h"

#include <detours/Detours.h>

#include "Bindings.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"

#include "ShaderTools/BSShaderHooks.h"

std::unordered_map<void*, std::pair<std::unique_ptr<uint8_t[]>, size_t>> ShaderBytecodeMap;

void RegisterShaderBytecode(void* Shader, const void* Bytecode, size_t BytecodeLength)
{
	// Grab a copy since the pointer isn't going to be valid forever
	auto codeCopy = std::make_unique<uint8_t[]>(BytecodeLength);
	memcpy(codeCopy.get(), Bytecode, BytecodeLength);
	logger::debug(fmt::runtime("Saving shader at index {:x} with {} bytes:\t{:x}"), (std::uintptr_t)Shader, BytecodeLength, (std::uintptr_t)Bytecode);
	ShaderBytecodeMap.emplace(Shader, std::make_pair(std::move(codeCopy), BytecodeLength));
}

const std::pair<std::unique_ptr<uint8_t[]>, size_t>& GetShaderBytecode(void* Shader)
{
	logger::debug(fmt::runtime("Loading shader at index {:x}"), (std::uintptr_t)Shader);
	return ShaderBytecodeMap.at(Shader);
}

void DumpShader(const REX::BSShader* thisClass, const RE::BSGraphics::VertexShader* shader, const std::pair<std::unique_ptr<uint8_t[]>, size_t>& bytecode)
{
	uint8_t* dxbcData = new uint8_t[bytecode.second];
	size_t dxbcLen = bytecode.second;
	memcpy(dxbcData, bytecode.first.get(), bytecode.second);

	std::string dumpDir = std::format("Data\\ShaderDump\\{}\\{}.vs.bin", thisClass->m_LoaderType, shader->id);
	auto directoryPath = std::format("Data\\ShaderDump\\{}", thisClass->m_LoaderType);
	logger::debug(fmt::runtime("Dumping vertex shader {} with id {:x} at {}"), thisClass->m_LoaderType, shader->id, dumpDir);

	if (!std::filesystem::is_directory(directoryPath)) {
		try {
			std::filesystem::create_directories(directoryPath);
		} catch (std::filesystem::filesystem_error const& ex) {
			logger::error("Failed to create folder: {}", ex.what());
		}
	}

	if (FILE * file; fopen_s(&file, dumpDir.c_str(), "wb") == 0) {
		fwrite(dxbcData, 1, dxbcLen, file);
		fclose(file);
	}

	delete[] dxbcData;
}

void DumpShader(const REX::BSShader* thisClass, const RE::BSGraphics::PixelShader* shader, const std::pair<std::unique_ptr<uint8_t[]>, size_t>& bytecode)
{
	uint8_t* dxbcData = new uint8_t[bytecode.second];
	size_t dxbcLen = bytecode.second;
	memcpy(dxbcData, bytecode.first.get(), bytecode.second);

	std::string dumpDir = std::format("Data\\ShaderDump\\{}\\{:X}.ps.bin", thisClass->m_LoaderType, shader->id);

	auto directoryPath = std::format("Data\\ShaderDump\\{}", thisClass->m_LoaderType);
	logger::debug(fmt::runtime("Dumping pixel shader {} with id {:x} at {}"), thisClass->m_LoaderType, shader->id, dumpDir);
	if (!std::filesystem::is_directory(directoryPath)) {
		try {
			std::filesystem::create_directories(directoryPath);
		} catch (std::filesystem::filesystem_error const& ex) {
			logger::error("Failed to create folder: {}", ex.what());
		}
	}

	if (FILE * file; fopen_s(&file, dumpDir.c_str(), "wb") == 0) {
		fwrite(dxbcData, 1, dxbcLen, file);
		fclose(file);
	}

	delete[] dxbcData;
}

void hk_BSShader_LoadShaders(RE::BSShader* shader, std::uintptr_t stream);

decltype(&hk_BSShader_LoadShaders) ptr_BSShader_LoadShaders;

void hk_BSShader_LoadShaders(RE::BSShader* shader, std::uintptr_t stream)
{
	(ptr_BSShader_LoadShaders)(shader, stream);
	auto& shaderCache = SIE::ShaderCache::Instance();

	if (shaderCache.IsDiskCache() || shaderCache.IsDump()) {
		for (const auto& entry : shader->vertexShaders) {
			if (entry->shader && shaderCache.IsDump()) {
				auto& bytecode = GetShaderBytecode(entry->shader);
				DumpShader((REX::BSShader*)shader, entry, bytecode);
			}
			auto vertexShaderDesriptor = entry->id;
			auto pixelShaderDescriptor = entry->id;
			State::GetSingleton()->ModifyShaderLookup(*shader, vertexShaderDesriptor, pixelShaderDescriptor);
			shaderCache.GetVertexShader(*shader, vertexShaderDesriptor);
		}
		for (const auto& entry : shader->pixelShaders) {
			if (entry->shader && shaderCache.IsDump()) {
				auto& bytecode = GetShaderBytecode(entry->shader);
				DumpShader((REX::BSShader*)shader, entry, bytecode);
			}
			auto vertexShaderDesriptor = entry->id;
			auto pixelShaderDescriptor = entry->id;
			State::GetSingleton()->ModifyShaderLookup(*shader, vertexShaderDesriptor, pixelShaderDescriptor);
			shaderCache.GetPixelShader(*shader, pixelShaderDescriptor);
		}
	}
	BSShaderHooks::hk_LoadShaders((REX::BSShader*)shader, stream);
};

bool hk_BSShader_BeginTechnique(RE::BSShader* shader, uint32_t vertexDescriptor, uint32_t pixelDescriptor, bool skipPixelShader);

decltype(&hk_BSShader_BeginTechnique) ptr_BSShader_BeginTechnique;

bool hk_BSShader_BeginTechnique(RE::BSShader* shader, uint32_t vertexDescriptor, uint32_t pixelDescriptor, bool skipPixelShader)
{
	auto state = State::GetSingleton();
	state->currentShader = shader;
	state->currentVertexDescriptor = vertexDescriptor;
	state->currentPixelDescriptor = pixelDescriptor;
	state->updateShader = true;

	return (ptr_BSShader_BeginTechnique)(shader, vertexDescriptor, pixelDescriptor, skipPixelShader);
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
	//auto& shaderCache = SIE::ShaderCache::Instance();

	//if (shaderCache.IsEnabled())
	//	Bindings::GetSingleton()->SetDirtyStates(isCompute);

	(ptr_BSGraphics_SetDirtyStates)(isCompute);

	State::GetSingleton()->Draw();
}

decltype(&ID3D11Device::CreateVertexShader) ptrCreateVertexShader;
decltype(&ID3D11Device::CreatePixelShader) ptrCreatePixelShader;

HRESULT hk_CreateVertexShader(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader)
{
	HRESULT hr = (This->*ptrCreateVertexShader)(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);

	if (SUCCEEDED(hr))
		RegisterShaderBytecode(*ppVertexShader, pShaderBytecode, BytecodeLength);

	return hr;
}

HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)
{
	HRESULT hr = (This->*ptrCreatePixelShader)(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);

	if (SUCCEEDED(hr))
		RegisterShaderBytecode(*ppPixelShader, pShaderBytecode, BytecodeLength);

	return hr;
}

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChain;

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	[[maybe_unused]] const D3D_FEATURE_LEVEL* pFeatureLevels,
	[[maybe_unused]] UINT FeatureLevels,
	UINT SDKVersion,
	const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	[[maybe_unused]] D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	logger::info("Upgrading D3D11 feature level to 11.1");

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;  // Create a device with only the latest feature level

#ifndef NDEBUG
	Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	HRESULT hr = (*ptrD3D11CreateDeviceAndSwapChain)(
		pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		nullptr,
		ppImmediateContext);

	return hr;
}

void hk_BSShaderRenderTargets_Create();

decltype(&hk_BSShaderRenderTargets_Create) ptr_BSShaderRenderTargets_Create;

void hk_BSShaderRenderTargets_Create()
{
	RE::GetINISetting("bUse64bitsHDRRenderTarget:Display")->data.b = false;
	(ptr_BSShaderRenderTargets_Create)();
	State::GetSingleton()->Setup();
}

static void hk_PollInputDevices(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events);
static inline REL::Relocation<decltype(hk_PollInputDevices)> _InputFunc;

void hk_PollInputDevices(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events)
{
	bool blockedDevice = true;
	auto menu = Menu::GetSingleton();

	if (a_events) {
		menu->ProcessInputEvents(a_events);

		if (*a_events) {
			if (auto device = (*a_events)->GetDevice()) {
				// Check that the device is not a Gamepad or VR controller. If it is, unblock input.
				auto vrDevice = (REL::Module::IsVR() && ((device == RE::INPUT_DEVICES::INPUT_DEVICE::kVivePrimary) ||
															(device == RE::INPUT_DEVICES::INPUT_DEVICE::kViveSecondary) ||
															(device == RE::INPUT_DEVICES::INPUT_DEVICE::kOculusPrimary) ||
															(device == RE::INPUT_DEVICES::INPUT_DEVICE::kOculusSecondary) ||
															(device == RE::INPUT_DEVICES::INPUT_DEVICE::kWMRPrimary) ||
															(device == RE::INPUT_DEVICES::INPUT_DEVICE::kWMRSecondary)));
				blockedDevice = !((device == RE::INPUT_DEVICES::INPUT_DEVICE::kGamepad) || vrDevice);
			}
		}
	}

	if (blockedDevice && menu->ShouldSwallowInput()) {  //the menu is open, eat all keypresses
		constexpr RE::InputEvent* const dummy[] = { nullptr };
		_InputFunc(a_dispatcher, dummy);
		return;
	}

	_InputFunc(a_dispatcher, a_events);
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

			auto manager = RE::BSGraphics::Renderer::GetSingleton();

			auto context = manager->GetRuntimeData().context;
			auto swapchain = manager->GetRuntimeData().renderWindows->swapChain;
			auto device = manager->GetRuntimeData().forwarder;

			logger::info("Detouring virtual function tables");

			*(uintptr_t*)&ptr_IDXGISwapChain_Present = Detours::X64::DetourClassVTable(*(uintptr_t*)swapchain, &hk_IDXGISwapChain_Present, 8);

			auto& shaderCache = SIE::ShaderCache::Instance();
			if (shaderCache.IsDump()) {
				*(uintptr_t*)&ptrCreateVertexShader = Detours::X64::DetourClassVTable(*(uintptr_t*)device, &hk_CreateVertexShader, 12);
				*(uintptr_t*)&ptrCreatePixelShader = Detours::X64::DetourClassVTable(*(uintptr_t*)device, &hk_CreatePixelShader, 15);
			}
			Menu::GetSingleton()->Init(swapchain, device, context);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderISSAOCompositeSAO_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			State::GetSingleton()->DrawDeferred();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderISSAOCompositeFog_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			State::GetSingleton()->DrawDeferred();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderISSAOCompositeSAOFog_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			State::GetSingleton()->DrawDeferred();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderHDRTonemapBlendCinematic_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			State::GetSingleton()->DrawPreProcess();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderHDRTonemapBlendCinematicFade_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			State::GetSingleton()->DrawPreProcess();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct WndProcHandler_Hook
	{
		static LRESULT thunk(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			if (a_msg == WM_KILLFOCUS) {
				Menu::GetSingleton()->OnFocusLost();
				auto& io = ImGui::GetIO();
				io.ClearInputKeys();
				io.ClearEventsQueue();
			}
			return func(a_hwnd, a_msg, a_wParam, a_lParam);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RegisterClassA_Hook
	{
		static ATOM thunk(WNDCLASSA* a_wndClass)
		{
			WndProcHandler_Hook::func = reinterpret_cast<uintptr_t>(a_wndClass->lpfnWndProc);
			a_wndClass->lpfnWndProc = &WndProcHandler_Hook::thunk;

			return func(a_wndClass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateRenderTarget_Main
	{
		static void thunk(RE::BSGraphics::Renderer* This, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
		{
			State::GetSingleton()->ModifyRenderTarget(a_target, a_properties);
			func(This, a_target, a_properties);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateRenderTarget_Normals
	{
		static void thunk(RE::BSGraphics::Renderer* This, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
		{
			State::GetSingleton()->ModifyRenderTarget(a_target, a_properties);
			func(This, a_target, a_properties);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateRenderTarget_NormalsSwap
	{
		static void thunk(RE::BSGraphics::Renderer* This, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
		{
			State::GetSingleton()->ModifyRenderTarget(a_target, a_properties);
			func(This, a_target, a_properties);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Install()
	{
		SKSE::AllocTrampoline(14);
		auto& trampoline = SKSE::GetTrampoline();
		logger::info("Hooking BSInputDeviceManager::PollInputDevices");
		_InputFunc = trampoline.write_call<5>(REL::RelocationID(67315, 68617).address() + REL::Relocate(0x7B, 0x7B, 0x81), hk_PollInputDevices);  //BSInputDeviceManager::PollInputDevices -> Inputfunc

		logger::info("Hooking BSShader::LoadShaders");
		*(uintptr_t*)&ptr_BSShader_LoadShaders = Detours::X64::DetourFunction(REL::RelocationID(101339, 108326).address(), (uintptr_t)&hk_BSShader_LoadShaders);
		logger::info("Hooking BSShader::BeginTechnique");
		*(uintptr_t*)&ptr_BSShader_BeginTechnique = Detours::X64::DetourFunction(REL::RelocationID(101341, 108328).address(), (uintptr_t)&hk_BSShader_BeginTechnique);
		logger::info("Hooking BSGraphics::SetDirtyStates");
		*(uintptr_t*)&ptr_BSGraphics_SetDirtyStates = Detours::X64::DetourFunction(REL::RelocationID(75580, 77386).address(), (uintptr_t)&hk_BSGraphics_SetDirtyStates);

		logger::info("Hooking BSGraphics::Renderer::InitD3D");
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));

		logger::info("Hooking deferred passes");
		stl::write_vfunc<0x2, BSImagespaceShaderISSAOCompositeSAO_SetupTechnique>(RE::VTABLE_BSImagespaceShaderISSAOCompositeSAO[0]);
		stl::write_vfunc<0x2, BSImagespaceShaderISSAOCompositeFog_SetupTechnique>(RE::VTABLE_BSImagespaceShaderISSAOCompositeFog[0]);
		stl::write_vfunc<0x2, BSImagespaceShaderISSAOCompositeSAOFog_SetupTechnique>(RE::VTABLE_BSImagespaceShaderISSAOCompositeSAOFog[0]);

		logger::info("Hooking preprocess passes");
		stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematic_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematic[0]);
		stl::write_vfunc<0x2, BSImagespaceShaderHDRTonemapBlendCinematicFade_SetupTechnique>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematicFade[0]);

		logger::info("Hooking WndProcHandler");
		stl::write_thunk_call_6<RegisterClassA_Hook>(REL::VariantID(75591, 77226, 0xDC4B90).address() + REL::VariantOffset(0x8E, 0x15C, 0x99).offset());

		//logger::info("Hooking D3D11CreateDeviceAndSwapChain");
		//*(FARPROC*)&ptrD3D11CreateDeviceAndSwapChain = GetProcAddress(GetModuleHandleA("d3d11.dll"), "D3D11CreateDeviceAndSwapChain");
		//SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChain, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");

		logger::info("Hooking BSShaderRenderTargets::Create");
		*(uintptr_t*)&ptr_BSShaderRenderTargets_Create = Detours::X64::DetourFunction(REL::RelocationID(100458, 107175).address(), (uintptr_t)&hk_BSShaderRenderTargets_Create);

		logger::info("Hooking BSShaderRenderTargets::Create::CreateRenderTarget(s)");
		bool marketplaceUpdate = REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_6_1130;
		stl::write_thunk_call<CreateRenderTarget_Main>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x3F0, marketplaceUpdate ? 0x3F3 : 0x200, 0x548));
		stl::write_thunk_call<CreateRenderTarget_Normals>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x458, marketplaceUpdate ? 0x45B : 0x454, 0x5B0));
		stl::write_thunk_call<CreateRenderTarget_NormalsSwap>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x46B, marketplaceUpdate ? 0x46E : 0x467, 0x5C3));
	}
}