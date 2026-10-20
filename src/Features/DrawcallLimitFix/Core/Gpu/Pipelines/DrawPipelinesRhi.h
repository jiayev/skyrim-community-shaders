#pragma once

// BasicRHI view of DrawPipelines, for the graph pass (include after rhi_interop_vulkan.h).

#include <array>
#include <memory>
#include <vector>
#include <rhi.h>

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

namespace DCLF
{
	/** @brief What an ExecuteIndirect of DCLF's draws binds; invalid until the first pipeline is in the set. */
	struct IndirectState
	{
		rhi::PipelineLayoutHandle layout{};
		/**
		 * @brief A state class (DrawPipelines.h, kSetClassShift): its indirect execution set and the signature that selects from it, by
		 * variant, and how many pipelines the version's sets hold of it. Every device-generated call uses one class's signature and
		 * draws one class's sub-range of a range, whose sequences name pipelines by their index in that class.
		 */
		struct Class
		{
			std::array<rhi::IndirectPipelineSetHandle, kVariantCount> sets{};
			std::array<rhi::CommandSignatureHandle, kVariantCount> signatures{};
			std::uint32_t pipelines = 0;
		};
		std::vector<Class> classes;  // in creation order (the calls' order within a range)
		// By set index (DrawInput::pipelineIndex): class << kSetClassShift | index in the class. The class map BuildDraws routes every
		// colour draw by, so a sequence's pipeline index and the sub-range it lands in come from one word.
		std::vector<std::uint32_t> classOf;
		// The Z-prepass's plain indirect draws (MainOpaquePass): every published pipeline's group (the pipelines that build the
		// same pulled depth pipeline, DrawPipelinesImpl.h ZPipelineKey) by its index, each group's pulled depth pipeline, their
		// layout and the signature of a DrawSequence's tail as a plain draw.
		std::vector<std::uint32_t> zGroups;
		std::vector<rhi::PipelineHandle> zPipelines;
		// By set index: a forward key's plain colour draws' pipeline (DrawPipelinesImpl.h, Built::pulledColour), under zLayout with
		// zDrawSignature (ForwardDrawPass); null for any other key and for one whose pulled stages would not build.
		std::vector<rhi::PipelineHandle> plainColour;
		// By set index: its key's own plain depth draws' pipeline (a Lighting key's Built::pulledDepth, an effect decal key's
		// pulledDecalDepth), for the decal depth pass's decal buckets; null for a key without one.
		std::vector<rhi::PipelineHandle> plainDepth;
		rhi::PipelineLayoutHandle zLayout{};
		rhi::CommandSignatureHandle zDrawSignature{};
		// The refraction draw (DrawPipelines::RequestRefraction; RefractionDrawPass): its pipeline, null while it waits or once it
		// failed, the refraction normals' format it was built for, and the shadow views' layout and plain draw signature it draws under.
		rhi::PipelineHandle refraction{};
		DXGI_FORMAT refractionFormat = DXGI_FORMAT_UNKNOWN;
		rhi::PipelineLayoutHandle refractionLayout{};
		rhi::CommandSignatureHandle refractionDrawSignature{};
		// The set version these handles belong to (DrawPipelinesImpl.h, SetVersion): while any copy of this state lives, the
		// version's sets are not written, so hold it for as long as the handles are recorded with.
		std::shared_ptr<const void> version;
		bool valid = false;
	};

	/**
	 * @brief The main set version of a_catalog (the one its lookups' indices are of), for the targets of generation a_targetsGeneration
	 * (DrawPipelines::Generation, as the caller has it: the frame's, or a revision's inputs). Invalid once those targets are not the ones
	 * the catalog's set was built for (SetTargetFormats): its pipelines draw into the old ones. Any thread: it reads the catalog (immutable)
	 * and the lane's layouts (made once, at setup).
	 */
	IndirectState GetIndirectState(const PipelineCatalog& a_catalog, std::uint32_t a_targetsGeneration);

	/**
	 * @brief The shadow views' pipelines (DrawPipelines::RequestShadow): the shadow set version of a catalog (GetShadowIndirectState),
	 * the one its shadow lookups' indices are of; valid once that catalog has one.
	 */
	struct ShadowIndirectState
	{
		rhi::PipelineLayoutHandle layout{};
		// The shadow views' plain indirect draws (ShadowViewPass): every published pipeline by its index, its geometry's vertex
		// layout (ShadowPipelineKey::vertexLayout, which the pulling vertex stage decodes) and its class (kShadowDiscards: its pixel
		// stage can discard or export depth, so a view draws it after the other class), and the signature of a DrawSequence's tail
		// as a plain draw.
		std::vector<rhi::PipelineHandle> pipelines;
		std::vector<std::uint64_t> vertexLayouts;
		std::vector<std::uint8_t> discards;
		rhi::CommandSignatureHandle drawSignature{};
		std::shared_ptr<const void> version;  // as IndirectState::version
		bool valid = false;
	};

	/**
	 * @brief The shadow set version of a_catalog (the one its shadow lookups' indices are of), for the shadow map format a_shadowFormat
	 * (DrawPipelines::ShadowFormat, as the caller has it). Invalid once the format is not the one the catalog's set was built for
	 * (SetShadowInputs). Any thread, as GetIndirectState.
	 */
	ShadowIndirectState GetShadowIndirectState(const PipelineCatalog& a_catalog, DXGI_FORMAT a_shadowFormat);

	/**
	 * @brief Render thread: the frame's (FrameCatalog, the installed publication's, for the frame's targets and shadow format). The one
	 * place the frame's epochs take their indirect states from; a revision makes its own from its request's catalog and its inputs.
	 */
	IndirectState FrameIndirectState();
	ShadowIndirectState FrameShadowIndirectState();

	/**
	 * @brief A forward view's pipeline (DrawPipelines::RequestForward) as a_catalog (the frame's: FrameCatalog) has it, for the targets
	 * a_targets. Null while it waits (not built, or built for other targets), and when it failed. The lane keeps every forward
	 * pipeline it built for the process, so a handle stays valid for as long as anything records with it.
	 */
	rhi::PipelineHandle ForwardPipelineOf(const PipelineCatalog* a_catalog, const ForwardPipelineKey& a_key, const ForwardTargets& a_targets);

	/**
	 * @brief A draw family's pipelines and its draws' signature (DrawPipelines::RequestFamily; the slot's signature,
	 * Api::DrawFamilySignature::DrawInstanced).
	 */
	struct FamilyPipelines
	{
		rhi::PipelineHandle depth{}, colour{};
		rhi::CommandSignatureHandle drawSignature{};  // one non-indexed DrawInstanced's arguments (16 bytes)
	};

	/**
	 * @brief a_family's pipelines as a_catalog (the frame's: FrameCatalog) has them: false while they wait, when they failed, and once the
	 * targets changed after that catalog's were built for them (as GetIndirectState). Kept by the lane for the process, as the forward ones.
	 */
	bool CatalogFamilyPipelines(const PipelineCatalog* a_catalog, Api::DrawFamily a_family, FamilyPipelines& a_out);
	/** @brief As above, for the targets of generation a_targetsGeneration (a revision's inputs'): any thread. */
	bool CatalogFamilyPipelines(const PipelineCatalog* a_catalog, Api::DrawFamily a_family, std::uint32_t a_targetsGeneration, FamilyPipelines& a_out);
}
