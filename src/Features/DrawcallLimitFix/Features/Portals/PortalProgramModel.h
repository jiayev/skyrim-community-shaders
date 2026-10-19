#pragma once

// The engine-independent half of PortalViews: BSCompoundFrustum's token stream and its builders (Emit, Finalize, ReopenAsOr,
// CompactEmptyGroups, Merge, the masked copy), the programs' encoding (PortalPrograms.h) and its CPU evaluation (PortalPrograms.hlsli's
// PortalTestAt). PortalViews builds the frame's programs with these from the engine's shapes; the CPU tests and the portal bench
// (tests/DclfPortalBench) build synthetic ones with the same code, and evaluate any encoding as the oracle.

#include "Features/DrawcallLimitFix/Features/Portals/PortalBytecode.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace DCLF::PortalModel
{
	// The token words (4 AND, 5 OR, 6 close, 7 s a portal test, 8 s an occluder test; Finalize appends 1 root, 2 ACCEPT, 3 REJECT).
	inline constexpr std::uint32_t kAnd = 4, kOr = 5, kClose = 6, kPortalTest = 7, kOccluderTest = 8;

	inline bool IsTest(std::uint32_t a_word) { return a_word == kPortalTest || a_word == kOccluderTest; }
	inline bool IsOpen(std::uint32_t a_word) { return a_word == kAnd || a_word == kOr; }

	/** @brief Every plane test's distance: ((n.y * p.y + n.x * p.x) + n.z * p.z) - d. */
	inline float Dist(const std::array<float, 4>& a_plane, float a_x, float a_y, float a_z)
	{
		return ((a_plane[1] * a_y + a_plane[0] * a_x) + a_plane[2] * a_z) - a_plane[3];
	}

	/** @brief FUN_140d32ab0: the plane through a, b, c (n = (b - a) x (c - b), normalised; zero below 1e-6). */
	inline std::array<float, 4> PlaneFromPoints(float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz)
	{
		const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
		const float e2x = cx - bx, e2y = cy - by, e2z = cz - bz;
		float nx = e2z * e1y - e2y * e1z;
		float ny = e2x * e1z - e2z * e1x;
		float nz = e2y * e1x - e2x * e1y;
		const float length = std::sqrt((ny * ny + nx * nx) + nz * nz);
		if (length <= 1e-6f) {
			nx = ny = nz = 0.0f;
		} else {
			const float inverse = 1.0f / length;
			nx *= inverse;
			ny *= inverse;
			nz *= inverse;
		}
		return { nx, ny, nz, (ny * ay + nx * ax) + nz * az };
	}

	/** @brief NiFrustumPlanes' layout: six planes {n, d} (dot(n, p) - d >= 0 inside), active mask, base mask. */
	struct PlaneSet
	{
		std::array<std::array<float, 4>, 6> plane{};
		std::uint32_t mask = 0;
		std::uint32_t base = 0x3F;
		std::uint32_t extra[2]{};
	};
	static_assert(sizeof(PlaneSet) == 0x70 && sizeof(PlaneSet) == kPortalSetBytes);

	/** @brief A BSCompoundFrustum operator record: token (4 AND, 5 OR, 6 close, 7/8 a test whose next record is its set), branches. */
	struct Op
	{
		std::uint32_t word = 0, yes = 0, no = 0;
	};

	/** @brief A compound frustum: the token stream (Finalize makes it a branch program) and its plane sets. */
	struct Program
	{
		std::vector<Op> ops;         // freeOp tokens, then (finalized) root, ACCEPT and REJECT
		std::vector<PlaneSet> sets;  // freePlane in use
		std::uint32_t freeOp = 0, freePlane = 0, firstOp = 0;
		bool skipView = false, finalized = false;
		std::array<float, 3> eye{};  // the camera's position (Reset)
	};

	// ---- BSCompoundFrustum's builders ----

	/** @brief FUN_140e32580: the counts and flags; the arrays' contents stay. */
	inline void Reset(Program& a_program, const std::array<float, 3>& a_eye)
	{
		a_program.eye = a_eye;
		a_program.freePlane = a_program.freeOp = a_program.firstOp = 0;
		a_program.skipView = a_program.finalized = false;
	}

	/** @brief FUN_140e32600 (AND: leaves finalized), FUN_140e32650 (OR), FUN_140e326b0 (close): both clear it. */
	inline void Emit(Program& a_program, std::uint32_t a_word)
	{
		if (a_program.ops.size() < a_program.freeOp + 4)
			a_program.ops.resize(a_program.freeOp + 16);
		a_program.ops[a_program.freeOp++].word = a_word;
		if (a_word != kAnd)
			a_program.finalized = false;
	}

	inline void EmitTest(Program& a_program, std::uint32_t a_type, std::uint32_t a_set)
	{
		if (a_program.ops.size() < a_program.freeOp + 5)
			a_program.ops.resize(a_program.freeOp + 16);
		a_program.ops[a_program.freeOp++].word = a_type;
		a_program.ops[a_program.freeOp++].word = a_set;
	}

	inline PlaneSet& Grow(Program& a_program, std::uint32_t a_index)
	{
		if (a_program.sets.size() <= a_index)
			a_program.sets.resize(a_index + 4);
		return a_program.sets[a_index];
	}

	/** @brief FUN_140e33c70: the token stream into a branch program, then jump threading. */
	inline void Finalize(Program& a_program)
	{
		auto& p = a_program;
		if (p.finalized || p.freeOp == 0) {
			p.finalized = true;
			return;
		}
		const std::uint32_t n = p.freeOp;
		if (p.ops.size() < n + 4)
			p.ops.resize(n + 16);
		auto& ops = p.ops;
		ops[n].word = 1;
		ops[n + 1].word = 2;
		ops[n + 2].word = 3;
		// Pass 1: each group's closing index (groups numbered from 1 in opener order; group 0 is the top level).
		std::vector<std::uint32_t> closeIndex(n + 2, 0), ids(n + 2, 0);
		std::uint32_t depth = 0, nextId = 1;
		for (std::uint32_t i = 0; i <= n; ++i) {
			const std::uint32_t w = ops[i].word;
			if (w == 1) {
				closeIndex[0] = i;
				if (depth != 0) {
					p.freeOp = 0;  // unbalanced: wiped, not finalized
					return;
				}
			} else if (IsOpen(w)) {
				ids[++depth] = nextId++;
			} else if (w == kClose) {
				if (depth == 0)
					break;
				closeIndex[ids[depth]] = i;
				--depth;
			} else if (IsTest(w)) {
				++i;
			}
		}
		// Pass 2: the branch targets.
		std::vector<std::uint32_t> typeStack{ 0 }, idStack{ 0 };
		nextId = 1;
		bool found = false;
		for (std::uint32_t i = 0; i <= n; ++i) {
			auto& op = ops[i];
			const std::uint32_t w = op.word;
			if (w == 1) {
				op.yes = n + 1;
				op.no = n + 2;
			} else if (IsOpen(w)) {
				typeStack.push_back(w);
				idStack.push_back(nextId++);
				op.yes = op.no = i + 1;
			} else if (w == kClose) {
				if (typeStack.size() > 1) {
					typeStack.pop_back();
					idStack.pop_back();
				}
				const std::uint32_t type = typeStack.back(), id = idStack.back();
				if (type == kAnd) {
					op.yes = i + 1;
					op.no = closeIndex[id];
				} else if (type == kOr) {
					op.yes = closeIndex[id];
					op.no = i + 1;
				} else {
					op.yes = op.no = i + 1;
				}
			} else if (IsTest(w)) {
				if (!found) {
					p.firstOp = i;
					found = true;
				}
				const std::uint32_t type = typeStack.back(), id = idStack.back();
				if (type == kAnd) {
					op.yes = i + 2;
					op.no = closeIndex[id];
				} else if (type == kOr) {
					op.yes = closeIndex[id];
					op.no = i + 2;
				} else {
					op.yes = op.no = n;
				}
				++i;
			}
		}
		if (!found) {
			p.firstOp = n + 1;
			p.finalized = true;
			return;
		}
		// Pass 3: tests point straight at tests, ACCEPT or REJECT.
		const auto terminal = [&](std::uint32_t a_index) { return a_index >= ops.size() || IsTest(ops[a_index].word) || ops[a_index].word == 2 || ops[a_index].word == 3; };
		for (std::uint32_t i = p.firstOp; i < p.freeOp; ++i) {
			if (!IsTest(ops[i].word))
				continue;
			for (std::uint32_t guard = 0; !terminal(ops[i].yes) && guard < 4096; ++guard)
				ops[i].yes = ops[ops[i].yes].yes;
			for (std::uint32_t guard = 0; !terminal(ops[i].no) && guard < 4096; ++guard)
				ops[i].no = ops[ops[i].no].no;
			++i;
		}
		p.finalized = true;
	}

	/**
	 * @brief FUN_140e334c0: a stream that is one OR group drops its close; any other is wrapped in a new, unclosed OR. The scan does not
	 * skip the set-index words (an index of 4, 5 or 6 reads as a token), as the engine's.
	 */
	inline void ReopenAsOr(Program& a_program)
	{
		auto& p = a_program;
		bool wrap = true;
		if (p.freeOp > 0 && p.ops[0].word == kOr) {
			std::int32_t depth = 1;
			std::uint32_t i = 1;
			do {
				if (i >= p.freeOp)
					break;
				const std::uint32_t w = p.ops[i].word;
				if (w > 3 && w < 6)
					++depth;
				else if (w == kClose)
					--depth;
				++i;
			} while (depth > 0);
			if (i == p.freeOp && depth == 0)
				wrap = false;
		}
		if (!wrap) {
			--p.freeOp;
			return;
		}
		if (p.ops.size() < p.freeOp + 4)
			p.ops.resize(p.freeOp + 16);
		for (std::uint32_t i = p.freeOp; i > 0; --i)
			p.ops[i].word = p.ops[i - 1].word;
		++p.freeOp;
		p.ops[0].word = kOr;
	}

	/** @brief FUN_140e33b90: one pass taking out adjacent {open, close} pairs. */
	inline void CompactEmptyGroups(Program& a_program)
	{
		auto& p = a_program;
		const std::uint32_t original = p.freeOp;
		if (original == 0)
			return;
		if (p.ops.size() < original + 4)
			p.ops.resize(original + 16);
		const auto word = [&](std::uint32_t a_index) { return a_index < p.ops.size() ? p.ops[a_index].word : 0u; };
		std::uint32_t i = 0, k = 0, current = original;
		do {
			while (current > 0 && IsOpen(word(i + k)) && word(i + k + 1) == kClose) {
				current -= 2;
				k += 2;
				p.freeOp = current;
				if (!(i < current))
					break;
			}
			const std::uint32_t j = i + k;
			if (j < original) {
				p.ops[i].word = word(j);
				if (IsTest(p.ops[i].word)) {
					++i;
					p.ops[i].word = word(j + 1);
				}
			}
			current = p.freeOp;
			++i;
		} while (i < current);
	}

	/** @brief FUN_140e33130: the source's tokens and sets appended, its set indices rebased. */
	inline void Merge(Program& a_destination, const Program& a_source)
	{
		auto& d = a_destination;
		d.finalized = false;
		if (d.ops.size() < d.freeOp + a_source.freeOp + 4)
			d.ops.resize(d.freeOp + a_source.freeOp + 16);
		if (d.sets.size() < d.freePlane + a_source.freePlane + 1)
			d.sets.resize(d.freePlane + a_source.freePlane + 4);
		for (std::uint32_t i = 0; i < a_source.freeOp; ++i)
			d.ops[d.freeOp + i].word = a_source.ops[i].word;
		for (std::uint32_t j = 0; j < a_source.freePlane; ++j)
			d.sets[d.freePlane + j] = a_source.sets[j];
		if (d.freePlane != 0)
			for (std::uint32_t i = d.freeOp; i < d.freeOp + a_source.freeOp; ++i)
				if (IsTest(d.ops[i].word)) {
					d.ops[i + 1].word += d.freePlane;
					++i;
				}
		d.freeOp += a_source.freeOp;
		d.freePlane += a_source.freePlane;
	}

	/**
	 * @brief FUN_140e33740 (after Clear, FUN_140e32040): the parent's stream for a child room, each set a_turnOff(set) names (the
	 * portal lies wholly inside its planes with no edge clipped) turned off (mask 0) for it.
	 */
	template <class TurnOff>
	void CopyMasked(Program& a_out, const Program& a_parent, TurnOff&& a_turnOff)
	{
		auto& t = a_out;
		t.freePlane = t.freeOp = t.firstOp = 0;
		t.skipView = t.finalized = false;
		const std::uint32_t n = a_parent.freeOp;
		if (t.ops.size() < n + 4)
			t.ops.resize(n + 16);
		if (t.sets.size() < a_parent.freePlane + 1)
			t.sets.resize(a_parent.freePlane + 4);
		for (std::uint32_t i = 0; i < n; ++i) {
			const std::uint32_t w = a_parent.ops[i].word;
			t.ops[i].word = w;
			if (IsTest(w) && i + 1 < n) {
				const std::uint32_t s = a_parent.ops[i + 1].word;
				const PlaneSet& set = s < a_parent.sets.size() ? a_parent.sets[s] : PlaneSet{};
				const bool off = a_turnOff(set);
				// The set keeps its index (in a builder's stream the tests' sets ascend, so it is the running count too).
				t.ops[i + 1].word = t.freePlane;
				Grow(t, std::max(s, t.freePlane));
				t.sets[s] = set;
				++t.freePlane;
				if (off)
					t.sets[s].mask = 0;
				++i;
			}
		}
		t.freeOp = n;
		CompactEmptyGroups(t);
	}

	// ---- The encoding (PortalPrograms.h) ----

	/** @brief One directory entry's source: a program (null: not reached), its room's cull mode, and whether it is a room's. */
	struct EncodeEntry
	{
		const Program* program = nullptr;
		std::uint32_t cullMode = 0;
		bool room = false;  // a room's program (1 + index); unbound space's (program 0) drops skipViewFrustum
	};

	/** @brief What the lowering of an entry did (Encode's report): a stream it rejected is encoded as a single ACCEPT. */
	struct EncodeError
	{
		std::uint32_t program = 0;
		const char* what = nullptr;
	};

	/**
	 * @brief PortalPrograms.h's words: the view planes, the header, a directory entry per program (a_entries; none: a header of no
	 * programs), the compact records, then the sets. a_onError (optional) is told of each stream the lowering rejected.
	 */
	template <class OnError>
	void Encode(const PlaneSet& a_view, std::span<const EncodeEntry> a_entries, std::vector<std::uint32_t>& a_words, OnError&& a_onError)
	{
		a_words.clear();
		const auto* view = reinterpret_cast<const std::uint32_t*>(&a_view);
		a_words.insert(a_words.end(), view, view + kPortalSetBytes / 4);
		const std::size_t header = a_words.size();
		const auto count = static_cast<std::uint32_t>(a_entries.size());
		a_words.insert(a_words.end(), { count, 0u, 0u, 0u });
		const std::size_t directory = a_words.size();
		a_words.resize(directory + std::size_t(count) * (kPortalDirectoryBytes / 4), 0u);
		std::uint32_t records = 0, sets = 0;
		std::vector<PortalBytecode::Compiled> compiled(count);
		for (std::uint32_t p = 0; p < count; ++p) {
			const auto& source = a_entries[p];
			const Program* program = source.program;
			auto* entry = a_words.data() + directory + std::size_t(p) * (kPortalDirectoryBytes / 4);
			entry[6] = source.cullMode;
			if (!program)
				continue;
			entry[0] = kPortalReached | (source.room && program->skipView ? kPortalSkipView : 0u);
			const std::uint32_t ops = program->freeOp && program->finalized ? std::min<std::uint32_t>(program->freeOp + 3, static_cast<std::uint32_t>(program->ops.size())) : 0u;
			const std::uint32_t planes = ops ? std::min<std::uint32_t>(program->freePlane, static_cast<std::uint32_t>(program->sets.size())) : 0u;
			// A stream the lowering rejects must not throw: it accepts the bound, as the previous interpreter did on a malformed
			// stream (the view test still applies).
			if (ops) {
				try {
					compiled[p] = PortalBytecode::Lower<Op>(std::span(program->ops).first(ops), program->firstOp, planes);
				} catch (const std::exception& e) {
					compiled[p] = PortalBytecode::AcceptAll();
					a_onError(EncodeError{ p, e.what() });
				}
			}
			entry[1] = static_cast<std::uint32_t>(compiled[p].instructions.size());
			entry[2] = planes;
			entry[3] = 0;
			entry[4] = records;
			entry[5] = sets;
			entry[7] = compiled[p].maxSteps;
			records += entry[1];
			sets += planes;
		}
		a_words[header + 1] = records;
		a_words[header + 2] = sets;
		std::vector<std::uint32_t> setWords;
		for (std::uint32_t p = 0; p < count; ++p) {
			const std::uint32_t ops = a_words[directory + std::size_t(p) * (kPortalDirectoryBytes / 4) + 1];
			const std::uint32_t planes = a_words[directory + std::size_t(p) * (kPortalDirectoryBytes / 4) + 2];
			if (!ops)
				continue;
			const Program& program = *a_entries[p].program;
			for (const auto& op : compiled[p].instructions)
				a_words.insert(a_words.end(), { op.kind, op.set, op.yes, op.no });
			for (std::uint32_t s = 0; s < planes; ++s) {
				const auto* words = reinterpret_cast<const std::uint32_t*>(&program.sets[s]);
				setWords.insert(setWords.end(), words, words + kPortalSetBytes / 4);
			}
		}
		a_words.insert(a_words.end(), setWords.begin(), setWords.end());
	}

	// ---- The CPU evaluation (PortalPrograms.hlsli's PortalTestAt) ----

	/** @brief What decided a Visible verdict (PortalViews::Visible names it). */
	enum class Reason : std::uint8_t
	{
		kNoProgram,
		kZeroRadius,
		kNotReached,  // detail: the room (program - 1)
		kCullMode2,
		kAlways,
		kPreprocessed,
		kViewPlane,  // detail: the plane
		kView,
		kAccepted,  // detail: the record
		kRejected,  // detail: the record
	};
	struct Verdict
	{
		int visible = -1;  // 1 in view, 0 culled, -1 no program
		Reason reason = Reason::kNoProgram;
		std::uint32_t detail = 0;
	};

	/**
	 * @brief PortalTestAt on the CPU over a_words (the encoding at word offset 0): a_nodeFlags' always-draw bit 11, preprocessed 12,
	 * preprocess-hidden 20. Throws on an encoding the producer's invariants exclude (the GPU rejects the bound there).
	 */
	inline Verdict Visible(std::span<const std::uint32_t> a_words, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags)
	{
		constexpr std::size_t kHeader = kPortalSetBytes / 4, kDirectory = kPortalHeaderBytes / 4, kEntry = kPortalDirectoryBytes / 4, kOp = kPortalOpBytes / 4,
							  kSet = kPortalSetBytes / 4;
		const auto word = [&](std::size_t a_index) { return a_index < a_words.size() ? a_words[a_index] : 0u; };
		const auto plane = [&](std::size_t a_index) {
			std::array<float, 4> p;
			for (std::size_t c = 0; c < 4; ++c)
				p[c] = std::bit_cast<float>(word(a_index + c));
			return p;
		};
		const std::uint32_t count = word(kHeader);
		if (a_program == kPortalNoProgram || a_program >= count)
			return { -1, Reason::kNoProgram };
		const std::size_t entry = kDirectory + std::size_t(a_program) * kEntry;
		const std::uint32_t flags = word(entry), ops = word(entry + 1), sets = word(entry + 2), firstOp = word(entry + 3);
		const std::uint32_t firstRecord = word(entry + 4), firstSet = word(entry + 5), cullMode = word(entry + 6);
		const bool alwaysDraw = (a_nodeFlags >> 11) & 1, preprocessed = (a_nodeFlags >> 12) & 1, hidden = (a_nodeFlags >> 20) & 1;
		if (a_radius == 0.0f && !alwaysDraw)
			return { 0, Reason::kZeroRadius };
		if (a_program != 0 && !(flags & kPortalReached))
			return { 0, Reason::kNotReached, a_program - 1 };
		if (cullMode == 2)
			return { 0, Reason::kCullMode2 };
		if (cullMode == 1 || alwaysDraw)
			return { 1, Reason::kAlways };
		if (preprocessed && cullMode != 4)
			return { hidden ? 0 : 1, Reason::kPreprocessed };
		const auto distance = [&](const std::array<float, 4>& p) { return ((p[1] * a_centre[1] + p[0] * a_centre[0]) + a_centre[2] * p[2]) - p[3]; };
		Verdict viewVerdict{ 1, Reason::kView };
		const std::uint32_t viewMask = word(24);
		for (std::uint32_t p = 0; p < 6; ++p)
			if (((viewMask >> p) & 1) && distance(plane(std::size_t(p) * 4)) <= -a_radius) {
				viewVerdict = { 0, Reason::kViewPlane, p };
				break;
			}
		if (cullMode == 3 || ops == 0)
			return viewVerdict;
		if (!(flags & kPortalSkipView) && !viewVerdict.visible)
			return viewVerdict;
		const std::size_t records = kDirectory + std::size_t(count) * kEntry + std::size_t(firstRecord) * kOp;
		const std::size_t setsBase = kDirectory + std::size_t(count) * kEntry + std::size_t(word(kHeader + 1)) * kOp + std::size_t(firstSet) * kSet;
		std::uint32_t op = firstOp;
		for (std::uint32_t step = 0; step < word(entry + 7); ++step) {
			if (op >= ops)
				throw std::logic_error("portal bytecode: invalid instruction");
			const std::uint32_t type = word(records + std::size_t(op) * kOp), yes = word(records + std::size_t(op) * kOp + 2), no = word(records + std::size_t(op) * kOp + 3);
			if (type == 2)
				return { 1, Reason::kAccepted, op };
			if (type == 3)
				return { 0, Reason::kRejected, op };
			bool result = false;
			if (type == 7 || type == 8) {
				const std::uint32_t set = word(records + std::size_t(op) * kOp + 1);
				if (set >= sets)
					throw std::logic_error("portal bytecode: invalid plane set");
				const std::size_t base = setsBase + std::size_t(set) * kSet;
				const std::uint32_t mask = word(base + 24);
				if (!mask) {
					result = type == 7;
				} else {
					std::uint32_t p = 0;
					for (; p < 6; ++p) {
						if (!((mask >> p) & 1))
							continue;
						const float d = distance(plane(base + std::size_t(p) * 4));
						if (d <= -a_radius)
							break;
						if (type == 8 && d < a_radius)
							break;
					}
					result = type == 7 ? p == 6 : p != 6;
				}
			}
			op = result ? yes : no;
		}
		throw std::logic_error("portal bytecode: validated path did not terminate");
	}
}
