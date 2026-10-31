// The registered payload products' build, publication and install (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	namespace
	{
		const std::shared_ptr<const void>& ProductAt(const PayloadProducts* a_products, std::size_t a_index)
		{
			static const std::shared_ptr<const void> kNone;
			return a_products && a_index < a_products->size() ? (*a_products)[a_index] : kNone;
		}
	}

	void BuildPayloadProducts(const Api::PayloadProductBuild& a_build, const PayloadProducts& a_previous, PayloadProducts& a_out)
	{
		const auto products = RegisteredProducts();
		a_out.resize(products.size());
		for (std::size_t i = 0; i < products.size(); ++i) {
			const Api::PayloadProductBuild build{ a_build.rows, a_build.tables, a_build.dirtyMaterials, a_build.everyMaterial, &ProductAt(&a_previous, i), a_build.check };
			a_out[i] = products[i].build(build);
		}
	}

	void CarryPayloadProducts(const PayloadProducts& a_main)
	{
		const auto products = RegisteredProducts();
		for (std::size_t i = 0; i < products.size(); ++i)
			if (products[i].carried)
				products[i].carried(ProductAt(&a_main, i));
	}

	void InstallPayloadProducts(const PayloadProducts* a_installed)
	{
		const auto products = RegisteredProducts();
		for (std::size_t i = 0; i < products.size(); ++i)
			products[i].install(ProductAt(a_installed, i));
	}
}
