#include "Features/DrawcallLimitFix/Features/Portals/PortalBatchLayout.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBytecode.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContextAtlas.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

namespace
{
	using namespace DCLF::PortalBytecode;
	struct Op
	{
		std::uint32_t word = 0, yes = 0, no = 0;
	};
	struct Set
	{
		std::array<std::array<float, 4>, 6> plane{};
		std::uint32_t mask = 0;
	};

	// Independent transcription of the original early-exit interpreter, retaining operand-only records.
	bool Legacy(std::span<const Op> ops, std::span<const Set> sets, const float c[3], float r)
	{
		std::uint32_t pc = 0;
		for (std::size_t step = 0; step <= ops.size(); ++step) {
			const auto& op = ops[pc];
			if (op.word == kAccept || op.word == kReject)
				return op.word == kAccept;
			const auto& set = sets[ops[pc + 1].word];
			bool result = op.word == kPortal;
			if (set.mask) {
				std::uint32_t p = 0;
				for (; p < 6; ++p) {
					if (!(set.mask & (1u << p)))
						continue;
					const auto& n = set.plane[p];
					const float d = ((n[1] * c[1] + n[0] * c[0]) + n[2] * c[2]) - n[3];
					if (d <= -r || (op.word == kOccluder && d < r))
						break;
				}
				result = op.word == kPortal ? p == 6 : p != 6;
			}
			pc = result ? op.yes : op.no;
		}
		assert(false);
		return false;
	}

	void Differential()
	{
		std::mt19937 random(0x504F5254);
		std::uniform_real_distribution<float> value(-10.0f, 10.0f);
		for (unsigned trial = 0; trial < 200; ++trial) {
			const std::uint32_t n = 1 + random() % 80;
			std::vector<Op> ops(2 * n + 3);
			std::vector<Set> sets(n);
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto target = [&] {
					const auto j = i + 1 + random() % (n - i + 1);
					return j < n ? 2 * j : 2 * n + (j - n);
				};
				ops[2 * i] = { random() & 1 ? kPortal : kOccluder, target(), target() };
				ops[2 * i + 1].word = i;
				sets[i].mask = random() & 63;
				for (auto& plane : sets[i].plane)
					for (auto& x : plane)
						x = value(random);
			}
			ops[2 * n].word = kAccept;
			ops[2 * n + 1].word = kReject;
			// An unreachable malformed instruction must not enter the executable program.
			ops[2 * n + 2].word = 123;
			const auto compact = Lower<Op>(ops, 0, n);
			for (std::uint32_t i = 0; i < compact.instructions.size(); ++i) {
				const auto& op = compact.instructions[i];
				if (op.kind == kPortal || op.kind == kOccluder)
					assert(op.yes > i && op.no > i);
			}
			for (unsigned q = 0; q < 200; ++q) {
				const float c[]{ value(random), value(random), value(random) };
				const float r = std::abs(value(random));
				const auto expected = Legacy(ops, sets, c, r);
				assert(expected == Run<Set>(compact, sets, c, r));
				assert(expected == Run<Set>(compact, sets, c, r, true));
			}
		}
	}

	void BatchLayouts()
	{
		using namespace DCLF::Portals;
		assert(DispatchFor(0, 64).x == 0);
		for (const auto width : { 1u, 64u, 256u }) {
			for (const auto count : { 1u, 63u, 64u, 65u, 65535u * 64, 65535u * 64 + 1, UINT32_MAX }) {
				if (width == 1 && count == UINT32_MAX)
					continue;
				const auto shape = DispatchFor(count, width);
				const auto groups = (std::uint64_t(count) + width - 1) / width;
				assert(shape.x <= 65535 && shape.y <= 65535);
				assert(std::uint64_t(shape.x) * shape.y >= groups);
				assert(shape.y == 1 || shape.x == 65535);
			}
		}
		for (const auto count : { 1u, 255u, 256u, 257u, 65535u, 65536u, 65537u, 1000003u }) {
			const auto plan = ScanFor(count);
			std::vector<std::uint64_t> counts(plan.units), offsets(plan.units);
			for (std::uint32_t i = 0; i < count; ++i)
				counts[i] = i % 71 == 0 ? 0 : i % 149;
			// Execute the actual host level layout, including short workgroups, against an independent serial prefix sum.
			for (std::size_t level = 0; level < plan.levels.size(); ++level) {
				const auto [base, elements] = plan.levels[level];
				for (std::uint32_t first = 0; first < elements; first += 256) {
					std::uint64_t sum = 0;
					for (auto i = first; i < std::min(elements, first + 256); ++i) {
						offsets[base + i] = sum;
						sum += counts[base + i];
					}
					if (level + 1 < plan.levels.size())
						counts[plan.levels[level + 1].base + first / 256] = sum;
				}
			}
			for (auto level = plan.levels.size() - 1; level > 0; --level) {
				const auto [base, elements] = plan.levels[level - 1];
				for (std::uint32_t i = 0; i < elements; ++i)
					offsets[base + i] += offsets[plan.levels[level].base + i / 256];
			}
			std::uint64_t sum = 0;
			for (std::uint32_t i = 0; i < count; ++i) {
				assert(offsets[i] == sum);
				sum += counts[i];
			}
			assert(plan.units <= std::uint64_t(count) * 2);
		}
		assert(ScanFor(0).units == 1);
		for (const auto scan : { false, true }) {
			bool rejected = false;
			try {
				if (scan)
					(void)ScanFor(UINT32_MAX);
				else
					(void)DispatchFor(UINT32_MAX, 1);
			} catch (const std::overflow_error&) {
				rejected = true;
			}
			assert(rejected);
		}
	}

	void Boundaries()
	{
		Set set;
		set.plane[0] = { 1, 0, 0, 0 };
		for (auto kind : { kPortal, kOccluder }) {
			std::vector<Op> ops{ { kind, 2, 3 }, { 0 }, { kAccept }, { kReject } };
			const auto compact = Lower<Op>(ops, 0, 1);
			for (auto mask : { 0u, 1u, 63u }) {
				set.mask = mask;
				for (float x : { -1.0f, std::nextafter(-1.0f, 0.0f), 0.0f, std::nextafter(1.0f, 0.0f), 1.0f,
						 std::numeric_limits<float>::quiet_NaN() }) {
					const float c[]{ x, 0, 0 };
					for (float r : { 0.0f, 1.0f }) {
						const auto expected = Legacy(ops, std::span(&set, 1), c, r);
						assert(expected == Run<Set>(compact, std::span(&set, 1), c, r));
						assert(expected == Run<Set>(compact, std::span(&set, 1), c, r, true));
					}
				}
			}
		}
	}

	void Validation()
	{
		const auto invalid = [](std::vector<Op> ops, std::uint32_t sets) {
			bool rejected = false;
			try {
				(void)Lower<Op>(ops, 0, sets);
			} catch (const std::logic_error&) {
				rejected = true;
			}
			assert(rejected);
		};
		invalid({}, 0);
		invalid({ { kPortal, 0, 2 }, { 0 }, { kReject } }, 1);
		invalid({ { kPortal, 9, 2 }, { 0 }, { kReject } }, 1);
		invalid({ { kPortal, 1, 2 }, { 2 }, { kAccept } }, 3);
		invalid({ { kPortal, 2, 2 }, { 1 }, { kReject } }, 1);
		invalid({ { 4 } }, 0);
		std::vector<Op> deep(2 * 1024 + 1);
		for (std::uint32_t i = 0; i < 1024; ++i)
			deep[2 * i] = { kPortal, 2 * i + 2, 2 * i + 2 };
		deep.back().word = kAccept;
		const auto compact = Lower<Op>(deep, 0, 1);
		assert(compact.maxSteps == 1025);
		const Set empty;
		const float c[]{ 0, 0, 0 };
		assert(Run<Set>(compact, std::span(&empty, 1), c, 1));
	}
}

int main()
{
	{
		using namespace DCLF::Portals;
		auto camera = std::make_shared<Context>();
		camera->frame = 17;
		camera->generation = 0x123456789abcdef0ull;
		camera->roomsGeneration = 0x9876543210123456ull;
		camera->programs.resize(DCLF::kPortalHeaderBytes / 4 + 8);
		camera->programs[DCLF::kPortalSetBytes / 4] = 1;
		auto light = std::make_shared<Context>(*camera);
		light->kind = Context::Kind::PointLight;
		light->strict = true;
		light->bothSides = false;
		light->sphere = { 1, 2, 3, 4 };
		const std::array<ContextHold, 2> contexts{ camera, light };
		const auto atlas = EncodeContexts(contexts, 0x12345678abcdef01ull);
		assert(ValidateContexts(atlas, 17) == 0x12345678abcdef01ull);
		const auto malformed = [&](std::vector<std::uint32_t> words, std::uint32_t frame = 17) {
			bool failed = false;
			try {
				(void)ValidateContexts(words, frame);
			} catch (const std::logic_error&) {
				failed = true;
			}
			assert(failed);
		};
		malformed(atlas, 18);
		auto broken = atlas;
		broken[2] = broken[3] = 0;
		malformed(broken);
		broken = atlas;
		broken[1]++;
		malformed(broken);
		broken = atlas;
		broken[4 + 6] += 4;
		malformed(broken);
		broken = atlas;
		broken[20 + 7] = 0;
		malformed(broken);
		broken = atlas;
		broken.pop_back();
		malformed(broken);
		broken = atlas;
		broken.push_back(0);
		malformed(broken);
		broken = atlas;
		broken[20]++;
		malformed(broken);
		malformed({ 0, 0, 0 });
		assert(ValidateContexts(EncodeContexts({}, 7), 17) == 7);
		auto newer = EncodeContexts(contexts, 0x12345678abcdef02ull);
		assert(ValidateContexts(newer, 17) != ValidateContexts(atlas, 17));
		broken = atlas;
		broken[broken[4 + 6] / 4 + DCLF::kPortalSetBytes / 4 + 1] = UINT32_MAX;
		malformed(broken);
		auto noGraph = std::make_shared<Context>();
		noGraph->frame = 17;
		noGraph->programs.resize(DCLF::kPortalHeaderBytes / 4);
		assert(ValidateContexts(EncodeContexts(std::array<ContextHold, 1>{ noGraph }, 9), 17) == 9);

		assert(atlas[0] == 2 && atlas[1] == 2);
		std::array<std::uint32_t, 16> words;
		std::copy_n(atlas.begin() + 4, 16, words.begin());
		const auto first = std::bit_cast<ContextEntry>(words);
		assert(first.frame == 17 && first.firstBucket == 0 && first.programsBase == 16 + 2 * 64);
		assert(first.generationLo == 0x9abcdef0 && first.generationHi == 0x12345678);
		assert(first.roomsGenerationLo == 0x10123456 && first.roomsGenerationHi == 0x98765432);
		std::copy_n(atlas.begin() + 20, 16, words.begin());
		const auto second = std::bit_cast<ContextEntry>(words);
		assert(second.firstBucket == 1 && second.flags == (kContextPoint | kContextStrict));
		assert(second.sphere == light->sphere);
		assert(second.programsBase == first.programsBase + camera->programs.size() * 4);
		assert(std::equal(light->programs.begin(), light->programs.end(), atlas.begin() + second.programsBase / 4));
		camera->programs[0] = 123;  // Published atlas owns its words independently of future snapshots.
		assert(atlas[first.programsBase / 4] == 0);
		assert(EncodeContexts({}).size() == 4);
		bool rejected = false;
		try {
			(void)EncodeContexts(std::array<ContextHold, 1>{ nullptr });
		} catch (const std::logic_error&) {
			rejected = true;
		}
		assert(rejected);
		auto incomplete = std::make_shared<Context>();
		incomplete->programs.resize(DCLF::kPortalHeaderBytes / 4);
		incomplete->programs[DCLF::kPortalSetBytes / 4] = 1;
		rejected = false;
		try {
			(void)EncodeContexts(std::array<ContextHold, 1>{ incomplete });
		} catch (const std::logic_error&) {
			rejected = true;
		}
		assert(rejected);
	}
	BatchLayouts();
	Differential();
	Boundaries();
	Validation();
	std::cout << "Portal bytecode differential and validation tests passed\n";
}
