// CS_DCLF_PERSISTENT_PARITY: tree wind parity. Every 120th colour epoch the records of up to 64 tree members are read back (3 frames
// later) and checked against the engine's formulas and clocks; and both wind buffers whole, every listed tree's entry. A member whose
// entry holds another generation than its listing's draws with its record's values (DCLFTreeWindEntry), and is counted, not checked.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindTables.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsView.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

namespace DCLF::Features::TreeWind
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

		/**
		 * @brief Up to 64 tree members' records, read back and checked against the engine's formulas; timer drift is reported.
		 */
		struct TreeReadback
		{
			winrt::com_ptr<ID3D11Buffer> records;
			std::uint32_t framesLeft = 0;
			struct Sample
			{
				std::uint32_t object = 0, tree = 0;
				std::uint32_t generation = 0;  // the listing's (kNodelessTree for the nodeless entry)
				const void* node = nullptr;
			};
			std::vector<Sample> samples;
			TreeWindFrame inputs{};
		};

		/** @brief Both wind buffers whole, with what the listed trees were when the copy was taken. */
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

		// Render thread (the colour epoch's MainEpochDone): the readbacks in flight, the nodes' timers at the last member readback, the
		// colour epochs counted.
		std::optional<TreeReadback> treeReadback;
		ankerl::unordered_dense::map<const void*, std::pair<float, float>> treeTimers;  // node -> engine's and GPU's timers at the last readback
		std::uint32_t treeEpochs = 0;
		std::optional<WholeReadback> whole;

		// One member readback's tallies (HarvestMembers).
		struct MemberCheck
		{
			std::uint32_t checked = 0, paramsDiffer = 0, fadeDiffer = 0, gustDiffer = 0, gusts = 0;
			// Members drawn with their records' values: their entry is not their listing's (TreeWindCS wrote none for it yet: its
			// seed not ready, or a reused slot's earlier listing).
			std::uint32_t fallbacks = 0;
			float drift = 0.0f;
			std::string first;
			// The engine's clock rate against the GPU's, per node, since the last readback that saw it: by distance (within the
			// manager's range or not) and whether the model has bones (+0xB8, the near loop's condition).
			std::array<std::array<std::uint32_t, 4>, 4> rates{};  // [near * 2 + bones][ratio ~0, ~1, ~2, other]
			ankerl::unordered_dense::map<const void*, std::pair<float, float>> seen;
		};

		// One member's record (TreeParams, WindTimers) against the engine's formulas and its node's clock.
		void CheckMember(const TreeReadback::Sample& a_sample, const float (&tree)[8], const TreeWindFrame& in, const TreeTables& tables, MemberCheck& c)
		{
			++c.checked;
			const bool nodeless = a_sample.tree == kNodelessTree;
			// The tree's static row, while the node is still listed under the same slot.
			// The row TreeWindCS takes, its seed's values (the seeds' pool) with the coordinator's generation.
			TreeStatic merged;
			const auto* treeSeeds = SeedsIfDone(true);
			const TreeStatic* row = !nodeless && a_sample.tree < tables.trees.size() && tables.treeNode[a_sample.tree] == a_sample.node && treeSeeds &&
			                                MergeTreeSeed(tables.trees[a_sample.tree], treeSeeds->data(), treeSeeds->size(), a_sample.tree, merged) ?
			                            &merged :
			                            nullptr;
			const float leafFrequency = nodeless ? 1.0f : row ? row->leafFrequency : tree[3];
			if (tree[0] != 0.0f || tree[1] != in.windMagnitude || tree[3] != leafFrequency) {
				if (c.paramsDiffer++ == 0 && c.first.empty())
					c.first = fmt::format("object {} TreeParams ({}, {}, {}, {}), wind magnitude {}, leaf frequency {}", a_sample.object, tree[0], tree[1], tree[2], tree[3],
						in.windMagnitude, leafFrequency);
			}
			const float amplitude = tree[7];
			const float distance = nodeless ? 0.0f : TreeFastSqrt(tree[6]);
			const float faded = std::min(std::max((1.0f - (distance - in.fadeStart) / (in.fadeEnd - in.fadeStart)) * amplitude, 0.0f), amplitude);
			if (std::abs(faded - tree[2]) > 1e-5f * std::max(1.0f, std::abs(faded))) {
				if (c.fadeDiffer++ == 0 && c.first.empty())
					c.first = fmt::format("object {} faded amplitude {} (expected {} from amplitude {} at distance {})", a_sample.object, tree[2], faded, amplitude, distance);
			}
			if (row && row->animated && in.timerScale != 0.0f && tree[6] < in.maxDistance2 && tree[4] != row->timer * in.timerScale) {
				++c.gusts;
				const float timer = tree[4] / in.timerScale;
				const float gust = TreeGust(in.windSpeed * timer) * row->modelAmplitude;
				if (std::abs(gust - amplitude) > 1e-3f * std::max(1.0f, std::abs(row->modelAmplitude))) {
					if (c.gustDiffer++ == 0 && c.first.empty())
						c.first = fmt::format("object {} amplitude {} (the gust at timer {} is {})", a_sample.object, amplitude, timer, gust);
				}
				const float engineTimer = *reinterpret_cast<const float*>(static_cast<const std::byte*>(a_sample.node) + 0x164);
				c.drift = std::max(c.drift, std::abs(timer - engineTimer));
				if (const auto it = treeTimers.find(a_sample.node); it != treeTimers.end() && timer > it->second.second) {
					const float ratio = (engineTimer - it->second.first) / (timer - it->second.second);
					const auto* holder = *reinterpret_cast<const std::byte* const*>(static_cast<const std::byte*>(a_sample.node) + 0xF8);
					const auto* model = holder ? *reinterpret_cast<const std::byte* const*>(holder + 0x40) : nullptr;
					const bool bones = model && *reinterpret_cast<const void* const*>(model + 0xB8);
					const std::size_t bucket = std::abs(ratio) < 0.1f ? 0 : std::abs(ratio - 1.0f) < 0.1f ? 1 : std::abs(ratio - 2.0f) < 0.1f ? 2 : 3;
					++c.rates[(tree[6] < in.maxDistance2 ? 2 : 0) + (bones ? 1 : 0)][bucket];
				}
				c.seen[a_sample.node] = { engineTimer, timer };
			}
		}

		// The member readback, three frames after its copy: every member checked, the clock rates and the parity's lines.
		void HarvestMembers(TreeReadback& a_readback, const TreeTables& a_tables, ID3D11DeviceContext* a_context)
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(a_context->Map(a_readback.records.get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return;
			const auto* bytes = static_cast<const std::byte*>(mapped.pData);
			MemberCheck c;
			for (std::size_t i = 0; i < a_readback.samples.size(); ++i) {
				constexpr std::size_t entryBytes = kTreeWindEntryRows * 16;
				float tree[8];
				std::memcpy(tree, bytes + i * entryBytes, sizeof(tree));
				std::uint32_t generation;
				std::memcpy(&generation, bytes + i * entryBytes + 32, sizeof(generation));
				if (generation != a_readback.samples[i].generation) {
					++c.fallbacks;
					continue;
				}
				CheckMember(a_readback.samples[i], tree, a_readback.inputs, a_tables, c);
			}
			a_context->Unmap(a_readback.records.get(), 0);
			treeTimers = std::move(c.seen);
			const auto& rates = c.rates;
			logger::info("[DCLF] tree wind clock rates (engine over GPU; ~0/~1/~2/other): far {}/{}/{}/{}, far with bones {}/{}/{}/{}, near {}/{}/{}/{}, near with bones {}/{}/{}/{}",
				rates[0][0], rates[0][1], rates[0][2], rates[0][3], rates[1][0], rates[1][1], rates[1][2], rates[1][3], rates[2][0], rates[2][1], rates[2][2], rates[2][3],
				rates[3][0], rates[3][1], rates[3][2], rates[3][3]);
			logger::info("[DCLF] tree wind parity: {} members checked ({} gusts), {} drawn with their records' (no entry of their listing); {} TreeParams, {} fades, {} gusts differ; timers {:.3f} s from the engine's own clocks at most{}{}{}",
				c.checked, c.gusts, c.fallbacks, c.paramsDiffer, c.fadeDiffer, c.gustDiffer, c.drift, c.paramsDiffer || c.fadeDiffer || c.gustDiffer ? " <- TREE WIND" : " <- OK",
				c.first.empty() ? "" : "; first: ", c.first);
		}

		// One wind buffer of the whole readback (mapped rows): every member's entry, its line's part.
		std::string CheckWholeBuffer(const WholeReadback& a_whole, std::uint32_t a_buffer, const float* a_rows)
		{
			std::uint32_t bad = 0, stale = 0, checked = 0;
			float largest = 0.0f;
			std::string first, worst;
			for (const auto& [object, tree] : a_whole.members) {
				const std::uint32_t entry = tree == kNodelessTree ? 0u : tree + 1;
				if (entry >= a_whole.entries)
					continue;
				const float* e = a_rows + std::size_t(entry) * kTreeWindEntryRows * 4;
				++checked;
				const std::uint32_t generation = std::bit_cast<std::uint32_t>(e[8]);
				const bool listed = tree != kNodelessTree && tree < a_whole.generations.size();
				if (listed && generation != a_whole.generations[tree])
					++stale;
				const float model = listed && tree < a_whole.rows.size() ? std::abs(a_whole.rows[tree].modelAmplitude) : 0.0f;
				if (std::abs(e[2]) > largest) {
					largest = std::abs(e[2]);
					worst = fmt::format("object {} tree {:#x}: on the engine's near list {}, faded {} amplitude {}, listing amplitude {}, the engine's now {}, model amplitude {}, animated {}, distance {:.0f}",
						object, tree, listed && tree < a_whole.nearList.size() ? a_whole.nearList[tree] : 0u, e[2], e[7], listed && tree < a_whole.rows.size() ? a_whole.rows[tree].amplitude : 0.0f,
						listed && tree < a_whole.engineAmplitude.size() ? a_whole.engineAmplitude[tree] : 0.0f, model,
						listed && tree < a_whole.rows.size() ? a_whole.rows[tree].animated : 0u, std::sqrt(std::max(e[6], 0.0f)));
				}
				bool finite = true;
				for (int k = 0; k < 8; ++k)
					finite = finite && std::isfinite(e[k]);
				if (!finite || std::abs(e[2]) > 1000.0f || std::abs(e[7]) > 1000.0f) {
					if (bad++ == 0)
						first = fmt::format("object {} tree {:#x}: params ({} {} {} {}), timers ({} {} {} {}), generation {:#x} (listing {:#x}), model amplitude {}", object, tree,
							e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], generation, listed ? a_whole.generations[tree] : 0u, model);
				}
			}
			return fmt::format("; buffer {}: {} entries checked, {} bad, {} with another generation, largest amplitude {}{}{}", a_buffer, checked, bad, stale, largest,
				first.empty() ? "" : " (first " + first + ")", worst.empty() ? "" : " (largest: " + worst + ")");
		}

		// The whole readback, three frames after its copies: both buffers' lines.
		void HarvestWhole(const WholeReadback& a_whole, ID3D11DeviceContext* a_context)
		{
			std::string text;
			for (std::uint32_t h = 0; h < 2; ++h) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (!a_whole.staging[h] || FAILED(a_context->Map(a_whole.staging[h].get(), 0, D3D11_MAP_READ, 0, &mapped)))
					continue;
				const auto* rows = static_cast<const float*>(mapped.pData);
				const auto part = CheckWholeBuffer(a_whole, h, rows);
				a_context->Unmap(a_whole.staging[h].get(), 0);
				text += part;
			}
			logger::info("[DCLF] tree wind buffers (frame {}, read index {}): the engine's near list holds {} nodes, {} of them DCLF's trees, their largest model amplitude {}{}",
				a_whole.frame, a_whole.frame + 1, a_whole.nearCount, a_whole.nearListed, a_whole.nearLargestModel, text);
		}

		// The manager's near list (+0x38, count +0x48), FUN_140437e50's selection this frame, against the listed trees.
		void ReadNearList(const TreeTables& a_tables, WholeReadback& a_next)
		{
			const auto manager = *reinterpret_cast<const std::uintptr_t*>(REL::Offset(Address::kTreeManager).address());
			ankerl::unordered_dense::set<const void*> nearNodes;
			if (manager) {
				const auto* nodes = *reinterpret_cast<const void* const* const*>(manager + 0x38);
				const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(manager + 0x48);
				for (std::uint32_t n = 0; nodes && n < count; ++n)
					nearNodes.insert(nodes[n]);
				a_next.nearCount = count;
			}
			a_next.nearList.assign(a_tables.trees.size(), 0);
			for (std::size_t t = 0; t < a_tables.trees.size(); ++t) {
				const void* node = t < a_tables.treeNode.size() ? a_tables.treeNode[t] : nullptr;
				if (node && nearNodes.contains(node)) {
					a_next.nearList[t] = 1;
					++a_next.nearListed;
					a_next.nearLargestModel = std::max(a_next.nearLargestModel, std::abs(a_next.rows[t].modelAmplitude));
				}
			}
		}

		// Both wind buffers copied whole, with the listed trees as they are now.
		void KickWhole(const TreeWindScene& a_wind, const Draws::NamedVersions& a_adopted, const TreeTables& a_tables, ID3D11DeviceContext* a_context)
		{
			WholeReadback next;
			next.entries = static_cast<std::uint32_t>(a_adopted.Of(a_wind.trees).units) + 1;
			next.frame = a_wind.treeFrame;
			for (const auto& member : a_tables.treeObjects)
				next.members.emplace_back(member.object, member.tree);
			next.rows = a_tables.trees;
			if (const auto* treeSeeds = SeedsIfDone(true))
				for (std::size_t t = 0; t < next.rows.size(); ++t)
					MergeTreeSeed(next.rows[t], treeSeeds->data(), treeSeeds->size(), static_cast<std::uint32_t>(t), next.rows[t]);
			next.generations.reserve(a_tables.trees.size());
			for (const auto& row : a_tables.trees)
				next.generations.push_back(row.generation);
			ReadNearList(a_tables, next);
			next.engineAmplitude.reserve(a_tables.trees.size());
			for (std::size_t t = 0; t < a_tables.trees.size(); ++t) {
				const void* node = t < a_tables.treeNode.size() ? a_tables.treeNode[t] : nullptr;
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
				const auto source = a_wind.treeWindRows[h] ? RenderGraphRuntime::Get().WrapBuffer(*a_wind.treeWindRows[h]->Get(), sourceDesc) : nullptr;
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = bytes;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (source && SUCCEEDED(globals::d3d::device->CreateBuffer(&desc, nullptr, next.staging[h].put())))
					a_context->CopyResource(next.staging[h].get(), source.get());
			}
			next.framesLeft = 3;
			whole = std::move(next);
		}

		// The first 64 members, each one's tree entry (TreeParams, WindTimers and the generation) of the wind buffer this frame's draws
		// read (the frame binding's): the one the frame before wrote, from its inputs.
		void KickMembers(const TreeWindScene& a_wind, const Draws::NamedVersions& a_adopted, const TreeTables& a_tables, ID3D11DeviceContext* a_context)
		{
			TreeReadback readback;
			const std::size_t count = std::min<std::size_t>(64, a_tables.treeObjects.size());
			D3D11_BUFFER_DESC sourceDesc{};
			sourceDesc.ByteWidth = static_cast<UINT>((a_adopted.Of(a_wind.trees).units + 1) * kTreeWindEntryRows * 16);
			sourceDesc.Usage = D3D11_USAGE_DEFAULT;
			sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			sourceDesc.StructureByteStride = sizeof(std::uint32_t);
			const auto source = RenderGraphRuntime::Get().WrapBuffer(*a_wind.treeWindRows[(a_wind.treeFrame + 1) & 1]->Get(), sourceDesc);
			if (!source)
				return;
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(count * kTreeWindEntryRows * 16);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.records.put())))
				return;
			ScopedPerfEvent event("CS DCLF: tree wind readback");
			for (std::size_t i = 0; i < count; ++i) {
				const auto& member = a_tables.treeObjects[i];
				const std::uint32_t entry = member.tree == kNodelessTree ? 0u : member.tree + 1;
				const auto offset = static_cast<UINT>(std::uint64_t(entry) * kTreeWindEntryRows * 16);
				const D3D11_BOX box{ offset, 0, 0, offset + kTreeWindEntryRows * 16, 1, 1 };
				a_context->CopySubresourceRegion(readback.records.get(), 0, static_cast<UINT>(i * kTreeWindEntryRows * 16), 0, 0, source.get(), 0, &box);
				const bool listed = member.tree < a_tables.trees.size();
				readback.samples.push_back({ member.object, member.tree, member.tree == kNodelessTree ? kNodelessTree : listed ? a_tables.trees[member.tree].generation : 0u,
					listed && member.tree < a_tables.treeNode.size() ? a_tables.treeNode[member.tree] : nullptr });
			}
			readback.inputs = a_wind.previousTreeInputs;
			readback.framesLeft = 3;
			treeReadback = std::move(readback);
		}

		void ReadTreeWind(const Diagnostics::MainEpochDone& a_epoch)
		{
			if (a_epoch.depthOnly)
				return;
			auto* context = globals::d3d::context;
			const auto& tables = TreesOf(Scene::FrameView::Get().GetTables());
			const auto& adopted = Diagnostics::DrawsView::Get().Adopted();
			if (treeReadback) {
				if (--treeReadback->framesLeft)
					return;
				auto readback = std::move(*treeReadback);
				treeReadback.reset();
				HarvestMembers(readback, tables, context);
				return;
			}
			const auto* wind = SceneOf(*a_epoch.resources->scene);
			if (!wind)
				return;
			const auto& buffers = *wind;
			// Both wind buffers whole, every listed tree's entry: non-finite values, an amplitude far past its model's, or a
			// generation that is not its listing's (a draw then falls back to its record).
			if (whole) {
				if (--whole->framesLeft == 0) {
					HarvestWhole(*whole, context);
					whole.reset();
				}
			} else if ((treeEpochs % 120) == 60 && buffers.treeWind && !tables.treeObjects.empty() && adopted.Names(buffers.trees) &&
					   buffers.TreesContentEpoch(adopted) == tables.treesContentEpoch) {
				KickWhole(buffers, adopted, tables, context);
			}
			if ((treeEpochs++ % 120) != 0 || !buffers.treeWind || tables.treeObjects.empty() || !adopted.Names(buffers.trees) || buffers.TreesContentEpoch(adopted) != tables.treesContentEpoch)
				return;
			// Not this frame's own buffer: the compute queue writes that beside the epoch, and nothing orders a copy of it after the
			// write. Only once a frame before this one has run the pass (the first frame's draws read the zeroed buffer).
			if (buffers.previousTreeFrame + 1 != buffers.treeFrame || buffers.previousTreesContentEpoch != tables.treesContentEpoch)
				return;
			KickMembers(buffers, adopted, tables, context);
		}
	}

	void SubscribeParity()
	{
		// A colour epoch's readbacks, after Core's (Diagnostics/Draws/DrawsInstall.cpp).
		Diagnostics::Subscribe<Diagnostics::MainEpochDone>(&ReadTreeWind);
	}
}
