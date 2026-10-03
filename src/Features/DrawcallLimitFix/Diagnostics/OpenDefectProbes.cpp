#include "OpenDefectProbes.h"

#include <DirectXPackedVector.h>
#include <map>

#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/VolumetricShadows.h"
#include "State.h"

namespace DCLF
{
/**
 * CS_DCLF_SHADOWMASK_PROBE=x,y: the sun's shadow mask (kSHADOW_MASK) at a screen pixel, read at the start of
 * the main pass, where the engine has built it and before anything samples it. Every 240 frames, with DCLF
 * running or not, so one run with CS_DCLF_TEST_TOGGLE gives both sides. The pixel is in the main target's
 * coordinates and scaled to the mask's size (iShadowMaskQuarter).
 */
/**
 * CS_DCLF_SHADOWMASK_PROBE=flicker: the sun's shadow mask every frame (a ring of staging copies, read three frames later):
 * the share of its texels (every 4th in each direction) whose first channel moved by more than half since the frame before,
 * the frame's flicker. Every 300 frames its mean, 99th percentile and maximum, the frames above 2%, and the worst few.
 */
void ProbeShadowMaskFlicker(bool a_running)
{
	struct Slot
	{
		winrt::com_ptr<ID3D11Texture2D> staging;
		std::uint32_t frame = 0;
		bool running = false, pending = false;
	};
	static std::array<Slot, 4> ring;
	static std::uint32_t frame = 0;
	static std::vector<float> previous;
	static std::uint32_t previousFrame = ~0u;
	static std::vector<float> flickers;
	static std::vector<std::pair<float, std::uint32_t>> worst;
	static std::uint32_t onFrames = 0;
	static bool formatLogged = false;
	auto* context = globals::d3d::context;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	auto* mask = reinterpret_cast<ID3D11Texture2D*>(targets[RE::RENDER_TARGETS::kSHADOW_MASK].texture);
	if (!mask)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	mask->GetDesc(&desc);
	auto& slot = ring[frame % ring.size()];
	// The slot's last copy, three frames old.
	if (slot.pending) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(slot.staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
			std::vector<float> texels;
			texels.reserve(std::size_t(desc.Width / 4 + 1) * (desc.Height / 4 + 1));
			for (std::uint32_t row = 0; row < desc.Height; row += 4) {
				const auto* line = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row) * mapped.RowPitch;
				for (std::uint32_t column = 0; column < desc.Width; column += 4) {
					float value = 0.0f;
					switch (desc.Format) {
					case DXGI_FORMAT_R16G16B16A16_FLOAT:
					case DXGI_FORMAT_R16G16_FLOAT:
					case DXGI_FORMAT_R16_FLOAT:
						{
							const std::size_t stride = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : desc.Format == DXGI_FORMAT_R16G16_FLOAT ? 4 : 2;
							std::uint16_t h;
							std::memcpy(&h, line + column * stride, 2);
							value = DirectX::PackedVector::XMConvertHalfToFloat(h);
							break;
						}
					case DXGI_FORMAT_R8_UNORM:
						value = line[column] / 255.0f;
						break;
					default:  // 4-byte texels, the first byte the first channel (RGBA8 and the like)
						value = line[column * 4] / 255.0f;
						break;
					}
					texels.push_back(value);
				}
			}
			context->Unmap(slot.staging.get(), 0);
			if (!formatLogged) {
				formatLogged = true;
				logger::info("[DCLF] shadow mask flicker: mask {}x{} format {}", desc.Width, desc.Height, static_cast<std::uint32_t>(desc.Format));
			}
			if (previousFrame + 1 == slot.frame && previous.size() == texels.size()) {
				std::size_t moved = 0;
				for (std::size_t t = 0; t < texels.size(); ++t)
					moved += std::abs(texels[t] - previous[t]) > 0.5f ? 1 : 0;
				const float flicker = 100.0f * float(moved) / float(texels.size());
				flickers.push_back(flicker);
				onFrames += slot.running ? 1 : 0;
				worst.emplace_back(flicker, slot.frame);
				std::sort(worst.begin(), worst.end(), std::greater<>());
				if (worst.size() > 5)
					worst.resize(5);
				if (flickers.size() == 300) {
					auto sorted = flickers;
					std::sort(sorted.begin(), sorted.end());
					double sum = 0;
					std::size_t spikes = 0;
					for (const float f : sorted) {
						sum += f;
						spikes += f > 2.0f ? 1 : 0;
					}
					std::string worstText;
					for (const auto& [f, at] : worst)
						worstText += fmt::format(" {:.1f}% at {};", f, at);
					logger::info("[DCLF] shadow mask flicker over 300 frames (DCLF on {}): texels moved by > 0.5 since the frame before: mean {:.2f}%, 99th {:.2f}%, max {:.2f}%, {} frames above 2%; worst:{}",
						onFrames, sum / 300.0, sorted[296], sorted.back(), spikes, worstText);
					flickers.clear();
					worst.clear();
					onFrames = 0;
				}
			}
			previous = std::move(texels);
			previousFrame = slot.frame;
		}
		slot.pending = false;
	}
	if (!slot.staging) {
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.MipLevels = stagingDesc.ArraySize = 1;
		stagingDesc.SampleDesc = { 1, 0 };
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.MiscFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, slot.staging.put())))
			return;
	}
	context->CopySubresourceRegion(slot.staging.get(), 0, 0, 0, 0, mask, 0, nullptr);
	slot.frame = frame;
	slot.running = a_running;
	slot.pending = true;
	++frame;
}

void ProbeShadowMask(bool a_running)
{
	const std::string& pixel = DCLF::SwitchValue(DCLF::Switch::ShadowMaskProbe);
	if (pixel.empty())
		return;
	if (pixel == "flicker")
		return ProbeShadowMaskFlicker(a_running);
	static winrt::com_ptr<ID3D11Texture2D> staging;
	static std::uint32_t framesLeft = 0, frames = 0, x = 0, y = 0, bytes = 0;
	static bool stagingRunning = false;
	static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	auto* context = globals::d3d::context;
	if (staging) {
		if (--framesLeft)
			return;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
			std::string hex;
			for (std::uint32_t b = 0; b < std::min(bytes, mapped.RowPitch); ++b)
				hex += fmt::format("{:02X}", static_cast<const std::uint8_t*>(mapped.pData)[b]);
			logger::info("[DCLF] shadow mask at mask texel ({}, {}), DCLF {}: {} (format {})", x, y, stagingRunning ? "on" : "off", hex,
				static_cast<std::uint32_t>(format));
			context->Unmap(staging.get(), 0);
		}
		staging = nullptr;
		return;
	}
	if ((frames++ % 240) != 0)
		return;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	auto* mask = reinterpret_cast<ID3D11Texture2D*>(targets[RE::RENDER_TARGETS::kSHADOW_MASK].texture);
	auto* main = reinterpret_cast<ID3D11Texture2D*>(targets[RE::RENDER_TARGETS::kMAIN].texture);
	if (!mask || !main)
		return;
	D3D11_TEXTURE2D_DESC maskDesc{}, mainDesc{};
	mask->GetDesc(&maskDesc);
	main->GetDesc(&mainDesc);
	const auto sep = pixel.find_first_of(",x");
	if (sep == std::string::npos)
		return;
	// Pixels, or fractions of the screen when both are at most 1.
	double px = std::strtod(pixel.substr(0, sep).c_str(), nullptr);
	double py = std::strtod(pixel.substr(sep + 1).c_str(), nullptr);
	if (px <= 1.0 && py <= 1.0) {
		px *= mainDesc.Width;
		py *= mainDesc.Height;
	}
	x = std::min<std::uint32_t>(static_cast<std::uint32_t>(px * maskDesc.Width / mainDesc.Width), maskDesc.Width - 1);
	y = std::min<std::uint32_t>(static_cast<std::uint32_t>(py * maskDesc.Height / mainDesc.Height), maskDesc.Height - 1);
	D3D11_TEXTURE2D_DESC desc = maskDesc;
	desc.Width = desc.Height = 1;
	desc.MipLevels = desc.ArraySize = 1;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.MiscFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, staging.put())))
		return;
	const D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
	context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, mask, 0, &box);
	format = maskDesc.Format;
	bytes = 8;  // the texel and whatever follows it in the row; the format says how many are the texel
	stagingRunning = a_running;
	framesLeft = 4;
}

/**
 * CS_DCLF_SHADOWMAP_PROBE=1: what the frame's directional shadow maps hold, read at the start of the main
 * pass: per slice of the engine's cascade texture (kSHADOWMAPS_ESRAM), the mean depth and the share of
 * texels at the clear value (no caster), the same per slice of the volumetric lighting copy
 * (kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM), and per mip of Volumetric Shadows' copy (one cascade each, the
 * nearer of the two maps) the mean first moment. Every 240 frames, with DCLF running or not.
 */
void ProbeShadowMaps(bool a_running)
{
	const bool enabled = DCLF::SwitchEnabled(DCLF::Switch::ShadowMapProbe);
	if (!enabled)
		return;
	struct Pending
	{
		winrt::com_ptr<ID3D11Texture2D> depth, volumetric, vsm;
		D3D11_TEXTURE2D_DESC depthDesc{}, volumetricDesc{}, vsmDesc{};
		bool running = false;
		std::uint32_t framesLeft = 0;
	};
	static std::optional<Pending> pending;
	static std::uint32_t frames = 0;
	// The previous probe's sampled texels, per label and slice, and whether DCLF ran then: each probe is diffed against it.
	static std::map<std::string, std::vector<float>> previous;
	static bool previousRunning = false;
	auto* context = globals::d3d::context;
	if (pending) {
		if (--pending->framesLeft)
			return;
		std::string text;
		auto summarise = [&](ID3D11Texture2D* a_texture, const D3D11_TEXTURE2D_DESC& desc, const char* a_label) {
			if (!a_texture)
				return;
			for (std::uint32_t slice = 0; slice < desc.ArraySize; ++slice) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const auto sub = D3D11CalcSubresource(0, slice, desc.MipLevels);
				if (FAILED(context->Map(a_texture, sub, D3D11_MAP_READ, 0, &mapped)))
					continue;
				double sum = 0;
				std::uint64_t cleared = 0, count = 0;
				std::vector<float> texels;
				texels.reserve(std::size_t(desc.Width / 4 + 1) * (desc.Height / 4 + 1));
				for (std::uint32_t row = 0; row < desc.Height; row += 4) {
					const auto* line = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row) * mapped.RowPitch;
					for (std::uint32_t column = 0; column < desc.Width; column += 4) {
						double depth = 0;
						if (desc.Format == DXGI_FORMAT_R16_TYPELESS) {
							depth = reinterpret_cast<const std::uint16_t*>(line)[column] / 65535.0;
						} else {
							const std::uint32_t word = reinterpret_cast<const std::uint32_t*>(line)[column];
							depth = desc.Format == DXGI_FORMAT_R24G8_TYPELESS ? double(word & 0xFFFFFFu) / 16777215.0 : double(std::bit_cast<float>(word));
						}
						sum += depth;
						cleared += depth >= 0.99999 ? 1 : 0;
						++count;
						texels.push_back(static_cast<float>(depth));
					}
				}
				context->Unmap(a_texture, sub);
				text += fmt::format(" {}slice {}: mean {:.5f}, {:.1f}% clear", a_label, slice, sum / double(count), 100.0 * double(cleared) / double(count));
				// Against the previous probe, texel by texel: the share differing by more than 1e-3, and how many of those only
				// one side has a caster at (the other clear).
				auto& before = previous[fmt::format("{}{}", a_label, slice)];
				if (before.size() == texels.size()) {
					std::uint64_t differ = 0, oneSided = 0;
					double largest = 0;
					for (std::size_t t = 0; t < texels.size(); ++t) {
						const double d = std::abs(double(texels[t]) - double(before[t]));
						if (d > 1e-3) {
							++differ;
							oneSided += (texels[t] >= 0.99999f) != (before[t] >= 0.99999f) ? 1 : 0;
						}
						largest = std::max(largest, d);
					}
					text += fmt::format(" (against the last probe, DCLF {}: {:.2f}% differ, {:.2f}% caster on one side only, largest {:.4f})",
						previousRunning == pending->running ? "unchanged" : pending->running ? "off -> on" : "on -> off", 100.0 * double(differ) / double(texels.size()),
						100.0 * double(oneSided) / double(texels.size()), largest);
				}
				before = std::move(texels);
				text += ";";
			}
		};
		summarise(pending->depth.get(), pending->depthDesc, "");
		summarise(pending->volumetric.get(), pending->volumetricDesc, "volumetric ");
		if (pending->vsm) {
			const auto& desc = pending->vsmDesc;
			for (std::uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(pending->vsm.get(), mip, D3D11_MAP_READ, 0, &mapped)))
					continue;
				const std::uint32_t width = std::max(1u, desc.Width >> mip), height = std::max(1u, desc.Height >> mip);
				double sum = 0;
				for (std::uint32_t row = 0; row < height; ++row) {
					const auto* texels = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row) * mapped.RowPitch);
					for (std::uint32_t column = 0; column < width; ++column)
						sum += texels[column * 2] / 65535.0;
				}
				context->Unmap(pending->vsm.get(), mip);
				text += fmt::format(" VSM mip {}: mean {:.5f};", mip, sum / double(width * height));
			}
		}
		logger::info("[DCLF] shadow maps, DCLF {} (depth format {}):{}", pending->running ? "on" : "off", static_cast<std::uint32_t>(pending->depthDesc.Format), text);
		previousRunning = pending->running;
		pending.reset();
		return;
	}
	if ((frames++ % 240) != 0)
		return;
	Pending next;
	next.running = a_running;
	next.framesLeft = 4;
	auto copy = [&](ID3D11Texture2D* a_source, winrt::com_ptr<ID3D11Texture2D>& a_staging, D3D11_TEXTURE2D_DESC& a_desc) {
		if (!a_source)
			return;
		a_source->GetDesc(&a_desc);
		D3D11_TEXTURE2D_DESC desc = a_desc;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (SUCCEEDED(globals::d3d::device->CreateTexture2D(&desc, nullptr, a_staging.put())))
			context->CopyResource(a_staging.get(), a_source);
	};
	const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
	copy(reinterpret_cast<ID3D11Texture2D*>(depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].texture), next.depth, next.depthDesc);
	copy(reinterpret_cast<ID3D11Texture2D*>(depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM].texture), next.volumetric,
		next.volumetricDesc);
	if (globals::features::volumetricShadows.loaded)
		copy(globals::features::volumetricShadows.shadowCopyTexture, next.vsm, next.vsmDesc);
	pending = std::move(next);
}
}

void DrawcallLimitFix::ProbeOpaqueTarget(bool a_afterDCLF)
{
	const std::string& pixel = DCLF::SwitchValue(DCLF::Switch::TargetProbe);
	if (pixel.empty())
		return;
	constexpr std::uint32_t kTargets = 8;
	constexpr std::uint32_t kBlock = 64;  // the mean over a block: one pixel is too noisy under TAA jitter
	struct Slot
	{
		std::array<winrt::com_ptr<ID3D11Texture2D>, kTargets> staging;
		std::array<DXGI_FORMAT, kTargets> format{};
		bool running = false;
	};
	static std::array<Slot, 2> slots;  // before and after DCLF's colour epoch
	static std::uint32_t frames = 0, framesLeft = 0;
	auto* context = globals::d3d::context;
	auto half = [](std::uint16_t a_h) {
		const std::uint32_t sign = (a_h >> 15) & 1, exponent = (a_h >> 10) & 0x1F, mantissa = a_h & 0x3FF;
		float value = exponent == 0 ? std::ldexp(float(mantissa), -24) : exponent == 31 ? std::numeric_limits<float>::infinity() : std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
		return sign ? -value : value;
	};
	if (!a_afterDCLF) {
		if (framesLeft && --framesLeft == 0) {
			for (std::uint32_t i = 0; i < 2; ++i) {
				auto& slot = slots[i];
				std::string text;
				for (std::uint32_t t = 0; t < kTargets; ++t) {
					if (!slot.staging[t])
						continue;
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (SUCCEEDED(context->Map(slot.staging[t].get(), 0, D3D11_MAP_READ, 0, &mapped))) {
						// The block's mean per channel.
						auto f11 = [](std::uint32_t a_bits, std::uint32_t a_mantissaBits) {
							const std::uint32_t exponent = a_bits >> a_mantissaBits, mantissa = a_bits & ((1u << a_mantissaBits) - 1);
							return exponent == 0 ? std::ldexp(float(mantissa), -14 - int(a_mantissaBits)) : std::ldexp(1.0f + float(mantissa) / float(1u << a_mantissaBits), int(exponent) - 15);
						};
						std::array<double, 4> sum{};
						std::uint32_t channels = 0;
						for (std::uint32_t row = 0; row < kBlock; ++row) {
							const auto* line = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row) * mapped.RowPitch;
							for (std::uint32_t column = 0; column < kBlock; ++column) {
								switch (slot.format[t]) {
								case DXGI_FORMAT_R16G16B16A16_FLOAT:
									for (std::uint32_t c = 0; c < 4; ++c)
										sum[c] += half(reinterpret_cast<const std::uint16_t*>(line)[column * 4 + c]);
									channels = 4;
									break;
								case DXGI_FORMAT_R16G16_FLOAT:
									for (std::uint32_t c = 0; c < 2; ++c)
										sum[c] += half(reinterpret_cast<const std::uint16_t*>(line)[column * 2 + c]);
									channels = 2;
									break;
								case DXGI_FORMAT_R10G10B10A2_UNORM: {
									const std::uint32_t w = reinterpret_cast<const std::uint32_t*>(line)[column];
									sum[0] += (w & 1023) / 1023.0;
									sum[1] += ((w >> 10) & 1023) / 1023.0;
									sum[2] += ((w >> 20) & 1023) / 1023.0;
									sum[3] += (w >> 30) / 3.0;
									channels = 4;
									break;
								}
								case DXGI_FORMAT_R11G11B10_FLOAT: {
									const std::uint32_t w = reinterpret_cast<const std::uint32_t*>(line)[column];
									sum[0] += f11(w & 2047, 6);
									sum[1] += f11((w >> 11) & 2047, 6);
									sum[2] += f11(w >> 22, 5);
									channels = 3;
									break;
								}
								case DXGI_FORMAT_R16_UNORM:
									sum[0] += reinterpret_cast<const std::uint16_t*>(line)[column] / 65535.0;
									channels = 1;
									break;
								case DXGI_FORMAT_R8G8B8A8_UNORM:
									for (std::uint32_t c = 0; c < 4; ++c)
										sum[c] += line[column * 4 + c] / 255.0;
									channels = 4;
									break;
								default:
									break;
								}
							}
						}
						const double n = double(kBlock) * kBlock;
						if (channels == 0)
							text += fmt::format(" rt{}=f{}", t, static_cast<std::uint32_t>(slot.format[t]));
						else if (channels == 1)
							text += fmt::format(" rt{}=({:.4f})", t, sum[0] / n);
						else if (channels == 2)
							text += fmt::format(" rt{}=({:.4f} {:.4f})", t, sum[0] / n, sum[1] / n);
						else if (channels == 3)
							text += fmt::format(" rt{}=({:.4f} {:.4f} {:.4f})", t, sum[0] / n, sum[1] / n, sum[2] / n);
						else
							text += fmt::format(" rt{}=({:.4f} {:.4f} {:.4f} {:.4f})", t, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
						context->Unmap(slot.staging[t].get(), 0);
					}
					slot.staging[t] = nullptr;
				}
				logger::info("[DCLF] target probe: targets {} DCLF's colour epoch, DCLF {}:{}", i ? "after " : "before", slot.running ? "on" : "off", text);
			}
		}
		if ((frames++ % 240) != 0)
			return;
		framesLeft = 4;
	} else if (framesLeft != 4) {
		return;
	}
	// The epoch unbinds the targets, so the read after it reuses the textures the read before it found.
	static std::array<winrt::com_ptr<ID3D11Texture2D>, kTargets> textures;
	if (!a_afterDCLF) {
		ID3D11RenderTargetView* views[kTargets] = {};
		context->OMGetRenderTargets(kTargets, views, nullptr);
		for (std::uint32_t t = 0; t < kTargets; ++t) {
			textures[t] = nullptr;
			if (!views[t])
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			views[t]->GetResource(resource.put());
			views[t]->Release();
			textures[t] = resource.try_as<ID3D11Texture2D>();
		}
	}
	const auto sep = pixel.find_first_of(",x");
	auto& slot = slots[a_afterDCLF ? 1 : 0];
	slot.running = Running();
	for (std::uint32_t t = 0; t < kTargets; ++t) {
		slot.staging[t] = nullptr;
		if (!textures[t])
			continue;
		D3D11_TEXTURE2D_DESC desc{};
		textures[t]->GetDesc(&desc);
		const auto x = std::min<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10), desc.Width - kBlock);
		const auto y = std::min<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10), desc.Height - kBlock);
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Width = stagingDesc.Height = kBlock;
		stagingDesc.MipLevels = stagingDesc.ArraySize = 1;
		stagingDesc.SampleDesc = { 1, 0 };
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = stagingDesc.MiscFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, slot.staging[t].put())))
			continue;
		const D3D11_BOX box{ x, y, 0, x + kBlock, y + kBlock, 1 };
		context->CopySubresourceRegion(slot.staging[t].get(), 0, 0, 0, 0, textures[t].get(), 0, &box);
		slot.format[t] = desc.Format;
	}
}
