#pragma once

// Small helpers every part of IndirectDraws uses: view indices, formats, handles, buffers, registers.
// Self-contained (features include it); Internal.h includes it in its order.

// volk must precede every Vulkan header in this translation unit (DrawPipelinesRhi.h is included after it).
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>

namespace DCLF::Draws
{
	// A live override of CS_DCLF_ASYNC_COMPUTE (the test harness's dclf:async-compute, then a graph rebuild): -1 none, 0 graphics, 1 compute.
	inline std::atomic<int> asyncComputeOverride{ -1 };
	// The sun views' contribution cull (kCullMinTexels): the least a caster's bound radius may project to, in texels; 0 off. Read by
	// each shadow commit into its sun views' latches (data only: no recording changes), so it is live for an in-session A/B.
	inline std::atomic<float> shadowMinTexels{ 0.0f };
	// The queue of a pass that prefers async compute: the compute queue, or the graphics queue under CS_DCLF_ASYNC_COMPUTE=0. Read at
	// the graph's build.
	inline org::QueueKind AsyncComputeQueue()
	{
		const int forced = asyncComputeOverride.load(std::memory_order_relaxed);
		return (forced < 0 ? SwitchOnUnlessZero(Switch::AsyncCompute) : forced != 0) ? org::QueueKind::Compute : org::QueueKind::Graphics;
	}

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
	// the preprocess memory stays near what the frame draws (the GPU count buffer says how many run). Never past a_limit, the
	// buffer's draws, even when the buffer is sized down under the current count.
	inline std::uint32_t GrowCapacity(std::uint32_t a_current, std::uint32_t a_needed, std::uint32_t a_limit)
	{
		if (a_needed <= a_current)
			return std::min(a_current, a_limit);
		return std::min(a_limit, std::max(256u, std::bit_ceil(a_needed)));
	}

	template <class H>
	bool SameHandle(const H& a, const H& b)
	{
		return a.index == b.index && a.generation == b.generation;
	}

	inline bool SameIndirect(const IndirectState& a, const IndirectState& b)
	{
		if (a.valid != b.valid || !SameHandle(a.layout, b.layout) || a.classes.size() != b.classes.size())
			return false;
		// Each state class's sets and signatures (what the calls bind) and how many pipelines they hold (what their preprocessing is
		// sized against); the class map is the commit's, written into the latch.
		for (std::size_t c = 0; c < a.classes.size(); ++c) {
			const auto &left = a.classes[c], &right = b.classes[c];
			if (left.pipelines != right.pipelines)
				return false;
			for (std::size_t v = 0; v < left.sets.size(); ++v)
				if (!SameHandle(left.sets[v], right.sets[v]) || !SameHandle(left.signatures[v], right.signatures[v]))
					return false;
		}
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

	// The constant buffers the native draw rebinds per object are Extensions::PerDrawMask's (Core's, plus the features' per-draw
	// buffers: Api::PerDrawBuffer); everything else bound in the main pass is per frame and comes from its CPU mirror.

	constexpr std::uint32_t kStrictLightDataBytes = 1216;  // LightLimitFix.hlsli StrictLightData (15 lights)
	constexpr std::uint32_t kCharacterLightMaterialRegister = 11;  // the engine's register of the character light's noise (kCharacterLightRegister)
	constexpr std::uint32_t kInvalidIndex = GpuTextures::kInvalid;
}
