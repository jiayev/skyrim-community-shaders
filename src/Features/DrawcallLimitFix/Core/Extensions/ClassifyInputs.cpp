// The classify inputs' samples and the scene passes' snapshots (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"

namespace DCLF::Extensions
{
	namespace
	{
		// The inputs a scene pass took at its start, installed on each thread its evaluations run on (ClassifyInputsScope); null: live.
		thread_local const ClassifyInputSet* snapshot = nullptr;
	}

	ClassifyInputSet SampleClassifyInputs()
	{
		if (snapshot)
			return *snapshot;
		ClassifyInputSet set;
		const auto inputs = RegisteredClassifyInputs();
		for (std::size_t i = 0; i < inputs.size(); ++i)
			if (inputs[i].sample())
				set.bits |= static_cast<std::uint8_t>(1u << i);
		return set;
	}

	bool ClassifyInputOn(Api::ClassifyInputId a_input)
	{
		if (snapshot)
			return snapshot->On(a_input);
		const auto inputs = RegisteredClassifyInputs();
		return a_input.index < inputs.size() && inputs[a_input.index].sample();
	}

	ClassifyInputsScope::ClassifyInputsScope(const ClassifyInputSet* a_inputs) :
		previous(snapshot)
	{
		snapshot = a_inputs;
	}

	ClassifyInputsScope::~ClassifyInputsScope() { snapshot = previous; }
}
