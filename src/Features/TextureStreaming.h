#pragma once

#include "Feature.h"
#include "I18n/I18n.h"

/**
 * @brief The menu's face of texture streaming (src/TextureStreaming, docs/development/texture-streaming.md).
 *
 * Game textures hold only the mips their surfaces need, which needs Drawcall Limit Fix to schedule them. Turning the feature
 * off takes effect after a restart: textures only become streamed as they load.
 */
struct TextureStreamingFeature : Feature
{
	virtual inline std::string GetName() override { return "Texture Streaming"; }
	virtual std::string GetDisplayName() override { return T("feature.texture_streaming.name", "Texture Streaming"); }
	virtual inline std::string GetShortName() override { return "TextureStreaming"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.texture_streaming.description", "Keeps only the texture detail the scene needs in VRAM, and loads the rest as surfaces come close."),
			{ T("feature.texture_streaming.key_feature_1", "Picks each texture's resolution from how far away its surfaces are and what the shaders report"),
				T("feature.texture_streaming.key_feature_2", "Fits a budget you can set above or below the driver's: as memory runs short, detail away from the camera goes first, so what is on screen stays sharp"),
				T("feature.texture_streaming.key_feature_3", "Needs Drawcall Limit Fix: without it, textures load as usual") } };
	}

	virtual bool IsCore() const override { return true; }
	/** @brief On by default, unlike other pre-release core features. */
	virtual bool IsDisabledByDefault() const override { return false; }

	/** @brief Draws the settings and the current totals. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	struct Settings
	{
		bool Distance = true;          // resolution from the distance to a surface
		bool Feedback = true;          // resolution the shaders report
		float DistanceScale = 1.0f;    // the distance floor's sharpness: above 1 finer mips
		std::uint32_t BaseSize = 128;  // the smallest window's largest side. Read at the first texture load.
		std::uint32_t RateMB = 256;    // detail started loading per second
		std::uint32_t BudgetMB = 0;    // the memory to keep to, above or below the driver's budget; 0 for the driver's
		float VisibleOffset = 1.0f;    // over the budget, what is on screen may step down this many mips; 0 loads what it asks for
	};

	Settings settings;

private:
	/** @brief Keeps the values in range and hands them to the streamer. */
	void Apply();
};
