#include "TextureColorManagement.h"

#include <DirectXTex.h>
#include <cctype>
#include <shared_mutex>

#include "Globals.h"
#include "LinearLighting.h"
#include "ShaderCache.h"
#include "State.h"

namespace TextureColorManagement
{
	namespace
	{
		constexpr float GAME_GAMMA = 1.6f;
		constexpr std::size_t MAX_CACHE_ENTRIES = 512;
		constexpr std::uint32_t DERIVED_CACHE_VERSION = 1;
		constexpr GUID SOURCE_PATH_GUID{ 0xb8a9476a, 0xcab7, 0x4362, { 0x92, 0x4a, 0xda, 0x46, 0xa1, 0x37, 0xf0, 0x7c } };

		struct GammaToLinearLUT
		{
			static constexpr std::size_t Size = 256;
			std::array<float, Size> values{};

			GammaToLinearLUT()
			{
				for (std::size_t i = 0; i < Size; ++i) {
					const float encoded = static_cast<float>(i) / static_cast<float>(Size - 1);
					values[i] = std::pow(encoded, GAME_GAMMA);
				}
			}
		};

		enum class ViewEncoding : std::uint8_t
		{
			Raw,
			SRGB,
			GameGamma
		};

		struct CacheKey
		{
			ID3D11ShaderResourceView* sourceView = nullptr;
			ViewEncoding encoding = ViewEncoding::Raw;
			AlphaMode alphaMode = AlphaMode::Data;

			bool operator==(const CacheKey&) const = default;
		};

		struct CacheKeyHash
		{
			std::size_t operator()(const CacheKey& key) const noexcept
			{
				return std::hash<ID3D11ShaderResourceView*>{}(key.sourceView) ^
				       (static_cast<std::size_t>(key.encoding) << 1) ^
				       (static_cast<std::size_t>(key.alphaMode) << 3);
			}
		};

		struct CachedView
		{
			winrt::com_ptr<ID3D11ShaderResourceView> sourceView;
			winrt::com_ptr<ID3D11Resource> source;
			winrt::com_ptr<ID3D11Resource> resource;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			std::uint64_t lastUse = 0;
		};

		struct BindingContract
		{
			ID3D11ShaderResourceView* sourceView = nullptr;
			TextureContract contract = TextureContract::Data();
		};

		std::unordered_map<CacheKey, CachedView, CacheKeyHash> viewCache;
		std::array<std::optional<BindingContract>, 16> bindingContracts;
		std::array<bool, 16> reboundSlots{};
		std::unordered_map<std::string, ColorManagement::Encoding> assetOverrides;
		std::shared_mutex assetOverrideMutex;
		winrt::com_ptr<ID3D11Device> cacheDevice;
		std::uint64_t useSerial = 0;

		float DecodeGameGamma(float value)
		{
			static const GammaToLinearLUT lut;
			value = std::max(value, 0.0f);
			if (value > 1.0f)
				return std::pow(value, GAME_GAMMA);

			const float position = value * static_cast<float>(GammaToLinearLUT::Size - 1);
			const auto lower = static_cast<std::size_t>(position);
			const auto upper = std::min(lower + 1, GammaToLinearLUT::Size - 1);
			return std::lerp(lut.values[lower], lut.values[upper], position - static_cast<float>(lower));
		}

		std::string NormalizePath(std::string_view path)
		{
			std::string result(path);
			std::ranges::transform(result, result.begin(), [](unsigned char value) {
				return value == '/' ? '\\' : static_cast<char>(std::tolower(value));
			});
			return result;
		}

		DXGI_FORMAT MakeRaw(DXGI_FORMAT format)
		{
			switch (format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_TYPELESS:
				return DXGI_FORMAT_B8G8R8X8_UNORM;
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC1_TYPELESS:
				return DXGI_FORMAT_BC1_UNORM;
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC2_TYPELESS:
				return DXGI_FORMAT_BC2_UNORM;
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC3_TYPELESS:
				return DXGI_FORMAT_BC3_UNORM;
			case DXGI_FORMAT_BC7_UNORM_SRGB:
			case DXGI_FORMAT_BC7_TYPELESS:
				return DXGI_FORMAT_BC7_UNORM;
			default:
				return format;
			}
		}

		DXGI_FORMAT MakeSRGB(DXGI_FORMAT format)
		{
			switch (MakeRaw(format)) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
				return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
				return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			case DXGI_FORMAT_B8G8R8X8_UNORM:
				return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
			case DXGI_FORMAT_BC1_UNORM:
				return DXGI_FORMAT_BC1_UNORM_SRGB;
			case DXGI_FORMAT_BC2_UNORM:
				return DXGI_FORMAT_BC2_UNORM_SRGB;
			case DXGI_FORMAT_BC3_UNORM:
				return DXGI_FORMAT_BC3_UNORM_SRGB;
			case DXGI_FORMAT_BC7_UNORM:
				return DXGI_FORMAT_BC7_UNORM_SRGB;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		bool IsTypeless(DXGI_FORMAT format)
		{
			switch (format) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8X8_TYPELESS:
			case DXGI_FORMAT_BC1_TYPELESS:
			case DXGI_FORMAT_BC2_TYPELESS:
			case DXGI_FORMAT_BC3_TYPELESS:
			case DXGI_FORMAT_BC7_TYPELESS:
				return true;
			default:
				return false;
			}
		}

		bool IsSRGB(DXGI_FORMAT format)
		{
			return MakeSRGB(format) == format;
		}

		void ResetCacheForDevice()
		{
			if (cacheDevice.get() == globals::d3d::device)
				return;

			viewCache.clear();
			reboundSlots.fill(false);
			cacheDevice.copy_from(globals::d3d::device);
			useSerial = 0;
		}

		void TrimCache()
		{
			while (viewCache.size() > MAX_CACHE_ENTRIES) {
				auto oldest = std::ranges::min_element(viewCache, {}, [](const auto& pair) { return pair.second.lastUse; });
				if (oldest == viewCache.end())
					break;
				viewCache.erase(oldest);
			}
		}

		std::optional<std::string> GetSourcePath(ID3D11Resource* resource)
		{
			UINT size = 0;
			const HRESULT query = resource->GetPrivateData(SOURCE_PATH_GUID, &size, nullptr);
			if ((FAILED(query) && query != DXGI_ERROR_MORE_DATA) || size == 0)
				return std::nullopt;

			std::string path(size, '\0');
			if (FAILED(resource->GetPrivateData(SOURCE_PATH_GUID, &size, path.data())))
				return std::nullopt;
			path.resize(std::char_traits<char>::length(path.c_str()));
			return path;
		}

		ColorManagement::Encoding ResolveEncoding(ID3D11ShaderResourceView* sourceView, const TextureContract& contract)
		{
			switch (contract.GetEncodingRule()) {
			case EncodingRule::SRGB:
				return ColorManagement::Encoding::SRGB;
			case EncodingRule::Linear:
				return ColorManagement::Encoding::Linear;
			case EncodingRule::GameGamma:
				return ColorManagement::Encoding::GameGamma;
			case EncodingRule::Infer:
				break;
			}

			winrt::com_ptr<ID3D11Resource> resource;
			sourceView->GetResource(resource.put());
			if (!resource)
				return globals::features::linearLighting.GetTextureInputEncoding();

			if (const auto path = GetSourcePath(resource.get())) {
				std::shared_lock lock(assetOverrideMutex);
				if (const auto it = assetOverrides.find(*path); it != assetOverrides.end())
					return it->second;
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			sourceView->GetDesc(&desc);
			if (IsSRGB(desc.Format))
				return ColorManagement::Encoding::SRGB;

			return globals::features::linearLighting.GetTextureInputEncoding();
		}

		std::optional<TextureContract> DescribeColorBinding(RE::BSShader::Type shaderType, std::uint32_t descriptor, std::uint32_t slot)
		{
			const auto inferredColor = [](AlphaMode alphaMode = AlphaMode::Data) {
				return TextureContract::Color(EncodingRule::Infer, alphaMode);
			};
			switch (shaderType) {
			case RE::BSShader::Type::Lighting:
				{
					using enum SIE::ShaderCache::LightingShaderTechniques;
					const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((descriptor >> 24) & 0x3F);
					const auto flags = descriptor & 0xFFFFFF;
					const bool isPBR = (flags & static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr)) != 0;
					const bool alphaTest = (flags & static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::DoAlphaTest)) != 0;
					const auto color = [encoding = isPBR ? EncodingRule::SRGB : EncodingRule::Infer](AlphaMode alphaMode = AlphaMode::Data) {
						return TextureContract::Color(encoding, alphaMode);
					};
					if ((technique == MTLand || technique == MTLandLODBlend) && slot <= 5)
						return color(alphaTest ? AlphaMode::Coverage : AlphaMode::Opacity);
					if (!isPBR && technique == MTLandLODBlend && (slot == 13 || slot == 15))
						return inferredColor(AlphaMode::Opacity);
					if (slot == 0)
						return color(alphaTest ? AlphaMode::Coverage : AlphaMode::Opacity);
					if (slot == 3 && (flags & static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)))
						return color();
					if (slot == 6 && technique == Glowmap)
						return inferredColor();
					break;
				}
			case RE::BSShader::Type::Effect:
				if (slot == 0 && !(descriptor & static_cast<std::uint32_t>(SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor)))
					return inferredColor(descriptor & static_cast<std::uint32_t>(SIE::ShaderCache::EffectShaderFlags::AlphaTest) ? AlphaMode::Coverage : AlphaMode::Opacity);
				if (slot == 4 && (descriptor & static_cast<std::uint32_t>(SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor)))
					return inferredColor();
				break;
			case RE::BSShader::Type::DistantTree:
				if (slot == 0)
					return inferredColor(descriptor & static_cast<std::uint32_t>(SIE::ShaderCache::DistantTreeShaderFlags::AlphaTest) ? AlphaMode::Coverage : AlphaMode::Opacity);
				break;
			case RE::BSShader::Type::Particle:
				if (slot == 1 && (descriptor == static_cast<std::uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::ParticlesGryColor) ||
									 descriptor == static_cast<std::uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::ParticlesGryColorAlpha)))
					return inferredColor();
				if (slot == 0 && descriptor != static_cast<std::uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::ParticlesGryColor) &&
					descriptor != static_cast<std::uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::ParticlesGryColorAlpha))
					return inferredColor(AlphaMode::Opacity);
				break;
			case RE::BSShader::Type::Sky:
				if (slot <= 1)
					return inferredColor(AlphaMode::Opacity);
				break;
			default:
				break;
			}
			return std::nullopt;
		}

		std::uint64_t HashCapturedImage(const DirectX::ScratchImage& image, DXGI_FORMAT targetFormat, AlphaMode alphaMode)
		{
			std::uint64_t hash = 1469598103934665603ull;
			auto append = [&](const void* data, std::size_t size) {
				const auto* bytes = static_cast<const std::uint8_t*>(data);
				for (std::size_t i = 0; i < size; ++i) {
					hash ^= bytes[i];
					hash *= 1099511628211ull;
				}
			};

			const auto& metadata = image.GetMetadata();
			append(&DERIVED_CACHE_VERSION, sizeof(DERIVED_CACHE_VERSION));
			append(&targetFormat, sizeof(targetFormat));
			append(&alphaMode, sizeof(alphaMode));
			append(&metadata.width, sizeof(metadata.width));
			append(&metadata.height, sizeof(metadata.height));
			append(&metadata.depth, sizeof(metadata.depth));
			append(&metadata.arraySize, sizeof(metadata.arraySize));
			append(&metadata.mipLevels, sizeof(metadata.mipLevels));
			append(&metadata.dimension, sizeof(metadata.dimension));
			for (std::size_t i = 0; i < image.GetImageCount(); ++i) {
				const auto& item = image.GetImages()[i];
				append(item.pixels, item.slicePitch);
			}
			return hash;
		}

		std::filesystem::path GetDerivedCachePath(std::uint64_t hash)
		{
			return std::filesystem::path("Data/ShaderCache/TextureColorManagement") / std::format("{:016X}.dds", hash);
		}

		HRESULT LoadDerivedImage(const std::filesystem::path& path, DirectX::ScratchImage& image)
		{
			if (!std::filesystem::exists(path))
				return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
			return DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
		}

		HRESULT SaveDerivedImage(const std::filesystem::path& path, const DirectX::ScratchImage& image)
		{
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			if (error)
				return HRESULT_FROM_WIN32(error.value());
			return DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS_NONE, path.c_str());
		}

		HRESULT TranscodeGameGamma(const DirectX::ScratchImage& captured, DXGI_FORMAT targetFormat, AlphaMode alphaMode, DirectX::ScratchImage& output)
		{
			std::vector<DirectX::Image> rawImages(captured.GetImages(), captured.GetImages() + captured.GetImageCount());
			DirectX::TexMetadata rawMetadata = captured.GetMetadata();
			rawMetadata.format = MakeRaw(rawMetadata.format);
			for (auto& image : rawImages)
				image.format = rawMetadata.format;

			DirectX::ScratchImage rgba;
			HRESULT result = DirectX::IsCompressed(rawMetadata.format) ?
			                     DirectX::Decompress(rawImages.data(), rawImages.size(), rawMetadata, DXGI_FORMAT_R32G32B32A32_FLOAT, rgba) :
			                     DirectX::Convert(rawImages.data(), rawImages.size(), rawMetadata, DXGI_FORMAT_R32G32B32A32_FLOAT, DirectX::TEX_FILTER_DEFAULT, 0.0f, rgba);
			if (FAILED(result))
				return result;

			DirectX::ScratchImage linear;
			result = DirectX::TransformImage(
				rgba.GetImages(), rgba.GetImageCount(), rgba.GetMetadata(),
				[](DirectX::XMVECTOR* outputPixels, const DirectX::XMVECTOR* inputPixels, std::size_t width, std::size_t) {
					for (std::size_t x = 0; x < width; ++x) {
						DirectX::XMFLOAT4 value;
						DirectX::XMStoreFloat4(&value, inputPixels[x]);
						value.x = DecodeGameGamma(value.x);
						value.y = DecodeGameGamma(value.y);
						value.z = DecodeGameGamma(value.z);
						outputPixels[x] = DirectX::XMLoadFloat4(&value);
					}
				},
				linear);
			if (FAILED(result))
				return result;

			DirectX::ScratchImage mipChain;
			const std::size_t mipLevels = captured.GetMetadata().mipLevels;
			result = DirectX::GenerateMipMaps(linear.GetImages(), linear.GetImageCount(), linear.GetMetadata(), DirectX::TEX_FILTER_DEFAULT, mipLevels, mipChain);
			if (FAILED(result))
				mipChain = std::move(linear);
			else if (alphaMode == AlphaMode::Coverage) {
				for (std::size_t item = 0; item < mipChain.GetMetadata().arraySize; ++item) {
					result = DirectX::ScaleMipMapsAlphaForCoverage(
						linear.GetImages(), linear.GetImageCount(), linear.GetMetadata(), item, 0.5f, mipChain);
					if (FAILED(result))
						return result;
				}
			}

			DirectX::ScratchImage encoded;
			result = DirectX::TransformImage(
				mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(),
				[](DirectX::XMVECTOR* outputPixels, const DirectX::XMVECTOR* inputPixels, std::size_t width, std::size_t) {
					auto encode = [](float value) {
						value = std::max(value, 0.0f);
						return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
					};
					for (std::size_t x = 0; x < width; ++x) {
						DirectX::XMFLOAT4 value;
						DirectX::XMStoreFloat4(&value, inputPixels[x]);
						value.x = encode(value.x);
						value.y = encode(value.y);
						value.z = encode(value.z);
						outputPixels[x] = DirectX::XMLoadFloat4(&value);
					}
				},
				encoded);
			if (FAILED(result))
				return result;

			return DirectX::IsCompressed(targetFormat) ?
			           DirectX::Compress(encoded.GetImages(), encoded.GetImageCount(), encoded.GetMetadata(), targetFormat, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, output) :
			           DirectX::Convert(encoded.GetImages(), encoded.GetImageCount(), encoded.GetMetadata(), targetFormat, DirectX::TEX_FILTER_DEFAULT, 0.0f, output);
		}

		winrt::com_ptr<ID3D11ShaderResourceView> CreateGameGammaView(ID3D11Resource* source, D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc, AlphaMode alphaMode)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (FAILED(source->QueryInterface(IID_PPV_ARGS(sourceTexture.put()))) || !sourceTexture)
				return nullptr;

			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			const DXGI_FORMAT targetFormat = MakeRaw(sourceViewDesc.Format);
			if (MakeSRGB(targetFormat) == DXGI_FORMAT_UNKNOWN)
				return nullptr;

			DirectX::ScratchImage captured;
			if (FAILED(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, source, captured)))
				return nullptr;

			const auto cachePath = GetDerivedCachePath(HashCapturedImage(captured, targetFormat, alphaMode));
			DirectX::ScratchImage derived;
			if (FAILED(LoadDerivedImage(cachePath, derived))) {
				if (FAILED(TranscodeGameGamma(captured, targetFormat, alphaMode, derived)))
					return nullptr;
				if (FAILED(SaveDerivedImage(cachePath, derived)))
					logger::warn("Texture color cache could not write {}", cachePath.string());
			}

			winrt::com_ptr<ID3D11Resource> derivedResource;
			if (FAILED(DirectX::CreateTextureEx(
					globals::d3d::device,
					derived.GetImages(), derived.GetImageCount(), derived.GetMetadata(),
					D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, sourceDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE,
					DirectX::CREATETEX_FORCE_SRGB, derivedResource.put())))
				return nullptr;

			sourceViewDesc.Format = MakeSRGB(targetFormat);
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (FAILED(globals::d3d::device->CreateShaderResourceView(derivedResource.get(), &sourceViewDesc, view.put())))
				return nullptr;
			return view;
		}

		winrt::com_ptr<ID3D11ShaderResourceView> CreateCompatibleView(ID3D11Resource* source, D3D11_SHADER_RESOURCE_VIEW_DESC sourceViewDesc, ViewEncoding encoding, winrt::com_ptr<ID3D11Resource>& ownedResource)
		{
			const DXGI_FORMAT desiredFormat = encoding == ViewEncoding::SRGB ? MakeSRGB(sourceViewDesc.Format) : MakeRaw(sourceViewDesc.Format);
			if (desiredFormat == DXGI_FORMAT_UNKNOWN)
				return nullptr;

			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (FAILED(source->QueryInterface(IID_PPV_ARGS(sourceTexture.put()))) || !sourceTexture)
				return nullptr;

			D3D11_TEXTURE2D_DESC desc{};
			sourceTexture->GetDesc(&desc);
			sourceViewDesc.Format = desiredFormat;

			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (IsTypeless(desc.Format)) {
				if (SUCCEEDED(globals::d3d::device->CreateShaderResourceView(source, &sourceViewDesc, view.put())))
					return view;
				return nullptr;
			}

			desc.Format = desiredFormat;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
			winrt::com_ptr<ID3D11Texture2D> clone;
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, clone.put())))
				return nullptr;
			globals::d3d::context->CopyResource(clone.get(), source);
			if (FAILED(globals::d3d::device->CreateShaderResourceView(clone.get(), &sourceViewDesc, view.put())))
				return nullptr;
			ownedResource = clone.as<ID3D11Resource>();
			return view;
		}

		struct BSShaderManager_GetTexture
		{
			static void thunk(const char* path, bool demand, RE::NiPointer<RE::NiTexture>& textureOut, bool isHeightMap)
			{
				func(path, demand, textureOut, isHeightMap);
				if (!path || !textureOut)
					return;

				auto* sourceTexture = skyrim_cast<RE::NiSourceTexture*>(textureOut.get());
				if (!sourceTexture || !sourceTexture->rendererTexture || !sourceTexture->rendererTexture->texture)
					return;

				const std::string normalized = NormalizePath(path);
				sourceTexture->rendererTexture->texture->SetPrivateData(SOURCE_PATH_GUID, static_cast<UINT>(normalized.size() + 1), normalized.c_str());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		stl::detour_thunk<BSShaderManager_GetTexture>(REL::RelocationID(98986, 105640));
	}

	ID3D11ShaderResourceView* ResolveView(ID3D11ShaderResourceView* sourceView, TextureContract contract)
	{
		if (!sourceView || !contract.IsColor() || !globals::d3d::device || !globals::d3d::context)
			return sourceView;
		if (!globals::features::linearLighting.IsColorManagementEnabled())
			return sourceView;

		ResetCacheForDevice();
		ViewEncoding target = ViewEncoding::Raw;
		switch (ResolveEncoding(sourceView, contract)) {
		case ColorManagement::Encoding::SRGB:
			target = ViewEncoding::SRGB;
			break;
		case ColorManagement::Encoding::GameGamma:
			target = ViewEncoding::GameGamma;
			break;
		case ColorManagement::Encoding::Linear:
			target = ViewEncoding::Raw;
			break;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		sourceView->GetDesc(&viewDesc);
		if ((target == ViewEncoding::SRGB && IsSRGB(viewDesc.Format)) ||
			(target == ViewEncoding::Raw && MakeRaw(viewDesc.Format) == viewDesc.Format))
			return sourceView;

		winrt::com_ptr<ID3D11Resource> source;
		sourceView->GetResource(source.put());
		if (!source)
			return sourceView;

		const CacheKey key{ sourceView, target, target == ViewEncoding::GameGamma ? contract.GetAlphaMode() : AlphaMode::Data };
		if (auto found = viewCache.find(key); found != viewCache.end()) {
			found->second.lastUse = ++useSerial;
			return found->second.view.get();
		}

		CachedView cached;
		cached.sourceView.copy_from(sourceView);
		cached.source = source;
		cached.lastUse = ++useSerial;
		cached.view = target == ViewEncoding::GameGamma ?
		                  CreateGameGammaView(source.get(), viewDesc, contract.GetAlphaMode()) :
		                  CreateCompatibleView(source.get(), viewDesc, target, cached.resource);
		if (!cached.view)
			return sourceView;

		auto* result = cached.view.get();
		viewCache.emplace(key, std::move(cached));
		TrimCache();
		return result;
	}

	void SetBindingContract(std::uint32_t slot, ID3D11ShaderResourceView* sourceView, TextureContract contract)
	{
		if (slot < bindingContracts.size())
			bindingContracts[slot] = BindingContract{ sourceView, contract };
	}

	void ResetBindingContracts()
	{
		bindingContracts.fill(std::nullopt);
	}

	void ApplyBindings(ID3D11DeviceContext* context)
	{
		if (!context || !globals::state->currentShader)
			return;

		auto& runtime = globals::game::shadowState->GetRuntimeData();
		if (!globals::features::linearLighting.IsColorManagementEnabled()) {
			for (std::uint32_t slot = 0; slot < std::size(runtime.PSTexture); ++slot) {
				if (!reboundSlots[slot])
					continue;
				auto* sourceView = runtime.PSTexture[slot];
				context->PSSetShaderResources(slot, 1, &sourceView);
				reboundSlots[slot] = false;
			}
			return;
		}

		const auto shaderType = globals::state->currentShader->shaderType.get();
		const auto descriptor = globals::state->currentPixelDescriptor;
		const bool hasExplicitContracts = shaderType == RE::BSShader::Type::Lighting &&
		                                  (descriptor & static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr));
		for (std::uint32_t slot = 0; slot < std::size(runtime.PSTexture); ++slot) {
			auto* sourceView = runtime.PSTexture[slot];
			std::optional<TextureContract> contract;
			if (hasExplicitContracts && bindingContracts[slot] && bindingContracts[slot]->sourceView == sourceView)
				contract = bindingContracts[slot]->contract;
			else
				contract = DescribeColorBinding(shaderType, descriptor, slot);
			if (!contract) {
				if (reboundSlots[slot])
					context->PSSetShaderResources(slot, 1, &sourceView);
				reboundSlots[slot] = false;
				continue;
			}

			auto* resolved = ResolveView(sourceView, *contract);
			if (resolved != sourceView || reboundSlots[slot])
				context->PSSetShaderResources(slot, 1, &resolved);
			reboundSlots[slot] = resolved != sourceView;
		}
	}

	void SetAssetOverride(std::string_view path, ColorManagement::Encoding sourceEncoding)
	{
		std::unique_lock lock(assetOverrideMutex);
		assetOverrides.insert_or_assign(NormalizePath(path), sourceEncoding);
	}

	void ClearAssetOverride(std::string_view path)
	{
		std::unique_lock lock(assetOverrideMutex);
		assetOverrides.erase(NormalizePath(path));
	}
}
