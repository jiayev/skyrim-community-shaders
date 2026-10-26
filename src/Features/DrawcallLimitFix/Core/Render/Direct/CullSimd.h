#pragma once

// The CPU cull's wide kernels (Cull.cpp's FrustumPass, 8 or 16 lanes at a time): CullAvx2.cpp and CullAvx512.cpp, each compiled for its
// instruction set alone (cmake/RenderGraph.cmake: their /arch, without the game's PCH, which is built for the baseline), and chosen at run
// time by what the CPU and the OS support (Level). Self-contained: plain types, so the kernels' files include nothing of the game's.
// Each computes what the SSE pass does, in its operations' order (no FMA), so its verdicts are the same bit for bit (kExpCullWideCheck).

#include <cmath>
#include <cstdint>

namespace DCLF::Direct::Simd
{
	/** @brief A batch's sphere columns (ViewBatch's live or snapshot spheres). */
	struct Spheres
	{
		const float *x, *y, *z, *r;
	};

	/**
	 * @brief The frustum over a_count lanes: a_pass[i] 1 inside every plane of a_planes (a_planeCount of (n, d), the bound outside where
	 * dot(n, c) - d < -r) or with a radius below 0 (kept), else 0; and 0 for a lane set in a_dead (a tombstone bitmap, null: none). A group of
	 * lanes stops testing planes once every lane in it is outside one.
	 */
	using FrustumFn = void (*)(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);

	void FrustumAvx2(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);
	void FrustumAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);

	/** @brief ParabolicFn's view flags: the paraboloid draws both hemispheres; it is the back hemisphere's view. */
	inline constexpr std::uint32_t kParabolicBothSides = 1u, kParabolicBack = 2u;

	/**
	 * @brief BuildDrawsCS's ParabolicRejected over a_count lanes, whole: a_pass[i] 1 where the lane is kept (a radius below 0 is), else 0, and
	 * 0 for a lane set in a_dead. a_sphere is the view's (centre, radius), a_plane its hemisphere plane, in ParabolicSphereMask's order:
	 * (sqrt((dx * dx + dy * dy) + dz * dz) - r) - R < 0 meets the sphere; side ((y * py + x * px) + z * pz) - pw, side - r < 0 straddles it,
	 * side > 0 is in front. The front hemisphere keeps codes 1 and 3, the back one 2 and 3.
	 */
	using ParabolicFn = void (*)(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags,
		const std::uint64_t* a_dead, std::uint8_t* a_pass);

	void ParabolicAvx2(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);
	void ParabolicAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);

	/** @brief One lane of ParabolicFn, scalar (the kernels' tails; Cull.cpp's check), in the same order. */
	inline bool ParabolicKept(float a_x, float a_y, float a_z, float a_r, const float* a_sphere, const float* a_plane, std::uint32_t a_flags)
	{
		if (a_r < 0.0f)
			return true;
		const float dx = a_x - a_sphere[0], dy = a_y - a_sphere[1], dz = a_z - a_sphere[2];
		const float squared = (dx * dx + dy * dy) + dz * dz;
		// The kernels' sqrt is the vector one: correctly rounded, as the scalar one.
		if (!((std::sqrt(squared) - a_r) - a_sphere[3] < 0.0f))
			return false;
		const float side = ((a_y * a_plane[1] + a_x * a_plane[0]) + a_z * a_plane[2]) - a_plane[3];
		const bool straddles = side - a_r < 0.0f, front = side > 0.0f;
		const bool both = (a_flags & kParabolicBothSides) != 0;
		return (a_flags & kParabolicBack) ? both && (straddles || !front) : (straddles || front);
	}

	/**
	 * @brief PortalPrograms' plane set test (a_kind 7: not outside; 8: not contained) over up to 32 bounds at once (a_x, a_y, a_z, a_r: 32
	 * floats each), as a mask of a_lanes: the active planes of the set at a_set (six (n, d) as the words hold them, the active mask at word
	 * 24), in plane order, stopping once every bound in a_lanes is decided (the verdict is an OR over the planes); only the vectors holding a
	 * bound of a_lanes are tested. PortalDistance's order:
	 * ((n.y * c.y + n.x * c.x) + c.z * n.z) - d.
	 */
	using PlaneGroupFn = std::uint32_t (*)(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		std::uint32_t a_lanes);

	std::uint32_t PlaneGroupAvx2(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		std::uint32_t a_lanes);
	std::uint32_t PlaneGroupAvx512(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		std::uint32_t a_lanes);

	/**
	 * @brief PlaneGroupFn over a_chunks chunks of 16 bounds (a_x, a_y, a_z, a_r: 16 * a_chunks floats each): a_pass[c] the test's mask of
	 * a_lanes[c] (0 where a_lanes[c] is 0), the set's planes read once. Every active plane is evaluated (no early stop: the plane count is
	 * the call's, so the loop is predictable; the verdict, an OR over the planes, is PlaneGroupFn's).
	 */
	using PlaneChunksFn = void (*)(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks);

	void PlaneChunksAvx2(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks);
	void PlaneChunksAvx512(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks);

	/** @brief The widest kernels the CPU and the OS support (AVX2: AVX2 and the YMM state; AVX-512: F and BW and the ZMM state). */
	enum class Level : std::uint8_t
	{
		Sse,
		Avx2,
		Avx512,
	};
	Level Supported();
	const char* NameOf(Level a_level);
	/** @brief The kernels the cull takes now: Supported, as far as Direct::kExpCullWide and kExpCullAvx512 allow (Cull.cpp). */
	Level Chosen();
}
