#include "Features/DrawcallLimitFix/KeptState.h"
#include <cassert>
#include <random>

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
    // A commit's unjournalled patch must survive storage reuse too.
    held.clear();
    auto patch = values.View();
    (*patch.elements)[0] = 0x12345678;
    values.BeginBuild(uploaded);
    values.Set(1, 42);
    assert(values.Get()[0] == 0x12345678);
    assert((*patch.elements)[1] != 42);
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
