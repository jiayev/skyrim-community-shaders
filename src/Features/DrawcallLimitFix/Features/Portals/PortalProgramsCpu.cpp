// PortalPrograms.hlsli on the CPU (PortalProgramsCpu.h): PortalTestAt, PortalProgramRun, PortalPlaneSetTest and StrictGateRejects, word for
// word over the same encodings. Keep the three in step with the shader.

#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramsCpu.h"

#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"

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
		// The bounds one walk carries: four SSE vectors of four.
		constexpr std::uint32_t kVectors = 4, kGroup = kVectors * 4;

		/** @brief A group's bounds: their centres and radii, by vector. */
		struct BoundsGroup
		{
			__m128 x[kVectors], y[kVectors], z[kVectors], r[kVectors], minusR[kVectors];
		};

		/** @brief A plane set decoded once a call: its active planes' {n, d} (Distance's words as floats, in plane order) in the call's planes. */
		struct DecodedSet
		{
			std::uint32_t first = 0, count = 0;
		};

		/**
		 * @brief PlaneSetTest (or ViewTest: a_kind 7) for a group, as a mask of its bounds: only the vectors with a bound in a_lanes, and only
		 * until every bound in a_lanes is decided (the verdict is an OR over the planes: the planes after cannot change it).
		 */
		std::uint32_t PlaneSetGroup(const float* a_planes, const DecodedSet& a_set, std::uint32_t a_kind, const BoundsGroup& a_b, std::uint32_t a_lanes)
		{
			__m128 outside[kVectors], notContained[kVectors];
			for (std::uint32_t v = 0; v < kVectors; ++v)
				outside[v] = notContained[v] = _mm_setzero_ps();
			std::uint32_t decided = 0;
			for (std::uint32_t p = 0; p < a_set.count; ++p) {
				const float* plane = a_planes + std::size_t(a_set.first + p) * 4;
				const __m128 nx = _mm_set1_ps(plane[0]), ny = _mm_set1_ps(plane[1]), nz = _mm_set1_ps(plane[2]), d0 = _mm_set1_ps(plane[3]);
				decided = 0;
				for (std::uint32_t v = 0; v < kVectors; ++v) {
					if (!((a_lanes >> (4 * v)) & 0xFu))
						continue;
					// PortalDistance: ((n.y * c.y + n.x * c.x) + c.z * n.z) - d, the shader's order.
					const __m128 d = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ny, a_b.y[v]), _mm_mul_ps(nx, a_b.x[v])), _mm_mul_ps(a_b.z[v], nz)), d0);
					const __m128 behind = _mm_cmple_ps(d, a_b.minusR[v]);
					outside[v] = _mm_or_ps(outside[v], behind);
					notContained[v] = _mm_or_ps(notContained[v], _mm_or_ps(behind, _mm_cmplt_ps(d, a_b.r[v])));
					decided |= static_cast<std::uint32_t>(_mm_movemask_ps(a_kind == 7u ? outside[v] : notContained[v])) << (4 * v);
				}
				if ((decided & a_lanes) == a_lanes)
					break;
			}
			// Kind 7 passes the bounds not outside; kind 8 those not contained (no active plane: none outside, none not contained).
			return (a_kind == 7u ? ~decided : decided) & a_lanes;
		}

		/**
		 * @brief Whether a program's walk can be made as one pass over its operators: each test's next operators after it (or past the
		 * operators: false), and its longest path within its step bound (ProgramRun's: a walk past it is false).
		 */
		bool Walkable(const Words& a_w, std::size_t a_entry, std::size_t a_recordsBase, std::vector<std::uint32_t>& a_depth)
		{
			const std::uint32_t ops = a_w[a_entry + 1], first = a_w[a_entry + 3], steps = a_w[a_entry + 7];
			if (first >= ops)
				return true;  // false for every bound at the first step, as ProgramRun
			a_depth.assign(ops, 0u);
			a_depth[first] = 1;
			for (std::uint32_t op = first; op < ops; ++op) {
				const std::uint32_t depth = a_depth[op];
				if (!depth)
					continue;
				if (depth > steps)
					return false;
				const std::size_t record = a_recordsBase + std::size_t(op) * kOpWords;
				const std::uint32_t kind = a_w[record];
				if (kind != 7u && kind != 8u)
					continue;
				for (const std::uint32_t next : { a_w[record + 2], a_w[record + 3] }) {
					if (next >= ops)
						continue;
					if (next <= op)
						return false;
					a_depth[next] = std::max(a_depth[next], depth + 1);
				}
			}
			return true;
		}

		/** @brief A plane set's active planes appended to a_planes. */
		DecodedSet DecodeSet(const Words& a_w, std::size_t a_set, std::vector<float>& a_planes)
		{
			DecodedSet set{ static_cast<std::uint32_t>(a_planes.size() / 4), 0 };
			const std::uint32_t mask = a_w[a_set + kMaskWord];
			for (std::uint32_t p = 0; p < 6; ++p)
				if (mask & (1u << p)) {
					for (std::uint32_t c = 0; c < 4; ++c)
						a_planes.push_back(a_w.Float(a_set + p * 4 + c));
					++set.count;
				}
			return set;
		}

		/** @brief An operator decoded once a call: its kind (0: invalid, false), plane set and next operators. */
		struct DecodedOp
		{
			std::uint32_t kind = 0;
			DecodedSet set;
			std::uint32_t next[2]{};
		};
	}

	void PortalTestCpuBatch(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint32_t a_count, std::uint8_t* a_rejected)
	{
		const Words w{ a_programs };
		const std::uint32_t count = w[kSetWords];
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
		const std::size_t records = recordsBase + std::size_t(w[entry + 4]) * kOpWords;
		const std::size_t setBase = setsBase + std::size_t(w[entry + 5]) * kSetWords;
		// The whole program's verdict, whatever the bound (PortalTestCpuAt's order; a zero radius is culled before them).
		int whole = -1;
		if (a_program != 0u && !(flags & kPortalReached))
			whole = 0;
		else if (cullMode == 2u)
			whole = 0;
		else if (cullMode == 1u)
			whole = 1;
		const bool viewOnly = cullMode == 3u || ops == 0u;
		thread_local std::vector<std::uint32_t> depth;
		thread_local std::vector<std::uint16_t> masks;
		thread_local std::vector<float> planes;
		thread_local std::vector<DecodedOp> decoded;
		if (whole < 0 && !viewOnly && !Walkable(w, entry, records, depth)) {
			for (std::uint32_t k = 0; k < a_count; ++k) {
				const std::uint16_t i = a_lanes[k];
				const float centre[3]{ a_x[i], a_y[i], a_z[i] };
				a_rejected[i] = PortalTestCpu(a_programs, a_program, centre, a_r[i], 0u) != 1 ? 1u : 0u;
			}
			return;
		}
		// The camera's view planes and the program's operators, decoded once.
		planes.clear();
		const DecodedSet view = DecodeSet(w, 0, planes);
		if (whole < 0 && !viewOnly) {
			decoded.assign(ops, DecodedOp{});
			for (std::uint32_t op = first; op < ops; ++op) {
				const std::size_t record = records + std::size_t(op) * kOpWords;
				auto& d = decoded[op];
				const std::uint32_t kind = w[record], set = w[record + 1];
				if (kind == 2u || kind == 3u) {
					d.kind = kind;
				} else if ((kind == 7u || kind == 8u) && set < sets) {
					d.kind = kind;
					d.set = DecodeSet(w, setBase + std::size_t(set) * kSetWords, planes);
					d.next[0] = w[record + 2];
					d.next[1] = w[record + 3];
				}
			}
			masks.assign(ops, 0u);
		}
		for (std::uint32_t k = 0; k < a_count; k += kGroup) {
			const std::uint32_t n = std::min(kGroup, a_count - k);
			alignas(16) float x[kGroup]{}, y[kGroup]{}, z[kGroup]{}, r[kGroup]{};
			std::uint32_t live = 0;  // the group's bounds with a radius (a zero radius: culled)
			for (std::uint32_t j = 0; j < n; ++j) {
				const std::uint16_t i = a_lanes[k + j];
				x[j] = a_x[i], y[j] = a_y[i], z[j] = a_z[i], r[j] = a_r[i];
				if (r[j] != 0.0f)
					live |= 1u << j;
			}
			std::uint32_t inView = 0;
			if (whole >= 0) {
				inView = whole == 1 ? live : 0u;
			} else if (live) {
				BoundsGroup b;
				for (std::uint32_t v = 0; v < kVectors; ++v) {
					b.x[v] = _mm_load_ps(x + 4 * v), b.y[v] = _mm_load_ps(y + 4 * v), b.z[v] = _mm_load_ps(z + 4 * v), b.r[v] = _mm_load_ps(r + 4 * v);
					b.minusR[v] = _mm_sub_ps(_mm_setzero_ps(), b.r[v]);
				}
				const std::uint32_t inCamera = PlaneSetGroup(planes.data(), view, 7u, b, live);
				if (viewOnly) {
					inView = inCamera;
				} else {
					const std::uint32_t walk = (flags & kPortalSkipView) ? live : inCamera;
					// ProgramRun for the group at once: each operator in order, its bounds' mask to the operator its test sends each to.
					if (walk && first < ops) {
						masks[first] = static_cast<std::uint16_t>(walk);
						for (std::uint32_t op = first; op < ops; ++op) {
							const std::uint32_t m = masks[op];
							if (!m)
								continue;
							masks[op] = 0;
							const DecodedOp& d = decoded[op];
							if (d.kind == 2u)
								inView |= m;
							if (d.kind != 7u && d.kind != 8u)
								continue;  // a terminal, or false
							const std::uint32_t pass = PlaneSetGroup(planes.data(), d.set, d.kind, b, m);
							if (pass && d.next[0] < ops)
								masks[d.next[0]] |= static_cast<std::uint16_t>(pass);
							if ((m & ~pass) && d.next[1] < ops)
								masks[d.next[1]] |= static_cast<std::uint16_t>(m & ~pass);
						}
					}
				}
			}
			for (std::uint32_t j = 0; j < n; ++j)
				a_rejected[a_lanes[k + j]] = (inView >> j) & 1u ? 0u : 1u;
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
