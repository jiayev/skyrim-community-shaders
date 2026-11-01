#pragma once

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include <array>
#include <memory>
#include <span>

namespace DCLF::Features::EnvironmentProbe
{
	inline constexpr std::uint8_t kPhase = 1u << 7;
	inline constexpr std::uint32_t kViewBit = 1u << 12;
	inline constexpr std::uint32_t kOctants = 8;
	Api::Feature Feature();
	bool OwnsCameraCapture();
	bool SupportsPipeline(std::uint32_t rasterFlags);
	// Captured on the render thread alongside Deferred's native shadow parameter copy.
	void CaptureShadowParameters(ID3D11Buffer* buffer, std::uint32_t frame);

	struct Capture
	{
		std::array<ID3D11Texture2D*, 4> targets;
		std::array<ID3D11ShaderResourceView*, 4> textures;  // LLF lights and probe mask, CSM and supplemental shadow
		std::array<RE::NiCamera*, kOctants> cameras{};
		std::array<std::span<const std::byte>, kOctants> parameters{};
		std::span<const std::byte> shared;
		bool usesShadowParameters = false;
	};
	// A nonempty result owns the exact claims whose opaque draws were submitted.
	std::shared_ptr<const SetSnapshot> Draw(const Capture& capture);
}
