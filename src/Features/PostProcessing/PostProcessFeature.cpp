#include "PostProcessFeature.h"

#include "CODBloom.h"
#include "HistogramAutoExporsure.h"
#include "MathTonemapper.h"

const ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor>& PostProcessFeatureConstructor::GetFeatureConstructors()
{
	static ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor> retval = {
		GetFeatureConstructorPair<CODBloom>(),
		GetFeatureConstructorPair<HistogramAutoExporsure>(),
		GetFeatureConstructorPair<MathTonemapper>(),
	};
	return retval;
}
