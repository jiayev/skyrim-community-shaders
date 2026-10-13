#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct ID3D11Buffer;
struct ID3D11Resource;
struct D3D11_BOX;

namespace DCLF
{
	/**
	 * @brief CPU copies of D3D11 buffers the graph needs but must not lock: constant buffers and dynamic
	 * structured buffers the game and Community Shaders rewrite with Map(WRITE_DISCARD) or
	 * UpdateSubresource. A buffer is mirrored once it is watched: every later write through the immediate
	 * context lands in its copy too.
	 *
	 * D3D11 immediate-context thread only (the render thread), like the hooks that feed it.
	 */
	class ConstantMirror
	{
	public:
		static ConstantMirror& Get();

		/** @brief Starts mirroring a buffer (idempotent). Its copy is invalid until the next full write. */
		void Watch(ID3D11Buffer* a_buffer);

		/**
		 * @brief The buffer's current contents, or empty when not watched or never written since. A mapped
		 * buffer is re-read from its last mapping, which DXVK keeps valid until the next Map: the engine
		 * writes some constants after Unmap (engine notes: SetupTechnique).
		 */
		std::span<const std::byte> Contents(const ID3D11Buffer* a_buffer);

		// Immediate-context hooks.
		void OnMap(ID3D11Resource* a_resource, void* a_data);
		void OnUnmap(ID3D11Resource* a_resource);
		void OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data);

		bool Any() const { return !entries.empty(); }

	private:
		struct Entry
		{
			ID3D11Buffer* buffer = nullptr;  // referenced while watched
			std::vector<std::byte> bytes;
			void* mapped = nullptr;
			void* lastMapped = nullptr;  // null once UpdateSubresource wrote the buffer
			bool valid = false;
		};

		Entry* Find(const ID3D11Resource* a_resource);
		const Entry* Find(const ID3D11Resource* a_resource) const;

		std::vector<Entry> entries;  // a handful of buffers: a linear search is cheapest
	};
}
