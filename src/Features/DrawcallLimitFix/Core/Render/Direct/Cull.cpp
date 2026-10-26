// The CPU-driven pipeline's cull (Cull.h): BuildDrawsCS.hlsl's per-input decisions (BuildInput, Culled, ParabolicRejected,
// StampFrustum, LodPartitions, LocalShadowMask, InSunCascades, OutsideSunEntry, RootFading, FadedOutOfOcclusion) over a ViewBatch's lanes,
// the frame's CPU state's publication, and the main camera's verdicts diffed into the visibility decoder (VisibilityEventsCS on the CPU).
// Keep the decisions the shader's: a change to one is a change to both.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"

#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <Tracy/Tracy.hpp>

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace DCLF::Direct
{
	namespace
	{
		constexpr std::uint32_t kNoRoot = 0xFFFFFFFFu;
		constexpr std::uint32_t kNoProgram = 0xFFFFFFFFu;  // Portals' kPortalNoProgram
		constexpr std::uint32_t kObjectCrossfadeCopy = 1u << 30;
		constexpr std::uint32_t kDrawAll = 0xFFFFu;  // CulledBatch::drawMasks: every sequence, none as the copy
		// The frustum stamp word (BuildDrawsCS): the stamp in the low 28 bits, the fade test's drop in bit 31.
		constexpr std::uint32_t kStampMask = 0x0FFFFFFFu;
		constexpr std::uint32_t kStampFadeHidden = 0x80000000u;
		// Skylighting's occlusion map size test (kCullMinRadius): a bound radius of 32 or less draws nothing.
		constexpr float kOcclusionMinRadius = 32.0f;

		bool MainCamera(ViewType a_type)
		{
			return a_type == ViewType::Main || a_type == ViewType::Decal || a_type == ViewType::Forward || a_type == ViewType::Refraction;
		}
		// The shadow epoch's views (single phase, a caster key word): their casters under a fading root are dropped.
		bool ShadowFamily(ViewType a_type)
		{
			return a_type == ViewType::Sun || a_type == ViewType::Spot || a_type == ViewType::Point || a_type == ViewType::SkyOccluder ||
			       a_type == ViewType::PrecipOccluder;
		}

		// ---- The frustum stamps: per object slot, the frame its bound was last in the main camera's frustum (and portals), and whether
		// its root's fade dropped it then. A fixed directory over the object word's 26-bit index range, chunks made on first write
		// (compare-exchange: a loser frees its own), never moved or freed while DCLF runs. ----

		class StampTable
		{
		public:
			static constexpr std::uint32_t kChunkShift = 16;
			static constexpr std::uint32_t kChunk = 1u << kChunkShift;
			static constexpr std::uint32_t kChunks = (kObjectIndexMask + 1) >> kChunkShift;

			~StampTable()
			{
				for (auto& chunk : chunks)
					delete[] chunk.load(std::memory_order_acquire);
			}
			std::uint32_t Load(std::uint32_t a_object) const
			{
				const auto* chunk = chunks[(a_object & kObjectIndexMask) >> kChunkShift].load(std::memory_order_acquire);
				return chunk ? chunk[a_object & (kChunk - 1)].load(std::memory_order_relaxed) : 0u;
			}
			void Store(std::uint32_t a_object, std::uint32_t a_word)
			{
				Chunk(a_object)[a_object & (kChunk - 1)].store(a_word, std::memory_order_relaxed);
			}

		private:
			std::atomic<std::uint32_t>* Chunk(std::uint32_t a_object)
			{
				auto& slot = chunks[(a_object & kObjectIndexMask) >> kChunkShift];
				if (auto* chunk = slot.load(std::memory_order_acquire))
					return chunk;
				auto* made = new std::atomic<std::uint32_t>[kChunk]();
				std::atomic<std::uint32_t>* expected = nullptr;
				if (slot.compare_exchange_strong(expected, made, std::memory_order_acq_rel, std::memory_order_acquire))
					return made;
				delete[] made;
				return expected;
			}
			std::array<std::atomic<std::atomic<std::uint32_t>*>, kChunks> chunks{};
		};
		StampTable stamps;

		// ---- The frame state: the arm (render thread), the producer's rows (producer thread), the published frames ----

		struct Arm
		{
			std::shared_ptr<const ViewBatchSet> set;
			std::uint32_t frame = 0, lastArmed = 0;
			bool armed = false, everArmed = false;
		};
		Arm arm;  // render thread
		std::atomic<std::uint32_t> cpuStatesFrame{ ~0u };
		struct RowStash
		{
			std::uint64_t seq = 0;
			const BindlessPlacement* rows = nullptr;
			std::size_t count = 0;
		};
		RowStash rowStash;  // the producers' (one at a time)

		// The newest CullFrame: readers count themselves before loading (sequentially consistent both sides), so a publish that sees no
		// reader frees what it replaced; else the replaced ones wait for a publish that sees none (PortalViews' VisibleSetHold).
		struct Held
		{
			std::shared_ptr<const CullFrame> frame;
		};
		std::atomic<const Held*> latest{ nullptr };
		std::atomic<std::uint32_t> latestReaders{ 0 };
		std::vector<const Held*> retired;  // the publisher's (one at a time: FrameValues' producers)

		struct Waiter
		{
			std::uint32_t frame = 0;
			std::function<void(const std::shared_ptr<const CullFrame>&)> then;
			Waiter* next = nullptr;
		};
		std::atomic<Waiter*> waiters{ nullptr };

		void PushWaiter(Waiter* a_waiter)
		{
			a_waiter->next = waiters.load(std::memory_order_relaxed);
			while (!waiters.compare_exchange_weak(a_waiter->next, a_waiter, std::memory_order_release, std::memory_order_relaxed)) {}
		}
		bool Reached(std::uint32_t a_published, std::uint32_t a_wanted) { return static_cast<std::int32_t>(a_published - a_wanted) >= 0; }

		/** @brief Runs every waiter the newest frame satisfies (each taken by one exchange: it runs once); the rest go back. */
		void RunReady()
		{
			for (;;) {
				const auto current = LatestCullFrame();
				if (!current)
					return;
				Waiter* later = nullptr;
				for (Waiter* w = waiters.exchange(nullptr, std::memory_order_acq_rel); w;) {
					Waiter* following = w->next;
					if (Reached(current->sceneFrame, w->frame)) {
						w->then(current);
						delete w;
					} else {
						w->next = later;
						later = w;
					}
					w = following;
				}
				if (!later)
					return;
				for (Waiter* w = later; w;) {
					Waiter* following = w->next;
					PushWaiter(w);
					w = following;
				}
				// A frame published while they were out drained before they went back: look again.
				const auto now = LatestCullFrame();
				if (!now || now->sceneFrame == current->sceneFrame)
					return;
			}
		}

		// ---- The tests ----

		/** @brief A view's sphere tests against planes: (n, d), the bound outside where dot(n, c) - d < -r. */
		struct PlaneSet
		{
			std::array<std::array<float, 4>, 12> planes{};
			std::uint32_t count = 0;
			void Add(const std::array<float, 4>& a_plane) { planes[count++] = a_plane; }
		};

		PlaneSet PlanesOf(const CullView& a_view)
		{
			PlaneSet set;
			for (std::uint32_t p = 0; p < 6; ++p)
				if (a_view.planeMask & (1u << p))
					set.Add(a_view.planes[p]);
			for (std::uint32_t p = 0; p < 6; ++p) {
				// A clamped shadow view pancakes casters in front of its near plane onto it (kCullNoNearPlane).
				if (p == 4 && (a_view.cullFlags & Draws::kCullNoNearPlane))
					continue;
				if (a_view.clipPlanes[p][0] != 0.0f || a_view.clipPlanes[p][1] != 0.0f || a_view.clipPlanes[p][2] != 0.0f)
					set.Add(a_view.clipPlanes[p]);
			}
			return set;
		}

		struct Spheres
		{
			const float *x, *y, *z, *r;
		};

		/**
		 * @brief The frustum over a batch's lanes, four at a time (SSE2, the build's baseline): a_pass[i] 1 inside every plane. A radius below 0
		 * (a per-snapshot cull's mover: its snapshot sphere unknown) is kept.
		 */
		void FrustumPass(const Spheres& a_spheres, std::uint32_t a_count, const PlaneSet& a_set, std::uint8_t* a_pass)
		{
			std::uint32_t i = 0;
			for (; i + 4 <= a_count; i += 4) {
				const __m128 x = _mm_loadu_ps(a_spheres.x + i), y = _mm_loadu_ps(a_spheres.y + i), z = _mm_loadu_ps(a_spheres.z + i);
				const __m128 r = _mm_loadu_ps(a_spheres.r + i);
				const __m128 reach = _mm_sub_ps(_mm_setzero_ps(), r);
				__m128 keep = _mm_castsi128_ps(_mm_set1_epi32(-1));
				for (std::uint32_t p = 0; p < a_set.count; ++p) {
					const auto& plane = a_set.planes[p];
					const __m128 d = _mm_sub_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(plane[0]), x), _mm_mul_ps(_mm_set1_ps(plane[1]), y)),
													_mm_mul_ps(_mm_set1_ps(plane[2]), z)),
						_mm_set1_ps(plane[3]));
					keep = _mm_and_ps(keep, _mm_cmpge_ps(d, reach));
				}
				keep = _mm_or_ps(keep, _mm_cmplt_ps(r, _mm_setzero_ps()));
				const int bits = _mm_movemask_ps(keep);
				for (std::uint32_t k = 0; k < 4; ++k)
					a_pass[i + k] = static_cast<std::uint8_t>((bits >> k) & 1);
			}
			for (; i < a_count; ++i) {
				bool keep = true;
				for (std::uint32_t p = 0; p < a_set.count && keep; ++p) {
					const auto& plane = a_set.planes[p];
					keep = plane[0] * a_spheres.x[i] + plane[1] * a_spheres.y[i] + plane[2] * a_spheres.z[i] - plane[3] >= -a_spheres.r[i];
				}
				a_pass[i] = keep || a_spheres.r[i] < 0.0f ? 1u : 0u;
			}
		}

		/**
		 * @brief ParabolicSphereMask's first test over a batch's lanes, four at a time: a_pass[i] 0 where the bound misses the view's sphere
		 * (code 0: rejected on either side), in the scalar test's order; a radius below 0 (ParabolicRejected keeps it) passes.
		 */
		void ParabolicPass(const Spheres& a_spheres, std::uint32_t a_count, const std::array<float, 4>& a_sphere, std::uint8_t* a_pass)
		{
			const __m128 sx = _mm_set1_ps(a_sphere[0]), sy = _mm_set1_ps(a_sphere[1]), sz = _mm_set1_ps(a_sphere[2]), sr = _mm_set1_ps(a_sphere[3]);
			std::uint32_t i = 0;
			for (; i + 4 <= a_count; i += 4) {
				const __m128 r = _mm_loadu_ps(a_spheres.r + i);
				const __m128 dx = _mm_sub_ps(_mm_loadu_ps(a_spheres.x + i), sx), dy = _mm_sub_ps(_mm_loadu_ps(a_spheres.y + i), sy),
							 dz = _mm_sub_ps(_mm_loadu_ps(a_spheres.z + i), sz);
				const __m128 reach = _mm_sub_ps(_mm_sub_ps(_mm_sqrt_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz))), r), sr);
				const int bits = _mm_movemask_ps(_mm_or_ps(_mm_cmplt_ps(reach, _mm_setzero_ps()), _mm_cmplt_ps(r, _mm_setzero_ps())));
				for (std::uint32_t k = 0; k < 4; ++k)
					a_pass[i + k] = static_cast<std::uint8_t>((bits >> k) & 1);
			}
			for (; i < a_count; ++i) {
				const float dx = a_spheres.x[i] - a_sphere[0], dy = a_spheres.y[i] - a_sphere[1], dz = a_spheres.z[i] - a_sphere[2];
				a_pass[i] = a_spheres.r[i] < 0.0f || (std::sqrt((dx * dx + dy * dy) + dz * dz) - a_spheres.r[i]) - a_sphere[3] < 0.0f ? 1u : 0u;
			}
		}

		/** @brief ParabolicPrograms.hlsli's ParabolicSphereMask (FUN_14151a1e0): the sqrt, the subtraction order and the strict comparisons. */
		std::uint32_t ParabolicSphereMask(const float a_bound[4], const std::array<float, 4>& a_sphere, const std::array<float, 4>& a_plane, bool a_bothSides)
		{
			const float dx = a_bound[0] - a_sphere[0], dy = a_bound[1] - a_sphere[1], dz = a_bound[2] - a_sphere[2];
			if (!((std::sqrt((dx * dx + dy * dy) + dz * dz) - a_bound[3]) - a_sphere[3] < 0.0f))
				return 0;
			const float side = ((a_bound[1] * a_plane[1] + a_bound[0] * a_plane[0]) + a_bound[2] * a_plane[2]) - a_plane[3];
			if (side - a_bound[3] < 0.0f)
				return a_bothSides ? 3u : 1u;
			return side > 0.0f ? 1u : (a_bothSides ? 2u : 0u);
		}

		/** @brief BuildDrawsCS's ParabolicRejected: the front hemisphere takes codes 1 and 3, the back one 2 and 3. */
		bool ParabolicRejected(const CullView& a_view, const float a_bound[4])
		{
			if (a_bound[3] < 0.0f)
				return false;
			const std::uint32_t code = ParabolicSphereMask(a_bound, a_view.planes[0], a_view.planes[1], (a_view.cullFlags & Draws::kCullParabolicBothSides) != 0);
			return (a_view.cullFlags & Draws::kCullParabolicBack) ? (code & 2u) == 0 : (code & 1u) == 0;
		}

		/** @brief LocalShadowMask: the local shadow lights whose sphere the bound meets and whose planes it is inside (strict lights gated). */
		std::uint32_t LocalShadowMask(const CullView& a_view, const float a_bound[4], bool a_landscape, std::uint32_t a_object)
		{
			std::uint32_t mask = 0;
			for (std::uint32_t v = 0; v < a_view.shadowVolumeCount; ++v) {
				const auto& volume = a_view.shadowVolumes[v];
				if ((mask & volume.maskBit) || (a_landscape && !volume.affectsLand))
					continue;
				const float dx = a_bound[0] - volume.sphere[0], dy = a_bound[1] - volume.sphere[1], dz = a_bound[2] - volume.sphere[2];
				const float reach = volume.sphere[3] + a_bound[3];
				if (dx * dx + dy * dy + dz * dz > reach * reach)
					continue;
				bool inside = true;
				for (std::uint32_t p = 0; p < 12 && inside; ++p) {
					if (!((p < 6 ? volume.masks[0] : volume.masks[1]) & (1u << (p % 6))))
						continue;
					const float* plane = volume.planes[p];
					inside = plane[0] * a_bound[0] + plane[1] * a_bound[1] + plane[2] * a_bound[2] - plane[3] >= -a_bound[3];
				}
				if (inside && volume.portalContext && a_view.strict && a_view.strict->rejects &&
					a_view.strict->rejects(*a_view.strict, volume.portalContext - 1, a_object, a_bound, a_bound[3]))
					inside = false;
				if (inside)
					mask |= volume.maskBit;
			}
			return mask;
		}

		bool InSunCascades(const CullView& a_view, const float a_bound[4])
		{
			for (std::uint32_t c = 0; c < a_view.cascadeCount; ++c)
				if (Draws::InSunCascade(a_view.cascades[c], a_bound, a_bound[3]))
					return true;
			return false;
		}

		/** @brief The draw's object word for a view: the index, the sun miss (the colour pass's sun test), the local shadow mask. */
		std::uint32_t ObjectWordOf(const CullView& a_view, std::uint32_t a_object, std::uint32_t a_flags, const float a_bound[4])
		{
			std::uint32_t word = a_object & kObjectIndexMask;
			if (a_view.sunTest && (a_flags & kLaneSunTest) && !InSunCascades(a_view, a_bound))
				word |= kObjectSunMiss;
			if (a_view.shadowVolumeCount)
				word |= (LocalShadowMask(a_view, a_bound, (a_flags & kLaneLandscapeLights) != 0, a_object) & 0xFu) << kObjectLocalShadowShift;
			return word;
		}

		std::uint32_t RootWordOf(const CullFrame& a_frame, std::uint32_t a_root)
		{
			return a_root < a_frame.rootWords.size() ? a_frame.rootWords[a_root] : 0u;
		}

		/**
		 * @brief MainFrustumRejected past the frustum: the portal program the member's root is culled through, tested with the member's own
		 * bound (the member batch's query; no node flags). A root without a program: the frustum alone.
		 */
		// kExpCullProfile: the calling thread's portal tests (calls, cycles) while its CullBatch profiles.
		thread_local bool profilePortals = false;
		thread_local std::uint64_t portalCalls = 0, portalCycles = 0;

		bool PortalRejected(const CullFrame& a_frame, std::uint32_t a_root, const float a_bound[4])
		{
			if (a_root == kNoRoot || a_root >= a_frame.rootPrograms.size() || a_frame.programs.empty() || !a_frame.portalTest)
				return false;
			const std::uint32_t program = a_frame.rootPrograms[a_root];
			if (program == kNoProgram)
				return false;
			if (!profilePortals)
				return a_frame.portalTest(a_frame.programs, program, a_bound, a_bound[3], 0u) != 1;
			const std::uint64_t start = __rdtsc();
			const bool rejected = a_frame.portalTest(a_frame.programs, program, a_bound, a_bound[3], 0u) != 1;
			portalCycles += __rdtsc() - start;
			++portalCalls;
			return rejected;
		}

		/**
		 * @brief StampFrustum: a main-camera lane in the frustum (and portals). The root's this-frame verdict where its OnVisible ran
		 * (serviced, not drawn: hidden), else the fade-out distance test against the previous stamp (kFrustumFadeHidden keeps a hidden
		 * one hidden while it stays past the distance). Writes the frame's stamp; returns the drop.
		 */
		bool StampFrustum(const CullFrame& a_frame, std::uint32_t a_object, std::uint32_t a_root, std::uint32_t a_flags, float a_fadeDistance)
		{
			bool fadeHidden = false;
			const std::uint32_t rootWord = a_root != kNoRoot ? RootWordOf(a_frame, a_root) : 0u;
			const bool serviced = (rootWord & kRootServiced) != 0;
			if (serviced)
				fadeHidden = (rootWord & kRootDrawn) == 0;
			const auto& eye = a_frame.fadeEye;
			if (!serviced && (a_flags & kLaneFadeTest) && a_fadeDistance != 0.0f && eye[3] != 0.0f && a_object < a_frame.fadeNode.size()) {
				const std::uint32_t previous = stamps.Load(a_object);
				const bool wasInView = (previous & kStampMask) == ((a_frame.stamp - 1) & kStampMask);
				const bool wasHidden = wasInView && (previous & kStampFadeHidden);
				const auto& node = a_frame.fadeNode[a_object];
				const float dx = node[0] - eye[0], dy = node[1] - eye[1], dz = node[2] - eye[2];
				const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) * (a_fadeDistance > 0.0f ? eye[3] : 1.0f);
				fadeHidden = distance > std::abs(a_fadeDistance) && (!wasInView || wasHidden);
			}
			stamps.Store(a_object, a_frame.stamp | (fadeHidden ? kStampFadeHidden : 0u));
			return fadeHidden;
		}

		/** @brief BSTreeNode::OnVisible's height test: a tree whose root is above the frame's limit draws nothing. */
		bool AboveTreeHeight(const CullFrame& a_frame, std::uint32_t a_object)
		{
			return a_object < a_frame.fadeNode.size() && a_frame.fadeNode[a_object][2] - a_frame.treeHeight[0] > a_frame.treeHeight[1];
		}

		/** @brief A mask of a lane's partitions (bits of the four level bytes' union) as bits of its sequences (the k-th set bit is sequence k). */
		std::uint32_t SequenceBits(std::uint32_t a_partitions, std::uint32_t a_union)
		{
			std::uint32_t bits = 0;
			for (std::uint32_t rest = a_partitions & a_union; rest; rest &= rest - 1) {
				const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(rest));
				bits |= 1u << std::popcount(a_union & ((1u << bit) - 1u));
			}
			return bits;
		}

		/**
		 * @brief LodPartitions: a LOD skin's partitions at its root's level and, in a main view while the root crosses levels, the cross-fade
		 * copy's (row L+1 less row L); as the lane's draw mask. False: the level draws none of them (kNoPartitions).
		 */
		bool LodMask(const CullFrame& a_frame, std::uint32_t a_lod, std::uint32_t a_partitions, std::uint32_t a_root, bool a_mainFamily, std::uint32_t& a_mask)
		{
			a_mask = kDrawAll;
			if (a_lod == 0)
				return true;
			const std::uint32_t all = (a_lod | (a_lod >> 8) | (a_lod >> 16) | (a_lod >> 24)) & 0xFFu;
			const std::uint32_t rootWord = a_root != kNoRoot ? RootWordOf(a_frame, a_root) : 0u;
			if (!(rootWord & kRootListed)) {
				// No state: the partitions as is (BuildDrawsCS's a_partitions).
				if (a_partitions != 0)
					a_mask = SequenceBits(a_partitions, all);
				return a_mask != 0;
			}
			const std::uint32_t level = (rootWord >> kRootLodShift) & 3u;
			const std::uint32_t mask = (a_lod >> (8u * level)) & 0xFFu;
			const std::uint32_t copy = a_mainFamily && (rootWord & kRootLodCopy) && level < 3 ? ((a_lod >> (8u * (level + 1))) & 0xFFu) & ~mask : 0u;
			if (mask == 0 && copy == 0)
				return false;
			a_mask = SequenceBits(mask | copy, all) | (SequenceBits(copy, all) << 16);
			return true;
		}

		/** @brief One lane's inputs, gathered. */
		struct Lane
		{
			std::uint32_t object = 0, flags = 0, root = kNoRoot;
			float fadeDistance = 0.0f;
			float bound[4]{};
		};

		/** @brief A shadow view's per-input drops before culling (BuildInput): caster class, sun entry, size tests, fading roots. */
		bool ShadowDropped(const CullView& a_view, const CullFrame& a_frame, const Lane& a_lane)
		{
			const bool volumetric = (a_lane.flags & kLaneVolumetricOnly) != 0;
			if (((a_view.cullFlags & Draws::kCullCastersOnly) && volumetric) || ((a_view.cullFlags & Draws::kCullVolumetricOnly) && !volumetric))
				return true;
			if ((a_view.cullFlags & Draws::kCullSunEntry) && a_view.sunEntryCount && a_lane.object < a_frame.sunEntry.size() &&
				Draws::OutsideSunEntryProcesses(std::span(a_view.sunEntry, a_view.sunEntryCount), a_frame.sunEntry[a_lane.object].data()))
				return true;
			const bool radiusKnown = a_lane.bound[3] >= 0.0f;
			if ((a_view.cullFlags & Draws::kCullMinRadius) && radiusKnown && a_lane.bound[3] <= kOcclusionMinRadius)
				return true;
			if ((a_view.cullFlags & Draws::kCullMinTexels) && radiusKnown && a_lane.bound[3] < a_view.minCasterRadius)
				return true;
			if (a_lane.root == kNoRoot)
				return false;
			const std::uint32_t rootWord = RootWordOf(a_frame, a_lane.root);
			return (a_view.cullFlags & Draws::kCullFadeOnVisible) ? (rootWord & kRootFadedOutOfOcclusion) != 0 : (rootWord & kRootFading) != 0;
		}

		/**
		 * @brief A main-camera list's lane (Main, Decal, Forward, Refraction): the stamp (frustum and portals, the fade drop) for kAccumulated,
		 * then the draw verdict: Main's phase 1 (the stamp's verdict, the height test), the colour segment's for the others (the frustum
		 * alone; a refraction lane also its root's stopped OnVisible).
		 */
		bool MainCameraKept(const CullView& a_view, const CullFrame& a_frame, const Lane& a_lane, bool a_inFrustum, bool a_live, const std::uint8_t* a_portalRejected)
		{
			const bool portalPassed = a_inFrustum && !(a_portalRejected ? *a_portalRejected != 0 : PortalRejected(a_frame, a_lane.root, a_lane.bound));
			bool fadeHidden = false;
			// Only a per-frame cull stamps: a per-snapshot one's verdicts are not the frame's (its lists get a stamp-only pass a frame).
			if (portalPassed && a_live)
				fadeHidden = StampFrustum(a_frame, a_lane.object, a_lane.root, a_lane.flags, a_lane.fadeDistance);
			if (a_view.stampOnly || (a_lane.flags & kLaneCullOnly))
				return false;
			if (a_view.type == ViewType::Main) {
				if (portalPassed && (a_lane.flags & kLaneHeightTest))
					fadeHidden = fadeHidden || AboveTreeHeight(a_frame, a_lane.object);
				return portalPassed && !fadeHidden;
			}
			if (!a_inFrustum)
				return false;
			if (a_view.type == ViewType::Refraction && a_lane.root != kNoRoot) {
				const std::uint32_t rootWord = RootWordOf(a_frame, a_lane.root);
				if ((rootWord & kRootServiced) && !(rootWord & kRootDrawn))
					return false;
			}
			return true;
		}

		/** @brief Any other view's lane (shadow, occlusion, reflection, first person): its drops, then its frustum or paraboloid, then its strict gates. */
		bool ViewKept(const CullView& a_view, const CullFrame& a_frame, const Lane& a_lane, bool a_inFrustum)
		{
			if (a_lane.flags & kLaneCullOnly)
				return false;
			if (ShadowFamily(a_view.type) && ShadowDropped(a_view, a_frame, a_lane))
				return false;
			if (!a_view.frustumCull)
				return true;
			const bool rejected = (a_view.cullFlags & Draws::kCullParabolic) ? ParabolicRejected(a_view, a_lane.bound) : !a_inFrustum;
			if (rejected)
				return false;
			if (a_view.strictContext && a_view.strict && a_view.strict->rejects && a_lane.bound[3] >= 0.0f &&
				a_view.strict->rejects(*a_view.strict, a_view.strictContext - 1, a_lane.object, a_lane.bound, a_lane.bound[3]))
				return false;
			return true;
		}

		/**
		 * @brief The main camera lists' portal tests over a batch's lanes in the frustum (kExpPortalBatch): PortalRejected's verdict by lane
		 * into a_rejected, the lanes grouped by their root's program (a counting sort) and each group handed to portalTestBatch.
		 */
		void PortalPass(const CullFrame& a_frame, const ViewBatch& a_batch, const Spheres& a_spheres, const std::uint8_t* a_pass, std::uint32_t a_count,
			std::uint8_t* a_rejected)
		{
			std::memset(a_rejected, 0, a_count);
			if (a_frame.programs.empty() || !a_batch.fadeRoot)
				return;
			const std::uint32_t programs = a_frame.programs.size() > 32 ? a_frame.programs[28] : 0u;  // the header's program count (word 112 / 4)
			if (!programs)
				return;
			thread_local std::vector<std::uint32_t> firsts;
			thread_local std::vector<std::uint16_t> lanes;
			thread_local std::vector<std::uint32_t> programOf;
			firsts.assign(std::size_t(programs) + 2, 0u);
			programOf.resize(a_count);
			// Pass 1: each lane's program (programs: none tested, rejected as PortalTestCpu's -1), counted.
			for (std::uint32_t i = 0; i < a_count; ++i) {
				std::uint32_t program = kNoProgram;
				if (a_pass[i]) {
					const std::uint32_t root = a_batch.fadeRoot[i] ? a_batch.fadeRoot[i] - 1 : kNoRoot;
					if (root != kNoRoot && root < a_frame.rootPrograms.size())
						program = a_frame.rootPrograms[root];
				}
				programOf[i] = program;
				if (program == kNoProgram)
					continue;
				if (program >= programs) {
					a_rejected[i] = 1;
					continue;
				}
				++firsts[std::size_t(program) + 2];
			}
			for (std::size_t p = 2; p < firsts.size(); ++p)
				firsts[p] += firsts[p - 1];
			lanes.resize(firsts.back());
			for (std::uint32_t i = 0; i < a_count; ++i)
				if (programOf[i] < programs)
					lanes[firsts[std::size_t(programOf[i]) + 1]++] = static_cast<std::uint16_t>(i);
			// firsts[p + 1] is now program p's end, firsts[p] its first.
			for (std::uint32_t p = 0; p < programs; ++p)
				if (const std::uint32_t first = firsts[p], end = firsts[std::size_t(p) + 1]; end > first)
					a_frame.portalTestBatch(a_frame.programs, p, a_spheres.x, a_spheres.y, a_spheres.z, a_spheres.r, lanes.data() + first, end - first, a_rejected);
		}

		Spheres SpheresOf(const ViewBatch& a_batch, bool a_live, std::uint32_t a_parity)
		{
			if (a_live) {
				const auto& live = a_batch.liveSpheres[a_parity & 1u];
				return { live.cx, live.cy, live.cz, live.r };
			}
			return { a_batch.cx, a_batch.cy, a_batch.cz, a_batch.r };
		}

		Lane LaneOf(const ViewBatch& a_batch, const Spheres& a_spheres, std::uint32_t a_i)
		{
			Lane lane;
			lane.object = a_batch.object[a_i] & kObjectIndexMask;
			lane.flags = a_batch.flags ? a_batch.flags[a_i] : 0u;
			lane.root = a_batch.fadeRoot && a_batch.fadeRoot[a_i] ? a_batch.fadeRoot[a_i] - 1 : kNoRoot;
			lane.fadeDistance = a_batch.fadeDistance ? a_batch.fadeDistance[a_i] : 0.0f;
			lane.bound[0] = a_spheres.x[a_i];
			lane.bound[1] = a_spheres.y[a_i];
			lane.bound[2] = a_spheres.z[a_i];
			lane.bound[3] = a_spheres.r[a_i];
			return lane;
		}

		void DiffVisibility(const CullFrame& a_frame);

		/** @brief kExpCullProfile: CullBatch's sections' TSC cycles and its lanes, by view type (TakeCullProfileLine). */
		struct CullProfile
		{
			enum Section { kFrustum, kGather, kKeep, kLod, kWord, kSections };
			enum Count { kLanes, kPassed, kKeptTest, kKept, kPortalCalls, kPortalCycles, kCounts };
			std::atomic<std::uint64_t> cycles[kViewTypes][kSections]{};
			std::atomic<std::uint64_t> counts[kViewTypes][kCounts]{};
		};
		CullProfile cullProfile;
		std::atomic<std::uint64_t> portalChecked{ 0 }, portalMismatches{ 0 };  // kExpPortalCheck
	}

	// ---- CullArena ----

	void* CullArena::Allocate(std::size_t a_bytes, std::size_t a_align)
	{
		for (;;) {
			if (current < chunks.size()) {
				auto& chunk = chunks[current];
				const std::size_t at = (used + a_align - 1) & ~(a_align - 1);
				if (at + a_bytes <= chunk.size) {
					used = at + a_bytes;
					return chunk.bytes.get() + at;
				}
				++current;
				used = 0;
				continue;
			}
			// A new chunk (one request larger than a chunk gets its own size); the old ones stay where they are.
			const std::size_t size = std::max(kChunkBytes, a_bytes + a_align);
			chunks.push_back({ std::make_unique<std::byte[]>(size), size });
			current = chunks.size() - 1;
			used = 0;
		}
	}

	void CullArena::Reset()
	{
		current = 0;
		used = 0;
	}

	std::size_t CullArena::BytesUsed() const
	{
		std::size_t bytes = used;
		for (std::size_t c = 0; c < current && c < chunks.size(); ++c)
			bytes += chunks[c].size;
		return bytes;
	}

	// ---- The frame state ----

	void KickCpuFrameState(std::shared_ptr<const ViewBatchSet> a_set)
	{
		arm.set = std::move(a_set);
		arm.frame = Scene::FrameView::Get().GetFrame();
		arm.armed = arm.set != nullptr;
	}

	CpuFrameArm TakeCpuFrameArm()
	{
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
		if (!arm.armed || arm.frame != frame) {
			arm.armed = false;
			arm.set.reset();
			return {};
		}
		CpuFrameArm taken{ std::move(arm.set), frame, !arm.everArmed || arm.lastArmed + 1 != frame };
		arm.armed = false;
		arm.everArmed = true;
		arm.lastArmed = frame;
		cpuStatesFrame.store(frame, std::memory_order_release);
		return taken;
	}

	namespace
	{
		std::atomic<StrictGatesSource> strictGatesSource{ nullptr };
	}

	void SetStrictGatesSource(StrictGatesSource a_source)
	{
		strictGatesSource.store(a_source, std::memory_order_release);
	}

	std::shared_ptr<const StrictGates> FrameStrictGates(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto source = strictGatesSource.load(std::memory_order_acquire);
		if (!source)
			return nullptr;
		auto gates = std::make_shared<const StrictGates>(source(a_scene, a_frame));
		return gates->rejects ? gates : nullptr;
	}

	bool CpuRootStatesFor(std::uint32_t a_sceneFrame)
	{
		return cpuStatesFrame.load(std::memory_order_acquire) == a_sceneFrame;
	}

	void OnFrameValuesSampled(std::uint64_t a_seq, std::span<const BindlessPlacement> a_rows)
	{
		rowStash = { a_seq, a_rows.data(), a_rows.size() };
	}

	std::span<const BindlessPlacement> SampledRows(std::uint64_t a_seq)
	{
		if (rowStash.seq != a_seq || !rowStash.rows)
			return {};
		return { rowStash.rows, rowStash.count };
	}

	void PublishCullFrame(std::shared_ptr<CullFrame> a_frame)
	{
		// The main camera's lists still to cull, plus this publication's own count (dropped below, after the waiters ran).
		std::uint32_t batches = 1;
		if (a_frame->set)
			for (const ViewType type : { ViewType::Main, ViewType::Decal, ViewType::Forward, ViewType::Refraction })
				batches += static_cast<std::uint32_t>(a_frame->set->List(type).size());
		a_frame->pendingStamps.store(batches, std::memory_order_relaxed);
		auto* held = new Held{ a_frame };
		const Held* replaced = latest.exchange(held, std::memory_order_seq_cst);
		if (replaced)
			retired.push_back(replaced);
		if (latestReaders.load(std::memory_order_seq_cst) == 0) {
			for (const Held* old : retired)
				delete old;
			retired.clear();
		}
		RunReady();
		if (a_frame->pendingStamps.fetch_sub(1, std::memory_order_acq_rel) == 1)
			DiffVisibility(*a_frame);
	}

	void AfterCullFrame(std::uint32_t a_sceneFrame, std::function<void(const std::shared_ptr<const CullFrame>&)> a_then)
	{
		PushWaiter(new Waiter{ a_sceneFrame, std::move(a_then) });
		RunReady();
	}

	std::shared_ptr<const CullFrame> LatestCullFrame()
	{
		latestReaders.fetch_add(1, std::memory_order_seq_cst);
		const Held* held = latest.load(std::memory_order_seq_cst);
		std::shared_ptr<const CullFrame> frame = held ? held->frame : nullptr;
		latestReaders.fetch_sub(1, std::memory_order_release);
		return frame;
	}

	// ---- The cull ----

	const CulledBatch& CullBatch(const ViewBatch& a_batch, bool a_live, std::uint32_t a_parity, const CullView& a_view, const CullFrame& a_frame, CullArena& a_arena)
	{
		auto& out = *new (a_arena.Make<CulledBatch>(1)) CulledBatch{};
		out.batch = &a_batch;
		out.view = a_view.viewIndex;
		const std::uint32_t count = a_batch.count;
		auto* lanes = a_arena.Make<std::uint16_t>(std::max(count, 1u));
		auto* words = a_arena.Make<std::uint32_t>(std::max(count, 1u));
		auto* masks = a_arena.Make<std::uint32_t>(std::max(count, 1u));
		thread_local std::vector<std::uint8_t> pass;
		pass.assign(count, 1u);
		const Spheres spheres = SpheresOf(a_batch, a_live, a_parity);
		const bool profile = ExperimentOn(kExpCullProfile);
		std::uint64_t cycles[CullProfile::kSections]{}, counts[CullProfile::kCounts]{};
		std::uint64_t at = profile ? __rdtsc() : 0;
		profilePortals = profile;
		if (profile)
			portalCalls = portalCycles = 0;
		const auto lap = [&](CullProfile::Section a_section) {
			if (!profile)
				return;
			const std::uint64_t now = __rdtsc();
			cycles[a_section] += now - at;
			at = now;
		};
		// A view without a frustum test (cull mode 0, or first person drawing every member) keeps every lane past its other tests.
		if (a_view.frustumCull && !(a_view.cullFlags & Draws::kCullParabolic))
			FrustumPass(spheres, count, PlanesOf(a_view), pass.data());
		else if (a_view.frustumCull && ExperimentOn(kExpParabolicPass))
			ParabolicPass(spheres, count, a_view.planes[0], pass.data());
		// The incremental lists' tombstones: a lane gone from this set passes nothing (the portal pass skips it; the loop drops it).
		if (a_batch.dead)
			for (std::uint32_t i = 0; i < count; ++i)
				if ((a_batch.dead[i >> 6] >> (i & 63)) & 1u)
					pass[i] = 0;
		lap(CullProfile::kFrustum);
		const bool mainCamera = MainCamera(a_view.type);
		const bool mainFamily = mainCamera || a_view.type == ViewType::Reflection || a_view.type == ViewType::FirstPerson;
		// A lane outside the frustum is kept by no view (MainCameraKept and ViewKept both drop it, and only a lane inside stamps); a view
		// without the frustum pass has every lane passed.
		const bool early = ExperimentOn(kExpCullEarly);
		// The main camera lists' portal tests, made over the batch before the lanes' other tests.
		thread_local std::vector<std::uint8_t> portalRejected;
		const bool portalBatch = mainCamera && a_frame.portalTestBatch && ExperimentOn(kExpPortalBatch);
		if (portalBatch) {
			portalRejected.resize(count);
			PortalPass(a_frame, a_batch, spheres, pass.data(), count, portalRejected.data());
			if (ExperimentOn(kExpPortalCheck)) {
				std::uint64_t checked = 0, mismatches = 0;
				for (std::uint32_t i = 0; i < count; ++i)
					if (pass[i]) {
						const Lane lane = LaneOf(a_batch, spheres, i);
						++checked;
						mismatches += PortalRejected(a_frame, lane.root, lane.bound) != (portalRejected[i] != 0) ? 1u : 0u;
					}
				portalChecked.fetch_add(checked, std::memory_order_relaxed);
				portalMismatches.fetch_add(mismatches, std::memory_order_relaxed);
			}
		}
		std::uint32_t kept = 0;
		ZoneScopedN("CS.DCLF.Cpu.CullLanes");
		for (std::uint32_t i = 0; i < count; ++i) {
			if (early && !pass[i])
				continue;
			if (a_batch.dead && ((a_batch.dead[i >> 6] >> (i & 63)) & 1u))
				continue;  // gone from this set (a view without a frustum test keeps every lane it has)
			if (profile)
				++counts[CullProfile::kPassed];
			const Lane lane = LaneOf(a_batch, spheres, i);
			// A focus view's casters: its target's members alone (an input's root word against the latch's root + 1).
			if (a_view.focusRoot != 0 && lane.root != a_view.focusRoot - 1)
				continue;
			lap(CullProfile::kGather);
			const bool keep = mainCamera ? MainCameraKept(a_view, a_frame, lane, pass[i] != 0, a_live, portalBatch ? &portalRejected[i] : nullptr) :
			                               ViewKept(a_view, a_frame, lane, pass[i] != 0);
			lap(CullProfile::kKeep);
			if (!keep)
				continue;
			if (profile)
				++counts[CullProfile::kKeptTest];
			std::uint32_t mask = kDrawAll;
			const bool lod = !a_batch.lodPartitions || LodMask(a_frame, a_batch.lodPartitions[i], a_batch.partitions ? a_batch.partitions[i] : 0u, lane.root, mainFamily, mask);
			lap(CullProfile::kLod);
			if (!lod)
				continue;
			lanes[kept] = static_cast<std::uint16_t>(i);
			words[kept] = ObjectWordOf(a_view, lane.object, lane.flags, lane.bound);
			masks[kept] = mask;
			++kept;
			lap(CullProfile::kWord);
		}
		if (profile) {
			const std::size_t v = static_cast<std::size_t>(a_view.type);
			counts[CullProfile::kLanes] = count;
			counts[CullProfile::kKept] = kept;
			counts[CullProfile::kPortalCalls] = std::exchange(portalCalls, 0);
			counts[CullProfile::kPortalCycles] = std::exchange(portalCycles, 0);
			profilePortals = false;
			for (std::size_t c = 0; c < CullProfile::kSections; ++c)
				cullProfile.cycles[v][c].fetch_add(cycles[c], std::memory_order_relaxed);
			for (std::size_t c = 0; c < CullProfile::kCounts; ++c)
				cullProfile.counts[v][c].fetch_add(counts[c], std::memory_order_relaxed);
		}
		out.count = kept;
		out.lanes = lanes;
		out.objectWords = words;
		out.drawMasks = masks;
		// The main camera's per-frame lists: the frame's last one diffs the verdicts for kAccumulated.
		if (mainCamera && a_live && a_frame.pendingStamps.fetch_sub(1, std::memory_order_acq_rel) == 1)
			DiffVisibility(a_frame);
		return out;
	}

	std::string TakeCullProfileLine()
	{
		std::string line;
		if (const auto checked = portalChecked.exchange(0, std::memory_order_relaxed)) {
			const auto mismatches = portalMismatches.exchange(0, std::memory_order_relaxed);
			line = fmt::format("portal batch check: {} lanes, {} verdicts differ{}", checked, mismatches, mismatches ? " <- PORTAL BATCH DIFFERS" : "");
		}
		for (std::size_t v = 0; v < kViewTypes; ++v) {
			std::uint64_t cycles[CullProfile::kSections], counts[CullProfile::kCounts];
			for (std::size_t c = 0; c < CullProfile::kSections; ++c)
				cycles[c] = cullProfile.cycles[v][c].exchange(0, std::memory_order_relaxed);
			for (std::size_t c = 0; c < CullProfile::kCounts; ++c)
				counts[c] = cullProfile.counts[v][c].exchange(0, std::memory_order_relaxed);
			if (!counts[CullProfile::kLanes])
				continue;
			// Each section's cycles a lane: gather and the keep test per lane passed, the LOD mask per lane the test kept, the word per lane kept.
			const auto per = [](std::uint64_t a_cycles, std::uint64_t a_n) { return a_n ? static_cast<double>(a_cycles) / static_cast<double>(a_n) : 0.0; };
			line += fmt::format("{}{}: {} lanes, {} passed, {} kept by the test, {} kept; Mcycles frustum {:.1f}, gather {:.1f}, keep {:.1f}, lod {:.1f}, word {:.1f}; "
								"cycles a lane: gather {:.0f}, keep {:.0f}, lod {:.0f}, word {:.0f}; portal tests {}, {:.0f} cycles each",
				line.empty() ? "" : " | ", CpuViewName(v), counts[CullProfile::kLanes], counts[CullProfile::kPassed], counts[CullProfile::kKeptTest], counts[CullProfile::kKept],
				cycles[CullProfile::kFrustum] / 1e6, cycles[CullProfile::kGather] / 1e6, cycles[CullProfile::kKeep] / 1e6, cycles[CullProfile::kLod] / 1e6,
				cycles[CullProfile::kWord] / 1e6, per(cycles[CullProfile::kGather], counts[CullProfile::kPassed]), per(cycles[CullProfile::kKeep], counts[CullProfile::kPassed]),
				per(cycles[CullProfile::kLod], counts[CullProfile::kKeptTest]), per(cycles[CullProfile::kWord], counts[CullProfile::kKept]), counts[CullProfile::kPortalCalls],
				per(counts[CullProfile::kPortalCycles], counts[CullProfile::kPortalCalls]));
		}
		return line;
	}

	const CulledBatch& ColourObjectWords(const CulledBatch& a_depth, const CullView& a_colour, const CullFrame& a_frame, CullArena& a_arena)
	{
		auto& out = *new (a_arena.Make<CulledBatch>(1)) CulledBatch(a_depth);
		out.commands = nullptr;
		auto* words = a_arena.Make<std::uint32_t>(std::max(a_depth.count, 1u));
		const auto& batch = *a_depth.batch;
		const Spheres spheres = SpheresOf(batch, true, static_cast<std::uint32_t>(LiveParity(a_frame.frameValuesSeq)));
		for (std::uint32_t k = 0; k < a_depth.count; ++k) {
			const Lane lane = LaneOf(batch, spheres, a_depth.lanes[k]);
			words[k] = ObjectWordOf(a_colour, lane.object, lane.flags, lane.bound);
		}
		out.objectWords = words;
		return out;
	}

	// ---- kAccumulated: VisibilityEventsCS on the CPU ----

	namespace
	{
		/** @brief The diff's own state (one diff at a time; a frame whose diff finds one running is skipped: the next reports its changes). */
		struct VisibilityDiff
		{
			std::atomic<bool> running{ false };
			std::vector<std::uint32_t> objectReported;                    // kReportedValid | the bits last reported
			std::vector<std::pair<std::uint32_t, std::uint32_t>> roots;  // the generation and the bits last reported
			std::atomic<bool> pendingReset{ false };  // a skipped frame was a reset: the next diff resets
			std::atomic<std::uint64_t> diffs{ 0 }, skipped{ 0 };
		};
		VisibilityDiff visibilityDiff;
		constexpr std::uint32_t kReportedValid = 1u << 31;

		void DiffRoots(const CullFrame& a_frame, VisibilityDiff& a_diff, std::vector<Draws::VisibilityEvent>& a_events)
		{
			const std::size_t roots = std::min(a_frame.rootWords.size(), a_frame.rootGenerations.size());
			if (a_diff.roots.size() < roots)
				a_diff.roots.resize(roots, { 0u, 0u });
			for (std::uint32_t r = 0; r < roots; ++r) {
				const std::uint32_t generation = a_frame.rootGenerations[r];
				const std::uint32_t word = a_frame.rootWords[r];
				if (!(word & kRootListed) || generation == 0)
					continue;
				const std::uint32_t bits = (word & kRootUpdated) ? (Draws::kVisibilityBitVerdict | ((word & kRootInView) ? Draws::kVisibilityBitVisible : 0u)) : 0u;
				auto& last = a_diff.roots[r];
				const std::uint32_t lastBits = last.first == generation ? last.second : 0u;
				if (lastBits != bits)
					a_events.push_back({ r, generation, Draws::kVisibilityKindRoot, bits });
				last = { generation, bits };
			}
		}

		void DiffObjects(const CullFrame& a_frame, std::size_t a_objects, VisibilityDiff& a_diff, std::vector<Draws::VisibilityEvent>& a_events)
		{
			if (a_diff.objectReported.size() < a_objects)
				a_diff.objectReported.resize(a_objects, 0u);
			const std::uint32_t stamp = a_frame.stamp & kStampMask;
			for (std::uint32_t o = 0; o < a_objects; ++o) {
				const std::uint32_t word = stamps.Load(o);
				const bool stamped = (word & kStampMask) == stamp;
				const std::uint32_t bits = stamped ? ((word & kStampFadeHidden) ? Draws::kVisibilityBitFadeHidden : Draws::kVisibilityBitVisible) : 0u;
				auto& last = a_diff.objectReported[o];
				const std::uint32_t lastBits = (last & kReportedValid) ? (last & ~kReportedValid) : 0u;
				if (lastBits != bits)
					a_events.push_back({ o, 0u, Draws::kVisibilityKindObject, bits });
				last = kReportedValid | bits;
			}
		}

		void DiffVisibility(const CullFrame& a_frame)
		{
			ZoneScopedN("CS.DCLF.Cpu.DiffVisibility");
			auto& diff = visibilityDiff;
			if (bool idle = false; !diff.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
				if (a_frame.visibilityReset)
					diff.pendingReset.store(true, std::memory_order_release);
				diff.skipped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			auto* batch = new Draws::VisibilityReadback::Batch();
			batch->frame = a_frame.sceneFrame;
			batch->reset = a_frame.visibilityReset || diff.pendingReset.exchange(false, std::memory_order_acq_rel);
			batch->tables = a_frame.tables;
			batch->candidates = a_frame.candidates;
			// A reset starts the reported words again: every verdict is reported, as a new backing's zeroed words make the GPU's.
			if (batch->reset) {
				diff.objectReported.clear();
				diff.roots.clear();
			}
			DiffRoots(a_frame, diff, batch->events);
			DiffObjects(a_frame, a_frame.tables ? a_frame.tables->objects.size() : 0u, diff, batch->events);
			diff.diffs.fetch_add(1, std::memory_order_relaxed);
			diff.running.store(false, std::memory_order_release);
			Draws::PostCpuVisibility(batch);
		}
	}

	// ---- The views ----

	namespace
	{
		/** @brief The view-projection's clip planes in world space (row-major, mul(M, v)): -x, +x, -y, +y, near (z >= 0), far (z <= w), normalised. */
		std::array<std::array<float, 4>, 6> ClipPlanesOf(const float a_viewProj[16])
		{
			const auto row = [&](std::uint32_t a_r) { return std::array<float, 4>{ a_viewProj[a_r * 4], a_viewProj[a_r * 4 + 1], a_viewProj[a_r * 4 + 2], a_viewProj[a_r * 4 + 3] }; };
			const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
			const auto add = [](const std::array<float, 4>& a, const std::array<float, 4>& b, float s) {
				return std::array<float, 4>{ a[0] + s * b[0], a[1] + s * b[1], a[2] + s * b[2], a[3] + s * b[3] };
			};
			const std::array<std::array<float, 4>, 6> raw{ add(r3, r0, 1.0f), add(r3, r0, -1.0f), add(r3, r1, 1.0f), add(r3, r1, -1.0f), r2, add(r3, r2, -1.0f) };
			std::array<std::array<float, 4>, 6> planes{};
			for (std::size_t p = 0; p < 6; ++p) {
				const float length = std::sqrt(raw[p][0] * raw[p][0] + raw[p][1] * raw[p][1] + raw[p][2] * raw[p][2]);
				if (!(length > 0.0f))
					continue;  // a degenerate plane tests nothing (FadeStateCS's InView)
				// (n, d) with the bound outside where dot(n, c) - d < -r: the clip plane's dot(n, c) + w over its length.
				planes[p] = { raw[p][0] / length, raw[p][1] / length, raw[p][2] / length, -raw[p][3] / length };
			}
			return planes;
		}
	}

	CullView CullViewOfLatch(ViewType a_type, std::uint32_t a_viewIndex, const Draws::BuildDrawsLatch& a_latch)
	{
		CullView view;
		view.type = a_type;
		view.viewIndex = a_viewIndex;
		std::copy(std::begin(a_latch.viewProj), std::end(a_latch.viewProj), view.viewProj.begin());
		for (std::size_t p = 0; p < 6; ++p)
			std::copy(std::begin(a_latch.cullPlanes[p]), std::end(a_latch.cullPlanes[p]), view.planes[p].begin());
		view.planeMask = a_latch.cullPlaneMask;
		view.cullFlags = a_latch.cullFlags;
		view.eye = { a_latch.fadeEye[0], a_latch.fadeEye[1], a_latch.fadeEye[2] };
		view.fadeEyeDistanceScale = a_latch.fadeEye[3];
		view.treeHeightBase = a_latch.treeHeight[0];
		view.treeHeight = a_latch.treeHeight[1];
		view.focusRoot = a_latch.focusRoot;
		view.frame = a_latch.visibilityStamp;
		// Cull mode 0 (no view-projection this frame): nothing is frustum-tested (BuildDrawsCS's CullMode() == 0).
		view.frustumCull = (a_latch.cullFlags & 0xFu) != 0;
		view.clipPlanes = ClipPlanesOf(a_latch.viewProj);
		view.minCasterRadius = a_latch.minCasterRadius;
		view.strictContext = a_latch.lightContextsIndex && a_latch.strictGatesIndex ? a_latch.strictContext : 0u;
		view.sunTest = (a_latch.sunState & Draws::kSunTestOn) != 0;
		return view;
	}

	CullView MainCullView(const Draws::BuildDrawsLatch& a_depthLatch)
	{
		return CullViewOfLatch(ViewType::Main, 0, a_depthLatch);
	}

	CullView ColourCullView(ViewType a_type, const Draws::BuildDrawsLatch& a_colourLatch, std::span<const Draws::GpuCascade> a_cascades,
		std::span<const Draws::GpuShadowVolume> a_volumes)
	{
		CullView view = CullViewOfLatch(a_type, 0, a_colourLatch);
		view.cascades = a_cascades.data();
		view.cascadeCount = static_cast<std::uint32_t>(a_cascades.size());
		// The latch's region is what it tests (localShadowOffset 0: none).
		if (a_colourLatch.localShadowOffset) {
			view.shadowVolumes = a_volumes.data();
			view.shadowVolumeCount = static_cast<std::uint32_t>(a_volumes.size());
		}
		return view;
	}

	CullView ShadowCullView(ViewType a_type, std::uint32_t a_slot, const Draws::BuildDrawsLatch& a_latch, std::span<const Draws::SunEntryProcess> a_sunEntry)
	{
		CullView view = CullViewOfLatch(a_type, a_slot, a_latch);
		if (a_latch.cullFlags & Draws::kCullSunEntry) {
			view.sunEntry = a_sunEntry.data();
			view.sunEntryCount = static_cast<std::uint32_t>(a_sunEntry.size());
		}
		return view;
	}

	CullView OcclusionCullView(ViewType a_type, std::uint32_t a_view, const Draws::BuildDrawsLatch& a_latch)
	{
		return CullViewOfLatch(a_type, a_view, a_latch);
	}

	CullView ReflectionCullView(std::uint32_t a_face, const Draws::BuildDrawsLatch& a_latch)
	{
		return CullViewOfLatch(ViewType::Reflection, a_face, a_latch);
	}

	CullView FirstPersonCullView(const Draws::BuildDrawsLatch& a_latch)
	{
		return CullViewOfLatch(ViewType::FirstPerson, 0, a_latch);
	}
}
