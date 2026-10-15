// The open defects' probes (render thread): DrawcallLimitFix::ProbeOpaqueTarget (CS_DCLF_TARGET_PROBE) and the player claims check (the
// tree LOD audit is Features/TreeLod's).
#include "OpenDefectProbes.h"
#include "TargetDump.h"

#include <DirectXPackedVector.h>

#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/TerrainBlending.h"
#include "State.h"
#include "Deferred.h"

void DrawcallLimitFix::ProbeOpaqueTarget(std::uint32_t a_stage)
{
	const bool a_afterDCLF = a_stage != 0;
	// CS_DCLF_TARGET_DUMP's points here (Deferred's hooks), with DCLF on or off.
	static constexpr const char* kDumpPoints[4] = { "opaque end", "after DCLF colour", "after terrain blending", "after deferred composite" };
	if (a_stage < 4)
		DCLF::TargetDump::Point(kDumpPoints[a_stage]);
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
	void ObservePlayerClaims()
	{
		if (!SwitchEnabled(Switch::SetParity))
			return;
		struct Observed
		{
			std::uint32_t frames = 0, hiddenFrames = 0, claimedFrames = 0, violations = 0;
			std::uint32_t streak = 0, longest = 0;
			std::string first;
		};
		static Observed observed;
		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto claims = Scene::FrameView::Get().FrameClaims();
		++observed.frames;
		std::uint32_t claimed = 0;
		std::string example;
		// The geometry under a_root claimed for the main view (a safe point: the engine's update is done, the culls not begun).
		auto count = [&](RE::NiAVObject* a_root) {
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) {
				if (claims->PhasesOf(a_geometry) & kSetMain) {
					if (!claimed++)
						example = a_geometry->name.c_str() ? a_geometry->name.c_str() : "?";
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};
		if (player && claims) {
			auto* thirdPerson = player->Get3D(false);
			auto* firstPerson = player->Get3D(true);
			const bool thirdHidden = thirdPerson && thirdPerson->GetFlags().any(RE::NiAVObject::Flag::kHidden);
			observed.hiddenFrames += thirdHidden ? 1 : 0;
			if (thirdHidden)
				count(thirdPerson);
			if (firstPerson && firstPerson != thirdPerson)
				count(firstPerson);
		}
		if (claimed) {
			++observed.claimedFrames;
			observed.longest = std::max(observed.longest, ++observed.streak);
			if (observed.streak == 9 && !observed.violations++)
				observed.first = fmt::format("'{}' and {} more claimed for 9 frames", example, claimed - 1);
		} else
			observed.streak = 0;
		if (observed.frames < 300)
			return;
		logger::info("[DCLF] player 3D against the claims (set parity) over {} frames: third person hidden in {}; frames with a hidden skeleton's geometry claimed for "
					 "the main view {} (longest run {}), runs past the publication's latency {}{}{}",
			observed.frames, observed.hiddenFrames, observed.claimedFrames, observed.longest, observed.violations, observed.violations ? " <- PLAYER 3D; first: " : " <- OK",
			observed.first);
		const std::uint32_t streak = observed.streak;
		observed = {};
		observed.streak = streak;
	}
}
