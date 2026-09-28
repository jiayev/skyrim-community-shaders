#include "ConstantMirror.h"

#include <cstring>

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
		if (entry->lastMapped && !entry->mapped)
			std::memcpy(entry->bytes.data(), entry->lastMapped, entry->bytes.size());
		return entry->bytes;
	}

	void ConstantMirror::OnMap(ID3D11Resource* a_resource, void* a_data)
	{
		if (auto* entry = Find(a_resource))
			entry->mapped = entry->lastMapped = a_data;
	}

	void ConstantMirror::OnUnmap(ID3D11Resource* a_resource)
	{
		auto* entry = Find(a_resource);
		if (!entry || !entry->mapped)
			return;
		// WRITE_DISCARD: whatever the writer left in the mapping is the buffer's new contents.
		std::memcpy(entry->bytes.data(), entry->mapped, entry->bytes.size());
		entry->mapped = nullptr;
		entry->valid = true;
	}

	void ConstantMirror::OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data)
	{
		auto* entry = Find(a_resource);
		if (!entry || !a_data)
			return;
		const std::size_t begin = a_box ? a_box->left : 0;
		const std::size_t end = a_box ? std::min<std::size_t>(a_box->right, entry->bytes.size()) : entry->bytes.size();
		if (begin < end)
			std::memcpy(entry->bytes.data() + begin, a_data, end - begin);
		entry->lastMapped = nullptr;
		// A partial update only counts once the buffer was fully written before.
		entry->valid |= !a_box;
	}
}
