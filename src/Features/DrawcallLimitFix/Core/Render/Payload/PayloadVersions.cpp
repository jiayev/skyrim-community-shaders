// The scene payload's parts' versions (PayloadPools, D5): per part a backing pool and a chain of versions built from each other on DCLF's
// state graph, the snapshot's payload naming them, and the frame's payload read from the installed one (PayloadVersions.h).
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/VersionedBufferBuild.h>
#include <Render/Runtime/IUploadService.h>

namespace DCLF::Draws
{
	namespace
	{
		using org::services::BufferByteRange;
		using PartVersion = org::services::PublishedBufferVersion;

		constexpr std::size_t Part(PayloadPart a_part) { return static_cast<std::size_t>(a_part); }

		template <class T>
		constexpr std::uint64_t Words(std::uint64_t a_elements) { return a_elements * sizeof(T) / 4; }

		// The parts' versions, in PayloadPart order: cs.dclf.payload.<name>. Inputs and geometries are 4-byte words (the shaders read them
		// as such); the row parts report their device address (the latches name them by it).
		struct PartPool
		{
			const char* name;
			std::uint32_t stride;
			bool deviceAddress;
		};
		constexpr std::array<PartPool, kPayloadParts> kPools{ { { "objects", sizeof(BindlessObject), false }, { "extras", 16, false },
			{ "geometries", 4, false }, { "material-rows", kMaterialRowBytes, true }, { "pipeline-rows", kPipelineRowBytes, true }, { "inputs", 4, false },
			{ "shadow-rows", sizeof(ShadowMaterialRow), true } } };

		// A class's backings at most: one a game frame in flight may still bind, the newest version and the one being built. A build that
		// finds none free suspends until one is (the frames meanwhile draw the snapshot they have).
		constexpr std::uint32_t kBackingsPerClass = RenderGraphRuntime::kGameFramesInFlight + 2;

		constexpr std::uint64_t kNoTail = ~0ull;   // PartHeld::tailStart: the part has no tails
		constexpr std::uint64_t kNoFloor = ~0ull;  // PartShared::floorEpoch: no backing holds anything

		/** @brief A version's journal position and the first byte of its tails (kNoTail: none): what a backing that holds it lacks of a later one. */
		struct PartHeld
		{
			std::uint64_t version = 0, journalEpoch = 0, tailStart = kNoTail;
		};

		/** @brief Any thread: a part's journal floor (its producer's, after each build) and the report's counters. */
		struct PartShared
		{
			std::atomic<std::uint64_t> floorEpoch{ kNoFloor };  // the oldest journal position a backing of the pool holds
			std::atomic<std::uint64_t> floorVersion{ 0 };       // the same as a version (the builder forgets the older ones)
			std::atomic<std::uint64_t> built{ 0 }, uploadedBytes{ 0 }, copiedBytes{ 0 }, wholeCopies{ 0 }, suspensions{ 0 }, failures{ 0 };
		};

		/** @brief One posted version's outcome: the coordinator lane's (SceneGraph::PostPayloadPart's done), read by the builder once `done`. */
		struct PartPending
		{
			std::atomic<bool> done{ false };
			std::shared_ptr<const PartVersion> version;  // null: failed (error)
			std::string error;
			PartHeld held;
			std::uint64_t units = 0, serial = 0;  // the source's
		};

		/** @brief The snapshot builder's: one part's chain. */
		struct PartChain
		{
			std::shared_ptr<org::services::BackingPool> pool;  // made at the part's first post
			std::shared_ptr<const PartVersion> last;           // the newest version built
			PartHeld lastHeld;
			std::uint64_t lastUnits = 0, lastSerial = 0;       // the source last was built for
			std::uint64_t versions = 0;                        // the versions numbered (writeSequence)
			std::vector<PartHeld> held;                        // by version, back to the pool's floor (last's included)
			std::shared_ptr<PartPending> pending;              // the version in flight
			bool failed = false;                               // the current source's build of it failed
			bool reported = false;                             // a failure logged
		};

		struct PartsState
		{
			std::array<PartShared, kPayloadParts> shared;
			std::array<PartChain, kPayloadParts> chains;    // the snapshot builder's
			std::uint64_t sources = 0;                      // the snapshot builder's: MakeSource's serials
			bool uploaderReported = false;                  // the snapshot builder's
			std::shared_ptr<const ScenePayload> installed;  // render thread: the frame's (Install)
		};
		/** @brief Never destroyed: a version's lease may return to its pool, and a build's outcome arrive, while the process tears down. */
		PartsState& Parts()
		{
			static auto* state = new PartsState();
			return *state;
		}

		/** @brief A part's rows in its version's elements (words for Geometries and Inputs). */
		std::uint64_t RowsOf(const PayloadSource& a_source, PayloadPart a_part)
		{
			const auto units = a_source.units[Part(a_part)];
			switch (a_part) {
			case PayloadPart::Geometries:
				return Words<GeometryDraw>(units);
			case PayloadPart::Inputs:
				return Words<DrawInput>(units);
			default:
				return units;
			}
		}

		/** @brief Whether a_source has a_part (the row parts are the main and the shadow payloads'). */
		bool Present(const PayloadSource& a_source, PayloadPart a_part)
		{
			switch (a_part) {
			case PayloadPart::MaterialRows:
			case PayloadPart::PipelineRows:
				return a_source.main != nullptr;
			case PayloadPart::ShadowRows:
				return a_source.shadow != nullptr;
			default:
				return true;
			}
		}

		/** @brief Whether a_part has tails: rows after its journalled ones that are the source's own, written whole into each version. */
		constexpr bool Tailed(PayloadPart a_part) { return a_part == PayloadPart::Geometries || a_part == PayloadPart::Inputs; }

		/** @brief The first byte of a_part's tails in a_source (the stream rows, the frame inputs); kNoTail for an untailed part. */
		std::uint64_t TailStartOf(const PayloadSource& a_source, PayloadPart a_part)
		{
			switch (a_part) {
			case PayloadPart::Geometries:
				return a_source.geometries->SlotCount() * sizeof(GeometryDraw);
			case PayloadPart::Inputs:
				return a_source.regionCount * sizeof(DrawInput);
			default:
				return kNoTail;
			}
		}

		/** @brief A part's journal position in a_source (0: not a kept array's, written whole). */
		std::uint64_t ContentEpochOf(const PayloadSource& a_source, PayloadPart a_part)
		{
			switch (a_part) {
			case PayloadPart::Objects:
				return a_source.views->objects.ContentEpoch();
			case PayloadPart::Extras:
				return a_source.views->extras.ContentEpoch();
			case PayloadPart::Geometries:
				return a_source.geometries->ContentEpoch();
			case PayloadPart::MaterialRows:
				return a_source.main->body->materialRows.ContentEpoch();
			case PayloadPart::PipelineRows:
				return a_source.main->body->pipelineRows.ContentEpoch();
			case PayloadPart::Inputs:
				return a_source.region->ContentEpoch();
			case PayloadPart::ShadowRows:
				return a_source.shadow->materialRows.ContentEpoch();
			default:
				return 0;
			}
		}

		/** @brief A journal's runs since a_from as byte ranges (a_unitBytes an element of a_count); nullopt where it does not reach back. */
		std::optional<std::vector<BufferByteRange>> RunsOf(const ChangeJournal::Snapshot& a_changes, std::uint64_t a_unitBytes, std::uint64_t a_count,
			std::uint64_t a_from)
		{
			if (!a_changes.contentEpoch || !a_from || a_from < a_changes.floor || a_from > a_changes.contentEpoch)
				return std::nullopt;
			std::vector<BufferByteRange> out;
			a_changes.ForEachRun(a_from, a_count, [&](std::uint64_t a_first, std::uint64_t a_run) { out.push_back({ a_first * a_unitBytes, a_run * a_unitBytes }); });
			return out;
		}

		/**
		 * @brief The bytes a_part's journal in a_source names changed since journal position a_from (through a_source's, which covers
		 * through the predecessor's: a superset is copied, never too little); nullopt below its floor.
		 */
		std::optional<std::vector<BufferByteRange>> JournalRanges(const PayloadSource& a_source, PayloadPart a_part, std::uint64_t a_from)
		{
			switch (a_part) {
			case PayloadPart::Objects:
				return RunsOf(a_source.views->objects.changes, sizeof(BindlessObject), a_source.views->objects.Count(), a_from);
			case PayloadPart::Extras:
				return RunsOf(a_source.views->extras.changes, 16, a_source.views->extras.Rows(), a_from);
			case PayloadPart::Geometries:
				return RunsOf(a_source.geometries->slots.changes, sizeof(GeometryDraw), a_source.geometries->SlotCount(), a_from);
			case PayloadPart::MaterialRows: {
				// Journalled by part (EmitMainRowParts).
				const auto& rows = a_source.main->body->materialRows;
				return RunsOf(rows.changes, sizeof(MaterialRow) / kMaterialRowParts, rows.Count() * kMaterialRowParts, a_from);
			}
			case PayloadPart::PipelineRows:
				return RunsOf(a_source.main->body->pipelineRows.changes, sizeof(PipelineRow), a_source.main->body->pipelineRows.Count(), a_from);
			case PayloadPart::Inputs:
				return RunsOf(a_source.region->changes, sizeof(DrawInput), a_source.region->Count(), a_from);
			case PayloadPart::ShadowRows:
				return RunsOf(a_source.shadow->materialRows.changes, sizeof(ShadowMaterialRow), a_source.shadow->materialRows.Count(), a_from);
			default:
				return std::nullopt;
			}
		}

		/**
		 * @brief TEMP (CS_DCLF_SET_PARITY): version a_version of the inputs as built (its predecessor a_previous's mirror, the delta written
		 * over it), mirrored per version, against the whole scene list. A difference is an input the GPU reads that the list lacks. The
		 * inputs' producer's alone (its builds are serial).
		 */
		std::vector<DrawInput>& MirrorInputs(std::uint64_t a_version, std::uint64_t a_previous, std::size_t a_units)
		{
			static ankerl::unordered_dense::map<std::uint64_t, std::vector<DrawInput>> mirrors;
			std::vector<DrawInput> base;
			if (const auto it = mirrors.find(a_previous); a_previous && it != mirrors.end())
				base = it->second;
			// The predecessor's is kept (a failed build is made again from it), the others go.
			for (auto it = mirrors.begin(); it != mirrors.end();)
				it = it->first != a_previous ? mirrors.erase(it) : std::next(it);
			auto& mirror = mirrors[a_version] = std::move(base);
			mirror.resize(a_units, DrawInput{});
			return mirror;
		}

		void CheckInputs(const PayloadSource& a_source, const std::vector<DrawInput>& a_mirror, std::uint64_t a_version, std::optional<std::uint64_t> a_from)
		{
			static std::uint64_t checks = 0, differ = 0;
			static std::string first;
			const auto& region = *a_source.region;
			++checks;
			const auto expected = [&](std::size_t a_i) -> const DrawInput* {
				if (a_i < region.Count())
					return region.At(a_i);
				if (a_source.mainList && a_i >= a_source.mainInputsAt && a_i < a_source.mainInputsAt + a_source.mainList->size())
					return &(*a_source.mainList)[a_i - a_source.mainInputsAt];
				if (a_source.shadowList && a_i >= a_source.shadowInputsAt && a_i < a_source.shadowInputsAt + a_source.shadowList->size())
					return &(*a_source.shadowList)[a_i - a_source.shadowInputsAt];
				return nullptr;
			};
			for (std::size_t i = 0; i < a_mirror.size(); ++i) {
				const auto* want = expected(i);
				if (!want || std::memcmp(want, &a_mirror[i], sizeof(DrawInput)) == 0)
					continue;
				++differ;
				if (first.empty())
					first = fmt::format("version {}: input {} of {} (region {}, built from journal position {}): object {} built, {} listed", a_version, i, a_mirror.size(),
						region.Count(), a_from ? fmt::format("{}", *a_from) : std::string("none (whole)"), a_mirror[i].objectIndex, want->objectIndex);
				break;
			}
			if (checks >= 300) {
				logger::info("[DCLF] payload inputs versions (set parity): {} versions checked, {} differ{}{}", checks, differ, differ ? " <- INPUTS DIFFER; first: " : " <- OK", first);
				checks = differ = 0;
				first.clear();
			}
		}

		using DeltaWrite = std::function<void(const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)>;

		/**
		 * @brief A part's delta (VersionedBufferBuildInput::emitDelta, the producer's thread): its journal's runs since a_from, the
		 * predecessor's journal position (nullopt: everything, a version with no predecessor), then its tails whole. The data are a_source's,
		 * which the version's build holds (VersionedBufferBuildInput::keep).
		 */
		void EmitDelta(PayloadPart a_part, const PayloadSource& a_source, std::optional<std::uint64_t> a_from, std::uint64_t a_version, std::uint64_t a_previous,
			const DeltaWrite& a_write)
		{
			// ~0 is past every journal's position: a holder there is sent everything.
			const std::uint64_t from = a_from ? *a_from : ~std::uint64_t{ 0 };
			const auto sink = [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_write(a_data, a_bytes, a_offset); };
			switch (a_part) {
			case PayloadPart::Objects:
				a_source.views->objects.Emit(from, sink);
				break;
			case PayloadPart::Extras: {
				// CS_DCLF_PERSISTENT_PARITY: the extras rows are checked by the version that holds them, as it is built (the observer's
				// mirror of it, its predecessor's with the delta written over it; null without the observer).
				std::vector<float>* mirror = nullptr;
				Diagnostics::Notify(Diagnostics::ExtrasSending{ a_version, a_previous, mirror });
				EmitExtras(a_source.views->extras, from, mirror, sink);
				Diagnostics::Notify(Diagnostics::ExtrasSent{ a_version, a_source.views->extras, a_source.views->tables.get() });
				break;
			}
			case PayloadPart::Geometries: {
				const auto& geometries = *a_source.geometries;
				geometries.slots.Emit(from, sink);
				if (!geometries.streamRows.empty())
					sink(geometries.streamRows.data(), geometries.streamRows.size() * sizeof(GeometryDraw), geometries.SlotCount() * sizeof(GeometryDraw));
				break;
			}
			case PayloadPart::MaterialRows: {
				const auto& rows = a_source.main->body->materialRows;
				EmitMainRowParts(rows, kMaterialRowParts, from, rows.Count(), sink);
				break;
			}
			case PayloadPart::PipelineRows: {
				const auto& rows = a_source.main->body->pipelineRows;
				EmitMainRows(rows, from, rows.Count(), sink);
				break;
			}
			case PayloadPart::Inputs: {
				std::vector<DrawInput>* mirror =
					Diagnostics::Observe().claims ? &MirrorInputs(a_version, a_previous, static_cast<std::size_t>(a_source.units[Part(PayloadPart::Inputs)])) : nullptr;
				const auto put = [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					if (mirror && a_offset + a_bytes <= mirror->size() * sizeof(DrawInput))
						std::memcpy(reinterpret_cast<std::byte*>(mirror->data()) + a_offset, a_data, a_bytes);
					a_write(a_data, a_bytes, a_offset);
				};
				a_source.region->Emit(from, put);
				if (a_source.mainList && !a_source.mainList->empty())
					put(a_source.mainList->data(), a_source.mainList->size() * sizeof(DrawInput), a_source.mainInputsAt * sizeof(DrawInput));
				if (a_source.shadowList && !a_source.shadowList->empty())
					put(a_source.shadowList->data(), a_source.shadowList->size() * sizeof(DrawInput), a_source.shadowInputsAt * sizeof(DrawInput));
				if (mirror)
					CheckInputs(a_source, *mirror, a_version, a_from);
				break;
			}
			case PayloadPart::ShadowRows:
				a_source.shadow->materialRows.Emit(from, sink);
				break;
			default:
				break;
			}
		}

		/**
		 * @brief The producer's thread, after a build (the pool's one acquirer then: the part's builds are serial): the oldest journal
		 * position a backing of the pool holds, by a_held (the versions known when the build was posted, its own included), published for
		 * the journals. A backing at a version not known (one whose build failed after writing it) keeps every change.
		 */
		void PublishFloor(org::services::BackingPool& a_pool, const std::vector<PartHeld>& a_held, PartShared& a_shared)
		{
			const auto oldest = a_pool.OldestContentEpoch();
			if (!oldest) {
				a_shared.floorEpoch.store(kNoFloor, std::memory_order_release);
				return;
			}
			const auto it = std::ranges::find(a_held, *oldest, &PartHeld::version);
			a_shared.floorVersion.store(*oldest, std::memory_order_release);
			a_shared.floorEpoch.store(it != a_held.end() ? it->journalEpoch : 0, std::memory_order_release);
		}

		std::shared_ptr<org::services::BackingPool> MakePool(std::size_t a_part)
		{
			auto hooks = PooledBackings::Hooks();
			hooks.telemetry = [shared = &Parts().shared[a_part]](org::services::BackingPoolEvent a_event, std::string_view, std::uint64_t) {
				if (a_event == org::services::BackingPoolEvent::Suspended)
					shared->suspensions.fetch_add(1, std::memory_order_relaxed);
			};
			// One spare: a version builds into one free backing, and a fresh one costs a GPU copy of its predecessor, not an upload, so
			// more spares (FrameValues' PerFrameSpares, a backing a frame) would only hold VRAM (HexRim: 5 pipeline-row backings, 40 MB).
			return org::services::BackingPool::Create(std::move(hooks), FrameRetirement(),
				{ .name = fmt::format("cs.dclf.payload.{}", kPools[a_part].name), .maxPerClass = kBackingsPerClass,
					.framesInFlight = RenderGraphRuntime::kGameFramesInFlight, .spareFree = 1 });
		}
	}

	namespace PayloadPools
	{
		std::shared_ptr<const PayloadSource> MakeSource(std::shared_ptr<const void> a_keep, const StreamViews& a_views, const MainPayload* a_main,
			const ShadowPayload* a_shadow)
		{
			ZoneScopedN("CS.DCLF.Payload.Source");
			if (!a_main && !a_shadow)
				return nullptr;
			auto out = std::make_shared<PayloadSource>();
			out->keep = std::move(a_keep);
			out->views = &a_views;
			out->main = a_main;
			out->shadow = a_shadow;
			if (a_main) {
				out->products = a_main->body->products;
				Extensions::CarryPayloadProducts(out->products);
			}
			// The geometry table: the main payloads' and the shadow payload's are the same (the publication's stream views and face streams).
			out->geometries = a_main ? &a_main->body->geometryDraws : &a_shadow->geometries;
			// The scene list: the region every payload of the publication holds at its head, then the main payloads' frame inputs, then
			// the shadow payload's (both written whole into each version: they are the frame's).
			out->region = a_main ? &a_main->resident : &a_shadow->regionInputs;
			out->mainList = a_main ? &a_main->body->inputList : nullptr;
			out->shadowList = a_shadow ? &a_shadow->inputList : nullptr;
			out->regionCount = out->region->Count();
			out->mainInputsAt = out->regionCount;
			out->shadowInputsAt = out->mainInputsAt + (out->mainList ? out->mainList->size() : 0);
			auto& units = out->units;
			units[Part(PayloadPart::Objects)] = a_views.objects.Count();
			units[Part(PayloadPart::Extras)] = a_views.extras.Rows();
			units[Part(PayloadPart::Geometries)] = out->geometries->Count();
			units[Part(PayloadPart::MaterialRows)] = a_main ? a_main->body->materialRows.Count() : 0;
			units[Part(PayloadPart::PipelineRows)] = a_main ? a_main->body->pipelineRows.Count() : 0;
			units[Part(PayloadPart::Inputs)] = out->shadowInputsAt + (out->shadowList ? out->shadowList->size() : 0);
			units[Part(PayloadPart::ShadowRows)] = a_shadow ? a_shadow->materialRows.Count() : 0;
			out->serial = ++Parts().sources;
			return out;
		}

		bool Settle()
		{
			auto& s = Parts();
			bool idle = true;
			for (std::size_t k = 0; k < kPayloadParts; ++k) {
				auto& chain = s.chains[k];
				auto& shared = s.shared[k];
				if (chain.pending) {
					if (!chain.pending->done.load(std::memory_order_acquire)) {
						idle = false;
						continue;
					}
					const auto pending = std::move(chain.pending);
					if (const auto& version = pending->version) {
						chain.last = version;
						chain.lastHeld = pending->held;
						chain.lastUnits = pending->units;
						chain.lastSerial = pending->serial;
						chain.held.push_back(pending->held);
						shared.built.fetch_add(1, std::memory_order_relaxed);
						shared.uploadedBytes.fetch_add(version->uploadedBytes, std::memory_order_relaxed);
						shared.copiedBytes.fetch_add(version->copiedBytes, std::memory_order_relaxed);
						shared.wholeCopies.fetch_add(version->copiedWhole ? 1u : 0u, std::memory_order_relaxed);
					} else {
						// No fallback: the part keeps its last version, the source has no payload, and the next one builds it again.
						chain.failed = true;
						shared.failures.fetch_add(1, std::memory_order_relaxed);
						if (!std::exchange(chain.reported, true))
							logger::error("[DCLF] the scene payload's {} version {} failed: {}; its snapshot has no payload, the next publication builds it again",
								kPartNames[k], pending->held.version, pending->error);
					}
				}
				// The versions no backing holds any more (below the pool's floor) are forgotten; the newest is always known.
				const auto floor = s.shared[k].floorVersion.load(std::memory_order_acquire);
				std::erase_if(chain.held, [&](const PartHeld& a_held) { return a_held.version < floor && a_held.version != chain.lastHeld.version; });
			}
			return idle;
		}

		std::string PendingParts()
		{
			std::string out;
			for (std::size_t k = 0; k < kPayloadParts; ++k)
				if (const auto pending = Parts().chains[k].pending)
					out += fmt::format("{}{} v{}{}", out.empty() ? "" : ", ", kPartNames[k], pending->held.version, pending->done.load(std::memory_order_acquire) ? " (done)" : "");
			std::uint64_t suspensions = 0;
			for (const auto& shared : Parts().shared)
				suspensions += shared.suspensions.load(std::memory_order_relaxed);
			return fmt::format("{}; {} suspensions since the report", out.empty() ? std::string("none") : out, suspensions);
		}

		bool Post(const std::shared_ptr<const PayloadSource>& a_source, const std::shared_ptr<org::runtime::IUploadService>& a_uploads)
		{
			auto& s = Parts();
			// The builds copy from the predecessor on the dedicated upload queue (startup frames precede it: made by the graph's first build).
			if (!a_uploads || !a_uploads->HasDedicatedStreamingQueue()) {
				if (!std::exchange(s.uploaderReported, true))
					logger::info("[DCLF] the scene payload's versions wait for the dedicated upload queue: snapshots have no payload until it is made");
				return false;
			}
			const auto& source = *a_source;
			bool posted = true;
			for (std::size_t k = 0; k < kPayloadParts; ++k) {
				const auto part = static_cast<PayloadPart>(k);
				auto& chain = s.chains[k];
				chain.failed = false;
				if (!Present(source, part))
					continue;  // the chain keeps its last version for a later source that has the part
				const auto epoch = ContentEpochOf(source, part);
				// Changed for this source: its journal moved, its rows, or (a tailed part, or one no journal keeps) the source is another.
				const bool changed = !chain.last || source.units[k] != chain.lastUnits || epoch != chain.lastHeld.journalEpoch ||
				                     ((Tailed(part) || !epoch) && source.serial != chain.lastSerial);
				if (!changed)
					continue;
				if (!chain.pool)
					chain.pool = MakePool(k);
				const PartHeld held{ ++chain.versions, epoch, TailStartOf(source, part) };
				const PartHeld previous = chain.last ? chain.lastHeld : PartHeld{};
				// The versions a backing may hold, this one's included (the producer's floor reads them; the catch-up looks them up).
				auto known = std::make_shared<std::vector<PartHeld>>(chain.held);
				known->push_back(held);
				const std::shared_ptr<const std::vector<PartHeld>> versions = std::move(known);
				const PayloadSource* view = a_source.get();  // held by the input (keep)

				org::services::VersionedBufferBuildInput input;
				input.uploadOwner = a_uploads;
				input.uploadService = a_uploads.get();
				input.debugName = fmt::format("cs.dclf.payload.{}", kPools[k].name);
				input.writeSequence = held.version;
				input.elementStride = kPools[k].stride;
				input.elementCount = RowsOf(source, part);
				input.capacity = std::max<std::uint64_t>(input.elementCount, 1);
				// A grown class: a quarter's headroom, not the build's power of two (a part that changes every frame keeps a backing a frame
				// in flight: HexRim's pipeline rows were 4 of 16 MB for 9.5 MB of rows).
				input.growth = org::services::GrowthPolicy::Headroom(25, 64);
				input.flags.deviceAddress = kPools[k].deviceAddress;
				input.previous = chain.last;
				input.backingPool = chain.pool;
				input.predecessorCopy = true;
				input.keep = a_source;
				// What a backing at version a_from lacks of the predecessor: the journal's runs since its position, and from the earlier of
				// its tails' start and the predecessor's on (both sources' own).
				input.changedSince = [view, part, versions, previousTail = previous.tailStart](std::uint64_t a_from) -> std::optional<std::vector<BufferByteRange>> {
					const auto it = std::ranges::find(*versions, a_from, &PartHeld::version);
					if (it == versions->end())
						return std::nullopt;
					auto ranges = JournalRanges(*view, part, it->journalEpoch);
					if (ranges && Tailed(part))
						if (const auto tail = std::min(it->tailStart, previousTail); tail != kNoTail)
							ranges->push_back({ tail, ~std::uint64_t{ 0 } - tail });
					return ranges;
				};
				// The delta: the runs since the predecessor's journal position (everything without one, or from one no journal kept).
				const std::optional<std::uint64_t> from = chain.last && previous.journalEpoch ? std::optional<std::uint64_t>(previous.journalEpoch) : std::nullopt;
				input.emitDelta = [view, part, from, version = held.version, previousVersion = previous.version](const DeltaWrite& a_write) {
					EmitDelta(part, *view, from, version, previousVersion, a_write);
				};
				auto tag = std::make_shared<Runtime::PayloadPartTag>();
				tag->built = [pool = chain.pool.get(), versions, shared = &s.shared[k]](const PartVersion&) { PublishFloor(*pool, *versions, *shared); };
				input.tag = std::move(tag);

				auto pending = std::make_shared<PartPending>();
				pending->held = held;
				pending->units = source.units[k];
				pending->serial = source.serial;
				// The coordinator lane's, once GPU ready or ended: the builder takes it at its next pass (Settle).
				const bool accepted = SceneScheduler::Graph().PostPayloadPart(std::move(input), [pending](std::shared_ptr<const PartVersion> a_version, std::string a_error) {
					pending->version = std::move(a_version);
					pending->error = std::move(a_error);
					pending->done.store(true, std::memory_order_release);
					WakeSnapshotBuilder();
				});
				if (!accepted) {
					chain.failed = true;
					posted = false;
					continue;
				}
				chain.pending = std::move(pending);
			}
			return posted;
		}

		std::shared_ptr<const ScenePayload> Finish(const PayloadSource& a_source)
		{
			auto& s = Parts();
			auto out = std::make_shared<ScenePayload>();
			for (std::size_t k = 0; k < kPayloadParts; ++k) {
				if (!Present(a_source, static_cast<PayloadPart>(k)))
					continue;
				const auto& chain = s.chains[k];
				if (chain.failed || !chain.last || chain.lastUnits != a_source.units[k])
					return nullptr;
				out->versions[k] = chain.last;
			}
			out->units = a_source.units;
			out->regionCount = a_source.regionCount;
			out->mainInputsAt = a_source.mainInputsAt;
			out->shadowInputsAt = a_source.shadowInputsAt;
			out->main = a_source.main != nullptr;
			out->shadow = a_source.shadow != nullptr;
			out->products = a_source.products;
			out->serial = a_source.serial;
			out->mainBody = a_source.main ? a_source.main->body.get() : nullptr;
			return out;
		}

		void Install(std::shared_ptr<const ScenePayload> a_payload)
		{
			Parts().installed = std::move(a_payload);
		}

		bool MakeFramePayload(PayloadVersions& a_out)
		{
			a_out = {};
			const auto& installed = Parts().installed;
			if (!installed)
				return false;
			const auto& payload = *installed;
			for (std::size_t k = 0; k < kPayloadParts; ++k) {
				const auto& version = payload.versions[k];
				if (!version)
					continue;
				const auto& backing = version->lease;
				a_out.parts[k] = { backing->srvIndex, kPools[k].deviceAddress ? backing->address : 0, payload.units[k] };
				a_out.backings[k] = backing.Get();
				if (!a_out.parts[k].srvIndex)
					return false;
			}
			a_out.regionCount = payload.regionCount;
			a_out.mainInputsAt = payload.mainInputsAt;
			a_out.shadowInputsAt = payload.shadowInputsAt;
			a_out.main = payload.main;
			a_out.shadow = payload.shadow;
			a_out.serial = payload.serial;
			a_out.mainBody = payload.mainBody;
			return true;
		}

		void BindInstalled(std::uint64_t a_point)
		{
			if (const auto& installed = Parts().installed; installed && a_point)
				for (const auto& version : installed->versions)
					if (version)
						version->lease.Bind(a_point);
		}

		bool Available()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			auto uploads = host ? host->RetainUploads() : nullptr;
			return uploads && uploads->HasDedicatedStreamingQueue();
		}

		std::optional<std::uint64_t> OldestContentEpoch(PayloadPart a_part)
		{
			const auto floor = Parts().shared[Part(a_part)].floorEpoch.load(std::memory_order_acquire);
			return floor == kNoFloor ? std::nullopt : std::optional<std::uint64_t>(floor);
		}

		std::array<PartReport, kPayloadParts> TakeReport()
		{
			auto& s = Parts();
			std::array<PartReport, kPayloadParts> out{};
			for (std::size_t k = 0; k < kPayloadParts; ++k) {
				auto& shared = s.shared[k];
				auto& r = out[k];
				r.built = shared.built.exchange(0, std::memory_order_relaxed);
				r.uploadedBytes = shared.uploadedBytes.exchange(0, std::memory_order_relaxed);
				r.copiedBytes = shared.copiedBytes.exchange(0, std::memory_order_relaxed);
				r.wholeCopies = shared.wholeCopies.exchange(0, std::memory_order_relaxed);
				r.suspensions = shared.suspensions.exchange(0, std::memory_order_relaxed);
				r.failures = shared.failures.exchange(0, std::memory_order_relaxed);
				if (const auto& pool = s.chains[k].pool) {
					const auto census = pool->Count();
					r.liveBackings = census.live;
					r.liveBytes = census.bytes;
				}
				r.floor = OldestContentEpoch(static_cast<PayloadPart>(k));
			}
			return out;
		}
	}
}
