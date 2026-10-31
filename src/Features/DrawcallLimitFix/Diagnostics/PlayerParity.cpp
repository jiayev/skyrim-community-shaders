// The compatibility monitor (PlayerParity.h): the request, applied at the frame's start; the offenders, the coordinator's, published
// to the menu as an immutable snapshot; the report.
#include "PlayerParity.h"

#include "Hooks.h"
#include "Plugin.h"
#include "Utils/UI.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorEvents.h"

#include <imgui.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>

namespace DCLF::Diagnostics::PlayerParity
{
	namespace
	{
		std::atomic<bool> requested{ false };  // the menu's
		std::atomic<bool> active{ false };     // the render thread's, applied at the frame's start
		std::atomic<bool> resetPending{ false };
		std::atomic<bool> clearPending{ false };

		// The coordinator's: the offenders by (reference, or the shared object for a property or an alpha; record type; fields), in the
		// order found, each with the references it was found under; and whether they changed since published.
		struct Table
		{
			std::map<std::tuple<std::uintptr_t, std::uint8_t, std::uint32_t>, std::size_t> index;
			std::vector<std::vector<std::uint32_t>> referencesOf;
			Offenders offenders;
			bool dirty = false;
		} table;
		std::atomic<std::shared_ptr<const Offenders>> published{ std::make_shared<const Offenders>() };

		// The render thread's leaks, to the coordinator's table (NoteCheck drains them).
		struct LeakNote
		{
			std::uint32_t form = 0;
			std::string example;
		};
		EventQueue<LeakNote, 64> leakNotes;

		// The longest example kept: a mirror parity line can run to several hundred characters.
		constexpr std::size_t kExampleChars = 400;
		// The offenders kept at most; past it a new one only counts (one noisy writer must not grow the table without bound).
		constexpr std::size_t kMaxOffenders = 4096;

		void Restart()
		{
			table = {};
			table.offenders.sessionFrame = SceneCapture::Frame();
			table.dirty = true;
		}

		const char* TypeName(std::uint8_t a_type)
		{
			return a_type < SceneMirror::kTypeNames.size() ? SceneMirror::kTypeNames[a_type] : a_type == kLeak ? "geometry" : a_type == kEvicted ? "subtree" : "?";
		}

		std::string FieldNames(std::uint8_t a_type, std::uint32_t a_fields)
		{
			switch (a_type) {
			case 0:
				return SceneMirror::Fields<SceneCapture::NodeRecord>(a_fields);
			case 1:
				return SceneMirror::Fields<SceneCapture::GeometryRecord>(a_fields);
			case 2:
				return SceneMirror::Fields<SceneCapture::PropertyRecord>(a_fields);
			case 3:
				return SceneMirror::Fields<SceneCapture::AlphaRecord>(a_fields);
			case kLeak:
				return "drawn by the engine as well as by DCLF";
			case kEvicted:
				return "left the world with no detach DCLF sees";
			default:
				return fmt::format("{:#x}", a_fields);
			}
		}

		/** @brief The plugin a form ID's load order index names (a reference no longer loaded still names its plugin). */
		std::string PluginOfId(std::uint32_t a_form)
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			if (!handler || !a_form)
				return {};
			const auto index = a_form >> 24;
			const RE::TESFile* file = index == 0xFE ? handler->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((a_form >> 12) & 0xFFF)) :
			                          index == 0xFF ? nullptr :
			                                          handler->LookupLoadedModByIndex(static_cast<std::uint8_t>(index));
			return file ? std::string(file->GetFilename()) : index == 0xFF ? "created in game" : "?";
		}

		/** @brief An offender's reference as the menu and the report name it: plugin, base object, and the plugin that last edits it. */
		struct Named
		{
			std::string plugin, lastPlugin, reference;
		};

		Named NameOf(std::uint32_t a_form)
		{
			Named named;
			if (!a_form) {
				named.plugin = "(none)";
				named.reference = "an object above any reference (a cell's or the world's node)";
				return named;
			}
			named.plugin = PluginOfId(a_form);
			auto* reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_form);
			if (!reference) {
				named.reference = fmt::format("{:08X} (not loaded now)", a_form);
				return named;
			}
			if (const auto* last = reference->GetFile(-1); last && named.plugin != last->GetFilename())
				named.lastPlugin = last->GetFilename();
			const auto* base = reference->GetBaseObject();
			const char* name = base ? base->GetName() : nullptr;
			named.reference = fmt::format("{:08X} ({}{}{}{}{})", a_form, base ? RE::FormTypeToString(base->GetFormType()) : "?", base ? fmt::format(" {:08X}", base->GetFormID()) : "",
				name && *name ? " '" : "", name && *name ? name : "", name && *name ? "'" : "");
			return named;
		}

		void OnFrameBegin(const FrameBegin&)
		{
			// The frame's start: every switchable observer on or off for the whole frame (Hooks.h); on starts a fresh session.
			const bool want = requested.load(std::memory_order_relaxed);
			if (want == active.load(std::memory_order_relaxed))
				return;
			active.store(want, std::memory_order_relaxed);
			if (want)
				resetPending.store(true, std::memory_order_release);
			SetSwitchableArmed(want);
			logger::info("[DCLF] compatibility monitor {}", want ? "on: watching for engine state other plugins change with no hook DCLF sees" : "off");
		}
	}

	void Install()
	{
		Subscribe<FrameBegin>(OnFrameBegin);
		// A subtree another plugin detached by a path no hook sees (the coordinator's; one offender per class of its root).
		SubscribeSwitchable<MirrorEvicted>([](const MirrorEvicted& a_event) {
			NoteMissed(a_event.rtti, 0, kEvicted, 0, fmt::format("a {} with {} records under it, {}", a_event.rtti ? a_event.rtti : "?", a_event.records, a_event.what));
		});
		if (SwitchEnabled(Switch::PlayerParity))
			requested.store(true, std::memory_order_relaxed);
	}

	void Request(bool a_on) { requested.store(a_on, std::memory_order_relaxed); }
	bool Requested() { return requested.load(std::memory_order_relaxed); }
	bool Active() { return active.load(std::memory_order_relaxed); }

	bool TakeReset()
	{
		if (!resetPending.exchange(false, std::memory_order_acq_rel))
			return false;
		Restart();
		leakNotes.Discard();  // from before the session (the queue's one consumer is this thread)
		return true;
	}

	void NoteMissed(const void* a_object, std::uint32_t a_form, std::uint8_t a_type, std::uint32_t a_fields, std::string_view a_example)
	{
		const auto frame = SceneCapture::Frame();
		// A property's or an alpha's write shows on every reference whose mesh shares it: one offender, by the object (an eviction's: by
		// its root's class).
		const bool shared = a_type == 2 || a_type == 3 || a_type == kEvicted;
		const auto key = std::make_tuple(shared ? reinterpret_cast<std::uintptr_t>(a_object) : a_form, a_type, a_fields);
		if (const auto found = table.index.find(key); found != table.index.end()) {
			auto& offender = table.offenders.entries[found->second];
			++offender.count;
			offender.lastFrame = frame;
			auto& references = table.referencesOf[found->second];
			if (std::find(references.begin(), references.end(), a_form) == references.end()) {
				references.push_back(a_form);
				offender.references = static_cast<std::uint32_t>(references.size());
			}
			table.dirty = true;
			return;
		}
		if (table.offenders.entries.size() >= kMaxOffenders) {
			++table.offenders.overflow;
			table.dirty = true;
			return;
		}
		table.index.emplace(key, table.offenders.entries.size());
		table.referencesOf.push_back({ a_form });
		table.offenders.entries.push_back({ a_form, 1, a_type, a_fields, 1, frame, frame, std::string(a_example.substr(0, kExampleChars)) });
		table.dirty = true;
		// One line per new offender (not per check): the plugin by the form ID's load order index, the rest in the menu and the report.
		if (a_type == kLeak || a_type == kEvicted)
			logger::warn("[DCLF] compatibility monitor: a {} {}: {}{} <- OFFENDER", TypeName(a_type), FieldNames(a_type, a_fields), a_example,
				a_form ? fmt::format(", reference {:08X} ({})", a_form, PluginOfId(a_form)) : "");
		else
			logger::warn("[DCLF] compatibility monitor: {} {} changed with no hook on an object of reference {:08X} ({}) <- OFFENDER", TypeName(a_type),
				FieldNames(a_type, a_fields), a_form, a_form ? PluginOfId(a_form) : "no reference");
	}

	void NoteLeak(std::uint32_t a_form, std::string a_example)
	{
		leakNotes.Push({ a_form, std::move(a_example) });
	}

	void NoteCheck()
	{
		if (clearPending.exchange(false, std::memory_order_acq_rel))
			Restart();
		leakNotes.Drain([](LeakNote&& a_note) { NoteMissed(nullptr, a_note.form, kLeak, 0, a_note.example); });
		++table.offenders.probes;
		// Each check counts, so the menu's numbers move; a copy every 30 checks while only counts change, at once for a new offender.
		if (!table.dirty && table.offenders.probes % 30 != 0)
			return;
		table.dirty = false;
		published.store(std::make_shared<const Offenders>(table.offenders), std::memory_order_release);
	}

	std::shared_ptr<const Offenders> Current()
	{
		return published.load(std::memory_order_acquire);
	}

	void Clear()
	{
		clearPending.store(true, std::memory_order_release);
	}

	std::string WriteReport()
	{
		const auto directory = logger::log_directory();
		if (!directory)
			return "no log directory";
		const auto now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char stamp[32]{};
		std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
		const auto path = *directory / fmt::format("CommunityShaders-DCLF-Compat-{}.txt", stamp);
		std::ofstream out(path);
		if (!out)
			return fmt::format("could not write {}", path.string());

		const auto offenders = Current();
		out << fmt::format("Community Shaders {} (build {}), Drawcall Limit Fix compatibility monitor report\n", Plugin::VERSION.string(), Plugin::BUILD_DESCRIBE);
		out << fmt::format("Game {}; monitor {}; {} checks since frame {}, now frame {}\n\n", REL::Module::get().version().string("."), Active() ? "on" : "off",
			offenders->probes, offenders->sessionFrame, SceneCapture::Frame());
		out << "Each offender is engine state an object's reference holds that changed with no hook DCLF sees (another plugin's write, most\n"
			   "likely): DCLF may draw it stale until something else updates it. The plugin is the reference's; the writer may be any plugin.\n\n";
		out << fmt::format("Offenders: {}{}\n", offenders->entries.size(), offenders->overflow ? fmt::format(" (and {} more past the table's limit)", offenders->overflow) : "");
		for (const auto& offender : offenders->entries) {
			const auto named = NameOf(offender.form);
			out << fmt::format("- {} {}: reference {}{}, plugin {}{}; {} checks, frames {}-{}\n  first: {}\n", TypeName(offender.type),
				FieldNames(offender.type, offender.fields), named.reference,
				offender.references > 1 ? fmt::format(" and {} other references sharing it", offender.references - 1) : "", named.plugin,
				named.lastPlugin.empty() ? "" : fmt::format(" (last edited by {})", named.lastPlugin), offender.count, offender.firstFrame, offender.lastFrame, offender.example);
		}
		if (auto* handler = RE::TESDataHandler::GetSingleton()) {
			out << "\nLoad order:\n";
			for (const auto* file : handler->files)
				if (file && file->compileIndex != 0xFF)
					out << (file->compileIndex == 0xFE ? fmt::format("  FE:{:03X} {}\n", file->smallFileCompileIndex, file->GetFilename()) :
					                                       fmt::format("  {:02X}     {}\n", file->compileIndex, file->GetFilename()));
		}
		out << "\nSKSE plugins:\n";
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path(ec) / "Data" / "SKSE" / "Plugins", ec))
			if (entry.path().extension() == ".dll")
				out << "  " << entry.path().filename().string() << "\n";
		out.close();
		logger::info("[DCLF] compatibility monitor: report written to {}", path.string());
		return path.string();
	}

	void DrawPanel()
	{
		if (!ImGui::TreeNodeEx("Compatibility monitor", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		ImGui::TextWrapped(
			"Watches for changes other plugins make to the scene (node flags, fades, materials, alpha) that Drawcall Limit Fix has no hook for, "
			"and names each object's reference and plugin. Costs a little CPU while on; it does not change what is drawn.");
		bool on = Requested();
		if (ImGui::Checkbox("Watch for changes other plugins make", &on))
			Request(on);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Each frame compares a slice of the scene with what DCLF holds; a difference no hook explains is an offender. Turning it on starts a fresh list.");
		const auto offenders = Current();
		ImGui::Text("%llu checks, %zu offenders", static_cast<unsigned long long>(offenders->probes), offenders->entries.size());
		static std::string reportStatus;
		if (ImGui::Button("Write report"))
			reportStatus = WriteReport();
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
			Clear();
		if (!reportStatus.empty())
			ImGui::TextWrapped("Report: %s", reportStatus.c_str());
		constexpr std::size_t kRows = 200;
		if (!offenders->entries.empty() && ImGui::BeginTable("##dclfOffenders", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
											   ImVec2(0.0f, 300.0f))) {
			ImGui::TableSetupColumn("Plugin");
			ImGui::TableSetupColumn("Reference");
			ImGui::TableSetupColumn("Changed");
			ImGui::TableSetupColumn("Checks");
			ImGui::TableSetupColumn("Frames");
			ImGui::TableHeadersRow();
			// The names resolved once per form (the menu draws every frame).
			static std::map<std::uint32_t, Named> names;
			for (std::size_t i = 0; i < std::min(kRows, offenders->entries.size()); ++i) {
				const auto& offender = offenders->entries[i];
				auto found = names.find(offender.form);
				if (found == names.end())
					found = names.emplace(offender.form, NameOf(offender.form)).first;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(found->second.plugin.c_str());
				ImGui::TableNextColumn();
				if (offender.references > 1)
					ImGui::Text("%s (+%u sharing it)", found->second.reference.c_str(), offender.references - 1);
				else
					ImGui::TextUnformatted(found->second.reference.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%s %s", TypeName(offender.type), FieldNames(offender.type, offender.fields).c_str());
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(offender.example.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(offender.count));
				ImGui::TableNextColumn();
				ImGui::Text("%u-%u", offender.firstFrame, offender.lastFrame);
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
}
