#pragma once

// Engine-independent lowering of finalized compound frusta. Also used by the CPU differential tests.
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace DCLF::PortalBytecode
{
	inline constexpr std::uint32_t kAccept = 2, kReject = 3, kPortal = 7, kOccluder = 8;

	struct Instruction
	{
		std::uint32_t kind = kReject, set = 0, yes = 0, no = 0;
	};
	static_assert(sizeof(Instruction) == 16);

	struct Compiled
	{
		std::vector<Instruction> instructions;
		std::uint32_t maxSteps = 0;
	};

	/** @brief Validate and lower reachable engine records; structural tokens must already be jump-threaded away. */
	template <class Op>
	Compiled Lower(std::span<const Op> a_ops, std::uint32_t a_first, std::uint32_t a_sets)
	{
		// Iterative DFS avoids depending on the host stack for deeply nested portal paths.
		struct Visit
		{
			std::uint32_t index;
			bool finish;
		};
		std::vector<Visit> pending{ { a_first, false } };
		std::vector<std::uint8_t> colour(a_ops.size());
		std::vector<bool> operand(a_ops.size());
		for (std::size_t i = 0; i < a_ops.size(); ++i)
			if ((a_ops[i].word == kPortal || a_ops[i].word == kOccluder) && i + 1 < a_ops.size())
				operand[++i] = true;
		std::vector<std::uint32_t> order, path(a_ops.size()), mapped(a_ops.size(), UINT32_MAX);
		while (!pending.empty()) {
			const auto [i, finish] = pending.back();
			pending.pop_back();
			if (i >= a_ops.size())
				throw std::logic_error("portal bytecode: branch outside program");
			if (operand[i])
				throw std::logic_error("portal bytecode: branch into operand");
			if (finish) {
				colour[i] = 2;
				const auto& op = a_ops[i];
				path[i] = op.word == kAccept || op.word == kReject ? 1u : 1u + std::max(path[op.yes], path[op.no]);
				order.push_back(i);
				continue;
			}
			if (colour[i] == 2)
				continue;
			if (colour[i] == 1)
				throw std::logic_error("portal bytecode: cycle");
			const auto& op = a_ops[i];
			if (op.word != kAccept && op.word != kReject && op.word != kPortal && op.word != kOccluder)
				throw std::logic_error("portal bytecode: unthreaded instruction");
			colour[i] = 1;
			pending.push_back({ i, true });
			if (op.word == kPortal || op.word == kOccluder) {
				if (i + 1 >= a_ops.size() || a_ops[i + 1].word >= a_sets)
					throw std::logic_error("portal bytecode: invalid plane set");
				pending.push_back({ op.no, false });
				pending.push_back({ op.yes, false });
			}
		}
		std::reverse(order.begin(), order.end());
		for (std::uint32_t i = 0; i < order.size(); ++i)
			mapped[order[i]] = i;
		Compiled result;
		result.maxSteps = path[a_first];
		result.instructions.reserve(order.size());
		for (const auto i : order) {
			const auto& op = a_ops[i];
			const bool test = op.word == kPortal || op.word == kOccluder;
			result.instructions.push_back({ op.word, test ? a_ops[i + 1].word : 0u, test ? mapped[op.yes] : 0u, test ? mapped[op.no] : 0u });
		}
		return result;
	}

	/** @brief Shared six-plane predicate, preserving the engine's comparisons, order of arithmetic and empty-set identities. */
	template <class PlaneSet>
	bool Test(const PlaneSet& a_set, std::uint32_t a_kind, const float a_centre[3], float a_radius)
	{
		bool outside = false, notContained = false;
		for (std::uint32_t p = 0; p < 6; ++p) {
			const auto& n = a_set.plane[p];
			const float d = ((n[1] * a_centre[1] + n[0] * a_centre[0]) + n[2] * a_centre[2]) - n[3];
			const bool active = (a_set.mask & (1u << p)) != 0;
			outside |= active && d <= -a_radius;
			notContained |= active && (d <= -a_radius || d < a_radius);
		}
		return a_kind == kPortal ? !outside : notContained;
	}

	/** @brief Reference evaluator for validated compact programs, including the diagnostic uniform scan. */
	template <class PlaneSet>
	bool Run(const Compiled& a_program, std::span<const PlaneSet> a_sets, const float a_centre[3], float a_radius, bool a_uniformScan = false)
	{
		std::uint32_t pc = 0;
		const auto steps = a_uniformScan ? static_cast<std::uint32_t>(a_program.instructions.size()) : a_program.maxSteps;
		for (std::uint32_t step = 0; step < steps; ++step) {
			const auto i = a_uniformScan ? step : pc;
			const auto& op = a_program.instructions.at(i);
			if (a_uniformScan && pc != i)
				continue;
			if (op.kind == kAccept || op.kind == kReject)
				return op.kind == kAccept;
			pc = Test(a_sets[op.set], op.kind, a_centre, a_radius) ? op.yes : op.no;
		}
		throw std::logic_error("portal bytecode: validated path did not terminate");
	}
}
