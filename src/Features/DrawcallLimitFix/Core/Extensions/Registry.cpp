// The extension registry (see the header): the install context the features register with, and the lists it fills.

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

#include <vector>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures (the main thread, before DCLF's threads exist: their start orders these writes before
		// every read), read-only after.
		struct Lists
		{
			std::vector<FrameValuePoolEntry> pools;
			std::vector<GpuStageEntry> stages;
			std::vector<PayloadProductEntry> products;
			bool installed = false;
		};

		Lists& Registered()
		{
			static Lists lists;
			return lists;
		}

		class Context final : public Api::InstallContext
		{
		public:
			explicit Context(Lists& a_lists) :
				lists(a_lists) {}

			Api::FrameValuePoolId RegisterFrameValuePool(const Api::FrameValuePool& a_pool) override
			{
				if (a_pool.name.empty() || !a_pool.stride || !a_pool.kick || !a_pool.sample || !a_pool.send)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete frame-value pool", feature));
				lists.pools.push_back({ std::string(a_pool.name), a_pool.stride, a_pool.kick, a_pool.sample, a_pool.send });
				return { static_cast<std::uint32_t>(lists.pools.size() - 1) };
			}

			void RegisterGpuStage(const Api::GpuStage& a_stage) override
			{
				if (a_stage.passName.empty() || !a_stage.setup || !a_stage.makePass)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete GPU stage", feature));
				// BuildDrawsLatch has one stage word, the depth segment's (Api/GpuStages.h).
				if (a_stage.latchWord) {
					if (a_stage.slot != Api::GpuStageSlot::DepthSegmentBeforeBuild)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s stage {} asks for a latch word outside the depth segment", feature, a_stage.passName));
					for (const auto& stage : lists.stages)
						if (stage.latchWord && stage.slot == a_stage.slot)
							stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s stage {} asks for the latch's stage word, which {} has: grow BuildDrawsLatch first",
								feature, a_stage.passName, stage.passName));
				}
				lists.stages.push_back({ std::string(a_stage.passName), a_stage.slot, a_stage.setup, a_stage.makePass, a_stage.latchWord });
			}

			void RegisterPayloadProduct(const Api::PayloadProduct& a_product) override
			{
				if (a_product.name.empty() || !a_product.build || !a_product.install)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete payload product", feature));
				lists.products.push_back({ std::string(a_product.name), a_product.build, a_product.carried, a_product.install });
			}

			std::string_view feature;  // the feature installing

		private:
			Lists& lists;
		};
	}

	void InstallFeatures(std::span<const Api::Feature> a_features)
	{
		auto& lists = Registered();
		if (std::exchange(lists.installed, true))
			stl::report_and_fail("Drawcall Limit Fix: the features were installed twice");
		Context context(lists);
		for (const auto& feature : a_features) {
			context.feature = feature.name;
			if (feature.install)
				feature.install(context);
		}
	}

	std::span<const FrameValuePoolEntry> RegisteredPools() { return Registered().pools; }
	std::span<const GpuStageEntry> RegisteredStages() { return Registered().stages; }
	std::span<const PayloadProductEntry> RegisteredProducts() { return Registered().products; }
}
