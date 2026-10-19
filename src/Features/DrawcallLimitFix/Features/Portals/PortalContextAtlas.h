#pragma once

#include "Features/DrawcallLimitFix/Features/Portals/PortalContext.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>

namespace DCLF::Portals
{
	/** @brief Immutable atlas directory ABI. Program offsets are absolute bytes; bucket IDs are dense across contexts. */
	struct ContextEntry
	{
		std::uint32_t frame = 0, flags = 0, generationLo = 0, generationHi = 0;
		std::uint32_t roomsGenerationLo = 0, roomsGenerationHi = 0, programsBase = 0, firstBucket = 0;
		std::array<float, 4> walkSphere{}, walkPlane{};  // Context::walkSphere/walkPlane, never the accumulation process's
	};
	static_assert(sizeof(ContextEntry) == 64);
	inline constexpr std::uint32_t kContextPoint = 1, kContextStrict = 2, kContextBothSides = 4;

	/** @brief Check the publication identity and every context's extent before publishing GPU addresses. Bytecode is validated by Lower. */
	inline std::uint64_t ValidateContexts(std::span<const std::uint32_t> a_words, std::uint32_t a_frame)
	{
		if (a_words.size() < 4 || 4ull + 16ull * a_words[0] > a_words.size())
			throw std::logic_error("portal atlas: incomplete context directory");
		const auto generation = std::uint64_t(a_words[2]) | (std::uint64_t(a_words[3]) << 32);
		if (!generation)
			throw std::logic_error("portal atlas: missing publication identity");
		std::uint64_t nextWord = 4ull + 16ull * a_words[0], nextBucket = 0;
		for (std::uint32_t i = 0; i < a_words[0]; ++i) {
			const auto entry = a_words.subspan(4ull + 16ull * i, 16);
			if (entry[0] != a_frame)
				throw std::logic_error("portal atlas: mixed context frames");
			if (entry[6] != nextWord * 4 || entry[7] != nextBucket || nextWord + kPortalHeaderBytes / 4 > a_words.size())
				throw std::logic_error("portal atlas: invalid context range");
			const auto programs = a_words.subspan(static_cast<std::size_t>(nextWord));
			const auto count = programs[kPortalSetBytes / 4], records = programs[kPortalSetBytes / 4 + 1], sets = programs[kPortalSetBytes / 4 + 2];
			nextWord += kPortalHeaderBytes / 4 + 8ull * count + 4ull * records + (kPortalSetBytes / 4ull) * sets;
			nextBucket += count;
			if (nextWord > a_words.size() || nextBucket > UINT32_MAX)
				throw std::logic_error("portal atlas: incomplete program extent");
		}
		if (nextWord != a_words.size() || nextBucket != a_words[1])
			throw std::logic_error("portal atlas: inconsistent publication extent");
		return generation;
	}

	/** @brief Pack camera/light snapshots into one owned buffer without retaining mutable walk scratch. */
	inline std::vector<std::uint32_t> EncodeContexts(std::span<const ContextHold> a_contexts, std::uint64_t a_publication = 0)
	{
		constexpr std::uint64_t maxWords = std::numeric_limits<std::uint32_t>::max() / 4;
		std::uint64_t words = 4 + 16ull * a_contexts.size(), buckets = 0;
		for (const auto& context : a_contexts) {
			if (!context || context->programs.size() < kPortalHeaderBytes / 4)
				throw std::logic_error("portal atlas: missing context header");
			const auto count = context->programs[kPortalSetBytes / 4];
			if (kPortalHeaderBytes / 4 + 8ull * count > context->programs.size())
				throw std::logic_error("portal atlas: incomplete room directory");
			words += context->programs.size();
			buckets += count;
		}
		if (words > maxWords || buckets > std::numeric_limits<std::uint32_t>::max())
			throw std::overflow_error("portal atlas: byte address range exhausted");
		std::vector<std::uint32_t> result(4 + 16 * a_contexts.size());
		result.reserve(static_cast<std::size_t>(words));
		result[0] = static_cast<std::uint32_t>(a_contexts.size());
		result[1] = static_cast<std::uint32_t>(buckets);
		result[2] = static_cast<std::uint32_t>(a_publication);
		result[3] = static_cast<std::uint32_t>(a_publication >> 32);
		std::uint32_t firstBucket = 0;
		for (std::size_t i = 0; i < a_contexts.size(); ++i) {
			const auto& context = *a_contexts[i];
			ContextEntry entry;
			entry.frame = context.frame;
			entry.flags = (context.kind == Context::Kind::PointLight ? kContextPoint : 0u) |
			              (context.strict ? kContextStrict : 0u) | (context.bothSides ? kContextBothSides : 0u);
			entry.generationLo = static_cast<std::uint32_t>(context.generation);
			entry.generationHi = static_cast<std::uint32_t>(context.generation >> 32);
			entry.roomsGenerationLo = static_cast<std::uint32_t>(context.roomsGeneration);
			entry.roomsGenerationHi = static_cast<std::uint32_t>(context.roomsGeneration >> 32);
			entry.programsBase = static_cast<std::uint32_t>(result.size() * 4);
			entry.firstBucket = firstBucket;
			entry.walkSphere = context.walkSphere;
			entry.walkPlane = context.walkPlane;
			const auto directory = std::bit_cast<std::array<std::uint32_t, 16>>(entry);
			std::copy(directory.begin(), directory.end(), result.begin() + 4 + 16 * i);
			result.insert(result.end(), context.programs.begin(), context.programs.end());
			firstBucket += context.programs[kPortalSetBytes / 4];
		}
		return result;
	}
}
