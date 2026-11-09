#include "Features/DrawcallLimitFix/Diagnostics/ShadowMaskProbe.h"

#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"

#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>

namespace DCLF::Diagnostics::ShadowMaskProbe
{
	namespace
	{
		std::mutex mutex;
		std::unordered_map<std::uint32_t, std::string> pending;
		std::atomic<bool> armed{ false };
		// A slot is another object's once the cell changes: the probe lapses after a second.
		std::chrono::steady_clock::time_point armedAt;
	}

	void Arm(std::vector<std::pair<std::uint32_t, std::string>> a_objects)
	{
		std::lock_guard lock(mutex);
		pending.clear();
		for (auto& [object, what] : a_objects)
			pending.emplace(object, std::move(what));
		armedAt = std::chrono::steady_clock::now();
		armed.store(!pending.empty(), std::memory_order_release);
	}

	bool Armed()
	{
		return armed.load(std::memory_order_relaxed);
	}

	void Note(std::uint32_t a_object, const float a_bound[4], float a_margin, std::uint32_t a_mask, const Draws::GpuShadowVolume* a_volumes, std::uint32_t a_count,
		const std::uint8_t* a_rejects)
	{
		std::string what;
		{
			std::lock_guard lock(mutex);
			if (std::chrono::steady_clock::now() - armedAt > std::chrono::seconds(1)) {
				pending.clear();
				armed.store(false, std::memory_order_release);
				return;
			}
			const auto it = pending.find(a_object);
			if (it == pending.end())
				return;
			what = std::move(it->second);
			pending.erase(it);
			if (pending.empty())
				armed.store(false, std::memory_order_release);
		}
		static constexpr const char* kReasons[] = { "taken", "outside its sphere", "outside a plane", "the strict gate", "landscape", "bit taken" };
		std::string lights;
		for (std::uint32_t v = 0; v < a_count; ++v) {
			const auto& volume = a_volumes[v];
			const float dx = a_bound[0] - volume.sphere[0], dy = a_bound[1] - volume.sphere[1], dz = a_bound[2] - volume.sphere[2];
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			lights += std::format(" [bit {:X}, centre ({:.0f} {:.0f} {:.0f}) r {:.0f}, distance {:.0f}, strict {}: {}]", volume.maskBit, volume.sphere[0], volume.sphere[1],
				volume.sphere[2], volume.sphere[3], distance, volume.portalContext, kReasons[a_rejects[v] < 6 ? a_rejects[v] : 0]);
		}
		logger::info("[DCLF] actor probe shadow mask: object {} {}: bound ({:.0f} {:.0f} {:.0f}) r {:.0f} (+{:.1f}), mask {:X} of {} volumes:{}", a_object, what, a_bound[0],
			a_bound[1], a_bound[2], a_bound[3], a_margin, a_mask, a_count, lights.empty() ? std::string(" none") : lights);
	}
}
