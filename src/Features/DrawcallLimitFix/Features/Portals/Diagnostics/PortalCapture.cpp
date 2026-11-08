// CS_DCLF_PORTAL_CAPTURE=<directory>: every CS_DCLF_PORTAL_CAPTURE_EVERY-th depth epoch (default 300), each registered portal batch's
// queries as the GPU prepared them, with the frame's program encoding, into a corpus file for the portal bench (Portals/PortalCorpus.h,
// tests/DclfPortalBench). The queries are copied after the depth epoch into a staging buffer and written three epochs later.
//
// Each capture also checks the batch's verdicts against the CPU's PortalTest on the same words and queries ("<- PORTAL VERDICTS
// DIFFER"). With CS_DCLF_PORTAL_TRACE the inline batch writes each query's decision trace (PortalPrograms.hlsli: the step that decided
// it, the view test, hashes of the view planes and directory entry it loaded), and a disagreement logs the GPU's trace against the
// CPU's. The copies are on the immediate context, which the async compute queue can run frames behind: the trace names the frame
// whose programs the batch read, and the check uses that frame's words.

#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContext.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalCorpus.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"

#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <bit>
#include <deque>
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
			// The batch's results (uint2 per destination: generation, verdict), for the check against the CPU's PortalTest.
			winrt::com_ptr<ID3D11Buffer> results;
			UINT resultBytes = 0;
			// CS_DCLF_PORTAL_TRACE: the inline batch's decision traces (uint4 per query, PortalPrograms.hlsli's PortalTrace).
			winrt::com_ptr<ID3D11Buffer> trace;
			UINT traceBytes = 0;
		};

		/** @brief PortalTraceHash (FNV-1a over words). */
		std::uint32_t TraceHash(std::uint32_t a_hash, std::uint32_t a_word)
		{
			return (a_hash ^ a_word) * 16777619u;
		}
		/** @brief The trace's view-plane hash over the words at offset 0: the six planes, then the mask. */
		std::uint32_t PlanesHash(std::span<const std::uint32_t> a_words)
		{
			if (a_words.size() < 25)
				return 0;
			std::uint32_t hash = 2166136261u;
			for (std::size_t i = 0; i < 25; ++i)
				hash = TraceHash(hash, a_words[i]);
			return hash;
		}
		/** @brief The CPU's side of a query's trace: the same fields PortalTestAt records on the GPU (PortalPrograms.hlsli). */
		struct CpuTrace
		{
			std::uint32_t planes = 0, directory = 0, cullMode = 15, rejected = 15;
			float nearest = 3.4e38f;
		};
		CpuTrace TraceOf(std::span<const std::uint32_t> a_words, std::uint32_t a_program, const float a_c[3], float a_r)
		{
			CpuTrace t;
			if (a_words.size() < 32)
				return t;
			t.planes = PlanesHash(a_words);
			const std::uint32_t count = a_words[28];
			const std::size_t directory = 32 + std::size_t(a_program) * 8;
			if (a_program < count && directory + 8 <= a_words.size()) {
				std::uint32_t hash = TraceHash(2166136261u, count);
				for (std::size_t i = 0; i < 8; ++i)
					hash = TraceHash(hash, a_words[directory + i]);
				t.directory = hash;
				t.cullMode = std::min(a_words[directory + 6], 15u);
			}
			const auto f = [&](std::size_t i) { return std::bit_cast<float>(a_words[i]); };
			for (std::uint32_t p = 0; p < 6; ++p) {
				const float d = ((f(p * 4 + 1) * a_c[1] + f(p * 4) * a_c[0]) + a_c[2] * f(p * 4 + 2)) - f(p * 4 + 3);
				if ((a_words[24] & (1u << p)) && t.rejected == 15) {
					if (d <= -a_r) {
						t.rejected = p;
						t.nearest = d;
					} else
						t.nearest = std::min(t.nearest, d);
				}
			}
			return t;
		}
		const char* TraceStep(std::uint32_t a_step)
		{
			constexpr const char* names[]{ "none", "no program", "no radius", "unreached", "cull mode", "preprocessed", "view only", "view rejected", "operators" };
			return a_step < std::size(names) ? names[a_step] : "?";
		}
		struct Capture
		{
			std::filesystem::path directory;
			std::uint32_t every = 300, epochs = 0, framesLeft = 0, frame = 0, written = 0;
			std::vector<std::uint32_t> words;
			std::string metadata;
			std::vector<Pending> pending;
			// CS_DCLF_PORTAL_TRACE: the walk's words at each recent depth epoch (and their view-plane hash), by scene frame: the
			// frame whose planes a batch's dispatch read, when the async compute queue ran it behind this copy.
			struct Epoch
			{
				std::uint32_t frame = 0, planes = 0;
				std::vector<std::uint32_t> words;
			};
			std::deque<Epoch> history;
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
				// The GPU's verdicts against the CPU's on the same words and queries (this frame's generation only).
				D3D11_MAPPED_SUBRESOURCE resultsMapped{};
				if (p.results && SUCCEEDED(context->Map(p.results.get(), 0, D3D11_MAP_READ, 0, &resultsMapped))) {
					const auto* results = static_cast<const std::uint32_t*>(resultsMapped.pData);
					const std::size_t resultCount = p.resultBytes / 8;
					std::uint32_t checked = 0, differ = 0, stale = 0, samples = 0;
					std::uint32_t traced = 0, planesDiffer = 0, directoryDiffer = 0, differPlanes[2]{}, differSteps[16]{};
					D3D11_MAPPED_SUBRESOURCE traceMapped{};
					const std::uint32_t* traceWords = p.trace && SUCCEEDED(context->Map(p.trace.get(), 0, D3D11_MAP_READ, 0, &traceMapped)) ?
					                                      static_cast<const std::uint32_t*>(traceMapped.pData) :
					                                      nullptr;
					// The buffer holds slots past the batch's live queries, which no dispatch reads any more (their results are left over).
					// With the trace, the batch's last dispatch stamped every query it evaluated with its frame (the first evaluated
					// query's stamp): a slot with another stamp was not evaluated, and is not checked.
					std::optional<std::uint32_t> lastStamp;
					std::uint32_t unevaluated = 0;
					for (std::size_t i = 0; traceWords && i < corpus.queries.size() && i < p.traceBytes / 16 && !lastStamp; ++i)
						if (!(corpus.queries[i].flags & PortalCorpus::kImmediate))
							lastStamp = traceWords[i * 4] >> 24;
					// The async compute queue can run frames behind the graphics queue, which this copy is ordered on: the batch's
					// buffers then hold an earlier frame's dispatch. With the trace, the words checked against are the frame's whose
					// view planes that dispatch read (the planes of its first view-tested query), when one of the recent epochs has them.
					std::span<const std::uint32_t> checkWords = corpus.words;
					std::optional<std::uint32_t> readFrame;
					for (std::size_t i = 0; lastStamp && i < corpus.queries.size() && i < p.traceBytes / 16; ++i) {
						const std::uint32_t* t = traceWords + i * 4;
						if ((corpus.queries[i].flags & PortalCorpus::kImmediate) || (t[0] >> 24) != *lastStamp || ((t[0] >> 8) & 15u) < 6u)
							continue;
						if (t[1] != PlanesHash(corpus.words))
							for (const auto& epoch : a_capture.history)
								if (epoch.planes == t[1]) {
									checkWords = epoch.words;
									readFrame = epoch.frame;
								}
						break;
					}
					for (const auto& q : corpus.queries) {
						if (q.flags & PortalCorpus::kImmediate)
							continue;
						if (q.destination >= resultCount || results[q.destination * 2] != q.generation) {
							++stale;
							continue;
						}
						if (const std::size_t i = static_cast<std::size_t>(&q - corpus.queries.data());
							lastStamp && (i >= p.traceBytes / 16 || traceWords[i * 4] >> 24 != *lastStamp)) {
							++unevaluated;
							continue;
						}
						const auto gpu = static_cast<std::int32_t>(results[q.destination * 2 + 1]);
						const float centre[3]{ q.bound[0], q.bound[1], q.bound[2] };
						std::string why;
						const int cpu = PortalTest(checkWords, q.program, centre, q.bound[3], q.flags, why);
						++checked;
						// The query's trace (its index in the queries buffer), against the CPU's same fields.
						const std::size_t index = static_cast<std::size_t>(&q - corpus.queries.data());
						const std::uint32_t* trace = traceWords && index < p.traceBytes / 16 ? traceWords + index * 4 : nullptr;
						CpuTrace expected;
						if (trace) {
							expected = TraceOf(checkWords, q.program, centre, q.bound[3]);
							// The planes only where the GPU ran the view test (step 'view only' or later).
							planesDiffer += ((trace[0] >> 8) & 15u) >= 6u && trace[1] != expected.planes;
							directoryDiffer += ((trace[0] >> 8) & 15u) > 1u && trace[2] != expected.directory;
							++traced;
						}
						if (gpu != cpu) {
							++differ;
							if (trace) {
								++differPlanes[trace[1] == expected.planes ? 0 : 1];
								++differSteps[(trace[0] >> 8) & 15u];
							}
							if (samples++ < 8) {
								logger::info("[DCLF] portal capture {}: GPU {} CPU {} - destination {}, program {}, bound ({:.0f} {:.0f} {:.0f}) r {:.0f}, flags {:#x}; {}",
									corpus.name, gpu, cpu, q.destination, q.program, q.bound[0], q.bound[1], q.bound[2], q.bound[3], q.flags, why);
								if (trace) {
									std::string from;
									if (trace[1] != expected.planes) {
										from = " - the planes of no recent epoch";
										for (const auto& epoch : a_capture.history)
											if (epoch.planes == trace[1])
												from = fmt::format(" - the planes of scene frame {} (capture frame {})", epoch.frame, a_capture.frame);
									}
									logger::info("[DCLF] portal capture {}:   trace: GPU frame stamp {} (the capture's {}), step '{}', cull mode {}, view {} (plane {}, distance {:.2f}), planes {:08x}, directory {:08x}; "
												 "CPU cull mode {}, view {} (plane {}, distance {:.2f}), planes {:08x}{}, directory {:08x}{}{}",
										corpus.name, trace[0] >> 24, a_capture.frame & 0xFFu, TraceStep((trace[0] >> 8) & 15u), (trace[0] >> 12) & 15u, (trace[0] >> 16) & 1u ? "in" : "out",
										(trace[0] >> 20) & 15u, std::bit_cast<float>(trace[3]), trace[1], trace[2], expected.cullMode,
										expected.rejected == 15 ? "in" : "out", expected.rejected, expected.nearest, expected.planes,
										trace[1] == expected.planes ? " (same)" : " (DIFFERENT)", expected.directory,
										trace[2] == expected.directory ? " (same)" : " (DIFFERENT)", from);
								}
							}
						}
					}
					context->Unmap(p.results.get(), 0);
					if (traceWords) {
						context->Unmap(p.trace.get(), 0);
						std::string steps;
						for (std::uint32_t i = 0; i < std::size(differSteps); ++i)
							if (differSteps[i])
								steps += fmt::format(" '{}' {}", TraceStep(i), differSteps[i]);
						logger::info("[DCLF] portal capture {}: traces of {} queries: planes read differently in {}, directory entries in {}; of the {} that differ, "
									 "{} read the CPU's planes, {} others; by step:{}{}",
							corpus.name, traced, planesDiffer, directoryDiffer, differ, differPlanes[0], differPlanes[1], steps,
							planesDiffer || directoryDiffer ? " <- THE GPU READ OTHER WORDS" : "");
					}
					logger::info("[DCLF] portal capture {}: {} queries checked against the CPU{}, {} differ, {} without this frame's result{}{}", corpus.name,
						checked, readFrame ? fmt::format(" on scene frame {}'s programs (its batch's last dispatch read them)", *readFrame) : std::string(), differ, stale,
						lastStamp ? fmt::format(", {} not evaluated by the last dispatch", unevaluated) : std::string(), differ ? " <- PORTAL VERDICTS DIFFER" : " <- OK");
				}
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
			// A staging copy of a batch buffer (its whole elements of a_stride bytes), on the immediate context; its size, 0 if none.
			const auto stage = [](const std::shared_ptr<org::Buffer>& a_buffer, UINT a_stride, winrt::com_ptr<ID3D11Buffer>& a_staging) -> UINT {
				const UINT bytes = a_buffer ? static_cast<UINT>(a_buffer->GetSize() / a_stride * a_stride) : 0u;
				if (!bytes)
					return 0;
				D3D11_BUFFER_DESC sourceDesc{};
				sourceDesc.ByteWidth = bytes;
				sourceDesc.Usage = D3D11_USAGE_DEFAULT;
				sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				sourceDesc.StructureByteStride = a_stride;
				const auto source = RenderGraphRuntime::Get().WrapBuffer(*a_buffer, sourceDesc);
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = bytes;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (!source || FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, a_staging.put())))
					return 0;
				globals::d3d::context->CopyResource(a_staging.get(), source.get());
				return bytes;
			};
			for (const auto& registered : Batches()) {
				const Batch* batch = registered.batchOf(a_scene);
				if (!batch)
					continue;
				Pending p;
				p.label = registered.label;
				p.bytes = stage(Draws::Target(batch->queries), sizeof(PortalCorpus::Query), p.staging);
				if (!p.bytes)
					continue;
				p.resultBytes = stage(Draws::Target(batch->results), sizeof(DCLF::Portals::Result), p.results);
				if (batch->trace)
					p.traceBytes = stage(Draws::Target(batch->tasks), 16, p.trace);
				a_capture.pending.push_back(std::move(p));
			}
			a_capture.framesLeft = a_capture.pending.empty() ? 0 : 3;
		}

		void OnDepthEpoch(const Diagnostics::MainEpochDone& a_epoch)
		{
			if (!a_epoch.depthOnly || !capture || !a_epoch.resources || !a_epoch.resources->scene)
				return;
			auto& c = *capture;
			static const bool trace = RegisteredSwitchValue("CS_DCLF_PORTAL_TRACE") == "1";
			if (trace) {
				const auto& words = PortalViews::Get().Encoded();
				c.history.push_back({ Scene::FrameView::Get().GetFrame(), PlanesHash(words), words });
				if (c.history.size() > 16)
					c.history.pop_front();
			}
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
