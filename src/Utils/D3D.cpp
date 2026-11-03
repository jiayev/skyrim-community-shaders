#include "D3D.h"
#include "Utils/FxcShaderStore.h"
#include "Utils/SharedShaderStore.h"

#include "Deferred.h"
#include "Feature.h"
#include "Features/TerrainBlending.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/FileRename.h"
#include "Utils/Format.h"
#include "Utils/Logging.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace Util
{
	namespace
	{
		std::mutex multithreadScopeMutex;
		int multithreadScopeHolders = 0;
		BOOL multithreadScopePriorProtection = FALSE;
	}

	MultithreadScope::MultithreadScope(ID3D11DeviceContext* context)
	{
		if (!context || FAILED(context->QueryInterface(multithread.put()))) {
			multithread = nullptr;
			return;
		}
		{
			std::lock_guard lock(multithreadScopeMutex);
			const BOOL prior = multithread->SetMultithreadProtected(TRUE);
			if (multithreadScopeHolders++ == 0)
				multithreadScopePriorProtection = prior;
		}
		multithread->Enter();
	}

	MultithreadScope::~MultithreadScope()
	{
		if (!multithread)
			return;
		multithread->Leave();
		std::lock_guard lock(multithreadScopeMutex);
		if (--multithreadScopeHolders == 0)
			multithread->SetMultithreadProtected(multithreadScopePriorProtection);
	}

	ID3D11ShaderResourceView* GetCurrentSceneDepthSRV(bool prefer16bit)
	{
		auto renderer = globals::game::renderer;
		if (!renderer)
			return nullptr;
		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		if (globals::deferred && globals::deferred->sceneDepthFinal)
			return zPrepassCopy.depthSRV;

		auto& tb = globals::features::terrainBlending;
		if (tb.loaded && tb.settings.Enabled) {
			auto* srv = prefer16bit ? (tb.blendedDepthTexture16 ? tb.blendedDepthTexture16->srv.get() : nullptr) : (tb.blendedDepthTexture ? tb.blendedDepthTexture->srv.get() : nullptr);
			if (srv)
				return srv;
		}
		return zPrepassCopy.depthSRV;
	}

	ID3D11ShaderResourceView* GetSRVFromRTV(const ID3D11RenderTargetView* a_rtv)
	{
		if (a_rtv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < RE::RENDER_TARGETS::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_rtv == rt.RTV) {
						return rt.SRV;
					}
				}
			}
		}
		return nullptr;
	}

	ID3D11RenderTargetView* GetRTVFromSRV(const ID3D11ShaderResourceView* a_srv)
	{
		if (a_srv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < RE::RENDER_TARGETS::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_srv == rt.SRV || a_srv == rt.SRVCopy) {
						return rt.RTV;
					}
				}
			}
		}
		return nullptr;
	}

	std::string GetNameFromSRV(const ID3D11ShaderResourceView* a_srv)
	{
		using RENDER_TARGET = RE::RENDER_TARGETS::RENDER_TARGET;

		if (a_srv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < RE::RENDER_TARGETS::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_srv == rt.SRV || a_srv == rt.SRVCopy) {
						return std::string(magic_enum::enum_name(static_cast<RENDER_TARGET>(i)));
					}
				}
			}
		}
		return "NONE";
	}

	std::string GetNameFromRTV(const ID3D11RenderTargetView* a_rtv)
	{
		using RENDER_TARGET = RE::RENDER_TARGETS::RENDER_TARGET;
		if (a_rtv) {
			if (auto r = globals::game::renderer) {
				for (int i = 0; i < RE::RENDER_TARGETS::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_rtv == rt.RTV) {
						return std::string(magic_enum::enum_name(static_cast<RENDER_TARGET>(i)));
					}
				}
			}
		}
		return "NONE";
	}

	GUID WKPDID_D3DDebugObjectNameT = { 0x429b8c22, 0x9188, 0x4b0c, 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 };

	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...)
	{
		if (!Resource)
			return;

		char buffer[1024];
		va_list va;

		va_start(va, Format);
		int len = _vsnprintf_s(buffer, _TRUNCATE, Format, va);
		va_end(va);

		if (len < 0)
			len = static_cast<int>(strnlen(buffer, sizeof(buffer)));
		Resource->SetPrivateData(WKPDID_D3DDebugObjectNameT, static_cast<UINT>(len), buffer);
	}

	// Per-frame getters would otherwise retry a failed compile every frame.
	namespace
	{
		std::mutex shaderCompileFailuresMutex;
		std::unordered_set<std::string> shaderCompileFailures;
	}

	void ClearShaderCompileFailures()
	{
		std::lock_guard lock(shaderCompileFailuresMutex);
		shaderCompileFailures.clear();
	}

	namespace
	{
		/**
		 * @brief Creates the shader object for a CompileShader target.
		 * @return The device's result; a target without a creation path returns S_OK and no shader.
		 */
		HRESULT CreateShaderObject(ID3D11Device* device, const char* programType, ID3DBlob* blob, ID3D11DeviceChild** shader)
		{
			*shader = nullptr;
			const void* code = blob->GetBufferPointer();
			const SIZE_T size = blob->GetBufferSize();
			HRESULT result = S_OK;
			if (!_stricmp(programType, "ps_5_0")) {
				ID3D11PixelShader* regShader = nullptr;
				result = device->CreatePixelShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "vs_5_0")) {
				ID3D11VertexShader* regShader = nullptr;
				result = device->CreateVertexShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "hs_5_0")) {
				ID3D11HullShader* regShader = nullptr;
				result = device->CreateHullShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "ds_5_0")) {
				ID3D11DomainShader* regShader = nullptr;
				result = device->CreateDomainShader(code, size, nullptr, &regShader);
				*shader = regShader;
			} else if (!_stricmp(programType, "cs_5_0") || !_stricmp(programType, "cs_4_0")) {
				ID3D11ComputeShader* regShader = nullptr;
				result = device->CreateComputeShader(code, size, nullptr, &regShader);
				*shader = regShader;
			}
			return result;
		}
	}

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program)
	{
		auto device = globals::d3d::device;

		// Build defines (aka convert vector->D3DCONSTANT array)
		std::vector<D3D_SHADER_MACRO> macros;
		std::string str = Util::WStringToString(FilePath);

		for (auto& i : Defines) {
			if (i.first && _stricmp(i.first, "") != 0) {
				macros.push_back({ i.first, i.second });
			} else {
				logger::error("Failed to process shader defines for {}", str);
			}
		}

		const bool debugCompile = SIE::ShaderCache::IsDebugCompile();
		if (debugCompile) {
			macros.push_back({ "D3DCOMPILE_SKIP_OPTIMIZATION", "" });
			macros.push_back({ "D3DCOMPILE_DEBUG", "" });
		}
		auto shaderDefines = globals::state->GetDefines();
		if (!shaderDefines->empty()) {
			for (unsigned int i = 0; i < shaderDefines->size(); i++)
				macros.push_back({ shaderDefines->at(i).first.c_str(), shaderDefines->at(i).second.c_str() });
		}

		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature->loaded)
				continue;
			for (const auto& [name, value] : feature->GetCommonShaderDefines()) {
				if (std::ranges::none_of(macros, [&](const auto& macro) { return name == macro.Name; }))
					macros.push_back({ name.data(), value.empty() ? nullptr : value.data() });
			}
		}
		if (!_stricmp(ProgramType, "ps_5_0"))
			macros.push_back({ "PSHADER", "" });
		else if (!_stricmp(ProgramType, "vs_5_0"))
			macros.push_back({ "VSHADER", "" });
		else if (!_stricmp(ProgramType, "hs_5_0"))
			macros.push_back({ "HULLSHADER", "" });
		else if (!_stricmp(ProgramType, "ds_5_0"))
			macros.push_back({ "DOMAINSHADER", "" });
		else if (!_stricmp(ProgramType, "cs_5_0"))
			macros.push_back({ "COMPUTESHADER", "" });
		else if (!_stricmp(ProgramType, "cs_4_0"))
			macros.push_back({ "COMPUTESHADER", "" });
		else if (!_stricmp(ProgramType, "cs_5_1"))
			macros.push_back({ "COMPUTESHADER", "" });
		else
			return nullptr;

		// Add null terminating entry
		macros.push_back({ "WINPC", "" });
		macros.push_back({ "DX11", "" });
		macros.push_back({ nullptr, nullptr });

		// Compiler setup
		const uint32_t flags = Util::FxcCompileFlags(debugCompile);

		const auto failureKey = std::format("{}|{}|{}|{}|{}", str, ProgramType, Program, flags, DefinesToString(macros));
		{
			std::lock_guard lock(shaderCompileFailuresMutex);
			if (shaderCompileFailures.contains(failureKey))
				return nullptr;
		}
		const auto recordFailure = [&]() {
			std::lock_guard lock(shaderCompileFailuresMutex);
			shaderCompileFailures.insert(failureKey);
		};

		Util::FxcRequest request;
		request.root = FilePath;
		request.includeBase = L"Data\\Shaders";
		request.defines = macros.data();
		request.entryPoint = Program;
		request.profile = ProgramType;
		request.flags = flags;
		request.debug = debugCompile;

		logger::debug("Building {} with {}", str, DefinesToString(macros));
		auto built = Util::ResolveFxcShader(request);
		ID3D11DeviceChild* shader = nullptr;
		if (built.blob && FAILED(CreateShaderObject(device, ProgramType, built.blob.get(), &shader)) && built.store.outcome != org::services::ShaderStoreOutcome::Compiled) {
			// A whole artifact can still hold bytecode the device rejects. Drop it and build from source once.
			logger::warn("Discarding the stored copy of {}, rejected by the device", str);
			Util::SharedShaderStore()->RemoveArtifact(built.store.compileKey);
			built = Util::ResolveFxcShader(request);
			shader = nullptr;
			if (built.blob)
				DX::ThrowIfFailed(CreateShaderObject(device, ProgramType, built.blob.get(), &shader));
		}
		if (!built.blob) {
			logger::warn("Shader compilation failed:\n\n{}", built.store.diagnostics.empty() ? "Unknown error" : built.store.diagnostics);
			recordFailure();
			return nullptr;
		}
		if (!built.store.diagnostics.empty())
			logger::debug("Shader logs:\n{}", built.store.diagnostics);
		if (shader)
			Util::SetResourceName(shader, "%s:%s", str.c_str(), Program);
		return shader;
	}

	// RAII wrapper for D3D mapped resources
	class ScopedD3DMap
	{
	public:
		ScopedD3DMap(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP mapType, UINT mapFlags) :
			context_(context), resource_(resource), subresource_(subresource), mapped_(false)
		{
			if (context && resource) {
				HRESULT hr = context->Map(resource, subresource, mapType, mapFlags, &mappedResource_);
				mapped_ = SUCCEEDED(hr);
			}
		}

		~ScopedD3DMap()
		{
			if (mapped_ && context_ && resource_) {
				context_->Unmap(resource_, subresource_);
			}
		}

		// Non-copyable, non-movable for simplicity
		ScopedD3DMap(const ScopedD3DMap&) = delete;
		ScopedD3DMap& operator=(const ScopedD3DMap&) = delete;

		bool IsValid() const { return mapped_; }
		D3D11_MAPPED_SUBRESOURCE* GetMappedResource() { return mapped_ ? &mappedResource_ : nullptr; }

	private:
		ID3D11DeviceContext* context_;
		ID3D11Resource* resource_;
		UINT subresource_;
		bool mapped_;
		D3D11_MAPPED_SUBRESOURCE mappedResource_{};
	};

	// Helper function to validate highlight color components
	bool ValidateHighlightColor(const std::array<float, 4>& color)
	{
		for (size_t i = 0; i < 4; ++i) {
			if (color[i] < 0.0f || color[i] > 1.0f || !std::isfinite(color[i])) {
				return false;
			}
		}
		return true;
	}

	void ApplyHighlightTintToTexture(ID3D11Texture2D* texture, bool isHighlighted, const std::array<float, 4>& highlightColor)
	{
		if (!isHighlighted || !texture)
			return;

		// Validate input color values
		if (!ValidateHighlightColor(highlightColor)) {
			logger::error("ApplyHighlightTintToTexture: Invalid highlight color values. All components must be finite and in range [0.0, 1.0]");
			return;
		}

		// Get texture description and validate dimensions
		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);

		// Performance consideration: warn about large textures (only once)
		const UINT pixelCount = desc.Width * desc.Height;
		const UINT largeTextureThreshold = 1024 * 1024;  // 1 megapixel
		if (pixelCount > largeTextureThreshold) {
			static Util::OnceFlag largeTextureWarning;
			if (largeTextureWarning.First())
				logger::warn("ApplyHighlightTintToTexture: Processing large texture ({}x{} = {} pixels). Consider using compute shader for better performance. This warning will only be shown once.",
					desc.Width, desc.Height, pixelCount);
		}  // Create a temporary staging texture to read from
		ID3D11Texture2D* stagingTexture = nullptr;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

		HRESULT hr = globals::d3d::device->CreateTexture2D(&desc, nullptr, &stagingTexture);
		if (FAILED(hr)) {
			logger::error("ApplyHighlightTintToTexture: Failed to create staging texture (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
			return;
		}

		// RAII wrapper ensures staging texture is always released
		std::unique_ptr<ID3D11Texture2D, void (*)(ID3D11Texture2D*)> stagingGuard(
			stagingTexture, [](ID3D11Texture2D* tex) { if (tex) tex->Release(); });

		// Copy the original texture to staging
		globals::d3d::context->CopyResource(stagingTexture, texture);

		// Use RAII wrapper for mapping/unmapping
		ScopedD3DMap scopedMap(globals::d3d::context, stagingTexture, 0, D3D11_MAP_READ_WRITE, 0);
		if (!scopedMap.IsValid()) {
			logger::error("ApplyHighlightTintToTexture: Failed to map staging texture");
			return;
		}

		D3D11_MAPPED_SUBRESOURCE* mapped = scopedMap.GetMappedResource();
		if (!mapped || !mapped->pData) {
			logger::error("ApplyHighlightTintToTexture: Invalid mapped resource data");
			return;
		}

		// Apply highlight tint to each pixel
		uint8_t* pixels = static_cast<uint8_t*>(mapped->pData);
		const float blendAlpha = highlightColor[3];

		for (UINT y = 0; y < desc.Height; ++y) {
			for (UINT x = 0; x < desc.Width; ++x) {
				uint8_t* pixel = pixels + (y * mapped->RowPitch + x * 4);

				// Only tint non-transparent pixels (alpha > 0)
				if (pixel[3] > 0) {
					// Apply configurable highlight tint
					// Blend the original color with the highlight color
					float originalR = pixel[0] / 255.0f;
					float originalG = pixel[1] / 255.0f;
					float originalB = pixel[2] / 255.0f;

					// Blend: original * (1 - alpha) + highlight * alpha
					pixel[0] = static_cast<uint8_t>((originalR * (1.0f - blendAlpha) + highlightColor[0] * blendAlpha) * 255.0f);
					pixel[1] = static_cast<uint8_t>((originalG * (1.0f - blendAlpha) + highlightColor[1] * blendAlpha) * 255.0f);
					pixel[2] = static_cast<uint8_t>((originalB * (1.0f - blendAlpha) + highlightColor[2] * blendAlpha) * 255.0f);
					// Alpha stays the same
				}
			}
		}

		// ScopedD3DMap destructor will automatically unmap the resource
		// Copy the modified texture back
		globals::d3d::context->CopyResource(texture, stagingTexture);

		// stagingGuard destructor will automatically release the staging texture
	}

	HRESULT CreateOverlayTextureAndRTV(ID3D11Device* device, int width, int height, ID3D11Texture2D** outTex, ID3D11RenderTargetView** outRTV)
	{
		// Input validation
		if (!device) {
			logger::error("CreateOverlayTextureAndRTV: device parameter is null");
			return E_INVALIDARG;
		}

		if (!outTex || !outRTV) {
			logger::error("CreateOverlayTextureAndRTV: output parameters cannot be null");
			return E_INVALIDARG;
		}

		if (width <= 0 || height <= 0) {
			logger::error("CreateOverlayTextureAndRTV: invalid dimensions ({}x{})", width, height);
			return E_INVALIDARG;
		}

		// Initialize output parameters
		*outTex = nullptr;
		*outRTV = nullptr;

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		HRESULT hr = device->CreateTexture2D(&desc, nullptr, outTex);
		if (FAILED(hr)) {
			logger::error("CreateOverlayTextureAndRTV: Failed to create texture (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
			return hr;
		}

		hr = device->CreateRenderTargetView(*outTex, nullptr, outRTV);
		if (FAILED(hr)) {
			logger::error("CreateOverlayTextureAndRTV: Failed to create render target view (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
			// Clean up the texture if RTV creation failed
			if (*outTex) {
				(*outTex)->Release();
				*outTex = nullptr;
			}
			return hr;
		}

		return S_OK;
	}

	HRESULT SaveTextureToFile(ID3D11Device* device, ID3D11DeviceContext* context, const std::filesystem::path& path, ID3D11Texture2D* tex)
	{
		// Input validation
		if (!device) {
			logger::error("SaveTextureToFile: device parameter is null");
			return E_INVALIDARG;
		}

		if (!context) {
			logger::error("SaveTextureToFile: context parameter is null");
			return E_INVALIDARG;
		}

		if (!tex) {
			logger::error("SaveTextureToFile: texture paramater is null");
			return E_INVALIDARG;
		}

		if (path.empty()) {
			logger::error("SaveTextureToFile: path parameter is empty");
			return E_INVALIDARG;
		}

		namespace fs = std::filesystem;

		DirectX::ScratchImage cpuImage;
		if (const auto hr = CaptureTexture(device, context, tex, cpuImage); FAILED(hr))
			return hr;

		const auto parent = path.parent_path();
		std::error_code ec;
		if (!parent.empty() && !fs::exists(parent, ec)) {
			if (!fs::create_directories(parent, ec)) {
				logger::error("SaveTextureToFile: failed to create directories");
				return HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value()));
			}
		}

		const std::wstring tmp = path.wstring().append(L".tmp");
		if (const auto hr = SaveToDDSFile(cpuImage.GetImages(), cpuImage.GetImageCount(), cpuImage.GetMetadata(), DirectX::DDS_FLAGS_NONE, tmp.c_str()); FAILED(hr)) {
			logger::error("SaveTextureToFile: failed to save to file");
			fs::remove(tmp);
			return hr;
		}

		Util::RenameSerialized(tmp, path, ec);
		if (ec) {
			fs::remove(path, ec);
			if (ec) {
				logger::error("SaveTextureToFile: failed to remove existing file");
				fs::remove(tmp);
				return HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value()));
			}
			Util::RenameSerialized(tmp, path, ec);
			if (ec) {
				logger::error("SaveTextureToFile: failed to rename {} onto {}: {} {} ({})", Util::WStringToString(tmp), Util::WStringToString(path.wstring()),
					ec.value(), ec.message(), Util::UnderUsvfs() ? "under usvfs" : "no usvfs");
				fs::remove(tmp);
				return HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value()));
			}
		}

		return S_OK;
	}

	HRESULT LoadTextureFromFile(ID3D11Device* device, const std::filesystem::path& path, ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSRV)
	{
		// Input validation
		if (!device) {
			logger::error("LoadTextureFromFile: device parameter is null");
			return E_INVALIDARG;
		}

		if (path.empty()) {
			logger::error("LoadTextureFromFile: path parameter is empty");
			return E_INVALIDARG;
		}

		*outTex = nullptr;
		*outSRV = nullptr;

		ID3D11Resource* resource;
		if (const auto hr = DirectX::CreateDDSTextureFromFile(device, path.wstring().c_str(), &resource, outSRV); FAILED(hr)) {
			logger::error("LoadTextureFromFile: failed to load resource from file");
			return hr;
		}

		const auto hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(outTex));
		resource->Release();
		resource = nullptr;

		if (FAILED(hr)) {
			logger::error("LoadTextureFromFile: failed to query texture interface");
			(*outSRV)->Release();
			*outSRV = nullptr;
			*outTex = nullptr;
			return hr;
		}

		return S_OK;
	}
}  // namespace Util
