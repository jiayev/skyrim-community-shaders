// PortalPrograms.hlsli on the CPU (PortalProgramsCpu.h): PortalTestAt, PortalProgramRun, PortalPlaneSetTest and StrictGateRejects, word for
// word over the same encodings. Keep the three in step with the shader.

#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramsCpu.h"

#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"

#include <bit>

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

		/** @brief PortalPlaneSetTest: one fixed six-plane calculation for both kinds (7: not outside, 8: not contained). */
		bool PlaneSetTest(const Words& a_w, std::size_t a_set, std::uint32_t a_kind, const float a_c[3], float a_r)
		{
			const std::uint32_t mask = a_w[a_set + kMaskWord];
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
		return PortalTestCpuAt(a_programs, 0, a_program, a_centre, a_radius, a_nodeFlags);
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
