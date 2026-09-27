#pragma once

#include "FaceSnapshots.h"

#include <cstddef>
#include <optional>
#include <span>

namespace DCLF::Published
{
	// CPU capture component schema; not a GPU buffer or disk/wire format.
	struct FaceComponentHeader
	{
		std::uint64_t generation = 0;
		std::uint32_t vertexCount = 0;
		std::uint32_t schema = 1;
	};
	static_assert(sizeof(FaceComponentHeader) == 16);
	// Call with views from ONE complete head at the existing safe scene-walk
	// boundary. Missing, recycled or mixed-generation inputs reject the whole head.
	// Each result is header + float4 positions, ready for ComponentKind::Face.
	// Completeness of head membership still belongs to the caller's group capture.
	std::optional<std::vector<std::vector<std::byte>>> CaptureHeadFaceValues(std::span<const FaceSnapshots::ShapeView>);
	std::optional<std::vector<std::vector<std::byte>>> CaptureHeadFaceValues(const FaceSnapshots::HeadView&);
}
