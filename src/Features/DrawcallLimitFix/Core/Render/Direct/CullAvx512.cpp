// The CPU cull's AVX-512 kernels (CullSimd.h), sixteen lanes at a time (F and BW); compiled with /arch:AVX512 and without the game's
// PCH. Reached only where Simd::Supported reports AVX-512.

#include "Features/DrawcallLimitFix/Core/Render/Direct/CullSimd.h"

#include <cstring>
#include <immintrin.h>

namespace DCLF::Direct::Simd
{
	void FrustumAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
		std::uint8_t* a_pass)
	{
		const __m128i one = _mm_set1_epi8(1);
		std::uint32_t i = 0;
		for (; i + 16 <= a_count; i += 16) {
			const __m512 x = _mm512_loadu_ps(a_spheres.x + i), y = _mm512_loadu_ps(a_spheres.y + i), z = _mm512_loadu_ps(a_spheres.z + i);
			const __m512 r = _mm512_loadu_ps(a_spheres.r + i);
			const __m512 reach = _mm512_sub_ps(_mm512_setzero_ps(), r);
			__mmask16 keep = 0xFFFF;
			for (std::uint32_t p = 0; p < a_planeCount && keep; ++p) {
				const float* plane = a_planes[p];
				// As the SSE pass: ((a * x + b * y) + c * z) - d against -r.
				const __m512 d = _mm512_sub_ps(_mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(_mm512_set1_ps(plane[0]), x), _mm512_mul_ps(_mm512_set1_ps(plane[1]), y)),
												   _mm512_mul_ps(_mm512_set1_ps(plane[2]), z)),
					_mm512_set1_ps(plane[3]));
				keep = _mm512_mask_cmp_ps_mask(keep, d, reach, _CMP_GE_OQ);
			}
			keep |= _mm512_cmp_ps_mask(r, _mm512_setzero_ps(), _CMP_LT_OQ);
			if (a_dead)
				keep &= static_cast<__mmask16>(~(a_dead[i >> 6] >> (i & 63)));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(a_pass + i), _mm_maskz_mov_epi8(keep, one));
		}
		for (; i < a_count; ++i) {
			bool keep = true;
			for (std::uint32_t p = 0; p < a_planeCount && keep; ++p) {
				const float* plane = a_planes[p];
				keep = plane[0] * a_spheres.x[i] + plane[1] * a_spheres.y[i] + plane[2] * a_spheres.z[i] - plane[3] >= -a_spheres.r[i];
			}
			keep = keep || a_spheres.r[i] < 0.0f;
			if (a_dead && ((a_dead[i >> 6] >> (i & 63)) & 1u))
				keep = false;
			a_pass[i] = keep ? 1u : 0u;
		}
	}

	std::uint32_t PlaneGroupAvx512(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		std::uint32_t a_lanes)
	{
		// Two vectors of sixteen: the second only while a bound of a_lanes is in it.
		const bool high = (a_lanes >> 16) != 0;
		const __m512 x0 = _mm512_loadu_ps(a_x), y0 = _mm512_loadu_ps(a_y), z0 = _mm512_loadu_ps(a_z), r0 = _mm512_loadu_ps(a_r);
		const __m512 minus0 = _mm512_sub_ps(_mm512_setzero_ps(), r0);
		__m512 x1 = x0, y1 = y0, z1 = z0, r1 = r0, minus1 = minus0;
		if (high) {
			x1 = _mm512_loadu_ps(a_x + 16), y1 = _mm512_loadu_ps(a_y + 16), z1 = _mm512_loadu_ps(a_z + 16), r1 = _mm512_loadu_ps(a_r + 16);
			minus1 = _mm512_sub_ps(_mm512_setzero_ps(), r1);
		}
		std::uint32_t outside = 0, notContained = 0;
		std::uint32_t mask;
		std::memcpy(&mask, a_set + 24, 4);
		for (std::uint32_t rest = mask & 0x3Fu; rest; rest &= rest - 1) {
			float n[4];
			std::memcpy(n, a_set + std::size_t(_tzcnt_u32(rest)) * 4, 16);
			const __m512 nx = _mm512_set1_ps(n[0]), ny = _mm512_set1_ps(n[1]), nz = _mm512_set1_ps(n[2]), d0 = _mm512_set1_ps(n[3]);
			const __m512 d = _mm512_sub_ps(_mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(ny, y0), _mm512_mul_ps(nx, x0)), _mm512_mul_ps(z0, nz)), d0);
			std::uint32_t behind = _mm512_cmp_ps_mask(d, minus0, _CMP_LE_OQ);
			std::uint32_t near = _mm512_cmp_ps_mask(d, r0, _CMP_LT_OQ);
			if (high) {
				const __m512 e = _mm512_sub_ps(_mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(ny, y1), _mm512_mul_ps(nx, x1)), _mm512_mul_ps(z1, nz)), d0);
				behind |= static_cast<std::uint32_t>(_mm512_cmp_ps_mask(e, minus1, _CMP_LE_OQ)) << 16;
				near |= static_cast<std::uint32_t>(_mm512_cmp_ps_mask(e, r1, _CMP_LT_OQ)) << 16;
			}
			outside |= behind;
			notContained |= behind | near;
			if (((a_kind == 7u ? outside : notContained) & a_lanes) == a_lanes)
				break;
		}
		return (a_kind == 7u ? ~outside : notContained) & a_lanes;
	}

	void ParabolicAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags, const std::uint64_t* a_dead,
		std::uint8_t* a_pass)
	{
		const __m512 sx = _mm512_set1_ps(a_sphere[0]), sy = _mm512_set1_ps(a_sphere[1]), sz = _mm512_set1_ps(a_sphere[2]), sr = _mm512_set1_ps(a_sphere[3]);
		const __m512 px = _mm512_set1_ps(a_plane[0]), py = _mm512_set1_ps(a_plane[1]), pz = _mm512_set1_ps(a_plane[2]), pw = _mm512_set1_ps(a_plane[3]);
		const __m512 zero = _mm512_setzero_ps();
		const __m128i one = _mm_set1_epi8(1);
		const bool back = (a_flags & kParabolicBack) != 0, both = (a_flags & kParabolicBothSides) != 0;
		std::uint32_t i = 0;
		for (; i + 16 <= a_count; i += 16) {
			const __m512 x = _mm512_loadu_ps(a_spheres.x + i), y = _mm512_loadu_ps(a_spheres.y + i), z = _mm512_loadu_ps(a_spheres.z + i);
			const __m512 r = _mm512_loadu_ps(a_spheres.r + i);
			const __m512 dx = _mm512_sub_ps(x, sx), dy = _mm512_sub_ps(y, sy), dz = _mm512_sub_ps(z, sz);
			const __m512 reach = _mm512_sub_ps(_mm512_sub_ps(_mm512_sqrt_ps(_mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(dx, dx), _mm512_mul_ps(dy, dy)), _mm512_mul_ps(dz, dz))), r), sr);
			const __mmask16 meets = _mm512_cmp_ps_mask(reach, zero, _CMP_LT_OQ);
			const __m512 side = _mm512_sub_ps(_mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(y, py), _mm512_mul_ps(x, px)), _mm512_mul_ps(z, pz)), pw);
			const __mmask16 straddles = _mm512_cmp_ps_mask(_mm512_sub_ps(side, r), zero, _CMP_LT_OQ);
			const __mmask16 front = _mm512_cmp_ps_mask(side, zero, _CMP_GT_OQ);
			__mmask16 keep = meets & (back ? (both ? static_cast<__mmask16>(straddles | ~front) : static_cast<__mmask16>(0)) : static_cast<__mmask16>(straddles | front));
			keep |= _mm512_cmp_ps_mask(r, zero, _CMP_LT_OQ);
			if (a_dead)
				keep &= static_cast<__mmask16>(~(a_dead[i >> 6] >> (i & 63)));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(a_pass + i), _mm_maskz_mov_epi8(keep, one));
		}
		for (; i < a_count; ++i) {
			const bool dead = a_dead && ((a_dead[i >> 6] >> (i & 63)) & 1u);
			a_pass[i] = !dead && ParabolicKept(a_spheres.x[i], a_spheres.y[i], a_spheres.z[i], a_spheres.r[i], a_sphere, a_plane, a_flags) ? 1u : 0u;
		}
	}

	void PlaneChunksAvx512(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks)
	{
		// The set's active planes, broadcast once.
		std::uint32_t mask;
		std::memcpy(&mask, a_set + 24, 4);
		__m512 nx[6], ny[6], nz[6], d0[6];
		std::uint32_t planes = 0;
		for (std::uint32_t rest = mask & 0x3Fu; rest; rest &= rest - 1, ++planes) {
			float n[4];
			std::memcpy(n, a_set + std::size_t(_tzcnt_u32(rest)) * 4, 16);
			nx[planes] = _mm512_set1_ps(n[0]), ny[planes] = _mm512_set1_ps(n[1]), nz[planes] = _mm512_set1_ps(n[2]), d0[planes] = _mm512_set1_ps(n[3]);
		}
		for (std::uint32_t c = 0; c < a_chunks; ++c) {
			const auto lanes = static_cast<__mmask16>(a_lanes[c]);
			if (!lanes) {
				a_pass[c] = 0;
				continue;
			}
			const __m512 x = _mm512_loadu_ps(a_x + 16 * c), y = _mm512_loadu_ps(a_y + 16 * c), z = _mm512_loadu_ps(a_z + 16 * c), r = _mm512_loadu_ps(a_r + 16 * c);
			const __m512 minusR = _mm512_sub_ps(_mm512_setzero_ps(), r);
			__mmask16 outside = 0, notContained = 0;
			for (std::uint32_t p = 0; p < planes; ++p) {
				const __m512 d = _mm512_sub_ps(_mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(ny[p], y), _mm512_mul_ps(nx[p], x)), _mm512_mul_ps(z, nz[p])), d0[p]);
				const __mmask16 behind = _mm512_cmp_ps_mask(d, minusR, _CMP_LE_OQ);
				outside |= behind;
				notContained |= behind | _mm512_cmp_ps_mask(d, r, _CMP_LT_OQ);
			}
			a_pass[c] = static_cast<std::uint16_t>((a_kind == 7u ? static_cast<__mmask16>(~outside) : notContained) & lanes);
		}
	}
}
