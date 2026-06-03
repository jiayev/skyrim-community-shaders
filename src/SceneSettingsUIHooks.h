#pragma once

struct Feature;

namespace SceneSettingsUIHooks
{
	class FeatureDrawGuard
	{
	public:
		FeatureDrawGuard(Feature* feature, bool enabled);
		~FeatureDrawGuard();

		FeatureDrawGuard(const FeatureDrawGuard&) = delete;
		FeatureDrawGuard& operator=(const FeatureDrawGuard&) = delete;

	private:
		Feature* previousFeature = nullptr;
		bool previousEnabled = false;
	};

	void Install();
}
