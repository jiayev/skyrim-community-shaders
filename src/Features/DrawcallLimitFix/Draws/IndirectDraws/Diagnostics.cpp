#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief The payload probes' comparison: byte for byte, naming the first difference by buffer and offset. */
		struct FirstDifference
		{
			std::string& out;

			/** @brief Whether the two ranges differ. */
			bool Bytes(const char* a_name, const void* a_lhs, std::size_t a_lhsBytes, const void* a_rhs, std::size_t a_rhsBytes)
			{
				if (a_lhsBytes != a_rhsBytes) {
					out = fmt::format("{}: {} vs {} bytes", a_name, a_lhsBytes, a_rhsBytes);
					return true;
				}
				if (!a_lhsBytes || std::memcmp(a_lhs, a_rhs, a_lhsBytes) == 0)
					return false;
				const auto* l = static_cast<const std::uint8_t*>(a_lhs);
				const auto* r = static_cast<const std::uint8_t*>(a_rhs);
				std::size_t k = 0;
				while (l[k] == r[k])
					++k;
				out = fmt::format("{}: byte {} of {} ({:#x} vs {:#x})", a_name, k, a_lhsBytes, l[k], r[k]);
				return true;
			}

			template <class Lhs, class Rhs>
			bool Vectors(const char* a_name, const Lhs& a_lhs, const Rhs& a_rhs)
			{
				using T = std::remove_cvref_t<decltype(a_lhs[0])>;
				return Bytes(a_name, a_lhs.data(), a_lhs.size() * sizeof(T), a_rhs.data(), a_rhs.size() * sizeof(T));
			}
		};
	}

	bool SamePayload(const MainPayload& a, const MainPayload& b, std::string& a_difference)
	{
		FirstDifference differ{ a_difference };
		auto bytesDiffer = [&](const char* a_name, const void* a_lhs, std::size_t a_lhsBytes, const void* a_rhs, std::size_t a_rhsBytes) {
			return differ.Bytes(a_name, a_lhs, a_lhsBytes, a_rhs, a_rhsBytes);
		};
		auto vectorDiffers = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) { return differ.Vectors(a_name, a_lhs, a_rhs); };
		auto rowsDiffer = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs, std::size_t a_stride) {
			return bytesDiffer(a_name, a_lhs.At(0), a_lhs.Count() * a_stride, a_rhs.At(0), a_rhs.Count() * a_stride);
		};
		if (rowsDiffer("material rows", a.materialRows, b.materialRows, sizeof(MaterialRow)) ||
			rowsDiffer("pipeline rows", a.pipelineRows, b.pipelineRows, sizeof(PipelineRow)) ||
			vectorDiffers("sequences", a.sequences, b.sequences) || vectorDiffers("inputs", a.inputList, b.inputList) ||
			vectorDiffers("geometries", a.geometryDraws.Flat(), b.geometryDraws.Flat()) || bytesDiffer("objects", a.objectRecords.At(0), a.objectRecords.Count() * sizeof(BindlessObject), b.objectRecords.At(0),
				b.objectRecords.Count() * sizeof(BindlessObject)) ||
			a.bones.Rows() != b.bones.Rows() || a.frameRegisters != b.frameRegisters ||
			a.drawnChanges.size() != b.drawnChanges.size())
			return false;
		for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
			if (vectorDiffers("decal templates", a.decalTemplates[group], b.decalTemplates[group]))
				return false;
		}
		if (a.decalCount != b.decalCount || a.skipped != b.skipped || a.missingVertexConstants != b.missingVertexConstants ||
			a.missingPixelConstants != b.missingPixelConstants || a.decalsDrawn != b.decalsDrawn || a.shortBuffers != b.shortBuffers ||
			a.deferredTextures != b.deferredTextures) {
			a_difference = "counts";
			return false;
		}
		return true;
	}

	bool SamePayload(const ShadowPayload& a, const ShadowPayload& b, std::string& a_difference)
	{
		FirstDifference differ{ a_difference };
		auto vectorDiffers = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) { return differ.Vectors(a_name, a_lhs, a_rhs); };
		// The object records by content: the worker's may be the store's.
		if (differ.Bytes("objects", a.objects.Count() ? a.objects.At(0) : nullptr, a.objects.Count() * sizeof(BindlessObject),
				b.objects.Count() ? b.objects.At(0) : nullptr, b.objects.Count() * sizeof(BindlessObject)))
			return false;
		if (vectorDiffers("constants", a.arena.Bytes(), b.arena.Bytes()) || differ.Bytes("material rows", a.materialRows.At(0), a.materialRows.Count() * sizeof(ShadowMaterialRow), b.materialRows.At(0),
				b.materialRows.Count() * sizeof(ShadowMaterialRow)) ||
			vectorDiffers("object records", a.objectRecord, b.objectRecord) || a.bones.Rows() != b.bones.Rows() ||
			vectorDiffers("geometries", a.geometries.Flat(), b.geometries.Flat()))
			return false;
		for (std::size_t m = 0; m < a.inputList.size(); ++m) {
			if (vectorDiffers("inputs", a.Flat(static_cast<std::uint32_t>(m)), b.Flat(static_cast<std::uint32_t>(m))))
				return false;
		}
		if (a.skippedTexture != b.skippedTexture || a.skippedPipeline != b.skippedPipeline || a.deferredTextures != b.deferredTextures ||
			a.deferredPipelines != b.deferredPipelines) {
			a_difference = "the skip counters";
			return false;
		}
		return true;
	}
}

namespace DCLF
{
	std::string IndirectDraws::AsyncReport()
	{
		std::string text = AsyncWorker::Get().Report();
		if (auto& rows = impl->mainRows; rows.builds) {
			text += fmt::format("[DCLF] main rows: {} builds; a build: {:.1f} material and {:.1f} pipeline rows written; {} material and {} pipeline rows held "
								"(tables {} and {} rows), {} resyncs\n",
				rows.builds, double(rows.materialsWritten) / rows.builds, double(rows.pipelinesWritten) / rows.builds, rows.material.Size(), rows.pipeline.Size(),
				impl->resources ? impl->resources->materialRows.capacity : 0u, impl->resources ? impl->resources->pipelineRows.capacity : 0u, rows.resyncs);
			rows.builds = rows.materialsWritten = rows.pipelinesWritten = rows.resyncs = 0;
		}
		if (auto* store = &impl->objectStore; store->updates) {
			text += fmt::format("[DCLF] persistent object records: {} updates, {:.1f} records rewritten an update, {} records held, {} resyncs, {} collisions; parity {} checked, {} differ{}\n",
				store->updates, static_cast<double>(store->rewritten) / store->updates, store->records.Size(), store->resyncs, store->collisions, store->parity.checks,
				store->parity.mismatches, store->collisions && store->parity.checks ? std::string(" <- DIFFER") : store->parity.Verdict(true));
			store->updates = store->rewritten = store->resyncs = store->collisions = 0;
			store->parity.Reset();
		}
		if (auto& k = impl->shadowKept; k.builds) {
			std::size_t entries = 0;
			for (const auto& mode : k.modes)
				entries += mode.inputs.Size();
			const auto* rowsTable = impl->shadow ? &impl->shadow->materialRows : nullptr;
			text += fmt::format(
				"[DCLF] persistent shadow state: {} builds, {:.1f} entries and {:.1f} material rows written a build, {} entries and {} material rows held "
				"(table {} rows, grown {} times), {} resyncs; parity {} inputs checked, {} differ{}\n",
				k.builds, static_cast<double>(k.entriesWritten) / k.builds, static_cast<double>(k.rowsWritten) / k.builds, entries, k.rows.Size(),
				rowsTable ? rowsTable->capacity : 0u, rowsTable ? rowsTable->growths : 0u, k.resyncs, k.parity.checks, k.parity.mismatches, k.parity.Verdict(true));
			if (impl->shadow)
				impl->shadow->materialRows.growths = 0;
			if (!k.missingBy.empty()) {
				std::string why;
				for (const auto& [reason, count] : k.missingBy)
					why += fmt::format("{}{} {} (first {})", why.empty() ? "" : ", ", reason, count, k.missingFirst[reason]);
				text += fmt::format("[DCLF] persistent shadow state: inputs of the per-frame build only, by why: {}\n", why);
				k.missingBy.clear();
				k.missingFirst.clear();
			}
			k.builds = k.entriesWritten = k.rowsWritten = k.resyncs = 0;
			k.parity.Reset();
		}
		if (auto* store = &impl->boneStore; store->updates) {
			text += fmt::format("[DCLF] persistent bone rows: {} updates, {} resyncs, {} capacity rows; parity {} checked, {} differ{}\n", store->updates, store->resyncs,
				store->capacity, store->parity.checks, store->parity.mismatches, store->parity.Verdict());
			store->updates = store->resyncs = 0;
			store->parity.Reset();
		}
		if (auto* store = &impl->geometryStore; store->updates) {
			text += fmt::format("[DCLF] persistent geometry table: {} updates, {:.2f} slots repacked an update, {} slots held, {} resyncs; parity {} checked, {} differ{}\n",
				store->updates, static_cast<double>(store->rewritten) / store->updates, store->packed.Size(), store->resyncs, store->parity.checks, store->parity.mismatches,
				store->parity.Verdict(true));
			store->updates = store->rewritten = store->resyncs = 0;
			store->parity.Reset();
		}
		if (auto& c = impl->sunExclusionCache; c.builds) {
			text += fmt::format("[DCLF] sun exclusion: {} builds, {} reused; parity {} checked, {} differ{}\n", c.builds, c.reused, c.parity.checks, c.parity.mismatches,
				c.parity.Verdict());
			c.builds = c.reused = 0;
			c.parity.Reset();
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.kicked && !a.notKicked && !a.builtInline && !a.leaked)
				continue;
			text += fmt::format("[DCLF] async {} epochs: {} used the worker's build, {} built inline ({} not kicked, {} stale, {} late, {} failed, {} cancelled), {} dropped, {} leaked; probe: {} compared, {} differ\n",
				kNames[i], a.used, a.builtInline, a.notKicked, a.stale, a.late, a.failed, a.cancelled, a.dropped, a.leaked, a.probeCompared, a.probeDiffer);
			if (i == kAsyncZPrepass && a.kicked)
				text += fmt::format("[DCLF] async zprepass eye: the predicted eye pair missed the captured one {} times ({} the previous eye only)\n", a.eyeMismatches, a.previousEyeMismatches);
			a = {};
		}
		AsyncWorker::Get().ResetStats();
		return text;
	}

	void IndirectDraws::Impl::ProbeGBuffer(const char* a_label)
	{
		(void)a_label;
		if (!resources || !resources->probeD3D11)
			return;
		auto* context = globals::d3d::context;
		if (gbufferStaging) {
			if (--gbufferFramesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(gbufferStaging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
				auto slots = [&](std::uint32_t a_base) {
					std::string text;
					for (std::uint32_t i = 0; i < gbufferCount; ++i) {
						const auto* texel = bytes + std::size_t(a_base + i) * kProbeSlotBytes;
						std::string hex;
						for (std::uint32_t b = 0; b < gbufferBytes[i]; ++b)
							hex += fmt::format("{:02X}", texel[b]);
						text += fmt::format("{}rt{}={}", text.empty() ? "" : " | ", i, hex);
					}
					return text;
				};
				auto depthAt = [&](std::uint32_t a_slot) {
					std::uint32_t packed = 0;
					std::memcpy(&packed, bytes + std::size_t(a_slot) * kProbeSlotBytes, sizeof(packed));
					return packed & 0x00FFFFFFu;
				};
				logger::info("[DCLF] G-buffer at ({}, {}) before DCLF's colour draws: {}", gbufferX, gbufferY, slots(0));
				logger::info("[DCLF] G-buffer at ({}, {}) after  DCLF's colour draws: {}", gbufferX, gbufferY, slots(kColorTargets));
				logger::info("[DCLF] depth at ({}, {}): {:06X} after the z-prepass, {:06X} when the colour pass tests it, {:06X} after it",
					gbufferX, gbufferY, depthAt(kProbeDepthAfterPrepass), depthAt(kProbeDepthBeforeColour), depthAt(kProbeDepthAfterColour));
				logger::info("[DCLF] depth gap at ({}, {}): {:06X} after sky occlusion, {:06X} after light culling",
					gbufferX, gbufferY, depthAt(kProbeDepthAfterSky), depthAt(kProbeDepthAfterLightCulling));
				context->Unmap(gbufferStaging.get(), 0);
			}
			gbufferStaging = nullptr;
			return;
		}
		if ((gbufferEpochs++ % 480) != 0)
			return;
		D3D11_BUFFER_DESC desc{};
		resources->probeD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Buffer> staging;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
			return;
		ScopedPerfEvent event("CS DCLF: G-buffer probe readback");
		context->CopyResource(staging.get(), resources->probeD3D11.get());
		gbufferStaging = std::move(staging);
		gbufferFramesLeft = 4;
		gbufferCount = probeTargetCount;
		for (std::uint32_t i = 0; i < gbufferCount; ++i) {
			D3D11_TEXTURE2D_DESC textureDesc{};
			if (probeTargets[i])
				probeTargets[i]->GetDesc(&textureDesc);
			gbufferBytes[i] = FormatBytes(textureDesc.Format);
		}
		if (const auto pixel = SwitchValue(Switch::GBufferProbe); !pixel.empty()) {
			if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
				gbufferX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
				gbufferY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
			}
		}
	}

	void IndirectDraws::Impl::CheckSetParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_depth, const MainPayload& a_colour)
	{
		auto* context = globals::d3d::context;
		auto& store = SceneStore::Get();
		auto& counts = setParity;
		auto stateName = [](std::uint8_t a_state) -> std::string {
			if (a_state == kObjectStateDrawable)
				return "drawable";
			if (a_state == kObjectStateDecal)
				return "decal";
			if (a_state == kObjectStateAbsent)
				return "absent";
			return a_state < kSkipNames.size() ? std::string("skipped: ") + kSkipNames[a_state] : fmt::format("state {}", a_state);
		};

		// The oldest snapshot, once its frame's copy has had time to land.
		for (auto& waiting : setParityFrames)
			if (waiting.framesLeft)
				--waiting.framesLeft;
		while (!setParityFrames.empty() && setParityFrames.front().framesLeft == 0) {
			auto snapshot = std::move(setParityFrames.front());
			setParityFrames.pop_front();
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(snapshot.staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				++counts.skipped;
				continue;
			}
			const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
			const std::uint32_t stamp = snapshot.frame & 0x0FFFFFFFu;
			bool damaged = false;
			for (std::size_t o = 0; o < snapshot.flags.size(); ++o) {
				const std::uint32_t word = words[o];
				const bool current = (word >> 4) == stamp;
				const std::uint32_t verdict = current ? (word & 3) : ~0u;
				const bool depthDrawn = current && (word & 4);
				const bool colourDrawn = current && (word & 8);
				const std::uint8_t flags = snapshot.flags[o];
				const bool withheld = flags & 1;
				const bool alpha = flags & 4;
				counts.depthDrawnTotal += depthDrawn;
				counts.colourDrawnTotal += colourDrawn;
				const char* kind = nullptr;
				const bool decal = o < snapshot.colourState.size() && snapshot.colourState[o] == kObjectStateDecal;
				// A decal is drawn by the colour segment's decal pass only (the depth segment never draws one), so it
				// is only checked for being withheld and undrawn.
				if (!decal && depthDrawn && !colourDrawn) {
					kind = "depth without colour";
					++counts.depthOnly;
					counts.alphaDepthOnly += alpha;
				} else if (!decal && colourDrawn && !depthDrawn) {
					kind = verdict == 3 ? "colour without depth (no verdict this frame)" : "colour without depth";
					++counts.colourOnly;
					counts.colourUnpublished += verdict == 3;
					counts.alphaColourOnly += alpha;
				} else if (withheld && !colourDrawn) {
					if (verdict == 0 || verdict == 2) {
						++counts.withheldCulled;  // the GPU culling rejected it: not drawn by anyone, as intended
					} else {
						kind = "withheld natively, drawn by nobody";
						++counts.withheldUndrawn;
						counts.alphaWithheldUndrawn += alpha;
					}
				}
				// The gap detector, by geometry (object indices are rebuilt every frame).
				if (const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr) {
					const bool kept = flags & 2;
					const std::uint8_t state = !kept ? 0 : (withheld && !colourDrawn) ? static_cast<std::uint8_t>(2 | (verdict << 4)) : 1;
					auto& history = gapHistory[geometry];
					if (history.frame + 1 == snapshot.frame && state == 1 && (history.last & 3) == 2 && history.before == 1) {
						++counts.gaps;
						const std::uint32_t gapVerdict = history.last >> 4;
						counts.gapsRetest += gapVerdict == 0;
						counts.gapsRejected += gapVerdict == 2;
						const RE::TESObjectREFR* owner = nullptr;
						for (const RE::NiAVObject* node = geometry; node && !owner; node = node->parent)
							owner = node->GetUserData();
						const auto* base = owner ? owner->GetBaseObject() : nullptr;
						const bool tree = base && base->GetFormType() == RE::FormType::Tree;
						counts.gapsTree += tree;
						if (counts.gapSamples++ < 12)
							logger::info("[DCLF] set parity, frame {}: one-frame gap - '{}' ({}{}), culled in frame {} with verdict {}", snapshot.frame,
								geometry->name.c_str() ? geometry->name.c_str() : "?", base ? RE::FormTypeToString(base->GetFormType()) : "no ref",
								geometry->GetGeometryRuntimeData().skinInstance ? ", skinned" : "", snapshot.frame - 1,
								gapVerdict == 0 ? "occluded (retest)" : gapVerdict == 2 ? "rejected" : "other");
					}
					history.before = history.frame + 1 == snapshot.frame ? history.last : 0;
					history.last = state;
					history.frame = snapshot.frame;
				}
				if (!kind)
					continue;
				damaged = true;
				if (counts.samples++ < 40) {
					const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
					const bool alive = geometry && store.IsTracked(geometry);
					static constexpr const char* kVerdicts[] = { "occluded (retest)", "visible", "rejected", "no verdict" };
					logger::info("[DCLF] set parity, frame {}: {} - object {} '{}'{}: verdict {}, depth build {}, colour build {}, {}{}{}", snapshot.frame, kind, o,
						alive ? geometry->name.c_str() : "?", alpha ? " (alpha tested)" : "", current ? kVerdicts[verdict] : "not this frame's",
						stateName(o < snapshot.depthState.size() ? snapshot.depthState[o] : kObjectStateAbsent),
						stateName(o < snapshot.colourState.size() ? snapshot.colourState[o] : kObjectStateAbsent),
						(flags & 2) ? "member" : "not bound", (flags & 8) ? ", claimed" : "", withheld ? ", withheld" : "");
				}
			}
			context->Unmap(snapshot.staging.get(), 0);
			setParityStaging.push_back(std::move(snapshot.staging));
			++counts.frames;
			counts.framesWithDamage += damaged;
			if (counts.frames == 300) {
				logger::info("[DCLF] set parity over {} frames ({} with damage, {} unread): depth without colour {} ({} alpha tested), colour without depth {} ({} alpha tested, {} with no verdict), withheld and drawn by nobody {} ({} alpha tested); withheld and GPU-culled {}; per frame {:.0f} depth draws, {:.0f} colour draws",
					counts.frames, counts.framesWithDamage, counts.skipped, counts.depthOnly, counts.alphaDepthOnly, counts.colourOnly, counts.alphaColourOnly,
					counts.colourUnpublished, counts.withheldUndrawn, counts.alphaWithheldUndrawn, counts.withheldCulled, double(counts.depthDrawnTotal) / counts.frames,
					double(counts.colourDrawnTotal) / counts.frames);
				if (counts.gaps)
					logger::info("[DCLF] set parity over {} frames: {} one-frame gaps (kept, drawn, withheld and GPU-culled, drawn again): {} occluded (retest), {} rejected; {} of them trees",
						counts.frames, counts.gaps, counts.gapsRetest, counts.gapsRejected, counts.gapsTree);
				counts = {};
				// Forget geometries not seen for a while, so the history does not keep every object ever drawn.
				std::erase_if(gapHistory, [&](const auto& a_entry) { return a_entry.second.frame + 8 < snapshot.frame; });
			}
		}

		// This frame: the words as the colour epoch left them, and the CPU side that explains them. Only when
		// the depth build is this frame's too.
		if (!a_resources->visibilityD3D11 || a_depth.inputs.frameNumber != a_colour.inputs.frameNumber || setParityFrames.size() >= 8)
			return;
		SetParityFrame snapshot;
		D3D11_BUFFER_DESC desc{};
		a_resources->visibilityD3D11->GetDesc(&desc);
		// A released staging of another size (the visibility buffer grew since) is dropped.
		while (!setParityStaging.empty() && !snapshot.staging) {
			D3D11_BUFFER_DESC held{};
			setParityStaging.back()->GetDesc(&held);
			if (held.ByteWidth == desc.ByteWidth)
				snapshot.staging = std::move(setParityStaging.back());
			setParityStaging.pop_back();
		}
		if (!snapshot.staging) {
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, snapshot.staging.put())))
				return;
		}
		context->CopyResource(snapshot.staging.get(), a_resources->visibilityD3D11.get());
		snapshot.frame = a_colour.inputs.frameNumber;
		snapshot.framesLeft = 3;
		snapshot.depthState = a_depth.objectState;
		snapshot.colourState = a_colour.objectState;
		const auto& tables = store.GetTables();
		const auto claims = PassCapture::Get().CurrentClaims();
		const std::size_t objects = tables.objects.size();
		// Left out of the engine's cull or registration this frame (PrimaryCull): the native loop does not draw it.
		const auto& stoodIn = PrimaryCull::Get().StoodInMembers();
		const ankerl::unordered_dense::set<const RE::BSGeometry*> leftOut(stoodIn.begin(), stoodIn.end());
		snapshot.flags.assign(objects, 0);
		snapshot.geometry.assign(objects, nullptr);
		for (std::size_t o = 0; o < objects && o < tables.objectGeometry.size(); ++o) {
			const auto* geometry = tables.objectGeometry[o];
			snapshot.geometry[o] = geometry;
			const auto objectFlags = tables.objects[o].flags;
			std::uint8_t flags = 0;
			if (objectFlags & kObjectMember)
				flags |= 2;
			if (objectFlags & kObjectAlphaTest)
				flags |= 4;
			if (geometry && claims && claims->contains(geometry)) {
				flags |= 8;
				if (leftOut.contains(geometry))
					flags |= 1;
			}
			snapshot.flags[o] = flags;
		}
		setParityFrames.push_back(std::move(snapshot));
	}

	void IndirectDraws::Impl::ReadCullCounters(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats, const MainPayload& a_payload)
	{
		auto* context = globals::d3d::context;
		if (cullReadback) {
			if (--cullReadback->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(cullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullOccluded = words[6];
				a_stats.hzbNear = words[7];
				a_stats.hzbFar = words[8];
				a_stats.hzbSampled = words[9];
				if (words[10]) {
					a_stats.hzbSample.valid = true;
					std::memcpy(&a_stats.hzbSample.farthest, &words[11], sizeof(float));
					std::memcpy(&a_stats.hzbSample.nearestZ, &words[12], sizeof(float));
					a_stats.hzbSample.uvMin[0] = (words[13] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMin[1] = (words[13] >> 16) / 65535.0f;
					a_stats.hzbSample.uvMax[0] = (words[14] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMax[1] = (words[14] >> 16) / 65535.0f;
					a_stats.hzbSample.mip = words[15];
				} else {
					a_stats.hzbSample.valid = false;
				}
				a_stats.cullDrawnPhaseTwo = words[17];
				a_stats.cullRescuedByPhaseTwo = words[18];
				a_stats.decalsCulled = words[kCountDecalsCulledWord];
				a_stats.decalsTested = words[kCountDecalsTestedWord];
				a_stats.sunTested = words[kCountSunTestedWord];
				a_stats.sunMissed = words[kCountSunMissedWord];
				a_stats.fadeTested = words[kCountFadeTestedWord];
				a_stats.fadeHidden = words[kCountFadeHiddenWord];
				a_stats.sunCpuTested = cullReadback->sunCpuTested;
				a_stats.sunCpuMissed = cullReadback->sunCpuMissed;
				context->Unmap(cullReadback->count.get(), 0);
			}
			cullReadback.reset();
			return;
		}
		if ((cullEpochs++ % 120) != 0 || !a_resources->countD3D11)
			return;
		D3D11_BUFFER_DESC desc{};
		a_resources->countD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		ScopedPerfEvent event("CS DCLF: culling readback");
		context->CopyResource(readback.count.get(), a_resources->countD3D11.get());
		readback.framesLeft = 3;
		// The CPU's side of the sun test, over the same frame's colour inputs and the same planes.
		auto sunTest = [&](const DrawInput& input) {
			if (!(input.flags & kObjectSunTest) || !(sunUpload.sunState & kSunTestOn))
				return;
			++readback.sunCpuTested;
			bool inside = false;
			for (std::size_t c = 0; c < sunCascades.size() && !inside; ++c)
				inside = InSunCascade(sunCascades[c], input.boundCentre, input.boundRadius);
			readback.sunCpuMissed += inside ? 0 : 1;
		};
		if (a_payload.resident.elements)
			for (const auto& input : *a_payload.resident.elements)
				sunTest(input);
		for (const auto& input : a_payload.inputList)
			sunTest(input);
		cullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::CheckBuildParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_payload, IndirectDraws::Stats& a_stats)
	{
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
			++a_stats.buildParityChecks;
			logger::info("[DCLF] BuildDraws local shadow lights {}: {} draws, {} differ from the CPU's selection{}{}", localDiffering ? "MISMATCH" : "OK",
				localChecked, localDiffering, localFirst.empty() ? "" : "; first: ", localFirst);
			if (localDiffering)
				++a_stats.buildParityMismatches;
			if (decalSlots)
				logger::info("[DCLF] BuildDraws decal parity {}: {} slots, {} culled, {} differ", decalDiffering ? "MISMATCH" : "OK", decalSlots, decalCulled, decalDiffering);
			if (decalDiffering)
				++a_stats.buildParityMismatches;
			if (!differing && !missing) {
				logger::info("[DCLF] BuildDraws parity OK: {} of {} sequences match the CPU templates ({} rejected by the culling)", count, expected.size(),
					expected.size() - count);
			} else {
				++a_stats.buildParityMismatches;
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
				readback.expected.insert(readback.expected.end(), sequences.begin() + first, sequences.begin() + sequence);
			}
		}
		// The CPU's local shadow selection for every object the epoch drew, with the volumes it uploaded (none on the Z-prepass).
		if (!a_payload.inputs.depthOnly) {
			const auto& tables = SceneStore::Get().GetTables();
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || input.objectIndex >= tables.objects.size() || input.objectIndex >= tables.objectGeometry.size())
					continue;
				const auto* geometry = tables.objectGeometry[input.objectIndex];
				const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				const float center[3]{ input.boundCentre[0], input.boundCentre[1], input.boundCentre[2] };
				readback.expectedLocalShadows[input.objectIndex] = localShadows.MaskOf(property, center, input.boundRadius) & 0xFu;
			}
		}
		readback.framesLeft = 3;
		if (readback.sequences && readback.count)
			parity = std::move(readback);
	}
}

#endif
