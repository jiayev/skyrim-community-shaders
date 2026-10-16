#pragma once

// The main pass's capture (render thread) and the layout of the per-frame constant blocks at their frame slots, with the frame push
// words that name them. Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	// The main pass's bindings where its opaque batches start (DrawcallLimitFix::BeforeOpaquePass), or the depth pass's.
	struct Capture
	{
		std::array<ID3D11Buffer*, kConstantBufferRegisters> vsBuffers{};
		std::array<ID3D11Buffer*, kConstantBufferRegisters> psBuffers{};
		std::array<ID3D11ShaderResourceView*, kTextureRegisters> psViews{};
		winrt::com_ptr<ID3D11Texture2D> depth;
		std::array<winrt::com_ptr<ID3D11Texture2D>, kColorTargets> targets;
		std::uint32_t targetCount = 0;
		std::uint32_t viewportWidth = 0, viewportHeight = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
		// The camera the main pass draws with: the world transforms are stored relative to it, and the
		// engine has moved it on by the time the epoch packs the constants.
		RE::NiPoint3 eye, previousEye;

		void Release()
		{
			for (auto* buffer : vsBuffers)
				if (buffer)
					buffer->Release();
			for (auto* buffer : psBuffers)
				if (buffer)
					buffer->Release();
			for (auto* view : psViews)
				if (view)
					view->Release();
		}
	};

	Capture CaptureBindings();

	// The CPU-written structured buffers the main pass binds at t16 and up, less the registers of the frame graph buffers on the graph
	// (a_graphBuffers: a bit per registration, Resources::graphBuffers), which come from the graph.
	std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, std::uint32_t a_graphBuffers);

	inline ID3D11Buffer* BufferOf(ID3D11ShaderResourceView* a_view)
	{
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		return static_cast<ID3D11Buffer*>(resource.get());  // the view keeps it alive
	}

	// A growing byte arena of 256-byte aligned constant blocks.
	class ConstantArena
	{
	public:
		void Reset(std::uint64_t a_capacity)
		{
			bytes.clear();
			capacity = a_capacity;
		}

		// Offset of a zeroed block, or ~0 when the arena is full.
		std::uint64_t Allocate(std::size_t a_size)
		{
			const std::uint64_t offset = (bytes.size() + kConstantAlignment - 1) & ~(kConstantAlignment - 1);
			if (offset + a_size > capacity)
				return ~0ull;
			bytes.resize(offset + std::max<std::size_t>(a_size, 16));
			return offset;
		}

		std::span<std::byte> At(std::uint64_t a_offset, std::size_t a_size) { return { bytes.data() + a_offset, a_size }; }
		const std::vector<std::byte>& Bytes() const { return bytes; }

	private:
		std::vector<std::byte> bytes;
		std::uint64_t capacity = 0;
	};

	// The per-frame constant blocks - whatever the main pass binds outside the per-draw registers - live
	// in Resources::frameConstants at fixed slots, so that a record can name a block before its contents
	// exist: the vertex stage's b0-b13, then the pixel stage's, then the zeroed StrictLightData block
	// every bindless draw's b3 reads, and the frame lighting every DCLF_BINDLESS draw's b13 reads
	// (FrameView::FrameCapture::lighting). A slot is D3D11's constant buffer maximum, so no block overflows.
	constexpr std::uint64_t kFrameSlotBytes = 65536;
	constexpr std::uint32_t kFrameSlotSharedLight = 2 * kConstantBufferRegisters;
	constexpr std::uint32_t kFrameSlotLighting = kFrameSlotSharedLight + 1;
	// The frame record (DrawBindings): every register a draw's rows do not give - the frame slots' addresses, the frame's
	// textures (t16 and up), the object and bone tables - written by each commit (FrameRecordOf).
	constexpr std::uint32_t kFrameSlotRecord = kFrameSlotLighting + 1;
	constexpr std::uint32_t kFrameSlotCount = kFrameSlotRecord + 1;
	constexpr std::uint64_t kFrameConstantBytes = std::uint64_t(kFrameSlotCount) * kFrameSlotBytes;

	constexpr std::uint64_t FrameSlotOffset(bool a_pixelStage, std::uint32_t a_register)
	{
		return (std::uint64_t(a_pixelStage ? kConstantBufferRegisters : 0u) + a_register) * kFrameSlotBytes;
	}
	// The lighting slot's rows (PS b13): the frame lighting (c0-c5), the LOD fades' inputs (c6-c13), the foliage record's (c14-c15),
	// the extras' pixel inputs (c16, ExtrasFrame::projectedGlobals).
	constexpr std::uint64_t kExtrasPixelFrameOffset = std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame) + 32;
	static_assert(sizeof(FrameLighting) + sizeof(LodFadeFrame) + 32 == 16 * 16);
	// Then Core's feature rows (c17 up to Extensions::kFrameConstantRowsEnd, Core/Extensions/Commits.h): the frame constant rows
	// (Api::FrameConstantRow; the Effect port's camera planes, light colour and blood at c17-c19), 16 bytes each. Uploads.cpp checks the
	// first is right after the extras' pixel row.
	constexpr std::uint64_t FrameConstantRowOffset(std::uint32_t a_row) { return std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + std::uint64_t(a_row) * 16; }
	static_assert(sizeof(FrameFog) == 3 * 16);

	/** @brief The frame push word of a stage's constant buffer register (FramePushWords' order), or ~0u when it is not pushed. */
	inline std::uint32_t FramePushWord(bool a_pixel, std::uint32_t a_register)
	{
		std::uint32_t word = kFramePushRegisters;
		for (const bool pixel : { false, true }) {
			const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
			for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
				if (!((mask >> r) & 1))
					continue;
				if (pixel == a_pixel && r == a_register)
					return word;
				word += 2;
			}
		}
		return ~0u;
	}

	inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants)
	{
		std::array<std::uint32_t, kFramePushWords> words{};
		std::uint32_t word = kFramePushRegisters;
		for (const bool pixel : { false, true }) {
			const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
			for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
				if (!((mask >> r) & 1))
					continue;
				// The frame slot, the shared light block at PS b3 and the frame lighting at PS b13 (kFrameLightingRegister).
				std::uint64_t address = a_frameConstants + FrameSlotOffset(pixel, r);
				if (pixel && r == 3)
					address = a_frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
				else if (pixel && r == kFrameLightingRegister)
					address = a_frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
				words[word++] = static_cast<std::uint32_t>(address);
				words[word++] = static_cast<std::uint32_t>(address >> 32);
			}
		}
		const std::uint64_t record = a_frameConstants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes;
		words[kFramePushRecord] = static_cast<std::uint32_t>(record);
		words[kFramePushRecord + 1] = static_cast<std::uint32_t>(record >> 32);
		return words;
	}

	// The blocks the render thread packs into the frame slots for one epoch.
	struct FrameBlocks
	{
		std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
		std::uint32_t vsMask = 0, psMask = 0;  // the slots supplied
	};
}
