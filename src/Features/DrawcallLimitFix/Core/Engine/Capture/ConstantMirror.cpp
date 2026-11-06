// ConstantMirror: the watched buffers' CPU copies, kept by the immediate-context hooks on the render thread.

#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"

#include <cstring>
#include <utility>

namespace DCLF
{
	ConstantMirror& ConstantMirror::Get()
	{
		static ConstantMirror mirror;
		return mirror;
	}

	ConstantMirror::Entry* ConstantMirror::Find(const ID3D11Resource* a_resource)
	{
		for (auto& entry : entries) {
			if (reinterpret_cast<const ID3D11Resource*>(entry.buffer) == a_resource)
				return &entry;
		}
		return nullptr;
	}

	const ConstantMirror::Entry* ConstantMirror::Find(const ID3D11Resource* a_resource) const
	{
		return const_cast<ConstantMirror*>(this)->Find(a_resource);
	}

	void ConstantMirror::Watch(ID3D11Buffer* a_buffer)
	{
		if (!a_buffer || Find(reinterpret_cast<ID3D11Resource*>(a_buffer)))
			return;
		D3D11_BUFFER_DESC desc{};
		a_buffer->GetDesc(&desc);
		a_buffer->AddRef();
		Entry entry;
		entry.buffer = a_buffer;
		entry.bytes.resize(desc.ByteWidth);
		entries.push_back(std::move(entry));
	}

	std::span<const std::byte> ConstantMirror::Contents(const ID3D11Buffer* a_buffer)
	{
		auto* entry = Find(reinterpret_cast<const ID3D11Resource*>(a_buffer));
		if (!entry || !entry->valid)
			return {};
		// The mapping's memory stays the writer's last contents after Unmap (DXVK keeps the slice until it is reused).
		if (entry->unread)
			std::memcpy(entry->bytes.data(), std::exchange(entry->unread, nullptr), entry->bytes.size());
		return entry->bytes;
	}

	void ConstantMirror::OnMap(ID3D11Resource* a_resource, void* a_data)
	{
		if (auto* entry = Find(a_resource))
			entry->mapped = a_data;
	}

	void ConstantMirror::OnUnmap(ID3D11Resource* a_resource)
	{
		auto* entry = Find(a_resource);
		if (!entry || !entry->mapped)
			return;
		// WRITE_DISCARD: whatever the writer left in the mapping is the buffer's new contents (copied when read: Contents).
		entry->unread = std::exchange(entry->mapped, nullptr);
		entry->valid = true;
	}

	void ConstantMirror::OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data)
	{
		auto* entry = Find(a_resource);
		if (!entry || !a_data)
			return;
		// A partial update applies over the last mapping's contents.
		if (entry->unread)
			std::memcpy(entry->bytes.data(), std::exchange(entry->unread, nullptr), entry->bytes.size());
		const std::size_t begin = a_box ? a_box->left : 0;
		const std::size_t end = a_box ? std::min<std::size_t>(a_box->right, entry->bytes.size()) : entry->bytes.size();
		if (begin < end)
			std::memcpy(entry->bytes.data() + begin, a_data, end - begin);
		// A partial update only counts once the buffer was fully written before.
		entry->valid |= !a_box;
	}
}
