#pragma once

// A portal corpus (.dclfportal): one frame's program encoding (PortalPrograms.h) and the queries a batch evaluated against it
// (PortalQuery, PortalQueries.hlsli). Written by CS_DCLF_PORTAL_CAPTURE (Portals/Diagnostics/PortalCapture.cpp) and the portal bench's
// synthetic generator, read by the bench (tests/DclfPortalBench). Engine-independent. The file is {magic, version, name, metadata,
// word count, words, query count, queries}, little-endian, strings as {length, bytes}.

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace DCLF::Portals::Corpus
{
	/** @brief PortalQueries.hlsli's PortalQuery. Flags bit 31: an immediate verdict, the signed verdict in program. */
	struct Query
	{
		std::array<float, 4> bound{};
		std::uint32_t program = 0, flags = 0, destination = 0, generation = 0;
	};
	static_assert(sizeof(Query) == 32);
	inline constexpr std::uint32_t kImmediate = 0x80000000u;

	struct Corpus
	{
		std::string name;
		std::string metadata;  // free text: the source (cell, camera, generator parameters)
		std::vector<std::uint32_t> words;
		std::vector<Query> queries;
	};

	inline constexpr std::uint64_t kMagic = 0x3154525050464C43ull;  // "CLFPPRT1"
	inline constexpr std::uint32_t kVersion = 1;

	inline void Save(const Corpus& a_corpus, const std::string& a_path)
	{
		std::ofstream out(a_path, std::ios::binary);
		if (!out)
			throw std::runtime_error("cannot write " + a_path);
		const auto put = [&](const void* a_data, std::size_t a_bytes) { out.write(static_cast<const char*>(a_data), static_cast<std::streamsize>(a_bytes)); };
		const auto putString = [&](const std::string& a_text) {
			const auto n = static_cast<std::uint32_t>(a_text.size());
			put(&n, 4);
			put(a_text.data(), n);
		};
		put(&kMagic, 8);
		put(&kVersion, 4);
		putString(a_corpus.name);
		putString(a_corpus.metadata);
		const auto words = static_cast<std::uint32_t>(a_corpus.words.size()), queries = static_cast<std::uint32_t>(a_corpus.queries.size());
		put(&words, 4);
		put(a_corpus.words.data(), std::size_t(words) * 4);
		put(&queries, 4);
		put(a_corpus.queries.data(), std::size_t(queries) * sizeof(Query));
		if (!out)
			throw std::runtime_error("write failed: " + a_path);
	}

	inline Corpus Load(const std::string& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		if (!in)
			throw std::runtime_error("cannot read " + a_path);
		const auto get = [&](void* a_data, std::size_t a_bytes) {
			if (!in.read(static_cast<char*>(a_data), static_cast<std::streamsize>(a_bytes)))
				throw std::runtime_error("truncated " + a_path);
		};
		const auto getString = [&] {
			std::uint32_t n = 0;
			get(&n, 4);
			if (n > (1u << 24))
				throw std::runtime_error("bad string in " + a_path);
			std::string text(n, '\0');
			get(text.data(), n);
			return text;
		};
		std::uint64_t magic = 0;
		std::uint32_t version = 0;
		get(&magic, 8);
		get(&version, 4);
		if (magic != kMagic || version != kVersion)
			throw std::runtime_error("not a version 1 portal corpus: " + a_path);
		Corpus corpus;
		corpus.name = getString();
		corpus.metadata = getString();
		std::uint32_t words = 0, queries = 0;
		get(&words, 4);
		if (words > (1u << 28))
			throw std::runtime_error("bad word count in " + a_path);
		corpus.words.resize(words);
		get(corpus.words.data(), std::size_t(words) * 4);
		get(&queries, 4);
		if (queries > (1u << 26))
			throw std::runtime_error("bad query count in " + a_path);
		corpus.queries.resize(queries);
		get(corpus.queries.data(), std::size_t(queries) * sizeof(Query));
		return corpus;
	}
}
