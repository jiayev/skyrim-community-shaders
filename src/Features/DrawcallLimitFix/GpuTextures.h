#pragma once

#include <array>
#include <cstdint>
#include <memory>

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/**
	 * @brief Shader-visible descriptors for the game's textures and samplers (Phase 2).
	 *
	 * A texture is resolved once per shader resource view: the image is marked stable in DXVK and imported
	 * into BasicRHI without ownership (dxvkGetInteropResourceInfo), and a view with the SRV's format,
	 * range and swizzle is written into a slot of ORG's shader-visible heap, read in the layout DXVK keeps
	 * the image in. The registry weakly deduplicates live imports; every numeric index handed to a draw
	 * is accompanied by ownership of its exact descriptor and SRV through GPU completion.
	 *
	 * Samplers copy the renderer's own D3D11 sampler states (the table the engine selects from by the
	 * shadow state's address and filter modes) into ORG's sampler heap.
	 *
	 * Only while a graph epoch prepares (ORG's descriptor service is active); render thread.
	 */
	class GpuTextures
	{
	public:
		static constexpr std::uint32_t kInvalid = ~0u;

		enum class Reject : std::uint32_t
		{
			None,
			NotImage,
			Import,
			View,
			Count
		};

		struct Stats
		{
			std::uint32_t cached = 0;
			std::uint32_t registrySlots = 0;
			std::uint64_t cleanupPending = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Reject::Count)> rejected{};
			std::uint32_t samplers = 0;
			std::uint32_t unsupportedLayouts = 0;  // images DXVK does not keep in GENERAL
		};
		struct Binding
		{
			std::uint32_t index = kInvalid;
			std::shared_ptr<const void> owner;
		};

		static GpuTextures& Get();

		/** @brief Resolve with CPU ownership of this exact import and descriptor slot. */
		// sourceTag is diagnostic only: material t0-t15, feature 16+, projected 32+,
		// pipeline mask 48, frame tN as 64+N, shadow diffuse 192.
		Binding ResolveBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag = ~0u);

		/** @brief A live import already known without an interop call. */
		bool KnownBinding(ID3D11ShaderResourceView* a_view, Binding& a_binding);

		/** @brief A null view's descriptor heap index (reads zero); kInvalid when unsupported. */
		std::uint32_t NullIndex();
		/** @brief The null descriptor and its exact device-generation owner. */
		Binding NullBinding();

		/** @brief The sampler heap index for the engine's (address mode, filter mode); kInvalid if unknown. */
		std::uint32_t Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode);
		/** @brief The sampler index with ownership of the fixed descriptor. */
		Binding SamplerBinding(std::uint32_t a_addressMode, std::uint32_t a_filterMode);

		/**
		 * @brief Changes on device/reset invalidation. Individual binding identities, not a global restamp,
		 * distinguish a successor import at a reused SRV address.
		 */
		std::uint32_t Generation() const { return generation; }
		Stats GetStats() const;

		void Clear();

		~GpuTextures();

	private:
		GpuTextures();

		std::uint32_t generation = 0;

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
	};
}
