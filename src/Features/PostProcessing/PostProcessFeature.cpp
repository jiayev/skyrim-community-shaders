#include "PostProcessFeature.h"

const ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor>& PostProcessFeatureConstructor::GetFeatureConstructors()
{
	static ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor> retval = {
		GetFeatureConstructorPair<TestFeature>(),
	};
	return retval;
}
