#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Features/DrawcallLimitFix/Common/EventQueue.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/**
	 * @brief Shader-visible descriptors for the game's textures and samplers.
	 *
	 * A texture is resolved once per shader resource view: the image is marked stable in DXVK and imported
	 * into BasicRHI without ownership (dxvkGetInteropResourceInfo), and a view with the SRV's format,
	 * range and swizzle is written into a slot of ORG's shader-visible heap, read in the layout DXVK keeps
	 * the image in. The registry weakly deduplicates live imports; every numeric index handed to a draw
	 * is accompanied by ownership of its exact descriptor and SRV through GPU completion.
	 *
	 * An import waits on DXVK's CS thread (marking the image stable is a synchronous CS chunk) and on the driver
	 * (the view and its descriptor), for milliseconds while the game streams textures. Request therefore never imports
	 * on the calling thread: the import thread answers it, and a draw whose texture is still asked for is deferred, and so
	 * stays the engine's. ResolveBinding imports inline, for the frame's textures, whose draws cannot be deferred.
	 *
	 * Samplers copy the renderer's own D3D11 sampler states (the table the engine selects from by the
	 * shadow state's address and filter modes) into ORG's sampler heap.
	 *
	 * Render thread: inside an epoch (its descriptor service) or outside one (the graph's own, retained, as the import thread uses).
	 * Request, Seal and Fixed are any thread's (T6b2c: the scene work's material and shared bindings): a request goes to the import
	 * thread through a lock-free queue, which answers it as an event into the requester's queue, from the registry or by importing
	 * with the context the render thread published (PublishImportContext, with the null descriptor and the samplers: Fixed). The
	 * registry's lock is the render thread's (ResolveBinding) and the import thread's alone.
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
			std::uint64_t importedAsync = 0;   // imports the import thread made
			std::uint32_t requestsPending = 0;    // Request: not answered yet
			std::uint64_t requestsAnswered = 0;  // Request: answered
		};
		struct Binding
		{
			std::uint32_t index = kInvalid;
			std::shared_ptr<const void> owner;
			bool pending = false;  // a requester's own: asked for and not answered yet (no owner, kInvalid)
		};
		/** @brief Request's answer: the view's binding (never pending: kInvalid with no owner when the view is rejected). */
		struct Reply
		{
			ID3D11ShaderResourceView* view = nullptr;  // as requested (an identity: the request's reference is gone)
			std::uint64_t cookie = 0;                   // as requested
			Binding binding;
		};
		using Replies = EventQueue<Reply, 1024>;
		static constexpr std::uint32_t kSamplerCount = 4 * 5;  // the engine's (address mode, filter mode) table
		/**
		 * @brief The bindings every record reads that never change after they are made (the null descriptor, the engine's samplers by
		 * address * 5 + filter, kInvalid where the engine has none), published with the import context: immutable.
		 */
		struct FixedBindings
		{
			Binding null;
			std::array<Binding, kSamplerCount> samplers{};
		};

		static GpuTextures& Get();

		/** @brief Resolve with CPU ownership of this exact import and descriptor slot, importing inline if needed. */
		// sourceTag is diagnostic only: material t0-t15, feature 16+, projected 32+,
		// pipeline mask 48, frame tN as 64+N, shadow diffuse 192.
		Binding ResolveBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag = ~0u);

		/**
		 * @brief Any thread: a_view's binding, answered as an event into a_replies (its one consumer drains it; it lives as long as the
		 * process). a_view must be alive at the call: the request takes a reference, which the import becomes or which is let go
		 * once answered. The import thread answers from the registry, or imports the view; a request made before the render thread
		 * published a context waits for one. A null view is answered with the null descriptor.
		 */
		void Request(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, Replies& a_replies, std::uint64_t a_cookie);

		/**
		 * @brief Render thread, outside an epoch (the frame's start): what the import thread imports with - the graph's retained
		 * descriptor service, the cleanup queue, the device, the registry - and the fixed bindings (made here: the samplers read the
		 * engine's table), published when any changed.
		 */
		void PublishImportContext();

		/** @brief Any thread: the fixed bindings the render thread last published (PublishImportContext), null before the first. */
		std::shared_ptr<const FixedBindings> Fixed() const;

		/**
		 * @brief Any thread: one owner for a_owners (an immutable ORG execution lease on the published cleanup queue: its last release
		 * runs there), null when none is non-null. An owner Request handed out was made with the published context, so it is there.
		 */
		std::shared_ptr<const void> Seal(std::vector<std::shared_ptr<const void>> a_owners) const;

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
		std::uint32_t Generation() const { return generation.load(std::memory_order_acquire); }  // any thread
		Stats GetStats() const;

		void Clear();

		~GpuTextures();

	private:
		GpuTextures();

		std::atomic<std::uint32_t> generation{ 0 };

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
	};
}
