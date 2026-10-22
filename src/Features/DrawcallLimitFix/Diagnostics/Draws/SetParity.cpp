// CS_DCLF_SET_PARITY: the frame's set against what the GPU drew. Each colour epoch's visibility words (its stamps and verdicts) are
// copied with the CPU's side that explains them, and read three frames later: a member drawn by nobody, an object drawn outside the
// set, the depth and colour segments disagreeing, the registration hooks' claims against the set, and the gaps.

#include "DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	void CheckSetParity(const MainEpochDone& a_epoch)
	{
		if (a_epoch.depthOnly || !a_epoch.depth)
			return;
		const auto& a_resources = a_epoch.resources;
		const MainPayload& a_depth = *a_epoch.depth;
		const MainPayload& a_colour = a_epoch.payload;
		auto& diagnostics = DrawsState::Get();
		auto& setParityFrames = diagnostics.setParityFrames;
		auto& setParityStaging = diagnostics.setParityStaging;
		auto& gapHistory = diagnostics.gapHistory;
		auto& fadeLogFocus = diagnostics.fadeLogFocus;
		using SetParityFrame = DrawsState::SetParityFrame;
		// A gap's first frame, by what rejected it: the frustum stamp and its fade bit, the root's ownership.
		static constexpr const char* kGapClasses[] = { "?", "the frustum", "an owned fade root", "the resident fade test",
			"past the frustum and fade (HZB or tree height)", "no frustum stamp", "no verdict this frame" };
		auto* context = globals::d3d::context;
		auto& counts = diagnostics.setParity;
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
			// TEMP: where the GPU's scene list (the inputs backing copied with the words) names an object.
			const auto gpuInputs = [&](std::size_t a_object) -> std::string {
				if (!snapshot.inputsStaging)
					return "; GPU inputs not copied";
				D3D11_MAPPED_SUBRESOURCE inputsMapped{};
				if (FAILED(context->Map(snapshot.inputsStaging.get(), 0, D3D11_MAP_READ, 0, &inputsMapped)))
					return "; GPU inputs not mapped";
				D3D11_BUFFER_DESC inputsDesc{};
				snapshot.inputsStaging->GetDesc(&inputsDesc);
				const auto* gpu = static_cast<const DrawInput*>(inputsMapped.pData);
				const std::size_t held = inputsDesc.ByteWidth / sizeof(DrawInput);
				std::string at;
				for (std::size_t i = 0; i < held && at.size() < 120; ++i)
					if (gpu[i].objectIndex == a_object)
						at += fmt::format(" {}", i);
				context->Unmap(snapshot.inputsStaging.get(), 0);
				return fmt::format("; the GPU's inputs (backing {}, {} held; colour dispatched {}, depth {}) name it at:{}", snapshot.inputsGeneration, held,
					snapshot.inputCount, snapshot.depthInputCount, at.empty() ? " none" : at);
			};
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
			// The snapshot's listed roots (the fade checks below read them by object).
			const Api::RootListView roots = snapshot.tables ? Extensions::RootListOf(*snapshot.tables) : Api::RootListView{};
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
				// A forward object is colour-only like a decal (no depth segment input, no frustum stamp).
				const bool decal = (o < snapshot.colourState.size() && snapshot.colourState[o] == kObjectStateDecal) ||
				                   (snapshot.tables && o < snapshot.tables->objects.size() && (snapshot.tables->objects[o].flags & kObjectForward));
				counts.recordDisagrees += (flags & 8) ? 1 : 0;
				if (!withheld && (depthDrawn || colourDrawn)) {
					++counts.outsideDrawn;
					if (counts.samples++ < 40) {
						const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
						logger::info("[DCLF] set parity, frame {}: drawn outside the set - object {} '{}' ({}{}); listed by {:#x} (input flags {:#x}){}", snapshot.frame, o,
							geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", depthDrawn ? "depth" : "", colourDrawn ? " colour" : "",
							o < snapshot.listedBy.size() ? snapshot.listedBy[o] : 0, o < snapshot.listedFlags.size() ? snapshot.listedFlags[o] : 0, gpuInputs(o));
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
						// and the engine would have drawn it. By the fade test: a root FadeStateCS has faded out, counted (a far root
						// fades out in view; the fade parities judge the fade), flagged where the engine's node has it fully faded in.
						++(fadeHidden ? counts.fadeHiddenInView : counts.rejectedInView);
						const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
						const bool alive = geometry != nullptr;  // held by snapshot.tables
						if (fadeHidden && alive) {
							// The engine's verdict where it still runs the root's OnVisible (not stood in): its node fully faded in
							// (currentFade and fadeAmount 1) draws the members. FadeStateCS models that node, so dropping them is a
							// fade the engine does not have: a missed input or a port difference.
							const std::uint32_t root = roots.RootOf(static_cast<std::uint32_t>(o));
							const auto* rootNode = static_cast<const RE::NiAVObject*>(roots.NodeOf(root));
							const auto* fadeNode = rootNode ? const_cast<RE::NiAVObject*>(rootNode)->AsFadeNode() : nullptr;
							if (fadeNode && root < roots.Size() && !(roots.rows[root].bits & kFadeRootWritten) &&
								fadeNode->GetRuntimeData().currentFade == 1.0f && Engine::At<float>(rootNode, 0x100) == 1.0f) {
								counts.fadeContradictedLast = snapshot.frame;
								if (counts.fadeContradicted++ == 0)
									counts.fadeContradictedFirst = fmt::format("frame {}: object {} '{}' under root {} '{}' (bits {:#x}, the row's fadeAmount {})", snapshot.frame, o,
										geometry->name.c_str() ? geometry->name.c_str() : "", root, rootNode->name.c_str() ? rootNode->name.c_str() : "", roots.rows[root].bits,
										roots.rows[root].fadeAmount);
							}
						}
						const bool skinned = alive && geometry->GetGeometryRuntimeData().skinInstance;
						counts.skinnedInView += skinned;
						if (alive && (fadeHidden ? counts.fadeSamples++ < 24 : counts.inViewSamples++ < 24)) {
							const RE::TESObjectREFR* owner = nullptr;
							for (const RE::NiAVObject* node = geometry; node && !owner; node = node->parent)
								owner = node->GetUserData();
							const auto* base = owner ? owner->GetBaseObject() : nullptr;
							const auto& tables = *snapshot.tables;
							const std::uint32_t root = roots.RootOf(static_cast<std::uint32_t>(o));
							const auto* rootNode = static_cast<const RE::NiAVObject*>(roots.NodeOf(root));
							const auto* fadeNode = rootNode ? const_cast<RE::NiAVObject*>(rootNode)->AsFadeNode() : nullptr;
							const auto& b = snapshot.bound[o];
							const auto& live = geometry->worldBound;
							logger::info(
								"[DCLF] set parity, frame {}: rejected in view ({}) - object {} '{}'{} of {} '{}': bound at the snapshot ({:.0f} {:.0f} {:.0f}) r {:.0f}, "
								"now ({:.0f} {:.0f} {:.0f}) r {:.0f}; fade root {} '{}' (bits {:#x}, currentFade {:.3f}); colour build {}",
								snapshot.frame, fadeHidden ? "the fade test" : "the frustum", o, geometry->name.c_str() ? geometry->name.c_str() : "", skinned ? " (skinned)" : "",
								base ? RE::FormTypeToString(base->GetFormType()) : "no ref", base && base->GetName() ? base->GetName() : "", b[0], b[1], b[2], b[3], live.center.x,
								live.center.y, live.center.z, live.radius, root == kNoFadeRoot ? -1 : static_cast<std::int64_t>(root),
								rootNode && rootNode->name.c_str() ? rootNode->name.c_str() : "", root < roots.Size() ? roots.rows[root].bits : 0u,
								fadeNode ? fadeNode->GetRuntimeData().currentFade : -1.0f,
								stateName(o < snapshot.colourState.size() ? snapshot.colourState[o] : kObjectStateAbsent));
							// The centre FadeStateCS tests the root at: its chosen member's placement row 8 (LodFadeNodeOf: the member's
							// property's fade node), against the root node's own bound.
							if (fadeHidden && root < roots.Size()) {
								fadeLogFocus.store(root, std::memory_order_relaxed);
								const std::uint32_t member = roots.rows[root].object;
								const auto* memberGeometry = member < tables.objectGeometry.size() ? tables.objectGeometry[member] : nullptr;
								const auto* property = memberGeometry ? memberGeometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
								const auto row = LodFadeNodeOf(property);
								const auto centre = rootNode ? rootNode->worldBound.center : RE::NiPoint3{};
								logger::info("[DCLF] set parity, frame {}:   its root's centre for FadeStateCS: member {} '{}' row 8 ({:.0f} {:.0f} {:.0f} w {:.2f}), property fade node {}; "
											 "the root node's bound ({:.0f} {:.0f} {:.0f}) r {:.0f}; listed near {:.0f} far {:.0f} radius {:.0f}",
									snapshot.frame, member, memberGeometry && memberGeometry->name.c_str() ? memberGeometry->name.c_str() : "?", row[0], row[1], row[2], row[3],
									property && property->fadeNode ? "present" : "none", centre.x, centre.y, centre.z, rootNode ? rootNode->worldBound.radius : -1.0f,
									roots.rows[root].nearDistance, roots.rows[root].farDistance, roots.rows[root].radius);
							}
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
								const std::uint32_t root = roots.RootOf(static_cast<std::uint32_t>(o));
								cls = root < roots.Size() && (roots.rows[root].bits & kFadeRootOwned) ? 2 : 3;
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
					// TEMP: the payload's tables against the snapshot's (a payload built from newer tables names objects these lack).
					const auto* payloadGeometry = snapshot.payloadTables && o < snapshot.payloadTables->objectGeometry.size() ? snapshot.payloadTables->objectGeometry[o] : nullptr;
					logger::info("[DCLF] set parity, frame {}: listed by {:#x} (input flags {:#x})", snapshot.frame, o < snapshot.listedBy.size() ? snapshot.listedBy[o] : 0,
						o < snapshot.listedFlags.size() ? snapshot.listedFlags[o] : 0);
					logger::info("[DCLF] set parity, frame {}: payload tables {} ({} objects against {}), there '{}'", snapshot.frame,
						!snapshot.payloadTables ? "none" : snapshot.payloadTables == snapshot.tables ? "the snapshot's" : "other",
						snapshot.payloadTables ? snapshot.payloadTables->objects.size() : 0, snapshot.flags.size(),
						payloadGeometry && payloadGeometry->name.c_str() ? payloadGeometry->name.c_str() : "?");
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
				// Counted, not flagged: the frustum's few are a bound at the view's edge or a far LOD block's, the fade test's a far root's
				// fade. Flagged: a fade rejection the engine's own node, where its cull still runs, contradicts.
				logger::info("[DCLF] set parity over {} frames: members rejected with their live bound in view: {} by the frustum, {} by the fade test (roots faded "
							 "out), {} of all of them skinned; {} fade rejections the engine's node contradicts (the last at frame {}){}{}",
					counts.frames, counts.rejectedInView, counts.fadeHiddenInView, counts.skinnedInView, counts.fadeContradicted, counts.fadeContradictedLast,
					counts.fadeContradicted ? " <- FADE" : " <- OK",
					counts.fadeContradictedFirst.empty() ? "" : "; first: " + counts.fadeContradictedFirst);
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
		if (&a_depth == &a_colour || setParityFrames.size() >= 8)
			return;
		auto* const visibilityView = diagnostics.setParityVisibility.Of(a_resources->visibility);
		if (!visibilityView)
			return;
		auto* const frustumView = diagnostics.setParityFrustum.Of(a_resources->frustum);
		SetParityFrame snapshot;
		D3D11_BUFFER_DESC desc{};
		visibilityView->GetDesc(&desc);
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
		context->CopyResource(snapshot.staging.get(), visibilityView);
		if (frustumView) {
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
				context->CopyResource(snapshot.frustumStaging.get(), frustumView);
		}
		// The frame the epochs drew (their visibility stamps), not the one the payloads were built in (ahead).
		snapshot.frame = Scene::FrameView::Get().GetFrame();
		snapshot.framesLeft = 3;
		snapshot.depthState = a_depth.body->objectState;
		snapshot.colourState = a_colour.body->objectState;
		// Only a publication's tables: the coordinator's own (none accepted yet) are not the render thread's to hold.
		snapshot.tables = Scene::FrameView::Get().AcceptedTables();
		snapshot.payloadTables = a_colour.body->streams ? a_colour.body->streams->tables : nullptr;
		{
			const std::size_t count = Scene::FrameView::Get().GetTables().objects.size();
			snapshot.listedBy.assign(count, 0);
			snapshot.listedFlags.assign(count, 0);
			const auto note = [&](const DrawInput& a_input, std::uint8_t a_bit) {
				if (a_input.objectIndex >= count)
					return;
				if (!snapshot.listedBy[a_input.objectIndex])
					snapshot.listedFlags[a_input.objectIndex] = a_input.flags;
				snapshot.listedBy[a_input.objectIndex] |= a_bit;
			};
			for (std::size_t i = 0; i < a_colour.resident.Count(); ++i)
				note(*a_colour.resident.At(i), 1);
			for (const auto& input : a_colour.body->inputList)
				note(input, 2);
			for (std::size_t i = 0; i < a_depth.resident.Count(); ++i)
				note(*a_depth.resident.At(i), 4);
			for (const auto& input : a_depth.body->inputList)
				note(input, 8);
		}
		if (!snapshot.tables) {
			setParityStaging.push_back(std::move(snapshot.staging));
			if (snapshot.frustumStaging)
				setParityStaging.push_back(std::move(snapshot.frustumStaging));
			return;
		}
		const auto& tables = Scene::FrameView::Get().GetTables();
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
			if (Scene::FrameView::Get().SetPhasesOf(static_cast<std::int32_t>(o)) & kSetMain)
				flags |= 1;
			if (Scene::FrameView::Get().IsMember(static_cast<std::int32_t>(o)))
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
		// TEMP: the GPU's scene list (the installed publication's inputs backing), copied after the colour epoch like the words.
		if (const auto* versions = a_epoch.payloadVersions) {
			if (const auto& lease = versions->leases[static_cast<std::size_t>(Draws::PayloadPart::Inputs)]; lease && lease->buffer) {
				static std::uint64_t wrappedGeneration = 0;
				static winrt::com_ptr<ID3D11Buffer> wrapped;
				if (!wrapped || wrappedGeneration != lease->generation) {
					D3D11_BUFFER_DESC wrap{};
					wrap.ByteWidth = static_cast<UINT>(lease->buffer->GetSize());
					wrap.Usage = D3D11_USAGE_DEFAULT;
					wrap.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					wrap.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
					wrap.StructureByteStride = sizeof(std::uint32_t);
					wrapped = RenderGraphRuntime::Get().WrapBuffer(*lease->buffer, wrap);
					wrappedGeneration = lease->generation;
				}
				if (wrapped) {
					D3D11_BUFFER_DESC staging{};
					wrapped->GetDesc(&staging);
					staging.Usage = D3D11_USAGE_STAGING;
					staging.BindFlags = 0;
					staging.MiscFlags = 0;
					staging.StructureByteStride = 0;
					staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					if (SUCCEEDED(globals::d3d::device->CreateBuffer(&staging, nullptr, snapshot.inputsStaging.put()))) {
						context->CopyResource(snapshot.inputsStaging.get(), wrapped.get());
						snapshot.inputsGeneration = lease->generation;
						snapshot.inputCount = static_cast<std::uint32_t>(a_colour.body->inputList.size() + a_colour.resident.Count());
						snapshot.depthInputCount = static_cast<std::uint32_t>(a_depth.body->inputList.size() + a_depth.resident.Count());
					}
				}
			}
		}
		setParityFrames.push_back(std::move(snapshot));
	}
}
