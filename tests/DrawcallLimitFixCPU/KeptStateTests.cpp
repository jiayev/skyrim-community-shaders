#include "Features/DrawcallLimitFix/Common/KeptState.h"
#include "Features/DrawcallLimitFix/Common/Retirement.h"
#include "Features/DrawcallLimitFix/Common/SlotTable.h"
#include "Features/DrawcallLimitFix/Scene/ActorValueIndex.h"
#include <cassert>
#include <map>
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
    // A ring of buffers (step 6e E): each brought up to date from its own held version, the journal trimmed to the oldest; every
    // holder's mirror equals the array after its replay, whatever the order and gaps of the replays.
    void TestRingHolders()
    {
        constexpr std::size_t kRing = 4;
        DCLF::KeptArray<std::uint32_t> values;
        DCLF::KeptHolders<kRing> holders;
        std::array<std::vector<std::uint32_t>, kRing> mirrors;
        std::mt19937 random(911);
        std::vector<DCLF::KeptView<std::uint32_t>> outstanding;  // views a producer still replays
        for (unsigned frame = 0; frame < 4000; ++frame) {
            values.BeginBuild(holders.Oldest());
            auto& elements = values.Mutable();
            if (frame % 97 == 0) {
                elements.resize(64 + random() % 900);
                values.Resync();
            } else if (frame % 151 == 0) {
                values.Clear();
                values.Mutable().resize(32 + random() % 300);
                values.Resync();
            } else {
                const std::size_t grow = random() % 3 == 0 ? random() % 20 : 0;
                if (grow) {
                    const std::size_t first = elements.size();
                    elements.resize(first + grow);
                    values.MarkRange(first, grow);
                }
                for (unsigned n = random() % 40; n > 0 && !values.Get().empty(); --n)
                    values.Set(random() % values.Get().size(), static_cast<std::uint32_t>(random()));
            }
            auto view = values.View();
            outstanding.push_back(view);
            if (outstanding.size() > 3)
                outstanding.erase(outstanding.begin());
            // The frame's holder replays the newest view; now and then a frame skips (its holder falls further behind).
            if (random() % 5 == 0)
                continue;
            const std::size_t r = frame % kRing;
            auto& mirror = mirrors[r];
            mirror.resize(view.Count());
            view.Emit(holders.Get(r), [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
                std::memcpy(reinterpret_cast<std::byte*>(mirror.data()) + a_offset, a_data, a_bytes);
            });
            holders.Set(r, view.Version());
            assert(mirror == *view.elements);
        }
        // Views a producer holds stay what they were while later builds write.
        for (const auto& view : outstanding)
            assert(view.elements && view.Count() == view.elements->size());
    }

    // Deferred reclamation by ownership: an item retired after publication p comes back only once every publication up to p is
    // gone, whatever order they are dropped in, exactly once.
    struct Retired
    {
        std::vector<std::pair<unsigned, unsigned>> items;  // (item, the newest publication made before it was retired)
        bool Empty() const { return items.empty(); }
    };
    void TestRetirementChain()
    {
        std::mt19937 random(1201);
        DCLF::RetirementChain<Retired> chain;
        std::map<unsigned, std::shared_ptr<const void>> held;  // publication -> its hold
        unsigned publications = 0, next = 0, returnedCount = 0;
        std::set<unsigned> outstanding;
        auto check = [&](const Retired& a_batch) {
            for (const auto& [item, after] : a_batch.items) {
                for (const auto& [p, hold] : held)
                    assert(p > after || after == 0);  // nothing up to 'after' still held (0: retired before any publication)
                assert(outstanding.erase(item) == 1);
                ++returnedCount;
            }
        };
        for (unsigned step = 0; step < 20000; ++step) {
            switch (random() % 4) {
            case 0:
                chain.Open().items.emplace_back(next, publications);
                outstanding.insert(next++);
                break;
            case 1:
                held.emplace(++publications, chain.Publish());
                break;
            default:
                if (!held.empty() && random() % 2) {
                    auto it = held.begin();
                    std::advance(it, random() % held.size());
                    held.erase(it);
                }
                break;
            }
            chain.Drain(check);
        }
        held.clear();
        chain.Publish();  // the open node closed, nothing holding the one before it
        chain.Drain(check);
        assert(outstanding.empty() && returnedCount == next);
    }

    void TestReferenceDrivenMaterialRetirement()
    {
        DCLF::SlotTable slots;
        const auto material = slots.Allocate().slot;
        auto generation = slots.Generation(material);
        slots.AddRef(material, generation);
        slots.AddRef(material, generation); // Two objects share a material.
        std::vector<std::uint32_t> freed;
        auto release = [&](auto slot) { freed.push_back(slot); };
        slots.Release(material, generation);
        assert(slots.DrainUnreferenced(release) == 0);
        slots.Release(material, generation);
        slots.AddRef(material, generation); // Reparent/equipment batch acquires a successor user.
        assert(slots.DrainUnreferenced(release) == 0);
        slots.Release(material, generation);
        assert(slots.DrainUnreferenced(release) == 1);
        assert(freed == std::vector<std::uint32_t>{material});
        assert(slots.DrainUnreferenced(release) == 0);

        const auto reused = slots.Allocate().slot;
        assert(reused == material && slots.Generation(reused) != generation);
        slots.AddRef(reused, slots.Generation(reused));
        slots.Release(reused, generation); // Old incarnation cannot retire the replacement.
        assert(slots.DrainUnreferenced(release) == 0);
        slots.ResetReferences();
        slots.AddRef(reused, slots.Generation(reused));
        assert(slots.DrainUnreferenced(release) == 0); // Recovery recount is one atomic batch.
        slots.ResetReferences();
        assert(slots.DrainUnreferenced(release) == 1);

        // A stale allocation event must not free a referenced reused slot.
        const auto abandoned = slots.Allocate().slot;
        slots.Free(abandoned);
        const auto successor = slots.Allocate().slot;
        slots.AddRef(successor, slots.Generation(successor));
        assert(slots.DrainUnreferenced(release) == 0);
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
    TestReferenceDrivenMaterialRetirement();
    TestActorValueMembership();
    TestRingHolders();
    TestRetirementChain();
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
