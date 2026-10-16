#pragma once

// The classify input contract's Core side (Api/ClassifyInputs.h): the registered inputs, the set of their values a frame or a scene pass
// takes (ClassifyInputSet), the per-thread snapshot a scene pass installs (ClassifyInputsScope) and the read classification makes
// (ClassifyInputOn).

#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace DCLF::Extensions
{
	/** @brief The most classify inputs there may be: a set holds each in a bit. An input past them fails the install: grow this first. */
	inline constexpr std::size_t kClassifyInputSlots = 8;

	/** @brief Every input's value at once, by its id's index (a bit each). */
	struct ClassifyInputSet
	{
		std::uint8_t bits = 0;

		bool On(Api::ClassifyInputId a_input) const { return a_input.index < kClassifyInputSlots && ((bits >> a_input.index) & 1u) != 0; }

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
