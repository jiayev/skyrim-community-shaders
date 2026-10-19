#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Portals
{
	/** @brief CPU-owned immutable context. Its identity travels with room membership, queries and verdicts. */
	struct Context
	{
		enum class Kind : std::uint32_t
		{
			Camera,
			PointLight
		};
		Kind kind = Kind::Camera;
		std::uint32_t frame = 0;
		std::uint64_t generation = 0, roomsGeneration = 0;
		std::array<float, 4> sphere{}, hemisphere{};
		bool strict = false, bothSides = true;
		std::vector<std::uint32_t> programs;
	};
	using ContextHold = std::shared_ptr<const Context>;

	/** @brief Independent room identity; a graph replacement invalidates it even if its numeric room index is reused. */
	struct Room
	{
		std::uint64_t generation = 0;
		std::uint32_t index = 0;  // 0: unbound, otherwise graph room index + 1
	};

	/** @brief GPU query ABI: one stable destination, independent of its temporary position in a program bucket. */
	struct Query
	{
		std::array<float, 4> bound{};
		std::uint32_t program = UINT32_MAX, flags = 0, destination = 0, generation = 0;
	};
	static_assert(sizeof(Query) == 32);

	/** @brief GPU result ABI. Consumers must match its generation before using the verdict. */
	struct Result
	{
		std::uint32_t generation = 0;
		std::int32_t visible = -1;
	};
	static_assert(sizeof(Result) == 8);

	/** @brief One program-homogeneous GPU workgroup. A short tail never borrows queries from a different program. */
	struct Task
	{
		std::uint32_t program = 0, first = 0, count = 0, padding = 0;
	};
	static_assert(sizeof(Task) == 16);
}
