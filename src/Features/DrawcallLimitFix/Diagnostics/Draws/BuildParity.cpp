// CS_DCLF_BUILD_PARITY: every 300th colour epoch, BuildDraws' output (the sequences, the decal slots, the local shadow lights in the object
// words) copied to staging and compared three main epochs later with the CPU's templates. The templates' row addresses are offsets into
// the row tables; the epoch's payload (PayloadVersions) places them, recorded when the check is armed.

#include "DrawsState.h"
#include "DrawnLocalShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		// The newest readback's local shadow lights by object (DrawnLocalShadowsOf).
		ankerl::unordered_dense::map<std::uint32_t, DrawnLocalShadows> drawnLocalShadows;
		/**
		 * @brief The colour draws BuildDraws wrote: each state class's main and terrain sub-ranges (PassFrame::classes), and the class plan's
		 * main and terrain plain buckets, as far as each count reaches (its classCounts word; past the capacity: dropped,
		 * kCountClassOverflowWord). Not the forward ones (sorted there).
		 */
		std::vector<DrawSequence> ClassSequences(const DrawsState::ParityReadback& a_parity, const DrawSequence* a_gpu, const std::uint32_t* a_classCounts)
		{
			std::vector<DrawSequence> built;
			for (std::uint32_t c = 0; c < a_parity.classes.size(); ++c)
				for (const std::uint32_t r : { Draws::kClassRangeMain, Draws::ClassRangeOfSlot(kDecalGroups), Draws::ClassRangeOfSlot(kDecalGroups + 1) }) {
					const auto& range = a_parity.classes[c].ranges[r];
					const std::uint32_t drawn = std::min(a_classCounts[Draws::ClassCountWord(c, r)], range.capacity);
					built.insert(built.end(), a_gpu + range.first, a_gpu + range.first + drawn);
				}
			if (const auto& plan = a_parity.classPlan) {
				const auto take = [&](const SetClassPlan::PlainBucket& a_bucket, std::uint32_t a_word) {
					const std::uint32_t drawn = std::min(a_classCounts[a_word], a_bucket.capacity);
					built.insert(built.end(), a_gpu + a_bucket.first, a_gpu + a_bucket.first + drawn);
				};
				for (std::size_t b = 0; b < plan->main.size(); ++b)
					take(plan->main[b], plan->MainCountWord(b));
				for (std::size_t b = 0; b < plan->terrain.size(); ++b)
					take(plan->terrain[b], plan->TerrainCountWord(b));
			}
			return built;
		}

		/**
		 * @brief A decal slot as BuildDraws wrote it: the sub-range of its group that draws it (its own class's: every other class's holds a
		 * zero-count filler there), null when none does (culled, or a blank).
		 */
		const DrawSequence* DrawnDecal(const DrawsState::ParityReadback& a_parity, const DrawSequence* a_gpu, std::uint32_t a_group, std::size_t a_slot)
		{
			for (const auto& entry : a_parity.classes) {
				const auto& range = entry.ranges[Draws::ClassRangeOfSlot(a_group)];
				if (a_slot < range.capacity && a_gpu[range.first + a_slot].indexCount != 0)
					return &a_gpu[range.first + a_slot];
			}
			return nullptr;
		}

		/** @brief A template's pipeline index (its set index) as BuildDraws stores it: its index in its state class (IndirectState::classOf). */
		void ClassIndexOf(DrawSequence& a_template, const std::vector<std::uint32_t>& a_classOf)
		{
			if (a_template.pipelineIndex < a_classOf.size() && a_classOf[a_template.pipelineIndex] != kNoSetClass)
				a_template.pipelineIndex = a_classOf[a_template.pipelineIndex] & kSetClassIndexMask;
		}
	}

	void CheckBuildParity(const MainEpochDone& a_epoch)
	{
		const auto& a_resources = a_epoch.resources;
		const MainPayload& a_payload = a_epoch.payload;
		const auto& shadowVolumes = DrawsView::Get().shadowVolumes;
		auto& state = DrawsState::Get();
		auto& parity = state.parity;
		auto& parityEpochs = state.parityEpochs;
		using ParityReadback = DrawsState::ParityReadback;
		const auto& sequences = a_payload.body->sequences;
		const auto& decalTemplates = a_payload.body->decalTemplates;
		const auto& inputs = a_payload.body->inputList;
		auto* context = globals::d3d::context;
		// Every main epoch's payload, while the check is on (by the colour epochs counted): where a row outside its epoch's tables came from.
		if (const auto* seen = a_epoch.payloadVersions; seen && seen->main) {
			auto& history = state.parityPayloads;
			const auto newest = state.parityPayloadsSeen ? &history[(state.parityPayloadsSeen - 1) % history.size()] : nullptr;
			if (!newest || newest->serial != seen->serial) {
				const auto& material = (*seen)[Draws::PayloadPart::MaterialRows];
				const auto& pipeline = (*seen)[Draws::PayloadPart::PipelineRows];
				history[state.parityPayloadsSeen++ % history.size()] = { seen->serial, parityEpochs, material.address, material.units * kMaterialRowBytes,
					pipeline.address, pipeline.units * kPipelineRowBytes };
			}
		}
		if (parity) {
			if (--parity->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE countMap{}, sequencesMap{}, classCountsMap{};
			if (FAILED(context->Map(parity->count.get(), 0, D3D11_MAP_READ, 0, &countMap)) ||
				FAILED(context->Map(parity->sequences.get(), 0, D3D11_MAP_READ, 0, &sequencesMap))) {
				parity.reset();
				return;
			}
			if (FAILED(context->Map(parity->classCounts.get(), 0, D3D11_MAP_READ, 0, &classCountsMap))) {
				context->Unmap(parity->count.get(), 0);
				context->Unmap(parity->sequences.get(), 0);
				parity.reset();
				return;
			}
			const std::uint32_t count = static_cast<const std::uint32_t*>(countMap.pData)[0];
			const std::uint32_t culled = static_cast<const std::uint32_t*>(countMap.pData)[1];
			const auto* gpuSequences = static_cast<const DrawSequence*>(sequencesMap.pData);
			// The colour draws and the terrain groups' (phase 2's range), from their state classes' sub-ranges.
			std::vector<DrawSequence> built = ClassSequences(*parity, gpuSequences, static_cast<const std::uint32_t*>(classCountsMap.pData));
			context->Unmap(parity->classCounts.get(), 0);
			// A sequence against its template, field by field, with the rows as indices into the payload's tables ("-": outside).
			// Outside: the recent payload whose tables hold it, if any (its serial, and how many colour epochs before the copy it was first seen).
			auto originOf = [&](std::uint64_t a_address) -> std::string {
				const auto& history = state.parityPayloads;
				const std::uint32_t kept = std::min<std::uint32_t>(state.parityPayloadsSeen, static_cast<std::uint32_t>(history.size()));
				for (std::uint32_t k = 1; k <= kept; ++k) {
					const auto& seen = history[(state.parityPayloadsSeen - k) % history.size()];
					const bool material = a_address >= seen.materialBase && a_address < seen.materialBase + seen.materialBytes;
					const bool pipeline = a_address >= seen.pipelineBase && a_address < seen.pipelineBase + seen.pipelineBytes;
					if (material || pipeline)
						return fmt::format(", {} rows of payload {} (seen {} colour epochs before the copy)", material ? "material" : "pipeline", seen.serial,
							static_cast<std::int64_t>(parity->armedEpoch) - static_cast<std::int64_t>(seen.epoch));
				}
				return ", in no recent payload";
			};
			auto rowOf = [&](std::uint64_t a_address, std::uint64_t a_base, std::uint64_t a_bytes) {
				return a_address >= a_base && (a_address - a_base) % a_bytes == 0 ? fmt::format("{}", (a_address - a_base) / a_bytes) :
				                                                                    fmt::format("- ({:#x}{})", a_address, originOf(a_address));
			};
			auto describe = [&](const DrawSequence& a_gpu, const DrawSequence& a_cpu) {
				const auto pipelineRow = [&](const DrawSequence& a) { return rowOf(a.pipelineRowAddress, parity->pipelineRowsBase, kPipelineRowBytes); };
				const auto materialRow = [&](const DrawSequence& a) { return rowOf(a.materialRowAddress, parity->materialRowsBase, kMaterialRowBytes); };
				return fmt::format("GPU vs CPU: pipeline {} vs {}, pipeline row {} vs {}, material row {} vs {}, vertices {:#x}/{} vs {:#x}/{}, indices {:#x}/{} vs {:#x}/{}, "
								   "index count {} vs {}, first index {} vs {}",
					a_gpu.pipelineIndex, a_cpu.pipelineIndex, pipelineRow(a_gpu), pipelineRow(a_cpu), materialRow(a_gpu), materialRow(a_cpu), a_gpu.vertexBufferAddress,
					a_gpu.vertexBufferSize, a_cpu.vertexBufferAddress, a_cpu.vertexBufferSize, a_gpu.indexBufferAddress, a_gpu.indexBufferSize, a_cpu.indexBufferAddress,
					a_cpu.indexBufferSize, a_gpu.indexCount, a_cpu.indexCount, a_gpu.firstIndex, a_cpu.firstIndex);
			};
			// The local shadow lights BuildDraws selected, from each object word, against the CPU's selection; then out of the word,
			// which the CPU's templates do not carry.
			std::size_t localChecked = 0, localDiffering = 0;
			std::string localFirst;
			drawnLocalShadows.clear();
			auto takeLocalShadows = [&](DrawSequence& a_sequence) {
				const std::uint32_t gpu = (a_sequence.objectIndex & kObjectLocalShadowMask) >> kObjectLocalShadowShift;
				a_sequence.objectIndex &= ~kObjectLocalShadowMask;
				const std::uint32_t object = a_sequence.objectIndex & kObjectIndexMask;
				const auto it = parity->expectedLocalShadows.find(object);
				const std::uint32_t cpu = it != parity->expectedLocalShadows.end() ? it->second : 0u;
				++localChecked;
				drawnLocalShadows[object] = { gpu, parity->gatedLocalShadows };
				if ((gpu & ~parity->gatedLocalShadows) != (cpu & ~parity->gatedLocalShadows) && localDiffering++ == 0)
					localFirst = fmt::format("object {}: GPU {:X}, CPU {:X}", object, gpu, cpu);
			};
			for (auto& sequence : built)
				takeLocalShadows(sequence);
			// The decal slots: fixed, so they compare in place, in the sub-range of their own state class (DrawnDecal). A culled slot is
			// the template with an index count of zero; anything else differing is a defect.
			std::size_t decalDiffering = 0, decalCulled = 0, decalSlots = 0;
			std::string decalFirst;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				const auto& expectedDecals = parity->expectedDecals[group];
				for (std::size_t slot = 0; slot < expectedDecals.size() && slot < parity->sequenceDecals; ++slot, ++decalSlots) {
					// A zero-count slot draws nothing whatever its other fields hold: it is either a decal
					// the culling rejected or a blank the epoch pushed for one it could not build a record
					// for (whose template is then empty). Either way only the count matters.
					const auto* drawn = DrawnDecal(*parity, gpuSequences, group, slot);
					if (!drawn) {
						++decalCulled;
						continue;
					}
					DrawSequence decal = *drawn;
					takeLocalShadows(decal);
					decal.objectIndex &= ~kObjectSunMiss;  // as the draws below: the CPU's template has no cascade test
					if (std::memcmp(&decal, &expectedDecals[slot], sizeof(DrawSequence)) != 0 && decalDiffering++ == 0)
						decalFirst = fmt::format("group {} slot {} (object {}): {}", group, slot, decal.objectIndex & kObjectIndexMask, describe(decal, expectedDecals[slot]));
				}
			}
			context->Unmap(parity->count.get(), 0);
			context->Unmap(parity->sequences.get(), 0);
			// BuildDraws appends in any order: compare as sets, keyed by the object index (a record address is shared by every sequence
			// of a deduplicated record, which would leave equal-key runs in arbitrary order on the two sides). A skin of several
			// partitions writes one sequence per partition drawn (GeometryDraw::nextPartition), which its index range tells apart:
			// partitions can share one index buffer, so the key is the index buffer and the first index.
			// A draw that missed every sun cascade carries kObjectSunMiss in its object word; the CPU's template does not.
			for (auto& sequence : built)
				sequence.objectIndex &= ~kObjectSunMiss;
			// The plain draws (CS_DCLF_MAIN_PLAIN, CS_DCLF_TERRAIN_PLAIN) are pooled: no pipeline index, the first index the index pool's, the
			// count 0 without pool indices, the bucket slot as first instance. With any, those fields are left out on both sides.
			auto& expected = parity->expected;
			if (const auto& plan = parity->classPlan; plan && (!plan->main.empty() || !plan->terrain.empty())) {
				const auto plainFields = [](DrawSequence& a_sequence) {
					a_sequence.pipelineIndex = 0;
					a_sequence.indexCount = 0;
					a_sequence.firstIndex = 0;
					a_sequence.vertexOffset = 0;
					a_sequence.firstInstance = 0;
				};
				for (auto& sequence : built)
					plainFields(sequence);
				for (auto& sequence : expected)
					plainFields(sequence);
			}
			auto keyOf = [](const DrawSequence& a) { return std::tuple(a.objectIndex, a.indexBufferAddress, a.firstIndex); };
			auto byObject = [&](const DrawSequence& a, const DrawSequence& b) { return keyOf(a) < keyOf(b); };
			std::sort(built.begin(), built.end(), byObject);
			std::sort(expected.begin(), expected.end(), byObject);
			// What the GPU writes is a subset of the CPU's templates whenever the culling rejects anything, so this is a
			// subsequence check: every sequence BuildDraws wrote must appear in the CPU list under the same object index, byte for byte.
			std::size_t differing = 0, missing = 0;
			std::size_t first = SIZE_MAX, firstExpected = 0;
			for (std::size_t b = 0, e = 0; b < built.size(); ++b) {
				while (e < expected.size() && byObject(expected[e], built[b]))
					++e;  // the CPU built a template the culling rejected (or a partition it did not draw)
				if (e == expected.size() || keyOf(expected[e]) != keyOf(built[b])) {
					++missing;  // a sequence with no template at all, which no culling can explain
					continue;
				}
				if (std::memcmp(&built[b], &expected[e], sizeof(DrawSequence)) != 0) {
					if (first == SIZE_MAX) {
						first = b;
						firstExpected = e;
					}
					++differing;
				}
				++e;
			}
			logger::info("[DCLF] BuildDraws local shadow lights {}: {} draws, {} differ from the CPU's selection{}{}", localDiffering ? "MISMATCH" : "OK",
				localChecked, localDiffering, localFirst.empty() ? "" : "; first: ", localFirst);
			if (decalSlots)
				logger::info("[DCLF] BuildDraws decal parity {}: {} slots, {} culled, {} differ{}{}", decalDiffering ? "MISMATCH" : "OK", decalSlots, decalCulled, decalDiffering,
					decalFirst.empty() ? "" : "; first: ", decalFirst);
			if (!differing && !missing) {
				logger::info("[DCLF] BuildDraws parity OK: {} of {} sequences match the CPU templates ({} rejected by the culling)", count, expected.size(),
					expected.size() - count);
			} else {
				logger::warn("[DCLF] BuildDraws parity MISMATCH: GPU wrote {}, CPU templated {} ({} counted culled); {} differ, {} have no template; sequences version {} "
							 "named, {} current{}",
					count, expected.size(), culled, differing, missing, parity->sequencesNamed, parity->sequencesCurrent,
					first != SIZE_MAX ? fmt::format(" (first: object {}, {})", built[first].objectIndex, describe(built[first], expected[firstExpected])) : "");
			}
			parity.reset();
			return;
		}
		// The colour epoch's only: the Z-prepass's draws go to its pipeline slots' buckets (shadow-format sequences at each bucket's
		// range, BuildDrawsCS's BucketTableOffset path), never to the plain range or the decal slots compared here, which then
		// still hold the last colour epoch's (another frame's payload).
		if (a_epoch.depthOnly || (parityEpochs++ % 300) != 0 || !a_resources->countD3D11)
			return;
		// The version the epoch's recording wrote: the one the adopted set names (resolved through it, IResourceVersions), which
		// need not be the buffer's current one.
		auto namedSequences = DrawsView::Get().Adopted().Find(*a_resources->sequences);
		const auto currentSequences = a_resources->sequences->Current();
		const std::uint64_t sequencesNamed = namedSequences ? namedSequences->number : 0;
		const std::uint64_t sequencesCurrent = currentSequences ? currentSequences->number : 0;
		auto* const sequencesView = state.paritySequences.Of(namedSequences ? std::move(namedSequences) : currentSequences);
		if (!sequencesView)
			return;
		// After the epoch in D3D11 stream order: the copies see what BuildDraws wrote.
		auto staging = [&](ID3D11Buffer* a_source) {
			D3D11_BUFFER_DESC desc{};
			a_source->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Buffer> buffer;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, buffer.put())))
				return buffer;
			context->CopyResource(buffer.get(), a_source);
			return buffer;
		};
		// The row tables BuildDraws read: the epoch's payload's (none: the epoch drew nothing of a payload to compare).
		const auto* payloadVersions = a_epoch.payloadVersions;
		if (!payloadVersions || !payloadVersions->main)
			return;
		ParityReadback readback;
		readback.pipelineRowsBase = (*payloadVersions)[Draws::PayloadPart::PipelineRows].address;
		readback.materialRowsBase = (*payloadVersions)[Draws::PayloadPart::MaterialRows].address;
		readback.sequences = staging(sequencesView);
		readback.count = staging(a_resources->countD3D11.get());
		// The ranges of the shape the epoch was committed into (the adopted revision's).
		const auto& active = DrawsView::Get().revisions.active;
		const auto epoch = Draws::EpochOfShape(Draws::ShapeKind::Main, static_cast<std::uint32_t>(a_epoch.depthOnly ? kDepthShape : kColourShape));
		const auto shape = active ? active->Get<PassFrame>(DrawsView::Impl::SceneRevisions::kShapeSlot + epoch) : nullptr;
		if (!shape)
			return;
		readback.sequenceDraws = shape->sequenceDraws;
		readback.sequenceDecals = shape->sequenceDecals;
		readback.classes = shape->classes;
		readback.classPlan = shape->classPlan;
		// The state class sub-ranges' counts, the version the adopted set names (as the sequences').
		auto* const classCountsView = state.parityClassCounts.Of(DrawsView::Get().Adopted().Find(*a_resources->classCounts));
		if (!classCountsView)
			return;
		readback.classCounts = staging(classCountsView);
		// The templates cover every candidate; the gate drops the ones the engine culled before BuildDraws
		// writes a sequence for them, and it is a pure per-object flag test, so the expectation can apply it
		// exactly. Frustum culling cannot be predicted here, which is why parity and culling are separate
		// switches. inputs is parallel to sequences.
		readback.expected.clear();
		readback.expected.reserve(sequences.size());
		// The templates' rows are offsets into the tables: placed at the payload's. Their pipeline index is the set index, which BuildDraws
		// stores as its index in its state class (the shape's set version's class map).
		auto place = [&](DrawSequence& a_sequence) {
			a_sequence.pipelineRowAddress += readback.pipelineRowsBase;
			a_sequence.materialRowAddress += readback.materialRowsBase;
			ClassIndexOf(a_sequence, shape->indirect.classOf);
		};
		for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
			readback.expectedDecals[group] = decalTemplates[group];
			for (auto& decal : readback.expectedDecals[group])
				place(decal);
		}
		{
			// Cull-only inputs carry no sequence, so the two run at different rates and the drawable ones
			// have to be counted off rather than indexed in step. Decals have their own slots and templates.
			// A skin of several partitions has one template per partition drawn, consecutive.
			std::size_t sequence = 0;
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || (input.flags & kObjectDecal))
					continue;
				const std::size_t templates = PartitionDraws(input.partitions);
				if (sequence + templates > sequences.size())
					break;
				const std::size_t first = sequence;
				sequence += templates;
				// A forward input's draws are in the forward range (sorted there), not compared here.
				if (input.flags & kObjectForward)
					continue;
				readback.expected.insert(readback.expected.end(), sequences.begin() + first, sequences.begin() + sequence);
			}
			for (auto& expected : readback.expected)
				place(expected);
		}
		// The CPU's local shadow selection for every object the epoch drew, with the volumes it sampled (none on the Z-prepass).
		if (!a_payload.inputs.depthOnly) {
			const auto& tables = Scene::FrameView::Get().GetTables();
			const std::uint64_t landscapeFlags = Extensions::LightVolumes().landscapeFlags;
			readback.gatedLocalShadows = 0;
			for (const auto& volume : shadowVolumes)
				if (volume.portalContext)
					readback.gatedLocalShadows |= volume.maskBit & 0xFu;
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || input.objectIndex >= tables.objects.size() || input.objectIndex >= tables.objectGeometry.size())
					continue;
				const auto* geometry = tables.objectGeometry[input.objectIndex];
				const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				if (!geometry)
					continue;
				const float center[3]{ geometry->worldBound.center.x, geometry->worldBound.center.y, geometry->worldBound.center.z };
				const bool landscape = property && (property->flags.underlying() & landscapeFlags);
				readback.expectedLocalShadows[input.objectIndex] = ShadowVolumeMaskOf(shadowVolumes, landscape, center, geometry->worldBound.radius) & 0xFu;
			}
		}
		readback.sequencesNamed = sequencesNamed;
		readback.sequencesCurrent = sequencesCurrent;
		readback.armedEpoch = parityEpochs;
		readback.framesLeft = 3;
		if (readback.sequences && readback.count && readback.classCounts)
			parity = std::move(readback);
	}

	bool DrawnLocalShadowsOf(std::uint32_t a_object, DrawnLocalShadows& a_out)
	{
		const auto it = drawnLocalShadows.find(a_object);
		if (it == drawnLocalShadows.end())
			return false;
		a_out = it->second;
		return true;
	}
}
