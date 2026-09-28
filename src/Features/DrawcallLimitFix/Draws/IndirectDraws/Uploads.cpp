#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	namespace
	{
		// The payload's own uploads: the constant arena, the per-object records and bone rows, the binding records,
		// and the draw inputs with their geometry. Each buffer has its own condition: a depth epoch where every
		// candidate is cull-only has no records and plenty of inputs, and BuildDraws dispatches over the inputs.
		template <class Emit>
		void ForEachMainPayloadUpload(const MainPayload& a_payload, const Resources& a_resources, Emit&& a_emit)
		{
			// Each segment's constants and records are its own buffers (the Z-prepass's: Resources::constantsDepth, recordsDepth).
			const bool depthBuffers = a_payload.inputs.depthOnly && a_resources.recordsDepth;
			const auto& constantsTarget = depthBuffers ? a_resources.constantsDepth : a_resources.constants;
			const auto& recordsTarget = depthBuffers ? a_resources.recordsDepth : a_resources.records;
			const std::size_t segment = a_payload.inputs.depthOnly ? 0 : 1;
			const auto& bytes = a_payload.arena.Bytes();
			if (!bytes.empty())
				a_emit(constantsTarget, bytes.data(), bytes.size(), false, 0);
			if (a_payload.persistent) {
				ZoneScopedN("CS.DCLF.UploadRanges.Bindings");
				// The kept blocks and records: what changed since the version the buffers hold, else all of them.
				a_payload.keptConstants.Emit(a_resources.constantsUploaded[segment],
					[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(constantsTarget, a_data, a_bytes, false, a_offset); });
				a_payload.keptRecords.Emit(a_resources.recordsUploaded[segment],
					[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(recordsTarget, a_data, a_bytes, false, a_offset); });
			}
			TracyCZoneN(objectUploadZone, "CS.DCLF.UploadRanges.Objects", true);
			a_payload.objectRecords.Emit(a_resources.tablesHeld.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_emit(a_resources.objects, a_data, a_bytes, false, a_offset);
			});
			TracyCZoneEnd(objectUploadZone);
			if (a_resources.bones)
				EmitBones(a_payload.bones, a_resources.tablesHeld.bones, nullptr, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					a_emit(a_resources.bones, a_data, a_bytes, false, a_offset);
				});
			if (!a_payload.records.empty())
				a_emit(recordsTarget, a_payload.records.data(), a_payload.records.size() * sizeof(DrawBindings), true, 0);
			// The segment's input buffer: the resident region at its head when the buffer does not hold this version of it,
			// then the frame's own inputs after it.
			const bool depth = a_payload.inputs.depthOnly && a_resources.inputsDepth;
			const auto& inputs = depth ? a_resources.inputsDepth : a_resources.inputs;
			const std::size_t regionCount = a_payload.resident.Count();
			TracyCZoneN(residentUploadZone, "CS.DCLF.UploadRanges.Resident", true);
			a_payload.resident.Emit(a_resources.residentUploaded[depth ? 0 : 1],
				[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(inputs, a_data, a_bytes, false, a_offset); });
			if (!a_payload.inputList.empty())
				a_emit(inputs, a_payload.inputList.data(), a_payload.inputList.size() * sizeof(DrawInput), false, regionCount * sizeof(DrawInput));
			TracyCZoneEnd(residentUploadZone);
			EmitGeometryDraws(a_payload.geometryDraws, a_resources.tablesHeld.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_emit(a_resources.geometries, a_data, a_bytes, false, a_offset);
			});
		}

		// Render thread: the payload through the commit's uploads, copied now.
		void UploadMainPayload(const MainPayload& a_payload, const Resources& a_resources, CommitUploads& a_uploads)
		{
			ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, bool, std::size_t a_offset) {
				a_uploads(a_target, a_data, a_bytes, a_offset);
			});
		}
	}

	// On the worker, after the build: the payload's uploads into a staged batch (a released one from the job's
	// pool, or a new one), so the commit copies nothing. The records' staging is kept for the commit's patches.
	void StageMainPayload(MainPayload& a_payload, const Resources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
	{
		ZoneScopedN("CS.DCLF.StageMainPayload");
		auto batch = AcquireStagedBatch(a_pool);
		ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, bool a_records, std::size_t a_offset) {
			auto* staging = batch->Stage(org::runtime::UploadTarget::FromShared(a_target), a_offset, a_data, a_bytes);
			if (a_records)
				a_payload.stagedRecords = reinterpret_cast<DrawBindings*>(staging);
		});
		a_payload.stagedFor = &a_resources;
		a_payload.staged = std::move(batch);
	}

	// On the worker, after the shadow build: what the commit would upload that does not depend on the views it
	// captures - the shared tables, the used modes' inputs, the arena past its view head - and, per view slot the
	// job expects, the records naming that slot's blocks and its zeroed counters. The commit uploads the view
	// head and any slot past a_slots itself.
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::uint32_t a_slots,
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
	{
		ZoneScopedN("CS.DCLF.StageShadowPayload");
		using org::runtime::UploadTarget;
		auto batch = AcquireStagedBatch(a_pool);
		a_payload.objects.Emit(a_resources.tablesHeld.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(a_resources.objects), a_offset, a_data, a_bytes);
		});
		EmitBones(a_payload.bones, a_resources.tablesHeld.bones, nullptr, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(a_resources.bones), a_offset, a_data, a_bytes);
		});
		EmitGeometryDraws(a_payload.geometries, a_resources.tablesHeld.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(a_resources.geometries), a_offset, a_data, a_bytes);
		});
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_payload.inputs.modeUsed[m])
				continue;
			EmitShadowInputs(a_payload, m, a_payload.kept ? a_resources.inputsUploaded[m] : 0, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				batch->Stage(UploadTarget::FromShared(a_resources.inputs[m]), a_offset, a_data, a_bytes);
			});
		}
		const auto& bytes = a_payload.arena.Bytes();
		if (bytes.size() > kShadowMaterialBlocksOffset)
			batch->Stage(UploadTarget::FromShared(a_resources.constants), kShadowMaterialBlocksOffset, bytes.data() + kShadowMaterialBlocksOffset,
				bytes.size() - kShadowMaterialBlocksOffset);
		const std::uint32_t slots = std::min<std::uint32_t>(a_slots, kMaxShadowViews);
		const std::uint64_t base = a_payload.inputs.addresses.constants;
		for (std::uint32_t slot = 0; slot < slots; ++slot) {
			const std::uint64_t viewBlockOffset = std::uint64_t(slot) * kShadowViewSlotBytes;
			const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
			const std::uint64_t recordsOffset = std::uint64_t(slot) * kShadowRecordCapacity * sizeof(DrawBindings);
			if (a_payload.kept) {
				// The kept records: what the slot does not hold yet.
				EmitShadowRecords(a_payload, a_resources.recordsUploaded[slot], base + viewBlockOffset, base + perFrameOffset,
					[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
						batch->Stage(UploadTarget::FromShared(a_resources.records), recordsOffset + a_offset, a_data, a_bytes);
					});
			} else if (auto* staging = batch->Stage(UploadTarget::FromShared(a_resources.records), recordsOffset, a_payload.records.size() * sizeof(DrawBindings))) {
				// Whole records into write-combined staging, in order: written, never read.
				for (std::size_t r = 0; r < a_payload.records.size(); ++r) {
					DrawBindings record = a_payload.records[r];
					AddressToSlot(record, base + viewBlockOffset, base + perFrameOffset);
					std::memcpy(staging + r * sizeof(DrawBindings), &record, sizeof(DrawBindings));
				}
			}
			batch->Stage(UploadTarget::FromShared(a_resources.count[slot]), 0, kZeroCounts, sizeof(kZeroCounts));
		}
		a_payload.stagedSlots = slots;
		a_payload.stagedFor = &a_resources;
		a_payload.staged = std::move(batch);
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats,
		std::vector<std::shared_ptr<const void>>& a_bindingOwners)
	{
		ZoneScopedN("CS.DCLF.CommitMainPayload");
		const auto& in = a_payload.inputs;
		const auto& tables = a_store.GetTables();
		const bool depthOnly = in.depthOnly;
		const std::uint32_t frameNumber = in.frameNumber;
		auto& textures = GpuTextures::Get();
		auto& mirror = ConstantMirror::Get();
		const bool replayVertexInputs = !depthOnly && prepassInputs;
		CommitUploads uploads(commitStagedPool);
		auto lap = [&, last = std::chrono::steady_clock::now()](std::size_t a_part) mutable {
			const auto now = std::chrono::steady_clock::now();
			a_stats.commitUs[a_part] += std::chrono::duration<double, std::micro>(now - last).count();
			last = now;
		};
		// Nothing to draw until this commit publishes the segment's shape again (a failed commit leaves it so).
		const std::size_t shapeIndex = depthOnly ? kDepthShape : kColourShape;
		a_resources->frames[shapeIndex].store(nullptr, std::memory_order_release);

		// The frame's textures (t16 and up), resolved now and patched into the records that read them.
		std::array<std::uint32_t, kTextureRegisters> frameTextures;
		frameTextures.fill(kInvalidIndex);
		if (frameTextureGeneration != textures.Generation()) {
			frameTextureBindings = {};
			frameTextureGeneration = textures.Generation();
		}
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t) {
			auto& held = frameTextureBindings[t];
			const auto* view = a_capture.psViews[t];
			if (!view) {
				if (held.view)
					held = {};
				frameTextures[t] = textures.NullIndex();
				continue;  // the shared lookup root already owns the null descriptor
			}
			if (held.view != view || !held.binding.owner || held.binding.index == GpuTextures::kInvalid) {
				held.view = a_capture.psViews[t];
				held.binding = textures.ResolveBinding(held.view, 64 + t);
			}
			frameTextures[t] = held.binding.index;
			if (held.binding.owner)
				a_bindingOwners.push_back(held.binding.owner);
		}
		if (a_resources->lightLimitFix && !depthOnly)
			ORGLightCulling::Get().GetShaderResourceIndices(frameTextures[kLightsRegister], frameTextures[kLightsRegister + 1], frameTextures[kLightsRegister + 2]);
		for (const auto& frameBuffer : depthOnly ? decltype(a_resources->frameBuffers){} : a_resources->frameBuffers) {
			auto* buffer = BufferOf(a_capture.psViews[frameBuffer.textureRegister]);
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			const std::size_t offset = std::size_t(frameBuffer.firstElement) * frameBuffer.stride;
			const std::size_t bytes = std::size_t(frameBuffer.elements) * frameBuffer.stride;
			if (contents.size() < offset + bytes) {
				// A structured buffer the commit cannot fill: its copy reads zero.
				static std::array<std::uint32_t, kTextureRegisters> unfilled{};
				if ((unfilled[frameBuffer.textureRegister]++ % 600) == 0)
					logger::warn("[DCLF] frame buffer t{} ({} elements of {} bytes) not filled: the mirror holds {} bytes of it ({} times)", frameBuffer.textureRegister,
						frameBuffer.elements, frameBuffer.stride, contents.size(), unfilled[frameBuffer.textureRegister]);
				continue;  // not written since it is watched
			}
			uploads(frameBuffer.copy, contents.data() + offset, bytes, 0);
			frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
		}
		std::uint32_t frameTexturesMissing = 0;
		std::array<std::uint64_t, 2> missingRegisters{};
		for (const auto& [record, t] : a_payload.framePatches) {
			const std::uint32_t index = frameTextures[t];
			// A frame texture the pipeline reads but the pass did not bind reads zero, as an unbound view does
			// natively; counted, because the build could not skip the draw for it. A view bound natively that
			// could not be resolved (a buffer that is not a CPU-written structured buffer) lands here too, and
			// reads zero where the native draw reads the resource.
			if (index == kInvalidIndex) {
				++frameTexturesMissing;
				missingRegisters[(t >> 6) & 1] |= 1ull << (t & 63);
				static std::array<bool, kTextureRegisters> described{};
				if (t < kTextureRegisters && !std::exchange(described[t], true)) {
					if (auto* view = a_capture.psViews[t]) {
						D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
						view->GetDesc(&viewDesc);
						winrt::com_ptr<ID3D11Resource> resource;
						view->GetResource(resource.put());
						std::string what = fmt::format("view dimension {}, format {}", static_cast<std::uint32_t>(viewDesc.ViewDimension), static_cast<std::uint32_t>(viewDesc.Format));
						if (auto buffer = resource.try_as<ID3D11Buffer>()) {
							D3D11_BUFFER_DESC bufferDesc{};
							buffer->GetDesc(&bufferDesc);
							what += fmt::format(", buffer of {} bytes, stride {}, usage {}, CPU access {:X}, bind {:X}, misc {:X}", bufferDesc.ByteWidth, bufferDesc.StructureByteStride,
								static_cast<std::uint32_t>(bufferDesc.Usage), bufferDesc.CPUAccessFlags, bufferDesc.BindFlags, bufferDesc.MiscFlags);
						}
						char name[128]{};
						UINT size = sizeof(name) - 1;
						if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
							what += fmt::format(", '{}'", name);
						logger::warn("[DCLF] frame texture t{} is bound natively but not resolved: {}", t, what);
					} else {
						logger::warn("[DCLF] frame texture t{} is read but nothing is bound there at the capture", t);
					}
				}
			}
			const std::uint32_t value = index == kInvalidIndex ? (textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u) : index;
			a_payload.records[record].textures[t] = value;
			if (a_payload.stagedRecords)
				a_payload.stagedRecords[record].textures[t] = value;  // write-combined: written, never read
		}
		a_stats.frameTexturesMissing = frameTexturesMissing;
		a_stats.frameTexturesMissingRegisters = missingRegisters;
		lap(2);

		// The frame slots: each block into its slot, and the zeroed light block every draw's b3 reads.
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_blocks.vs[slot].empty())
				uploads(a_resources->frameConstants, a_blocks.vs[slot].data(), a_blocks.vs[slot].size(), FrameSlotOffset(false, slot));
			if (!a_blocks.ps[slot].empty())
				uploads(a_resources->frameConstants, a_blocks.ps[slot].data(), a_blocks.ps[slot].size(), FrameSlotOffset(true, slot));
		}
		{
			static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
			uploads(a_resources->frameConstants, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		}
		// The frame lighting, only when it changed (RefreshFrameConstants versions it).
		if (const auto& lightingTables = a_store.GetTables(); a_resources->frameLightingUploaded != lightingTables.frameLightingVersion) {
			uploads(a_resources->frameConstants, lightingTables.frameLighting.data(), sizeof(lightingTables.frameLighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
			a_resources->frameLightingUploaded = lightingTables.frameLightingVersion;
		}

		lap(3);
		// Upload (the graph's upload pass runs ahead of every pass of this epoch). The worker's build staged its
		// payload itself: one submission, no copies here. A build made here, or staged against resources since
		// recreated, is uploaded from its vectors.
		const auto& bytes = a_payload.arena.Bytes();
		const bool staged = a_payload.staged && a_payload.stagedFor == a_resources.get();
		if (staged) {
			org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(a_payload.staged));
			a_payload.stagedRecords = nullptr;
		} else if (!bytes.empty()) {
			uploads(depthOnly && a_resources->constantsDepth ? a_resources->constantsDepth : a_resources->constants, bytes.data(), bytes.size(), 0);
		}
		if (a_resources->facePositions)
			UploadFaceStreams(a_payload.faceStreams, a_resources->facePositions, a_resources->faceUploaded, uploads);
		// The depth segment clears every counter; the colour segment clears only the word its own draws
		// append through. The culling happens in the depth segment, so clearing the
		// whole buffer again here would erase the phase 1 and phase 2 numbers before anything read them
		// - they are written earlier in the same frame.
		const std::size_t zeroBytes = depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t);
		uploads(a_resources->count, kZeroCounts, zeroBytes, 0);
		// The sort's counts start at zero; from then on the scan that reads them clears them.
		if (a_resources->sort)
			a_resources->sort->ZeroCountsOnce(uploads);
		// The decal words: each group's slot count for its draw, and the tallies zeroed. Written by the
		// colour segment only, which is the one that submits decals.
		if (!depthOnly) {
			decalWords = { a_payload.decalCount[0], a_payload.decalCount[1], 0u, 0u };
			uploads(a_resources->count, decalWords.data(), decalWords.size() * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
		}
		if (!staged)
			UploadMainPayload(a_payload, *a_resources, uploads);
		// The kept records are immutable templates. Patch only commit-owned copies, after the worker's
		// base uploads. A rewritten base record also needs a patch even when the frame texture did not
		// change: its upload replaced the GPU's previously resolved descriptor with the template's zero.
		if (a_payload.persistent && !depthOnly && a_payload.keptRecords.elements) {
			auto& committed = a_resources->committedFrameTextures[1];
			std::array<std::uint64_t, 2> used{}, changed{}, missing{};
			for (const auto& mask : a_payload.patchMasks) {
				used[0] |= mask[0];
				used[1] |= mask[1];
			}
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				const std::uint32_t index = frameTextures[t];
				const std::uint32_t value = index == kInvalidIndex ? (textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u) : index;
				const bool indexChanged = committed[t] != value;
				committed[t] = value;
				const std::uint64_t bit = 1ull << (t % 64);
				if (!(used[t / 64] & bit))
					continue;
				if (index == kInvalidIndex)
					missing[t / 64] |= bit;
				if (indexChanged)
					changed[t / 64] |= bit;
			}
			std::uint32_t keptMissing = 0;
			const auto& recordsTarget = a_resources->records;
			for (std::uint32_t slot = 0; slot < a_payload.patchMasks.size() && slot < a_payload.keptRecords.Count(); ++slot) {
				const auto& mask = a_payload.patchMasks[slot];
				keptMissing += static_cast<std::uint32_t>(std::popcount(mask[0] & missing[0]) + std::popcount(mask[1] & missing[1]));
			}
			const std::uint32_t patched = EmitFrameRecordPatches(a_payload.keptRecords, a_resources->recordsUploaded[1],
				std::span<const std::array<std::uint64_t, 2>>(a_payload.patchMasks), changed, committed,
				[&](std::size_t slot, const DrawBindings& record) { uploads(recordsTarget, &record, sizeof(record), slot * sizeof(DrawBindings)); });
			a_stats.frameTexturesMissing = keptMissing;
			a_stats.frameTexturesMissingRegisters = missing;
			a_stats.framePatchedRecords += patched;
		}
		if (a_payload.persistent) {
			a_resources->constantsUploaded[depthOnly ? 0 : 1] = a_payload.keptConstants.Version();
			a_resources->recordsUploaded[depthOnly ? 0 : 1] = a_payload.keptRecords.Version();
		}
		// Either path uploaded the resident region when the buffer held another version of it, and the object records.
		a_resources->residentUploaded[depthOnly && a_resources->inputsDepth ? 0 : 1] = a_payload.resident.Version();
		const std::size_t objectBytes = a_payload.objectRecords.Emit(a_resources->tablesHeld.objects, [](const void*, std::size_t, std::size_t) {});
		if (a_payload.objectRecords.Version())
			a_resources->tablesHeld.objects = a_payload.objectRecords.Version();
		const std::size_t geometryBytes = EmitGeometryDraws(a_payload.geometryDraws, a_resources->tablesHeld.geometries, [](const void*, std::size_t, std::size_t) {});
		if (a_payload.geometryDraws.Version())
			a_resources->tablesHeld.geometries = a_payload.geometryDraws.Version();
		std::size_t boneRowsSent = 0;
		if (a_payload.bones.Version()) {
			BonesStore* bonesParity = PersistentParityEnabled() ? &mainBones : nullptr;
			boneRowsSent = EmitBones(a_payload.bones, a_resources->tablesHeld.bones, bonesParity, [](const void*, std::size_t, std::size_t) {});
			if (bonesParity && ParityDue(frameNumber))
				CheckBones(*bonesParity, a_payload.bones);
			a_resources->tablesHeld.bones = a_payload.bones.Version();
			mainBones.rowsSent += boneRowsSent;
		}
		a_stats.residentInputs = static_cast<std::uint32_t>(a_payload.resident.Count());
		if (!depthOnly) {
			a_stats.residentDraws = a_payload.residentDraws;
			a_stats.residentPairs = a_payload.residentPairs;
			a_stats.residentUndrawable = a_payload.residentUndrawable;
		}
		a_stats.residentVersions += a_payload.resident.Version() != a_stats.residentLastVersion[depthOnly ? 0 : 1] ? 1 : 0;
		a_stats.residentLastVersion[depthOnly ? 0 : 1] = a_payload.resident.Version();
		a_stats.residentResyncs += a_payload.residentResyncs;
		a_stats.residentParityChecks += a_payload.residentParityChecks;
		a_stats.residentParityMismatches += a_payload.residentParityMismatches;
		a_stats.residentMissing += a_payload.residentMissing;
		a_stats.boneRows = a_payload.bones.Version() ? static_cast<std::uint32_t>(boneRowsSent) : a_payload.bones.Rows();

		lap(4);
		// The build's stats.
		a_stats.skipped = a_payload.skipped;
		a_stats.missingTextures = a_payload.missingTextures;
		a_stats.missingVertexConstants = a_payload.missingVertexConstants;
		a_stats.missingPixelConstants = a_payload.missingPixelConstants;
		a_stats.deferredTextures = a_payload.deferredTextures;
		a_stats.bindlessParityChecks += a_payload.bindlessParityChecks;
		a_stats.bindlessParityMismatches += a_payload.bindlessParityMismatches;
		a_stats.recordParityChecks += a_payload.recordParityChecks;
		a_stats.recordParityMismatches += a_payload.recordParityMismatches;
		for (std::size_t i = 0; i < a_payload.partMs.size(); ++i)
			a_stats.partMs[i] = a_payload.partMs[i];
		if (!depthOnly)
			a_stats.decalsDrawn = a_payload.decalsDrawn;
		if (a_payload.shortBuffers) {
			const bool first = a_stats.shortBuffers == 0;
			a_stats.shortBuffers += a_payload.shortBuffers;
			const auto& shortBuffer = a_payload.shortBuffer;
			if (first && shortBuffer.object < tables.objects.size()) {
				const auto& geometry = tables.geometries[tables.objects[shortBuffer.object].geometryIndex];
				logger::warn("[DCLF] '{}' draws past its buffers: {} vertices x {} bytes needs {}, the slice holds {}; indices to {} need {}, the slice holds {}",
					tables.objectGeometry[shortBuffer.object] ? tables.objectGeometry[shortBuffer.object]->name.c_str() : "?", geometry.vertexCount, geometry.vertexStride,
					shortBuffer.vertexNeeded, geometry.vertexBytes, geometry.firstIndex + geometry.indexCount, shortBuffer.indexNeeded, geometry.indexBytes);
			}
		}
		a_stats.uploadBytes = bytes.size() + a_payload.records.size() * sizeof(DrawBindings) + a_payload.inputList.size() * sizeof(DrawInput) +
		                      geometryBytes + objectBytes;
		a_stats.records = a_payload.persistent ? a_payload.recordsHeld : static_cast<std::uint32_t>(a_payload.records.size());
		lap(6);
		// What the colour epoch drew: the native loop's skip set and the claims, from the build's changes alone. A build made
		// against another applied version than this one asks for every slot again.
		if (!depthOnly && a_payload.drawnValid) {
			if (a_payload.drawnFull || a_payload.drawnBase == drawnCommitted) {
				if (a_payload.drawnFull) {
					++drawnResyncs;
					// Slots past the full send's end were not known to the build: nothing draws them.
					for (std::uint32_t slot = static_cast<std::uint32_t>(a_payload.drawnChanges.size()); slot < slotDrawn.size(); ++slot)
						if (slotDrawn[slot].drawn)
							ApplyDrawn(slot, nullptr, false, frameNumber);
				}
				for (const auto& change : a_payload.drawnChanges)
					ApplyDrawn(change.slot, change.geometry, change.drawn, frameNumber);
				drawnCommitted = a_payload.drawnVersion;
				drawnResync = false;
			} else {
				drawnResync = true;
			}
		}
		if (!depthOnly)
			drawnCommitFrame = frameNumber;
		lap(5);

		// The resident region's draws and inputs lead the frame's own (ResidentRegion).
		const std::uint32_t drawCount = static_cast<std::uint32_t>(a_payload.sequences.size() + a_payload.residentDraws);
		const auto decalCount = depthOnly ? std::array<std::uint32_t, kDecalGroups>{} : a_payload.decalCount;
		// Inputs the culling dispatch covers. In the depth segment this exceeds drawCount, because that
		// segment submits a cull-only input for every candidate it is not allowed to draw.
		const std::uint32_t inputCount = static_cast<std::uint32_t>(a_payload.inputList.size() + (a_payload.resident.Count()));
		const auto& previousShape = a_resources->published[shapeIndex];
		auto frame = std::make_shared<PassFrame>();
		frame->drawCapacity = GrowCapacity(previousShape ? previousShape->drawCapacity : 0u, drawCount, kMaxDraws);
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			frame->decalCapacity[group] = GrowCapacity(previousShape ? previousShape->decalCapacity[group] : 0u, decalCount[group], kMaxDecalDraws);
		frame->width = a_capture.viewportWidth;
		frame->height = a_capture.viewportHeight;
		// Both epochs rasterise with the main pass's depth range; see Impl::mainMinDepth.
		const bool useMainRange = mainMaxDepth > 0.0f;
		frame->minDepth = useMainRange ? mainMinDepth : a_capture.minDepth;
		frame->maxDepth = useMainRange ? mainMaxDepth : a_capture.maxDepth;
		frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
		frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
		frame->indirect = GetIndirectState();
		frame->cullMode = ActiveToggles().cullMode;
		// The main pass's ViewProj (VS_PerFrame c8), which the draws project with: the culling has to
		// use the same matrix or it would reject what the draws would have put on screen.
		std::array<float, 16> viewProj{};
		bool hasViewProj = false;
		if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister]) {
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			if (contents.size() >= 48 * sizeof(float)) {
				std::memcpy(viewProj.data(), reinterpret_cast<const float*>(contents.data()) + 32, sizeof(float) * 16);
				hasViewProj = true;
			}
		}
		if (!loggedNoViewProj) {
			loggedNoViewProj = true;
			logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", frame->cullMode, hasViewProj ? "yes" : "no",
				kPerFrameVertexRegister, a_capture.vsBuffers[kPerFrameVertexRegister] ? "bound" : "not bound");
		}
		if (const auto pixel = SwitchValue(Switch::GBufferProbe); !pixel.empty()) {
			if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
				frame->probeX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
				frame->probeY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
				frame->probePixel = frame->probeX < frame->width && frame->probeY < frame->height;
			}
		}
		// The two epochs must rasterise into the same pixels for the colour pass's EQUAL test to have any
		// chance: a different viewport or depth range between the depth pass and the main pass puts the
		// same vertex on a different pixel, at a different depth.
		if ((depthOnly ? loggedDepthViewport : loggedColourViewport)++ % 480 == 0) {
			// The z row of the ViewProj each epoch actually packs: this is what turns a vertex into the
			// depth the test compares, so if the two epochs differ it shows up here.
			std::string viewProjZ = "(none)";
			{
				std::span<const std::byte> perFrame;
				if (replayVertexInputs)
					perFrame = prepassVS[kPerFrameVertexRegister];
				else if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister])
					perFrame = mirror.Contents(buffer);
				if (perFrame.size() >= 48 * sizeof(float)) {
					const auto* floats = reinterpret_cast<const float*>(perFrame.data());
					viewProjZ = fmt::format("({:.6f} {:.6f} {:.6f} {:.6f})", floats[40], floats[41], floats[42], floats[43]);
				}
			}
			logger::info("[DCLF] {} epoch: tables frame {} holding {} objects; render area {}x{}, depth range [{}, {}] (captured [{}, {}]), replay {}, eye ({:.2f} {:.2f} {:.2f}), ViewProj z row {}",
				depthOnly ? "z-prepass" : "colour", frameNumber, tables.liveObjects, frame->width, frame->height, frame->minDepth, frame->maxDepth,
				a_capture.minDepth, a_capture.maxDepth, replayVertexInputs ? "on" : "off", a_capture.eye.x, a_capture.eye.y, a_capture.eye.z, viewProjZ);
		}
		if (depthOnly) {
			prepassEye = a_capture.eye;
			prepassPreviousEye = a_capture.previousEye;
			prepassInputs = true;
		}
		a_stats.drawn = drawCount;
		++a_stats.commitEpochs;

		// The execution's values, into its slot of the latch (this runs inside the epoch, after the host
		// waited for the slot): what the BuildDraws dispatches of the segment read instead of push constants.
		BuildDrawsLatch latch{};
		latch.dispatch[0] = (inputCount + 63) / 64;
		latch.dispatch[1] = 1;
		latch.dispatch[2] = 1;
		latch.drawCount = inputCount;
		latch.cullFlags = (hasViewProj ? frame->cullMode : 0u) | 0x100u;  // RequireNativeVisible
		// The frame number, not the epoch: the depth segment publishes and the colour segment reads within one
		// frame, so the stamp has to be the thing they share.
		latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
		if (a_resources->hzb && frame->width && frame->height) {
			// Mip 0 covers twice its own size in source pixels, of which only the rendered area holds real
			// depth. A texture coordinate in the image scales by that ratio to reach the HZB.
			const auto scale = [](std::uint32_t a_rendered, std::uint32_t a_covered) {
				const double ratio = a_covered ? std::min(1.0, double(a_rendered) / double(a_covered)) : 0.0;
				return static_cast<std::uint32_t>(std::lround(ratio * 65535.0)) & 0xFFFFu;
			};
			latch.hzbUvScalePacked = scale(frame->width, a_resources->hzbWidth * 2) | (scale(frame->height, a_resources->hzbHeight * 2) << 16);
		}
		FoldEyeIntoViewProj(viewProj, a_capture.eye, latch.viewProj);
		// The depth segment: the camera the fade roots' distances are measured from (kObjectFadeTest).
		if (depthOnly) {
			const auto eye = PrimaryCull::Get().FadeEye();
			std::copy(eye.begin(), eye.end(), latch.fadeEye);
			const auto treeHeight = PrimaryCull::Get().TreeHeightTest();
			latch.treeHeight[0] = treeHeight[0];
			latch.treeHeight[1] = treeHeight[1];
		}
		// The colour pass: this frame's cascades, for the synthetic passes' sun test. The sun's Accumulate has run.
		if (!depthOnly) {
			latch.sunState = kSunTestOn | SunAccumulation::Get().GpuCascades(latch.sunMasks, latch.sunPlanes);
			sunUpload = latch;
			ArmFeedback(*a_resources, frameNumber, static_cast<std::uint32_t>(std::min<std::size_t>(tables.objects.size(), kMaxObjects)));
		}
		a_resources->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, latch);

		PublishShape(std::move(frame), a_resources->published[shapeIndex], a_resources->frames[shapeIndex], a_resources->shapeGenerations);
		lap(6);
		return true;
	}
}

#endif
