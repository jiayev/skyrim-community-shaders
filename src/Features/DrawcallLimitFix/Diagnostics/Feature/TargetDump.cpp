// CS_DCLF_TARGET_DUMP: staging copies at the frame's target points, written as PNGs and statistics once readable. Render thread.
#include "TargetDump.h"
#include "Features/Upscaling.h"

#include <DirectXPackedVector.h>
#include <DirectXTex.h>
#include <cmath>
#include <array>
#include <deque>
#include <filesystem>
#include <fstream>

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

namespace DCLF::TargetDump
{
	namespace
	{
		// One copied target: its staging texture, what it was, where it was copied.
		struct Copy
		{
			winrt::com_ptr<ID3D11Texture2D> staging;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t width = 0, height = 0;
			std::string name;  // "<order>-<point>-<slot>"
		};

		// A dumping frame's copies, written once all are readable.
		struct Frame
		{
			std::uint32_t count = 0;
			std::vector<Copy> copies;
			std::vector<std::string> points;  // reached, in order
			std::uint32_t presentsWaited = 0;
		};

		std::uint32_t presents = 0;
		std::vector<std::uint32_t> wanted;  // the Present counts that dump
		bool parsed = false;
		std::optional<Frame> current;       // the frame whose points are copying
		std::deque<Frame> written;          // frames waiting for their copies

		void Parse()
		{
			if (std::exchange(parsed, true))
				return;
			const auto& value = SwitchValue(Switch::TargetDump);
			for (std::size_t at = 0; at < value.size();) {
				const auto end = std::min(value.find(',', at), value.size());
				if (end > at)
					wanted.push_back(static_cast<std::uint32_t>(std::strtoul(value.substr(at, end - at).c_str(), nullptr, 10)));
				at = end + 1;
			}
		}

		std::filesystem::path Directory(std::uint32_t a_count)
		{
			auto dir = logger::log_directory().value_or(std::filesystem::path{});
			return dir / "dclf-dump" / std::to_string(a_count);
		}

		void Stage(ID3D11Texture2D* a_texture, const std::string& a_name, Frame& a_frame)
		{
			if (!a_texture)
				return;
			D3D11_TEXTURE2D_DESC desc{};
			a_texture->GetDesc(&desc);
			if (desc.SampleDesc.Count != 1)
				return;
			D3D11_TEXTURE2D_DESC staging = desc;
			staging.MipLevels = staging.ArraySize = 1;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.BindFlags = staging.MiscFlags = 0;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Copy copy;
			if (FAILED(globals::d3d::device->CreateTexture2D(&staging, nullptr, copy.staging.put())))
				return;
			// Subresource 0 (mip 0, slice 0); a depth buffer copies whole, as D3D11 requires.
			globals::d3d::context->CopySubresourceRegion(copy.staging.get(), 0, 0, 0, 0, a_texture, 0, nullptr);
			copy.format = desc.Format;
			copy.width = desc.Width;
			copy.height = desc.Height;
			copy.name = a_name;
			a_frame.copies.push_back(std::move(copy));
		}

		/** @brief Each slice of a texture array (at most 16), as Stage copies slice 0: a whole subresource, as a depth array requires. */
		void StageSlices(ID3D11Texture2D* a_texture, const std::string& a_name, Frame& a_frame)
		{
			if (!a_texture)
				return;
			D3D11_TEXTURE2D_DESC desc{};
			a_texture->GetDesc(&desc);
			if (desc.SampleDesc.Count != 1)
				return;
			D3D11_TEXTURE2D_DESC staging = desc;
			staging.MipLevels = staging.ArraySize = 1;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.BindFlags = staging.MiscFlags = 0;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (std::uint32_t slice = 0; slice < std::min(desc.ArraySize, 16u); ++slice) {
				Copy copy;
				if (FAILED(globals::d3d::device->CreateTexture2D(&staging, nullptr, copy.staging.put())))
					return;
				globals::d3d::context->CopySubresourceRegion(copy.staging.get(), 0, 0, 0, 0, a_texture, D3D11CalcSubresource(0, slice, desc.MipLevels), nullptr);
				copy.format = desc.Format;
				copy.width = desc.Width;
				copy.height = desc.Height;
				copy.name = fmt::format("{}-s{}", a_name, slice);
				a_frame.copies.push_back(std::move(copy));
			}
		}

		ID3D11Texture2D* TextureOf(ID3D11View* a_view)
		{
			if (!a_view)
				return nullptr;
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			return resource.try_as<ID3D11Texture2D>().get();  // the view keeps the texture alive while it is bound
		}

		// A texel as up to four floats (NaN/Inf kept), or false for a format this does not decode.
		bool Decode(DXGI_FORMAT a_format, const std::uint8_t* a_row, std::uint32_t a_x, float (&a_out)[4], std::uint32_t& a_channels)
		{
			using namespace DirectX::PackedVector;
			a_out[0] = a_out[1] = a_out[2] = 0.0f;
			a_out[3] = 1.0f;
			switch (a_format) {
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				for (std::uint32_t c = 0; c < 4; ++c)
					a_out[c] = XMConvertHalfToFloat(reinterpret_cast<const HALF*>(a_row)[a_x * 4 + c]);
				a_channels = 4;
				return true;
			case DXGI_FORMAT_R16G16_FLOAT:
				for (std::uint32_t c = 0; c < 2; ++c)
					a_out[c] = XMConvertHalfToFloat(reinterpret_cast<const HALF*>(a_row)[a_x * 2 + c]);
				a_channels = 2;
				return true;
			case DXGI_FORMAT_R16_FLOAT:
				a_out[0] = XMConvertHalfToFloat(reinterpret_cast<const HALF*>(a_row)[a_x]);
				a_channels = 1;
				return true;
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
				for (std::uint32_t c = 0; c < 4; ++c)
					a_out[c] = reinterpret_cast<const float*>(a_row)[a_x * 4 + c];
				a_channels = 4;
				return true;
			case DXGI_FORMAT_R32G32_FLOAT:
				for (std::uint32_t c = 0; c < 2; ++c)
					a_out[c] = reinterpret_cast<const float*>(a_row)[a_x * 2 + c];
				a_channels = 2;
				return true;
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
				a_out[0] = reinterpret_cast<const float*>(a_row)[a_x];
				a_channels = 1;
				return true;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				a_out[0] = reinterpret_cast<const float*>(a_row)[a_x * 2];
				a_channels = 1;
				return true;
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				a_out[0] = (reinterpret_cast<const std::uint32_t*>(a_row)[a_x] & 0xFFFFFFu) / 16777215.0f;
				a_channels = 1;
				return true;
			case DXGI_FORMAT_R11G11B10_FLOAT: {
				XMFLOAT3PK packed;
				packed.v = reinterpret_cast<const std::uint32_t*>(a_row)[a_x];
				const auto v = XMLoadFloat3PK(&packed);
				a_out[0] = DirectX::XMVectorGetX(v);
				a_out[1] = DirectX::XMVectorGetY(v);
				a_out[2] = DirectX::XMVectorGetZ(v);
				a_channels = 3;
				return true;
			}
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
				const std::uint32_t w = reinterpret_cast<const std::uint32_t*>(a_row)[a_x];
				a_out[0] = (w & 1023) / 1023.0f;
				a_out[1] = ((w >> 10) & 1023) / 1023.0f;
				a_out[2] = ((w >> 20) & 1023) / 1023.0f;
				a_out[3] = (w >> 30) / 3.0f;
				a_channels = 4;
				return true;
			}
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				for (std::uint32_t c = 0; c < 4; ++c)
					a_out[c] = a_row[a_x * 4 + c] / 255.0f;
				a_channels = 4;
				return true;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				a_out[0] = a_row[a_x * 4 + 2] / 255.0f;
				a_out[1] = a_row[a_x * 4 + 1] / 255.0f;
				a_out[2] = a_row[a_x * 4 + 0] / 255.0f;
				a_out[3] = a_row[a_x * 4 + 3] / 255.0f;
				a_channels = 4;
				return true;
			case DXGI_FORMAT_R8G8_UNORM:
				a_out[0] = a_row[a_x * 2] / 255.0f;
				a_out[1] = a_row[a_x * 2 + 1] / 255.0f;
				a_channels = 2;
				return true;
			case DXGI_FORMAT_R8_UNORM:
				a_out[0] = a_row[a_x] / 255.0f;
				a_channels = 1;
				return true;
			case DXGI_FORMAT_R16G16_UNORM:
				for (std::uint32_t c = 0; c < 2; ++c)
					a_out[c] = reinterpret_cast<const std::uint16_t*>(a_row)[a_x * 2 + c] / 65535.0f;
				a_channels = 2;
				return true;
			case DXGI_FORMAT_R16G16_SNORM:
				for (std::uint32_t c = 0; c < 2; ++c)
					a_out[c] = std::max(-1.0f, reinterpret_cast<const std::int16_t*>(a_row)[a_x * 2 + c] / 32767.0f);
				a_channels = 2;
				return true;
			case DXGI_FORMAT_R16_UNORM:
				a_out[0] = reinterpret_cast<const std::uint16_t*>(a_row)[a_x] / 65535.0f;
				a_channels = 1;
				return true;
			default:
				return false;
			}
		}

		/**
		 * @brief The local shadow mask's agreement on a DCLF frame: the engine draws each light's channel for its registered receivers
		 * before the colour segment (opaque-end), and DCLF's resolve (LocalShadowMaskCS) rewrites every texel of the channels it has a
		 * record for at the segment's start (after-DCLF-colour). Where the engine lit a receiver (> 0.5), DCLF's should not be dark (< 0.5):
		 * those texels per channel, and how many DCLF left dark. Empty without both copies; a DCLF-off frame compares the engine's with itself.
		 */
		std::string MaskAgreement(const Frame& a_frame, const std::vector<D3D11_MAPPED_SUBRESOURCE>& a_mapped, std::size_t a_done)
		{
			std::size_t engine = a_done, dclf = a_done;
			for (std::size_t i = 0; i < a_done; ++i) {
				if (a_frame.copies[i].name.ends_with("-opaque-end-kSHADOWMASK"))
					engine = i;
				else if (a_frame.copies[i].name.ends_with("-after-DCLF-colour-kSHADOWMASK"))
					dclf = i;
			}
			if (engine == a_done || dclf == a_done)
				return {};
			const auto& copy = a_frame.copies[engine];
			std::array<std::uint64_t, 4> lit{}, dark{};
			for (std::uint32_t y = 0; y < copy.height; ++y) {
				const auto* engineRow = static_cast<const std::uint8_t*>(a_mapped[engine].pData) + std::size_t(y) * a_mapped[engine].RowPitch;
				const auto* dclfRow = static_cast<const std::uint8_t*>(a_mapped[dclf].pData) + std::size_t(y) * a_mapped[dclf].RowPitch;
				for (std::uint32_t x = 0; x < copy.width; ++x) {
					float before[4]{}, after[4]{};
					std::uint32_t channels = 0;
					if (!Decode(copy.format, engineRow, x, before, channels) || !Decode(copy.format, dclfRow, x, after, channels))
						return "local shadow mask agreement: the mask's format does not decode";
					for (std::uint32_t c = 0; c < std::min(channels, 4u); ++c)
						if (before[c] > 0.5f) {
							++lit[c];
							dark[c] += after[c] < 0.5f ? 1 : 0;
						}
				}
			}
			std::string out = "local shadow mask agreement (texels the engine lit at opaque-end, of them dark after DCLF's resolve):";
			bool differs = false;
			for (std::uint32_t c = 0; c < 4; ++c) {
				out += fmt::format(" channel {} {} lit, {} dark;", c, lit[c], dark[c]);
				differs = differs || (lit[c] && dark[c] * 10 > lit[c]);
			}
			return out + (differs ? " <- SHADOW MASK RESOLVE" : " <- OK");
		}

		std::uint8_t Shade(float a_value)
		{
			const float v = a_value < 0.0f ? 0.0f : a_value / (1.0f + a_value);  // negative shows as black, the stats keep it
			return static_cast<std::uint8_t>(std::lround(std::min(v, 1.0f) * 255.0f));
		}

		// Writes one mapped copy: a half-size PNG and its statistics line.
		std::string Write(const Copy& a_copy, const D3D11_MAPPED_SUBRESOURCE& a_mapped, const std::filesystem::path& a_dir, bool a_image)
		{
			const std::uint32_t w = std::max(1u, a_copy.width / 2), h = std::max(1u, a_copy.height / 2);
			DirectX::ScratchImage image;
			if (FAILED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1)))
				return a_copy.name + ": no image";
			auto* pixels = image.GetPixels();
			const auto rowPitch = image.GetImages()->rowPitch;
			double sum[4] = {}, minimum[4], maximum[4];
			for (std::uint32_t c = 0; c < 4; ++c) {
				minimum[c] = std::numeric_limits<double>::max();
				maximum[c] = -std::numeric_limits<double>::max();
			}
			std::uint64_t finite = 0, nans = 0, infs = 0;
			std::uint32_t channels = 0;
			bool decoded = true;
			for (std::uint32_t y = 0; y < a_copy.height && decoded; ++y) {
				const auto* row = static_cast<const std::uint8_t*>(a_mapped.pData) + std::size_t(y) * a_mapped.RowPitch;
				for (std::uint32_t x = 0; x < a_copy.width; ++x) {
					float v[4];
					if (!Decode(a_copy.format, row, x, v, channels)) {
						decoded = false;
						break;
					}
					bool bad = false;
					for (std::uint32_t c = 0; c < channels; ++c) {
						if (std::isnan(v[c])) {
							++nans;
							bad = true;
						} else if (std::isinf(v[c])) {
							++infs;
							bad = true;
						}
					}
					if (!bad) {
						++finite;
						for (std::uint32_t c = 0; c < channels; ++c) {
							sum[c] += v[c];
							minimum[c] = std::min<double>(minimum[c], v[c]);
							maximum[c] = std::max<double>(maximum[c], v[c]);
						}
					}
					if ((x & 1) || (y & 1) || x / 2 >= w || y / 2 >= h)
						continue;
					auto* out = pixels + std::size_t(y / 2) * rowPitch + std::size_t(x / 2) * 4;
					if (bad) {
						out[0] = 255, out[1] = 0, out[2] = 255, out[3] = 255;
					} else if (channels == 1) {
						out[0] = out[1] = out[2] = Shade(v[0]), out[3] = 255;
					} else {
						out[0] = Shade(v[0]), out[1] = Shade(v[1]), out[2] = channels > 2 ? Shade(v[2]) : 0, out[3] = 255;
					}
				}
			}
			if (!decoded)
				return fmt::format("{}: format {} not decoded ({}x{})", a_copy.name, static_cast<std::uint32_t>(a_copy.format), a_copy.width, a_copy.height);
			const auto path = a_dir / (a_copy.name + ".png");
			if (a_image)
				DirectX::SaveToWICFile(*image.GetImages(), DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.c_str());
			std::string text = fmt::format("{}: format {} {}x{}; {} NaN, {} Inf channel values; finite texels {}", a_copy.name, static_cast<std::uint32_t>(a_copy.format), a_copy.width,
				a_copy.height, nans, infs, finite);
			for (std::uint32_t c = 0; c < channels && finite; ++c)
				text += fmt::format("; c{} min {:.4g} max {:.4g} mean {:.4g}", c, minimum[c], maximum[c], sum[c] / double(finite));
			return text;
		}
	}

	bool Armed()
	{
		return current.has_value();
	}

	void Point(const char* a_label, ID3D11Texture2D* a_extra)
	{
		if (!current)
			return;
		auto& frame = *current;
		std::string label(a_label);
		std::ranges::replace(label, ' ', '-');
		if (std::ranges::find(frame.points, label) != frame.points.end())
			return;
		frame.points.push_back(label);
		const auto order = fmt::format("{:02}-{}", frame.points.size(), label);
		auto* context = globals::d3d::context;
		ID3D11RenderTargetView* views[8] = {};
		ID3D11DepthStencilView* depthView = nullptr;
		context->OMGetRenderTargets(8, views, &depthView);
		// The frame's G-buffer: the targets of the last point that had several bound (the deferred pass's), staged again at a point with
		// none bound (an epoch unbinds them), so DCLF's colour epoch's writes are seen.
		static std::array<winrt::com_ptr<ID3D11Texture2D>, 8> gbuffer;
		std::uint32_t bound = 0;
		for (std::uint32_t t = 0; t < 8; ++t)
			bound += views[t] ? 1u : 0u;
		if (bound > 2)
			for (std::uint32_t t = 0; t < 8; ++t)
				gbuffer[t].copy_from(TextureOf(views[t]));
		for (std::uint32_t t = 0; t < 8; ++t) {
			Stage(TextureOf(views[t]), fmt::format("{}-rt{}", order, t), frame);
			if (views[t])
				views[t]->Release();
		}
		if (bound == 0)
			for (std::uint32_t t = 0; t < 8; ++t)
				Stage(gbuffer[t].get(), fmt::format("{}-g{}", order, t), frame);
		Stage(TextureOf(depthView), order + "-depth", frame);
		if (depthView)
			depthView->Release();
		// Without an extra: the upscaler's output (what the previous frame's evaluate wrote), at every point.
		if (!a_extra)
			if (auto* upscaled = globals::features::upscaling.upscaledTexture; upscaled && upscaled->resource)
				a_extra = upscaled->resource.get();
		Stage(a_extra, order + "-extra", frame);
		// The renderer's main colour, motion vectors and depth, bound or not (an epoch runs with the context's targets unbound).
		if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
			Stage(reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].texture), order + "-kMOTION", frame);
			Stage(reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture), order + "-kMAIN", frame);
			Stage(reinterpret_cast<ID3D11Texture2D*>(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture), order + "-kMAINdepth",
				frame);
			// The local shadow lights' screen-space masks (the engine's per-receiver passes, DCLF's resolve at the colour epoch's start).
			Stage(reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK].texture), order + "-kSHADOWMASK", frame);
			// The shadow maps, every slice, once a frame (at its Present: the frame's views are drawn, by the engine or DCLF).
			if (label == "present") {
				const auto& depths = renderer->GetDepthStencilData().depthStencils;
				StageSlices(reinterpret_cast<ID3D11Texture2D*>(depths[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].texture), order + "-kSHADOWMAPS_ESRAM", frame);
				StageSlices(reinterpret_cast<ID3D11Texture2D*>(depths[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS].texture), order + "-kSHADOWMAPS", frame);
			}
		}
	}

	void Present()
	{
		Parse();
		if (wanted.empty())
			return;
		// The frame that just ended waits for its copies.
		if (current) {
			written.push_back(std::move(*current));
			current.reset();
		}
		// Counted as the test harness counts its frames (loading screens do not count), so a dump lands where CS_DCLF_TEST_COMMANDS put the
		// camera, with DCLF on or off.
		if (!Scene::FrameView::IsLoadingScreenUp() && std::ranges::find(wanted, ++presents) != wanted.end()) {
			current.emplace();
			current->count = presents;
		}
		// The oldest waiting frame, once every copy maps without waiting (or 60 Presents later, waiting then).
		if (written.empty())
			return;
		auto& frame = written.front();
		auto* context = globals::d3d::context;
		const bool force = ++frame.presentsWaited > 60;
		if (frame.presentsWaited < 3)
			return;
		std::vector<D3D11_MAPPED_SUBRESOURCE> mapped(frame.copies.size());
		std::size_t done = 0;
		for (; done < frame.copies.size(); ++done) {
			const HRESULT result = context->Map(frame.copies[done].staging.get(), 0, D3D11_MAP_READ, force ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[done]);
			if (FAILED(result))
				break;
		}
		if (done < frame.copies.size()) {
			for (std::size_t i = 0; i < done; ++i)
				context->Unmap(frame.copies[i].staging.get(), 0);
			if (!force)
				return;
		}
		const auto dir = Directory(frame.count);
		std::error_code error;
		std::filesystem::create_directories(dir, error);
		std::ofstream stats(dir / "stats.txt");
		stats << "Present " << frame.count << ": points";
		for (const auto& point : frame.points)
			stats << " " << point;
		stats << "\n";
		if (const auto agreement = MaskAgreement(frame, mapped, done); !agreement.empty()) {
			stats << agreement << "\n";
			logger::info("[DCLF] target dump: Present {}: {}", frame.count, agreement);
		}
		for (std::size_t i = 0; i < done; ++i) {
			// Every point's images on the first frame listed; on the others the presented image alone (the statistics of all), so a run of
			// consecutive frames stays small.
			const bool mask = frame.copies[i].name.ends_with("-kSHADOWMASK") || frame.copies[i].name.find("-kSHADOWMAPS") != std::string::npos;
			const bool image = frame.count == wanted.front() || mask || frame.copies[i].name.find("-present-rt0") != std::string::npos;
			stats << Write(frame.copies[i], mapped[i], dir, image) << "\n";
			// The extra target's raw texels (the upscaler's output), row by row, for a byte-level look at what overwrote it.
			// The shadow mask's too, every channel at full size (the PNG drops the fourth and halves it).
			if (mask || (frame.copies[i].name.ends_with("-extra") && (frame.copies[i].name.find("before-upscale") != std::string::npos || frame.copies[i].name.find("after-evaluate") != std::string::npos))) {
				std::ofstream raw(dir / (frame.copies[i].name + ".raw"), std::ios::binary);
				const auto& c = frame.copies[i];
				const std::size_t rowBytes = std::size_t(c.width) * (DirectX::BitsPerPixel(c.format) / 8);
				for (std::uint32_t y = 0; y < c.height; ++y)
					raw.write(static_cast<const char*>(mapped[i].pData) + std::size_t(y) * mapped[i].RowPitch, static_cast<std::streamsize>(rowBytes));
			}
			context->Unmap(frame.copies[i].staging.get(), 0);
		}
		logger::info("[DCLF] target dump: Present {}, {} points, {} targets written to {}", frame.count, frame.points.size(), done, dir.string());
		written.pop_front();
	}
}
