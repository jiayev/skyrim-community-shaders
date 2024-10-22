#include "Hooks.h"

#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Util.h"

#include "ShaderTools/BSShaderHooks.h"

#include "Streamline.h"

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

template <class ShaderType>
void DumpShader(const REX::BSShader* thisClass, const ShaderType* shader, const std::pair<std::unique_ptr<uint8_t[]>, size_t>& bytecode)
{
	static_assert(std::is_same_v<ShaderType, RE::BSGraphics::VertexShader> || std::is_same_v<ShaderType, RE::BSGraphics::PixelShader>);

	uint8_t* dxbcData = new uint8_t[bytecode.second];
	size_t dxbcLen = bytecode.second;
	memcpy(dxbcData, bytecode.first.get(), bytecode.second);

	constexpr auto shaderExtStr = std::is_same_v<ShaderType, RE::BSGraphics::VertexShader> ? "vs" : "ps";
	constexpr auto shaderTypeStr = std::is_same_v<ShaderType, RE::BSGraphics::VertexShader> ? "vertex" : "pixel";

	std::string dumpDir = std::format("Data\\ShaderDump\\{}\\{:X}.{}.bin", thisClass->m_LoaderType, shader->id, shaderExtStr);
	auto directoryPath = std::format("Data\\ShaderDump\\{}", thisClass->m_LoaderType);
	logger::debug(fmt::runtime("Dumping {} shader {} with id {:x} at {}"), shaderTypeStr, thisClass->m_LoaderType, shader->id, dumpDir);

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

struct BSShader_LoadShaders
{
	static void thunk(RE::BSShader* shader, std::uintptr_t stream)
	{
		func(shader, stream);
		auto& shaderCache = SIE::ShaderCache::Instance();

		if (shaderCache.IsDiskCache() || shaderCache.IsDump()) {
			if (shaderCache.IsDiskCache()) {
				TruePBR::GetSingleton()->GenerateShaderPermutations(shader);
			}

			for (const auto& entry : shader->vertexShaders) {
				if (entry->shader && shaderCache.IsDump()) {
					const auto& bytecode = GetShaderBytecode(entry->shader);
					DumpShader((REX::BSShader*)shader, entry, bytecode);
				}
				auto vertexShaderDesriptor = entry->id;
				auto pixelShaderDescriptor = entry->id;
				State::GetSingleton()->ModifyShaderLookup(*shader, vertexShaderDesriptor, pixelShaderDescriptor);
				shaderCache.GetVertexShader(*shader, vertexShaderDesriptor);
			}
			for (const auto& entry : shader->pixelShaders) {
				if (entry->shader && shaderCache.IsDump()) {
					const auto& bytecode = GetShaderBytecode(entry->shader);
					DumpShader((REX::BSShader*)shader, entry, bytecode);
				}
				auto vertexShaderDesriptor = entry->id;
				auto pixelShaderDescriptor = entry->id;
				State::GetSingleton()->ModifyShaderLookup(*shader, vertexShaderDesriptor, pixelShaderDescriptor);
				shaderCache.GetPixelShader(*shader, pixelShaderDescriptor);
				State::GetSingleton()->ModifyShaderLookup(*shader, vertexShaderDesriptor, pixelShaderDescriptor, true);
				shaderCache.GetPixelShader(*shader, pixelShaderDescriptor);
			}
		}
		BSShaderHooks::hk_LoadShaders((REX::BSShader*)shader, stream);
	};
	static inline REL::Relocation<decltype(thunk)> func;
};

bool Hooks::BSShader_BeginTechnique::thunk(RE::BSShader* shader, uint32_t vertexDescriptor, uint32_t pixelDescriptor, bool skipPixelShader)
{
	auto state = State::GetSingleton();

	state->updateShader = true;
	state->currentShader = shader;

	state->currentVertexDescriptor = vertexDescriptor;
	state->currentPixelDescriptor = pixelDescriptor;

	state->modifiedVertexDescriptor = vertexDescriptor;
	state->modifiedPixelDescriptor = pixelDescriptor;

	state->ModifyShaderLookup(*shader, state->modifiedVertexDescriptor, state->modifiedPixelDescriptor);

	bool shaderFound = func(shader, vertexDescriptor, pixelDescriptor, skipPixelShader);

	if (!shaderFound) {
		auto& shaderCache = SIE::ShaderCache::Instance();
		RE::BSGraphics::VertexShader* vertexShader = shaderCache.GetVertexShader(*shader, state->modifiedVertexDescriptor);
		RE::BSGraphics::PixelShader* pixelShader = shaderCache.GetPixelShader(*shader, state->modifiedPixelDescriptor);
		if (vertexShader == nullptr || (!skipPixelShader && pixelShader == nullptr)) {
			shaderFound = false;
		} else {
			state->settingCustomShader = true;
			RE::BSGraphics::RendererShadowState::GetSingleton()->SetVertexShader(vertexShader);
			if (skipPixelShader) {
				pixelShader = nullptr;
			}
			RE::BSGraphics::RendererShadowState::GetSingleton()->SetPixelShader(pixelShader);
			state->settingCustomShader = false;
			shaderFound = true;
		}
	}

	state->lastModifiedVertexDescriptor = state->modifiedVertexDescriptor;
	state->lastModifiedPixelDescriptor = state->modifiedPixelDescriptor;

	return shaderFound;
}

struct IDXGISwapChain_Present
{
	static HRESULT WINAPI thunk(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
	{
		State::GetSingleton()->Reset();
		Menu::GetSingleton()->DrawOverlay();
		Streamline::GetSingleton()->Present();
		auto retval = func(This, SyncInterval, Flags);
		TracyD3D11Collect(State::GetSingleton()->tracyCtx);
		return retval;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void Hooks::BSGraphics_SetDirtyStates::thunk(bool isCompute)
{
	func(isCompute);
	State::GetSingleton()->Draw();
}

struct ID3D11Device_CreateVertexShader
{
	static HRESULT thunk(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader)
	{
		HRESULT hr = func(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);

		if (SUCCEEDED(hr))
			RegisterShaderBytecode(*ppVertexShader, pShaderBytecode, BytecodeLength);

		return hr;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct ID3D11Device_CreatePixelShader
{
	static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* This, const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader)
	{
		HRESULT hr = func(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);

		if (SUCCEEDED(hr))
			RegisterShaderBytecode(*ppPixelShader, pShaderBytecode, BytecodeLength);

		return hr;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

decltype(&CreateDXGIFactory) ptrCreateDXGIFactory;

HRESULT WINAPI hk_CreateDXGIFactory(REFIID, void** ppFactory)
{
	return Streamline::GetSingleton()->CreateDXGIFactory(__uuidof(IDXGIFactory1), ppFactory);
}

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChain;

HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChainNoStreamline(
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
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	DXGI_ADAPTER_DESC adapterDesc;
	pAdapter->GetDesc(&adapterDesc);
	State::GetSingleton()->SetAdapterDescription(adapterDesc.Description);

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;  // Create a device with only the latest feature level
	return ptrD3D11CreateDeviceAndSwapChain(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);
}

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
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	DXGI_ADAPTER_DESC adapterDesc;
	pAdapter->GetDesc(&adapterDesc);
	State::GetSingleton()->SetAdapterDescription(adapterDesc.Description);

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;  // Create a device with only the latest feature level
	auto result = Streamline::GetSingleton()->CreateDeviceAndSwapChain(
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
		pFeatureLevel,
		ppImmediateContext);
	if (SUCCEEDED(result)) {
		return result;
	}
	return ptrD3D11CreateDeviceAndSwapChain(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);
}

struct BSShaderRenderTargets_Create
{
	static void thunk()
	{
		func();
		State::GetSingleton()->Setup();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct BSInputDeviceManager_PollInputDevices
{
	static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events)
	{
		bool blockedDevice = true;
		auto menu = Menu::GetSingleton();

		if (a_events) {
			menu->ProcessInputEvents(a_events);

			if (*a_events) {
				if (auto device = (*a_events)->GetDevice()) {
					// Check that the device is not a Gamepad or VR controller. If it is, unblock input.
					bool vrDevice = false;
#ifdef ENABLE_SKYRIM_VR
					vrDevice = (REL::Module::IsVR() && ((device == RE::INPUT_DEVICES::INPUT_DEVICE::kVivePrimary) ||
														   (device == RE::INPUT_DEVICES::INPUT_DEVICE::kViveSecondary) ||
														   (device == RE::INPUT_DEVICES::INPUT_DEVICE::kOculusPrimary) ||
														   (device == RE::INPUT_DEVICES::INPUT_DEVICE::kOculusSecondary) ||
														   (device == RE::INPUT_DEVICES::INPUT_DEVICE::kWMRPrimary) ||
														   (device == RE::INPUT_DEVICES::INPUT_DEVICE::kWMRSecondary)));
#endif
					blockedDevice = !((device == RE::INPUT_DEVICES::INPUT_DEVICE::kGamepad) || vrDevice);
				}
			}
		}

		if (blockedDevice && menu->ShouldSwallowInput()) {  //the menu is open, eat all keypresses
			constexpr RE::InputEvent* const dummy[] = { nullptr };
			func(a_dispatcher, dummy);
			return;
		}

		func(a_dispatcher, a_events);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

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

			auto context = reinterpret_cast<ID3D11DeviceContext*>(manager->GetRuntimeData().context);
			auto swapchain = reinterpret_cast<IDXGISwapChain*>(manager->GetRuntimeData().renderWindows->swapChain);
			auto device = reinterpret_cast<ID3D11Device*>(manager->GetRuntimeData().forwarder);

			logger::info("Detouring virtual function tables");
			stl::detour_vfunc<8, IDXGISwapChain_Present>(swapchain);

			auto& shaderCache = SIE::ShaderCache::Instance();
			if (shaderCache.IsDump()) {
				stl::detour_vfunc<12, ID3D11Device_CreateVertexShader>(device);
				stl::detour_vfunc<15, ID3D11Device_CreatePixelShader>(device);
			}
			Menu::GetSingleton()->Init(swapchain, device, context);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct WndProcHandler_Hook
	{
		static LRESULT thunk(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			auto menu = Menu::GetSingleton();
			if (a_msg == WM_KILLFOCUS && menu->initialized) {
				menu->OnFocusLost();
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

	struct CreateRenderTarget_Snow
	{
		static void thunk(RE::BSGraphics::Renderer* This, RE::RENDER_TARGETS::RENDER_TARGET a_target, RE::BSGraphics::RenderTargetProperties* a_properties)
		{
			State::GetSingleton()->ModifyRenderTarget(a_target, a_properties);
			func(This, a_target, a_properties);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShader__BeginTechnique_SetVertexShader
	{
		static void thunk(RE::BSGraphics::Renderer* This, RE::BSGraphics::VertexShader* a_vertexShader)
		{
			func(This, a_vertexShader);  // TODO: Remove original call
			auto state = State::GetSingleton();
			if (!state->settingCustomShader) {
				auto& shaderCache = SIE::ShaderCache::Instance();
				if (shaderCache.IsEnabled()) {
					auto currentShader = state->currentShader;
					auto type = currentShader->shaderType.get();
					if (type > 0 && type < RE::BSShader::Type::Total) {
						if (state->enabledClasses[type - 1]) {
							RE::BSGraphics::VertexShader* vertexShader = shaderCache.GetVertexShader(*currentShader, state->modifiedVertexDescriptor);
							if (vertexShader) {
								state->context->VSSetShader(reinterpret_cast<ID3D11VertexShader*>(vertexShader->shader), NULL, NULL);
								auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
								GET_INSTANCE_MEMBER(currentVertexShader, shadowState)
								currentVertexShader = a_vertexShader;
								return;
							}
						}
					}
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShader__BeginTechnique_SetPixelShader
	{
		static void thunk(RE::BSGraphics::Renderer* This, RE::BSGraphics::PixelShader* a_pixelShader)
		{
			auto state = State::GetSingleton();
			if (!state->settingCustomShader) {
				auto& shaderCache = SIE::ShaderCache::Instance();
				if (shaderCache.IsEnabled()) {
					auto currentShader = state->currentShader;
					auto type = currentShader->shaderType.get();
					if (type > 0 && type < RE::BSShader::Type::Total) {
						if (state->enabledClasses[type - 1]) {
							RE::BSGraphics::PixelShader* pixelShader = shaderCache.GetPixelShader(*currentShader, state->modifiedPixelDescriptor);
							if (pixelShader) {
								state->context->PSSetShader(reinterpret_cast<ID3D11PixelShader*>(pixelShader->shader), NULL, NULL);
								auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
								GET_INSTANCE_MEMBER(currentPixelShader, shadowState)
								currentPixelShader = a_pixelShader;
								return;
							}
						}
					}
				}
			}
			func(This, a_pixelShader);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateDepthStencil_PrecipitationMask
	{
		static void thunk(RE::BSGraphics::Renderer* This, uint32_t a_target, RE::BSGraphics::DepthStencilTargetProperties* a_properties)
		{
			a_properties->use16BitsDepth = true;
			a_properties->stencil = false;
			func(This, a_target, a_properties);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateCubemapRenderTarget_Reflections
	{
		static void thunk(RE::BSGraphics::Renderer* This, uint32_t a_target, RE::BSGraphics::CubeMapRenderTargetProperties* a_properties)
		{
			a_properties->height = 128;
			a_properties->width = 128;
			func(This, a_target, a_properties);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Sky Reflection Fix
	struct TESWaterReflections_Update_Actor_GetLOSPosition
	{
		static RE::NiPoint3* thunk(RE::PlayerCharacter* a_player, RE::NiPoint3* a_target, int unk1, float unk2)
		{
			auto ret = func(a_player, a_target, unk1, unk2);

			auto camera = RE::PlayerCamera::GetSingleton();
			ret->x = camera->cameraRoot->world.translate.x;
			ret->y = camera->cameraRoot->world.translate.y;
			ret->z = camera->cameraRoot->world.translate.z;

			return ret;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

#ifdef TRACY_ENABLE
	struct Main_Update
	{
		static void thunk(RE::Main* a_this, float a2)
		{
			func(a_this, a2);
			FrameMark;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
#endif

	namespace CSShadersSupport
	{
		RE::BSImagespaceShader* CurrentlyDispatchedShader = nullptr;
		RE::BSComputeShader* CurrentlyDispatchedComputeShader = nullptr;
		uint32_t CurrentComputeShaderTechniqueId = 0;

		RE::BSImagespaceShader* vlGenerateShader = nullptr;
		RE::BSImagespaceShader* vlRaymarchShader = nullptr;

		RE::BSImagespaceShader* CreateVLShader(const std::string_view& name, const std::string_view& fileName, RE::BSComputeShader* computeShader)
		{
			auto shader = RE::BSImagespaceShader::Create();
			shader->shaderType = RE::BSShader::Type::ImageSpace;
			shader->fxpFilename = fileName.data();
			shader->name = name.data();
			shader->originalShaderName = fileName.data();
			shader->computeShader = computeShader;
			shader->isComputeShader = true;
			return shader;
		}

		RE::BSImagespaceShader* GetOrCreateVLGenerateShader(RE::BSComputeShader* computeShader)
		{
			if (vlGenerateShader == nullptr) {
				vlGenerateShader = CreateVLShader("BSImagespaceShaderVolumetricLightingGenerateCS", "ISVolumetricLightingGenerateCS", computeShader);
			}
			return vlGenerateShader;
		}

		RE::BSImagespaceShader* GetOrCreateVLRaymarchShader(RE::BSComputeShader* computeShader)
		{
			if (vlRaymarchShader == nullptr) {
				vlRaymarchShader = CreateVLShader("BSImagespaceShaderVolumetricLightingRaymarchCS", "ISVolumetricLightingRaymarchCS", computeShader);
			}
			return vlRaymarchShader;
		}

		struct BSImagespaceShader_DispatchComputeShader
		{
			static void thunk(RE::BSImagespaceShader* shader, uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
			{
				CurrentlyDispatchedShader = shader;
				func(shader, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
				CurrentlyDispatchedShader = nullptr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSComputeShader_Dispatch
		{
			static void thunk(RE::BSComputeShader* shader, uint32_t techniqueId, uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
			{
				CurrentlyDispatchedComputeShader = shader;
				CurrentComputeShaderTechniqueId = techniqueId;
				func(shader, techniqueId, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
				CurrentlyDispatchedComputeShader = nullptr;
				CurrentComputeShaderTechniqueId = 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Renderer_DispatchCSShader
		{
			static void thunk(RE::BSGraphics::Renderer* renderer, RE::BSGraphics::ComputeShader* shader, uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
			{
				auto state = State::GetSingleton();
				if (state->enabledClasses[RE::BSShader::Type::ImageSpace]) {
					auto& shaderCache = SIE::ShaderCache::Instance();
					RE::BSImagespaceShader* isShader = CurrentlyDispatchedShader;
					uint32_t techniqueId = CurrentComputeShaderTechniqueId;
					if (CurrentlyDispatchedShader == nullptr) {
						techniqueId = 0;
						if (CurrentlyDispatchedComputeShader->name == std::string_view("ISVolumetricLightingGenerateCS")) {
							isShader = GetOrCreateVLGenerateShader(CurrentlyDispatchedComputeShader);
						} else if (CurrentlyDispatchedComputeShader->name == std::string_view("ISVolumetricLightingRaymarchCS")) {
							isShader = GetOrCreateVLRaymarchShader(CurrentlyDispatchedComputeShader);
						}
					}
					if (isShader != nullptr) {
						if (auto* computeShader = shaderCache.GetComputeShader(*isShader, techniqueId)) {
							shader = computeShader;
						}
					}
				}
				func(renderer, shader, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		logger::info("Hooking BSInputDeviceManager::PollInputDevices");
		stl::write_thunk_call<BSInputDeviceManager_PollInputDevices>(REL::RelocationID(67315, 68617).address() + REL::Relocate(0x7B, 0x7B, 0x81));

		logger::info("Hooking BSShader::LoadShaders");
		stl::detour_thunk<BSShader_LoadShaders>(REL::RelocationID(101339, 108326));
		logger::info("Hooking BSShader::BeginTechnique");
		stl::detour_thunk<BSShader_BeginTechnique>(REL::RelocationID(101341, 108328));

		stl::write_thunk_call<BSShader__BeginTechnique_SetVertexShader>(REL::RelocationID(101341, 108328).address() + REL::Relocate(0xC3, 0xD5));
		stl::write_thunk_call<BSShader__BeginTechnique_SetPixelShader>(REL::RelocationID(101341, 108328).address() + REL::Relocate(0xD7, 0xEB));

		logger::info("Hooking BSGraphics::SetDirtyStates");
		stl::detour_thunk<BSGraphics_SetDirtyStates>(REL::RelocationID(75580, 77386));

		logger::info("Hooking BSGraphics::Renderer::InitD3D");
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));

		logger::info("Hooking WndProcHandler");
		stl::write_thunk_call<RegisterClassA_Hook, 6>(REL::VariantID(75591, 77226, 0xDC4B90).address() + REL::VariantOffset(0x8E, 0x15C, 0x99).offset());

		logger::info("Hooking BSShaderRenderTargets::Create");
		stl::detour_thunk<BSShaderRenderTargets_Create>(REL::RelocationID(100458, 107175));

		logger::info("Hooking BSShaderRenderTargets::Create::CreateRenderTarget(s)");
		stl::write_thunk_call<CreateRenderTarget_Main>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x3F0, 0x3F3, 0x548));
		stl::write_thunk_call<CreateRenderTarget_Normals>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x458, 0x45B, 0x5B0));
		stl::write_thunk_call<CreateRenderTarget_NormalsSwap>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x46B, 0x46E, 0x5C3));
		stl::write_thunk_call<CreateRenderTarget_Snow>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x406, 0x409, 0x55e));
		stl::write_thunk_call<CreateDepthStencil_PrecipitationMask>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x1245, 0x123B, 0x1917));
		stl::write_thunk_call<CreateCubemapRenderTarget_Reflections>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0xA25, 0xA25, 0xCD2));

#ifdef TRACY_ENABLE
		stl::write_thunk_call<Main_Update>(REL::RelocationID(35551, 36544).address() + REL::Relocate(0x11F, 0x160));
#endif

		logger::info("Hooking BSImagespaceShader");
		stl::detour_thunk<CSShadersSupport::BSImagespaceShader_DispatchComputeShader>(REL::RelocationID(100952, 107734));

		logger::info("Hooking BSComputeShader");
		stl::write_vfunc<0x02, CSShadersSupport::BSComputeShader_Dispatch>(RE::VTABLE_BSComputeShader[0]);

		logger::info("Hooking Renderer::DispatchCSShader");
		stl::detour_thunk<CSShadersSupport::Renderer_DispatchCSShader>(REL::RelocationID(75532, 77329));

		logger::info("Hooking TESWaterReflections::Update_Actor::GetLOSPosition for Sky Reflection Fix");
		stl::write_thunk_call<TESWaterReflections_Update_Actor_GetLOSPosition>(REL::RelocationID(31373, 32160).address() + REL::Relocate(0x1AD, 0x1CA, 0x1ed));
	}

	void InstallD3DHooks()
	{
		auto streamline = Streamline::GetSingleton();
		auto state = State::GetSingleton();

		streamline->LoadInterposer();

		if (streamline->interposer && !state->IsFeatureDisabled("Frame Generation")) {
			Streamline::InstallHooks();

			logger::info("Hooking D3D11CreateDeviceAndSwapChain");
			*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChain = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChain, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");

			logger::info("Hooking CreateDXGIFactory");
			*(uintptr_t*)&ptrCreateDXGIFactory = SKSE::PatchIAT(hk_CreateDXGIFactory, "dxgi.dll", !REL::Module::IsVR() ? "CreateDXGIFactory" : "CreateDXGIFactory1");
		} else if (!state->IsFeatureDisabled("Upscaling")) {
			logger::info("Hooking D3D11CreateDeviceAndSwapChain");
			*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChain = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChainNoStreamline, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
		}
	}
}