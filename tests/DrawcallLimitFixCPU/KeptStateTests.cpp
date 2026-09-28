#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Common/FrameRecordPatches.h"
#include "Features/DrawcallLimitFix/Common/SlotTable.h"
#include "Features/DrawcallLimitFix/Scene/ActorValueIndex.h"
#include <cassert>
#include <random>
#include <set>

namespace
{
    void TestActorValueMembership()
    {
        DCLF::ActorValueIndex index;
        using Value = DCLF::ActorValueIndex::Value;
        std::array<Value, 6> rows{};
        Value current{1, 2, 3, 4};
        unsigned sampled = 0;
        auto sample = [&](auto) { ++sampled; return current; };
        auto apply = [&](auto slot, const auto& value) { rows[slot] = value; };
        index.Set(0, 100, 1);
        index.Set(1, 100, 2);
        auto stats = index.Update(sample, apply);
        assert(sampled == 1 && stats.actors == 1 && stats.visited == 2 && rows[0] == rows[1]);
        stats = index.Update(sample, apply);
        assert(stats.visited == 0 && stats.changed == 0); // No per-mesh iteration for unchanged actors.
        index.Set(2, 100, 3); // Equipment join without output change.
        stats = index.Update(sample, apply);
        assert(stats.visited == 1 && rows[2] == current);
        rows[1] = {};
        index.Set(1, 100, 2, true); // Scene writer reset the row, membership stayed constant.
        assert(index.Update(sample, apply).visited == 1 && rows[1] == current);
        index.Set(0, 0, 0); // Representative detached; another member samples the actor.
        index.Set(3, 100, 4);
        current[0] = 0.5f; // Fade affects all surviving parts in one batch.
        stats = index.Update(sample, apply);
        assert(stats.actors == 1 && stats.members == 3 && stats.visited == 3);
        assert(rows[1] == current && rows[2] == current && rows[3] == current);
        index.Set(2, 200, 5); // Slot reuse / actor replacement must initialize the new incarnation.
        assert(!index.Contains(2, 100, 3) && index.Contains(2, 200, 5));
        stats = index.Update(sample, apply);
        assert(stats.actors == 2 && stats.visited == 1);
        index.Set(1, 0, 0);
        index.Set(3, 0, 0);
        assert(index.Update(sample, apply).actors == 1); // Last member retires the group immediately.
        auto snapshot = index;
        index.Clear();
        assert(index.Update(sample, apply).actors == 0);
        assert(snapshot.Update(sample, apply).actors == 1); // Parity copies have independent ownership.
        index.Set(5, 300, 6);
        current = {}; // Disabled skin / zero delta output is still published once.
        assert(index.Update(sample, apply).visited == 1 && rows[5] == current);
        assert(index.Update(sample, apply).visited == 0);

        // Compare incremental propagation with full derivation from the same
        // captured values under reordered membership/value changes.
        index.Clear();
        std::array<std::uint64_t, 6> groups{}, identities{};
        std::array<Value, 4> inputs{};
        std::mt19937 random(317);
        std::uint64_t incarnation = 1000;
        for (unsigned frame = 0; frame < 1000; ++frame) {
            for (unsigned event = 0; event < 4; ++event) {
                const auto slot = random() % groups.size();
                const auto group = random() % inputs.size();
                groups[slot] = group;
                identities[slot] = ++incarnation;
                index.Set(static_cast<std::uint32_t>(slot), group, identities[slot]);
            }
            if (frame % 3 == 0) inputs[random() % inputs.size()][random() % 4] = float(random() % 16);
            stats = index.Update([&](auto slot) { return inputs[groups[slot]]; }, apply);
            std::set<std::uint64_t> actors;
            unsigned members = 0;
            for (unsigned slot = 0; slot < groups.size(); ++slot) {
                if (!groups[slot]) continue;
                actors.insert(groups[slot]);
                ++members;
                assert(rows[slot] == inputs[groups[slot]]);
                assert(index.Contains(slot, groups[slot], identities[slot]));
            }
            assert(stats.actors == actors.size() && stats.members == members);
            assert(index.Update([&](auto slot) { return inputs[groups[slot]]; }, apply).visited == 0);
        }
    }
    void TestReferenceDrivenMaterialRetirement()
    {
        DCLF::SlotTable slots;
        const auto material = slots.Allocate(1).slot;
        auto generation = slots.Generation(material);
        slots.AddRef(material, generation);
        slots.AddRef(material, generation); // Two objects share a material.
        std::vector<std::uint32_t> freed;
        auto release = [&](auto slot) { freed.push_back(slot); };
        slots.Release(material, generation, 2);
        assert(slots.DrainUnreferenced(release) == 0);
        slots.Release(material, generation, 3);
        slots.AddRef(material, generation); // Reparent/equipment batch acquires a successor user.
        assert(slots.DrainUnreferenced(release) == 0);
        slots.Release(material, generation, 3);
        assert(slots.DrainUnreferenced(release) == 1);
        assert(freed == std::vector<std::uint32_t>{material});
        assert(slots.DrainUnreferenced(release) == 0);

        const auto reused = slots.Allocate(3).slot;
        assert(reused == material && slots.Generation(reused) != generation);
        slots.AddRef(reused, slots.Generation(reused));
        slots.Release(reused, generation, 3); // Old incarnation cannot retire the replacement.
        assert(slots.DrainUnreferenced(release) == 0);
        slots.ResetReferences(4);
        slots.AddRef(reused, slots.Generation(reused));
        assert(slots.DrainUnreferenced(release) == 0); // Recovery recount is one atomic batch.
        slots.ResetReferences(5);
        assert(slots.DrainUnreferenced(release) == 1);

        // A stale allocation event must not free a referenced reused slot.
        const auto abandoned = slots.Allocate(6).slot;
        slots.Free(abandoned);
        const auto successor = slots.Allocate(6).slot;
        slots.AddRef(successor, slots.Generation(successor));
        assert(slots.DrainUnreferenced(release) == 0);
    }

    struct Record
    {
        std::array<std::uint32_t, 128> textures{};
        std::uint32_t staticValue = 0;
        bool operator==(const Record&) const = default;
    };

    void TestImmutableFramePatches()
    {
        DCLF::KeptArray<Record> templates;
        templates.BeginBuild(0);
        templates.Mutable().resize(2);
        templates.Resync();
        auto first = templates.View();
        const auto original = *first.elements;
        const std::array<std::array<std::uint64_t, 2>, 2> masks{{ { 1ull << 17, 1ull << 4 }, {} }};
        std::array<std::uint32_t, 128> indices{};
        indices[17] = 71;
        indices[68] = 99;
        std::array<std::uint64_t, 2> changed{ 1ull << 17, 1ull << 4 };
        std::vector<std::pair<std::size_t, Record>> emitted;
        auto emit = [&](std::size_t slot, const Record& record) { emitted.emplace_back(slot, record); };
        assert(DCLF::EmitFrameRecordPatches(first, 0, std::span<const std::array<std::uint64_t, 2>>(masks), changed, indices, emit) == 1);
        assert(emitted[0].first == 0 && emitted[0].second.textures[17] == 71 && emitted[0].second.textures[68] == 99);
        assert(*first.elements == original);
        emitted.clear();
        changed = {};
        assert(DCLF::EmitFrameRecordPatches(first, first.Version(), std::span<const std::array<std::uint64_t, 2>>(masks), changed, indices, emit) == 0);

        // A base upload overwrites both dynamic registers with template zero. Even with no
        // descriptor change, the frame patch must restore both current values.
        templates.BeginBuild(first.Version());
        Record next = templates.Get()[0];
        next.staticValue = 42;
        templates.Set(0, next);
        auto second = templates.View();
        assert(DCLF::EmitFrameRecordPatches(second, first.Version(), std::span<const std::array<std::uint64_t, 2>>(masks), changed, indices, emit) == 1);
        assert(emitted[0].second.staticValue == 42 && emitted[0].second.textures[17] == 71 && emitted[0].second.textures[68] == 99);
        assert(*first.elements == original && second.elements->at(0).textures[17] == 0);

        // A changed descriptor requires a whole-record upload; all referenced registers
        // must be resolved, including the one whose index did not change.
        emitted.clear();
        indices[17] = 72;
        changed = { 1ull << 17, 0 };
        assert(DCLF::EmitFrameRecordPatches(second, second.Version(), std::span<const std::array<std::uint64_t, 2>>(masks), changed, indices, emit) == 1);
        assert(emitted[0].second.textures[17] == 72 && emitted[0].second.textures[68] == 99);
        assert(second.elements->at(0).textures[17] == 0);
    }
}

int main()
{
    DCLF::KeptArray<unsigned> values;
    std::vector<DCLF::KeptView<unsigned>> held;
    std::vector<std::vector<unsigned>> expected;
    std::vector<unsigned> gpu(2048);
    std::uint64_t uploaded = 0;
    std::mt19937 random(1234);
    for (unsigned frame = 0; frame < 1000; ++frame) {
        values.BeginBuild(uploaded);
        if (!frame) { values.Mutable().resize(2048); values.Resync(); }
        for (unsigned n = 0; n < 173; ++n) {
            const auto index = random() % 2048;
            values.Set(index, random());
        }
        auto snapshot = values.View();
        snapshot.Emit(uploaded, [&](const void* data, std::size_t bytes, std::size_t offset) {
            std::memcpy(reinterpret_cast<std::byte*>(gpu.data()) + offset, data, bytes);
        });
        assert(gpu == values.Get());
        if (frame % 3 == 0) uploaded = snapshot.Version();
        for (std::size_t i = 0; i < held.size(); ++i) assert(*held[i].elements == expected[i]);
        held.push_back(snapshot);
        expected.push_back(*snapshot.elements);
        // Exercise both recycled storage and exhaustion with outstanding readers.
        if (held.size() > (frame % 13 < 7 ? 1u : 6u)) { held.erase(held.begin()); expected.erase(expected.begin()); }
    }
    // An outstanding view is immutable even when the coordinator writes its next version.
    held.clear();
    auto patch = values.View();
    const auto before = *patch.elements;
    values.BeginBuild(uploaded);
    values.Set(0, 0x12345678);
    assert(*patch.elements == before);
    assert(values.Get()[0] == 0x12345678);
    TestImmutableFramePatches();
    TestReferenceDrivenMaterialRetirement();
    TestActorValueMembership();
    for (unsigned trial = 0; trial < 1000; ++trial) {
        DCLF::ChangeJournal::Snapshot changes{10, 1, {}};
        const std::uint64_t count = 1 + random() % 8192;
        for (unsigned n = 0; n < 256; ++n)
            changes.entries.push_back({1 + random() % 10, random() % 9000, random() % 180});
        const auto heldVersion = random() % 12;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reference, actual;
        DCLF::reuseKeptStorage = false;
        const auto referenceSent = changes.ForEachRun(heldVersion, count, [&](auto first, auto size) { reference.emplace_back(first, size); });
        DCLF::reuseKeptStorage = true;
        const auto actualSent = changes.ForEachRun(heldVersion, count, [&](auto first, auto size) { actual.emplace_back(first, size); });
        assert(actual == reference && actualSent == referenceSent);
    }
}
