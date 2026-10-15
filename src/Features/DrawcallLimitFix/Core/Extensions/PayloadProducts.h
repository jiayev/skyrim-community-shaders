#pragma once

// The payload product contract's Core side (Api/Payload.h): what a registered product's build reads, the products a payload carries,
// and the calls Core makes where they are built, published and installed.

#include "Features/DrawcallLimitFix/Api/Payload.h"

#include <memory>
#include <vector>

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct MainRows;
	}
}

namespace DCLF::Api
{
	/** @brief A product's build (the snapshot builder, MainBuild::Finish): what the main build made its rows from, read-only. */
	class PayloadProductBuild
	{
	public:
		const Draws::MainRows& rows;   // the main rows, as this build left them
		const SceneTables& tables;     // the publication's tables the build read
	};
}

namespace DCLF::Extensions
{
	/** @brief A payload's products, by registration (Extensions::RegisteredProducts()); a slot may be null. */
	using PayloadProducts = std::vector<std::shared_ptr<const void>>;

	/** @brief Snapshot builder: every product's build into a_out (sized to the registrations; its capacity kept across builds). */
	void BuildPayloadProducts(const Api::PayloadProductBuild& a_build, PayloadProducts& a_out);
	/** @brief Snapshot builder (PayloadBuilder::Publish of a payload with main parts): each product's carried, with the main payload's. */
	void CarryPayloadProducts(const PayloadProducts& a_main);
	/** @brief Render thread (AdoptSnapshot): each product's install, with a_installed's (null: no payload's). */
	void InstallPayloadProducts(const PayloadProducts* a_installed);
}
