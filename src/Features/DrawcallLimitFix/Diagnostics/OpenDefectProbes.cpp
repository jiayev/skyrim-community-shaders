#include "OpenDefectProbes.h"

#include <DirectXPackedVector.h>
#include <map>

#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Engine/PassCapture.h"
#include "Features/TerrainBlending.h"
#include "Features/VolumetricShadows.h"
#include "State.h"
#include "Deferred.h"

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

/**
 * Under CS_DCLF_TARGET_PROBE: the pixel stage's state as each landscape pass Terrain Blending replays left it (the
 * geometry, its textures, and the contents of the material and geometry constant buffers b1 and b2), every 240
 * frames, with DCLF running or not, so one run with CS_DCLF_TEST_TOGGLE diffs the same geometry both ways.
 */
void DCLF::ProbeTerrainPassState(bool a_running, std::uint32_t a_index, const RE::BSRenderPass* a_pass)
{
	if (DCLF::SwitchValue(DCLF::Switch::TargetProbe).empty())
		return;
	struct Entry
	{
		std::string text;
		std::array<winrt::com_ptr<ID3D11Buffer>, 3> staging;  // PS b1, PS b2, VS b2
	};
	struct Pending
	{
		std::vector<Entry> entries;
		bool running = false;
		std::uint32_t framesLeft = 0;
	};
	static std::optional<Pending> pending;
	static bool capturing = false;
	static std::uint32_t frames = 0;
	auto* context = globals::d3d::context;
	if (a_index == 0) {
		capturing = false;
		if (pending && pending->framesLeft && --pending->framesLeft == 0) {
			std::string text;
			for (auto& entry : pending->entries) {
				text += "\n  " + entry.text;
				for (std::uint32_t b = 0; b < 3; ++b) {
					if (!entry.staging[b])
						continue;
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(entry.staging[b].get(), 0, D3D11_MAP_READ, 0, &mapped)))
						continue;
					D3D11_BUFFER_DESC desc{};
					entry.staging[b]->GetDesc(&desc);
					text += fmt::format("\n    {}:", b == 2 ? "VS b2" : b ? "PS b2" : "PS b1");
					const auto* words = static_cast<const float*>(mapped.pData);
					for (std::uint32_t w = 0; w < desc.ByteWidth / 4; ++w)
						text += fmt::format("{}{:.4g}", w % 4 ? " " : " | ", words[w]);
					context->Unmap(entry.staging[b].get(), 0);
				}
			}
			logger::info("[DCLF] terrain pass state, DCLF {}:{}", pending->running ? "on" : "off", text);
			pending.reset();
		}
		if (!pending && (frames++ % 240) == 0) {
			pending.emplace();
			pending->running = a_running;
			capturing = true;
		}
	}
	if (!capturing)
		return;
	Entry entry;
	const auto* geometry = a_pass ? a_pass->geometry : nullptr;
	const auto* property = a_pass ? a_pass->shaderProperty : nullptr;
	entry.text = fmt::format("pass {} '{}' material {} technique {:08X}", a_index, geometry && geometry->name.c_str() ? geometry->name.c_str() : "",
		property ? static_cast<const void*>(property->material) : nullptr, a_pass ? a_pass->passEnum : 0u);
	if (property) {
		const auto* lighting = static_cast<const RE::BSLightingShaderProperty*>(property);
		const auto* node = property->fadeNode;
		entry.text += fmt::format(" fades specular {} envmap {}; fade node {} '{}' metric {} last visible {} flags {:08X}", lighting->specularLODFade, lighting->envmapLODFade,
			static_cast<const void*>(node), node && node->name.c_str() ? node->name.c_str() : "", node ? Engine::At<float>(node, 0x144) : 0.0f,
			node ? Engine::At<std::int32_t>(node, 0x13C) : 0, node ? Engine::At<std::uint32_t>(node, 0xF4) : 0u);
	}
	std::array<ID3D11ShaderResourceView*, 16> views{};
	context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
	entry.text += " textures:";
	for (std::uint32_t t = 0; t < views.size(); ++t) {
		entry.text += fmt::format(" {}", static_cast<const void*>(views[t]));
		if (views[t])
			views[t]->Release();
	}
	// TruePBR's landscape layers: displacement t80-t85, RMAOS t86-t91.
	std::array<ID3D11ShaderResourceView*, 12> layers{};
	context->PSGetShaderResources(80, static_cast<UINT>(layers.size()), layers.data());
	entry.text += " layers:";
	for (std::uint32_t t = 0; t < layers.size(); ++t) {
		entry.text += fmt::format(" {}", static_cast<const void*>(layers[t]));
		if (layers[t])
			layers[t]->Release();
	}
	// The rest of the pipeline's state, for the first pass only (the same for every one).
	if (a_index == 0) {
		ID3D11BlendState* blend = nullptr;
		float factor[4]{};
		UINT sampleMask = 0;
		context->OMGetBlendState(&blend, factor, &sampleMask);
		if (blend) {
			D3D11_BLEND_DESC desc{};
			blend->GetDesc(&desc);
			entry.text += fmt::format(" blend {} a2c {} independent {}:", static_cast<const void*>(blend), desc.AlphaToCoverageEnable, desc.IndependentBlendEnable);
			for (const auto& rt : desc.RenderTarget)
				entry.text += fmt::format(" [{} {}/{}/{} {}/{}/{} m{:X}]", rt.BlendEnable, int(rt.SrcBlend), int(rt.DestBlend), int(rt.BlendOp), int(rt.SrcBlendAlpha),
					int(rt.DestBlendAlpha), int(rt.BlendOpAlpha), rt.RenderTargetWriteMask);
			blend->Release();
		}
		ID3D11DepthStencilState* depth = nullptr;
		UINT stencilRef = 0;
		context->OMGetDepthStencilState(&depth, &stencilRef);
		if (depth) {
			D3D11_DEPTH_STENCIL_DESC desc{};
			depth->GetDesc(&desc);
			entry.text += fmt::format(" depth {} test {} write {} func {} stencil {} ref {}", static_cast<const void*>(depth), desc.DepthEnable, int(desc.DepthWriteMask), int(desc.DepthFunc),
				desc.StencilEnable, stencilRef);
			depth->Release();
		}
		ID3D11RasterizerState* raster = nullptr;
		context->RSGetState(&raster);
		if (raster) {
			D3D11_RASTERIZER_DESC desc{};
			raster->GetDesc(&desc);
			entry.text += fmt::format(" raster cull {} bias {} {} {}", int(desc.CullMode), desc.DepthBias, desc.SlopeScaledDepthBias, desc.DepthClipEnable);
			raster->Release();
		}
		std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};
		context->PSGetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
		entry.text += "\n    samplers:";
		for (std::uint32_t s = 0; s < samplers.size(); ++s) {
			if (!samplers[s])
				continue;
			D3D11_SAMPLER_DESC desc{};
			samplers[s]->GetDesc(&desc);
			entry.text += fmt::format(" s{}={:X}/{}/{}/{}", s, int(desc.Filter), int(desc.AddressU), desc.MaxAnisotropy, desc.MipLODBias);
			samplers[s]->Release();
		}
		std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> all{};
		context->PSGetShaderResources(0, static_cast<UINT>(all.size()), all.data());
		entry.text += "\n    all textures:";
		for (std::uint32_t t = 16; t < all.size(); ++t) {
			if (all[t])
				entry.text += fmt::format(" t{}={}", t, static_cast<const void*>(all[t]));
		}
		for (auto* view : all)
			if (view)
				view->Release();
		std::array<ID3D11Buffer*, 14> constants{};
		context->PSGetConstantBuffers(0, static_cast<UINT>(constants.size()), constants.data());
		entry.text += "\n    PS buffers:";
		for (std::uint32_t b = 0; b < constants.size(); ++b) {
			if (!constants[b])
				continue;
			entry.text += fmt::format(" b{}={}", b, static_cast<const void*>(constants[b]));
			constants[b]->Release();
		}
		ID3D11RenderTargetView* targets[8]{};
		context->OMGetRenderTargets(8, targets, nullptr);
		entry.text += "\n    targets:";
		for (auto* target : targets) {
			entry.text += fmt::format(" {}", static_cast<const void*>(target));
			if (target)
				target->Release();
		}
	}
	std::array<ID3D11Buffer*, 3> buffers{};
	context->PSGetConstantBuffers(1, 2, buffers.data());
	context->VSGetConstantBuffers(2, 1, &buffers[2]);
	for (std::uint32_t b = 0; b < 3; ++b) {
		if (!buffers[b])
			continue;
		D3D11_BUFFER_DESC desc{};
		buffers[b]->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (SUCCEEDED(globals::d3d::device->CreateBuffer(&desc, nullptr, entry.staging[b].put())))
			context->CopyResource(entry.staging[b].get(), buffers[b]);
		buffers[b]->Release();
	}
	pending->entries.push_back(std::move(entry));
	pending->framesLeft = 4;
}

void DrawcallLimitFix::ProbeOpaqueTarget(std::uint32_t a_stage)
{
	const bool a_afterDCLF = a_stage != 0;
	const std::string& pixel = DCLF::SwitchValue(DCLF::Switch::TargetProbe);
	if (pixel.empty())
		return;
	// The eight colour targets, then the bound depth buffer (rt8) and the pixel stage's t55 (rt9: Terrain Blending's mask).
	constexpr std::uint32_t kTargets = 10;
	constexpr std::uint32_t kBlock = 64;  // the mean over a block: one pixel is too noisy under TAA jitter
	struct Slot
	{
		std::array<winrt::com_ptr<ID3D11Texture2D>, kTargets> staging;
		std::array<DXGI_FORMAT, kTargets> format{};
		std::array<std::uint32_t, kTargets> x{}, y{};  // where the block is in the staging copy (a depth buffer is copied whole)
		bool running = false;
	};
	// Before and after DCLF's colour epoch, after Terrain Blending's passes, after the deferred composite.
	constexpr std::uint32_t kStages = 4;
	static std::array<Slot, kStages> slots;
	static std::array<const char*, kStages> labels{ "before DCLF's colour epoch", "after DCLF's colour epoch", "after Terrain Blending's passes", "after the deferred composite" };
	static std::string terrainPasses;
	static std::uint32_t frames = 0, framesLeft = 0;
	auto* context = globals::d3d::context;
	auto half = [](std::uint16_t a_h) {
		const std::uint32_t sign = (a_h >> 15) & 1, exponent = (a_h >> 10) & 0x1F, mantissa = a_h & 0x3FF;
		float value = exponent == 0 ? std::ldexp(float(mantissa), -24) : exponent == 31 ? std::numeric_limits<float>::infinity() : std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
		return sign ? -value : value;
	};
	if (!a_afterDCLF) {
		if (framesLeft && --framesLeft == 0) {
			for (std::uint32_t i = 0; i < kStages; ++i) {
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
						// The fourth channel's spread over the block: near 0, near 1, between.
						std::array<std::uint32_t, 3> alphas{};
						auto alphaOf = [&](double a_value) { ++alphas[a_value < 0.01 ? 0 : a_value > 0.99 ? 1 : 2]; };
						for (std::uint32_t row = 0; row < kBlock; ++row) {
							const auto* line = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row + slot.y[t]) * mapped.RowPitch;
							for (std::uint32_t c0 = 0; c0 < kBlock; ++c0) {
								const std::uint32_t column = c0 + slot.x[t];
								switch (slot.format[t]) {
								case DXGI_FORMAT_R24G8_TYPELESS:
								case DXGI_FORMAT_D24_UNORM_S8_UINT:
									sum[0] += (reinterpret_cast<const std::uint32_t*>(line)[column] & 0xFFFFFFu) / 16777215.0;
									channels = 1;
									break;
								case DXGI_FORMAT_R32_TYPELESS:
								case DXGI_FORMAT_R32_FLOAT:
								case DXGI_FORMAT_D32_FLOAT:
									sum[0] += reinterpret_cast<const float*>(line)[column];
									channels = 1;
									break;
								case DXGI_FORMAT_R16G16B16A16_FLOAT:
									for (std::uint32_t c = 0; c < 4; ++c)
										sum[c] += half(reinterpret_cast<const std::uint16_t*>(line)[column * 4 + c]);
									alphaOf(half(reinterpret_cast<const std::uint16_t*>(line)[column * 4 + 3]));
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
									alphaOf(line[column * 4 + 3] / 255.0);
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
							text += fmt::format(" rt{}=({:.7f})", t, sum[0] / n);
						else if (channels == 2)
							text += fmt::format(" rt{}=({:.4f} {:.4f})", t, sum[0] / n, sum[1] / n);
						else if (channels == 3)
							text += fmt::format(" rt{}=({:.4f} {:.4f} {:.4f})", t, sum[0] / n, sum[1] / n, sum[2] / n);
						else
							text += fmt::format(" rt{}=({:.4f} {:.4f} {:.4f} {:.4f} | a0 {} a1 {} amid {})", t, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, alphas[0], alphas[1],
								alphas[2]);
						context->Unmap(slot.staging[t].get(), 0);
					}
					slot.staging[t] = nullptr;
				}
				if (!text.empty())
					logger::info("[DCLF] target probe: targets {}, DCLF {}:{}{}", labels[i], slot.running ? "on" : "off", text, i == 2 ? terrainPasses : std::string());
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
		ID3D11RenderTargetView* views[8] = {};
		ID3D11DepthStencilView* depthView = nullptr;
		context->OMGetRenderTargets(8, views, &depthView);
		for (const std::uint32_t t : { 8u, 9u })
			textures[t] = nullptr;
		if (depthView) {
			winrt::com_ptr<ID3D11Resource> resource;
			depthView->GetResource(resource.put());
			depthView->Release();
			textures[8] = resource.try_as<ID3D11Texture2D>();
		}
		// Terrain Blending's mask texture itself (t55 of its passes; nothing need be bound to t55 here).
		if (globals::features::terrainBlending.loaded && globals::features::terrainBlending.terrainDepth.texture) {
			textures[9].copy_from(reinterpret_cast<ID3D11Texture2D*>(globals::features::terrainBlending.terrainDepth.texture));
		}
		for (std::uint32_t t = 0; t < 8; ++t) {
			textures[t] = nullptr;
			if (!views[t])
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			views[t]->GetResource(resource.put());
			views[t]->Release();
			textures[t] = resource.try_as<ID3D11Texture2D>();
		}
	}
	if (a_stage == 1 && globals::features::terrainBlending.loaded) {
		const auto& blending = globals::features::terrainBlending;
		terrainPasses = fmt::format(" (Terrain Blending replayed {} landscape and {} no-blend passes)", blending.terrainRenderPasses.size(), blending.renderPasses.size());
	}
	const auto sep = pixel.find_first_of(",x");
	auto& slot = slots[a_stage];
	slot.running = Running();
	for (std::uint32_t t = 0; t < kTargets; ++t) {
		slot.staging[t] = nullptr;
		if (!textures[t])
			continue;
		D3D11_TEXTURE2D_DESC desc{};
		textures[t]->GetDesc(&desc);
		const auto x = std::min<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10), desc.Width - kBlock);
		const auto y = std::min<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10), desc.Height - kBlock);
		// A depth buffer is copied whole (D3D11 copies no box of one).
		const bool whole = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		if (!whole)
			stagingDesc.Width = stagingDesc.Height = kBlock;
		stagingDesc.MipLevels = stagingDesc.ArraySize = 1;
		stagingDesc.SampleDesc = { 1, 0 };
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = stagingDesc.MiscFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, slot.staging[t].put())))
			continue;
		if (whole) {
			context->CopyResource(slot.staging[t].get(), textures[t].get());
			slot.x[t] = x;
			slot.y[t] = y;
		} else {
			const D3D11_BOX box{ x, y, 0, x + kBlock, y + kBlock, 1 };
			context->CopySubresourceRegion(slot.staging[t].get(), 0, 0, 0, 0, textures[t].get(), 0, &box);
			slot.x[t] = slot.y[t] = 0;
		}
		slot.format[t] = desc.Format;
	}
}

namespace DCLF
{
	void CensusNativePass(const RE::BSRenderPass* a_pass, std::uint32_t a_technique, std::uint32_t a_mode, bool a_depth)
	{
		static const bool enabled = [] {
			const char* value = std::getenv("CS_DCLF_LOD_CENSUS");
			return value && *value && *value != '0';
		}();
		if (!enabled || !a_pass || !a_pass->geometry)
			return;
		// Where in the frame: the main-pass draws are numbered, and the first in the deferred pass marks its start.
		static std::uint32_t phaseFrame = 0, sequence = 0, firstDeferred = ~0u;
		if (a_mode == ~0u) {
			if (const std::uint32_t now = SceneStore::Get().GetFrame(); now != phaseFrame) {
				phaseFrame = now;
				sequence = 0;
				firstDeferred = ~0u;
			}
			if (globals::deferred->deferredPass && firstDeferred == ~0u)
				firstDeferred = sequence;
			++sequence;
		}
		struct Row
		{
			std::uint64_t draws = 0, triangles = 0;
			ankerl::unordered_dense::set<const void*> geometries;
			std::string example;
		};
		static std::mutex mutex;
		static std::map<std::string, Row> rows;
		static std::uint32_t lastFrame = 0, frames = 0;
		const auto* geometry = a_pass->geometry;
		const auto* shader = a_pass->shader;
		const auto* property = a_pass->shaderProperty;
		std::string lod = "-";
		std::uint32_t technique = 0;
		const auto type = shader ? static_cast<std::uint32_t>(shader->shaderType.get()) : 99u;
		if (shader && shader->shaderType.get() == RE::BSShader::Type::Lighting && property) {
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			const auto& flags = property->flags;
			lod = flags.all(Flag::kLODLandscape) ? "land" : flags.all(Flag::kHDLODObjects) ? "hdobj" : flags.all(Flag::kLODObjects) ? "obj" :
			      flags.all(Flag::kMultiTextureLandscape)                                           ? "mtland" : "-";
			if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material))
				lod += fmt::format(" feature {}", static_cast<std::uint32_t>(material->GetFeature()));
			technique = (a_technique >> 24) & 0x3f;
		}
		// The scene root: the ancestor right under the ShadowSceneNode (or the topmost one).
		const RE::NiAVObject* root = geometry;
		for (const RE::NiAVObject* object = geometry; object; object = object->parent) {
			root = object;
			if (object->parent && object->parent->GetRTTI() && std::string_view(object->parent->GetRTTI()->name) == "ShadowSceneNode")
				break;
		}
		std::string rootName = root && root->name.c_str() && *root->name.c_str() ? root->name.c_str() : (root && root->GetRTTI() ? root->GetRTTI()->name : "?");
		if (const auto* tes = RE::TES::GetSingleton()) {
			for (const RE::NiAVObject* object = geometry; object; object = object->parent) {
				if (object == tes->lodLandRoot) { rootName = "lodLandRoot"; break; }
				if (object == tes->objLODWaterRoot) { rootName = "objLODWaterRoot"; break; }
				if (object == tes->objRoot) { rootName = "objRoot"; break; }
			}
		}
		// Below the root: the node right under the named root, and the shape's parent class.
		std::string below;
		for (const RE::NiAVObject* object = geometry; object && object->parent; object = object->parent) {
			const auto* tes = RE::TES::GetSingleton();
			if (tes && (object->parent == tes->lodLandRoot || object->parent == tes->objLODWaterRoot || object->parent == tes->objRoot || object->parent == root)) {
				below = fmt::format("{}:{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "");
				break;
			}
		}
		rootName += fmt::format(" / {} (parent {})", below, geometry->parent && geometry->parent->GetRTTI() ? geometry->parent->GetRTTI()->name : "-");
		// DCLF's view of it (object LOD's native passes: why the skip left them).
		if (lod != "-" && a_mode == ~0u) {
			const auto object = SceneStore::Get().FindObject(geometry);
			rootName += object < 0 ? " [no object]" :
			            fmt::format(" [object, bound {}, set phases {}, fading at registration {}, drawable {}, deferred pass {}, running {}]", SceneStore::Get().IsMember(object),
							SceneStore::Get().SetPhasesOf(object), PassCapture::FadingAtRegistration(const_cast<RE::BSRenderPass*>(a_pass)),
							SceneStore::Get().ObjectDrawable(object), globals::deferred->deferredPass, globals::features::drawcallLimitFix.loaded);
			rootName += fmt::format(" [write mode {}, blend mode {}]", globals::game::shadowState->GetRuntimeData().alphaBlendWriteMode,
				globals::game::shadowState->GetRuntimeData().alphaBlendMode);
			rootName += fmt::format(" [{}, in world {}]", globals::deferred->deferredPass ? "deferred" : firstDeferred == ~0u ? "before the deferred pass" : "after the deferred pass",
				globals::state->inWorld);
		}
		const auto key = fmt::format("{} type {} lod {} tech {} {} under {}", a_mode == ~0u ? (a_depth ? "main-depth" : "main-colour") : fmt::format("shadow-mode-{:x}", a_mode),
			type, lod, technique, geometry->GetRTTI() ? geometry->GetRTTI()->name : "?", rootName);
		std::uint64_t triangles = 0;
		if (const auto* shape = const_cast<RE::BSGeometry*>(geometry)->AsTriShape())
			triangles = shape->GetTrishapeRuntimeData().triangleCount;
		std::scoped_lock lock(mutex);
		if (a_mode == ~0u) {
			const std::uint32_t frame = SceneStore::Get().GetFrame();
			if (frame != lastFrame) {
				lastFrame = frame;
				if (++frames == 300) {
					std::string text = "[DCLF] LOD census, per frame over 300 frames (draws, triangles, distinct geometries in all):";
					for (const auto& [name, row] : rows)
						text += fmt::format("\n    {}: {:.1f} draws, {:.0f} triangles, {} geometries; e.g. {}", name, row.draws / 300.0, row.triangles / 300.0, row.geometries.size(), row.example);
					logger::info("{}", text);
					rows.clear();  // examples too: each report samples afresh
					frames = 0;
				}
			}
		}
		auto& row = rows[key];
		if (row.example.empty()) {
			std::string texture = "-";
			if (shader && shader->shaderType.get() == RE::BSShader::Type::Lighting && property)
				if (const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material); material && material->diffuseTexture)
					texture = material->diffuseTexture->name.c_str() ? material->diffuseTexture->name.c_str() : "?";
			row.example = fmt::format("'{}' {}", geometry->name.c_str() ? geometry->name.c_str() : "", texture);
		}
		++row.draws;
		row.triangles += triangles;
		row.geometries.insert(geometry);
	}
}
