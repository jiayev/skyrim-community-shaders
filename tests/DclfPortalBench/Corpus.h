#pragma once

// The bench's view of a portal corpus (Portals/PortalCorpus.h, the format the game's capture writes): the oracle and the statistics.

#include "Features/DrawcallLimitFix/Features/Portals/PortalCorpus.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramModel.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace Bench
{
	using DCLF::Portals::Corpus::Corpus;
	using DCLF::Portals::Corpus::kImmediate;
	using DCLF::Portals::Corpus::Load;
	using DCLF::Portals::Corpus::Query;
	using DCLF::Portals::Corpus::Save;

	/** @brief The oracle: a query's verdict word as PortalBatchCS writes it (PortalTestAt on the CPU; an immediate verdict as is). */
	inline std::uint32_t Expected(const Corpus& a_corpus, const Query& a_query)
	{
		if (a_query.flags & kImmediate)
			return a_query.program;
		const float c[3]{ a_query.bound[0], a_query.bound[1], a_query.bound[2] };
		return static_cast<std::uint32_t>(DCLF::PortalModel::Visible(a_corpus.words, a_query.program, c, a_query.bound[3], a_query.flags).visible);
	}

	/** @brief The encoding's shape and the queries' spread, for the report. */
	struct Stats
	{
		std::uint32_t programs = 0, reached = 0, withOps = 0, records = 0, sets = 0, maxSets = 0, maxRecords = 0, maxPath = 0;
		std::uint32_t queries = 0, immediate = 0, evaluated = 0, programQueries = 0, accepted = 0;
		std::map<std::uint32_t, std::uint32_t> pathHistogram;  // max path length -> programs
	};

	inline Stats Measure(const Corpus& a_corpus, const std::vector<std::uint32_t>& a_expected)
	{
		using namespace DCLF;
		Stats s;
		const auto& w = a_corpus.words;
		constexpr std::size_t kHeader = kPortalSetBytes / 4, kDirectory = kPortalHeaderBytes / 4, kEntry = kPortalDirectoryBytes / 4;
		s.programs = w.size() > kHeader ? w[kHeader] : 0;
		std::vector<std::uint32_t> opsOf(s.programs);
		for (std::uint32_t p = 0; p < s.programs; ++p) {
			const std::size_t e = kDirectory + std::size_t(p) * kEntry;
			const std::uint32_t flags = w[e], ops = w[e + 1], sets = w[e + 2], path = w[e + 7];
			opsOf[p] = ops;
			s.reached += (flags & kPortalReached) ? 1 : 0;
			if (ops) {
				++s.withOps;
				s.records += ops;
				s.sets += sets;
				s.maxSets = std::max(s.maxSets, sets);
				s.maxRecords = std::max(s.maxRecords, ops);
				s.maxPath = std::max(s.maxPath, path);
				++s.pathHistogram[path];
			}
		}
		s.queries = static_cast<std::uint32_t>(a_corpus.queries.size());
		for (std::size_t i = 0; i < a_corpus.queries.size(); ++i) {
			const auto& q = a_corpus.queries[i];
			if (q.flags & kImmediate) {
				++s.immediate;
				continue;
			}
			++s.evaluated;
			if (q.program < s.programs && opsOf[q.program])
				++s.programQueries;
			if (a_expected[i] == 1)
				++s.accepted;
		}
		return s;
	}
}
