#include "TextureStreaming.h"

#include "TextureStreaming/Policy.h"
#include "TextureStreaming/Streamer.h"

#include "Menu.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.texture_streaming."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TextureStreamingFeature::Settings,
	Distance,
	Feedback,
	DistanceScale,
	BaseSize,
	RateMB,
	BudgetMB,
	VisibleOffset)

namespace
{
	const std::uint32_t kMinRate = 32, kMaxRate = 2048;
	const std::uint32_t kMinBudget = 0, kMaxBudget = 65536;
	constexpr std::uint64_t kMiB = 1024 * 1024;
	const std::uint32_t kMinBase = 64, kMaxBase = 1024;
}

void TextureStreamingFeature::DrawSettings()
{
	if (!TextureStreaming::Active())
		ImGui::TextWrapped("%s", T(TKEY("inactive"), "Not streaming this session: Drawcall Limit Fix is off or unavailable, so textures load as usual."));

	bool changed = false;
	changed |= ImGui::Checkbox(T(TKEY("distance"), "Use Distance"), &settings.Distance);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_tooltip"), "Picks each texture's resolution from how far away its surfaces are, whichever way the camera faces."));
	}

	changed |= ImGui::Checkbox(T(TKEY("feedback"), "Use Shader Feedback"), &settings.Feedback);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("feedback_tooltip"), "Shaders report the resolution their surfaces want, which corrects surfaces the distance misjudges."));
	}

	changed |= ImGui::SliderFloat(T(TKEY("distance_scale"), "Distance Sharpness"), &settings.DistanceScale, 0.25f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("distance_scale_tooltip"), "Above 1 keeps sharper textures at the same distance, using more VRAM."));
	}

	changed |= ImGui::SliderScalar(T(TKEY("rate"), "Loading Rate"), ImGuiDataType_U32, &settings.RateMB, &kMinRate, &kMaxRate, "%u MiB/s");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("rate_tooltip"), "How much texture detail starts loading each second. Lower values may reduce stutter, while sharp textures arrive later."));
	}

	const auto stats = TextureStreaming::GetStats();
	// Up to twice the card, once it is known: that is the range worth setting, and a slider to 64 GiB could not set it finely.
	const std::uint32_t budgetLimit = stats.cardBytes ? std::clamp(static_cast<std::uint32_t>(2 * stats.cardBytes / kMiB), settings.BudgetMB, kMaxBudget) : kMaxBudget;
	changed |= ImGui::SliderScalar(T(TKEY("budget"), "VRAM Budget"), ImGuiDataType_U32, &settings.BudgetMB, &kMinBudget, &budgetLimit, settings.BudgetMB ? "%u MiB" : T(TKEY("budget_driver"), "Driver's budget"));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("budget_tooltip"), "How much memory the game may use before texture detail away from the camera is given up. What is on screen still loads what it asks for, unless On-Screen Detail Offset lets it step down. The driver's budget leaves out what other programs hold on the card. Past it, the overflow goes to system memory: sharper, but it can be slower to draw."));
	}
	if (stats.driverBudgetBytes && settings.BudgetMB > stats.driverBudgetBytes / kMiB)
		ImGui::TextWrapped("%s", T(TKEY("budget_over_card"), "Above the driver's budget: textures that do not fit are drawn from system memory, which can lower the frame rate."));

	changed |= ImGui::SliderFloat(T(TKEY("visible_offset"), "On-Screen Detail Offset"), &settings.VisibleOffset, 0.0f, TextureStreaming::Policy::kMaxVisibleOffset, "%.2f mips");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("visible_offset_tooltip"), "When memory is over the VRAM Budget and nothing away from the camera is left to give up, what is on screen may step down by up to this many mips, and only as far as it needs. Each mip is about a quarter of the memory. At 0, it loads what it asks for: sharpest, but it can overflow into system memory and lower the frame rate."));
	}

	changed |= ImGui::SliderScalar(T(TKEY("base_size"), "Smallest Size"), ImGuiDataType_U32, &settings.BaseSize, &kMinBase, &kMaxBase, "%u");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("base_size_tooltip"), "The largest side a texture is held at when nothing needs more. Takes effect after a restart."));
	}

	if (changed)
		Apply();

	if (stats.budgetBytes) {
		ImGui::Text(T(TKEY("budget_use"), "Memory use: %llu of %llu MiB (card: %llu MiB, %llu MiB in system memory)"), stats.usedBytes / kMiB, stats.budgetBytes / kMiB, stats.cardBytes / kMiB, stats.overflowBytes / kMiB);
		ImGui::Text(T(TKEY("allowance"), "Room for textures: %llu MiB, wanted: %llu MiB (%llu on screen)"), stats.allowanceBytes / kMiB, stats.wantedBytes / kMiB, stats.visibleBytes / kMiB);
		ImGui::Text(T(TKEY("pullback"), "Detail held back: %.2f mips away from the camera, %.2f on screen"), stats.distancePullback, stats.visiblePullback);
	}
	ImGui::Text(T(TKEY("usage"), "Texture VRAM: %llu of %llu MiB"), stats.residentBytes / kMiB, stats.fullBytes / kMiB);
	ImGui::Text(T(TKEY("textures"), "Textures streamed: %llu (%llu left as they were)"), stats.textures, stats.passedThrough);
	if (stats.loadFailures || stats.engineFailures)
		ImGui::Text(T(TKEY("failures"), "Failed loads: %llu (details in the log)"), stats.loadFailures + stats.engineFailures);
}

void TextureStreamingFeature::LoadSettings(json& o_json)
{
	settings = o_json;
	Apply();
}

void TextureStreamingFeature::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TextureStreamingFeature::RestoreDefaultSettings()
{
	settings = {};
	Apply();
}

void TextureStreamingFeature::Apply()
{
	settings.DistanceScale = std::clamp(settings.DistanceScale, 0.25f, 4.0f);
	settings.RateMB = std::clamp(settings.RateMB, kMinRate, kMaxRate);
	settings.BudgetMB = std::clamp(settings.BudgetMB, kMinBudget, kMaxBudget);
	settings.VisibleOffset = std::clamp(settings.VisibleOffset, 0.0f, TextureStreaming::Policy::kMaxVisibleOffset);
	settings.BaseSize = std::clamp(settings.BaseSize, kMinBase, kMaxBase);
	TextureStreaming::SetSettings({ settings.Distance, settings.Feedback, settings.DistanceScale, settings.BaseSize, settings.RateMB, settings.BudgetMB, settings.VisibleOffset });
}
