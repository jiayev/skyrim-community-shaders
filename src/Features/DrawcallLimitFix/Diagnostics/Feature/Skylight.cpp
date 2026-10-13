#include "Skylight.h"

#include "Features/DrawcallLimitFix/Runtime/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/Skylighting.h"

#include <filesystem>
#include <fstream>

// CS_DCLF_SKYLIGHT_PARITY: the two renders of one occlusion map, the engine's (kept before the map is cleared again) and DCLF's (kept
// after its epoch drew it), compared texel by texel once both copies are readable. Render thread.

namespace DCLF::Diagnostics::Skylight
{
	namespace
	{
		struct Map
		{
			std::array<winrt::com_ptr<ID3D11Texture2D>, 2> staging;
			D3D11_TEXTURE2D_DESC desc{};
			ID3D11Texture2D* texture = nullptr;  // the map's, as stage 0 found it (Skylighting swaps the precipitation target meanwhile)
			std::uint32_t framesLeft = 0;
			std::uint32_t frames = 0;
			bool pending = false;
		};
		std::array<Map, 2> maps;
		std::uint32_t waiting = 0;  // the maps whose engine render was kept this frame (stage 0)
		constexpr const char* kNames[2] = { "Skylighting", "precipitation" };

		void Compare(std::uint32_t a_map)
		{
			auto& map = maps[a_map];
			if (!map.pending || --map.framesLeft)
				return;
			map.pending = false;
			auto* context = globals::d3d::context;
			std::array<D3D11_MAPPED_SUBRESOURCE, 2> mapped{};
			if (FAILED(context->Map(map.staging[0].get(), 0, D3D11_MAP_READ, 0, &mapped[0])))
				return;
			if (FAILED(context->Map(map.staging[1].get(), 0, D3D11_MAP_READ, 0, &mapped[1]))) {
				context->Unmap(map.staging[0].get(), 0);
				return;
			}
			// Depth as the texture holds it: 16-bit UNORM (the engine's D16 maps), else the top 24 of 32 bits, else a float.
			const auto format = map.desc.Format;
			const bool unorm16 = format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_R16_UNORM;
			const bool float32 = format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
			auto depthAt = [&](std::uint32_t a_image, std::uint32_t x, std::uint32_t y) {
				const auto* row = static_cast<const std::uint8_t*>(mapped[a_image].pData) + std::size_t(y) * mapped[a_image].RowPitch;
				if (unorm16)
					return reinterpret_cast<const std::uint16_t*>(row)[x] / 65535.0;
				const std::uint32_t word = reinterpret_cast<const std::uint32_t*>(row)[x];
				if (float32)
					return static_cast<double>(std::bit_cast<float>(word));
				return (word & 0xFFFFFF) / 16777215.0;
			};
			std::uint64_t texels = 0, equal = 0, dclfFarther = 0, dclfNearer = 0, engineFar = 0, dclfFar = 0, bigDiff = 0;
			double maxDiff = 0.0, sumDiff = 0.0;
			for (std::uint32_t y = 0; y < map.desc.Height; ++y)
				for (std::uint32_t x = 0; x < map.desc.Width; ++x) {
					const double engine = depthAt(0, x, y), dclf = depthAt(1, x, y);
					++texels;
					engineFar += engine >= 1.0 ? 1 : 0;
					dclfFar += dclf >= 1.0 ? 1 : 0;
					const double diff = std::abs(engine - dclf);
					if (diff == 0.0) {
						++equal;
						continue;
					}
					(dclf > engine ? dclfFarther : dclfNearer) += 1;
					bigDiff += diff > 1.0 / 256.0 ? 1 : 0;
					sumDiff += diff;
					maxDiff = std::max(maxDiff, diff);
				}
			// A map that differs over more than 5% of its texels: both written out (16-bit PGM, the depth scaled to 0..65535).
			if (bigDiff * 20 > texels) {
				static std::uint32_t dumps = 0;
				if (dumps < 4) {
					const auto& dir = SwitchValue(Switch::SkylightDumpDir);
					const auto directory = std::filesystem::path(dir.empty() ? "." : dir);
					for (std::uint32_t image = 0; image < 2; ++image) {
						std::ofstream out(directory / fmt::format("{}-parity-{}-{}.pgm", a_map ? "precipitation" : "sky", dumps, image ? "dclf" : "engine"), std::ios::binary);
						out << "P5\n" << map.desc.Width << " " << map.desc.Height << "\n65535\n";
						for (std::uint32_t y = 0; y < map.desc.Height; ++y)
							for (std::uint32_t x = 0; x < map.desc.Width; ++x) {
								const auto value = static_cast<std::uint16_t>(std::clamp(depthAt(image, x, y), 0.0, 1.0) * 65535.0);
								const char bytes[2] = { static_cast<char>(value >> 8), static_cast<char>(value & 0xFF) };
								out.write(bytes, 2);
							}
					}
					logger::info("[DCLF] {} occlusion parity: map {} written to {}", kNames[a_map], dumps, directory.string());
					++dumps;
				}
			}
			context->Unmap(map.staging[0].get(), 0);
			context->Unmap(map.staging[1].get(), 0);
			logger::info("[DCLF] {} occlusion parity: {}x{} format {}: {} texels equal of {}, DCLF farther {} nearer {} ({} by more than 1/256), max diff {:.6f}, mean diff {:.6f}; clear texels engine {} DCLF {}",
				kNames[a_map], map.desc.Width, map.desc.Height, static_cast<std::uint32_t>(format), equal, texels, dclfFarther, dclfNearer, bigDiff, maxDiff,
				texels - equal ? sumDiff / double(texels - equal) : 0.0, engineFar, dclfFar);
		}

		/** @brief Stage 0: the engine's render, kept before the map is cleared for DCLF's; stage 1: DCLF's, once its epoch drew it. */
		void Copy(std::uint32_t a_map, std::uint32_t a_stage)
		{
			auto& map = maps[a_map];
			// The map's texture: Skylighting's own, or depth target 10 as stage 0 finds it (Skylighting swaps its own in later).
			ID3D11Texture2D* texture = map.texture;
			if (a_stage == 0) {
				if (a_map == 0)
					texture = globals::features::skylighting.texOcclusion ? globals::features::skylighting.texOcclusion->resource.get() : nullptr;
				else if (auto* renderer = globals::game::renderer)
					texture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP].texture;
				map.texture = texture;
			}
			if (!texture || a_stage > 1)
				return;
			if (a_stage == 0) {
				texture->GetDesc(&map.desc);
				D3D11_TEXTURE2D_DESC desc = map.desc;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.BindFlags = 0;
				desc.MiscFlags = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				for (auto& staging : map.staging) {
					staging = nullptr;
					if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, staging.put())))
						return;
				}
			}
			if (!map.staging[a_stage])
				return;
			globals::d3d::context->CopyResource(map.staging[a_stage].get(), texture);
			if (a_stage == 0)
				waiting |= 1u << a_map;
			if (a_stage == 1) {
				map.pending = true;
				map.framesLeft = 4;
			}
		}
	}

	bool ReferenceDue(std::uint32_t a_map)
	{
		if (!Observe().skylight || a_map >= maps.size())
			return false;
		auto& map = maps[a_map];
		return !map.pending && (map.frames++ % 120) == 60;
	}

	void Install()
	{
		if (!Observe().skylight)
			return;
		Subscribe<OcclusionReferenceRendered>([](const OcclusionReferenceRendered& a_event) { Copy(a_event.map, 0); });
		Subscribe<OcclusionMapsDrawn>([](const OcclusionMapsDrawn& a_event) {
			for (std::uint32_t map = 0; map < 2; ++map)
				if (waiting & (1u << map)) {
					if (a_event.drawn & (1u << map))
						Copy(map, 1);
					else
						maps[map].staging = {};
				}
			waiting = 0;
		});
		Subscribe<PrepassBegin>([](const PrepassBegin&) {
			for (std::uint32_t map = 0; map < 2; ++map)
				Compare(map);
		});
	}
}
