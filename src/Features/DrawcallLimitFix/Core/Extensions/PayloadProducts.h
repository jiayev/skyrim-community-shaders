#pragma once

// The payload product contract's Core side (Api/Payload.h): what a registered product's build reads, the products a payload carries,
// and the calls Core makes where they are built, published and installed.

#include "Features/DrawcallLimitFix/Api/Payload.h"

#include <cstdint>
#include <memory>
#include <span>
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
	/**
	 * @brief A product's build (the snapshot builder, MainBuild::Finish): what the main build made its rows from, read-only, and what moved
	 * since the product's previous build (MainRows::ProductDirty), so a build rewrites only that and returns its previous product when
	 * nothing did.
	 */
	class PayloadProductBuild
	{
	public:
		const Draws::MainRows& rows;   // the main rows, as this build left them
		const SceneTables& tables;     // the publication's tables the build read
		// The material slots whose rows or tables' material columns may have moved since *previous was built (unique, any order); every:
		// all of them (no previous to build from).
		std::span<const std::uint32_t> dirtyMaterials;
		bool everyMaterial = true;
		const std::shared_ptr<const void>* previous = nullptr;  // the product's last build (null or empty: none)
		bool check = false;  // the builder's parity (Policy::CheckBuilderPairs): the product made whole too, against what it built
	};
}

namespace DCLF::Extensions
{
	/** @brief A payload's products, by registration (Extensions::RegisteredProducts()); a slot may be null. */
	using PayloadProducts = std::vector<std::shared_ptr<const void>>;

	/**
	 * @brief Snapshot builder: every product's build into a_out (sized to the registrations; its capacity kept across builds), each from its
	 * a_previous (the last build's products, by registration: a_build's previous, set per product).
	 */
	void BuildPayloadProducts(const Api::PayloadProductBuild& a_build, const PayloadProducts& a_previous, PayloadProducts& a_out);
	/** @brief Snapshot builder (PayloadPools::MakeSource of a payload with main parts): each product's carried, with the main payload's. */
	void CarryPayloadProducts(const PayloadProducts& a_main);
	/** @brief Render thread (AdoptSnapshot): each product's install, with a_installed's (null: no payload's). */
	void InstallPayloadProducts(const PayloadProducts* a_installed);
}
