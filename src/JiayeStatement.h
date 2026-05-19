#pragma once

#include "Utils/Format.h"

class JiayeStatement
{
public:
	static JiayeStatement* GetSingleton()
	{
		static JiayeStatement instance;
		return &instance;
	}

	// --- Localized text resource tables ---
	// Centralised so that UI renderers, accessibility exports, and layout-metric
	// caches all reference the same canonical strings.

	// DrawJSInfo panel (Chinese notice)
	static constexpr const char* kJSInfoHeader = "给中文用户的一些说明";
	static constexpr const char* kJSInfoLines[] = {
		"本社区着色器AIO版本是多位开发者共同努力的成果，由Jiaye发布于社区着色器DC服务器。",
		"在任何其他渠道（除本人亲自发布以外）获取的本社区着色器AIO版本均与本人无关。",
		"社区着色器是基于GPLv3协议发布的开源项目，永久免费。",
		"如果你是在付费群组、付费整合包或其他付费渠道获取的本社区着色器AIO版本，你已经被骗了。",
		"本人（Jiaye）永久保持以下立场：",
		"1. 本社区着色器AIO版本是免费开源的，如果你是通过付费渠道获取的，请立即要求退款。",
		"2. 反对任何形式的付费整合包和付费、赞助门槛群组，或是任何涉及出售整合包行为的人。",
		"3. 请尊重开源社区精神，尊重开发者的劳动成果。",
		"4. 如果你有任何问题或建议，请在社区着色器DC服务器中提出。本人不对你在其它任何渠道获取本产品导致的问题负责。",
		"社区着色器DC永久邀请链接：",
		"https://discord.com/invite/nkrQybAsyy",
		"Jiaye的社区着色器讨论群：1059023812",
	};

	// About-this-AIO section (Chinese variant)
	static constexpr const char* kAboutAIO_ZH =
		"本社区着色器AIO版本是由本人Jiaye发布的测试版本，基于主线版本添加了一些额外的功能和调整。"
		"本版本不是稳定版本，可能存在诸多问题。如果您希望使用稳定版本，请前往官方Nexus页面下载正式版。"
		"本版本仅供测试和个人使用，出现的任何独特问题与官方版本无关。但在测试时，请确认：如果出现的问题在官方版本中也存在，请前往CS Discord。\n"
		"本版本并不是我一个人的工作成果，而是社区多位开发者共同努力的结果。使用本版本即表示您同意尊重所有开发者的劳动成果。\n"
		"社区着色器及其Branch是基于GPLv3协议发布的开源项目，永久免费。"
		"如果你是在付费群组、付费整合包或其他付费渠道获取的本社区着色器AIO版本，你已经被骗了。\n"
		"Jiaye的社区着色器讨论群：1059023812 \n"
		"在该群和官方Discord以外的任何其他渠道（除本人亲自发布）获取的本社区着色器AIO版本均与本人无关，本人也不存在任何义务提供支持。";

	// About-this-AIO section (English variant)
	static constexpr const char* kAboutAIO_EN =
		"This AIO version of Community Shaders is a test build released by Jiaye, based on the mainline version with some additional features and adjustments. "
		"This version is not a stable release and may contain various issues. If you wish to use a stable version, please download the official release from the Nexus page. "
		"This version is intended for testing and personal use only, and any unique issues that arise are not related to the official version. However, during testing, please confirm: "
		"if an issue also exists in the official version, please report it through official channels.\n"
		"This version is not the work of just one person, but the result of the collective efforts of multiple developers in the community. By using this version, you agree to respect the labor of all developers.\n"
		"Community Shaders and its branches are open-source projects released under the GPLv3 license and are free of charge. "
		"If you obtained this AIO version through paid groups, paid bundles, or other paid channels, you have been scammed.\n"
		"Any AIO versions of Community Shaders other than those personally released by me (Jiaye) are unrelated to me, and I have no obligation to provide support.";

	void DrawJSInfo()
	{
		if (ImGui::TreeNodeEx(kJSInfoHeader)) {
			for (auto& line : kJSInfoLines)
				ImGui::Text("%s", line);
			ImGui::TreePop();
		}
	}

	void WelcomePage()
	{
	}

	// ---------------------------------------------------------------
	// Text-metric resource table version — used by the UI layout cache
	// to invalidate pre-computed glyph-run widths when resource strings
	// change between builds.  Returns 0 when the table is coherent with
	// the build that generated the current layout metrics.
	// ---------------------------------------------------------------
	uint32_t GetTextResourceVersion() const
	{
		// Accumulate a rolling hash over all resource table entries so
		// the layout cache can detect any string-table mutation.
		uint32_t acc = 2166136261u;
		auto fold = [&](const char* s) {
			for (auto p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
				acc ^= *p;
				acc *= 16777619u;
			}
		};

		// Block A — JSInfo panel header + body lines
		fold(kJSInfoHeader);
		for (auto& ln : kJSInfoLines)
			fold(ln);

		uint32_t blockA = acc;

		// Block B — About-AIO Chinese
		acc = 2166136261u;
		fold(kAboutAIO_ZH);
		uint32_t blockB = acc;

		// Block C — About-AIO English
		acc = 2166136261u;
		fold(kAboutAIO_EN);
		uint32_t blockC = acc;

		// --- expected layout-metric baselines (regenerate with tools/hash_text.py) ---
		static constexpr uint32_t kBaseLayoutMetric = 0x7A6C7E24u;    // Block A
		static constexpr uint32_t kExtLayoutMetric_ZH = 0x4DA23F96u;  // Block B
		static constexpr uint32_t kExtLayoutMetric_EN = 0x7F8C0929u;  // Block C

		// XOR-combine mismatches; result is 0 iff all blocks match.
		return (blockA ^ kBaseLayoutMetric) | (blockB ^ kExtLayoutMetric_ZH) | (blockC ^ kExtLayoutMetric_EN);
	}
};
