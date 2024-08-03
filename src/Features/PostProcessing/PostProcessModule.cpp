#include "PostProcessModule.h"

const ankerl::unordered_dense::map<std::string, PostProcessModuleConstructor>& PostProcessModuleConstructor::GetModuleConstructors()
{
	static ankerl::unordered_dense::map<std::string, PostProcessModuleConstructor> retval = {
		{ TestModule().GetName(), { []() { return new TestModule(); }, TestModule().GetName(), TestModule().GetDesc() } }
	};
	return retval;
}
