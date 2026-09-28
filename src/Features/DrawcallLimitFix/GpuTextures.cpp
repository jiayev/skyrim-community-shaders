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
#	include <Tracy/Tracy.hpp>

#	include <atomic>
#	include <cstdlib>
#	include <mutex>
#	include <format>

namespace DCLF
{
	namespace
	{
		// The renderer's sampler states, [address mode][filter mode], selected by the shadow state's
		// PSTextureAddressMode / PSTextureFilterMode (engine notes: samplers). AE 1.6.1170 only for now.
		constexpr std::uint32_t kAddressModes = 4;
		constexpr std::uint32_t kFilterModes = 5;
		constexpr std::uintptr_t kSamplerTableAE = 0x3288210;

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
		struct Entry;
		struct Registry
		{
			struct Slot
			{
				std::weak_ptr<Entry> live;
				std::shared_ptr<Entry> rejected;  // permanent rejection pins its source identity for this device generation
				std::uint64_t serial = 0;
			};
			std::mutex mutex;
			ankerl::unordered_dense::map<ID3D11ShaderResourceView*, Slot> entries;
			std::atomic<std::uint32_t> liveCount{ 0 };
			std::uint64_t nextSerial = 0;
		};
		struct ImportedBacking
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			rhi::ResourcePtr image;
		};
		struct Entry
		{
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			rhi::ResourcePtr image;
			org::OwnedDescriptorBinding binding;
			std::uint32_t index = kInvalid;
			Reject reject = Reject::None;
			std::weak_ptr<Registry> registry;
			ID3D11ShaderResourceView* key = nullptr;
			std::uint64_t serial = 0;
			~Entry()
			{
				if (auto state = registry.lock()) {
					if (binding)
						state->liveCount.fetch_sub(1, std::memory_order_relaxed);
					std::scoped_lock lock(state->mutex);
					if (const auto found = state->entries.find(key); found != state->entries.end() && found->second.serial == serial)
						state->entries.erase(found);
				}
			}
		};

		std::shared_ptr<Registry> registry;
		std::weak_ptr<org::runtime::ResourceCleanupQueue> cleanup;
		std::array<std::uint32_t, kAddressModes * kFilterModes> samplers{};
		std::array<org::OwnedDescriptorBinding, kAddressModes * kFilterModes> samplerBindings{};
		std::array<bool, kAddressModes * kFilterModes> samplerFailed{};
		org::OwnedDescriptorBinding nullBinding;
		std::uint32_t nullIndex = kInvalid;
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

	GpuTextures::Binding GpuTextures::ResolveBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag)
	{
		ZoneScopedN("CS.DCLF.Texture.ResolveBinding");
		if (!a_view)
			return NullBinding();
		// A descriptor import is legal only inside an epoch. Do not cache an Import
		// rejection merely because the caller reached us outside that boundary:
		// the same view may become executable at the next epoch.
		auto* service = org::runtime::GetActiveDescriptorService();
		auto* host = RenderGraphRuntime::Get().Host();
		auto registry = impl->registry;
		if (registry) {
			ZoneScopedN("CS.DCLF.Texture.RegistryLookup");
			std::scoped_lock lock(registry->mutex);
			if (const auto found = registry->entries.find(a_view); found != registry->entries.end()) {
				if (auto live = found->second.live.lock())
					return { live->index, live->index == kInvalid ? std::shared_ptr<const void>{} : live };
				if (found->second.rejected)
					return { kInvalid, {} };
			}
		}
		if (!service || !host)
			return { kInvalid, {} };
		const auto cleanup = service->GetResourceCleanupQueue();
		if (!cleanup)
			return { kInvalid, {} };
		{
			ZoneScopedN("CS.DCLF.Texture.Import");
			impl->cleanup = cleanup;
			if (!registry)
				impl->registry = registry = cleanup->Make<Impl::Registry>();
			auto entry = cleanup->Make<Impl::Entry>();
			entry->view.copy_from(a_view);
			entry->registry = registry;
			entry->key = a_view;
			{
				ZoneScopedN("CS.DCLF.Texture.Import.Register");
				std::scoped_lock lock(registry->mutex);
				entry->serial = ++registry->nextSerial;
				registry->entries[a_view] = { entry, {}, entry->serial };
			}
			auto reject = [&](Reject a_reason) {
				entry->reject = a_reason;
				{
					std::scoped_lock lock(registry->mutex);
					if (const auto found = registry->entries.find(a_view); found != registry->entries.end() && found->second.serial == entry->serial)
						found->second.rejected = entry;
				}
				++stats.rejected[static_cast<std::size_t>(a_reason)];
				return Binding{ kInvalid, {} };
			};

			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			a_view->GetDesc(&viewDesc);
			rhi::SrvDesc srv{};
			if (!DescribeView(viewDesc, srv)) {
				if (stats.rejected[static_cast<std::size_t>(Reject::View)] < 8)
					logger::info("[DCLF] Game texture view not supported: format {}, dimension {}", static_cast<int>(viewDesc.Format), static_cast<int>(viewDesc.ViewDimension));
				return reject(Reject::View);
			}

			DxvkOrgInteropResourceInfo info{};
			{
				ZoneScopedN("CS.DCLF.Texture.Import.DescribeResource");
				if (!RenderGraphRuntime::Get().DescribeResource(a_view, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
					return reject(Reject::NotImage);
			}
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
			// Aftermath resource tracking reports Vulkan object names at a fault. Distinguish
			// imports by the stable DXVK image and the game SRV that produced this view.
			const auto importName = std::format("DCLF texture image {:#x} SRV {:#x}",
				reinterpret_cast<std::uintptr_t>(image.image), reinterpret_cast<std::uintptr_t>(a_view));
			import.debugName = importName.c_str();
			auto device = host->GetDesc().device;
			{
				ZoneScopedN("CS.DCLF.Texture.Import.WrapImage");
				if (rhi::vulkan::import_image(device, import, entry->image) != rhi::Result::Ok || !entry->image)
					return reject(Reject::Import);
			}

			srv.componentMapping = MappingOf(image.components);
			auto deviceOwner = RenderGraphRuntime::Get().DeviceOwner();
			if (!deviceOwner)
				return reject(Reject::Import);
			auto backing = cleanup->Make<Impl::ImportedBacking>();
			backing->view.copy_from(a_view);
			backing->image = std::move(entry->image);
			{
				ZoneScopedN("CS.DCLF.Texture.Import.CreateView");
				entry->binding = service->CreateOwnedShaderResourceView(deviceOwner, backing->image.Get(), backing, srv);
			}
			if (!entry->binding) {
				return reject(Reject::View);
			}
			entry->index = entry->binding.Index();
			registry->liveCount.fetch_add(1, std::memory_order_relaxed);
			TracyPlot("CS.DCLF.Texture.ImportSource", static_cast<std::int64_t>(a_sourceTag));
			static const bool traceIdentities = std::getenv("CS_DCLF_TRACE_TEXTURE_PATHS") != nullptr;
			if (traceIdentities) {
				TracyPlot("CS.DCLF.Texture.ImportView", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(a_view)));
				TracyPlot("CS.DCLF.Texture.ImportImage", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(image.image)));
			}
			return { entry->index, entry };
		}
	}

	bool GpuTextures::KnownBinding(ID3D11ShaderResourceView* a_view, Binding& a_binding)
	{
		if (!a_view) {
			a_binding = { impl->nullIndex, impl->nullBinding.Owner() };
			return impl->nullIndex != kInvalid;
		}
		const auto registry = impl->registry;
		if (!registry)
			return false;
		std::scoped_lock lock(registry->mutex);
		const auto found = registry->entries.find(a_view);
		if (found == registry->entries.end())
			return false;
		if (auto live = found->second.live.lock()) {
			a_binding = { live->index, live->index == kInvalid ? std::shared_ptr<const void>{} : live };
			return true;
		}
		if (found->second.rejected) {
			a_binding = { kInvalid, {} };
			return true;
		}
		return false;
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
		impl->nullBinding = service->CreateOwnedShaderResourceView(RenderGraphRuntime::Get().DeviceOwner(), {}, {}, srv);
		if (!impl->nullBinding) {
			logger::warn("[DCLF] Null texture descriptors are unsupported (VK_EXT_robustness2 nullDescriptor)");
			return kInvalid;
		}
		impl->nullIndex = impl->nullBinding.Index();
		return impl->nullIndex;
	}

	GpuTextures::Binding GpuTextures::NullBinding()
	{
		return { NullIndex(), impl->nullBinding.Owner() };
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
			impl->samplerBindings[key] = service->CreateOwnedSampler(RenderGraphRuntime::Get().DeviceOwner(), SamplerOf(desc));
			if (!impl->samplerBindings[key])
				throw std::runtime_error("owned sampler creation failed");
			index = impl->samplerBindings[key].Index();
		} catch (const std::exception& e) {
			impl->samplerFailed[key] = true;
			logger::error("[DCLF] Engine sampler (address mode {}, filter mode {}) could not be created; draws that sample with it are skipped: {}", a_addressMode, a_filterMode, e.what());
			return kInvalid;
		}
		++stats.samplers;
		return index;
	}

	GpuTextures::Binding GpuTextures::SamplerBinding(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
	{
		const auto index = Sampler(a_addressMode, a_filterMode);
		return { index, index == kInvalid ? std::shared_ptr<const void>{} : impl->samplerBindings[a_addressMode * kFilterModes + a_filterMode].Owner() };
	}

	GpuTextures::Stats GpuTextures::GetStats() const
	{
		auto result = stats;
		if (const auto registry = impl->registry) {
			result.cached = registry->liveCount.load(std::memory_order_relaxed);
			std::scoped_lock lock(registry->mutex);
			result.registrySlots = static_cast<std::uint32_t>(registry->entries.size());
		}
		if (const auto cleanup = impl->cleanup.lock())
			result.cleanupPending = cleanup->Pending();
		return result;
	}

	void GpuTextures::Clear()
	{
		++generation;
		impl->registry.reset();
		impl->samplers.fill(kInvalid);
		impl->samplerBindings = {};
		impl->samplerFailed.fill(false);
		impl->nullIndex = kInvalid;
		impl->nullBinding = {};
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
	GpuTextures::Binding GpuTextures::ResolveBinding(ID3D11ShaderResourceView*, std::uint32_t) { return {}; }
	bool GpuTextures::KnownBinding(ID3D11ShaderResourceView*, Binding&) { return false; }
	std::uint32_t GpuTextures::NullIndex() { return kInvalid; }
	GpuTextures::Binding GpuTextures::NullBinding() { return {}; }
	std::uint32_t GpuTextures::Sampler(std::uint32_t, std::uint32_t) { return kInvalid; }
	GpuTextures::Binding GpuTextures::SamplerBinding(std::uint32_t, std::uint32_t) { return {}; }
	GpuTextures::Stats GpuTextures::GetStats() const { return stats; }
	void GpuTextures::Clear() {}
}

#endif
