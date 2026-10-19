#pragma once

// The experimental evaluators' auxiliary tables (PortalEvaluators.hlsli), built from an encoding's words on the CPU.

#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace Bench
{
	struct EncodingView
	{
		const std::vector<std::uint32_t>& w;
		static constexpr std::size_t kHeader = DCLF::kPortalSetBytes / 4, kDirectory = DCLF::kPortalHeaderBytes / 4,
									 kEntry = DCLF::kPortalDirectoryBytes / 4, kOp = DCLF::kPortalOpBytes / 4, kSet = DCLF::kPortalSetBytes / 4;
		std::uint32_t Programs() const { return w.size() > kHeader ? w[kHeader] : 0; }
		const std::uint32_t* Entry(std::uint32_t a_program) const { return w.data() + kDirectory + std::size_t(a_program) * kEntry; }
		std::size_t RecordsBase() const { return kDirectory + std::size_t(Programs()) * kEntry; }
		std::size_t SetsBase() const { return RecordsBase() + std::size_t(w[kHeader + 1]) * kOp; }
		const std::uint32_t* Record(std::uint32_t a_program, std::uint32_t a_op) const { return w.data() + RecordsBase() + (std::size_t(Entry(a_program)[4]) + a_op) * kOp; }
		const std::uint32_t* Set(std::uint32_t a_program, std::uint32_t a_set) const { return w.data() + SetsBase() + (std::size_t(Entry(a_program)[5]) + a_set) * kSet; }
	};

	/** @brief What a table covered: programs with operators, those flattened (or bit-mapped), and the largest. */
	struct AuxReport
	{
		std::uint32_t programs = 0, covered = 0, fallback = 0, maxTerms = 0, maxLiterals = 0, maxPlanes = 0;
		std::uint64_t terms = 0, literals = 0, planes = 0, slots = 0;
	};

	/**
	 * @brief DCLF_PORTAL_FLAT's table: each program's accepting paths from its first record, a literal per test on the path. Exact: in a
	 * decision program exactly one path is taken, and an accepting path whose literals all hold is that path.
	 */
	inline std::vector<std::uint32_t> BuildFlat(const std::vector<std::uint32_t>& a_words, std::uint32_t a_maxTerms, AuxReport& a_report)
	{
		const EncodingView e{ a_words };
		const std::uint32_t programs = e.Programs();
		std::vector<std::uint32_t> header(4 + 2 * std::size_t(programs), 0);
		std::vector<std::uint32_t> terms, literals;
		for (std::uint32_t p = 0; p < programs; ++p) {
			const auto* entry = e.Entry(p);
			const std::uint32_t ops = entry[1];
			if (!ops) {
				header[4 + 2 * p] = 0;
				header[4 + 2 * p + 1] = 0;
				continue;
			}
			++a_report.programs;
			std::vector<std::vector<std::uint32_t>> found;
			std::vector<std::uint32_t> path;
			bool overflow = false;
			std::uint32_t visits = 0;  // rejecting paths are not terms, but can still be exponentially many
			const auto walk = [&](auto&& a_self, std::uint32_t a_op) -> void {
				if (overflow || ++visits > 1u << 16) {
					overflow = true;
					return;
				}
				if (a_op >= ops) {
					overflow = true;  // a producer invariant failure: let the interpreter answer
					return;
				}
				const auto* r = e.Record(p, a_op);
				if (r[0] == 2) {
					found.push_back(path);
					overflow = found.size() > a_maxTerms;
					return;
				}
				if (r[0] == 3)
					return;
				const std::uint32_t literal = (r[1] & 0xFFFFu) | (r[0] == 8 ? 0x10000u : 0u);
				if (r[1] > 0xFFFFu) {
					overflow = true;
					return;
				}
				path.push_back(literal | 0x20000u);
				a_self(a_self, r[2]);
				path.back() = literal;
				a_self(a_self, r[3]);
				path.pop_back();
			};
			walk(walk, entry[3]);
			if (overflow) {
				++a_report.fallback;
				header[4 + 2 * p] = 0;
				header[4 + 2 * p + 1] = 0xFFFFFFFFu;
				continue;
			}
			++a_report.covered;
			header[4 + 2 * p] = static_cast<std::uint32_t>(terms.size() / 2);
			header[4 + 2 * p + 1] = static_cast<std::uint32_t>(found.size());
			std::uint32_t programLiterals = 0;
			for (const auto& term : found) {
				terms.push_back(static_cast<std::uint32_t>(literals.size()));
				terms.push_back(static_cast<std::uint32_t>(term.size()));
				literals.insert(literals.end(), term.begin(), term.end());
				programLiterals += static_cast<std::uint32_t>(term.size());
			}
			a_report.terms += found.size();
			a_report.literals += programLiterals;
			a_report.maxTerms = std::max<std::uint32_t>(a_report.maxTerms, static_cast<std::uint32_t>(found.size()));
			a_report.maxLiterals = std::max(a_report.maxLiterals, programLiterals);
		}
		header[0] = programs;
		header[1] = static_cast<std::uint32_t>(header.size());
		header[2] = static_cast<std::uint32_t>(header.size() + terms.size());
		std::vector<std::uint32_t> out = std::move(header);
		out.insert(out.end(), terms.begin(), terms.end());
		out.insert(out.end(), literals.begin(), literals.end());
		if (out.size() == 4)
			out.push_back(0);
		return out;
	}

	/**
	 * @brief DCLF_PORTAL_PLANE_BITS's table: per program its distinct active planes (bitwise-equal float4s once) and per set the mask of
	 * its active planes' slots. Exact: a set's portal test is "no active plane outside", its occluder test "some active plane not
	 * containing", each plane's comparison made once on the same distance.
	 */
	inline std::vector<std::uint32_t> BuildPlaneBits(const std::vector<std::uint32_t>& a_words, AuxReport& a_report)
	{
		const EncodingView e{ a_words };
		const std::uint32_t programs = e.Programs();
		std::vector<std::uint32_t> header(4 + 4 * std::size_t(programs), 0);
		std::vector<std::uint32_t> planes, masks;
		for (std::uint32_t p = 0; p < programs; ++p) {
			const auto* entry = e.Entry(p);
			auto* info = header.data() + 4 + 4 * std::size_t(p);
			if (!entry[1])
				continue;
			++a_report.programs;
			std::map<std::array<std::uint32_t, 4>, std::uint32_t> index;
			std::vector<std::array<std::uint32_t, 2>> setMasks(entry[2], { 0u, 0u });
			std::vector<std::array<std::uint32_t, 4>> distinct;
			bool overflow = false;
			for (std::uint32_t s = 0; s < entry[2] && !overflow; ++s) {
				const auto* set = e.Set(p, s);
				const std::uint32_t mask = set[24];
				for (std::uint32_t slot = 0; slot < 6; ++slot) {
					if (!((mask >> slot) & 1))
						continue;
					++a_report.slots;
					const std::array<std::uint32_t, 4> plane{ set[slot * 4], set[slot * 4 + 1], set[slot * 4 + 2], set[slot * 4 + 3] };
					auto [it, inserted] = index.try_emplace(plane, static_cast<std::uint32_t>(distinct.size()));
					if (inserted)
						distinct.push_back(plane);
					if (it->second >= 64) {
						overflow = true;
						break;
					}
					setMasks[s][it->second / 32] |= 1u << (it->second % 32);
				}
			}
			if (overflow) {
				++a_report.fallback;
				info[1] = 0xFFFFFFFFu;
				continue;
			}
			++a_report.covered;
			info[0] = static_cast<std::uint32_t>(planes.size() / 4);
			info[1] = static_cast<std::uint32_t>(distinct.size());
			info[2] = static_cast<std::uint32_t>(masks.size() / 2);
			for (const auto& plane : distinct)
				planes.insert(planes.end(), plane.begin(), plane.end());
			for (const auto& m : setMasks)
				masks.insert(masks.end(), m.begin(), m.end());
			a_report.planes += distinct.size();
			a_report.maxPlanes = std::max<std::uint32_t>(a_report.maxPlanes, static_cast<std::uint32_t>(distinct.size()));
		}
		header[0] = programs;
		header[1] = static_cast<std::uint32_t>(header.size());
		header[2] = static_cast<std::uint32_t>(header.size() + planes.size());
		std::vector<std::uint32_t> out = std::move(header);
		out.insert(out.end(), planes.begin(), planes.end());
		out.insert(out.end(), masks.begin(), masks.end());
		return out;
	}
}
