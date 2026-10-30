// CS_DCLF_SCHEDULE_DUMP=1: the epochs' pass lists as gathered (ScheduleDump.h). One line per epoch whose list changed:
//   [DCLF schedule] epoch <n>: <name>/<type>/<queue> | ...
// and its passes' C++ types (ORG names a pass's recipe by them) on a second line, which the diff only warns about.

#include "Features/DrawcallLimitFix/Diagnostics/ScheduleDump.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include <map>
#include <string>
#include <typeinfo>
#include <vector>

namespace DCLF::Diagnostics
{
	namespace
	{
		// The last lines logged per epoch. Graph builds gather on the graph host's thread, one at a time.
		std::map<std::uint32_t, std::pair<std::string, std::string>> logged;

		std::string QueueOf(const org::RenderGraph::ExternalPassDesc& a_desc)
		{
			return a_desc.preferredQueueKind ? std::to_string(static_cast<int>(*a_desc.preferredQueueKind)) : std::string("-");
		}

		std::string TypeOf(const org::RenderGraph::ExternalPassDesc& a_desc)
		{
			if (!a_desc.pass)
				return "-";
			const auto& pass = *a_desc.pass;
			return typeid(pass).name();
		}

		struct Dumped final : org::RenderGraph::IRenderGraphExtension
		{
			std::string id;
			std::unique_ptr<org::RenderGraph::IRenderGraphExtension> inner;

			void PrepareForBuild(org::RenderGraph& a_graph) override { inner->PrepareForBuild(a_graph); }
			void Initialize(org::RenderGraph& a_graph) override { inner->Initialize(a_graph); }
			void Shutdown(org::RenderGraph& a_graph) override { inner->Shutdown(a_graph); }
			void OnRegistryReset(org::ResourceRegistry* a_registry) override { inner->OnRegistryReset(a_registry); }
			void GatherFramePasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override { inner->GatherFramePasses(a_graph, a_out); }

			void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				const std::size_t first = a_out.size();
				inner->GatherStructuralPasses(a_graph, a_out);
				std::map<std::uint32_t, std::pair<std::string, std::string>> lines;
				for (std::size_t i = first; i < a_out.size(); ++i) {
					const auto& desc = a_out[i];
					auto& [passes, types] = lines[desc.epoch];
					if (!passes.empty()) {
						passes += " | ";
						types += " | ";
					}
					passes += fmt::format("{}/{}/{}", desc.name, static_cast<int>(desc.type), QueueOf(desc));
					types += TypeOf(desc);
				}
				for (auto& [epoch, line] : lines) {
					auto& last = logged[epoch];
					if (last == line)
						continue;
					logger::info("[DCLF schedule] epoch {}: {}", epoch, line.first);
					logger::info("[DCLF schedule types] epoch {}: {}", epoch, line.second);
					last = std::move(line);
				}
			}
		};
	}

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> ScheduleDumped(std::string_view a_extensionId,
		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> a_extension)
	{
		if (!a_extension || !SwitchEnabled(Switch::ScheduleDump))
			return a_extension;
		auto dumped = std::make_unique<Dumped>();
		dumped->id = a_extensionId;
		dumped->inner = std::move(a_extension);
		return dumped;
	}
}
