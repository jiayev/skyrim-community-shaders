#include "PostProcessFeature.h"

#include "CODBloom.h"
#include "ColourTransforms.h"
#include "HistogramAutoExposure.h"


const ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor>& PostProcessFeatureConstructor::GetFeatureConstructors()
{
	static ankerl::unordered_dense::map<std::string, PostProcessFeatureConstructor> retval = {
		GetFeatureConstructorPair<CODBloom>(),
		GetFeatureConstructorPair<HistogramAutoExporsure>(),
		GetFeatureConstructorPair<ColourTransforms>(),
	};
	return retval;
}
