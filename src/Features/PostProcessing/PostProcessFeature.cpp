#include "PostProcessFeature.h"

#include "CODBloom.h"

const ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor>& PostProcessFeatureConstructor::GetFeatureConstructors()
{
	static ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor> retval = {
		GetFeatureConstructorPair<TestFeature>(),
		GetFeatureConstructorPair<CODBloom>(),
	};
	return retval;
}
