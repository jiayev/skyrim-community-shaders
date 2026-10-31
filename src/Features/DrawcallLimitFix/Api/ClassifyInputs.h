#pragma once

// The classify input contract (Api::InstallContext::RegisterClassifyInput): a world fact outside the toggles that classification reads,
// such as whether a CS feature holds a class of objects back from the opaque pass (Integrations/TerrainBlending's terrain). Core samples
// every input once a frame where the toggles enter classification (a change drops the classification caches, as an entersClassification
// toggle's does), and a scene pass takes them with the toggles at its start (Extensions::ClassifyInputsScope), so the pass classifies by
// one answer while the menu switches the feature. What reads an input (a technique gate, a held group) names it by its id. A feature that
// reads an input another registers (one installed after it, or maybe never) asks for it by name (InstallContext::NeedClassifyInput).

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief A registered classify input (RegisterClassifyInput's result), or one needed by name (NeedClassifyInput's: bound once every
	 * feature and integration installed, reading false while unbound): Extensions::ClassifyInputOn reads it by this.
	 */
	struct ClassifyInputId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A classify input.
	 *
	 * - name: what the install's failures call it.
	 * - sample: any thread, per frame and per classification outside a scene pass: the fact's live value. Cheap (a load or two), reading
	 *   only state the feature keeps itself; no engine read.
	 */
	struct ClassifyInputContribution
	{
		std::string_view name;
		bool (*sample)() = nullptr;
	};
}
