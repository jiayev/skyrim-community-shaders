#pragma once

// The classify input contract's Core side (Api/ClassifyInputs.h): the registered inputs, the set of their values a frame or a scene pass
// takes (ClassifyInputSet), the per-thread snapshot a scene pass installs (ClassifyInputsScope) and the read classification makes
// (ClassifyInputOn); and the inputs needed by name, bound after every install (BindClassifyInputNeeds).

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace DCLF::Extensions
{
	/** @brief The most classify inputs there may be: a set holds each in a bit. An input past them fails the install: grow this first. */
	inline constexpr std::size_t kClassifyInputSlots = 8;
	/**
	 * @brief The most inputs needed by name there may be (InstallContext::NeedClassifyInput): each a slot of the bound table. A need past
	 * them fails the install: grow this first.
	 */
	inline constexpr std::size_t kClassifyInputNeedSlots = 4;
	/** @brief A needed input's id: this bit and its need slot (a registered input's id is its index). */
	inline constexpr std::uint32_t kClassifyInputNeedBit = 0x8000'0000u;

	namespace Detail
	{
		// By need slot: the registered input it is bound to (none: ~0u, read false). Written only by BindClassifyInputNeeds (the main
		// thread, in InstallFeatures, before DCLF's threads exist: their start orders these writes before every read), read-only after.
		inline std::array<std::uint32_t, kClassifyInputNeedSlots> boundClassifyInputNeeds = [] {
			std::array<std::uint32_t, kClassifyInputNeedSlots> bound{};
			bound.fill(~0u);
			return bound;
		}();
	}

	/** @brief a_input as the registered input it reads: itself, or a need's bound input (none while unbound). */
	inline Api::ClassifyInputId ResolvedClassifyInput(Api::ClassifyInputId a_input)
	{
		if (a_input.index == ~0u || !(a_input.index & kClassifyInputNeedBit))
			return a_input;
		const auto slot = a_input.index & ~kClassifyInputNeedBit;
		return { slot < kClassifyInputNeedSlots ? Detail::boundClassifyInputNeeds[slot] : ~0u };
	}

	/** @brief Every input's value at once, by its id's index (a bit each). */
	struct ClassifyInputSet
	{
		std::uint8_t bits = 0;

		bool On(Api::ClassifyInputId a_input) const
		{
			const auto input = ResolvedClassifyInput(a_input);
			return input.index < kClassifyInputSlots && ((bits >> input.index) & 1u) != 0;
		}

		bool operator==(const ClassifyInputSet&) const = default;
	};
	static_assert(kClassifyInputSlots <= sizeof(ClassifyInputSet::bits) * 8);

	/** @brief A registered classify input (its descriptor, the name owned). */
	struct ClassifyInputEntry
	{
		std::string name;
		decltype(Api::ClassifyInputContribution::sample) sample = nullptr;
	};
	/** @brief Any thread, after InstallFeatures (empty before): the inputs, in registration order (an index is the input's id). */
	std::span<const ClassifyInputEntry> RegisteredClassifyInputs();
	/**
	 * @brief Main thread, in InstallFeatures after every install (Registry): each need (by slot, its name) bound to the input registered
	 * under that name, or left unbound. Once.
	 */
	void BindClassifyInputNeeds(std::span<const std::string> a_needs);

	/**
	 * @brief Any thread: every input's value, the thread's snapshot while a ClassifyInputsScope is installed, else each live (its sample).
	 * The frame driver's per-frame sample (a change drops the classification caches) and a scene pass's at its start.
	 */
	ClassifyInputSet SampleClassifyInputs();
	/**
	 * @brief Any thread: a_input's value, as SampleClassifyInputs takes it (the thread's snapshot, else live); false for none (its feature
	 * is not installed).
	 */
	bool ClassifyInputOn(Api::ClassifyInputId a_input);

	/**
	 * @brief a_inputs are this thread's inputs while the scope lives (null: live), as a scene pass took them at its start: the menu may
	 * switch a feature while the pass's pool evaluations run.
	 */
	class ClassifyInputsScope
	{
	public:
		explicit ClassifyInputsScope(const ClassifyInputSet* a_inputs);
		~ClassifyInputsScope();
		ClassifyInputsScope(const ClassifyInputsScope&) = delete;
		ClassifyInputsScope& operator=(const ClassifyInputsScope&) = delete;

	private:
		const ClassifyInputSet* previous;
	};
}
