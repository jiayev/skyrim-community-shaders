#include "Features/DrawcallLimitFix/KeptState.h"
#include "Features/DrawcallLimitFix/FrameRecordPatches.h"
#include <cassert>
#include <random>

namespace
{
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
