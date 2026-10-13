#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "DrawsView.h"
#	include "Events.h"
#	include "Observers.h"

// CS_DCLF_BUILD_PARITY: every 300th epoch, BuildDraws' output (the sequences, the decal slots, the local shadow lights in the object
// words) copied to staging and compared three frames later with the CPU's templates.

namespace DCLF::Diagnostics
{
	void CheckBuildParity(const MainEpochDone& a_epoch)
	{
		const auto& a_resources = a_epoch.resources;
		const MainPayload& a_payload = a_epoch.payload;
		const auto& localShadows = DrawsView::Get().localShadows;
		auto& state = DrawsState::Get();
		auto& parity = state.parity;
		auto& parityEpochs = state.parityEpochs;
		using ParityReadback = DrawsState::ParityReadback;
		const auto& sequences = a_payload.sequences;
		const auto& decalTemplates = a_payload.decalTemplates;
		const auto& inputs = a_payload.inputList;
		auto* context = globals::d3d::context;
		if (parity) {
			if (--parity->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE countMap{}, sequencesMap{};
			if (FAILED(context->Map(parity->count.get(), 0, D3D11_MAP_READ, 0, &countMap)) ||
				FAILED(context->Map(parity->sequences.get(), 0, D3D11_MAP_READ, 0, &sequencesMap))) {
				parity.reset();
				return;
			}
			const std::uint32_t count = static_cast<const std::uint32_t*>(countMap.pData)[0];
			const std::uint32_t culled = static_cast<const std::uint32_t*>(countMap.pData)[1];
			const auto* gpuSequences = static_cast<const DrawSequence*>(sequencesMap.pData);
			std::vector<DrawSequence> built(gpuSequences, gpuSequences + std::min<std::size_t>(count, parity->sequenceDraws));
			// And the terrain groups' draws, from their ranges in phase 2's (BuildDrawsCS appends them there, not to the draws').
			for (std::uint32_t group = 0; group < kTerrainGroups; ++group) {
				const std::uint32_t appended = std::min(static_cast<const std::uint32_t*>(countMap.pData)[kCountTerrainGroupWord + group], parity->terrainCapacity[group]);
				const auto* range = gpuSequences + std::size_t(parity->sequenceDraws) + (group == 0 ? 0u : parity->terrainCapacity[0]);
				built.insert(built.end(), range, range + appended);
			}
			// The local shadow lights BuildDraws selected, from each object word, against the CPU's selection; then out of the word,
			// which the CPU's templates do not carry.
			std::size_t localChecked = 0, localDiffering = 0;
			std::string localFirst;
			auto takeLocalShadows = [&](DrawSequence& a_sequence) {
				const std::uint32_t gpu = (a_sequence.objectIndex & kObjectLocalShadowMask) >> kObjectLocalShadowShift;
				a_sequence.objectIndex &= ~kObjectLocalShadowMask;
				const std::uint32_t object = a_sequence.objectIndex & kObjectIndexMask;
				const auto it = parity->expectedLocalShadows.find(object);
				const std::uint32_t cpu = it != parity->expectedLocalShadows.end() ? it->second : 0u;
				++localChecked;
				if (gpu != cpu && localDiffering++ == 0)
					localFirst = fmt::format("object {}: GPU {:X}, CPU {:X}", object, gpu, cpu);
			};
			for (auto& sequence : built)
				takeLocalShadows(sequence);
			// The decal slots: fixed, so they compare in place. A culled slot is the template with an index
			// count of zero; anything else differing is a defect.
			std::size_t decalDiffering = 0, decalCulled = 0, decalSlots = 0;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				const auto& expectedDecals = parity->expectedDecals[group];
				const auto* slots = gpuSequences + 2 * std::size_t(parity->sequenceDraws) + std::size_t(group) * parity->sequenceDecals;
				for (std::size_t slot = 0; slot < expectedDecals.size() && slot < parity->sequenceDecals; ++slot, ++decalSlots) {
					// A zero-count slot draws nothing whatever its other fields hold: it is either a decal
					// the culling rejected or a blank the epoch pushed for one it could not build a record
					// for (whose template is then empty). Either way only the count matters.
					if (slots[slot].indexCount == 0) {
						++decalCulled;
						continue;
					}
					DrawSequence decal = slots[slot];
					takeLocalShadows(decal);
					decal.objectIndex &= ~kObjectSunMiss;  // as the draws below: the CPU's template has no cascade test
					if (std::memcmp(&decal, &expectedDecals[slot], sizeof(DrawSequence)) != 0)
						++decalDiffering;
				}
			}
			context->Unmap(parity->count.get(), 0);
			context->Unmap(parity->sequences.get(), 0);
			// BuildDraws appends in any order: compare as sets. The key has to be unique per sequence, which
			// the object index is and the record address is not - once records deduplicate, dozens of
			// sequences share an address, the sort stops being a total order, and equal-key runs land in
			// arbitrary relative order on the two sides. That reports mismatches that are not mismatches.
			// A skin of several partitions writes one sequence per partition, which its index buffer tells apart.
			// A draw that missed every sun cascade carries kObjectSunMiss in its object word; the CPU's template does not.
			for (auto& sequence : built)
				sequence.objectIndex &= ~kObjectSunMiss;
			auto byObject = [](const DrawSequence& a, const DrawSequence& b) {
				return a.objectIndex != b.objectIndex ? a.objectIndex < b.objectIndex : a.indexBufferAddress < b.indexBufferAddress;
			};
			std::sort(built.begin(), built.end(), byObject);
			auto& expected = parity->expected;
			std::sort(expected.begin(), expected.end(), byObject);
			// What the GPU writes is a SUBSET of the CPU's templates whenever the culling rejects anything, so
			// this is a subsequence check, not an element-wise one: every sequence BuildDraws wrote must
			// appear in the CPU list under the same object index, byte for byte. Comparing position by
			// position instead reported every sequence past the first culled object as differing - a
			// mismatch that says nothing, on the configuration DCLF actually ships.
			std::size_t differing = 0, missing = 0;
			std::size_t first = SIZE_MAX, firstExpected = 0;
			for (std::size_t b = 0, e = 0; b < built.size(); ++b) {
				while (e < expected.size() && expected[e].objectIndex < built[b].objectIndex)
					++e;  // the CPU built a template the culling rejected
				if (e == expected.size() || expected[e].objectIndex != built[b].objectIndex) {
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
			++state.sequenceChecks;
			logger::info("[DCLF] BuildDraws local shadow lights {}: {} draws, {} differ from the CPU's selection{}{}", localDiffering ? "MISMATCH" : "OK",
				localChecked, localDiffering, localFirst.empty() ? "" : "; first: ", localFirst);
			if (localDiffering)
				++state.sequenceMismatches;
			if (decalSlots)
				logger::info("[DCLF] BuildDraws decal parity {}: {} slots, {} culled, {} differ", decalDiffering ? "MISMATCH" : "OK", decalSlots, decalCulled, decalDiffering);
			if (decalDiffering)
				++state.sequenceMismatches;
			if (!differing && !missing) {
				logger::info("[DCLF] BuildDraws parity OK: {} of {} sequences match the CPU templates ({} rejected by the culling)", count, expected.size(),
					expected.size() - count);
			} else {
				++state.sequenceMismatches;
				logger::warn("[DCLF] BuildDraws parity MISMATCH: GPU wrote {}, CPU templated {} ({} counted culled); {} differ, {} have no template{}", count,
					expected.size(), culled, differing, missing,
					first != SIZE_MAX ? fmt::format(" (first: object {}, GPU vs CPU: pipeline {} vs {}, rows {:#x}/{:#x} vs {:#x}/{:#x}, vertices {:#x}/{} vs {:#x}/{}, "
													"indices {:#x}/{} vs {:#x}/{}, index count {} vs {}, first index {} vs {})",
											built[first].objectIndex, built[first].pipelineIndex, expected[firstExpected].pipelineIndex, built[first].pipelineRowAddress,
											built[first].materialRowAddress, expected[firstExpected].pipelineRowAddress, expected[firstExpected].materialRowAddress,
											built[first].vertexBufferAddress, built[first].vertexBufferSize, expected[firstExpected].vertexBufferAddress,
											expected[firstExpected].vertexBufferSize, built[first].indexBufferAddress, built[first].indexBufferSize,
											expected[firstExpected].indexBufferAddress, expected[firstExpected].indexBufferSize, built[first].indexCount,
											expected[firstExpected].indexCount, built[first].firstIndex, expected[firstExpected].firstIndex) :
										"");
			}
			parity.reset();
			return;
		}
		if ((parityEpochs++ % 300) != 0 || !a_resources->sequencesD3D11 || !a_resources->countD3D11)
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
		ParityReadback readback;
		readback.sequences = staging(a_resources->sequencesD3D11.get());
		readback.count = staging(a_resources->countD3D11.get());
		readback.sequenceDraws = a_resources->sequenceDraws;
		readback.sequenceDecals = a_resources->sequenceDecals;
		readback.terrainCapacity = a_resources->terrainCapacity;
		// The templates cover every candidate; the gate drops the ones the engine culled before BuildDraws
		// writes a sequence for them, and it is a pure per-object flag test, so the expectation can apply it
		// exactly. Frustum culling cannot be predicted here, which is why parity and culling are separate
		// switches. inputs is parallel to sequences.
		readback.expected.clear();
		readback.expected.reserve(sequences.size());
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			readback.expectedDecals[group] = decalTemplates[group];
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
		}
		// The CPU's local shadow selection for every object the epoch drew, with the volumes it uploaded (none on the Z-prepass).
		if (!a_payload.inputs.depthOnly) {
			const auto& tables = Scene::FrameView::Get().GetTables();
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || input.objectIndex >= tables.objects.size() || input.objectIndex >= tables.objectGeometry.size())
					continue;
				const auto* geometry = tables.objectGeometry[input.objectIndex];
				const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				if (!geometry)
					continue;
				const float center[3]{ geometry->worldBound.center.x, geometry->worldBound.center.y, geometry->worldBound.center.z };
				readback.expectedLocalShadows[input.objectIndex] = localShadows.MaskOf(property, center, geometry->worldBound.radius) & 0xFu;
			}
		}
		readback.framesLeft = 3;
		if (readback.sequences && readback.count)
			parity = std::move(readback);
	}
}

#endif
