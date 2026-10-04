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
			vectorDiffers("geometries", a.geometryDraws.Flat(), b.geometryDraws.Flat()) || a.frameRegisters != b.frameRegisters)
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
		if (vectorDiffers("constants", a.arena.Bytes(), b.arena.Bytes()) || differ.Bytes("material rows", a.materialRows.At(0), a.materialRows.Count() * sizeof(ShadowMaterialRow), b.materialRows.At(0),
				b.materialRows.Count() * sizeof(ShadowMaterialRow)) ||
			vectorDiffers("object records", a.objectRecord, b.objectRecord) ||
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
		std::string text = AsyncWorker::Get().Report() + AsyncWorker::Get().RenderWaitReport();
		if (impl->scene && impl->scene->fadeWriteBack) {
			auto& writeBack = *impl->scene->fadeWriteBack;
			if (writeBack.applied || writeBack.stale || writeBack.late) {
				text += fmt::format("[DCLF] fade write-back: {} milestones written onto stood-in roots' nodes, {} stale (the root listed again or no longer stood in), "
									"{} joins late (their rest carried over); event list {} events\n",
					writeBack.applied, writeBack.stale, writeBack.late, writeBack.capacity.load(std::memory_order_relaxed));
				writeBack.applied = writeBack.stale = writeBack.late = 0;
			}
		}
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
			std::string causes;
			for (std::uint32_t c = 0; c < kChangeCauseCount; ++c)
				if (store->byCause[c])
					causes += fmt::format("{}{} {}", causes.empty() ? "" : ", ", kChangeCauseNames[c], store->byCause[c]);
			text += fmt::format("[DCLF] object record updates by what changed: {} only placements and palettes, {} structural{}{}\n", store->streamOnly,
				store->structural, causes.empty() ? "" : " (by cause: ", causes.empty() ? "" : causes + ")");
			store->streamOnly = store->structural = 0;
			store->byCause = {};
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
		for (auto* cache : { &impl->sunExclusionCache, &impl->parabolicExclusionCache }) {
			if (auto& c = *cache; c.builds) {
				text += fmt::format("[DCLF] {} exclusion: {} builds, {} reused; parity {} checked, {} differ{}\n", cache == &impl->sunExclusionCache ? "sun" : "paraboloid",
					c.builds, c.reused, c.parity.checks, c.parity.mismatches, c.parity.Verdict());
				c.builds = c.reused = 0;
				c.parity.Reset();
			}
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.kicked && !a.notKicked && !a.builtInline && !a.leaked)
				continue;
			text += fmt::format("[DCLF] async {} epochs: {} used the worker's build, {} built inline ({} not kicked, {} stale ({} on the lookups), {} late, {} failed, {} cancelled), {} dropped, {} leaked; probe: {} compared, {} differ\n",
				kNames[i], a.used, a.builtInline, a.notKicked, a.stale, a.staleLookups, a.late, a.failed, a.cancelled, a.dropped, a.leaked, a.probeCompared, a.probeDiffer);
			if (i == kAsyncShadow && a.earlyKicked)
				text += fmt::format("[DCLF] async shadow early: {} kicked at the end of the scene phase, {} kept at BeforeShadowMaps, {} kicked again ({} change logs, {} shared or feature data, {} other inputs)\n",
					a.earlyKicked, a.earlyKept, a.earlyRekicked, a.earlyRekickedBy[0], a.earlyRekickedBy[1], a.earlyRekickedBy[2]);
			if (i == kAsyncColour && a.earlyKicked)
				text += fmt::format("[DCLF] async colour early: {} kicked at EarlyPrepass, {} kept at Prepass, {} kicked again (tables' versions {}, material records {}, lookups {})\n",
					a.earlyKicked, a.earlyKept, a.earlyRekicked, a.earlyRekickedBy[0], a.earlyRekickedBy[1], a.earlyRekickedBy[2]);
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
				counts.recordDisagrees += (flags & 8) ? 1 : 0;
				if (!withheld && (depthDrawn || colourDrawn)) {
					++counts.outsideDrawn;
					if (counts.samples++ < 40) {
						const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
						logger::info("[DCLF] set parity, frame {}: drawn outside the set - object {} '{}' ({}{})", snapshot.frame, o,
							geometry && store.IsTracked(geometry) ? geometry->name.c_str() : "?", depthDrawn ? "depth" : "", colourDrawn ? " colour" : "");
					}
				}
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
						kind = "in the set, drawn by nobody";
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
						// The snapshot is frames old: a geometry released since (a teleport, a cell unloading) is not read.
						const bool alive = store.IsTracked(geometry);
						const RE::TESObjectREFR* owner = nullptr;
						for (const RE::NiAVObject* node = alive ? geometry : nullptr; node && !owner; node = node->parent)
							owner = node->GetUserData();
						const auto* base = owner ? owner->GetBaseObject() : nullptr;
						const bool tree = base && base->GetFormType() == RE::FormType::Tree;
						counts.gapsTree += tree;
						if (counts.gapSamples++ < 12)
							logger::info("[DCLF] set parity, frame {}: one-frame gap - '{}' ({}{}), culled in frame {} with verdict {}", snapshot.frame,
								alive && geometry->name.c_str() ? geometry->name.c_str() : "?", base ? RE::FormTypeToString(base->GetFormType()) : "no ref",
								alive && geometry->GetGeometryRuntimeData().skinInstance ? ", skinned" : "", snapshot.frame - 1,
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
						(flags & 2) ? "bound" : "not bound", withheld ? ", in the set" : "", (flags & 8) ? ", RECORD DISAGREES" : "");
				}
			}
			context->Unmap(snapshot.staging.get(), 0);
			setParityStaging.push_back(std::move(snapshot.staging));
			++counts.frames;
			counts.framesWithDamage += damaged;
			if (counts.frames == 300) {
				logger::info("[DCLF] set parity over {} frames ({} with damage, {} unread): depth without colour {} ({} alpha tested), colour without depth {} ({} alpha tested, {} with no verdict), in the set and drawn by nobody {} ({} alpha tested); in the set and GPU-culled {}; drawn outside the set {}; records disagreeing with the set {}; per frame {:.0f} depth draws, {:.0f} colour draws{}",
					counts.frames, counts.framesWithDamage, counts.skipped, counts.depthOnly, counts.alphaDepthOnly, counts.colourOnly, counts.alphaColourOnly,
					counts.colourUnpublished, counts.withheldUndrawn, counts.alphaWithheldUndrawn, counts.withheldCulled, counts.outsideDrawn, counts.recordDisagrees,
					double(counts.depthDrawnTotal) / counts.frames, double(counts.colourDrawnTotal) / counts.frames,
					counts.depthOnly || counts.colourOnly || counts.withheldUndrawn || counts.outsideDrawn || counts.recordDisagrees ? " <- SET PARITY" : " <- OK");
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
		const std::size_t objects = tables.objects.size();
		// The frame's set: the engine withholds every member from the main camera's views, so one the GPU culling kept and
		// neither segment drew is drawn by nobody.
		snapshot.flags.assign(objects, 0);
		snapshot.geometry.assign(objects, nullptr);
		for (std::size_t o = 0; o < objects && o < tables.objectGeometry.size(); ++o) {
			const auto* geometry = tables.objectGeometry[o];
			snapshot.geometry[o] = geometry;
			const auto objectFlags = tables.objects[o].flags;
			std::uint8_t flags = 0;
			if (store.SetPhasesOf(static_cast<std::int32_t>(o)) & kSetMain)
				flags |= 1;
			if (store.IsMember(static_cast<std::int32_t>(o)))
				flags |= 2;
			if (objectFlags & kObjectAlphaTest)
				flags |= 4;
			// The record the builds read must say the same (kObjectMember is the set's bit).
			if (((objectFlags & kObjectMember) != 0) != ((flags & 1) != 0))
				flags |= 8;
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
				if (scene)
					a_stats.fadeRoots = scene->fadeRootCount;
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
				inside = InSunCascade(sunCascades[c], SceneStore::Get().GetTables().objects[input.objectIndex].boundCenter, SceneStore::Get().GetTables().objects[input.objectIndex].boundRadius);
			readback.sunCpuMissed += inside ? 0 : 1;
		};
		if (a_payload.resident.elements)
			for (const auto& input : *a_payload.resident.elements)
				sunTest(input);
		for (const auto& input : a_payload.inputList)
			sunTest(input);
		cullReadback = std::move(readback);
	}

	namespace
	{
		// FUN_140438950(x, 3), as TreeWindCS.hlsl evaluates it.
		float TreeGust(float a_x)
		{
			const float factors[4] = { 3.14159274f, 9.42477798f, 15.7079639f, 21.9911499f };
			float lanes[4];
			for (int i = 0; i < 4; ++i) {
				float v = a_x * factors[i];
				v = v - std::nearbyint(v * 0.159154937f) * 6.28318548f;
				const float v2 = v * v, v3 = v2 * v;
				lanes[i] = ((-0.166521862f * v3 + v) + 0.00819991343f * (v3 * v2) + -0.000161475939f * (v3 * v2 * v2)) * 0.25f;
			}
			return (lanes[3] + lanes[2]) + (lanes[1] + lanes[0]);
		}

		float TreeFastSqrt(float a_value)
		{
			const auto bits = std::bit_cast<std::int32_t>(a_value);
			const float estimate = std::bit_cast<float>(static_cast<std::uint32_t>(0x5f3759df - (bits >> 1)));
			return (1.5f - a_value * 0.5f * estimate * estimate) * estimate * a_value;
		}
	}

	void IndirectDraws::Impl::ReadTreeWind(const std::shared_ptr<Resources>& a_resources)
	{
		auto* context = globals::d3d::context;
		const auto& tables = SceneStore::Get().GetTables();
		if (treeReadback) {
			if (--treeReadback->framesLeft)
				return;
			auto readback = std::move(*treeReadback);
			treeReadback.reset();
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(readback.records.get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return;
			const auto* bytes = static_cast<const std::byte*>(mapped.pData);
			const auto& in = readback.inputs;
			std::uint32_t checked = 0, paramsDiffer = 0, fadeDiffer = 0, gustDiffer = 0, gusts = 0;
			float drift = 0.0f;
			std::string first;
			// The engine's clock rate against the GPU's, per node, since the last readback that saw it: by distance (within the
			// manager's range or not) and whether the model has bones (+0xB8, the near loop's condition).
			std::array<std::array<std::uint32_t, 4>, 4> rates{};  // [near * 2 + bones][ratio ~0, ~1, ~2, other]
			ankerl::unordered_dense::map<const void*, std::pair<float, float>> seen;
			for (std::size_t i = 0; i < readback.samples.size(); ++i) {
				const auto& sample = readback.samples[i];
				float tree[8];
				std::memcpy(tree, bytes + i * sizeof(tree), sizeof(tree));
				++checked;
				const bool nodeless = sample.tree == kNodelessTree;
				// The tree's static row, while the node is still listed under the same slot.
				const TreeStatic* row = !nodeless && sample.tree < tables.trees.size() && tables.treeNode[sample.tree] == sample.node ? &tables.trees[sample.tree] : nullptr;
				const float leafFrequency = nodeless ? 1.0f : row ? row->leafFrequency : tree[3];
				if (tree[0] != 0.0f || tree[1] != in.windMagnitude || tree[3] != leafFrequency) {
					if (paramsDiffer++ == 0 && first.empty())
						first = fmt::format("object {} TreeParams ({}, {}, {}, {}), wind magnitude {}, leaf frequency {}", sample.object, tree[0], tree[1], tree[2], tree[3],
							in.windMagnitude, leafFrequency);
				}
				const float amplitude = tree[7];
				const float distance = nodeless ? 0.0f : TreeFastSqrt(tree[6]);
				const float faded = std::min(std::max((1.0f - (distance - in.fadeStart) / (in.fadeEnd - in.fadeStart)) * amplitude, 0.0f), amplitude);
				if (std::abs(faded - tree[2]) > 1e-5f * std::max(1.0f, std::abs(faded))) {
					if (fadeDiffer++ == 0 && first.empty())
						first = fmt::format("object {} faded amplitude {} (expected {} from amplitude {} at distance {})", sample.object, tree[2], faded, amplitude, distance);
				}
				if (row && row->animated && in.timerScale != 0.0f && tree[6] < in.maxDistance2 && tree[4] != row->timer * in.timerScale) {
					++gusts;
					const float timer = tree[4] / in.timerScale;
					const float gust = TreeGust(in.windSpeed * timer) * row->modelAmplitude;
					if (std::abs(gust - amplitude) > 1e-3f * std::max(1.0f, std::abs(row->modelAmplitude))) {
						if (gustDiffer++ == 0 && first.empty())
							first = fmt::format("object {} amplitude {} (the gust at timer {} is {})", sample.object, amplitude, timer, gust);
					}
					const float engineTimer = *reinterpret_cast<const float*>(static_cast<const std::byte*>(sample.node) + 0x164);
					drift = std::max(drift, std::abs(timer - engineTimer));
					if (const auto it = treeTimers.find(sample.node); it != treeTimers.end() && timer > it->second.second) {
						const float ratio = (engineTimer - it->second.first) / (timer - it->second.second);
						const auto* holder = *reinterpret_cast<const std::byte* const*>(static_cast<const std::byte*>(sample.node) + 0xF8);
						const auto* model = holder ? *reinterpret_cast<const std::byte* const*>(holder + 0x40) : nullptr;
						const bool bones = model && *reinterpret_cast<const void* const*>(model + 0xB8);
						const std::size_t bucket = std::abs(ratio) < 0.1f ? 0 : std::abs(ratio - 1.0f) < 0.1f ? 1 : std::abs(ratio - 2.0f) < 0.1f ? 2 : 3;
						++rates[(tree[6] < in.maxDistance2 ? 2 : 0) + (bones ? 1 : 0)][bucket];
					}
					seen[sample.node] = { engineTimer, timer };
				}
			}
			context->Unmap(readback.records.get(), 0);
			treeTimers = std::move(seen);
			logger::info("[DCLF] tree wind clock rates (engine over GPU; ~0/~1/~2/other): far {}/{}/{}/{}, far with bones {}/{}/{}/{}, near {}/{}/{}/{}, near with bones {}/{}/{}/{}",
				rates[0][0], rates[0][1], rates[0][2], rates[0][3], rates[1][0], rates[1][1], rates[1][2], rates[1][3], rates[2][0], rates[2][1], rates[2][2], rates[2][3],
				rates[3][0], rates[3][1], rates[3][2], rates[3][3]);
			logger::info("[DCLF] tree wind parity: {} members checked ({} gusts); {} TreeParams, {} fades, {} gusts differ; timers {:.3f} s from the engine's own clocks at most{}{}{}",
				checked, gusts, paramsDiffer, fadeDiffer, gustDiffer, drift, paramsDiffer || fadeDiffer || gustDiffer ? " <- TREE WIND" : " <- OK", first.empty() ? "" : "; first: ", first);
			return;
		}
		auto& buffers = *a_resources->scene;
		// Both wind buffers whole, every listed tree's entry: non-finite values, an amplitude far past its model's, or a
		// generation that is not its listing's (a draw then falls back to its record).
		{
			struct WholeReadback
			{
				std::array<winrt::com_ptr<ID3D11Buffer>, 2> staging;
				std::uint32_t framesLeft = 0, entries = 0, frame = 0;
				std::vector<std::pair<std::uint32_t, std::uint32_t>> members;  // object, tree
				std::vector<TreeStatic> rows;
				std::vector<std::uint32_t> generations;
				std::vector<float> engineAmplitude;  // per tree slot: the node's +0x15C when the copy was taken
				std::vector<std::uint8_t> nearList;   // per tree slot: on the manager's near list (+0x38) when the copy was taken
				std::uint32_t nearCount = 0, nearListed = 0;
				float nearLargestModel = 0.0f;
			};
			static std::optional<WholeReadback> whole;
			if (whole) {
				if (--whole->framesLeft == 0) {
					std::string text;
					for (std::uint32_t h = 0; h < 2; ++h) {
						D3D11_MAPPED_SUBRESOURCE mapped{};
						if (!whole->staging[h] || FAILED(context->Map(whole->staging[h].get(), 0, D3D11_MAP_READ, 0, &mapped)))
							continue;
						const auto* rows = static_cast<const float*>(mapped.pData);
						std::uint32_t bad = 0, stale = 0, checked = 0;
						float largest = 0.0f;
						std::string first, worst;
						for (const auto& [object, tree] : whole->members) {
							const std::uint32_t entry = tree == kNodelessTree ? 0u : tree + 1;
							if (entry >= whole->entries)
								continue;
							const float* e = rows + std::size_t(entry) * kTreeWindEntryRows * 4;
							++checked;
							const std::uint32_t generation = std::bit_cast<std::uint32_t>(e[8]);
							const bool listed = tree != kNodelessTree && tree < whole->generations.size();
							if (listed && generation != whole->generations[tree])
								++stale;
							const float model = listed && tree < whole->rows.size() ? std::abs(whole->rows[tree].modelAmplitude) : 0.0f;
							if (std::abs(e[2]) > largest) {
								largest = std::abs(e[2]);
								worst = fmt::format("object {} tree {:#x}: on the engine's near list {}, faded {} amplitude {}, listing amplitude {}, the engine's now {}, model amplitude {}, animated {}, distance {:.0f}",
									object, tree, listed && tree < whole->nearList.size() ? whole->nearList[tree] : 0u, e[2], e[7], listed && tree < whole->rows.size() ? whole->rows[tree].amplitude : 0.0f,
									listed && tree < whole->engineAmplitude.size() ? whole->engineAmplitude[tree] : 0.0f, model,
									listed && tree < whole->rows.size() ? whole->rows[tree].animated : 0u, std::sqrt(std::max(e[6], 0.0f)));
							}
							bool finite = true;
							for (int k = 0; k < 8; ++k)
								finite = finite && std::isfinite(e[k]);
							if (!finite || std::abs(e[2]) > 1000.0f || std::abs(e[7]) > 1000.0f) {
								if (bad++ == 0)
									first = fmt::format("object {} tree {:#x}: params ({} {} {} {}), timers ({} {} {} {}), generation {:#x} (listing {:#x}), model amplitude {}", object, tree,
										e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], generation, listed ? whole->generations[tree] : 0u, model);
							}
						}
						context->Unmap(whole->staging[h].get(), 0);
						text += fmt::format("; buffer {}: {} entries checked, {} bad, {} with another generation, largest amplitude {}{}{}", h, checked, bad, stale, largest,
							first.empty() ? "" : " (first " + first + ")", worst.empty() ? "" : " (largest: " + worst + ")");
					}
					logger::info("[DCLF] tree wind buffers (frame {}, read index {}): the engine's near list holds {} nodes, {} of them DCLF's trees, their largest model amplitude {}{}",
						whole->frame, whole->frame + 1, whole->nearCount, whole->nearListed, whole->nearLargestModel, text);
					whole.reset();
				}
			} else if ((treeEpochs % 120) == 60 && buffers.treeWind && !tables.treeObjects.empty() && buffers.treesHeld == tables.treesVersion) {
				WholeReadback next;
				next.entries = buffers.treeCapacity + 1;
				next.frame = buffers.treeFrame;
				for (const auto& member : tables.treeObjects)
					next.members.emplace_back(member.object, member.tree);
				next.rows = tables.trees;
				next.generations.reserve(tables.trees.size());
				for (const auto& row : tables.trees)
					next.generations.push_back(row.generation);
				{
					// The manager's near list (+0x38, count +0x48), FUN_140437e50's selection this frame.
					const auto manager = *reinterpret_cast<const std::uintptr_t*>(REL::Offset(0x20F6A18).address());
					ankerl::unordered_dense::set<const void*> nearNodes;
					if (manager) {
						const auto* nodes = *reinterpret_cast<const void* const* const*>(manager + 0x38);
						const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(manager + 0x48);
						for (std::uint32_t n = 0; nodes && n < count; ++n)
							nearNodes.insert(nodes[n]);
						next.nearCount = count;
					}
					next.nearList.assign(tables.trees.size(), 0);
					for (std::size_t t = 0; t < tables.trees.size(); ++t) {
						const void* node = t < tables.treeNode.size() ? tables.treeNode[t] : nullptr;
						if (node && nearNodes.contains(node)) {
							next.nearList[t] = 1;
							++next.nearListed;
							next.nearLargestModel = std::max(next.nearLargestModel, std::abs(tables.trees[t].modelAmplitude));
						}
					}
				}
				next.engineAmplitude.reserve(tables.trees.size());
				for (std::size_t t = 0; t < tables.trees.size(); ++t) {
					const void* node = t < tables.treeNode.size() ? tables.treeNode[t] : nullptr;
					next.engineAmplitude.push_back(node ? *reinterpret_cast<const float*>(static_cast<const std::byte*>(node) + 0x15C) : 0.0f);
				}
				const UINT bytes = static_cast<UINT>(std::uint64_t(next.entries) * kTreeWindEntryRows * 16);
				for (std::uint32_t h = 0; h < 2; ++h) {
					D3D11_BUFFER_DESC sourceDesc{};
					sourceDesc.ByteWidth = bytes;
					sourceDesc.Usage = D3D11_USAGE_DEFAULT;
					sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
					sourceDesc.StructureByteStride = sizeof(std::uint32_t);
					const auto source = buffers.treeWindRows[h] ? RenderGraphRuntime::Get().WrapBuffer(*buffers.treeWindRows[h], sourceDesc) : nullptr;
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = bytes;
					desc.Usage = D3D11_USAGE_STAGING;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					if (source && SUCCEEDED(globals::d3d::device->CreateBuffer(&desc, nullptr, next.staging[h].put())))
						context->CopyResource(next.staging[h].get(), source.get());
				}
				next.framesLeft = 3;
				whole = std::move(next);
			}
		}
		if ((treeEpochs++ % 120) != 0 || !buffers.treeWind || tables.treeObjects.empty() || buffers.treesHeld != tables.treesVersion)
			return;
		// The first 64 members, each one's tree entry (TreeParams and WindTimers, 32 bytes) of the wind buffer this frame's draws
		// read (TreeWindReadIndex): the one the frame before wrote, from its inputs. Not this frame's own buffer: the compute
		// queue writes that beside the epoch, and nothing orders a copy of it after the write. Only once a frame before this one
		// has run the pass (the first frame's draws read the zeroed buffer).
		if (buffers.previousTreeFrame + 1 != buffers.treeFrame || buffers.previousTreesHeld != tables.treesVersion)
			return;
		TreeReadback readback;
		const std::size_t count = std::min<std::size_t>(64, tables.treeObjects.size());
		D3D11_BUFFER_DESC sourceDesc{};
		sourceDesc.ByteWidth = static_cast<UINT>(std::uint64_t(buffers.treeCapacity + 1) * kTreeWindEntryRows * 16);
		sourceDesc.Usage = D3D11_USAGE_DEFAULT;
		sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sourceDesc.StructureByteStride = sizeof(std::uint32_t);
		const auto source = RenderGraphRuntime::Get().WrapBuffer(*buffers.treeWindRows[(buffers.treeFrame + 1) & 1], sourceDesc);
		if (!source)
			return;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = static_cast<UINT>(count * 32);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.records.put())))
			return;
		ScopedPerfEvent event("CS DCLF: tree wind readback");
		for (std::size_t i = 0; i < count; ++i) {
			const auto& member = tables.treeObjects[i];
			const std::uint32_t entry = member.tree == kNodelessTree ? 0u : member.tree + 1;
			const auto offset = static_cast<UINT>(std::uint64_t(entry) * kTreeWindEntryRows * 16);
			const D3D11_BOX box{ offset, 0, 0, offset + 32, 1, 1 };
			context->CopySubresourceRegion(readback.records.get(), 0, static_cast<UINT>(i * 32), 0, 0, source.get(), 0, &box);
			readback.samples.push_back({ member.object, member.tree, member.tree < tables.treeNode.size() ? tables.treeNode[member.tree] : nullptr });
		}
		readback.inputs = buffers.previousTreeInputs;
		readback.framesLeft = 3;
		treeReadback = std::move(readback);
	}

	std::uint32_t IndirectDraws::Impl::NextFadeLog(std::uint32_t a_frame, const SceneStore::Tables& a_tables, const FadeFrame& a_inputs)
	{
		if (!SwitchEnabled(Switch::FadeParity) || fadeReadback || (a_frame % 30) != 0 || a_tables.fadeRoots.empty())
			return ~0u;
		const auto count = static_cast<std::uint32_t>(a_tables.fadeRoots.size());
		const std::uint32_t base = fadeLogCursor < count ? fadeLogCursor : 0u;
		fadeLogCursor = base + kFadeLogEntries;
		FadeReadback readback;
		readback.frame = a_frame;
		readback.base = base;
		readback.roots.assign(a_tables.fadeRoots.begin() + base, a_tables.fadeRoots.begin() + std::min(count, base + kFadeLogEntries));
		readback.inputs = a_inputs;
		readback.nodes.resize(readback.roots.size());
		readback.engine.assign(readback.roots.size(), 0);
		{
			const auto& cull = PrimaryCull::Get();
			readback.visibility.assign(cull.FadeVisibility().begin(), cull.FadeVisibility().begin() + std::size_t(cull.FadeVisibilityBlocks()) * kFadeVisibilityBytes);
			std::vector<std::uint32_t> lists;
			cull.FadeRootLists(a_tables.fadeRootNode, lists);
			readback.lists.assign(readback.roots.size(), kFadeRootNoList);
			readback.radii.assign(readback.roots.size(), 0.0f);
			readback.nodeRadii.assign(readback.roots.size(), 0.0f);
			readback.nodeFlags.assign(readback.roots.size(), 0u);
			for (std::size_t i = 0; i < readback.roots.size(); ++i) {
				readback.lists[i] = base + i < lists.size() ? lists[base + i] : kFadeRootNoList;
				const auto object = readback.roots[i].object;
				const float entry = object < a_tables.sunEntry.size() ? a_tables.sunEntry[object][3] : -1.0f;
				readback.radii[i] = entry >= 0.0f && entry < 1e30f ? entry : readback.roots[i].radius;
				if (const auto* node = static_cast<const RE::NiAVObject*>(a_tables.fadeRootNode[base + i])) {
					readback.nodeRadii[i] = node->worldBound.radius;
					readback.nodeFlags[i] = node->GetFlags().underlying();
				}
			}
		}
		if (const auto* camera = RE::Main::WorldRootCamera())
			readback.worldCamera = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z, Engine::At<float>(camera, 0x184) };
		// The engine's nodes as the list jobs left them (PrimaryCull's snapshot): after this cull's OnVisible, before the next
		// animation job's updates, which run beside the render thread.
		const auto* snapshot = PrimaryCull::Get().NodeSnapshot(a_frame);
		for (std::size_t i = 0; i < readback.roots.size(); ++i) {
			const auto bits = readback.roots[i].bits;
			const auto* node = static_cast<const RE::NiAVObject*>(a_tables.fadeRootNode[base + i]);
			if (node && snapshot && base + i < snapshot->size() && (bits & kFadeRootOwned) && !(bits & kFadeRootStoodIn)) {
				readback.nodes[i] = (*snapshot)[base + i];
				readback.engine[i] = 1;
				readback.nodeCentres.resize(readback.roots.size());
				readback.nodeNames.resize(readback.roots.size());
				readback.nodeCentres[i] = { node->worldBound.center.x, node->worldBound.center.y, node->worldBound.center.z };
				readback.nodeNames[i] = node->name.c_str() ? node->name.c_str() : "";
			}
		}
		fadeReadback = std::move(readback);
		return base;
	}

	void IndirectDraws::Impl::ReadFadeLog(const std::shared_ptr<Resources>& a_resources)
	{
		if (!fadeReadback)
			return;
		auto* context = globals::d3d::context;
		auto& readback = *fadeReadback;
		auto& buffers = *a_resources->scene;
		if (!readback.log) {
			// After the depth epoch that logged: the log into a staging buffer, read three frames later.
			if (!buffers.fadeLog || buffers.fadeLogBase != readback.base || buffers.fadeFrameNumber != readback.frame) {
				fadeReadback.reset();
				return;
			}
			constexpr UINT bytes = kFadeLogEntries * sizeof(FadeLogEntry);
			D3D11_BUFFER_DESC sourceDesc{};
			sourceDesc.ByteWidth = bytes;
			sourceDesc.Usage = D3D11_USAGE_DEFAULT;
			sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			sourceDesc.StructureByteStride = sizeof(std::uint32_t);
			const auto source = RenderGraphRuntime::Get().WrapBuffer(*buffers.fadeLog, sourceDesc);
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = bytes;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (!source || FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.log.put()))) {
				fadeReadback.reset();
				return;
			}
			ScopedPerfEvent event("CS DCLF: fade state readback");
			const D3D11_BOX box{ 0, 0, 0, bytes, 1, 1 };
			context->CopySubresourceRegion(readback.log.get(), 0, 0, 0, 0, source.get(), 0, &box);
			readback.framesLeft = 3;
			return;
		}
		if (--readback.framesLeft)
			return;
		const auto done = std::move(*fadeReadback);
		fadeReadback.reset();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(done.log.get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return;
		const auto* entries = static_cast<const FadeLogEntry*>(mapped.pData);
		auto& p = fadeParity;
		++p.logs;
		for (std::uint32_t i = 0; i < done.roots.size(); ++i) {
			const auto& entry = entries[i];
			const auto& root = done.roots[i];
			// Only what the pass did that frame, for the row it had.
			if (entry.root != done.base + i || entry.after.frame != done.frame || entry.after.generation != root.generation || !root.generation)
				continue;
			// A root the engine culls too: its OnVisible on the node and FadeStateCS's for the members, from the same state.
			if (i < done.engine.size() && done.engine[i]) {
				++p.engineChecked;
				const auto& n = done.nodes[i];
				const auto& g = entry.after;
				const auto within = [](float a_a, float a_b) { return std::abs(a_a - a_b) <= 1e-5f * std::max(1.0f, std::abs(a_a)); };
				const auto engineDifferences = FadeState::Differences(n, g);
				if (engineDifferences.empty())
					++p.engineExact;
				else if (n.flags == g.flags && n.lastVisible == g.lastVisible && (n.levels & 0xFFFF) == (g.levels & 0xFFFF) && within(n.currentFade, g.currentFade) &&
						 within(n.snapRadius, g.snapRadius) && within(n.amountFade, g.amountFade) && within(n.metric, g.metric) && within(n.previousMetric, g.previousMetric) &&
						 within(n.blend, g.blend))
					++p.engineRounding;
				else {
					++p.engineDiffer;
					if (p.engineFirst.empty()) {
						// Where each metric was measured from: the distance it implies at the frame's scale, against the root's
						// distance from the fade eye and from the world camera.
						const auto& in = done.inputs;
						const auto distanceTo = [&](const float* a_eye) {
							const float dx = entry.centre[0] - a_eye[0], dy = entry.centre[1] - a_eye[1], dz = entry.centre[2] - a_eye[2];
							return std::sqrt(dx * dx + dy * dy + dz * dz);
						};
						const float perMetric = g.metric != 0.0f ? distanceTo(in.eye) / g.metric : 0.0f;
						p.engineFirst = fmt::format("root {} (plan {}, verdict {:#x}): {}; centre ({:.0f} {:.0f} {:.0f}), fade eye ({:.0f} {:.0f} {:.0f}) lodAdjust {} at {:.0f}, "
													"world camera ({:.0f} {:.0f} {:.0f}) lodAdjust {} at {:.0f}; the engine's metric implies {:.0f} at the fade eye's scale",
							entry.root, root.bits & kFadeRootPlanMask, g.verdict, engineDifferences, entry.centre[0], entry.centre[1], entry.centre[2], in.eye[0], in.eye[1],
							in.eye[2], in.lodAdjust, distanceTo(in.eye), done.worldCamera[0], done.worldCamera[1], done.worldCamera[2], done.worldCamera[3],
							distanceTo(done.worldCamera.data()), n.metric * perMetric);
						// The metric at this frame's eye (the port's OnVisible on a copy): which side is current.
						FadeNodeState now = entry.before;
						FadeState::OnVisible(now, root, entry.centre, in);
						p.engineFirst += fmt::format("; at this frame's eye the metric is {}; FadeStateCS's before {} lastVisible {} (after {}), the engine's lastVisible {}",
							now.metric, entry.before.metric, entry.before.lastVisible, g.lastVisible, n.lastVisible);
						{
							// FadeStateCS's test again, on what it was given: the root's list block, its centre and the radius it read.
							const std::uint32_t list = done.lists[i];
							std::string why = "no list: the latch's frustum";
							int port = -2;
							if (list != kFadeRootNoList && (std::size_t(list) + 1) * kFadeVisibilityBytes <= done.visibility.size())
								port = PrimaryCull::FadeVisibilityPort(done.visibility.data() + std::size_t(list) * kFadeVisibilityBytes, entry.centre, done.radii[i],
									done.nodeFlags[i], why);
							{
								// How far below its listed entry the root is (the cut lists entries; the engine culls a child after its parents).
								const auto* node = static_cast<const RE::NiAVObject*>(SceneStore::Get().GetTables().fadeRootNode[entry.root]);
								std::uint32_t depth = 0;
								const RE::NiAVObject* at = node;
								while (at && !PrimaryCull::Get().IsEntry(at))
									at = at->parent, ++depth;
								p.engineFirst += fmt::format("; {} below its entry '{}'", at ? fmt::format("{} levels", depth) : std::string("no entry"),
									at && at->name.c_str() ? at->name.c_str() : "");
							}
							p.engineFirst += fmt::format("; its list {} ({} blocks), radius read {} (the node's {}), the test on those {} ({})",
								static_cast<std::int32_t>(list), done.visibility.size() / kFadeVisibilityBytes, done.radii[i], done.nodeRadii[i], port, why);
						}
						if (i < done.nodeCentres.size())
							p.engineFirst += fmt::format("; the node '{}' centre ({:.0f} {:.0f} {:.0f}) at {:.0f}", done.nodeNames[i], done.nodeCentres[i][0], done.nodeCentres[i][1],
								done.nodeCentres[i][2], distanceTo(done.nodeCentres[i].data()) * 0.0f + [&] {
									const float dx = done.nodeCentres[i][0] - in.eye[0], dy = done.nodeCentres[i][1] - in.eye[1], dz = done.nodeCentres[i][2] - in.eye[2];
									return std::sqrt(dx * dx + dy * dy + dz * dz);
								}());
					}
				}
			}
			++p.updates;
			FadeNodeState port = entry.before;
			std::uint32_t verdict = 0;
			if (entry.after.verdict & kFadeVerdictAnimated) {
				const std::uint32_t updates = (entry.after.verdict >> 5) & kFadeAnimatedCountMask;
				FadeState::AnimatedUpdate(port, root, entry.centre, done.inputs, updates);
				verdict |= kFadeVerdictAnimated | (updates << 5);
			}
			if (entry.after.verdict & kFadeVerdictInView) {
				++p.inView;
				verdict |= FadeState::OnVisible(port, root, entry.centre, done.inputs);
			}
			p.serviced += (verdict & kFadeVerdictServiced) ? 1 : 0;
			const auto differences = FadeState::Differences(port, entry.after);
			if (differences.empty() && verdict == entry.after.verdict) {
				++p.exact;
				continue;
			}
			// Rounding: the same integers and bits, the floats within a few units in the last place (the GPU's division and
			// square root need not round as SSE does).
			const auto close = [](float a_a, float a_b) { return std::abs(a_a - a_b) <= 1e-5f * std::max(1.0f, std::abs(a_a)); };
			const auto& a = entry.after;
			const bool rounding = verdict == a.verdict && port.flags == a.flags && port.lastVisible == a.lastVisible && (port.levels & 0xFFFF) == (a.levels & 0xFFFF) &&
			                      close(port.currentFade, a.currentFade) && close(port.snapRadius, a.snapRadius) && close(port.amountFade, a.amountFade) &&
			                      close(port.metric, a.metric) && close(port.previousMetric, a.previousMetric) && close(port.blend, a.blend);
			++(rounding ? p.rounding : p.differ);
			if (!rounding && p.first.empty())
				p.first = fmt::format("root {} (plan {}, verdict {:#x}, expected {:#x}): {}", entry.root, root.bits & kFadeRootPlanMask, a.verdict, verdict,
					differences.empty() ? "the verdict" : differences);
		}
		context->Unmap(done.log.get(), 0);
		if ((p.logs % 10) == 0) {
			logger::info("[DCLF] fade state parity (FadeStateCS against the port): {} logs, {} root updates ({} in view, {} serviced); {} exact, {} within rounding, {} differ{}{}",
				p.logs, p.updates, p.inView, p.serviced, p.exact, p.rounding, p.differ, p.differ ? " <- FADE STATE" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
			logger::info("[DCLF] fade state of roots with engine-drawn parts (FadeStateCS against the engine's node): {} checked, {} exact, {} within rounding, {} differ{}{}",
				p.engineChecked, p.engineExact, p.engineRounding, p.engineDiffer, p.engineDiffer ? " <- ENGINE FADE" : " <- OK",
				p.engineFirst.empty() ? "" : "; first: " + p.engineFirst);
			p = {};
		}
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
				const auto& object = tables.objects[input.objectIndex];
				readback.expectedLocalShadows[input.objectIndex] = localShadows.MaskOf(property, object.boundCenter, object.boundRadius) & 0xFu;
			}
		}
		readback.framesLeft = 3;
		if (readback.sequences && readback.count)
			parity = std::move(readback);
	}
}

#endif
