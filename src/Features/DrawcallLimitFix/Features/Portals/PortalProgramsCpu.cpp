// PortalPrograms.hlsli on the CPU (PortalProgramsCpu.h): PortalTestAt, PortalProgramRun, PortalPlaneSetTest and StrictGateRejects, word for
// word over the same encodings. Keep the three in step with the shader.

#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramsCpu.h"

#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/CullSimd.h"

#include <atomic>
#include <bit>
#include <algorithm>
#include <vector>

#include <emmintrin.h>

namespace DCLF::Portals
{
	namespace
	{
		constexpr std::size_t kSetWords = kPortalSetBytes / 4, kHeaderWords = kPortalHeaderBytes / 4, kEntryWords = kPortalDirectoryBytes / 4,
							  kOpWords = kPortalOpBytes / 4;
		constexpr std::size_t kMaskWord = 24;  // a plane set's active mask (+96)
		constexpr std::size_t kStrictHeaderWords = kGateHeaderWords;
		constexpr std::size_t kContextEntryWords = 64 / 4;  // PortalContextAtlas.h's directory entry

		/** @brief The words at a span's offset, 0 past its end (a malformed stream then fails the walk's checks, never reads outside). */
		struct Words
		{
			std::span<const std::uint32_t> words;
			std::uint32_t operator[](std::size_t a_at) const { return a_at < words.size() ? words[a_at] : 0u; }
			float Float(std::size_t a_at) const { return std::bit_cast<float>((*this)[a_at]); }
		};

		/** @brief PortalDistance: y, then x, then z, less the constant. */
		float Distance(const Words& a_w, std::size_t a_plane, const float a_c[3])
		{
			return ((a_w.Float(a_plane + 1) * a_c[1] + a_w.Float(a_plane) * a_c[0]) + a_c[2] * a_w.Float(a_plane + 2)) - a_w.Float(a_plane + 3);
		}

		/** @brief PortalViewTest: outside when the bound is wholly behind an active view plane. */
		bool ViewTest(const Words& a_w, std::size_t a_base, const float a_c[3], float a_r)
		{
			const std::uint32_t mask = a_w[a_base + kMaskWord];
			for (std::uint32_t p = 0; p < 6; ++p)
				if ((mask & (1u << p)) && Distance(a_w, a_base + p * 4, a_c) <= -a_r)
					return false;
			return true;
		}

		// Direct::kExpCullProfile: the main camera's tests' steps and plane evaluations, logged every 2^20 tests (diagnostic).
		std::atomic<std::uint64_t> profileTests{ 0 }, profileSteps{ 0 }, profilePlanes{ 0 }, profileOps{ 0 };
		thread_local std::uint64_t threadSteps = 0, threadPlanes = 0;

		/**
		 * @brief PortalPlaneSetTest (7: not outside, 8: not contained). The shader's is one fixed six-plane calculation; the verdict is an OR
		 * over the active planes, so stopping at the first plane that decides it (each plane's distance as the shader computes it) is
		 * the same verdict (Direct::kExpPortalEarly; cleared, the fixed calculation).
		 */
		bool PlaneSetTest(const Words& a_w, std::size_t a_set, std::uint32_t a_kind, const float a_c[3], float a_r)
		{
			const std::uint32_t mask = a_w[a_set + kMaskWord];
			if (Direct::ExperimentOn(Direct::kExpPortalEarly)) {
				for (std::uint32_t rest = mask & 0x3Fu; rest; rest &= rest - 1) {
					++threadPlanes;
					const float d = Distance(a_w, a_set + std::size_t(std::countr_zero(rest)) * 4, a_c);
					if (a_kind == 7u ? d <= -a_r : (d <= -a_r || d < a_r))
						return a_kind != 7u;
				}
				return a_kind == 7u;
			}
			bool outside = false, notContained = false;
			for (std::uint32_t p = 0; p < 6; ++p) {
				const bool active = (mask & (1u << p)) != 0;
				const float d = Distance(a_w, a_set + p * 4, a_c);
				outside = outside || (active && d <= -a_r);
				notContained = notContained || (active && (d <= -a_r || d < a_r));
			}
			return a_kind == 7u ? !outside : notContained;
		}

		/** @brief PortalProgramRun's default (short-circuit) walk: a producer invariant failure never accepts a bound. */
		bool ProgramRun(const Words& a_w, std::size_t a_entry, std::size_t a_recordsBase, std::size_t a_setsBase, const float a_c[3], float a_r)
		{
			const std::uint32_t ops = a_w[a_entry + 1], sets = a_w[a_entry + 2];
			const std::size_t records = a_recordsBase + std::size_t(a_w[a_entry + 4]) * kOpWords;
			const std::size_t setBase = a_setsBase + std::size_t(a_w[a_entry + 5]) * kSetWords;
			std::uint32_t op = a_w[a_entry + 3];
			const std::uint32_t steps = a_w[a_entry + 7];
			for (std::uint32_t step = 0; step < steps; ++step) {
				if (op >= ops)
					return false;
				const std::size_t record = records + std::size_t(op) * kOpWords;
				const std::uint32_t kind = a_w[record];
				if (kind == 2u || kind == 3u)
					return kind == 2u;
				const std::uint32_t set = a_w[record + 1];
				if ((kind != 7u && kind != 8u) || set >= sets)
					return false;
				++threadSteps;
				op = PlaneSetTest(a_w, setBase + std::size_t(set) * kSetWords, kind, a_c, a_r) ? a_w[record + 2] : a_w[record + 3];
			}
			return false;
		}

		/** @brief StrictProgramPass: Process1's compound test on a strict light's room program (no view planes). */
		bool StrictProgramPass(const Words& a_w, std::size_t a_base, std::uint32_t a_program, const float a_c[3], float a_r)
		{
			const std::uint32_t count = a_w[a_base + kSetWords];
			if (a_program >= count)
				return true;
			const std::size_t entry = a_base + kHeaderWords + std::size_t(a_program) * kEntryWords;
			if (a_w[entry + 1] == 0u)
				return true;
			const std::size_t recordsBase = a_base + kHeaderWords + std::size_t(count) * kEntryWords;
			const std::size_t setsBase = recordsBase + std::size_t(a_w[a_base + kSetWords + 1]) * kOpWords;
			return ProgramRun(a_w, entry, recordsBase, setsBase, a_c, a_r);
		}
	}

	int PortalTestCpuAt(std::span<const std::uint32_t> a_words, std::size_t a_baseWord, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags)
	{
		const Words w{ a_words };
		const std::uint32_t count = w[a_baseWord + kSetWords];
		if (a_program == kPortalNoProgram || a_program >= count)
			return -1;
		const std::size_t entry = a_baseWord + kHeaderWords + std::size_t(a_program) * kEntryWords;
		const std::uint32_t flags = w[entry], ops = w[entry + 1], cullMode = w[entry + 6];
		const bool alwaysDraw = (a_nodeFlags >> 11) & 1u, preprocessed = (a_nodeFlags >> 12) & 1u, hidden = (a_nodeFlags >> 20) & 1u;
		if (a_radius == 0.0f && !alwaysDraw)
			return 0;
		// A room the walk did not reach is not culled into.
		if (a_program != 0u && !(flags & kPortalReached))
			return 0;
		if (cullMode == 2u)
			return 0;
		if (cullMode == 1u || alwaysDraw)
			return 1;
		if (preprocessed && cullMode != 4u)
			return hidden ? 0 : 1;
		const bool view = ViewTest(w, a_baseWord, a_centre, a_radius);
		if (cullMode == 3u || ops == 0u)
			return view ? 1 : 0;
		if (!(flags & kPortalSkipView) && !view)
			return 0;
		const std::size_t recordsBase = a_baseWord + kHeaderWords + std::size_t(count) * kEntryWords;
		const std::size_t setsBase = recordsBase + std::size_t(w[a_baseWord + kSetWords + 1]) * kOpWords;
		return ProgramRun(w, entry, recordsBase, setsBase, a_centre, a_radius) ? 1 : 0;
	}

	int PortalTestCpu(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags)
	{
		if (!Direct::ExperimentOn(Direct::kExpCullProfile))
			return PortalTestCpuAt(a_programs, 0, a_program, a_centre, a_radius, a_nodeFlags);
		threadSteps = threadPlanes = 0;
		const int verdict = PortalTestCpuAt(a_programs, 0, a_program, a_centre, a_radius, a_nodeFlags);
		profileSteps.fetch_add(threadSteps, std::memory_order_relaxed);
		profilePlanes.fetch_add(threadPlanes, std::memory_order_relaxed);
		const Words w{ a_programs };
		profileOps.fetch_add(a_program < w[kSetWords] ? w[kHeaderWords + std::size_t(a_program) * kEntryWords + 1] : 0u, std::memory_order_relaxed);
		if (const auto n = profileTests.fetch_add(1, std::memory_order_relaxed) + 1; (n & ((1u << 20) - 1)) == 0) {
			const auto steps = profileSteps.exchange(0), planes = profilePlanes.exchange(0), ops = profileOps.exchange(0);
			logger::info("[DCLF] portal profile: {} programs; a test's program {:.1f} operators, {:.1f} plane set steps, {:.1f} planes evaluated (last 2^20 tests)",
				w[kSetWords], ops / 1048576.0, steps / 1048576.0, planes / 1048576.0);
		}
		return verdict;
	}

	namespace
	{
		// The most bounds a group walk carries (without kExpPortalOpMajor: 16; the kernels take up to 32).
		constexpr std::uint32_t kGroup = 32;

		/** @brief Simd::PlaneGroupFn in SSE: eight vectors of four, a vector only while a bound of a_lanes is in it. */
		std::uint32_t PlaneGroupSse(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
			std::uint32_t a_lanes)
		{
			std::uint32_t outside = 0, notContained = 0;
			for (std::uint32_t rest = a_set[kMaskWord] & 0x3Fu; rest; rest &= rest - 1) {
				const std::uint32_t* plane = a_set + std::size_t(std::countr_zero(rest)) * 4;
				const __m128 nx = _mm_set1_ps(std::bit_cast<float>(plane[0])), ny = _mm_set1_ps(std::bit_cast<float>(plane[1])),
							 nz = _mm_set1_ps(std::bit_cast<float>(plane[2])), d0 = _mm_set1_ps(std::bit_cast<float>(plane[3]));
				for (std::uint32_t v = 0; v < kGroup / 4; ++v) {
					if (!((a_lanes >> (4 * v)) & 0xFu))
						continue;
					const __m128 x = _mm_loadu_ps(a_x + 4 * v), y = _mm_loadu_ps(a_y + 4 * v), z = _mm_loadu_ps(a_z + 4 * v), r = _mm_loadu_ps(a_r + 4 * v);
					// PortalDistance: ((n.y * c.y + n.x * c.x) + c.z * n.z) - d, the shader's order.
					const __m128 d = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ny, y), _mm_mul_ps(nx, x)), _mm_mul_ps(z, nz)), d0);
					const auto behind = static_cast<std::uint32_t>(_mm_movemask_ps(_mm_cmple_ps(d, _mm_sub_ps(_mm_setzero_ps(), r))));
					outside |= behind << (4 * v);
					notContained |= (behind | static_cast<std::uint32_t>(_mm_movemask_ps(_mm_cmplt_ps(d, r)))) << (4 * v);
				}
				if (((a_kind == 7u ? outside : notContained) & a_lanes) == a_lanes)
					break;
			}
			// Kind 7 passes the bounds not outside; kind 8 those not contained (no active plane: none outside, none not contained).
			return (a_kind == 7u ? ~outside : notContained) & a_lanes;
		}

		/** @brief Simd::PlaneChunksFn in SSE: each chunk's PlaneGroupSse. */
		void PlaneChunksSse(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
			const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks)
		{
			for (std::uint32_t c = 0; c < a_chunks; ++c)
				a_pass[c] = a_lanes[c] ? static_cast<std::uint16_t>(PlaneGroupSse(a_set, a_kind, a_x + 16 * c, a_y + 16 * c, a_z + 16 * c, a_r + 16 * c, a_lanes[c])) : 0u;
		}

		Direct::Simd::PlaneChunksFn PlaneChunksOf(Direct::Simd::Level a_level)
		{
			return a_level == Direct::Simd::Level::Avx512 ? &Direct::Simd::PlaneChunksAvx512 :
			       a_level == Direct::Simd::Level::Avx2   ? &Direct::Simd::PlaneChunksAvx2 :
			                                                &PlaneChunksSse;
		}

		/**
		 * @brief The batch test walked once over all a_count bounds: the program's operators in order (each mask it sets is an operator after
		 * the one setting it), each over every 16-bound chunk routed to it.
		 */
		void WalkOpMajor(const std::uint32_t* a_w, const std::uint32_t* a_records, const std::uint32_t* a_programSets, std::uint32_t a_sets, std::uint32_t a_ops,
			std::uint32_t a_first, std::uint32_t a_flags, bool a_viewOnly, Direct::Simd::PlaneChunksFn a_chunksTest, const float* a_x, const float* a_y, const float* a_z,
			const float* a_r, const std::uint16_t* a_lanes, std::uint32_t a_count, std::uint8_t* a_rejected)
		{
			const std::uint32_t chunks = (a_count + 15) / 16;
			thread_local std::vector<float> x, y, z, r;
			thread_local std::vector<std::uint16_t> live, inView, pass, routes;
			x.assign(std::size_t(chunks) * 16, 0.0f), y.assign(x.size(), 0.0f), z.assign(x.size(), 0.0f), r.assign(x.size(), 0.0f);
			live.assign(chunks, 0u), inView.assign(chunks, 0u), pass.resize(chunks);
			for (std::uint32_t k = 0; k < a_count; ++k) {
				const std::uint16_t i = a_lanes[k];
				x[k] = a_x[i], y[k] = a_y[i], z[k] = a_z[i], r[k] = a_r[i];
				if (r[k] != 0.0f)
					live[k / 16] |= static_cast<std::uint16_t>(1u << (k % 16));  // a zero radius: culled
			}
			// The camera's view planes (the header's plane set), then the walk from the first operator.
			a_chunksTest(a_w, 7u, x.data(), y.data(), z.data(), r.data(), live.data(), pass.data(), chunks);
			if (a_viewOnly) {
				inView.assign(pass.begin(), pass.end());
			} else if (a_first < a_ops) {
				// Each operator's chunk masks (routes[op * chunks + c]), cleared as the walk takes them: zero between calls.
				if (routes.size() < std::size_t(a_ops) * chunks)
					routes.resize(std::size_t(a_ops) * chunks, 0u);
				std::uint16_t* firstRoute = routes.data() + std::size_t(a_first) * chunks;
				bool any = false;
				for (std::uint32_t c = 0; c < chunks; ++c) {
					firstRoute[c] = (a_flags & kPortalSkipView) ? live[c] : pass[c];
					any |= firstRoute[c] != 0;
				}
				for (std::uint32_t op = a_first; op < a_ops && any; ++op) {
					std::uint16_t* m = routes.data() + std::size_t(op) * chunks;
					bool reached = false;
					for (std::uint32_t c = 0; c < chunks && !reached; ++c)
						reached = m[c] != 0;
					if (!reached)
						continue;
					const std::uint32_t* record = a_records + std::size_t(op) * kOpWords;
					const std::uint32_t kind = record[0], set = record[1];
					if (kind == 2u)
						for (std::uint32_t c = 0; c < chunks; ++c)
							inView[c] |= m[c];
					if ((kind == 7u || kind == 8u) && set < a_sets) {
						a_chunksTest(a_programSets + std::size_t(set) * kSetWords, kind, x.data(), y.data(), z.data(), r.data(), m, pass.data(), chunks);
						std::uint16_t* passRoute = record[2] < a_ops ? routes.data() + std::size_t(record[2]) * chunks : nullptr;
						std::uint16_t* failRoute = record[3] < a_ops ? routes.data() + std::size_t(record[3]) * chunks : nullptr;
						for (std::uint32_t c = 0; c < chunks; ++c) {
							if (passRoute)
								passRoute[c] |= pass[c];
							if (failRoute)
								failRoute[c] |= static_cast<std::uint16_t>(m[c] & ~pass[c]);
						}
					}
					for (std::uint32_t c = 0; c < chunks; ++c)
						m[c] = 0;
				}
				// Operators the walk stopped before (any was false: none reached) hold none; those past the last reached were cleared.
			}
			for (std::uint32_t k = 0; k < a_count; ++k)
				a_rejected[a_lanes[k]] = (inView[k / 16] >> (k % 16)) & 1u ? 0u : 1u;
		}

		Direct::Simd::PlaneGroupFn PlaneGroupOf(Direct::Simd::Level a_level)
		{
			return a_level == Direct::Simd::Level::Avx512 ? &Direct::Simd::PlaneGroupAvx512 :
			       a_level == Direct::Simd::Level::Avx2   ? &Direct::Simd::PlaneGroupAvx2 :
			                                                &PlaneGroupSse;
		}
	}

	void PortalTestCpuBatch(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint32_t a_count, std::uint8_t* a_rejected)
	{
		// The producer's programs are trusted (PortalPrograms.h): forward-only operators within their bounds, read in place.
		const std::uint32_t* w = a_programs.data();
		const std::uint32_t count = a_programs.size() > kSetWords ? w[kSetWords] : 0u;
		if (a_program == kPortalNoProgram || a_program >= count) {
			for (std::uint32_t k = 0; k < a_count; ++k)
				a_rejected[a_lanes[k]] = 1;  // -1
			return;
		}
		const std::size_t entry = kHeaderWords + std::size_t(a_program) * kEntryWords;
		const std::uint32_t flags = w[entry], ops = w[entry + 1], cullMode = w[entry + 6];
		const std::size_t recordsBase = kHeaderWords + std::size_t(count) * kEntryWords;
		const std::size_t setsBase = recordsBase + std::size_t(w[kSetWords + 1]) * kOpWords;
		const std::uint32_t sets = w[entry + 2], first = w[entry + 3];
		const std::uint32_t* records = w + recordsBase + std::size_t(w[entry + 4]) * kOpWords;
		const std::uint32_t* programSets = w + setsBase + std::size_t(w[entry + 5]) * kSetWords;
		// The whole program's verdict, whatever the bound (PortalTestCpuAt's order; a zero radius is culled before them).
		int whole = -1;
		if (a_program != 0u && !(flags & kPortalReached))
			whole = 0;
		else if (cullMode == 2u)
			whole = 0;
		else if (cullMode == 1u)
			whole = 1;
		const bool viewOnly = cullMode == 3u || ops == 0u;
		const auto planeGroup = PlaneGroupOf(Direct::Simd::Chosen());
		if (whole < 0 && Direct::ExperimentOn(Direct::kExpPortalOpMajor)) {
			WalkOpMajor(w, records, programSets, sets, ops, first, flags, viewOnly, PlaneChunksOf(Direct::Simd::Chosen()), a_x, a_y, a_z, a_r, a_lanes, a_count, a_rejected);
			return;
		}
		const std::uint32_t group = 16u;
		// Each operator's bounds, cleared as the walk takes them (every mask it sets is an operator after the one setting it): zero between calls.
		thread_local std::vector<std::uint32_t> masks;
		if (masks.size() < ops)
			masks.resize(ops, 0u);
		const std::uint16_t* lanes = a_lanes;
		for (std::uint32_t k = 0; k < a_count; k += group) {
			const std::uint32_t n = std::min(group, a_count - k);
			alignas(64) float x[kGroup]{}, y[kGroup]{}, z[kGroup]{}, r[kGroup]{};
			std::uint32_t live = 0;  // the group's bounds with a radius (a zero radius: culled)
			for (std::uint32_t j = 0; j < n; ++j) {
				const std::uint16_t i = lanes[k + j];
				x[j] = a_x[i], y[j] = a_y[i], z[j] = a_z[i], r[j] = a_r[i];
				if (r[j] != 0.0f)
					live |= 1u << j;
			}
			std::uint32_t inView = 0;
			if (whole >= 0) {
				inView = whole == 1 ? live : 0u;
			} else if (live) {
				// The camera's view planes: the header's plane set.
				const std::uint32_t inCamera = planeGroup(w, 7u, x, y, z, r, live);
				if (viewOnly) {
					inView = inCamera;
				} else {
					const std::uint32_t walk = (flags & kPortalSkipView) ? live : inCamera;
					// ProgramRun for the group at once: each operator in order, its bounds' mask to the operator its test sends each to.
					if (walk && first < ops) {
						masks[first] = walk;
						for (std::uint32_t op = first; op < ops; ++op) {
							const std::uint32_t m = masks[op];
							if (!m)
								continue;
							masks[op] = 0;
							const std::uint32_t* record = records + std::size_t(op) * kOpWords;
							const std::uint32_t kind = record[0], set = record[1];
							if (kind == 2u)
								inView |= m;
							if ((kind != 7u && kind != 8u) || set >= sets)
								continue;  // a terminal, or false
							const std::uint32_t pass = planeGroup(programSets + std::size_t(set) * kSetWords, kind, x, y, z, r, m);
							if (pass && record[2] < ops)
								masks[record[2]] |= pass;
							if ((m & ~pass) && record[3] < ops)
								masks[record[3]] |= m & ~pass;
						}
					}
				}
			}
			for (std::uint32_t j = 0; j < n; ++j)
				a_rejected[lanes[k + j]] = (inView >> j) & 1u ? 0u : 1u;
		}
	}

	bool StrictGateRejectsCpu(const Direct::StrictGates& a_gates, std::uint32_t a_context, std::uint32_t a_object, const float a_centre[3], float a_radius)
	{
		const Words gates{ a_gates.gates }, atlas{ a_gates.atlas };
		if (a_gates.gates.size() < kStrictHeaderWords || a_gates.atlas.size() < 4 || a_gates.objectGates.empty())
			return false;
		// A publication of another atlas, or a context it does not hold, gates nothing.
		const std::uint32_t gateCount = gates[0], contexts = gates[1];
		if (gates[2] != atlas[2] || gates[3] != atlas[3] || a_context >= contexts || a_context >= atlas[0] || gateCount == 0u)
			return false;
		std::uint32_t gate = a_object < a_gates.objectGates.size() ? a_gates.objectGates[a_object] : 0u;
		gate = gate < gateCount ? gate : 0u;
		const std::uint32_t verdict = gates[kStrictHeaderWords + std::size_t(gate) * contexts + a_context];
		if (verdict == kGateClosed)
			return true;
		if (verdict == kPortalNoProgram)
			return false;
		// ContextEntry::programsBase: absolute bytes into the atlas.
		const std::uint32_t baseBytes = atlas[4 + std::size_t(a_context) * kContextEntryWords + 6];
		return !StrictProgramPass(atlas, baseBytes / 4, verdict, a_centre, a_radius);
	}

	Direct::StrictGates StrictGatesOf(std::span<const std::uint32_t> a_gates, std::span<const std::uint32_t> a_atlas, std::span<const std::uint32_t> a_objectGates,
		std::shared_ptr<const void> a_hold)
	{
		return { a_gates, a_atlas, a_objectGates, &StrictGateRejectsCpu, std::move(a_hold) };
	}
}
