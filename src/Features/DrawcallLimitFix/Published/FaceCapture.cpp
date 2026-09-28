#include "FaceCapture.h"

#include <cstring>
#include <limits>

namespace DCLF::Published
{
	std::optional<std::vector<std::vector<std::byte>>> CaptureHeadFaceValues(std::span<const FaceSnapshots::ShapeView> views)
	{
		if (views.empty()) return {};
		const auto& first = views.front();
		if (!first.owner || !first.generation) return {};
		std::vector<std::vector<std::byte>> result;
		result.reserve(views.size());
		for (const auto& view : views) {
			if (view.owner != first.owner || view.owner.owner_before(first.owner) || first.owner.owner_before(view.owner) ||
				view.generation != first.generation || !view.positions || !view.vertexCount) return {};
			const auto base = reinterpret_cast<std::uintptr_t>(view.owner->data());
			const auto address = reinterpret_cast<std::uintptr_t>(view.positions);
			if (address < base || (address - base) % sizeof(float)) return {};
			const auto offset = (address - base) / sizeof(float);
			if (offset > view.owner->size() || view.vertexCount > (view.owner->size() - offset) / 4) return {};
			const auto floats = static_cast<std::size_t>(view.vertexCount) * 4;
			if (floats > (std::numeric_limits<std::size_t>::max() - sizeof(FaceComponentHeader)) / sizeof(float)) return {};
			auto& bytes = result.emplace_back(sizeof(FaceComponentHeader) + floats * sizeof(float));
			const FaceComponentHeader header{ view.generation, view.vertexCount, 1 };
			std::memcpy(bytes.data(), &header, sizeof(header));
			// Read the retained owner, never dereference a possibly recycled raw view.
			std::memcpy(bytes.data() + sizeof(header), view.owner->data() + offset, floats * sizeof(float));
		}
		return result;
	}

	std::optional<std::vector<std::vector<std::byte>>> CaptureHeadFaceValues(const FaceSnapshots::HeadView& head)
	{
		if (!head.recordId || !head.generation || !head.owner || head.shapes.empty()) return {};
		std::vector<FaceSnapshots::ShapeView> views;
		views.reserve(head.shapes.size());
		std::size_t next = 0;
		for (std::size_t i = 0; i < head.shapes.size(); ++i) {
			const auto& shape = head.shapes[i];
			if (shape.ordinal != i || shape.offsetFloats != next || !shape.vertexCount ||
				next > head.owner->size() || shape.vertexCount > (head.owner->size() - next) / 4) return {};
			views.push_back({ head.owner->data() + next, shape.vertexCount, head.generation, head.owner });
			next += std::size_t(shape.vertexCount) * 4;
		}
		if (next != head.owner->size()) return {};
		return CaptureHeadFaceValues(views);
	}
}
