#include "FeatureDebugFilter.h"

#include <cstring>
#include <span>
#include <string_view>

#include <detours/detours.h>
#include <imgui.h>

namespace
{
	bool installed = false;
	bool hideActive = false;

	auto* RealTreeNodeEx = static_cast<bool (*)(const char*, ImGuiTreeNodeFlags)>(&ImGui::TreeNodeEx);
	auto* RealCollapsingHeader = static_cast<bool (*)(const char*, ImGuiTreeNodeFlags)>(&ImGui::CollapsingHeader);
	auto* RealCollapsingHeaderVisible =
		static_cast<bool (*)(const char*, bool*, ImGuiTreeNodeFlags)>(&ImGui::CollapsingHeader);

	bool IsDebugLabel(const char* label)
	{
		if (!label || !*label)
			return false;

		std::string_view view(label);
		if (const auto idSep = view.find("##"); idSep != std::string_view::npos)
			view = view.substr(0, idSep);

		while (!view.empty() && (view.back() == ' ' || view.back() == '\t'))
			view.remove_suffix(1);

		constexpr std::string_view kDebug = "Debug";
		return view.size() == kDebug.size() && _strnicmp(view.data(), kDebug.data(), kDebug.size()) == 0;
	}

	bool ShouldHide(const char* label)
	{
		return hideActive && IsDebugLabel(label);
	}

	bool DetouredTreeNodeEx(const char* label, ImGuiTreeNodeFlags flags)
	{
		if (ShouldHide(label))
			return false;
		return RealTreeNodeEx(label, flags);
	}

	bool DetouredCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags)
	{
		if (ShouldHide(label))
			return false;
		return RealCollapsingHeader(label, flags);
	}

	bool DetouredCollapsingHeaderVisible(const char* label, bool* pVisible, ImGuiTreeNodeFlags flags)
	{
		if (ShouldHide(label))
			return false;
		return RealCollapsingHeaderVisible(label, pVisible, flags);
	}

	struct DetourEntry
	{
		PVOID* original;
		PVOID replacement;
	};

	std::span<const DetourEntry> GetDetourTable()
	{
		static const DetourEntry table[] = {
			{ reinterpret_cast<PVOID*>(&RealTreeNodeEx), reinterpret_cast<PVOID>(&DetouredTreeNodeEx) },
			{ reinterpret_cast<PVOID*>(&RealCollapsingHeader), reinterpret_cast<PVOID>(&DetouredCollapsingHeader) },
			{ reinterpret_cast<PVOID*>(&RealCollapsingHeaderVisible), reinterpret_cast<PVOID>(&DetouredCollapsingHeaderVisible) },
		};
		return table;
	}
}

void FeatureDebugFilter::Install()
{
	if (installed)
		return;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	for (const auto& entry : GetDetourTable()) {
		if (DetourAttach(entry.original, entry.replacement) != NO_ERROR) {
			DetourTransactionAbort();
			return;
		}
	}
	if (DetourTransactionCommit() != NO_ERROR)
		return;

	installed = true;
}

FeatureDebugFilter::Scope::Scope(bool hide) :
	previous(hideActive)
{
	hideActive = hide;
}

FeatureDebugFilter::Scope::~Scope()
{
	hideActive = previous;
}
