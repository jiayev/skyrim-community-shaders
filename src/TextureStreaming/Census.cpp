#include "Census.h"

#include "Utils/SwitchFile.h"

#include <DirectXTex.h>
#include <intrin.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace TextureStreaming::Census
{
	namespace
	{
		// A bind this recent counts as a texture still in native use.
		constexpr std::uint64_t kRecentMilliseconds = 15000;
		// The base window's largest side, as streaming would use it.
		constexpr std::uint32_t kBaseSize = 128;

		// {5D8F6C2A-7B40-4B6E-9A51-2E7C1F0A9D31}
		constexpr GUID kSentinelGuid = { 0x5d8f6c2a, 0x7b40, 0x4b6e, { 0x9a, 0x51, 0x2e, 0x7c, 0x1f, 0x0a, 0x9d, 0x31 } };

		struct Entry
		{
			std::string path;
			D3D11_TEXTURE2D_DESC desc{};
			bool is2D = false;
			std::uint64_t bytes = 0;
			std::uint64_t baseBytes = 0;
			std::atomic<std::uint64_t> engineBinds{ 0 }, otherBinds{ 0 }, directUses{ 0 };
			std::atomic<std::uint64_t> lastEngineBind{ 0 }, lastOtherBind{ 0 };
			std::atomic<bool> rendererUse{ false };
		};

		std::shared_mutex g_mutex;
		std::unordered_map<const void*, std::shared_ptr<Entry>> g_views;
		std::unordered_map<const void*, std::shared_ptr<Entry>> g_resources;

		// Lives in the view's private data, so the entry goes when the view does.
		class Sentinel final : public IUnknown
		{
		public:
			Sentinel(const void* a_view, const void* a_resource) :
				view(a_view), resource(a_resource) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_id, void** a_out) override
			{
				if (a_id == IID_IUnknown) {
					AddRef();
					*a_out = this;
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++count; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = --count;
				if (left == 0) {
					{
						std::unique_lock lock(g_mutex);
						g_views.erase(view);
						g_resources.erase(resource);
					}
					delete this;
				}
				return left;
			}

		private:
			std::atomic<ULONG> count{ 1 };
			const void* view;
			const void* resource;
		};

		std::uint64_t MipBytes(const D3D11_TEXTURE2D_DESC& a_desc, std::uint32_t a_mip)
		{
			std::size_t row = 0, slice = 0;
			const auto width = std::max(a_desc.Width >> a_mip, 1u), height = std::max(a_desc.Height >> a_mip, 1u);
			if (FAILED(DirectX::ComputePitch(a_desc.Format, width, height, row, slice)))
				return 0;
			return slice * a_desc.ArraySize;
		}

		// The finest mip a base window can start at: its larger side at most kBaseSize and, for block formats, both sides multiples of 4.
		std::uint32_t BaseMip(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			const bool block = DirectX::IsCompressed(a_desc.Format);
			for (std::uint32_t mip = 0; mip < a_desc.MipLevels; ++mip) {
				const auto width = std::max(a_desc.Width >> mip, 1u), height = std::max(a_desc.Height >> mip, 1u);
				if (std::max(width, height) <= kBaseSize && (!block || (width % 4 == 0 && height % 4 == 0)))
					return mip;
			}
			return 0;
		}

		std::string StreamName(RE::NiBinaryStream* a_stream)
		{
			if (!a_stream || *reinterpret_cast<std::uintptr_t*>(a_stream) != RE::VTABLE_BSResourceNiBinaryStream[0].address())
				return {};
			auto* stream = static_cast<RE::BSResourceNiBinaryStream*>(a_stream)->stream.get();
			RE::BSFixedString name;
			if (!stream || !stream->DoGetName(name) || !name.c_str())
				return {};
			return name.c_str();
		}

		void Register(RE::NiBinaryStream* a_stream, RE::BSGraphics::Texture* a_texture)
		{
			if (!a_texture || !a_texture->texture || !a_texture->resourceView)
				return;
			auto entry = std::make_shared<Entry>();
			entry->path = StreamName(a_stream);
			D3D11_RESOURCE_DIMENSION dimension{};
			a_texture->texture->GetType(&dimension);
			entry->is2D = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			if (entry->is2D) {
				static_cast<ID3D11Texture2D*>(a_texture->texture)->GetDesc(&entry->desc);
				const auto base = BaseMip(entry->desc);
				for (std::uint32_t mip = 0; mip < entry->desc.MipLevels; ++mip) {
					const auto bytes = MipBytes(entry->desc, mip);
					entry->bytes += bytes;
					if (mip >= base)
						entry->baseBytes += bytes;
				}
			}
			auto* view = a_texture->resourceView;
			auto* resource = a_texture->texture;
			{
				std::unique_lock lock(g_mutex);
				g_views[view] = entry;
				g_resources[resource] = std::move(entry);
			}
			auto* sentinel = new Sentinel(view, resource);
			view->SetPrivateDataInterface(kSentinelGuid, sentinel);
			sentinel->Release();
		}

		bool FromEngine(const void* a_address)
		{
			static const auto base = REL::Module::get().base();
			static const auto end = REL::Module::get().segment(REL::Segment::textx).address() + REL::Module::get().segment(REL::Segment::textx).size();
			const auto address = reinterpret_cast<std::uintptr_t>(a_address);
			return address >= base && address < end;
		}

		std::uint64_t Now() { return ::GetTickCount64(); }

		void NoteBinds(std::uint32_t a_count, ID3D11ShaderResourceView* const* a_views, const void* a_caller)
		{
			if (!a_views)
				return;
			const bool engine = FromEngine(a_caller);
			const auto now = Now();
			std::shared_lock lock(g_mutex);
			for (std::uint32_t i = 0; i < a_count; ++i) {
				if (!a_views[i])
					continue;
				if (const auto it = g_views.find(a_views[i]); it != g_views.end()) {
					auto& entry = *it->second;
					(engine ? entry.engineBinds : entry.otherBinds).fetch_add(1, std::memory_order_relaxed);
					(engine ? entry.lastEngineBind : entry.lastOtherBind).store(now, std::memory_order_relaxed);
				}
			}
		}

		void NoteDirect(const void* a_resource)
		{
			std::shared_lock lock(g_mutex);
			if (const auto it = g_resources.find(a_resource); it != g_resources.end())
				it->second->directUses.fetch_add(1, std::memory_order_relaxed);
		}

		template <std::size_t Slot>
		struct SetShaderResources
		{
			static void thunk(ID3D11DeviceContext* a_context, UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_views)
			{
				NoteBinds(a_count, a_views, _ReturnAddress());
				func(a_context, a_start, a_count, a_views);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CopySubresourceRegion
		{
			static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a_dst, UINT a_dstSub, UINT a_x, UINT a_y, UINT a_z, ID3D11Resource* a_src, UINT a_srcSub, const D3D11_BOX* a_box)
			{
				NoteDirect(a_dst);
				NoteDirect(a_src);
				func(a_context, a_dst, a_dstSub, a_x, a_y, a_z, a_src, a_srcSub, a_box);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CopyResource
		{
			static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a_dst, ID3D11Resource* a_src)
			{
				NoteDirect(a_dst);
				NoteDirect(a_src);
				func(a_context, a_dst, a_src);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Map
		{
			static HRESULT thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a_resource, UINT a_sub, D3D11_MAP a_type, UINT a_flags, D3D11_MAPPED_SUBRESOURCE* a_mapped)
			{
				NoteDirect(a_resource);
				return func(a_context, a_resource, a_sub, a_type, a_flags, a_mapped);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct UpdateSubresource
		{
			static void thunk(ID3D11DeviceContext* a_context, ID3D11Resource* a_resource, UINT a_sub, const D3D11_BOX* a_box, const void* a_data, UINT a_row, UINT a_depth)
			{
				NoteDirect(a_resource);
				func(a_context, a_resource, a_sub, a_box, a_data, a_row, a_depth);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct GenerateMips
		{
			static void thunk(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_view)
			{
				winrt::com_ptr<ID3D11Resource> resource;
				if (a_view) {
					a_view->GetResource(resource.put());
					NoteDirect(resource.get());
				}
				func(a_context, a_view);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// BSGraphics::Renderer's DDS loader: the renderer's load and EngineFixes' replacement of it both end here.
		struct LoadTextureFromStream
		{
			static HRESULT thunk(ID3D11Device* a_device, RE::NiBinaryStream* a_stream, RE::BSGraphics::Texture** a_out, void* a_header, std::uint32_t a_arg5, std::uint32_t a_arg6)
			{
				const HRESULT result = func(a_device, a_stream, a_out, a_header, a_arg5, a_arg6);
				if (SUCCEEDED(result) && a_out)
					Register(a_stream, *a_out);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Enabled()
	{
		static const bool enabled = Util::SwitchFile::Enabled("CS_TEX_CENSUS");
		return enabled;
	}

	void InstallGameHooks()
	{
		if (!Enabled())
			return;
		stl::detour_thunk<LoadTextureFromStream>(REL::RelocationID(75721, 77533));
		logger::info("[TextureStreaming] Census on");
	}

	void InstallD3DHooks(ID3D11DeviceContext* a_context)
	{
		if (!Enabled())
			return;
		stl::detour_vfunc<8, SetShaderResources<8>>(a_context);    // PS
		stl::detour_vfunc<25, SetShaderResources<25>>(a_context);  // VS
		stl::detour_vfunc<31, SetShaderResources<31>>(a_context);  // GS
		stl::detour_vfunc<59, SetShaderResources<59>>(a_context);  // HS
		stl::detour_vfunc<63, SetShaderResources<63>>(a_context);  // DS
		stl::detour_vfunc<67, SetShaderResources<67>>(a_context);  // CS
		stl::detour_vfunc<14, Map>(a_context);
		stl::detour_vfunc<46, CopySubresourceRegion>(a_context);
		stl::detour_vfunc<47, CopyResource>(a_context);
		stl::detour_vfunc<48, UpdateSubresource>(a_context);
		stl::detour_vfunc<54, GenerateMips>(a_context);
	}

	void NoteRendererUse(ID3D11ShaderResourceView* a_view)
	{
		if (!Enabled())
			return;
		std::shared_lock lock(g_mutex);
		if (const auto it = g_views.find(a_view); it != g_views.end())
			it->second->rendererUse.store(true, std::memory_order_relaxed);
	}

	std::filesystem::path WriteJson(const std::filesystem::path& a_directory)
	{
		static std::atomic<std::uint32_t> serial{ 0 };
		const auto now = Now();
		struct Totals
		{
			std::uint64_t count = 0, bytes = 0, baseBytes = 0;
		};
		std::map<std::string, Totals> byClass;
		std::map<std::string, Totals> byFolderNative;
		nlohmann::json rows = nlohmann::json::array();
		{
			std::shared_lock lock(g_mutex);
			for (const auto& [view, entry] : g_views) {
				const auto recent = [&](const std::atomic<std::uint64_t>& a_stamp) {
					const auto stamp = a_stamp.load(std::memory_order_relaxed);
					return stamp && now - stamp < kRecentMilliseconds;
				};
				const bool streamable = entry->is2D && entry->desc.ArraySize == 1 && !(entry->desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) &&
				                        std::max(entry->desc.Width, entry->desc.Height) > kBaseSize;
				const bool engineRecent = recent(entry->lastEngineBind), otherRecent = recent(entry->lastOtherBind);
				const bool renderer = entry->rendererUse.load(std::memory_order_relaxed);
				const char* use = (engineRecent || otherRecent) ? (renderer ? "native+renderer" : "native") : (renderer ? "renderer" : "unused");
				const std::string kind = std::string(streamable ? "streamable/" : "fixed/") + use;
				auto& totals = byClass[kind];
				++totals.count;
				totals.bytes += entry->bytes;
				totals.baseBytes += entry->baseBytes;
				if (engineRecent || otherRecent) {
					const auto first = entry->path.find('\\');
					const auto second = first == std::string::npos ? first : entry->path.find('\\', first + 1);
					auto& folder = byFolderNative[entry->path.substr(0, second)];
					++folder.count;
					folder.bytes += entry->bytes;
				}
				rows.push_back({ { "path", entry->path },
					{ "width", entry->desc.Width },
					{ "height", entry->desc.Height },
					{ "mips", entry->desc.MipLevels },
					{ "array", entry->desc.ArraySize },
					{ "format", static_cast<int>(entry->desc.Format) },
					{ "usage", static_cast<int>(entry->desc.Usage) },
					{ "bind", entry->desc.BindFlags },
					{ "misc", entry->desc.MiscFlags },
					{ "bytes", entry->bytes },
					{ "baseBytes", entry->baseBytes },
					{ "streamable", streamable },
					{ "use", use },
					{ "engineBinds", entry->engineBinds.load(std::memory_order_relaxed) },
					{ "otherBinds", entry->otherBinds.load(std::memory_order_relaxed) },
					{ "directUses", entry->directUses.load(std::memory_order_relaxed) },
					{ "engineRecent", engineRecent },
					{ "otherRecent", otherRecent },
					{ "renderer", renderer } });
			}
		}
		const auto mib = [](std::uint64_t a_bytes) { return static_cast<double>(a_bytes) / (1024.0 * 1024.0); };
		for (const auto& [kind, totals] : byClass)
			logger::info("[TextureStreaming] Census {}: {} textures, {:.0f} MiB, {:.0f} MiB as base windows", kind, totals.count, mib(totals.bytes), mib(totals.baseBytes));
		for (const auto& [folder, totals] : byFolderNative)
			logger::info("[TextureStreaming] Census native folder {}: {} textures, {:.0f} MiB", folder, totals.count, mib(totals.bytes));
		const auto path = a_directory / std::format("texture-census-{}.json", serial.fetch_add(1));
		std::ofstream(path) << rows.dump(1);
		return path;
	}
}
