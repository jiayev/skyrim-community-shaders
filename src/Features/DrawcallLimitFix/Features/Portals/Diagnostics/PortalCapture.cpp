// CS_DCLF_PORTAL_CAPTURE=<directory>: every CS_DCLF_PORTAL_CAPTURE_EVERY-th depth epoch (default 300), each registered portal batch's
// queries as the GPU prepared them, with the frame's program encoding, into a corpus file for the portal bench (Portals/PortalCorpus.h,
// tests/DclfPortalBench). The queries are copied after the depth epoch into a staging buffer and written three epochs later.

#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalCorpus.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"

#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <filesystem>

namespace DCLF::Features::Portals
{
	namespace
	{
		namespace PortalCorpus = DCLF::Portals::Corpus;

		struct Registered
		{
			std::string label;
			BatchOf batchOf = nullptr;
		};
		struct Pending
		{
			std::string label;
			winrt::com_ptr<ID3D11Buffer> staging;
			UINT bytes = 0;
		};
		struct Capture
		{
			std::filesystem::path directory;
			std::uint32_t every = 300, epochs = 0, framesLeft = 0, frame = 0, written = 0;
			std::vector<std::uint32_t> words;
			std::string metadata;
			std::vector<Pending> pending;
		};

		std::vector<Registered>& Batches()
		{
			static std::vector<Registered> batches;  // install
			return batches;
		}
		std::optional<Capture> capture;  // render thread

		std::string Location()
		{
			const auto* player = RE::PlayerCharacter::GetSingleton();
			const auto* cell = player ? player->GetParentCell() : nullptr;
			const char* name = cell ? cell->GetName() : nullptr;
			const auto position = player ? player->GetPosition() : RE::NiPoint3{};
			return fmt::format("cell {:08X} '{}' ({}), player at ({:.0f}, {:.0f}, {:.0f})", cell ? cell->GetFormID() : 0u, name ? name : "",
				cell && cell->IsInteriorCell() ? "interior" : "exterior", position.x, position.y, position.z);
		}

		void Write(Capture& a_capture)
		{
			auto* context = globals::d3d::context;
			for (auto& p : a_capture.pending) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (!p.staging || FAILED(context->Map(p.staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
					continue;
				PortalCorpus::Corpus corpus;
				corpus.name = fmt::format("{}-{}", p.label, a_capture.frame);
				corpus.metadata = fmt::format("game capture: {} batch, frame {}, {}", p.label, a_capture.frame, a_capture.metadata);
				corpus.words = a_capture.words;
				const auto* queries = static_cast<const PortalCorpus::Query*>(mapped.pData);
				corpus.queries.assign(queries, queries + p.bytes / sizeof(PortalCorpus::Query));
				context->Unmap(p.staging.get(), 0);
				const auto path = a_capture.directory / (corpus.name + ".dclfportal");
				try {
					PortalCorpus::Save(corpus, path.string());
					++a_capture.written;
					logger::info("[DCLF] portal capture: {} ({} queries, {} words) to {}", corpus.name, corpus.queries.size(), corpus.words.size(), path.string());
				} catch (const std::exception& e) {
					logger::warn("[DCLF] portal capture: {}", e.what());
				}
			}
			a_capture.pending.clear();
		}

		void Kick(Capture& a_capture, const Draws::SceneBuffers& a_scene)
		{
			a_capture.words = PortalViews::Get().Encoded();
			a_capture.metadata = Location();
			a_capture.frame = Scene::FrameView::Get().GetFrame();
			for (const auto& registered : Batches()) {
				const Batch* batch = registered.batchOf(a_scene);
				const auto buffer = batch ? Draws::Target(batch->queries) : nullptr;
				if (!buffer)
					continue;
				Pending p;
				p.label = registered.label;
				p.bytes = static_cast<UINT>(buffer->GetSize() / sizeof(PortalCorpus::Query) * sizeof(PortalCorpus::Query));
				if (!p.bytes)
					continue;
				D3D11_BUFFER_DESC sourceDesc{};
				sourceDesc.ByteWidth = p.bytes;
				sourceDesc.Usage = D3D11_USAGE_DEFAULT;
				sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				sourceDesc.StructureByteStride = sizeof(PortalCorpus::Query);
				const auto source = RenderGraphRuntime::Get().WrapBuffer(*buffer, sourceDesc);
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = p.bytes;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (!source || FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, p.staging.put())))
					continue;
				globals::d3d::context->CopyResource(p.staging.get(), source.get());
				a_capture.pending.push_back(std::move(p));
			}
			a_capture.framesLeft = a_capture.pending.empty() ? 0 : 3;
		}

		void OnDepthEpoch(const Diagnostics::MainEpochDone& a_epoch)
		{
			if (!a_epoch.depthOnly || !capture || !a_epoch.resources || !a_epoch.resources->scene)
				return;
			auto& c = *capture;
			if (c.framesLeft) {
				if (!--c.framesLeft)
					Write(c);
				return;
			}
			if (++c.epochs % c.every == 0)
				Kick(c, *a_epoch.resources->scene);
		}
	}

	void RegisterBatchCapture(std::string a_label, BatchOf a_batchOf)
	{
		Batches().push_back({ std::move(a_label), a_batchOf });
	}

	void InstallPortalCapture(Api::InstallContext& a_context)
	{
		a_context.RegisterSwitch({ .name = "CS_DCLF_PORTAL_CAPTURE", .kind = Api::SwitchKind::Diagnostic,
			.description = "Directory to write portal corpora (programs and batch queries) to, for tests/DclfPortalBench" });
		a_context.RegisterSwitch({ .name = "CS_DCLF_PORTAL_CAPTURE_EVERY", .kind = Api::SwitchKind::Diagnostic,
			.description = "Depth epochs between portal captures (default 300)" });
		const auto directory = RegisteredSwitchValue("CS_DCLF_PORTAL_CAPTURE");
		if (directory.empty() || directory == "0")
			return;
		Capture c;
		c.directory = directory;
		std::error_code error;
		std::filesystem::create_directories(c.directory, error);
		if (const auto every = RegisteredSwitchValue("CS_DCLF_PORTAL_CAPTURE_EVERY"); !every.empty()) {
			try {
				c.every = static_cast<std::uint32_t>(std::max(1ul, std::stoul(every)));
			} catch (const std::exception&) {
				logger::warn("[DCLF] portal capture: CS_DCLF_PORTAL_CAPTURE_EVERY '{}' is not a number; every {} epochs", every, c.every);
			}
		}
		logger::info("[DCLF] portal capture: every {} depth epochs into {}", c.every, c.directory.string());
		capture = std::move(c);
		Diagnostics::Subscribe<Diagnostics::MainEpochDone>(&OnDepthEpoch);
	}
}
