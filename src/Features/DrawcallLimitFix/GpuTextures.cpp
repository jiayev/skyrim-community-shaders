#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "GpuTextures.h"

#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/IDescriptorService.h>
#	include <rhi_helpers.h>

namespace DCLF
{
	namespace
	{
		// The renderer's sampler states, [address mode][filter mode], selected by the shadow state's
		// PSTextureAddressMode / PSTextureFilterMode (engine notes: samplers). AE 1.6.1170 only for now.
		constexpr std::uint32_t kAddressModes = 4;
		constexpr std::uint32_t kFilterModes = 5;
		constexpr std::uintptr_t kSamplerTableAE = 0x3288210;
		constexpr std::uint32_t kGraveyardFrames = 16;  // an evicted import outlives every epoch that may still read it

		// VkComponentMapping as D3D12's shader 4-component mapping (0: identity).
		rhi::ComponentMapping MappingOf(const VkComponentMapping& a_components)
		{
			auto source = [](VkComponentSwizzle a_swizzle, std::uint32_t a_identity) -> std::uint32_t {
				switch (a_swizzle) {
				case VK_COMPONENT_SWIZZLE_IDENTITY:
					return a_identity;
				case VK_COMPONENT_SWIZZLE_R:
					return 0;
				case VK_COMPONENT_SWIZZLE_G:
					return 1;
				case VK_COMPONENT_SWIZZLE_B:
					return 2;
				case VK_COMPONENT_SWIZZLE_A:
					return 3;
				case VK_COMPONENT_SWIZZLE_ZERO:
					return 4;
				default:
					return 5;
				}
			};
			const std::uint32_t r = source(a_components.r, 0), g = source(a_components.g, 1), b = source(a_components.b, 2), a = source(a_components.a, 3);
			if (r == 0 && g == 1 && b == 2 && a == 3)
				return 0;
			return r | (g << 3) | (b << 6) | (a << 9) | (1u << 12);
		}

		rhi::AddressMode AddressOf(D3D11_TEXTURE_ADDRESS_MODE a_mode)
		{
			switch (a_mode) {
			case D3D11_TEXTURE_ADDRESS_WRAP:
				return rhi::AddressMode::Wrap;
			case D3D11_TEXTURE_ADDRESS_MIRROR:
				return rhi::AddressMode::Mirror;
			case D3D11_TEXTURE_ADDRESS_BORDER:
				return rhi::AddressMode::Border;
			case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE:
				return rhi::AddressMode::MirrorOnce;
			default:
				return rhi::AddressMode::Clamp;
			}
		}

		rhi::SamplerDesc SamplerOf(const D3D11_SAMPLER_DESC& a_desc)
		{
			rhi::SamplerDesc desc{};
			const auto filter = static_cast<std::uint32_t>(a_desc.Filter);
			const bool anisotropic = (filter & 0x40) != 0;
			desc.mipFilter = (filter & 0x1) || anisotropic ? rhi::MipFilter::Linear : rhi::MipFilter::Nearest;
			desc.magFilter = (filter & 0x4) || anisotropic ? rhi::Filter::Linear : rhi::Filter::Nearest;
			desc.minFilter = (filter & 0x10) || anisotropic ? rhi::Filter::Linear : rhi::Filter::Nearest;
			desc.maxAnisotropy = anisotropic ? std::max(1u, a_desc.MaxAnisotropy) : 1u;
			switch (filter & 0x180) {
			case 0x80:
				desc.compareEnable = true;
				desc.reduction = rhi::ReductionMode::Comparison;
				desc.compareOp = static_cast<rhi::CompareOp>(std::clamp<int>(a_desc.ComparisonFunc, 1, 8) - 1);
				break;
			case 0x100:
				desc.reduction = rhi::ReductionMode::Min;
				break;
			case 0x180:
				desc.reduction = rhi::ReductionMode::Max;
				break;
			default:
				break;
			}
			desc.addressU = AddressOf(a_desc.AddressU);
			desc.addressV = AddressOf(a_desc.AddressV);
			desc.addressW = AddressOf(a_desc.AddressW);
			desc.mipLodBias = a_desc.MipLODBias;
			desc.minLod = a_desc.MinLOD;
			desc.maxLod = a_desc.MaxLOD;
			desc.borderPreset = rhi::BorderPreset::Custom;
			std::copy(std::begin(a_desc.BorderColor), std::end(a_desc.BorderColor), desc.borderColor);
			return desc;
		}

		bool DescribeView(const D3D11_SHADER_RESOURCE_VIEW_DESC& a_view, rhi::SrvDesc& a_out)
		{
			a_out.formatOverride = rhi::helpers::ToRHI(a_view.Format);
			switch (a_view.ViewDimension) {
			case D3D11_SRV_DIMENSION_TEXTURE2D:
				a_out.dimension = rhi::SrvDim::Texture2D;
				a_out.tex2D.mostDetailedMip = a_view.Texture2D.MostDetailedMip;
				a_out.tex2D.mipLevels = a_view.Texture2D.MipLevels;
				break;
			case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
				a_out.dimension = rhi::SrvDim::Texture2DArray;
				a_out.tex2DArray.mostDetailedMip = a_view.Texture2DArray.MostDetailedMip;
				a_out.tex2DArray.mipLevels = a_view.Texture2DArray.MipLevels;
				a_out.tex2DArray.firstArraySlice = a_view.Texture2DArray.FirstArraySlice;
				a_out.tex2DArray.arraySize = a_view.Texture2DArray.ArraySize;
				break;
			case D3D11_SRV_DIMENSION_TEXTURECUBE:
				a_out.dimension = rhi::SrvDim::TextureCube;
				a_out.cube.mostDetailedMip = a_view.TextureCube.MostDetailedMip;
				a_out.cube.mipLevels = a_view.TextureCube.MipLevels;
				break;
			case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
				a_out.dimension = rhi::SrvDim::TextureCubeArray;
				a_out.cubeArray.mostDetailedMip = a_view.TextureCubeArray.MostDetailedMip;
				a_out.cubeArray.mipLevels = a_view.TextureCubeArray.MipLevels;
				a_out.cubeArray.first2DArrayFace = a_view.TextureCubeArray.First2DArrayFace;
				a_out.cubeArray.numCubes = a_view.TextureCubeArray.NumCubes;
				break;
			case D3D11_SRV_DIMENSION_TEXTURE3D:
				a_out.dimension = rhi::SrvDim::Texture3D;
				a_out.tex3D.mostDetailedMip = a_view.Texture3D.MostDetailedMip;
				a_out.tex3D.mipLevels = a_view.Texture3D.MipLevels;
				break;
			default:
				return false;  // buffers, 1D and multisampled views: not used by the eligible Lighting draws
			}
			return a_out.formatOverride != rhi::Format::Unknown;
		}
	}

	struct GpuTextures::Impl
	{
		struct Entry
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			rhi::ResourcePtr image;
			rhi::DescriptorSlot slot{};
			std::uint32_t index = kInvalid;
			std::uint32_t lastUsed = 0;
			Reject reject = Reject::None;
		};

		struct Retired
		{
			rhi::ResourcePtr image;
			std::uint32_t frame = 0;
		};

		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, Entry> entries;
		std::vector<Retired> graveyard;
		std::array<std::uint32_t, kAddressModes * kFilterModes> samplers{};
		std::array<bool, kAddressModes * kFilterModes> samplerFailed{};
		rhi::DescriptorSlot nullSlot{};
		std::uint32_t nullIndex = kInvalid;
		std::uint32_t frame = 0;
	};

	GpuTextures::GpuTextures() :
		impl(std::make_unique<Impl>())
	{
		impl->samplers.fill(kInvalid);
		impl->samplerFailed.fill(false);
	}

	GpuTextures::~GpuTextures() = default;

	GpuTextures& GpuTextures::Get()
	{
		static GpuTextures textures;
		return textures;
	}

	std::uint32_t GpuTextures::Resolve(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return NullIndex();
		auto [it, inserted] = impl->entries.try_emplace(a_view);
		auto& entry = it->second;
		entry.lastUsed = impl->frame;
		if (!inserted)
			return entry.index;

		entry.view.copy_from(a_view);
		auto reject = [&](Reject a_reason) {
			entry.reject = a_reason;
			++stats.rejected[static_cast<std::size_t>(a_reason)];
			return kInvalid;
		};
		auto* service = org::runtime::GetActiveDescriptorService();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!service || !host)
			return reject(Reject::Import);

		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		a_view->GetDesc(&viewDesc);
		rhi::SrvDesc srv{};
		if (!DescribeView(viewDesc, srv)) {
			if (stats.rejected[static_cast<std::size_t>(Reject::View)] < 8)
				logger::info("[DCLF] Game texture view not supported: format {}, dimension {}", static_cast<int>(viewDesc.Format), static_cast<int>(viewDesc.ViewDimension));
			return reject(Reject::View);
		}

		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_view, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return reject(Reject::NotImage);
		const auto& image = info.image;
		// DXVK keeps images it hands out (marked stable) in GENERAL and uses them between the graph's
		// commands: imported with simultaneous access, they are read in GENERAL and never transitioned.
		if (image.layout != VK_IMAGE_LAYOUT_GENERAL) {
			if (stats.unsupportedLayouts++ == 0)
				logger::warn("[DCLF] A game texture is in Vulkan layout {}, not GENERAL; such textures stay native", static_cast<int>(image.layout));
			return reject(Reject::Import);
		}

		rhi::vulkan::ImportedImageDesc import{};
		import.image = image.image;
		import.createInfo.flags = image.flags;
		import.createInfo.imageType = image.type;
		import.createInfo.format = image.format;
		import.createInfo.extent = image.extent;
		import.createInfo.mipLevels = image.mipLevels;
		import.createInfo.arrayLayers = image.arrayLayers;
		import.createInfo.samples = image.samples;
		import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		import.createInfo.usage = image.usage;
		import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		import.currentLayout = image.layout;
		import.simultaneousAccess = true;
		import.debugName = "DCLF game texture";
		auto device = host->GetDesc().device;
		if (rhi::vulkan::import_image(device, import, entry.image) != rhi::Result::Ok || !entry.image)
			return reject(Reject::Import);

		srv.componentMapping = MappingOf(image.components);
		entry.slot = service->AllocateDescriptorSlot(rhi::DescriptorHeapType::CbvSrvUav, true);
		if (device.CreateShaderResourceView(entry.slot, entry.image->GetHandle(), srv) != rhi::Result::Ok) {
			service->RetireDescriptorSlot(entry.slot);
			entry.slot = {};
			entry.image.Reset();
			return reject(Reject::View);
		}
		entry.index = entry.slot.index;
		++stats.cached;
		return entry.index;
	}

	bool GpuTextures::Known(ID3D11ShaderResourceView* a_view, std::uint32_t& a_index)
	{
		if (!a_view) {
			a_index = impl->nullIndex;
			return impl->nullIndex != kInvalid;
		}
		const auto it = impl->entries.find(a_view);
		if (it == impl->entries.end())
			return false;
		it->second.lastUsed = impl->frame;
		a_index = it->second.index;
		return true;
	}

	std::uint32_t GpuTextures::NullIndex()
	{
		if (impl->nullIndex != kInvalid)
			return impl->nullIndex;
		auto* service = org::runtime::GetActiveDescriptorService();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!service || !host)
			return kInvalid;
		rhi::SrvDesc srv{};
		srv.dimension = rhi::SrvDim::Texture2D;
		srv.formatOverride = rhi::Format::R8G8B8A8_UNorm;
		srv.tex2D.mipLevels = 1;
		impl->nullSlot = service->AllocateDescriptorSlot(rhi::DescriptorHeapType::CbvSrvUav, true);
		auto device = host->GetDesc().device;
		if (device.CreateShaderResourceView(impl->nullSlot, {}, srv) != rhi::Result::Ok) {
			logger::warn("[DCLF] Null texture descriptors are unsupported (VK_EXT_robustness2 nullDescriptor)");
			service->RetireDescriptorSlot(impl->nullSlot);
			impl->nullSlot = {};
			return kInvalid;
		}
		impl->nullIndex = impl->nullSlot.index;
		return impl->nullIndex;
	}

	std::uint32_t GpuTextures::Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
	{
		if (a_addressMode >= kAddressModes || a_filterMode >= kFilterModes || !REL::Module::IsAE())
			return kInvalid;
		auto& index = impl->samplers[a_addressMode * kFilterModes + a_filterMode];
		if (index != kInvalid)
			return index;
		auto* service = org::runtime::GetActiveDescriptorService();
		const auto* table = reinterpret_cast<ID3D11SamplerState* const*>(REL::Offset(kSamplerTableAE).address());
		auto* state = table[a_addressMode * kFilterModes + a_filterMode];
		if (!service || !state)
			return kInvalid;
		D3D11_SAMPLER_DESC desc{};
		state->GetDesc(&desc);
		// A sampler the backend cannot create is never handed out (the service throws): the draws that need it
		// are skipped rather than drawn through an empty descriptor. Not retried; the engine's table is fixed.
		const std::size_t key = a_addressMode * kFilterModes + a_filterMode;
		if (impl->samplerFailed[key])
			return kInvalid;
		try {
			index = service->CreateIndexedSampler(SamplerOf(desc));
		} catch (const std::exception& e) {
			impl->samplerFailed[key] = true;
			logger::error("[DCLF] Engine sampler (address mode {}, filter mode {}) could not be created; draws that sample with it are skipped: {}", a_addressMode, a_filterMode, e.what());
			return kInvalid;
		}
		++stats.samplers;
		return index;
	}

	void GpuTextures::BeginFrame(std::uint32_t a_frame)
	{
		impl->frame = a_frame;
		std::erase_if(impl->graveyard, [&](const Impl::Retired& a_retired) { return a_frame - a_retired.frame > kGraveyardFrames; });
		if ((a_frame % 64) != 0)
			return;
		auto* service = org::runtime::GetActiveDescriptorService();
		for (auto it = impl->entries.begin(); it != impl->entries.end();) {
			auto& entry = it->second;
			if (a_frame - entry.lastUsed <= kEvictFrames) {
				++it;
				continue;
			}
			++generation;
			if (entry.index != kInvalid) {
				--stats.cached;
				if (service)
					service->RetireDescriptorSlot(entry.slot);
				impl->graveyard.push_back({ std::move(entry.image), a_frame });
			} else {
				--stats.rejected[static_cast<std::size_t>(entry.reject)];
			}
			it = impl->entries.erase(it);
		}
	}

	void GpuTextures::Clear()
	{
		++generation;
		impl->entries.clear();
		impl->graveyard.clear();
		impl->samplers.fill(kInvalid);
		impl->samplerFailed.fill(false);
		impl->nullIndex = kInvalid;
		impl->nullSlot = {};
		stats = {};
	}
}

#else  // no render graph

#	include "GpuTextures.h"

namespace DCLF
{
	struct GpuTextures::Impl
	{};
	GpuTextures::GpuTextures() = default;
	GpuTextures::~GpuTextures() = default;
	GpuTextures& GpuTextures::Get()
	{
		static GpuTextures textures;
		return textures;
	}
	std::uint32_t GpuTextures::Resolve(ID3D11ShaderResourceView*) { return kInvalid; }
	bool GpuTextures::Known(ID3D11ShaderResourceView*, std::uint32_t&) { return false; }
	std::uint32_t GpuTextures::NullIndex() { return kInvalid; }
	std::uint32_t GpuTextures::Sampler(std::uint32_t, std::uint32_t) { return kInvalid; }
	void GpuTextures::BeginFrame(std::uint32_t) {}
	void GpuTextures::Clear() {}
}

#endif
