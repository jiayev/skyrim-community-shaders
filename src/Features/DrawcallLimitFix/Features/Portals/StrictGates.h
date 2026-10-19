#pragma once

// The portal-strict point lights' per-object gate (dclf-portal-query-batches.md, "Strict point-light gates"): what a strict light's
// native cull (BSParabolicCullingProcess from Process2's root, mode 4) decides about an object from its ancestry and its own bound, as
// StrictLightParity's per-object predicate measured it against the engine's appends. The gating ancestors are the rooms
// (BSMultiBoundRoom::OnVisible: a room the light's walk did not reach is not culled into; a reached one culls its children through its
// compound frustum), the portals' shared nodes (BSPortalSharedNode::OnVisible: only those the walk crossed, with the sphere alone) and
// the kAllPass multibound nodes (BSMultiBoundNode::OnVisible: their children untested). Everything below a fade root (switches, fades,
// hidden nodes) is the same for every light and DCLF's own already.
//
// The CPU side (ChainOf, Evaluate, Admit) serves the receivers' bits (Features/Shadows/Local's NoteMainRegistration) and the parity; the
// GPU side (Features/LocalLights' LightContextResources) is a versioned column of each object's gate (its fade root's chain's, its own
// chain's without a root), kept while the objects, roots and rooms hold, and a per-frame publication of each gate's verdict per context
// of the frame's atlas, which PortalPrograms.hlsli's StrictGateRejects reads with the atlas.

#include "Features/DrawcallLimitFix/Features/Portals/PortalContext.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"

#include <cstdint>
#include <span>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Portals
{
	/** @brief A context's programs (PortalPrograms.h's encoding) as a strict light's Process1 reads them. */
	class ProgramWords
	{
	public:
		explicit ProgramWords(std::span<const std::uint32_t> a_words);
		std::uint32_t Count() const { return count; }
		/** @brief The walk reached the program's room (kPortalReached). */
		bool Reached(std::uint32_t a_program) const;
		/** @brief The room's compound frustum has operators (freeOp): Process1 tests it. */
		bool HasOps(std::uint32_t a_program) const;
		bool SkipView(std::uint32_t a_program) const;
		/** @brief BSCompoundFrustum::Process of the program's finalized frustum, as its validated instructions run it. */
		bool Pass(std::uint32_t a_program, const float a_centre[3], float a_radius) const;

	private:
		const std::uint32_t* Entry(std::uint32_t a_program) const;
		std::span<const std::uint32_t> words;
		std::uint32_t count = 0, records = 0;
	};

	/** @brief A gating ancestor, as the strict cull meets it walking down. */
	struct GateLink
	{
		enum Kind : std::uint8_t
		{
			kRoom,     // BSMultiBoundRoom: program (1 + its room index; kPortalNoProgram: not the walked graph's), its culling mode
			kShared,   // BSPortalSharedNode
			kAllPass,  // BSMultiBoundNode with cullingMode kAllPass
			kShape,    // BSMultiBoundNode with a shape: tested by the engine outside modes 1, 3 and 4 (not modelled)
		};
		Kind kind = kRoom;
		const void* node = nullptr;
		std::uint32_t program = kPortalNoProgram;
		std::uint32_t roomMode = 0;
		bool operator==(const GateLink&) const = default;
	};
	using GateChain = std::vector<GateLink>;  // top-down

	/**
	 * @brief a_object's gating ancestors, itself included, top-down (at most 256 levels). Reads parents, vtables and PortalViews' room index:
	 * the render thread, or a cull's thread while the graph does not change.
	 */
	void ChainOf(const RE::NiAVObject* a_object, GateChain& a_out);

	/**
	 * @brief What a strict light's cull (a_context: its walk's snapshot) does under a chain: closed (an unreached room or shared node, mode 2),
	 * else whether its objects are untested (mode 1) and the room program whose compound frustum their own bound is tested in
	 * (kPortalNoProgram: the sphere alone). unmodelled: a multibound shape the engine would test (the verdict is open).
	 */
	struct GateVerdict
	{
		bool open = true, untested = false, unmodelled = false;
		std::uint32_t program = kPortalNoProgram;
	};
	GateVerdict Evaluate(std::span<const GateLink> a_chain, const Context& a_context);

	/** @brief The per-object predicate besides the sphere: a_chain's verdict, then the bound in its program's compound frustum. */
	bool Admit(const Context& a_context, std::span<const GateLink> a_chain, const float a_centre[3], float a_radius);

	/**
	 * @brief The verdicts publication's words (a ByteAddressBuffer, PortalPrograms.hlsli's StrictGateRejects): header {gate count, context
	 * count, atlas identity low, atlas identity high, 0, 0, 0, 0}, then per gate per context of the atlas its verdict: kGateClosed,
	 * kPortalNoProgram (open, the sphere alone) or the program whose compound frustum the object's bound must pass. Gate 0 is the empty chain
	 * (open everywhere): an object past the column, or a gate past the count, takes it.
	 */
	inline constexpr std::uint32_t kGateHeaderWords = 8;
	inline constexpr std::uint32_t kGateClosed = 0xFFFFFFFEu;
	/** @brief The verdict word of a gate for one context (a camera context: open). */
	std::uint32_t GateWord(std::span<const GateLink> a_chain, const Context& a_context);
}
