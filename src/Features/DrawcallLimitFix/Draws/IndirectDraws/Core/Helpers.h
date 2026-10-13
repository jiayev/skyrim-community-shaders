#pragma once

// Small helpers every part of IndirectDraws uses: view indices, formats, handles, buffers, registers.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	inline std::uint32_t CaptureViewIndex(const org::PassPrepareContext& a_preparation, const org::DeclaredViewToken& a_token)
	{
		(void)a_preparation.Capture(a_token);
		return a_preparation.Resolve(a_token).index;
	}

	// The format that reads a depth resource's depth aspect in a shader, as DXGI names it.
	inline rhi::Format DepthReadFormat(rhi::Format a_format)
	{
		switch (a_format) {
		case rhi::Format::D24_UNorm_S8_UInt:
		case rhi::Format::R24G8_Typeless:
			return rhi::Format::R24_UNorm_X8_Typeless;
		case rhi::Format::D32_Float_S8X24_UInt:
		case rhi::Format::R32G8X24_Typeless:
			return rhi::Format::R32_Float_X8X24_Typeless;
		case rhi::Format::D32_Float:
			return rhi::Format::R32_Float;
		case rhi::Format::D16_UNorm:
			return rhi::Format::R16_UNorm;
		default:
			return a_format;
		}
	}

	// A draw's ExecuteIndirect max count: a power of two that only grows, so the recording settles while
	// the preprocess memory stays near what the frame draws (the GPU count buffer says how many run).
	inline std::uint32_t GrowCapacity(std::uint32_t a_current, std::uint32_t a_needed, std::uint32_t a_limit)
	{
		if (a_needed <= a_current)
			return a_current;
		return std::min(a_limit, std::max(256u, std::bit_ceil(a_needed)));
	}

	template <class H>
	bool SameHandle(const H& a, const H& b)
	{
		return a.index == b.index && a.generation == b.generation;
	}

	inline bool SameIndirect(const IndirectState& a, const IndirectState& b)
	{
		if (a.valid != b.valid || !SameHandle(a.layout, b.layout))
			return false;
		for (std::size_t i = 0; i < a.sets.size(); ++i)
			if (!SameHandle(a.sets[i], b.sets[i]) || !SameHandle(a.signatures[i], b.signatures[i]))
				return false;
		return SameHandle(a.zLayout, b.zLayout) && SameHandle(a.zDrawSignature, b.zDrawSignature) && SameHandle(a.refraction, b.refraction) &&
		       a.refractionFormat == b.refractionFormat;
	}

	// The dispatch signature every BuildDraws pass records with: one Dispatch argument, read from a latch.
	inline rhi::CommandSignaturePtr CreateDispatchSignature(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout)
	{
		rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
		rhi::CommandSignaturePtr signature;
		if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 }, a_layout, signature) != rhi::Result::Ok)
			return {};
		return signature;
	}

	inline std::shared_ptr<org::Buffer> CreateWords(std::uint64_t a_words, bool a_unorderedAccess, const char* a_name)
	{
		auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(a_words), sizeof(std::uint32_t), a_unorderedAccess);
		buffer->SetName(a_name);
		buffer->Materialize();
		return buffer;
	}

	// Constant buffers the native draw rebinds per object (b0-b2 are the Lighting groups): everything
	// else bound in the main pass is per frame and comes from its CPU mirror.
	constexpr std::uint32_t kPerDrawVS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 9) | (1u << 10);
	// b7 is Advanced Skin's SkinPerGeometry: bound per draw, for the actor the engine drew last (kSkinRegister).
	constexpr std::uint32_t kPerDrawPS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8) | (1u << 11);
	// Bytes per texel, for printing exactly the pixel a readback holds and nothing beyond it.
	inline std::uint32_t FormatBytes(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			return 16;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R32G32_FLOAT:
			return 8;
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_D16_UNORM:
			return 2;
		case DXGI_FORMAT_R8_UNORM:
			return 1;
		default:
			return 4;  // the 32-bit packed formats the G-buffer uses (RGBA8, R10G10B10A2, R11G11B10)
		}
	}

	// The probe's sample points, in the order they happen within a frame.
	constexpr const char* kProbeFirstLabel = "before z-prepass";

	constexpr std::uint32_t kLinearLightingRegister = 8;  // LLPerGeometry: Linear Lighting's per-object emissive multiplier
	constexpr std::uint32_t kSkinRegister = 7;             // SkinPerGeometry: Advanced Skin's per-object wetness
	constexpr std::uint32_t kStrictLightDataBytes = 1216;  // LightLimitFix.hlsli StrictLightData (15 lights)
	constexpr std::uint32_t kLightsRegister = 35;          // t35-t37: Light Limit Fix's lights, list and grid
	constexpr std::uint32_t kCharacterLightMaterialRegister = 11;  // the engine's register of the character light's noise (kCharacterLightRegister)
	constexpr std::uint32_t kInvalidIndex = GpuTextures::kInvalid;
}
