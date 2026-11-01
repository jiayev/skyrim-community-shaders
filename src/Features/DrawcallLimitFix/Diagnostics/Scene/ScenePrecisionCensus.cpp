// CS_DCLF_PRECISION_CENSUS: how much translation precision the engine's float transforms lose, by distance from the origin. Each
// report recomposes every record's translation (and each of its skin's bones') down its parent chain in double, and compares it with the
// float the engine stored. It answers whether a row's high and low translation pair would recover anything (dclf-validation.md).

#include "Events.h"
#include "SceneObservers.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <array>
#include <cmath>

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		constexpr std::uint32_t kReportFrames = 300;
		constexpr std::size_t kMaxChain = 64;
		// Buckets by the exponent of the largest translation component: below 2^10, then one per power of two.
		constexpr int kMinExponent = 10;
		constexpr int kMaxExponent = 24;
		// Further from the engine's value than this many float steps, something other than the chain wrote the transform.
		constexpr double kRefuseSteps = 2.0;

		struct Bucket
		{
			std::uint64_t count = 0;
			std::uint64_t localZero = 0;    // no translation of its own: its parent's value, so nothing to recover at this link
			std::uint64_t none = 0;         // under 1/16 of a float step lost
			std::uint64_t recoverable = 0;  // a quarter of a float step or more lost
			std::uint64_t refused = 0;      // the chain disagrees with the engine
			double largestLoss = 0.0;       // units, of those not refused
		};
		using Buckets = std::array<Bucket, kMaxExponent - kMinExponent + 1>;

		double FloatStep(float a_value)
		{
			const float magnitude = std::abs(a_value);
			return static_cast<double>(std::nextafter(magnitude, std::numeric_limits<float>::infinity())) - magnitude;
		}

		/** @brief a_object's translation composed down its parent chain in double, from its root's world; false for a chain too deep. */
		bool ComposedTranslation(const RE::NiAVObject& a_object, std::array<double, 3>& a_out)
		{
			std::array<const RE::NiAVObject*, kMaxChain> chain;
			std::size_t depth = 0;
			for (const RE::NiAVObject* at = &a_object; at; at = at->parent) {
				if (depth == kMaxChain)
					return false;
				chain[depth++] = at;
			}
			const auto& root = chain[depth - 1]->world.translate;
			a_out = { root.x, root.y, root.z };
			for (std::size_t i = depth - 1; i > 0; --i) {
				const auto& parent = chain[i]->world;
				const auto& t = chain[i - 1]->local.translate;
				for (int row = 0; row < 3; ++row) {
					const auto* r = parent.rotate.entry[row];
					a_out[row] += static_cast<double>(parent.scale) *
					              (static_cast<double>(r[0]) * t.x + static_cast<double>(r[1]) * t.y + static_cast<double>(r[2]) * t.z);
				}
			}
			return true;
		}

		void Count(const RE::NiAVObject& a_object, Buckets& a_buckets, float& a_farthest)
		{
			const auto& world = a_object.world.translate;
			const float components[3] = { world.x, world.y, world.z };
			const float largest = std::max({ std::abs(world.x), std::abs(world.y), std::abs(world.z) });
			if (!std::isfinite(largest))
				return;
			a_farthest = std::max(a_farthest, largest);
			int exponent = kMinExponent;
			if (largest >= static_cast<float>(1 << kMinExponent))
				exponent = std::min(std::ilogb(largest), kMaxExponent);
			auto& bucket = a_buckets[static_cast<std::size_t>(exponent - kMinExponent)];
			++bucket.count;
			const auto& local = a_object.local.translate;
			if (local.x == 0.0f && local.y == 0.0f && local.z == 0.0f)
				++bucket.localZero;
			std::array<double, 3> composed;
			if (!ComposedTranslation(a_object, composed)) {
				++bucket.refused;
				return;
			}
			double loss = 0.0;
			for (int axis = 0; axis < 3; ++axis) {
				const double difference = std::abs(composed[axis] - components[axis]);
				if (difference > kRefuseSteps * FloatStep(components[axis])) {
					++bucket.refused;
					return;
				}
				loss = std::max(loss, difference);
			}
			const double step = FloatStep(largest);
			if (loss < step / 16.0)
				++bucket.none;
			else if (loss >= step / 4.0)
				++bucket.recoverable;
			bucket.largestLoss = std::max(bucket.largestLoss, loss);
		}

		/**
		 * @brief Count, surviving a dangling pointer: the tables' geometries and a skin's bones are not pinned here, and across a cell
		 * change either can point at freed memory. False when the read faulted. A diagnostic's guard only: a live path reads pinned items.
		 */
		bool CountGuarded(const RE::NiAVObject* a_object, Buckets& a_buckets, float& a_farthest)
		{
			__try {
				Count(*a_object, a_buckets, a_farthest);
				return true;
			} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
				return false;
			}
		}

		/** @brief Each bone of a_geometry's skin through CountGuarded; false when reading the skin itself faulted. */
		bool CountBonesGuarded(const RE::BSGeometry* a_geometry, Buckets& a_buckets, float& a_farthest, std::uint64_t& a_faults)
		{
			__try {
				const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
				const auto* data = skin ? skin->skinData.get() : nullptr;
				if (!data || !skin->bones)
					return true;
				for (std::uint32_t b = 0; b < data->GetBoneCount(); ++b)
					if (const auto* bone = skin->bones[b]; bone && !CountGuarded(bone, a_buckets, a_farthest))
						++a_faults;
				return true;
			} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
				return false;
			}
		}

		void LogBuckets(const char* a_kind, const Buckets& a_buckets)
		{
			for (std::size_t b = 0; b < a_buckets.size(); ++b) {
				const auto& bucket = a_buckets[b];
				if (!bucket.count)
					continue;
				const int exponent = kMinExponent + static_cast<int>(b);
				logger::info("[DCLF] precision census: {} at {}2^{}: {} (float step {}), local zero {}, under 1/16 step {}, 1/4 step or more {}, refused {}, largest loss {}",
					a_kind, b == 0 ? "under " : "", exponent, bucket.count, std::ldexp(1.0, exponent - 23), bucket.localZero, bucket.none, bucket.recoverable,
					bucket.refused, bucket.largestLoss);
			}
		}

		void OnPrepassBegin(const On<ScenePoint::PrepassConstantsBegin>&)
		{
			// The render thread, inside the engine-read window: the scene lane's passes mostly run after it closes.
			const std::uint32_t frame = V::frame();
			static bool due = false;
			due = due || frame % kReportFrames == 0;
			if (!due)
				return;
			// The window can be shut at this point of a frame: the report waits for a frame where it is open.
			if (const EngineReadWindow::Lease lease; !lease)
				return;
			due = false;
			Buckets rows{}, bones{};
			float farthest = 0.0f;
			std::uint64_t refusedLeases = 0;
			std::uint64_t faults = 0;
			for (const auto* geometry : Scene::FrameView::Get().GetTables().objectGeometry) {
				if (!geometry)
					continue;
				// A lease per record: its bones are read under it too.
				const EngineReadWindow::Lease lease;
				if (!lease) {
					++refusedLeases;
					continue;
				}
				if (!CountGuarded(geometry, rows, farthest) || !CountBonesGuarded(geometry, bones, farthest, faults))
					++faults;
			}
			RE::NiPoint3 player{};
			if (const EngineReadWindow::Lease lease; lease)
				if (auto* character = RE::PlayerCharacter::GetSingleton())
					player = character->GetPosition();
			logger::info("[DCLF] precision census: frame {}, player ({:.1f}, {:.1f}, {:.1f}), farthest component {:.1f}, records skipped (window closed) {}, dangling reads {}",
				frame, player.x, player.y, player.z, farthest, refusedLeases, faults);
			LogBuckets("rows", rows);
			LogBuckets("bones", bones);
		}
	}

	void InstallPrecisionCensus()
	{
		Subscribe<ScenePoint::PrepassConstantsBegin>(OnPrepassBegin);
	}
}
