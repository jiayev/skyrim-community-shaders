#pragma once

// The main cull input contract's Core side (Api/CullInputs.h): the input's entry, resolved once at InstallFeatures (none registered: no
// walk); the batch its judge answers (RootProgramsJudge); and whether its walk's hook is in, which the primary cull's install requires
// (noted from the feature's early hooks, before InstallFeatures). And the cull bindings': what a binding's select is handed and answers
// (CullBindingSelect), the entries by slot, resolved once at InstallFeatures, and the selection Core's cull makes per pass or commit.

#include "Features/DrawcallLimitFix/Api/CullInputs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org
{
	class Buffer;
	class VersionedBuffer;
	namespace services
	{
		class NamedVersions;
	}
}

namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Api
{
	/**
	 * @brief The fade roots a depth commit asks the judge about (render thread, UploadFadeRootPrograms): Count of them, each one's node
	 * (Root: non-null, a root without one takes noProgram unasked), and the answer Put back by the same index. Core keeps the batch's
	 * storage beside its programs mirror (Draws::FadeScene::fadeRootProgramsBatch): no allocation per commit once grown.
	 */
	class RootProgramsJudge
	{
	public:
		std::size_t Count() const { return asked.size(); }
		const void* Root(std::size_t a_index) const { return nodes[roots[asked[a_index]]]; }
		void Put(std::size_t a_index, std::uint32_t a_program) const { programs[asked[a_index]] = a_program; }

		std::span<const void* const> nodes;     // by root slot (Extensions::RootListOf's nodes)
		std::span<const std::uint32_t> roots;   // the roots judged, in Core's order
		std::span<const std::uint32_t> asked;   // by question: its place in roots (those with a node)
		std::span<std::uint32_t> programs;      // by place in roots: the answers
	};

	/**
	 * @brief A cull binding's select (render thread, per pass or commit): the scene's tables (Extensions::SceneResourcesOf finds the
	 * feature's state in them), the frame and the adopted set; select answers with one of Versioned, Buffer or Index (none: unbound).
	 */
	class CullBindingSelect
	{
	public:
		const Draws::SceneBuffers& scene;
		std::uint32_t frame = 0;                                     // the scene frame whose cull reads it (a binding may alternate by frame)
		const org::services::NamedVersions* adopted = nullptr;       // the set a commit or a preparation reads (null at a declaration)

		/** @brief A versioned buffer of the scene's set (the feature's scene resources: made at setup, never replaced). */
		void Versioned(const std::shared_ptr<org::VersionedBuffer>& a_buffer) { versioned = &a_buffer; }
		/** @brief A plain buffer made at the scene's setup (the feature's scene resources). */
		void Buffer(const std::shared_ptr<org::Buffer>& a_buffer) { buffer = &a_buffer; }
		/** @brief A pooled buffer's SRV index this frame (a frame value pool's: 0 none), which no pass declares. */
		void Index(std::uint32_t a_srv) { srv = a_srv; }
		/** @brief With Versioned or Buffer: how many of its units hold the content its readers may read now (a by-root buffer: the roots the
		 *  depth commit sent; 0: none yet, its readers bind none). Unanswered: all it names. */
		void Rows(std::uint32_t a_rows) { rows = a_rows; }

		// The answer (Core reads it).
		const std::shared_ptr<org::VersionedBuffer>* versioned = nullptr;
		const std::shared_ptr<org::Buffer>* buffer = nullptr;
		std::uint32_t srv = 0;
		std::uint32_t rows = ~0u;
	};
}

namespace DCLF::Extensions
{
	/** @brief The input's entry: its registration (copied, its name owned). */
	struct MainCullInputEntry
	{
		std::string name;
		Api::MainCullInputContribution contribution;
	};

	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no name: none). */
	void ResolveMainCullInput(const Api::MainCullInputContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): the input's entry, or null without one. */
	const MainCullInputEntry* MainCullInput();

	/** @brief Main thread at PostPostLoad, from a feature's early hooks: the main walk's hook is in. */
	void NoteMainWalkInstalled();
	/** @brief Main thread at PostPostLoad (PrimaryCull::Install): whether NoteMainWalkInstalled came; the primary cull requires it. */
	bool MainWalkInstalled();

	/** @brief The cull binding slots (Api::CullBindingSlot::Count). */
	inline constexpr std::size_t kCullBindingSlots = static_cast<std::size_t>(Api::CullBindingSlot::Count);
	/** @brief A slot's binding: its registration (copied, its name owned). */
	struct CullBindingEntry
	{
		std::string name;
		Api::CullBinding binding;
	};

	/** @brief Main thread, the end of InstallFeatures: the entries from the registrations, by slot (no select: none). */
	void ResolveCullBindings(const std::array<Api::CullBinding, kCullBindingSlots>& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): a_slot's binding, or null without one. */
	const CullBindingEntry* CullBindingOf(Api::CullBindingSlot a_slot);
	/**
	 * @brief Render thread, per pass (its declaration, revision or preparation) or commit, never per object: a_slot's select run for
	 * a_scene, a_frame and a_adopted (null at a declaration); no answer without a binding.
	 */
	Api::CullBindingSelect SelectCullBinding(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame = 0,
		const org::services::NamedVersions* a_adopted = nullptr);
	/**
	 * @brief SelectCullBinding's versioned answer for a pass's declaration and preparation (Declare: bound when non-null): the
	 * scene resources' handle, or a null one held by Core (no binding, or a binding answering otherwise). Stable while a_scene lives.
	 */
	const std::shared_ptr<org::VersionedBuffer>& CullBindingBuffer(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame = 0);
}
