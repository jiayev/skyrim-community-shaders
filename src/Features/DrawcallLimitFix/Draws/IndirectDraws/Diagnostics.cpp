#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF
{
	std::string IndirectDraws::AsyncReport()
	{
		std::string text = AsyncWorker::Get().Report() + AsyncWorker::Get().RenderWaitReport();
		if (auto window = EngineReadWindow::Report(); !window.empty())
			text += window + "\n";
		if (impl->scene && impl->scene->fadeWriteBack) {
			auto& writeBack = *impl->scene->fadeWriteBack;
			const auto applied = writeBack.applied.exchange(0), stale = writeBack.stale.exchange(0), busy = writeBack.busy.exchange(0);
			const auto storeNs = writeBack.storeNs.exchange(0);
			if (applied || stale || busy)
				text += fmt::format("[DCLF] fade write-back (6e S4, F1): {} milestones written onto stood-in roots' nodes by the render thread at the frame's "
									"start ({:.1f} us in all), {} stale (the root listed again or no longer stood in), {} frames found the last task still running "
									"(their batches its or the next's); event list {} events\n",
					applied, static_cast<double>(storeNs) / 1000.0, stale, busy, writeBack.capacity.load(std::memory_order_relaxed));
		}
		text += ReflectionReport();
		if (impl->scene && impl->scene->treeLodCull && impl->scene->treeLodUploads) {
			auto& scene = *impl->scene;
			text += fmt::format("[DCLF] tree LOD draws: {} ({} depth commits, {} shape slots sent; tables {} shape and {} mesh slots; {} commits that could not draw a "
								"withheld frame){}\n",
				impl->treeLodOwned ? "DCLF's" : "the engine's", scene.treeLodUploads, scene.treeLodSlotsSent, scene.treeLodShapeCapacity, scene.treeLodMeshCapacity,
				impl->treeLodMissed, impl->treeLodMissed ? " <- TREE LOD HOLES" : "");
			scene.treeLodUploads = scene.treeLodSlotsSent = 0;
			// Its two-phase cull: in view, drawn by phase 1, occluded by the last frame's HZB, brought back by the rebuilt one.
			if (auto& counts = scene.treeLodCounts) {
				if (const auto frames = counts->frames.exchange(0, std::memory_order_relaxed)) {
					const double phaseOne = double(counts->phaseOne.exchange(0, std::memory_order_relaxed)) / frames;
					const double retests = double(counts->retests.exchange(0, std::memory_order_relaxed)) / frames;
					const double phaseTwo = double(counts->phaseTwo.exchange(0, std::memory_order_relaxed)) / frames;
					text += fmt::format("[DCLF] tree LOD cull over {} frames: per frame {:.0f} instances in view, {:.0f} drawn by phase 1, {:.0f} occluded by the last "
										"frame's HZB, {:.0f} of them brought back by phase 2 ({:.0f} drawn, {:.1f}% occluded)\n",
						frames, phaseOne + retests, phaseOne, retests, phaseTwo, phaseOne + phaseTwo,
						phaseOne + retests > 0.0 ? 100.0 * (retests - phaseTwo) / (phaseOne + retests) : 0.0);
				}
			}
		}
		if (auto& rows = impl->mainRows; rows.builds) {
			text += fmt::format("[DCLF] main rows: {} builds; a build: {:.1f} material and {:.1f} pipeline rows written; {} material and {} pipeline rows held "
								"(tables {} and {} rows), {} resyncs; material rows written again by what moved: record {}, frame values {}, lookup {}, shared {}, "
								"technique bindings {}, projected {}, constant tables {}\n",
				rows.builds, double(rows.materialsWritten) / rows.builds, double(rows.pipelinesWritten) / rows.builds, rows.material.Size(), rows.pipeline.Size(),
				impl->resources ? impl->resources->materialRows.capacity : 0u, impl->resources ? impl->resources->pipelineRows.capacity : 0u, rows.resyncs,
				rows.keyMoved[0], rows.keyMoved[1], rows.keyMoved[2], rows.keyMoved[3], rows.keyMoved[4], rows.keyMoved[5], rows.keyMoved[6] + rows.keyMoved[7]);
			rows.builds = rows.materialsWritten = rows.pipelinesWritten = rows.resyncs = 0;
			rows.keyMoved = {};
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
		if (auto line = FrameValues::Get().Report(); !line.empty())
			text += line + "\n";
		if (auto line = FrameData::Report(); !line.empty())
			text += line + "\n";
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
		{
			// R3c (a): the main shapes a scene revision makes at the join, against the commits' (Impl::ShapeParity).
			auto& p = impl->shapeParity;
			std::string segments;
			for (const std::size_t shape : { kDepthShape, kColourShape }) {
				std::string lags;
				for (std::size_t lag = 0; lag < 2; ++lag) {
					auto& c = p.counts[shape][lag];
					std::string fields;
					for (std::uint32_t f = 0; f < kMainShapeFields; ++f)
						if (c.differ[f])
							fields += fmt::format("{}{} {}", fields.empty() ? "" : ", ", kMainShapeFieldNames[f], c.differ[f]);
					lags += fmt::format("{}{}: {} compared, {} same, {} without one{}", lags.empty() ? "" : "; ", lag ? "the frame before's" : "the frame's own", c.compared,
						c.same, c.missing, fields.empty() ? std::string() : " (differ: " + fields + ")");
					c = {};
				}
				if (!lags.empty())
					segments += fmt::format("{}{} against {}; layout misses {}", segments.empty() ? "" : " | ", shape == kDepthShape ? "Z-prepass" : "colour", lags,
						p.layoutMisses[shape]);
				p.layoutMisses[shape] = 0;
			}
			auto& sp = impl->shadowParity;
			for (std::size_t kind = 0; kind < 2; ++kind)
				for (std::size_t lag = 0; lag < 2; ++lag) {
					auto& c = sp.counts[kind][lag];
					std::string fields;
					for (std::uint32_t f = 0; f < Impl::ShadowParity::kFields; ++f)
						if (c.differ[f])
							fields += fmt::format("{}{} {}", fields.empty() ? "" : ", ", Impl::ShadowParity::kFieldNames[f], c.differ[f]);
					segments += fmt::format("{}{} against {}: {} compared, {} same, {} without its layout{}", lag ? "; " : " | ", kind ? "occlusion" : "shadow",
						lag ? "the frame before's" : "the frame's own", c.compared, c.same, c.missing, fields.empty() ? std::string() : " (differ: " + fields + ")");
					c = {};
				}
			segments += fmt::format("; shadow layout misses {}", sp.layoutMisses);
			sp.layoutMisses = 0;
			auto& rp = impl->reflectionParity;
			for (std::size_t lag = 0; lag < 2; ++lag) {
				auto& c = rp.counts[lag];
				segments += fmt::format("{}reflection against {}: {} compared, {} same, {} without one, {} differ", lag ? "; " : " | ", lag ? "the frame before's" : "the frame's own",
					c.compared, c.same, c.missing, c.differ[kShapePipelines]);
				c = {};
			}
			text += fmt::format("[DCLF] shape parity (R3c): {}\n", segments);
			text += impl->RevisionReport();
		}
		if (auto& bound = impl->drawBound; bound.updates) {
			text += fmt::format("[DCLF] scene draw bound: {} updates, {:.1f} slots changed an update, {} draws over {} slots, {} resyncs; parity {} checked, {} differ{}\n",
				bound.updates, static_cast<double>(bound.changes) / bound.updates, bound.draws, bound.produced.size(), bound.resyncs, bound.parity.checks,
				bound.parity.mismatches, bound.parity.Verdict(true));
			bound.updates = bound.changes = bound.resyncs = 0;
			bound.parity.Reset();
		}
		if (auto* store = &impl->extrasStore; store->updates) {
			text += fmt::format("[DCLF] persistent extras rows: {} updates, {} resyncs; parity {} checked, {} differ{}\n", store->updates, store->resyncs,
				store->parity.checks, store->parity.mismatches, store->parity.Verdict());
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
				text += fmt::format("[DCLF] {} exclusion: {} builds, {} reused ({} of them for newer candidates, the entries that moved judged again); parity {} checked, {} differ{}\n", cache == &impl->sunExclusionCache ? "sun" : "paraboloid",
					c.builds, c.reused, c.translated, c.parity.checks, c.parity.mismatches, c.parity.Verdict());
				c.builds = c.reused = c.translated = 0;
				c.parity.Reset();
			}
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.builtInline && !a.used)
				continue;
			text += fmt::format("[DCLF] {} epochs (6e E3b, S1): {} committed the payload built ahead with their publication, {} built at the epoch (bindless parity); "
								"revision parity: {} payloads for other inputs than the frame's{}\n",
				kNames[i], a.used, a.builtInline, a.stale, a.stale ? " <- STALE" : " <- OK");
			if (i == kAsyncZPrepass)
				text += fmt::format("[DCLF] frame slots supplied from an earlier capture (6e E5): {}\n", std::exchange(impl->frameSlotsCarried, 0));
			a = {};
		}
		if (auto& p = impl->viewMaskParity; p.checks.load(std::memory_order_relaxed)) {
			static constexpr std::array<const char*, 2 + kShadowModeCount> kLists{ "Z-prepass", "colour", "plain", "clamped", "paraboloid", "sky occlusion", "precipitation" };
			const auto first = p.first.exchange(~0ull);
			const auto differ = p.differ.exchange(0);
			text += fmt::format("[DCLF] view mask parity (U3): {} publications, {} inputs, {} with a mask other than their object's or outside their list's views{}{}\n",
				p.checks.exchange(0), p.inputs.exchange(0), differ, differ ? " <- VIEW MASK" : " <- OK",
				first == ~0ull ? std::string() :
								 fmt::format("; first: {} input of object {}, mask {:#x}, expected {:#x}", kLists[std::min<std::size_t>(first >> 56, kLists.size() - 1)],
									 (first >> 24) & 0xFFFFFFFFu, p.firstMask.load(std::memory_order_relaxed), first & 0xFFFFFFu));
		}
		{
			auto& ring = impl->ringStats;
			const auto frames = std::max<std::uint64_t>(ring.frames, 1);
			text += fmt::format("[DCLF] payload ring (6e E4): {} frames filled by the producer ({} grew an entry), {} commits read it; {:.1f} KB in {:.1f} runs a frame\n",
				ring.frames, ring.grown, ring.committed, impl->ringBytes.exchange(0) / 1024.0 / frames, double(impl->ringRuns.exchange(0)) / frames);
			auto part = [&](std::size_t a_part) { return impl->ringPartBytes[a_part].exchange(0) / 1024.0 / frames; };
			text += fmt::format("[DCLF] payload ring by buffer (KB a frame): objects {:.1f}, extras {:.1f}, geometries {:.1f}, material rows {:.1f}, pipeline rows {:.1f}, resident regions {:.1f}, frame inputs {:.1f}, shadow rows {:.1f}, shadow inputs {:.1f}; {} shadow commits read it (6e S2)\n",
				part(Impl::kRingObjects), part(Impl::kRingExtras), part(Impl::kRingGeometries), part(Impl::kRingMaterialRows), part(Impl::kRingPipelineRows),
				part(Impl::kRingResident), part(Impl::kRingFrameInputs), part(Impl::kRingShadowRows), part(Impl::kRingShadowInputs), ring.shadowCommitted);
			ring = {};
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
		// A gap's first frame, by what rejected it: the frustum stamp and its fade bit, the root's ownership.
		static constexpr const char* kGapClasses[] = { "?", "the frustum", "an owned fade root", "the resident fade test",
			"past the frustum and fade (HZB or tree height)", "no frustum stamp", "no verdict this frame" };
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
			D3D11_MAPPED_SUBRESOURCE frustumMapped{};
			const std::uint32_t* frustum = nullptr;
			std::size_t frustumWords = 0;
			if (snapshot.frustumStaging && SUCCEEDED(context->Map(snapshot.frustumStaging.get(), 0, D3D11_MAP_READ, 0, &frustumMapped))) {
				D3D11_BUFFER_DESC frustumDesc{};
				snapshot.frustumStaging->GetDesc(&frustumDesc);
				frustum = static_cast<const std::uint32_t*>(frustumMapped.pData);
				frustumWords = frustumDesc.ByteWidth / sizeof(std::uint32_t);
				// No stamp of this frame at all: phase 1 wrote no feedback (a frame before the first cull), nothing to read.
				if (std::none_of(frustum, frustum + std::min(frustumWords, snapshot.flags.size()), [&](std::uint32_t a_word) { return (a_word & 0x0FFFFFFFu) == stamp; })) {
					context->Unmap(snapshot.frustumStaging.get(), 0);
					frustum = nullptr;
				}
			}
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
							geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", depthDrawn ? "depth" : "", colourDrawn ? " colour" : "");
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
					// The frustum stamp is this frame's for an object phase 1 found inside the frustum, with the fade test's drop in its
					// top bit: one without it was rejected by the frustum, one with it and no drop by the HZB (phase 2's final verdict).
					const std::uint32_t frustumWord = frustum && o < frustumWords ? frustum[o] : 0u;
					const bool passedFrustum = (frustumWord & 0x0FFFFFFFu) == stamp;
					const bool fadeHidden = passedFrustum && (frustumWord & 0x80000000u);
					if (verdict == 2 && frustum && !decal && (fadeHidden || !passedFrustum) && o < snapshot.inView.size() && snapshot.inView[o]) {
						// Rejected as final with its live bound in view. By the frustum: the bound the culling read was not the live one,
						// and the engine would have drawn it. By the fade test: an owned root FadeStateCS has faded out, counted (a far
						// root fades out in view; the fade parities judge the fade).
						++(fadeHidden ? counts.fadeHiddenInView : counts.rejectedInView);
						const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
						const bool alive = geometry != nullptr;  // held by snapshot.tables
						const bool skinned = alive && geometry->GetGeometryRuntimeData().skinInstance;
						counts.skinnedInView += skinned;
						if (alive && !fadeHidden && counts.inViewSamples++ < 24) {
							const RE::TESObjectREFR* owner = nullptr;
							for (const RE::NiAVObject* node = geometry; node && !owner; node = node->parent)
								owner = node->GetUserData();
							const auto* base = owner ? owner->GetBaseObject() : nullptr;
							const auto& tables = *snapshot.tables;
							const std::uint32_t root = o < tables.objectFadeRoot.size() ? tables.objectFadeRoot[o] : kNoFadeRoot;
							const auto* rootNode = root < tables.fadeRootNode.size() ? static_cast<const RE::NiAVObject*>(tables.fadeRootNode[root]) : nullptr;
							const auto* fadeNode = rootNode ? const_cast<RE::NiAVObject*>(rootNode)->AsFadeNode() : nullptr;
							const auto& b = snapshot.bound[o];
							const auto& live = geometry->worldBound;
							logger::info(
								"[DCLF] set parity, frame {}: rejected in view ({}) - object {} '{}'{} of {} '{}': bound at the snapshot ({:.0f} {:.0f} {:.0f}) r {:.0f}, "
								"now ({:.0f} {:.0f} {:.0f}) r {:.0f}; fade root {} '{}' (bits {:#x}, currentFade {:.3f}); colour build {}",
								snapshot.frame, fadeHidden ? "the fade test" : "the frustum", o, geometry->name.c_str() ? geometry->name.c_str() : "", skinned ? " (skinned)" : "",
								base ? RE::FormTypeToString(base->GetFormType()) : "no ref", base && base->GetName() ? base->GetName() : "", b[0], b[1], b[2], b[3], live.center.x,
								live.center.y, live.center.z, live.radius, root == kNoFadeRoot ? -1 : static_cast<std::int64_t>(root),
								rootNode && rootNode->name.c_str() ? rootNode->name.c_str() : "", root < tables.fadeRoots.size() ? tables.fadeRoots[root].bits : 0u,
								fadeNode ? fadeNode->GetRuntimeData().currentFade : -1.0f,
								stateName(o < snapshot.colourState.size() ? snapshot.colourState[o] : kObjectStateAbsent));
						}
					}
					if (verdict == 0 || verdict == 2) {
						++counts.withheldCulled;  // the GPU culling rejected it: not drawn by anyone, as intended
					} else {
						kind = "in the set, drawn by nobody";
						++counts.withheldUndrawn;
						counts.alphaWithheldUndrawn += alpha;
					}
				}
				// The registration hooks' main claim against the set: claimed outside it, nobody draws the object.
				if (o < snapshot.claims.size() && ((snapshot.claims[o] & kSetMain) != 0) != withheld) {
					const bool claimed = (snapshot.claims[o] & kSetMain) != 0;
					++(claimed ? counts.claimedOutside : counts.unclaimedMembers);
					const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
					if (geometry && counts.claimSamples++ < 12)
					{
						const auto& tables = *snapshot.tables;
						const std::uint32_t partner = tables.IsLayer(static_cast<std::uint32_t>(o)) ? tables.layerBase[o] : o < tables.layerOf.size() ? tables.layerOf[o] : kNoObjectSlot;
						logger::info("[DCLF] set parity, frame {}: {} - object {} '{}' (claims {:#x}{}){}", snapshot.frame,
							claimed ? "claimed by the registration hooks, not in the set: drawn by nobody" : "in the set, not claimed: drawn twice", o,
							geometry->name.c_str() ? geometry->name.c_str() : "", snapshot.claims[o],
							partner == kNoObjectSlot ? std::string() :
													 fmt::format(", {} {} {}", tables.IsLayer(static_cast<std::uint32_t>(o)) ? "layer of" : "base of", partner,
														 partner < snapshot.flags.size() && (snapshot.flags[partner] & 1) ? "in the set" : "not in the set"),
							depthDrawn || colourDrawn ? ", drawn by DCLF" : "");
					}
				}
				// The gap detector, by geometry (object indices are rebuilt every frame): a run of one to eight frames withheld and
				// GPU-culled between frames something drew it.
				if (const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr) {
					const bool kept = flags & 2;
					const std::uint8_t state = !kept ? 0 : (withheld && !colourDrawn) ? 2 : 1;
					auto& history = gapHistory[geometry];
					const bool contiguous = history.frame + 1 == snapshot.frame;
					if (!contiguous || !(history.last & 3))
						history.first = kept ? snapshot.frame : 0;
					if (state == 1 && contiguous && history.last == 2 && history.run && history.run <= 8 && history.runDrawnBy) {
						++counts.gaps;
						++counts.gapsByLength[history.run == 1 ? 0 : history.run == 2 ? 1 : 2];
						++counts.gapsByClass[std::min<std::uint8_t>(history.runClass, 6)];
						counts.gapsRetest += history.runVerdict == 0;
						counts.gapsRejected += history.runVerdict == 2;
						counts.gapsInView += history.runInView;
						const std::uint32_t age = snapshot.frame - history.run - history.first;
						counts.gapsNew += age <= 60;
						counts.gapsBeforeNative += history.runDrawnBy == 1;
						counts.gapsAfterNative += !withheld;
						const RE::TESObjectREFR* owner = nullptr;
						for (const RE::NiAVObject* node = geometry; node && !owner; node = node->parent)
							owner = node->GetUserData();
						const auto* base = owner ? owner->GetBaseObject() : nullptr;
						const bool tree = base && base->GetFormType() == RE::FormType::Tree;
						counts.gapsTree += tree;
						if (counts.gapSamples++ < 12)
							logger::info("[DCLF] set parity, frame {}: {}-frame gap - '{}' ({}{}), culled from frame {} by {} (verdict {}{}), {} frames after it was first resident; "
										 "before it {}, after it {}",
								snapshot.frame, history.run, geometry->name.c_str() ? geometry->name.c_str() : "?", base ? RE::FormTypeToString(base->GetFormType()) : "no ref",
								geometry->GetGeometryRuntimeData().skinInstance ? ", skinned" : "", snapshot.frame - history.run,
								kGapClasses[std::min<std::uint8_t>(history.runClass, 6)], history.runVerdict, history.runInView ? ", live bound in view" : "", age,
								history.runDrawnBy == 1 ? "outside the set" : "drawn by DCLF", withheld ? "drawn by DCLF" : "outside the set");
					}
					if (state == 2) {
						if (contiguous && history.last == 2) {
							history.run = static_cast<std::uint8_t>(std::min(history.run + 1, 255));
						} else {
							// The run's first frame: what rejected it.
							const std::uint32_t frustumWord = frustum && o < frustumWords ? frustum[o] : 0u;
							const bool passedFrustum = (frustumWord & 0x0FFFFFFFu) == stamp;
							std::uint8_t cls = !current ? 6 : !frustum ? 5 : !passedFrustum ? 1 : 4;
							if (passedFrustum && (frustumWord & 0x80000000u)) {
								const auto& tables = *snapshot.tables;
								const std::uint32_t root = o < tables.objectFadeRoot.size() ? tables.objectFadeRoot[o] : kNoFadeRoot;
								cls = root < tables.fadeRoots.size() && (tables.fadeRoots[root].bits & kFadeRootOwned) ? 2 : 3;
							}
							history.run = 1;
							history.runClass = cls;
							history.runVerdict = static_cast<std::uint8_t>(verdict & 3);
							history.runInView = o < snapshot.inView.size() && snapshot.inView[o];
							history.runDrawnBy = contiguous && history.last == 1 ? history.drawnBy : 0;
						}
					} else {
						history.run = 0;
					}
					if (state == 1)
						history.drawnBy = withheld ? 2 : 1;
					history.last = state;
					history.frame = snapshot.frame;
				}
				if (!kind)
					continue;
				damaged = true;
				if (counts.samples++ < 40) {
					const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
					const bool alive = geometry != nullptr;  // held by snapshot.tables
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
			if (frustum)
				context->Unmap(snapshot.frustumStaging.get(), 0);
			if (snapshot.frustumStaging)
				setParityStaging.push_back(std::move(snapshot.frustumStaging));
			++counts.frames;
			counts.framesWithDamage += damaged;
			if (counts.frames == 300) {
				logger::info("[DCLF] set parity over {} frames ({} with damage, {} unread): depth without colour {} ({} alpha tested), colour without depth {} ({} alpha tested, {} with no verdict), in the set and drawn by nobody {} ({} alpha tested); in the set and GPU-culled {}; drawn outside the set {}; records disagreeing with the set {}; per frame {:.0f} depth draws, {:.0f} colour draws{}",
					counts.frames, counts.framesWithDamage, counts.skipped, counts.depthOnly, counts.alphaDepthOnly, counts.colourOnly, counts.alphaColourOnly,
					counts.colourUnpublished, counts.withheldUndrawn, counts.alphaWithheldUndrawn, counts.withheldCulled, counts.outsideDrawn, counts.recordDisagrees,
					double(counts.depthDrawnTotal) / counts.frames, double(counts.colourDrawnTotal) / counts.frames,
					counts.depthOnly || counts.colourOnly || counts.withheldUndrawn || counts.outsideDrawn || counts.recordDisagrees ? " <- SET PARITY" : " <- OK");
				// Counted, not flagged: the frustum's few are a bound at the view's edge or a far LOD block's, the fade test's legitimate.
				logger::info("[DCLF] set parity over {} frames: members rejected with their live bound in view: {} by the frustum, {} by the fade test (owned roots faded "
							 "out), {} of all of them skinned",
					counts.frames, counts.rejectedInView, counts.fadeHiddenInView, counts.skinnedInView);
				logger::info("[DCLF] set parity over {} frames: the registration hooks' main claims against the set: {} claimed outside it (drawn by nobody), {} members "
							 "not claimed (drawn twice){}",
					counts.frames, counts.claimedOutside, counts.unclaimedMembers, counts.claimedOutside || counts.unclaimedMembers ? " <- CLAIMS" : " <- OK");
				if (counts.gaps)
					logger::info("[DCLF] set parity over {} frames: {} gaps (resident, drawn, withheld and GPU-culled for 1-8 frames, drawn again; counted, not flagged: an "
								 "occlusion rejection is against this frame's depth): {} of one frame, {} of two, {} longer; {} occluded (retest), {} rejected; by the first gap "
								 "frame's rejection: frustum {}, owned fade root {}, resident fade test {}, HZB or tree height {}, no frustum stamp {}, no verdict {}; {} with the live "
								 "bound in view, {} within 60 frames of first resident, {} outside the set before, {} after; {} of them trees",
						counts.frames, counts.gaps, counts.gapsByLength[0], counts.gapsByLength[1], counts.gapsByLength[2], counts.gapsRetest, counts.gapsRejected,
						counts.gapsByClass[1], counts.gapsByClass[2], counts.gapsByClass[3], counts.gapsByClass[4], counts.gapsByClass[5], counts.gapsByClass[6],
						counts.gapsInView, counts.gapsNew, counts.gapsBeforeNative, counts.gapsAfterNative, counts.gapsTree);
				counts = {};
				// Forget geometries not seen for a while, so the history does not keep every object ever drawn.
				std::erase_if(gapHistory, [&](const auto& a_entry) { return a_entry.second.frame + 8 < snapshot.frame; });
			}
		}

		// This frame: the words as the colour epoch left them, and the CPU side that explains them. Only when
		// the depth build is this frame's too.
		if (!a_resources->visibilityD3D11 || &a_depth == &a_colour || setParityFrames.size() >= 8)
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
		if (a_resources->frustumD3D11) {
			while (!setParityStaging.empty() && !snapshot.frustumStaging) {
				D3D11_BUFFER_DESC held{};
				setParityStaging.back()->GetDesc(&held);
				if (held.ByteWidth == desc.ByteWidth)
					snapshot.frustumStaging = std::move(setParityStaging.back());
				setParityStaging.pop_back();
			}
			D3D11_BUFFER_DESC staging = desc;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.BindFlags = 0;
			staging.MiscFlags = 0;
			staging.StructureByteStride = 0;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (snapshot.frustumStaging || SUCCEEDED(globals::d3d::device->CreateBuffer(&staging, nullptr, snapshot.frustumStaging.put())))
				context->CopyResource(snapshot.frustumStaging.get(), a_resources->frustumD3D11.get());
		}
		// The frame the epochs drew (their visibility stamps), not the one the payloads were built in (step 6e E3b).
		snapshot.frame = store.GetFrame();
		snapshot.framesLeft = 3;
		snapshot.depthState = a_depth.objectState;
		snapshot.colourState = a_colour.objectState;
		// Only a publication's tables: the coordinator's own (none accepted yet) are not the render thread's to hold.
		snapshot.tables = store.AcceptedTables();
		if (!snapshot.tables) {
			setParityStaging.push_back(std::move(snapshot.staging));
			if (snapshot.frustumStaging)
				setParityStaging.push_back(std::move(snapshot.frustumStaging));
			return;
		}
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
		// The engine's frustum verdict for each member: its live bound against the depth build's ViewProj, the corner test the
		// culling makes (BuildDrawsCS: Culled).
		if (a_depth.culled) {
			const auto& m = a_depth.cullViewProj;
			snapshot.inView.assign(objects, 0);
			snapshot.bound.assign(objects, {});
			for (std::size_t o = 0; o < objects; ++o) {
				const auto* geometry = snapshot.geometry[o];
				if (!(snapshot.flags[o] & 1) || !geometry)
					continue;
				const auto& b = geometry->worldBound;
				snapshot.bound[o] = { b.center.x, b.center.y, b.center.z, b.radius };
				std::uint32_t outside = 0x3F;  // -x, +x, -y, +y, near, far: every corner outside
				for (std::uint32_t corner = 0; corner < 8; ++corner) {
					const float p[3] = { b.center.x + ((corner & 1) ? b.radius : -b.radius), b.center.y + ((corner & 2) ? b.radius : -b.radius),
						b.center.z + ((corner & 4) ? b.radius : -b.radius) };
					float clip[4];
					for (std::uint32_t row = 0; row < 4; ++row)
						clip[row] = m[row * 4] * p[0] + m[row * 4 + 1] * p[1] + m[row * 4 + 2] * p[2] + m[row * 4 + 3];
					std::uint32_t corners = 0;
					corners |= clip[0] < -clip[3] ? 1u : 0u;
					corners |= clip[0] > clip[3] ? 2u : 0u;
					corners |= clip[1] < -clip[3] ? 4u : 0u;
					corners |= clip[1] > clip[3] ? 8u : 0u;
					corners |= clip[2] < 0.0f ? 16u : 0u;
					corners |= clip[2] > clip[3] ? 32u : 0u;
					outside &= corners;
				}
				snapshot.inView[o] = outside == 0;
			}
		}
		{
			// The claims the registration hooks withhold by this frame, by the object's geometry.
			if (const auto set = PassCapture::Get().CurrentSet()) {
				snapshot.claims.assign(objects, 0);
				for (std::size_t o = 0; o < objects; ++o)
					if (snapshot.geometry[o])
						snapshot.claims[o] = set->PhasesOf(snapshot.geometry[o]);
			}
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
			const auto& tablesNow = SceneStore::Get().GetTables();
			const auto* geometry = input.objectIndex < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[input.objectIndex] : nullptr;
			if (!geometry)
				return;
			const auto& bound = geometry->worldBound;
			const float center[3]{ bound.center.x, bound.center.y, bound.center.z };
			for (std::size_t c = 0; c < sunCascades.size() && !inside; ++c)
				inside = InSunCascade(sunCascades[c], center, bound.radius);
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
					const auto source = buffers.treeWindRows[h] ? RenderGraphRuntime::Get().WrapBuffer(*buffers.treeWindRows[h]->Get(), sourceDesc) : nullptr;
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
		const auto source = RenderGraphRuntime::Get().WrapBuffer(*buffers.treeWindRows[(buffers.treeFrame + 1) & 1]->Get(), sourceDesc);
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
				const auto* entryNode = object < a_tables.sunEntryNode.size() ? a_tables.sunEntryNode[object] : nullptr;
				const float entry = entryNode ? entryNode->worldBound.radius : -1.0f;
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
			if (node && snapshot && base + i < snapshot->size() && !(bits & kFadeRootStoodIn)) {
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
