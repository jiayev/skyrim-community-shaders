#pragma once

// The payload product contract (Api::InstallContext::RegisterPayloadProduct): an immutable value a feature builds with every main
// payload, carried by the snapshot's payload and installed with it on the render thread.

#include <memory>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/PayloadProducts.h), which the features inside DCLF include for them.
	class PayloadProductBuild;

	/**
	 * @brief A payload product.
	 *
	 * - build: the snapshot builder (DCLF's pool), at the end of each main build (MainBuild::Finish), given read access to the build's
	 *   rows and tables, its previous result and the material slots that may have moved since. Its result (null allowed) is immutable
	 *   from here and shared by both segments' payloads; it may hold engine pointers only while the publication the payload belongs to
	 *   holds them. Nothing moved: it may return its previous result (the same object), whose pointers the moves it was told of keep
	 *   current.
	 * - carried (optional): the snapshot builder, when a payload with main parts is published (PayloadBuilder::Publish), with the main
	 *   payload's product (null when its build made none).
	 * - install: render thread, when the frame adopts a snapshot whose payload is not the installed one (AdoptSnapshot), with the
	 *   installed payload's product (null: no payload, or one without main parts). It stays valid until the next install.
	 */
	struct PayloadProduct
	{
		std::string_view name;
		std::shared_ptr<const void> (*build)(const PayloadProductBuild& a_build) = nullptr;
		void (*carried)(const std::shared_ptr<const void>& a_product) = nullptr;
		void (*install)(std::shared_ptr<const void> a_product) = nullptr;
	};
}
