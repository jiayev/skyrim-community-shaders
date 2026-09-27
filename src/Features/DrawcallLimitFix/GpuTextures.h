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
	 * the image in. The SRV stays referenced while cached; entries unused for kEvictFrames are released
	 * and their slots retire against ORG's queue fences.
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
			std::array<std::uint32_t, static_cast<std::size_t>(Reject::Count)> rejected{};
			std::uint32_t samplers = 0;
			std::uint32_t unsupportedLayouts = 0;  // images DXVK does not keep in GENERAL
		};

		static GpuTextures& Get();

		/**
		 * @brief The texture's descriptor heap index, resolving it on first use; kInvalid if unsupported. A null
		 * view (nothing bound) resolves to a null descriptor, which reads zero as D3D11's null SRV does.
		 */
		std::uint32_t Resolve(ID3D11ShaderResourceView* a_view);

		/**
		 * @brief A view Resolve has already seen: its index (a null view: the null descriptor, once created),
		 * counted as a use. False for anything that would have to be imported, which only works inside an
		 * epoch - outside one Resolve rejects the view, and the rejection sticks.
		 */
		bool Known(ID3D11ShaderResourceView* a_view, std::uint32_t& a_index);

		/** @brief A null view's descriptor heap index (reads zero); kInvalid when unsupported. */
		std::uint32_t NullIndex();

		/** @brief The sampler heap index for the engine's (address mode, filter mode); kInvalid if unknown. */
		std::uint32_t Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode);

		/** @brief Once per epoch: releases entries unused for kEvictFrames. */
		void BeginFrame(std::uint32_t a_frame);

		/**
		 * @brief Changes whenever an entry is released (eviction, Clear): a view pointer may then be reused by
		 * a different view. While it is unchanged, an index Resolve returned for a view is still that view's.
		 */
		std::uint32_t Generation() const { return generation; }
		/**
		 * @brief How often a caller that keeps Resolve's results must resolve its views again, so that what it
		 * uses is never evicted (Resolve is what marks an entry used).
		 */
		static constexpr std::uint32_t kRestampFrames = 32;

		const Stats& GetStats() const { return stats; }

		void Clear();

		~GpuTextures();

	private:
		GpuTextures();

		static constexpr std::uint32_t kEvictFrames = 600;
		static_assert(kRestampFrames + 64 < kEvictFrames, "an entry restamped every kRestampFrames must outlive the eviction sweep");
		std::uint32_t generation = 0;

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
	};
}
