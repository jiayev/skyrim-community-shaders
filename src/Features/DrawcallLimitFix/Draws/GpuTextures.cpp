#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "GpuTextures.h"
#	include "Features/DrawcallLimitFix/Common/Switches.h"

#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/BindingTable.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/IDescriptorService.h>
#	include <rhi_helpers.h>
#	include <Tracy/Tracy.hpp>

#	include <atomic>
#	include <chrono>
#	include <cstdlib>
#	include <mutex>
#	include <format>
#	include <thread>
#	include <vector>

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
		enum class State : std::uint8_t
		{
			Pending,
			Ready,
			Rejected
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
			// The fields above are the importing thread's until it stores the state (release); a lookup reads it first.
			std::atomic<State> state{ State::Pending };
			~Entry()
			{
				if (auto owner = registry.lock()) {
					if (binding)
						owner->liveCount.fetch_sub(1, std::memory_order_relaxed);
					std::scoped_lock lock(owner->mutex);
					if (const auto found = owner->entries.find(key); found != owner->entries.end() && found->second.serial == serial)
						owner->entries.erase(found);
				}
			}
		};
		// The counters the import thread writes as well as the render thread.
		struct Counters
		{
			std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(Reject::Count)> rejected{};
			std::atomic<std::uint32_t> unsupportedLayouts{ 0 };
			std::atomic<std::uint64_t> importedAsync{ 0 };
		};
		// Everything an import needs, owned, so the import thread can make it outside any epoch: the descriptor service is
		// retained from the graph (its heap allocation is locked), not the epoch's active pointer.
		struct ImportContext
		{
			std::shared_ptr<org::runtime::IDescriptorService> service;
			std::shared_ptr<org::runtime::ResourceCleanupQueue> cleanup;
			rhi::Device device;
			std::shared_ptr<const void> deviceOwner;
		};
		// The import thread's work: a Request to answer.
		struct Work
		{
			std::uint32_t sourceTag = ~0u;
			winrt::com_ptr<ID3D11ShaderResourceView> view;  // the request's reference on its view
			Replies* replies = nullptr;
			std::uint64_t cookie = 0;
		};
		// What a Request is answered with, published by the render thread (PublishImportContext): immutable.
		struct Published
		{
			ImportContext context;
			std::shared_ptr<Registry> registry;
			std::shared_ptr<Counters> counters;
			std::shared_ptr<const FixedBindings> fixed;  // never null
		};
		// The import thread's queue (lock-free, many producers, the thread its one consumer) and its wake. The thread shares it, so the
		// last owner only signals it and never joins.
		struct Importer
		{
			EventQueue<Work, 1024> queue;
			std::atomic<std::uint64_t> wake{ 0 };
			std::atomic<bool> stop{ false };
			std::atomic<std::shared_ptr<const Published>> published;
			std::atomic<std::uint32_t> requestsPending{ 0 };  // Request: not answered yet
			std::atomic<std::uint64_t> requestsAnswered{ 0 };

			void Notify()
			{
				wake.fetch_add(1, std::memory_order_release);
				wake.notify_one();
			}
		};
		enum class Found
		{
			Unknown,
			Ready,
			Pending,
			Rejected
		};
		std::shared_ptr<Registry> registry;
		std::shared_ptr<Counters> counters = std::make_shared<Counters>();
		std::shared_ptr<Importer> importer;
		std::weak_ptr<org::runtime::ResourceCleanupQueue> cleanup;
		std::array<std::uint32_t, kAddressModes * kFilterModes> samplers{};
		std::array<org::OwnedDescriptorBinding, kAddressModes * kFilterModes> samplerBindings{};
		std::array<bool, kAddressModes * kFilterModes> samplerFailed{};
		org::OwnedDescriptorBinding nullBinding;
		std::uint32_t nullIndex = kInvalid;

		~Impl()
		{
			if (importer) {
				importer->stop.store(true, std::memory_order_release);
				importer->Notify();
			}
		}

		// The descriptor service: the epoch's while one prepares, else the graph's own, retained (its heap allocation is locked), as
		// the import thread makes its imports outside any epoch. The lookups are the scene lane's (T6b2c step 5).
		static org::runtime::IDescriptorService* ServiceNow(std::shared_ptr<org::runtime::IDescriptorService>& a_retained)
		{
			if (auto* active = org::runtime::GetActiveDescriptorService())
				return active;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !host->Graph())
				return nullptr;
			a_retained = host->Graph()->RetainDescriptorService();
			return a_retained.get();
		}

		// What an import needs: the graph's descriptor service and cleanup queue (the descriptor heap manager's, the epoch's too).
		static bool MakeContext(ImportContext& a_context)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !host->Graph())
				return false;
			a_context.service = host->Graph()->RetainDescriptorService();
			auto* active = org::runtime::GetActiveDescriptorService();
			a_context.cleanup = active ? active->GetResourceCleanupQueue() : host->ResourceCleanup();
			a_context.device = host->GetDesc().device;
			a_context.deviceOwner = RenderGraphRuntime::Get().DeviceOwner();
			return a_context.service && a_context.cleanup && a_context.deviceOwner;
		}

		// The registry's answer for a view; under its lock. The registry holds entries weakly: an import in flight is owned by
		// the import thread, and an import goes when nobody holds it any more.
		static Found Find(Registry& a_registry, ID3D11ShaderResourceView* a_view, Binding& a_binding)
		{
			const auto found = a_registry.entries.find(a_view);
			if (found == a_registry.entries.end())
				return Found::Unknown;
			auto& slot = found->second;
			if (auto live = slot.live.lock()) {
				switch (live->state.load(std::memory_order_acquire)) {
				case State::Pending:
					a_binding.index = kInvalid;
					a_binding.owner = std::move(live);  // the asker holds the import until it settles
					a_binding.pending = true;
					return Found::Pending;
				case State::Rejected:
					slot.rejected = std::move(live);
					a_binding = { kInvalid, {} };
					return Found::Rejected;
				case State::Ready:
					a_binding.index = live->index;
					a_binding.owner = live->index == kInvalid ? std::shared_ptr<const void>{} : std::move(live);
					return Found::Ready;
				}
			}
			if (slot.rejected) {
				a_binding = { kInvalid, {} };
				return Found::Rejected;
			}
			return Found::Unknown;
		}

		// The import: marks the image stable in DXVK (a synchronous chunk on its CS thread), wraps it in BasicRHI and writes
		// its view into ORG's heap. Any thread; the entry is the caller's until Settle.
		static Reject Import(Entry& a_entry, const ImportContext& a_context, Counters& a_counters, std::uint32_t a_sourceTag)
		{
			auto* view = a_entry.view.get();
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			view->GetDesc(&viewDesc);
			rhi::SrvDesc srv{};
			if (!DescribeView(viewDesc, srv)) {
				if (a_counters.rejected[static_cast<std::size_t>(Reject::View)].load(std::memory_order_relaxed) < 8)
					logger::info("[DCLF] Game texture view not supported: format {}, dimension {}", static_cast<int>(viewDesc.Format), static_cast<int>(viewDesc.ViewDimension));
				return Reject::View;
			}

			DxvkOrgInteropResourceInfo info{};
			{
				ZoneScopedN("CS.DCLF.Texture.Import.DescribeResource");
				if (!RenderGraphRuntime::Get().DescribeResource(view, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
					return Reject::NotImage;
			}
			const auto& image = info.image;
			// DXVK keeps images it hands out (marked stable) in GENERAL and uses them between the graph's
			// commands: imported with simultaneous access, they are read in GENERAL and never transitioned.
			if (image.layout != VK_IMAGE_LAYOUT_GENERAL) {
				if (a_counters.unsupportedLayouts.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::warn("[DCLF] A game texture is in Vulkan layout {}, not GENERAL; such textures stay native", static_cast<int>(image.layout));
				return Reject::Import;
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
				reinterpret_cast<std::uintptr_t>(image.image), reinterpret_cast<std::uintptr_t>(view));
			import.debugName = importName.c_str();
			{
				ZoneScopedN("CS.DCLF.Texture.Import.WrapImage");
				if (rhi::vulkan::import_image(a_context.device, import, a_entry.image) != rhi::Result::Ok || !a_entry.image)
					return Reject::Import;
			}

			srv.componentMapping = MappingOf(image.components);
			auto backing = a_context.cleanup->Make<ImportedBacking>();
			backing->view.copy_from(view);
			backing->image = std::move(a_entry.image);
			{
				ZoneScopedN("CS.DCLF.Texture.Import.CreateView");
				a_entry.binding = a_context.service->CreateOwnedShaderResourceView(a_context.deviceOwner, backing->image.Get(), backing, srv);
			}
			if (!a_entry.binding)
				return Reject::View;
			a_entry.index = a_entry.binding.Index();
			TracyPlot("CS.DCLF.Texture.ImportSource", static_cast<std::int64_t>(a_sourceTag));
			if (!SwitchValue(Switch::TraceTexturePaths).empty()) {
				TracyPlot("CS.DCLF.Texture.ImportView", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(view)));
				TracyPlot("CS.DCLF.Texture.ImportImage", static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(image.image)));
			}
			return Reject::None;
		}

		// Publishes an import's result: its fields, then its state (release).
		static void Settle(Entry& a_entry, Reject a_reject, Counters& a_counters)
		{
			a_entry.reject = a_reject;
			if (a_reject != Reject::None)
				a_counters.rejected[static_cast<std::size_t>(a_reject)].fetch_add(1, std::memory_order_relaxed);
			else if (auto owner = a_entry.registry.lock())
				owner->liveCount.fetch_add(1, std::memory_order_relaxed);
			a_entry.state.store(a_reject == Reject::None ? State::Ready : State::Rejected, std::memory_order_release);
		}

		// An import's binding as a Request is answered with: the entry's, or kInvalid with no owner when it was rejected.
		static Binding AnswerOf(const std::shared_ptr<Entry>& a_entry, Reject a_reject)
		{
			if (a_reject != Reject::None || a_entry->index == kInvalid)
				return { kInvalid, {} };
			return { a_entry->index, a_entry };
		}

		static void Answered(Importer& a_importer, Replies& a_replies, ID3D11ShaderResourceView* a_view, std::uint64_t a_cookie, Binding a_binding)
		{
			Reply reply;
			reply.view = a_view;
			reply.cookie = a_cookie;
			reply.binding = std::move(a_binding);
			a_replies.Push(std::move(reply));
			a_importer.requestsPending.fetch_sub(1, std::memory_order_relaxed);
			a_importer.requestsAnswered.fetch_add(1, std::memory_order_relaxed);
		}

		// The import thread: a Request answered from the registry, or by importing the view now. The registry holds no entry pending
		// but the one this thread is importing (ResolveBinding registers only what it has imported).
		static void Answer(Importer& a_importer, Work& a_work, const Published& a_published)
		{
			ZoneScopedN("CS.DCLF.Texture.Answer");
			auto* key = a_work.view.get();
			if (!key) {
				Answered(a_importer, *a_work.replies, nullptr, a_work.cookie, a_published.fixed->null);
				return;
			}
			auto& table = *a_published.registry;
			Binding binding{ kInvalid, {} };
			std::shared_ptr<Entry> entry;
			{
				std::scoped_lock lock(table.mutex);
				if (Find(table, key, binding) == Found::Unknown) {
					entry = a_published.context.cleanup->Make<Entry>();
					entry->view = std::move(a_work.view);  // the request's reference: the SRV stays alive (its address unused) while imported
					entry->registry = a_published.registry;
					entry->key = key;
					entry->serial = ++table.nextSerial;
					auto& slot = table.entries[key];
					slot = {};
					slot.live = entry;
					slot.serial = entry->serial;
				}
			}
			if (entry) {
				Reject reject = Reject::Import;
				try {
					reject = Import(*entry, a_published.context, *a_published.counters, a_work.sourceTag);
				} catch (const std::exception& e) {
					logger::error("[DCLF] A game texture could not be imported; its draws stay native: {}", e.what());
				}
				if (reject == Reject::None)
					a_published.counters->importedAsync.fetch_add(1, std::memory_order_relaxed);
				Settle(*entry, reject, *a_published.counters);
				if (reject != Reject::None) {
					std::scoped_lock lock(table.mutex);
					if (const auto found = table.entries.find(key); found != table.entries.end() && found->second.serial == entry->serial)
						found->second.rejected = entry;
				}
				binding = AnswerOf(entry, reject);
			}
			Answered(a_importer, *a_work.replies, key, a_work.cookie, std::move(binding));
		}

		static void RunImporter(Importer& a_importer)
		{
#	if defined(TRACY_ENABLE)
			tracy::SetThreadName("CS DCLF texture import");
#	endif
			// Requests that came before the render thread published a context (PublishImportContext wakes the thread).
			std::vector<Work> parked;
			for (;;) {
				const std::uint64_t observed = a_importer.wake.load(std::memory_order_acquire);
				if (a_importer.stop.load(std::memory_order_acquire))
					return;
				const auto published = a_importer.published.load(std::memory_order_acquire);
				bool worked = false;
				if (published && !parked.empty()) {
					for (auto& work : parked)
						Answer(a_importer, work, *published);
					parked.clear();
					worked = true;
				}
				// The work's references (an import nobody asks for any more, its material gone meanwhile) are let go here.
				worked |= a_importer.queue.Drain([&](Work&& a_work) {
					if (published)
						Answer(a_importer, a_work, *published);
					else
						parked.push_back(std::move(a_work));
				}) != 0;
				if (!worked)
					a_importer.wake.wait(observed, std::memory_order_acquire);
			}
		}

		void Queue(Work&& a_work)
		{
			importer->queue.Push(std::move(a_work));
			importer->Notify();
		}
	};

	GpuTextures::GpuTextures() :
		impl(std::make_unique<Impl>())
	{
		impl->samplers.fill(kInvalid);
		impl->samplerFailed.fill(false);
		// The import thread from the start, so any thread may queue for it (Request) without a lazy start to race on.
		impl->importer = std::make_shared<Impl::Importer>();
		std::thread([state = impl->importer] { Impl::RunImporter(*state); }).detach();
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
		auto registry = impl->registry;
		if (registry) {
			ZoneScopedN("CS.DCLF.Texture.RegistryLookup");
			Binding binding;
			std::scoped_lock lock(registry->mutex);
			if (impl->Find(*registry, a_view, binding) != Impl::Found::Unknown)
				return binding;  // a pending import is not waited for: the caller asks again (its binding is kInvalid)
		}
		// A descriptor import is legal only inside an epoch. Do not cache an Import
		// rejection merely because the caller reached us outside that boundary:
		// the same view may become executable at the next epoch.
		Impl::ImportContext context;
		if (!Impl::MakeContext(context))
			return { kInvalid, {} };
		{
			ZoneScopedN("CS.DCLF.Texture.Import");
			impl->cleanup = context.cleanup;
			if (!registry)
				impl->registry = registry = context.cleanup->Make<Impl::Registry>();
			auto entry = context.cleanup->Make<Impl::Entry>();
			entry->view.copy_from(a_view);
			entry->registry = registry;
			entry->key = a_view;
			const auto reject = Impl::Import(*entry, context, *impl->counters, a_sourceTag);
			Impl::Settle(*entry, reject, *impl->counters);
			{
				// Registered settled, never pending: a pending entry is one whose import is queued for the import thread, which a
				// Request that finds it waits on (Impl::Answer). The import thread may have imported the view meanwhile: both are valid.
				ZoneScopedN("CS.DCLF.Texture.Import.Register");
				std::scoped_lock lock(registry->mutex);
				entry->serial = ++registry->nextSerial;
				auto& slot = registry->entries[a_view];
				slot = {};
				slot.live = entry;
				slot.serial = entry->serial;
				if (reject != Reject::None)
					slot.rejected = entry;
			}
			if (reject != Reject::None)
				return { kInvalid, {} };
			return { entry->index, entry };
		}
	}

	void GpuTextures::Request(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, Replies& a_replies, std::uint64_t a_cookie)
	{
		Impl::Work work;
		work.view.copy_from(a_view);  // the caller's view is alive here; the request holds it until the import does, or it is answered
		work.sourceTag = a_sourceTag;
		work.replies = &a_replies;
		work.cookie = a_cookie;
		impl->importer->requestsPending.fetch_add(1, std::memory_order_relaxed);
		impl->Queue(std::move(work));
	}

	void GpuTextures::PublishImportContext()
	{
		Impl::ImportContext context;
		if (!Impl::MakeContext(context))
			return;
		impl->cleanup = context.cleanup;
		if (!impl->registry)
			impl->registry = context.cleanup->Make<Impl::Registry>();
		// The fixed bindings, made here once each (the samplers read the engine's table, a render thread read): the scene work takes
		// them from the publication (Fixed), never makes them.
		FixedBindings fixed;
		fixed.null = NullBinding();
		for (std::uint32_t address = 0; address < kAddressModes; ++address)
			for (std::uint32_t filter = 0; filter < kFilterModes; ++filter)
				fixed.samplers[address * kFilterModes + filter] = SamplerBinding(address, filter);
		auto same = [](const Binding& a_a, const Binding& a_b) { return a_a.index == a_b.index && a_a.owner == a_b.owner; };
		auto& importer = *impl->importer;
		const auto current = importer.published.load(std::memory_order_acquire);
		const bool sameFixed = current && same(current->fixed->null, fixed.null) &&
		                       std::equal(fixed.samplers.begin(), fixed.samplers.end(), current->fixed->samplers.begin(), same);
		if (current && sameFixed && current->context.service == context.service && current->context.cleanup == context.cleanup &&
			current->context.deviceOwner == context.deviceOwner && current->registry == impl->registry && current->counters == impl->counters)
			return;
		auto next = std::make_shared<Impl::Published>();
		next->context = std::move(context);
		next->registry = impl->registry;
		next->counters = impl->counters;
		next->fixed = sameFixed ? current->fixed : std::make_shared<const FixedBindings>(std::move(fixed));
		importer.published.store(std::move(next), std::memory_order_release);
		// The requests parked for want of a context.
		importer.Notify();
	}

	std::shared_ptr<const GpuTextures::FixedBindings> GpuTextures::Fixed() const
	{
		const auto published = impl->importer->published.load(std::memory_order_acquire);
		return published ? published->fixed : nullptr;
	}

	std::shared_ptr<const void> GpuTextures::Seal(std::vector<std::shared_ptr<const void>> a_owners) const
	{
		const auto published = impl->importer->published.load(std::memory_order_acquire);
		if (!published)
			return {};  // nothing was answered yet, so nothing holds an owner
		return org::ExecutionResourceLease::Create(published->context.cleanup, std::move(a_owners)).Owner();
	}

	std::uint32_t GpuTextures::NullIndex()
	{
		if (impl->nullIndex != kInvalid)
			return impl->nullIndex;
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Impl::ServiceNow(retained);
		if (!service)
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
		if (a_addressMode >= kAddressModes || a_filterMode >= kFilterModes)
			return kInvalid;
		auto& index = impl->samplers[a_addressMode * kFilterModes + a_filterMode];
		if (index != kInvalid)
			return index;
		std::shared_ptr<org::runtime::IDescriptorService> retained;
		auto* service = Impl::ServiceNow(retained);
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
		const auto& counters = *impl->counters;
		for (std::size_t i = 0; i < result.rejected.size(); ++i)
			result.rejected[i] = counters.rejected[i].load(std::memory_order_relaxed);
		result.unsupportedLayouts = counters.unsupportedLayouts.load(std::memory_order_relaxed);
		result.importedAsync = counters.importedAsync.load(std::memory_order_relaxed);
		result.requestsPending = impl->importer->requestsPending.load(std::memory_order_relaxed);
		result.requestsAnswered = impl->importer->requestsAnswered.load(std::memory_order_relaxed);
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
		// Imports in flight settle into the old registry and counters, which their requests hold.
		impl->counters = std::make_shared<Impl::Counters>();
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
	void GpuTextures::Request(ID3D11ShaderResourceView* a_view, std::uint32_t, Replies& a_replies, std::uint64_t a_cookie)
	{
		// No render graph, no import: every view is answered rejected.
		Reply reply;
		reply.view = a_view;
		reply.cookie = a_cookie;
		reply.binding = { kInvalid, {} };
		a_replies.Push(std::move(reply));
	}
	void GpuTextures::PublishImportContext() {}
	std::shared_ptr<const GpuTextures::FixedBindings> GpuTextures::Fixed() const { return nullptr; }
	std::shared_ptr<const void> GpuTextures::Seal(std::vector<std::shared_ptr<const void>>) const { return {}; }
	std::uint32_t GpuTextures::NullIndex() { return kInvalid; }
	GpuTextures::Binding GpuTextures::NullBinding() { return {}; }
	std::uint32_t GpuTextures::Sampler(std::uint32_t, std::uint32_t) { return kInvalid; }
	GpuTextures::Binding GpuTextures::SamplerBinding(std::uint32_t, std::uint32_t) { return {}; }
	GpuTextures::Stats GpuTextures::GetStats() const { return stats; }
	void GpuTextures::Clear() {}
}

#endif
