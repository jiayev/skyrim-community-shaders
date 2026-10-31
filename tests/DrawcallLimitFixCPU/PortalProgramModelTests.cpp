// The program model (Portals/PortalProgramModel.h): random AND/OR expression trees of portal and occluder tests, emitted through the
// builders (Emit, EmitTest, ReopenAsOr, Merge, CopyMasked), finalized, encoded and evaluated by Visible, against the trees evaluated
// directly. The stream semantics are the engine's (Finalize's jump threading); the trees are the boolean meaning the builders intend.

#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramModel.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <random>

namespace
{
	using namespace DCLF;
	using namespace DCLF::PortalModel;

	struct Node
	{
		bool group = false;
		std::uint32_t kind = kPortalTest;  // a test's kind, or a group's (kAnd, kOr)
		PlaneSet set;
		std::vector<std::unique_ptr<Node>> children;
	};

	std::array<float, 4> RandomPlane(std::mt19937& a_rng)
	{
		std::normal_distribution<float> n;
		std::uniform_real_distribution<float> d(-40.0f, 40.0f);
		float x = n(a_rng), y = n(a_rng), z = n(a_rng);
		const float l = std::sqrt(x * x + y * y + z * z);
		if (l < 1e-3f)
			x = 1.0f, y = z = 0.0f;
		else
			x /= l, y /= l, z /= l;
		return { x, y, z, d(a_rng) };
	}

	PlaneSet RandomSet(std::mt19937& a_rng)
	{
		PlaneSet s;
		for (auto& p : s.plane)
			p = RandomPlane(a_rng);
		// Mostly every plane on; sometimes a partial mask or none (CopyMasked's turned-off set, a portal within 5 units).
		const std::uint32_t pick = a_rng() % 8;
		s.mask = pick == 0 ? 0u : pick == 1 ? (a_rng() & 0x3F) : 0x3Du;
		return s;
	}

	std::unique_ptr<Node> RandomTree(std::mt19937& a_rng, std::uint32_t a_depth)
	{
		auto node = std::make_unique<Node>();
		if (a_depth == 0 || a_rng() % 3 == 0) {
			node->kind = a_rng() % 3 == 0 ? kOccluderTest : kPortalTest;
			node->set = RandomSet(a_rng);
			return node;
		}
		node->group = true;
		node->kind = a_rng() % 2 ? kAnd : kOr;
		const std::uint32_t children = 1 + a_rng() % 4;
		for (std::uint32_t i = 0; i < children; ++i)
			node->children.push_back(RandomTree(a_rng, a_depth - 1));
		return node;
	}

	/** @brief A top-level group (the builders' streams are one group). */
	std::unique_ptr<Node> RandomRoot(std::mt19937& a_rng, std::uint32_t a_kind)
	{
		auto root = std::make_unique<Node>();
		root->group = true;
		root->kind = a_kind;
		const std::uint32_t children = 1 + a_rng() % 4;
		for (std::uint32_t i = 0; i < children; ++i)
			root->children.push_back(RandomTree(a_rng, 3));
		return root;
	}

	void EmitTree(Program& a_program, const Node& a_node)
	{
		if (!a_node.group) {
			const std::uint32_t s = a_program.freePlane;
			EmitTest(a_program, a_node.kind, s);
			Grow(a_program, s) = a_node.set;
			++a_program.freePlane;
			return;
		}
		Emit(a_program, a_node.kind);
		for (const auto& child : a_node.children)
			EmitTree(a_program, *child);
		Emit(a_program, kClose);
	}

	bool TestSet(const PlaneSet& a_set, std::uint32_t a_kind, const float a_c[3], float a_r)
	{
		return PortalBytecode::Test(a_set, a_kind == kPortalTest ? PortalBytecode::kPortal : PortalBytecode::kOccluder, a_c, a_r);
	}

	bool EvalTree(const Node& a_node, const float a_c[3], float a_r)
	{
		if (!a_node.group)
			return TestSet(a_node.set, a_node.kind, a_c, a_r);
		if (a_node.kind == kAnd) {
			for (const auto& child : a_node.children)
				if (!EvalTree(*child, a_c, a_r))
					return false;
			return true;
		}
		for (const auto& child : a_node.children)
			if (EvalTree(*child, a_c, a_r))
				return true;
		return false;
	}

	/** @brief The program's verdict through the encoding: one room program, no view planes, cull mode 4. */
	int Encoded(const Program& a_program, const float a_c[3], float a_r)
	{
		std::vector<std::uint32_t> words;
		const EncodeEntry entries[2]{ { nullptr, 0, false }, { &a_program, 4, true } };
		bool failed = false;
		Encode(PlaneSet{}, entries, words, [&](const EncodeError&) { failed = true; });
		assert(!failed);
		return Visible(words, 1, a_c, a_r, 0).visible;
	}

	void RandomQuery(std::mt19937& a_rng, float a_c[3], float& a_r)
	{
		std::uniform_real_distribution<float> p(-60.0f, 60.0f), r(0.01f, 25.0f);
		a_c[0] = p(a_rng), a_c[1] = p(a_rng), a_c[2] = p(a_rng);
		a_r = r(a_rng);
	}

	// Single trees: Emit, Finalize, Encode and Visible against the tree.
	void Trees()
	{
		std::mt19937 rng(0x7EE5);
		std::uint64_t checked = 0, accepted = 0;
		for (std::uint32_t t = 0; t < 400; ++t) {
			const auto root = RandomRoot(rng, rng() % 2 ? kAnd : kOr);
			Program program;
			Reset(program, {});
			EmitTree(program, *root);
			Finalize(program);
			assert(program.finalized);
			for (std::uint32_t q = 0; q < 200; ++q) {
				float c[3], r;
				RandomQuery(rng, c, r);
				const bool expected = EvalTree(*root, c, r);
				const int got = Encoded(program, c, r);
				if (got != (expected ? 1 : 0)) {
					std::fprintf(stderr, "tree %u query %u: expected %d, got %d\n", t, q, expected, got);
					assert(false);
				}
				++checked;
				accepted += expected;
			}
		}
		std::printf("model trees: %llu queries, %llu accepted\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(accepted));
		assert(accepted > checked / 20 && accepted < checked - checked / 20);
	}

	// A room reached again (BuildRoom): OR(the first path, AND(the second parent merged, the portal)). And a child room's masked copy:
	// AND(the parent with some sets turned off, the portal).
	void Composition()
	{
		std::mt19937 rng(0xC0DE);
		std::uint64_t checked = 0;
		for (std::uint32_t t = 0; t < 300; ++t) {
			// The first path: an AND group (ReopenAsOr wraps it whatever its set indices read as).
			const auto first = RandomRoot(rng, kAnd);
			const auto second = RandomRoot(rng, rng() % 2 ? kAnd : kOr);
			Node portal;
			portal.kind = kPortalTest;
			portal.set = RandomSet(rng);
			Program g, f;
			Reset(g, {});
			Reset(f, {});
			EmitTree(g, *first);
			EmitTree(f, *second);
			ReopenAsOr(g);
			Emit(g, kAnd);
			Merge(g, f);
			EmitTree(g, portal);
			Emit(g, kClose);
			Emit(g, kClose);
			Finalize(g);

			// The masked copy of the second path, a random half of its sets turned off.
			std::vector<bool> off;
			Program copy;
			CopyMasked(copy, f, [&](const PlaneSet&) {
				off.push_back(rng() % 2 == 0);
				return off.back();
			});
			Program child;
			Reset(child, {});
			Emit(child, kAnd);
			Merge(child, copy);
			EmitTree(child, portal);
			Emit(child, kClose);
			Finalize(child);
			// The reference copy: the same tree with the turned-off sets' masks cleared, in emission order.
			std::size_t leaf = 0;
			const auto masked = [&](auto&& a_self, const Node& a_node) -> std::unique_ptr<Node> {
				auto out = std::make_unique<Node>();
				out->group = a_node.group;
				out->kind = a_node.kind;
				out->set = a_node.set;
				if (!a_node.group && off.at(leaf++))
					out->set.mask = 0;
				for (const auto& c : a_node.children)
					out->children.push_back(a_self(a_self, *c));
				return out;
			};
			const auto secondMasked = masked(masked, *second);
			assert(leaf == off.size());

			for (std::uint32_t q = 0; q < 200; ++q) {
				float c[3], r;
				RandomQuery(rng, c, r);
				const bool reopened = EvalTree(*first, c, r) || (EvalTree(*second, c, r) && EvalTree(portal, c, r));
				const bool copied = EvalTree(*secondMasked, c, r) && EvalTree(portal, c, r);
				assert(Encoded(g, c, r) == (reopened ? 1 : 0));
				assert(Encoded(child, c, r) == (copied ? 1 : 0));
				checked += 2;
			}
		}
		std::printf("model composition: %llu queries\n", static_cast<unsigned long long>(checked));
	}

	// The node rules ahead of the program, on a program that rejects everything: the verdicts PortalTestAt makes without it.
	void NodeRules()
	{
		Program never;
		Reset(never, {});
		Emit(never, kAnd);
		PlaneSet nothing;
		nothing.mask = 1;
		nothing.plane[0] = { 1.0f, 0.0f, 0.0f, 1.0e6f };  // everything outside
		EmitTest(never, kPortalTest, 0);
		Grow(never, 0) = nothing;
		never.freePlane = 1;
		Emit(never, kClose);
		Finalize(never);
		std::vector<std::uint32_t> words;
		const auto encode = [&](std::uint32_t a_cullMode, bool a_reached) {
			const EncodeEntry entries[2]{ { nullptr, 0, false }, { a_reached ? &never : nullptr, a_cullMode, true } };
			Encode(PlaneSet{}, entries, words, [](const EncodeError&) { assert(false); });
		};
		const float c[3]{ 0, 0, 0 };
		encode(4, true);
		assert(Visible(words, 1, c, 1.0f, 0).visible == 0);
		assert(Visible(words, 1, c, 1.0f, 1u << 11).visible == 1);                              // always-draw
		assert(Visible(words, 1, c, 0.0f, 0).reason == Reason::kZeroRadius);                    // zero radius
		assert(Visible(words, 1, c, 1.0f, 1u << 12).visible == 0);                              // preprocessed ignored in mode 4
		assert(Visible(words, 2, c, 1.0f, 0).visible == -1);                                    // past the programs
		assert(Visible(words, kPortalNoProgram, c, 1.0f, 0).visible == -1);
		encode(1, true);
		assert(Visible(words, 1, c, 1.0f, 0).visible == 1);  // cull mode 1: untested
		encode(2, true);
		assert(Visible(words, 1, c, 1.0f, 0).visible == 0);  // cull mode 2
		encode(0, true);
		assert(Visible(words, 1, c, 1.0f, 1u << 12).visible == 1);               // preprocessed, shown
		assert(Visible(words, 1, c, 1.0f, (1u << 12) | (1u << 20)).visible == 0);  // preprocessed, hidden
		encode(3, true);
		assert(Visible(words, 1, c, 1.0f, 0).visible == 1);  // cull mode 3: the view alone
		encode(0, false);
		assert(Visible(words, 1, c, 1.0f, 0).reason == Reason::kNotReached);
		std::printf("model node rules: ok\n");
	}
}

int main()
{
	Trees();
	Composition();
	NodeRules();
	std::printf("PortalProgramModelTests passed\n");
	return 0;
}
