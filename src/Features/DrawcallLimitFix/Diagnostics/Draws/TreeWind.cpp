// CS_DCLF_PERSISTENT_PARITY: tree wind parity. Every 120th colour epoch the records of up to 64 tree members are read back (3 frames
// later) and checked against the engine's formulas and clocks; and both wind buffers whole, every listed tree's entry.

#include "DrawsState.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
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

	void ReadTreeWind(const MainEpochDone& a_epoch)
	{
		if (a_epoch.depthOnly)
			return;
		const auto& a_resources = a_epoch.resources;
		auto& state = DrawsState::Get();
		auto& treeReadback = state.treeReadback;
		auto& treeTimers = state.treeTimers;
		auto& treeEpochs = state.treeEpochs;
		using TreeReadback = DrawsState::TreeReadback;
		auto* context = globals::d3d::context;
		const auto& tables = Scene::FrameView::Get().GetTables();
		const auto& adopted = DrawsView::Get().Adopted();
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
				// The row TreeWindCS takes, its seed's values (FrameValues' tree seeds) with the coordinator's generation.
				TreeStatic merged;
				const auto* treeSeeds = FrameValues::Get().TreeSeedsIfDone(true);
				const TreeStatic* row = !nodeless && sample.tree < tables.trees.size() && tables.treeNode[sample.tree] == sample.node && treeSeeds &&
				                                MergeTreeSeed(tables.trees[sample.tree], treeSeeds->data(), treeSeeds->size(), sample.tree, merged) ?
				                            &merged :
				                            nullptr;
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
			} else if ((treeEpochs % 120) == 60 && buffers.treeWind && !tables.treeObjects.empty() && adopted.Names(buffers.trees) &&
					   buffers.TreesContentEpoch(adopted) == tables.treesContentEpoch) {
				WholeReadback next;
				next.entries = static_cast<std::uint32_t>(adopted.Of(buffers.trees).units) + 1;
				next.frame = buffers.treeFrame;
				for (const auto& member : tables.treeObjects)
					next.members.emplace_back(member.object, member.tree);
				next.rows = tables.trees;
				if (const auto* treeSeeds = FrameValues::Get().TreeSeedsIfDone(true))
					for (std::size_t t = 0; t < next.rows.size(); ++t)
						MergeTreeSeed(next.rows[t], treeSeeds->data(), treeSeeds->size(), static_cast<std::uint32_t>(t), next.rows[t]);
				next.generations.reserve(tables.trees.size());
				for (const auto& row : tables.trees)
					next.generations.push_back(row.generation);
				{
					// The manager's near list (+0x38, count +0x48), FUN_140437e50's selection this frame.
					const auto manager = *reinterpret_cast<const std::uintptr_t*>(REL::Offset(Engine::Address::kTreeManager).address());
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
							next.nearLargestModel = std::max(next.nearLargestModel, std::abs(next.rows[t].modelAmplitude));
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
		if ((treeEpochs++ % 120) != 0 || !buffers.treeWind || tables.treeObjects.empty() || !adopted.Names(buffers.trees) || buffers.TreesContentEpoch(adopted) != tables.treesContentEpoch)
			return;
		// The first 64 members, each one's tree entry (TreeParams and WindTimers, 32 bytes) of the wind buffer this frame's draws
		// read (TreeWindReadIndex): the one the frame before wrote, from its inputs. Not this frame's own buffer: the compute
		// queue writes that beside the epoch, and nothing orders a copy of it after the write. Only once a frame before this one
		// has run the pass (the first frame's draws read the zeroed buffer).
		if (buffers.previousTreeFrame + 1 != buffers.treeFrame || buffers.previousTreesContentEpoch != tables.treesContentEpoch)
			return;
		TreeReadback readback;
		const std::size_t count = std::min<std::size_t>(64, tables.treeObjects.size());
		D3D11_BUFFER_DESC sourceDesc{};
		sourceDesc.ByteWidth = static_cast<UINT>((adopted.Of(buffers.trees).units + 1) * kTreeWindEntryRows * 16);
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
}
