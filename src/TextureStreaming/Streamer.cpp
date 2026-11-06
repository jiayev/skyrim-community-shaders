#include "Streamer.h"

#include "Features/TextureStreaming.h"
#include "Globals.h"
#include "Policy.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "Utils/SwitchFile.h"
#include "Vram/Debug.h"
#include "Vram/DxvkMemory.h"

#include <DirectXTex.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace TextureStreaming
{
	namespace
	{
		using PFN_Missing = void(__stdcall*)(std::uint64_t);
		using PFN_Gone = void(__stdcall*)(std::uint64_t);
		using PFN_Fill = const D3D11_SUBRESOURCE_DATA*(__stdcall*)(std::uint64_t, BOOL);
		using PFN_EnableView = HRESULT(__stdcall*)(ID3D11ShaderResourceView*, PFN_Missing, PFN_Gone, PFN_Fill, std::uint64_t);
		using PFN_SetWindow = void(__stdcall*)(ID3D11ShaderResourceView*, ID3D11ShaderResourceView*);
		using PFN_GetWindow = ID3D11ShaderResourceView*(__stdcall*)(ID3D11ShaderResourceView*);
		using PFN_GetBinds = std::uint64_t(__stdcall*)(ID3D11ShaderResourceView*);

		// The DDS file's header: the magic, DDS_HEADER, and DDS_HEADER_DXT10 when its four-cc says so.
		constexpr std::uint32_t kDdsMagic = 0x20534444;
		constexpr std::uint32_t kDx10FourCC = 0x30315844;
		constexpr std::uint32_t kHeaderBytes = 0x80, kDx10HeaderBytes = 0x94;
		constexpr std::uint32_t kMaxDimension = 0x4000;
		constexpr std::chrono::milliseconds kTick{ 250 };
		// The driver's use at this share of its budget may be clamped to it.
		constexpr double kClampedShare = 0.995;
		constexpr std::size_t kMaxReleasesPerTick = 16;
		// After a change of worldspace, for this long (about 8 s, through most loads), what is not wanted goes at once: the last place's demand is over.
		constexpr std::uint32_t kWorldChangeTicks = 32;

		struct Exports
		{
			PFN_EnableView enable = nullptr;
			PFN_SetWindow setWindow = nullptr;
			PFN_GetWindow getWindow = nullptr;
			PFN_GetBinds getBinds = nullptr;
			explicit operator bool() const { return enable && setWindow && getWindow && getBinds; }
		};

		const Exports& Dxvk()
		{
			static const Exports exports = [] {
				Exports result;
				if (HMODULE module = ::GetModuleHandleW(L"dxvk_d3d11.dll")) {
					result.enable = reinterpret_cast<PFN_EnableView>(reinterpret_cast<void*>(::GetProcAddress(module, "dxvkStreamEnableView")));
					result.setWindow = reinterpret_cast<PFN_SetWindow>(reinterpret_cast<void*>(::GetProcAddress(module, "dxvkStreamSetWindow")));
					result.getWindow = reinterpret_cast<PFN_GetWindow>(reinterpret_cast<void*>(::GetProcAddress(module, "dxvkStreamGetWindow")));
					result.getBinds = reinterpret_cast<PFN_GetBinds>(reinterpret_cast<void*>(::GetProcAddress(module, "dxvkStreamGetBinds")));
				}
				return result;
			}();
			return exports;
		}

		std::uint32_t SwitchNumber(std::string_view a_name, std::uint32_t a_default)
		{
			const auto text = Util::SwitchFile::Value(a_name);
			std::uint32_t value = a_default;
			if (!text.empty())
				std::from_chars(text.data(), text.data() + text.size(), value);
			return value;
		}

		// The menu's values, with the switches that are set in their place.
		Settings WithSwitches(Settings a_settings)
		{
			const auto flag = [](std::string_view a_name, bool a_value) {
				const auto text = Util::SwitchFile::Value(a_name);
				return text.empty() ? a_value : text != "0";
			};
			a_settings.distance = flag("CS_DCLF_TEX_DISTANCE", a_settings.distance);
			a_settings.feedback = flag("CS_DCLF_TEX_FEEDBACK", a_settings.feedback);
			if (const auto text = Util::SwitchFile::Value("CS_DCLF_TEX_STREAM_DISTANCE_SCALE"); !text.empty())
				std::from_chars(text.data(), text.data() + text.size(), a_settings.distanceScale);
			a_settings.baseSize = SwitchNumber("CS_DCLF_TEX_STREAM_BASE", a_settings.baseSize);
			a_settings.rateMB = SwitchNumber("CS_DCLF_TEX_STREAM_RATE_MB", a_settings.rateMB);
			a_settings.budgetMB = SwitchNumber("CS_DCLF_TEX_STREAM_BUDGET_MB", a_settings.budgetMB);
			if (const auto text = Util::SwitchFile::Value("CS_DCLF_TEX_STREAM_VISIBLE_OFFSET"); !text.empty())
				std::from_chars(text.data(), text.data() + text.size(), a_settings.visibleOffset);
			a_settings.visibleOffset = std::clamp(a_settings.visibleOffset, 0.0f, Policy::kMaxVisibleOffset);
			return a_settings;
		}

		std::mutex g_settingsMutex;
		Settings& CurrentSettings()
		{
			static Settings settings = WithSwitches({});
			return settings;
		}

		// The base window's largest side. Above 32: Lighting treats a texture of 32 or less as absent (skin extras, hair flow).
		std::uint32_t BaseSize()
		{
			static const std::uint32_t size = std::clamp(GetSettings().baseSize, 64u, 1024u);
			return size;
		}

		struct Texture
		{
			TextureId id = 0;
			std::string path;
			winrt::com_ptr<ID3D11Device> device;
			D3D11_TEXTURE2D_DESC desc{};
			std::uint32_t headerBytes = 0;
			std::uint32_t baseTop = 0;
			std::uint64_t fullBytes = 0;
			std::vector<std::uint64_t> bytesFrom;  // by top: the window's bytes

			// Keeps the view from finishing its destruction under a call that names it: DXVK's gone callback takes it first.
			std::mutex stateMutex;
			ID3D11ShaderResourceView* shell = nullptr;  // not held: the record goes when the view does
			std::uint64_t residentBytes = 0;

			std::mutex buildMutex;  // one build of a texture at a time
			std::atomic<std::uint32_t> residentTop{ Policy::kNoWindow };
			std::atomic<bool> baseReady{ false };  // a window exists, or the load failed
			std::atomic<bool> failed{ false };
			std::atomic<bool> baseRequested{ false };
			std::atomic<bool> pinned{ false };
			std::atomic<std::uint64_t> nextRevision{ kBaseRevision + 1 };

			std::mutex fillMutex;
			std::vector<std::byte> fillBytes;  // while DXVK uploads a storage fill
			std::vector<D3D11_SUBRESOURCE_DATA> fillData;

			// The worker's alone.
			Policy::Pace pace;
			std::uint64_t lastBinds = 0;
			std::uint32_t nativeTicks = 0;
			std::uint32_t pendingTop = Policy::kNoWindow;  // asked for, not there yet
			std::uint32_t pendingTicks = 0;
			float distanceFootprint = std::numeric_limits<float>::infinity();  // this tick's demand: the finest noted of each kind
			float visibleFootprint = std::numeric_limits<float>::infinity();
			std::uint32_t wantedTop = 0;
		};

		std::shared_mutex g_mutex;
		std::unordered_map<TextureId, std::shared_ptr<Texture>> g_textures;
		std::unordered_map<const void*, std::shared_ptr<Texture>> g_shells;
		std::atomic<TextureId> g_nextId{ 1 };

		std::shared_mutex g_hooksMutex;
		Scheduler g_scheduler;
		std::function<void(const ID3D11ShaderResourceView*)> g_windowListener;
		std::function<void(DemandSink&)> g_demandSource;

		std::mutex g_retiredMutex;
		std::vector<TextureId> g_retired;
		std::mutex g_directMutex;
		std::vector<std::pair<TextureId, std::uint32_t>> g_direct;  // builds with no scheduler: the worker runs them

		std::atomic<int> g_forceTop{ -1 };
		std::atomic<bool> g_workerStarted{ false };

		std::atomic<std::uint64_t> g_cardBytes{ 0 }, g_driverBudgetBytes{ 0 }, g_budgetBytes{ 0 }, g_usedBytes{ 0 }, g_overflowBytes{ 0 }, g_otherBytes{ 0 }, g_outsideBytes{ 0 }, g_allowanceBytes{ 0 }, g_visibleBytes{ 0 };
		std::atomic<float> g_distancePullback{ 0.0f }, g_visiblePullback{ 0.0f };
		std::atomic<std::uint64_t> g_worldChanges{ 0 };
		std::mutex g_dumpMutex;
		std::string g_dumpPath;
		std::atomic<std::uint64_t> g_residentBytes{ 0 }, g_fullBytes{ 0 }, g_wantedBytes{ 0 }, g_windowsBuilt{ 0 }, g_bytesRead{ 0 }, g_loadFailures{ 0 }, g_safetyNetLoads{ 0 },
			g_storageFills{ 0 }, g_passedThrough{ 0 }, g_engineFailures{ 0 }, g_upgrades{ 0 }, g_downgrades{ 0 }, g_pressureDowngrades{ 0 }, g_noted{ 0 }, g_nativeFull{ 0 }, g_withWindow{ 0 }, g_feedbackReported{ 0 }, g_feedbackFiner{ 0 },
			g_baseBuilds{ 0 }, g_baseReadMicros{ 0 }, g_baseCreateMicros{ 0 }, g_otherBuilds{ 0 }, g_otherReadMicros{ 0 }, g_otherCreateMicros{ 0 };

		std::shared_ptr<Texture> Find(TextureId a_id)
		{
			std::shared_lock lock(g_mutex);
			const auto it = g_textures.find(a_id);
			return it == g_textures.end() ? nullptr : it->second;
		}

		std::shared_ptr<Texture> FindShell(const void* a_view)
		{
			std::shared_lock lock(g_mutex);
			const auto it = g_shells.find(a_view);
			return it == g_shells.end() ? nullptr : it->second;
		}

		std::uint32_t MipSide(std::uint32_t a_side, std::uint32_t a_mip) { return std::max(a_side >> a_mip, 1u); }

		std::uint64_t MipBytes(const D3D11_TEXTURE2D_DESC& a_desc, std::uint32_t a_mip, std::size_t* a_rowPitch = nullptr)
		{
			std::size_t row = 0, slice = 0;
			if (FAILED(DirectX::ComputePitch(a_desc.Format, MipSide(a_desc.Width, a_mip), MipSide(a_desc.Height, a_mip), row, slice)))
				return 0;
			if (a_rowPitch)
				*a_rowPitch = row;
			return slice;
		}

		std::uint64_t BytesFrom(const D3D11_TEXTURE2D_DESC& a_desc, std::uint32_t a_top)
		{
			std::uint64_t bytes = 0;
			for (std::uint32_t mip = a_top; mip < a_desc.MipLevels; ++mip)
				bytes += MipBytes(a_desc, mip);
			return bytes;
		}

		// A block-compressed texture's top level has sides of whole blocks.
		bool TopAllowed(const D3D11_TEXTURE2D_DESC& a_desc, std::uint32_t a_top)
		{
			return (MipSide(a_desc.Width, a_top) % 4) == 0 && (MipSide(a_desc.Height, a_top) % 4) == 0;
		}

		// The first window whose largest side is within the base size, or the coarsest a texture's sides allow.
		std::uint32_t BaseTop(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			std::uint32_t top = 0;
			for (std::uint32_t mip = 0; mip < a_desc.MipLevels; ++mip) {
				if (!TopAllowed(a_desc, mip))
					break;
				top = mip;
				if (std::max(MipSide(a_desc.Width, mip), MipSide(a_desc.Height, mip)) <= BaseSize())
					break;
			}
			return top;
		}

		std::uint32_t LimitTop(const Texture& a_texture, std::uint32_t a_top)
		{
			std::uint32_t top = std::min(a_top == kBaseTop ? a_texture.baseTop : a_top, a_texture.baseTop);
			while (top > 0 && !TopAllowed(a_texture.desc, top))
				--top;
			return top;
		}

		// The file's bytes from a_offset, a_bytes of them. Loose files and plain archive entries seek; a compressed entry's stream reads through.
		bool ReadFile(const std::string& a_path, std::uint64_t a_offset, std::uint64_t a_bytes, std::vector<std::byte>& a_out)
		{
			RE::BSResourceNiBinaryStream stream(a_path.c_str());
			if (!stream.good())
				return false;
			if (a_offset) {
				stream.seek(static_cast<std::int32_t>(a_offset));
				if (stream.tell() != a_offset)
					return false;
			}
			a_out.resize(a_bytes);
			if (!stream.read(reinterpret_cast<char*>(a_out.data()), static_cast<std::uint32_t>(a_bytes)))
				return false;
			g_bytesRead.fetch_add(a_bytes, std::memory_order_relaxed);
			return true;
		}

		// Subresources of mips [a_top, last], which lie in a_bytes one after the other.
		std::vector<D3D11_SUBRESOURCE_DATA> Subresources(const D3D11_TEXTURE2D_DESC& a_desc, std::uint32_t a_top, const std::vector<std::byte>& a_bytes)
		{
			std::vector<D3D11_SUBRESOURCE_DATA> data;
			std::uint64_t offset = 0;
			for (std::uint32_t mip = a_top; mip < a_desc.MipLevels; ++mip) {
				std::size_t row = 0;
				const auto slice = MipBytes(a_desc, mip, &row);
				data.push_back({ a_bytes.data() + offset, static_cast<UINT>(row), static_cast<UINT>(slice) });
				offset += slice;
			}
			return data;
		}

		std::uint64_t WindowOffset(const Texture& a_texture, std::uint32_t a_top)
		{
			std::uint64_t offset = a_texture.headerBytes;
			for (std::uint32_t mip = 0; mip < a_top; ++mip)
				offset += MipBytes(a_texture.desc, mip);
			return offset;
		}

		void Build(Texture& a_texture, std::uint32_t a_top)
		{
			std::scoped_lock build(a_texture.buildMutex);
			const auto resident = a_texture.residentTop.load(std::memory_order_relaxed);
			// A late base request does not coarsen what is there.
			if (a_top == kBaseTop && resident != Policy::kNoWindow)
				return;
			const auto top = LimitTop(a_texture, a_top);
			if (resident == top)
				return;
			const auto fail = [&](const char* a_why) {
				g_loadFailures.fetch_add(1, std::memory_order_relaxed);
				logger::warn("[TextureStreaming] {}: {}", a_texture.path, a_why);
				// A base window that cannot come opens the gate: its objects read zero rather than wait for ever.
				a_texture.failed.store(true, std::memory_order_relaxed);
				a_texture.baseReady.store(true, std::memory_order_release);
			};
			const auto bytes = BytesFrom(a_texture.desc, top);
			std::vector<std::byte> contents;
			const auto started = std::chrono::steady_clock::now();
			if (!ReadFile(a_texture.path, WindowOffset(a_texture, top), bytes, contents))
				return fail("could not read the file");
			const auto readDone = std::chrono::steady_clock::now();

			auto desc = a_texture.desc;
			desc.Width = MipSide(desc.Width, top);
			desc.Height = MipSide(desc.Height, top);
			desc.MipLevels = desc.MipLevels - top;
			const auto data = Subresources(a_texture.desc, top, contents);
			winrt::com_ptr<ID3D11Texture2D> window;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (FAILED(a_texture.device->CreateTexture2D(&desc, data.data(), window.put())) ||
				FAILED(a_texture.device->CreateShaderResourceView(window.get(), nullptr, view.put())))
				return fail("could not create the window");
			Vram::Debug::TagGameTexture(window.get(), a_texture.path);
			{
				const auto micros = [](auto a_span) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(a_span).count()); };
				const bool base = resident == Policy::kNoWindow;
				(base ? g_baseBuilds : g_otherBuilds).fetch_add(1, std::memory_order_relaxed);
				(base ? g_baseReadMicros : g_otherReadMicros).fetch_add(micros(readDone - started), std::memory_order_relaxed);
				(base ? g_baseCreateMicros : g_otherCreateMicros).fetch_add(micros(std::chrono::steady_clock::now() - readDone), std::memory_order_relaxed);
			}

			const ID3D11ShaderResourceView* shell = nullptr;
			{
				std::scoped_lock state(a_texture.stateMutex);
				if (!a_texture.shell)
					return;
				shell = a_texture.shell;
				Dxvk().setWindow(a_texture.shell, view.get());
				g_residentBytes.fetch_add(bytes, std::memory_order_relaxed);
				g_residentBytes.fetch_sub(a_texture.residentBytes, std::memory_order_relaxed);
				a_texture.residentBytes = bytes;
			}
			if (resident == Policy::kNoWindow)
				g_withWindow.fetch_add(1, std::memory_order_relaxed);
			a_texture.residentTop.store(top, std::memory_order_release);
			g_windowsBuilt.fetch_add(1, std::memory_order_relaxed);
			a_texture.baseReady.store(true, std::memory_order_release);

			std::function<void(const ID3D11ShaderResourceView*)> listener;
			{
				std::shared_lock lock(g_hooksMutex);
				listener = g_windowListener;
			}
			if (listener && resident != Policy::kNoWindow)
				listener(shell);
		}

		void PostBuild(Texture& a_texture, std::uint64_t a_revision, std::uint32_t a_top)
		{
			{
				std::shared_lock lock(g_hooksMutex);
				if (g_scheduler.post) {
					g_scheduler.post(a_texture.id, a_revision, a_top);
					return;
				}
			}
			std::scoped_lock lock(g_directMutex);
			g_direct.emplace_back(a_texture.id, a_top);
		}

		// A bind found no window: no scheduled load got there first. The bind waits for the base rather than read zero.
		void __stdcall OnMissing(std::uint64_t a_id)
		{
			if (const auto texture = Find(a_id); texture && !texture->failed.load(std::memory_order_relaxed)) {
				g_safetyNetLoads.fetch_add(1, std::memory_order_relaxed);
				Build(*texture, kBaseTop);
			}
		}

		// The view's destructor has begun: nothing may name it from here on.
		void __stdcall OnGone(std::uint64_t a_id)
		{
			std::shared_ptr<Texture> texture;
			{
				std::unique_lock lock(g_mutex);
				const auto it = g_textures.find(a_id);
				if (it == g_textures.end())
					return;
				texture = std::move(it->second);
				g_textures.erase(it);
				g_shells.erase(texture->shell);
			}
			g_fullBytes.fetch_sub(texture->fullBytes, std::memory_order_relaxed);
			{
				std::scoped_lock state(texture->stateMutex);
				texture->shell = nullptr;
				g_residentBytes.fetch_sub(texture->residentBytes, std::memory_order_relaxed);
				texture->residentBytes = 0;
			}
			if (texture->residentTop.load(std::memory_order_acquire) != Policy::kNoWindow)
				g_withWindow.fetch_sub(1, std::memory_order_relaxed);
			std::scoped_lock lock(g_retiredMutex);
			g_retired.push_back(a_id);
		}

		// Something needs the shell's own storage (a Map or a copy): the whole chain, on the calling thread.
		const D3D11_SUBRESOURCE_DATA* __stdcall OnFill(std::uint64_t a_id, BOOL a_release)
		{
			const auto texture = Find(a_id);
			if (!texture)
				return nullptr;
			if (a_release) {
				texture->fillData = {};
				texture->fillBytes = {};
				texture->fillMutex.unlock();
				return nullptr;
			}
			texture->fillMutex.lock();
			g_storageFills.fetch_add(1, std::memory_order_relaxed);
			logger::info("[TextureStreaming] {}: its own storage was needed", texture->path);
			if (!ReadFile(texture->path, texture->headerBytes, BytesFrom(texture->desc, 0), texture->fillBytes)) {
				texture->fillMutex.unlock();
				return nullptr;
			}
			texture->fillData = Subresources(texture->desc, 0, texture->fillBytes);
			return texture->fillData.data();
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

		bool StartsWith(std::string_view a_path, std::string_view a_prefix)
		{
			return a_path.size() >= a_prefix.size() && std::equal(a_prefix.begin(), a_prefix.end(), a_path.begin(), [](char a_a, char a_b) {
				return std::tolower(static_cast<unsigned char>(a_a)) == std::tolower(static_cast<unsigned char>(a_b));
			});
		}

		// BSResourceNiBinaryStream's constructor looks under Data: a stream's own name may carry the folder already.
		std::string OpenablePath(std::string a_name)
		{
			if (StartsWith(a_name, "data\\"))
				a_name.erase(0, 5);
			return a_name;
		}

		// What stays the engine's: what only native draws read, by folder.
		bool DeniedPath(std::string_view a_path)
		{
			for (const auto prefix : { "textures\\interface", "textures\\sky", "textures\\water", "textures\\effects", "textures\\cubemaps" }) {
				if (StartsWith(a_path, prefix))
					return true;
			}
			return false;
		}

		bool BlockCompressed(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return true;
			default:
				return false;
			}
		}

		// Reads the file's header into a description. Anything but a plain block-compressed 2D texture with mips stays the engine's.
		bool Describe(const std::byte* a_file, std::uint32_t a_bytes, D3D11_TEXTURE2D_DESC& a_desc, std::uint32_t& a_headerBytes)
		{
			DirectX::TexMetadata meta{};
			if (a_bytes < kHeaderBytes || FAILED(DirectX::GetMetadataFromDDSMemory(a_file, a_bytes, DirectX::DDS_FLAGS_NONE, meta)))
				return false;
			if (meta.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || meta.arraySize != 1 || meta.IsCubemap() || meta.mipLevels < 2 || !BlockCompressed(meta.format))
				return false;
			if (meta.width > kMaxDimension || meta.height > kMaxDimension || std::max(meta.width, meta.height) <= BaseSize())
				return false;
			const auto* words = reinterpret_cast<const std::uint32_t*>(a_file);
			if (words[0] != kDdsMagic)
				return false;
			a_headerBytes = words[21] == kDx10FourCC ? kDx10HeaderBytes : kHeaderBytes;
			a_desc = {};
			a_desc.Width = static_cast<UINT>(meta.width);
			a_desc.Height = static_cast<UINT>(meta.height);
			a_desc.MipLevels = static_cast<UINT>(meta.mipLevels);
			a_desc.ArraySize = 1;
			a_desc.Format = meta.format;
			a_desc.SampleDesc.Count = 1;
			a_desc.Usage = D3D11_USAGE_DEFAULT;
			a_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			return true;
		}

		// The block the engine's loader allocates (BSGraphics::Texture), holding the shell. Null: the engine's load goes ahead.
		RE::BSGraphics::Texture* MakeShell(ID3D11Device* a_device, RE::NiBinaryStream* a_stream, void* a_header)
		{
			auto path = OpenablePath(StreamName(a_stream));
			if (path.empty() || DeniedPath(path))
				return nullptr;
			std::array<std::byte, kDx10HeaderBytes> file{};
			RE::NiBinaryStream::BufferInfo info{};
			a_stream->get_info(info);
			if (info.totalSize < file.size())
				return nullptr;
			// The header comes from a stream of its own. The engine's load needs its stream untouched: it reads the whole file, and a rewind of
			// a stream already read did not give that back, so whatever was left to the engine failed to load.
			RE::BSResourceNiBinaryStream header(path.c_str());
			if (!header.good() || !header.read(reinterpret_cast<char*>(file.data()), static_cast<std::uint32_t>(file.size())))
				return nullptr;

			auto entry = std::make_shared<Texture>();
			if (!Describe(file.data(), static_cast<std::uint32_t>(file.size()), entry->desc, entry->headerBytes))
				return nullptr;
			const auto bytes = BytesFrom(entry->desc, 0);
			if (!bytes || info.totalSize < entry->headerBytes + bytes)
				return nullptr;

			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (FAILED(a_device->CreateTexture2D(&entry->desc, nullptr, texture.put())) || FAILED(a_device->CreateShaderResourceView(texture.get(), nullptr, view.put())))
				return nullptr;
			entry->id = g_nextId.fetch_add(1, std::memory_order_relaxed);
			if (FAILED(Dxvk().enable(view.get(), OnMissing, OnGone, OnFill, entry->id)))
				return nullptr;  // DXVK committed storage after all (d3d11.deferTextureAllocation off)
			entry->path = std::move(path);
			entry->shell = view.get();
			entry->device.copy_from(a_device);
			entry->baseTop = BaseTop(entry->desc);
			entry->fullBytes = bytes;
			entry->bytesFrom.resize(entry->desc.MipLevels);
			for (std::uint32_t top = 0; top < entry->desc.MipLevels; ++top)
				entry->bytesFrom[top] = BytesFrom(entry->desc, top);
			if (a_header)
				std::memcpy(a_header, file.data() + 4, 0x7C);
			g_fullBytes.fetch_add(bytes, std::memory_order_relaxed);
			{
				std::unique_lock lock(g_mutex);
				g_textures[entry->id] = entry;
				g_shells[entry->shell] = entry;
			}
			if (FullMode())
				Build(*entry, 0);

			auto* block = static_cast<RE::BSGraphics::Texture*>(RE::malloc(sizeof(RE::BSGraphics::Texture)));
			std::memset(static_cast<void*>(block), 0, sizeof(RE::BSGraphics::Texture));
			block->texture = texture.detach();
			block->resourceView = view.detach();
			return block;
		}

		// The worker's sink: the finest footprint each texture was noted with this tick.
		class Sink final : public DemandSink
		{
		public:
			void Note(const ID3D11ShaderResourceView* a_view, float a_footprintLog2, DemandKind a_kind) override
			{
				++noted;
				if (auto texture = FindShell(a_view)) {
					auto& footprint = a_kind == DemandKind::Visible ? texture->visibleFootprint : texture->distanceFootprint;
					footprint = std::min(footprint, a_footprintLog2);
				}
			}
			std::uint64_t noted = 0;
		};

		// What the worker keeps between ticks.
		struct Controller
		{
			Policy::Pullback pullback;
			std::uint64_t outsideBytes = 0;  // device-local memory that is not DXVK's: ORG, the swap chain, the driver
			std::uint64_t otherBytes = 0;    // what is not windows, smoothed
			const void* world = nullptr;     // an identity: the worldspace last seen
			std::uint32_t worldChangeTicks = 0;
			bool measured = false;
		};

		/**
		 * @brief What the windows may take: the budget's target less what everything else uses. The windows are subtracted from the measured use
		 * rather than the use compared with the budget, so the controller knows how much room there is, not just that it ran out.
		 *
		 * This counts DXVK's memory itself, wherever it lies, because the driver's figures cannot show use beyond its budget. Past that budget DXVK
		 * places what can spill in system memory, and AMD's driver reports the device-local heap's use as the budget at most. While it does, what
		 * is not DXVK's cannot be read: use less DXVK's allocations would seem to rise each time DXVK frees a chunk. So it keeps the value from
		 * before. False when it cannot tell.
		 */
		bool MeasureAllowance(Controller& a_controller, std::uint64_t& a_allowance)
		{
			std::uint64_t heapSize = 0, driverUsage = 0, driverBudget = 0, allocated = 0, used = 0, overflow = 0;
			for (const auto& report : Vram::DxvkHeapReports()) {
				if (report.heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
					if (!report.hasVkBudget)
						return false;
					heapSize += report.heapSize;
					driverUsage += report.vkUsage;
					driverBudget += report.vkBudget;
					allocated += report.dxvkAllocated;
					used += report.dxvkUsed;
				} else {
					overflow += report.dxvkUsed;
				}
			}
			if (!driverBudget)
				return false;

			const std::uint64_t outside = driverUsage > allocated ? driverUsage - allocated : 0;
			const bool clamped = static_cast<double>(driverUsage) >= static_cast<double>(driverBudget) * kClampedShare;
			if (!clamped || !a_controller.measured)
				a_controller.outsideBytes = outside;
			const std::uint64_t total = used + a_controller.outsideBytes + overflow;
			const std::uint64_t resident = g_residentBytes.load(std::memory_order_relaxed);
			const std::uint64_t other = total > resident ? total - resident : 0;
			// A rise counts within a second or so, and a fall takes tens: a window just replaced is still held for a while, and a load's spike passes.
			if (!a_controller.measured)
				a_controller.otherBytes = other;
			else if (other > a_controller.otherBytes)
				a_controller.otherBytes += (other - a_controller.otherBytes) / 4;
			else
				a_controller.otherBytes -= (a_controller.otherBytes - other) / 20;
			a_controller.measured = true;

			const std::uint64_t cap = std::uint64_t(GetSettings().budgetMB) << 20;
			const std::uint64_t budget = cap ? cap : driverBudget;
			const auto target = static_cast<std::uint64_t>(static_cast<double>(budget) * Policy::kBudgetTarget);
			a_allowance = target > a_controller.otherBytes ? target - a_controller.otherBytes : 0;

			g_cardBytes.store(heapSize, std::memory_order_relaxed);
			g_driverBudgetBytes.store(driverBudget, std::memory_order_relaxed);
			g_budgetBytes.store(budget, std::memory_order_relaxed);
			g_usedBytes.store(total, std::memory_order_relaxed);
			g_overflowBytes.store(overflow, std::memory_order_relaxed);
			g_otherBytes.store(a_controller.otherBytes, std::memory_order_relaxed);
			g_outsideBytes.store(a_controller.outsideBytes, std::memory_order_relaxed);
			g_allowanceBytes.store(a_allowance, std::memory_order_relaxed);
			return true;
		}

		std::uint32_t VisibleTop(const Texture& a_texture, float a_pullback)
		{
			std::uint32_t top = a_texture.baseTop;
			const auto largest = std::max(a_texture.desc.Width, a_texture.desc.Height);
			if (std::isfinite(a_texture.visibleFootprint))
				top = Policy::TopForFootprint(largest, a_texture.visibleFootprint + a_pullback, top);
			// The engine's own draws are on screen too, and report nothing.
			if (a_texture.nativeTicks >= Policy::kNativeTicks)
				top = std::min(top, static_cast<std::uint32_t>(a_pullback));
			return top;
		}

		std::uint32_t DistanceTop(const Texture& a_texture, float a_pullback)
		{
			if (!std::isfinite(a_texture.distanceFootprint))
				return a_texture.baseTop;
			return Policy::TopForFootprint(std::max(a_texture.desc.Width, a_texture.desc.Height), a_texture.distanceFootprint + a_pullback, a_texture.baseTop);
		}

		// The window a texture's demand asks for at this pull-back. A holder that does not follow swaps stays pinned at full.
		std::uint32_t WantedTop(const Texture& a_texture, float a_distance, float a_visible)
		{
			if (a_texture.pinned.load(std::memory_order_relaxed) || FullMode())
				return 0;
			return LimitTop(a_texture, std::min(VisibleTop(a_texture, a_visible), DistanceTop(a_texture, a_distance)));
		}

		void DrainRetired()
		{
			std::vector<TextureId> retired;
			{
				std::scoped_lock lock(g_retiredMutex);
				retired.swap(g_retired);
			}
			std::shared_lock lock(g_hooksMutex);
			if (g_scheduler.retire)
				for (const auto id : retired)
					g_scheduler.retire(id);
		}

		void RunDirectBuilds()
		{
			std::vector<std::pair<TextureId, std::uint32_t>> builds;
			{
				std::scoped_lock lock(g_directMutex);
				builds.swap(g_direct);
			}
			for (const auto& [id, top] : builds)
				BuildWindow(id, top);
		}

		std::atomic<const void*> g_playerWorld{ nullptr };
		std::atomic<bool> g_worldTaskQueued{ false };

		// Where the player is: a form only the main thread may follow, so read there, once per tick at most. All interiors count as one place of
		// their own, and going from one to another is not a change: the plain wait for a downgrade covers it.
		void ReadPlayerWorld()
		{
			static const char interiors = 0;
			if (const auto* player = RE::PlayerCharacter::GetSingleton()) {
				// No cell while a load has the player between places: the place stays what it was.
				if (const auto* cell = player->GetParentCell())
					g_playerWorld.store(cell->IsInteriorCell() ? static_cast<const void*>(&interiors) : cell->GetRuntimeData().worldSpace, std::memory_order_relaxed);
			}
			g_worldTaskQueued.store(false, std::memory_order_relaxed);
		}

		void CheckWorld(Controller& a_controller)
		{
			if (const auto tasks = SKSE::GetTaskInterface(); tasks && !g_worldTaskQueued.exchange(true, std::memory_order_relaxed))
				tasks->AddTask(&ReadPlayerWorld);
			const void* world = g_playerWorld.load(std::memory_order_relaxed);
			if (world != a_controller.world) {
				// The first place the player is seen in is no change of place.
				if (a_controller.world) {
					a_controller.worldChangeTicks = kWorldChangeTicks;
					g_worldChanges.fetch_add(1, std::memory_order_relaxed);
					logger::info("[TextureStreaming] the worldspace changed: what is not wanted goes at once");
				}
				a_controller.world = world;
			} else if (a_controller.worldChangeTicks) {
				--a_controller.worldChangeTicks;
			}
		}

		void WriteDump(const std::vector<std::shared_ptr<Texture>>& a_textures)
		{
			std::string path;
			{
				std::scoped_lock lock(g_dumpMutex);
				path.swap(g_dumpPath);
			}
			if (path.empty())
				return;
			std::ofstream out(path);
			out << "path\twidth\theight\tbaseTop\tresident\twanted\tpending\tpendingTicks\tvisible\tdistance\tnativeTicks\tpinned\tfailed\tresidentBytes\twantedBytes\tfullBytes\n";
			for (const auto& texture : a_textures) {
				const auto& t = *texture;
				const auto resident = t.residentTop.load(std::memory_order_acquire);
				const bool has = resident != Policy::kNoWindow;
				out << t.path << '\t' << t.desc.Width << '\t' << t.desc.Height << '\t' << t.baseTop << '\t' << (has ? static_cast<int>(resident) : -1) << '\t' << t.wantedTop << '\t'
					<< (t.pendingTop == Policy::kNoWindow ? -1 : static_cast<int>(t.pendingTop)) << '\t' << t.pendingTicks << '\t' << t.visibleFootprint << '\t' << t.distanceFootprint << '\t'
					<< t.nativeTicks << '\t' << t.pinned.load(std::memory_order_relaxed) << '\t' << t.failed.load(std::memory_order_relaxed) << '\t' << (has ? t.bytesFrom[resident] : 0) << '\t'
					<< t.bytesFrom[t.wantedTop] << '\t' << t.fullBytes << '\n';
			}
			logger::info("[TextureStreaming] wrote {} textures to {}", a_textures.size(), path);
		}

		void Tick(Controller& a_controller)
		{
			ZoneScopedN("CS.TextureStreaming.Tick");
			DrainRetired();
			RunDirectBuilds();
			CheckWorld(a_controller);

			std::vector<std::shared_ptr<Texture>> textures;
			{
				std::shared_lock lock(g_mutex);
				textures.reserve(g_textures.size());
				for (const auto& [id, texture] : g_textures)
					textures.push_back(texture);
			}
			if (textures.empty())
				return;

			for (auto& texture : textures) {
				texture->distanceFootprint = std::numeric_limits<float>::infinity();
				texture->visibleFootprint = std::numeric_limits<float>::infinity();
			}
			Sink sink;
			{
				std::function<void(DemandSink&)> source;
				{
					std::shared_lock lock(g_hooksMutex);
					source = g_demandSource;
				}
				if (source)
					source(sink);
			}
			g_noted.store(sink.noted, std::memory_order_relaxed);

			std::uint64_t nativeFull = 0;
			for (auto& texture : textures) {
				auto& t = *texture;
				// Native binds, read under the state lock: the view cannot finish its destruction meanwhile.
				std::uint64_t binds = t.lastBinds;
				{
					std::scoped_lock state(t.stateMutex);
					if (t.shell)
						binds = Dxvk().getBinds(t.shell);
				}
				t.nativeTicks = binds != t.lastBinds ? t.nativeTicks + 1 : 0;
				t.lastBinds = binds;
				nativeFull += t.nativeTicks >= Policy::kNativeTicks ? 1 : 0;
			}
			g_nativeFull.store(nativeFull, std::memory_order_relaxed);

			const auto hasWindow = [](const Texture& a_texture) {
				return a_texture.residentTop.load(std::memory_order_acquire) != Policy::kNoWindow && !a_texture.failed.load(std::memory_order_relaxed);
			};
			const int force = g_forceTop.load(std::memory_order_relaxed);
			std::uint64_t allowance = std::numeric_limits<std::uint64_t>::max();
			const bool bounded = MeasureAllowance(a_controller, allowance);
			auto& pullback = a_controller.pullback;
			if (force >= 0 || !bounded) {
				pullback = {};
			} else {
				Policy::StepPullback(pullback, allowance, GetSettings().visibleOffset, [&](float a_distance, float a_visible) {
					std::uint64_t bytes = 0;
					for (const auto& texture : textures) {
						if (hasWindow(*texture))
							bytes += texture->bytesFrom[WantedTop(*texture, a_distance, a_visible)];
					}
					return bytes;
				});
			}
			g_distancePullback.store(pullback.distance, std::memory_order_relaxed);
			g_visiblePullback.store(pullback.visible, std::memory_order_relaxed);

			struct Candidate
			{
				Texture* texture;
				std::uint32_t top;
				std::uint32_t short_;
				std::uint64_t bytes;  // an upgrade's: what it adds. A release's: what it frees.
				bool visible;
			};
			std::uint64_t wantedBytes = 0, visibleBytes = 0;
			std::uint64_t committed = g_residentBytes.load(std::memory_order_relaxed);  // with what is on its way
			std::vector<Candidate> upgrades;
			std::vector<Candidate> downgrades;
			for (auto& texture : textures) {
				auto& t = *texture;
				if (!hasWindow(t)) {
					t.wantedTop = t.baseTop;
					continue;
				}
				const auto resident = t.residentTop.load(std::memory_order_acquire);
				std::uint32_t wanted = WantedTop(t, pullback.distance, pullback.visible);
				if (force >= 0)
					wanted = LimitTop(t, static_cast<std::uint32_t>(force));
				t.wantedTop = wanted;
				wantedBytes += t.bytesFrom[wanted];
				const bool visible = VisibleTop(t, pullback.visible) <= DistanceTop(t, pullback.distance);
				if (visible)
					visibleBytes += t.bytesFrom[wanted];

				// What was asked for and not there yet stays asked for, unless it takes too long (the job failed or was replaced).
				if (t.pendingTop != Policy::kNoWindow) {
					if (t.pendingTop != resident && ++t.pendingTicks < 40) {
						if (t.pendingTop < resident)
							committed += t.bytesFrom[t.pendingTop] - t.bytesFrom[resident];
						continue;
					}
					t.pendingTop = Policy::kNoWindow;
					t.pendingTicks = 0;
				}
				auto action = Policy::Decide(resident, wanted, t.pace);
				if (force >= 0 && wanted != resident)
					action = wanted < resident ? Policy::Action::Upgrade : Policy::Action::Downgrade;
				if (a_controller.worldChangeTicks && wanted > resident)
					action = Policy::Action::Downgrade;
				if (action == Policy::Action::Upgrade)
					upgrades.push_back({ &t, wanted, resident - wanted, t.bytesFrom[wanted] - t.bytesFrom[resident], visible });
				else if (action == Policy::Action::Downgrade)
					downgrades.push_back({ &t, wanted, 0, t.bytesFrom[resident] - t.bytesFrom[wanted], visible });
			}
			g_wantedBytes.store(wantedBytes, std::memory_order_relaxed);
			g_visibleBytes.store(visibleBytes, std::memory_order_relaxed);

			const auto post = [&](Candidate& a_candidate) {
				a_candidate.texture->pendingTop = a_candidate.top;
				a_candidate.texture->pendingTicks = 0;
				PostBuild(*a_candidate.texture, a_candidate.texture->nextRevision.fetch_add(1, std::memory_order_relaxed), a_candidate.top);
			};
			for (auto& candidate : downgrades) {
				g_downgrades.fetch_add(1, std::memory_order_relaxed);
				post(candidate);
			}
			if (committed > allowance) {
				// Over the allowance: the wait for a downgrade is off, and the largest surplus goes first. Only down to what is wanted: the
				// controller decides what that is, so a texture on screen is not released to make room for another.
				std::vector<Candidate> surplus;
				for (auto& texture : textures) {
					auto& t = *texture;
					const auto resident = t.residentTop.load(std::memory_order_acquire);
					if (!hasWindow(t) || t.pendingTop != Policy::kNoWindow || t.wantedTop <= resident)
						continue;
					surplus.push_back({ &t, t.wantedTop, 0, t.bytesFrom[resident] - t.bytesFrom[t.wantedTop], false });
				}
				std::sort(surplus.begin(), surplus.end(), [](const Candidate& a_a, const Candidate& a_b) { return a_a.bytes > a_b.bytes; });
				std::uint64_t over = committed - allowance;
				for (std::size_t i = 0; i < std::min(surplus.size(), kMaxReleasesPerTick) && over; ++i) {
					g_pressureDowngrades.fetch_add(1, std::memory_order_relaxed);
					post(surplus[i]);
					committed -= surplus[i].bytes;
					over -= std::min(over, surplus[i].bytes);
				}
			}
			// What is on screen first, then the furthest short. The rate caps what starts this tick, and the next tick takes the rest. What is on screen
			// loads whatever the allowance says: the controller has already stepped it down as far as the setting lets it. Another upgrade that does
			// not fit is skipped rather than ending the list: a smaller one may still. A test's forced windows keep to the allowance.
			std::sort(upgrades.begin(), upgrades.end(), [](const Candidate& a_a, const Candidate& a_b) {
				return a_a.visible != a_b.visible ? a_a.visible : a_a.short_ > a_b.short_;
			});
			const std::uint64_t rate = std::uint64_t(GetSettings().rateMB) << 20;
			std::uint64_t uploadLeft = rate / 4;
			for (auto& candidate : upgrades) {
				if (candidate.bytes > uploadLeft && uploadLeft != rate / 4)
					break;
				if ((!candidate.visible || force >= 0) && (candidate.bytes > allowance || committed > allowance - candidate.bytes))
					continue;
				uploadLeft -= std::min(uploadLeft, candidate.bytes);
				committed += candidate.bytes;
				g_upgrades.fetch_add(1, std::memory_order_relaxed);
				post(candidate);
			}
			WriteDump(textures);
		}

		void StartWorker()
		{
			if (g_workerStarted.exchange(true))
				return;
			std::thread([] {
				SetThreadDescription(GetCurrentThread(), L"CS texture streaming");
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
				Controller controller;
				for (;;) {
					std::this_thread::sleep_for(kTick);
					Tick(controller);
				}
			}).detach();
		}

		// Shells need a renderer that schedules their windows and holds objects back until a base exists. Without one (DCLF off or forced off),
		// every first bind would load on the render thread, so the textures stay the engine's. The full chain needs neither.
		bool HasRenderer()
		{
			if (FullMode())
				return true;
			std::shared_lock lock(g_hooksMutex);
			return static_cast<bool>(g_scheduler.post);
		}

		// BSGraphics::Renderer's DDS loader: the renderer's load and EngineFixes' replacement of it both end here.
		struct LoadTextureFromStream
		{
			static HRESULT thunk(ID3D11Device* a_device, RE::NiBinaryStream* a_stream, RE::BSGraphics::Texture** a_out, void* a_header, std::uint32_t a_arg5, std::uint32_t a_arg6)
			{
				if (a_device && a_out && a_arg5 == 0 && a_arg6 == 0 && Dxvk() && HasRenderer()) {
					if (auto* block = MakeShell(a_device, a_stream, a_header)) {
						StartWorker();
						*a_out = block;
						return S_OK;
					}
				}
				g_passedThrough.fetch_add(1, std::memory_order_relaxed);
				const auto name = StreamName(a_stream);
				const auto result = func(a_device, a_stream, a_out, a_header, a_arg5, a_arg6);
				if (FAILED(result)) {
					g_engineFailures.fetch_add(1, std::memory_order_relaxed);
					logger::warn("[TextureStreaming] the engine's load of {} failed: {:#x}", name, static_cast<std::uint32_t>(result));
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Enabled()
	{
		// The loader's identity was checked on SE 1.5.97 and AE 1.6.1170, not on VR.
		static const bool enabled = [] {
			if (REL::Module::IsVR())
				return false;
			const auto text = Util::SwitchFile::Value("CS_DCLF_TEX_STREAM");
			return text.empty() ? globals::features::textureStreaming.loaded : text != "0";
		}();
		return enabled;
	}

	bool Active()
	{
		return Enabled() && Dxvk() && HasRenderer();
	}

	void SetSettings(const Settings& a_settings)
	{
		const auto settings = WithSwitches(a_settings);
		std::scoped_lock lock(g_settingsMutex);
		CurrentSettings() = settings;
	}

	Settings GetSettings()
	{
		std::scoped_lock lock(g_settingsMutex);
		return CurrentSettings();
	}

	bool FullMode()
	{
		static const bool full = Util::SwitchFile::Enabled("CS_DCLF_TEX_STREAM_FULL");
		return full;
	}

	void InstallGameHooks()
	{
		if (!Enabled())
			return;
		stl::detour_thunk<LoadTextureFromStream>(REL::RelocationID(75721, 77533));
		logger::info("[TextureStreaming] On{}", FullMode() ? ", every window the full chain" : "");
	}

	TextureId IdOf(const ID3D11ShaderResourceView* a_view)
	{
		if (!Enabled() || !a_view)
			return 0;
		const auto texture = FindShell(a_view);
		return texture ? texture->id : 0;
	}

	ID3D11ShaderResourceView* CurrentWindow(ID3D11ShaderResourceView* a_shell)
	{
		if (!Enabled() || !a_shell)
			return nullptr;
		return FindShell(a_shell) ? Dxvk().getWindow(a_shell) : nullptr;
	}

	bool BaseReady(const ID3D11ShaderResourceView* a_view)
	{
		if (!Enabled() || !a_view)
			return true;
		const auto texture = FindShell(a_view);
		return !texture || texture->baseReady.load(std::memory_order_acquire);
	}

	void RequestBase(const ID3D11ShaderResourceView* a_view)
	{
		if (!Enabled() || !a_view)
			return;
		const auto texture = FindShell(a_view);
		if (texture && !texture->baseReady.load(std::memory_order_acquire) && !texture->baseRequested.exchange(true, std::memory_order_acq_rel))
			PostBuild(*texture, kBaseRevision, kBaseTop);
	}

	bool PinFull(const ID3D11ShaderResourceView* a_view)
	{
		if (!Enabled() || !a_view)
			return true;
		const auto texture = FindShell(a_view);
		if (!texture || texture->failed.load(std::memory_order_relaxed) || texture->residentTop.load(std::memory_order_acquire) == 0)
			return true;
		if (!texture->pinned.exchange(true, std::memory_order_acq_rel))
			PostBuild(*texture, texture->nextRevision.fetch_add(1, std::memory_order_relaxed), 0);
		return false;
	}

	void BuildWindow(TextureId a_id, std::uint32_t a_top)
	{
		// A test's slow disk: what a late window looks like.
		static const std::uint32_t delay = SwitchNumber("CS_DCLF_TEX_STREAM_DELAY_MS", 0);
		if (delay)
			std::this_thread::sleep_for(std::chrono::milliseconds(delay));
		if (const auto texture = Find(a_id))
			Build(*texture, a_top);
	}

	void SetScheduler(Scheduler a_scheduler)
	{
		std::unique_lock lock(g_hooksMutex);
		g_scheduler = std::move(a_scheduler);
	}

	void SetWindowListener(std::function<void(const ID3D11ShaderResourceView*)> a_listener)
	{
		std::unique_lock lock(g_hooksMutex);
		g_windowListener = std::move(a_listener);
	}

	void SetDemandSource(std::function<void(DemandSink&)> a_source)
	{
		std::unique_lock lock(g_hooksMutex);
		g_demandSource = std::move(a_source);
	}

	void NoteFeedback(std::uint64_t a_reported, std::uint64_t a_finer)
	{
		g_feedbackReported.fetch_add(a_reported, std::memory_order_relaxed);
		g_feedbackFiner.fetch_add(a_finer, std::memory_order_relaxed);
	}

	std::uint64_t WorldChanges()
	{
		return g_worldChanges.load(std::memory_order_relaxed);
	}

	bool Describe(const ID3D11ShaderResourceView* a_view, std::string& a_path, std::uint32_t& a_residentTop, std::uint32_t& a_wantedTop)
	{
		const auto texture = a_view ? FindShell(a_view) : nullptr;
		if (!texture)
			return false;
		a_path = texture->path;
		a_residentTop = texture->residentTop.load(std::memory_order_acquire);
		a_wantedTop = texture->wantedTop;
		return true;
	}

	void RequestDump(std::string a_path)
	{
		std::scoped_lock lock(g_dumpMutex);
		g_dumpPath = std::move(a_path);
	}

	void ForceTop(int a_top)
	{
		g_forceTop.store(a_top, std::memory_order_relaxed);
	}

	Stats GetStats()
	{
		Stats stats;
		{
			std::shared_lock lock(g_mutex);
			stats.textures = g_textures.size();
			for (const auto& [id, texture] : g_textures)
				stats.pinned += texture->pinned.load(std::memory_order_relaxed) ? 1 : 0;
		}
		stats.withWindow = g_withWindow.load(std::memory_order_relaxed);
		stats.residentBytes = g_residentBytes.load(std::memory_order_relaxed);
		stats.wantedBytes = g_wantedBytes.load(std::memory_order_relaxed);
		stats.fullBytes = g_fullBytes.load(std::memory_order_relaxed);
		stats.windowsBuilt = g_windowsBuilt.load(std::memory_order_relaxed);
		stats.upgrades = g_upgrades.load(std::memory_order_relaxed);
		stats.downgrades = g_downgrades.load(std::memory_order_relaxed);
		stats.pressureDowngrades = g_pressureDowngrades.load(std::memory_order_relaxed);
		stats.bytesRead = g_bytesRead.load(std::memory_order_relaxed);
		stats.loadFailures = g_loadFailures.load(std::memory_order_relaxed);
		stats.safetyNetLoads = g_safetyNetLoads.load(std::memory_order_relaxed);
		stats.storageFills = g_storageFills.load(std::memory_order_relaxed);
		stats.passedThrough = g_passedThrough.load(std::memory_order_relaxed);
		stats.engineFailures = g_engineFailures.load(std::memory_order_relaxed);
		stats.noted = g_noted.load(std::memory_order_relaxed);
		stats.nativeFull = g_nativeFull.load(std::memory_order_relaxed);
		stats.feedbackReported = g_feedbackReported.load(std::memory_order_relaxed);
		stats.feedbackFiner = g_feedbackFiner.load(std::memory_order_relaxed);
		stats.baseBuilds = g_baseBuilds.load(std::memory_order_relaxed);
		stats.baseReadMicroseconds = g_baseReadMicros.load(std::memory_order_relaxed);
		stats.baseCreateMicroseconds = g_baseCreateMicros.load(std::memory_order_relaxed);
		stats.otherBuilds = g_otherBuilds.load(std::memory_order_relaxed);
		stats.otherReadMicroseconds = g_otherReadMicros.load(std::memory_order_relaxed);
		stats.otherCreateMicroseconds = g_otherCreateMicros.load(std::memory_order_relaxed);
		stats.cardBytes = g_cardBytes.load(std::memory_order_relaxed);
		stats.driverBudgetBytes = g_driverBudgetBytes.load(std::memory_order_relaxed);
		stats.budgetBytes = g_budgetBytes.load(std::memory_order_relaxed);
		stats.usedBytes = g_usedBytes.load(std::memory_order_relaxed);
		stats.overflowBytes = g_overflowBytes.load(std::memory_order_relaxed);
		stats.otherBytes = g_otherBytes.load(std::memory_order_relaxed);
		stats.outsideBytes = g_outsideBytes.load(std::memory_order_relaxed);
		stats.allowanceBytes = g_allowanceBytes.load(std::memory_order_relaxed);
		stats.visibleBytes = g_visibleBytes.load(std::memory_order_relaxed);
		stats.distancePullback = g_distancePullback.load(std::memory_order_relaxed);
		stats.visiblePullback = g_visiblePullback.load(std::memory_order_relaxed);
		return stats;
	}
}
