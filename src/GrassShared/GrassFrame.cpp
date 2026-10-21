#include "GrassFrame.h"

namespace GrassShared
{
	namespace
	{
		constexpr uint32_t kMaxStoodDownCallbacks = 4;

		std::atomic<bool> dclfOwned{ false };

		// Filled during PostPostLoad, then only read; the count publishes each entry.
		std::array<void (*)(), kMaxStoodDownCallbacks> stoodDownCallbacks{};
		std::atomic<uint32_t> stoodDownCount{ 0 };

		// Written by the settings UI and the render thread, read by DCLF: each publish swaps in a whole snapshot.
		std::atomic<std::shared_ptr<const CullSettings>> settings;
		std::atomic<std::shared_ptr<const Collision>> collision;
	}

	bool DclfOwned()
	{
		return dclfOwned.load(std::memory_order_acquire);
	}

	void SetDclfOwned(bool owned)
	{
		dclfOwned.store(owned, std::memory_order_release);
	}

	void OnStoodDownFrame(void (*callback)())
	{
		if (!callback)
			return;
		const uint32_t n = stoodDownCount.load(std::memory_order_relaxed);
		if (n >=kMaxStoodDownCallbacks) {
			logger::error("[GRASS SHARED] stood-down callback rejected: all {} slots are taken", kMaxStoodDownCallbacks);
			return;
		}
		stoodDownCallbacks[n] = callback;
		stoodDownCount.store(n + 1, std::memory_order_release);
	}

	void RunStoodDownFrame()
	{
		const uint32_t n = stoodDownCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < n; ++i)
			stoodDownCallbacks[i]();
	}

	void PublishSettings(const CullSettings& published)
	{
		auto snapshot = std::make_shared<CullSettings>(published);
		snapshot->valid = true;
		settings.store(std::move(snapshot), std::memory_order_release);
	}

	CullSettings Settings()
	{
		if (auto snapshot = settings.load(std::memory_order_acquire))
			return *snapshot;
		return {};
	}

	void PublishCollision(const Collision& published)
	{
		collision.store(std::make_shared<const Collision>(published), std::memory_order_release);
	}

	bool LatestCollision(Collision& out)
	{
		auto snapshot = collision.load(std::memory_order_acquire);
		if (!snapshot)
			return false;
		out = *snapshot;
		return true;
	}
}
