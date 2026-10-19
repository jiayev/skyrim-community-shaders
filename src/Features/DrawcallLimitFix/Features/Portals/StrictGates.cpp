// The portal-strict point lights' per-object gate (StrictGates.h): the chain, its verdict per context, and the programs reader.

#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBytecode.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include <algorithm>
#include <cstring>

namespace DCLF::Portals
{
	namespace
	{
		struct Set
		{
			float plane[6][4];
			std::uint32_t mask, pad[3];
		};
		static_assert(sizeof(Set) == kPortalSetBytes);

		constexpr std::uint32_t kAlwaysDraw = 1u << 11;
		constexpr std::uint32_t kMaxDepth = 256;
	}

	ProgramWords::ProgramWords(std::span<const std::uint32_t> a_words) :
		words(a_words)
	{
		if (words.size() >= kPortalHeaderBytes / 4) {
			count = words[kPortalSetBytes / 4];
			records = words[kPortalSetBytes / 4 + 1];
			// A directory that does not fit is no program at all.
			if (kPortalHeaderBytes / 4 + std::size_t(count) * (kPortalDirectoryBytes / 4) > words.size())
				count = 0;
		}
	}

	const std::uint32_t* ProgramWords::Entry(std::uint32_t a_program) const
	{
		return a_program < count ? words.data() + kPortalHeaderBytes / 4 + std::size_t(a_program) * (kPortalDirectoryBytes / 4) : nullptr;
	}

	bool ProgramWords::Reached(std::uint32_t a_program) const
	{
		const auto* entry = Entry(a_program);
		return entry && (entry[0] & kPortalReached);
	}

	bool ProgramWords::HasOps(std::uint32_t a_program) const
	{
		const auto* entry = Entry(a_program);
		return entry && entry[1] != 0;
	}

	bool ProgramWords::SkipView(std::uint32_t a_program) const
	{
		const auto* entry = Entry(a_program);
		return entry && (entry[0] & kPortalSkipView);
	}

	bool ProgramWords::Pass(std::uint32_t a_program, const float a_centre[3], float a_radius) const
	{
		const auto* entry = Entry(a_program);
		if (!entry)
			return false;
		const std::size_t recordsBase = kPortalHeaderBytes / 4 + std::size_t(count) * (kPortalDirectoryBytes / 4);
		const std::size_t setsBase = recordsBase + std::size_t(records) * (kPortalOpBytes / 4);
		std::uint32_t op = entry[3];
		// The shader's loop: a producer invariant failure never accepts a bound.
		for (std::uint32_t step = 0; step < entry[7] && op < entry[1]; ++step) {
			const std::size_t at = recordsBase + (std::size_t(entry[4]) + op) * (kPortalOpBytes / 4);
			if (at + 4 > words.size())
				return false;
			const auto* record = words.data() + at;
			if (record[0] == PortalBytecode::kAccept || record[0] == PortalBytecode::kReject)
				return record[0] == PortalBytecode::kAccept;
			const std::size_t setAt = setsBase + (std::size_t(entry[5]) + record[1]) * (kPortalSetBytes / 4);
			if (record[1] >= entry[2] || setAt + kPortalSetBytes / 4 > words.size())
				return false;
			Set set;
			std::memcpy(&set, words.data() + setAt, sizeof(set));
			op = PortalBytecode::Test(set, record[0], a_centre, a_radius) ? record[2] : record[3];
		}
		return false;
	}

	void ChainOf(const RE::NiAVObject* a_object, GateChain& a_out)
	{
		a_out.clear();
		static const std::uintptr_t room = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSMultiBoundRoom[0] }.address();
		static const std::uintptr_t shared = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSPortalSharedNode[0] }.address();
		static const std::uintptr_t multiBound = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSMultiBoundNode[0] }.address();
		std::uint32_t depth = 0;
		for (const auto* node = a_object; node && depth < kMaxDepth; node = node->parent, ++depth) {
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(node);
			if (vtable == room) {
				const auto& data = static_cast<const RE::BSMultiBoundNode*>(node)->GetRuntimeData();
				a_out.push_back({ GateLink::kRoom, node, PortalViews::Get().ProgramOfRoom(node), static_cast<std::uint32_t>(data.cullingMode) });
			} else if (vtable == shared) {
				a_out.push_back({ GateLink::kShared, node });
			} else if (vtable == multiBound) {
				const auto& data = static_cast<const RE::BSMultiBoundNode*>(node)->GetRuntimeData();
				if (data.cullingMode == RE::BSCullingProcess::BSCPCullingType::kAllPass)
					a_out.push_back({ GateLink::kAllPass, node });
				else if (data.multiBound && !(Engine::FlagsOf(node) & kAlwaysDraw))
					a_out.push_back({ GateLink::kShape, node });
			}
		}
		std::reverse(a_out.begin(), a_out.end());
	}

	GateVerdict Evaluate(std::span<const GateLink> a_chain, const Context& a_context)
	{
		// StrictLightParity's model: Process1 from the request's mode 4, the node types' OnVisible on the way down.
		const ProgramWords programs(a_context.programs);
		GateVerdict verdict;
		std::uint32_t mode = 4, program = kPortalNoProgram;
		for (const auto& link : a_chain) {
			switch (link.kind) {
			case GateLink::kAllPass:
				if (mode != 3)
					mode = 1;
				break;
			case GateLink::kShape:
				if (mode != 1 && mode != 3 && mode != 4)
					verdict.unmodelled = true;
				break;
			case GateLink::kRoom:
				{
					const bool reached = link.program != kPortalNoProgram && programs.Reached(link.program);
					if (!reached && mode != 1 && mode != 3)
						return { false, false, verdict.unmodelled, kPortalNoProgram };
					if (link.roomMode != 1 && link.roomMode != 3)
						program = reached ? link.program : kPortalNoProgram;
					if (mode != 3 && mode != 4)
						mode = link.roomMode;
					break;
				}
			case GateLink::kShared:
				{
					const auto& shared = a_context.sharedNodes;
					const bool reached = std::find(shared.begin(), shared.end(), link.node) != shared.end();
					if (!reached && mode != 1 && mode != 3)
						return { false, false, verdict.unmodelled, kPortalNoProgram };
					program = kPortalNoProgram;
					break;
				}
			}
		}
		if (mode == 2)
			return { false, false, verdict.unmodelled, kPortalNoProgram };
		verdict.untested = mode == 1;
		// Process1's compound test: the room's frustum with operators, outside mode 3.
		verdict.program = !verdict.untested && mode != 3 && program != kPortalNoProgram && programs.HasOps(program) ? program : kPortalNoProgram;
		return verdict;
	}

	bool Admit(const Context& a_context, std::span<const GateLink> a_chain, const float a_centre[3], float a_radius)
	{
		const auto verdict = Evaluate(a_chain, a_context);
		if (!verdict.open)
			return false;
		if (verdict.program == kPortalNoProgram)
			return true;
		return ProgramWords(a_context.programs).Pass(verdict.program, a_centre, a_radius);
	}

	std::uint32_t GateWord(std::span<const GateLink> a_chain, const Context& a_context)
	{
		if (a_context.kind != Context::Kind::PointLight || !a_context.strict)
			return kPortalNoProgram;
		const auto verdict = Evaluate(a_chain, a_context);
		return verdict.open ? verdict.program : kGateClosed;
	}
}
