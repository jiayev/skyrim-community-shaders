#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Runtime/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SlotTable.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/OwnerValues.h"
#include <cassert>
#include <map>
#include <random>
#include <set>
#include <thread>

namespace
{
    void TestActorValueMembership()
    {
        DCLF::Integrations::AdvancedSkin::OwnerValues index;
        using Value = DCLF::Integrations::AdvancedSkin::OwnerValues::Value;
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
    // Rows journalled by part (KeptArray::SetParts, the material rows): holders at different content epochs catch up only the parts that moved,
    // and each holder's mirror equals the array after its replay, with untouched parts left as the holder had them.
    template <std::size_t kChunk>
    void TestPartHolders()
    {
        constexpr std::size_t kParts = 4, kHolders = 5;
        using Row = std::array<std::uint32_t, 8>;  // two words a part
        constexpr std::size_t kPartWords = Row{}.size() / kParts;
        DCLF::KeptArray<Row, kChunk> rows;
        std::array<std::uint64_t, kHolders> contentEpochs{};
        std::array<std::vector<Row>, kHolders> mirrors;
        std::mt19937 random(4242);
        std::uint64_t partsSent = 0, partsMarked = 0;
        for (unsigned frame = 0; frame < 3000; ++frame) {
            rows.BeginBuild(*std::min_element(contentEpochs.begin(), contentEpochs.end()));
            if (frame % 503 == 0) {
                rows.Clear();
                rows.Mutable().resize(16 + random() % 200);
                rows.Resync();
            }
            for (unsigned n = random() % 30; n > 0 && !rows.Get().empty(); --n) {
                const std::size_t r = random() % rows.Get().size();
                Row row = rows.Get()[r];
                row[(random() % kParts) * kPartWords + random() % kPartWords] = static_cast<std::uint32_t>(random());
                if (random() % 4 == 0)
                    row[random() % row.size()] = static_cast<std::uint32_t>(random());
                partsMarked += rows.SetParts(r, row, kParts);
            }
            const auto view = rows.View();
            auto& holder = random() % 3 ? contentEpochs[frame % kHolders] : contentEpochs[random() % kHolders];
            auto& mirror = mirrors[&holder - contentEpochs.data()];
            mirror.resize(view.Count());
            partsSent += view.changes.ForEachRun(holder, view.Count() * kParts, [&](std::uint64_t a_first, std::uint64_t a_count) {
                for (std::uint64_t part = a_first; part < a_first + a_count; ++part)
                    std::memcpy(mirror[part / kParts].data() + (part % kParts) * kPartWords, (*view.elements)[part / kParts].data() + (part % kParts) * kPartWords,
                        kPartWords * sizeof(std::uint32_t));
            });
            holder = view.ContentEpoch();
            assert(mirror == *view.elements);
        }
        assert(partsMarked > 0 && partsSent > 0);
    }
    // Pooled backings (D6): each brought up to date from its own content epoch, the journal trimmed to the oldest; every
    // holder's mirror equals the array after its replay, whatever the order and gaps of the replays.
    template <std::size_t kChunk>
    void TestPooledHolders()
    {
        constexpr std::size_t kHolders = 4;
        DCLF::KeptArray<std::uint32_t, kChunk> values;
        std::array<std::uint64_t, kHolders> contentEpochs{};  // each holder's (a pooled backing's contentEpoch)
        std::array<std::vector<std::uint32_t>, kHolders> mirrors;
        std::mt19937 random(911);
        std::vector<DCLF::KeptView<std::uint32_t>> outstanding;  // views a producer still replays
        for (unsigned frame = 0; frame < 4000; ++frame) {
            values.BeginBuild(*std::min_element(contentEpochs.begin(), contentEpochs.end()));
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
            const std::size_t r = frame % kHolders;
            auto& mirror = mirrors[r];
            mirror.resize(view.Count());
            view.Emit(contentEpochs[r], [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
                // A chunked view's spans stay within a chunk.
                const std::size_t first = a_offset / sizeof(std::uint32_t), last = first + a_bytes / sizeof(std::uint32_t) - 1;
                assert(a_bytes && first / view.elements->ChunkElements() == last / view.elements->ChunkElements());
                std::memcpy(reinterpret_cast<std::byte*>(mirror.data()) + a_offset, a_data, a_bytes);
            });
            contentEpochs[r] = view.ContentEpoch();
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
        // A freed slot retires first (a publication may still name it): reused once its owner recycles it.
        auto recycle = [&] { slots.TakeRetiring([&](auto a_slot, auto a_generation) { slots.Recycle(a_slot, a_generation); }); };
        assert(slots.Retiring() == 1);
        recycle();

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
        recycle();
        const auto successor = slots.Allocate().slot;
        assert(successor == abandoned);
        slots.AddRef(successor, slots.Generation(successor));
        assert(slots.DrainUnreferenced(release) == 0);
    }
}

namespace
{
    // Holders uploading at different content epochs with outstanding views: each view stays what it was, and the uploads replay the
    // array exactly.
    template <std::size_t kChunk>
    void TestUploadedEpochs()
    {
        DCLF::KeptArray<unsigned, kChunk> values;
        std::vector<DCLF::KeptView<unsigned>> held;
        std::vector<std::vector<unsigned>> expected;
        std::vector<unsigned> gpu(2048);
        std::uint64_t uploadedEpoch = 0;
        std::mt19937 random(1234);
        for (unsigned frame = 0; frame < 1000; ++frame) {
            values.BeginBuild(uploadedEpoch);
            if (!frame) { values.Mutable().resize(2048); values.Resync(); }
            for (unsigned n = 0; n < 173; ++n) {
                const auto index = random() % 2048;
                values.Set(index, random());
            }
            auto snapshot = values.View();
            snapshot.Emit(uploadedEpoch, [&](const void* data, std::size_t bytes, std::size_t offset) {
                std::memcpy(reinterpret_cast<std::byte*>(gpu.data()) + offset, data, bytes);
            });
            assert(gpu == values.Get());
            if (frame % 3 == 0) uploadedEpoch = snapshot.ContentEpoch();
            for (std::size_t i = 0; i < held.size(); ++i) assert(*held[i].elements == expected[i]);
            held.push_back(snapshot);
            expected.push_back(*snapshot.elements);
            // Exercise both recycled storage and exhaustion with outstanding readers.
            if (held.size() > (frame % 13 < 7 ? 1u : 6u)) { held.erase(held.begin()); expected.erase(expected.begin()); }
        }
        // An outstanding view is immutable even when the coordinator writes its next content epoch.
        held.clear();
        auto patch = values.View();
        const std::vector<unsigned> before = *patch.elements;
        values.BeginBuild(uploadedEpoch);
        values.Set(0, 0x12345678);
        assert(*patch.elements == before);
        assert(values.Get()[0] == 0x12345678);
    }

    constexpr std::size_t kSmallChunk = 64;
    using Small = DCLF::KeptArray<std::uint32_t, kSmallChunk>;

    void Fill(Small& a_values, std::size_t a_count)
    {
        auto& elements = a_values.Mutable();
        elements.resize(a_count);
        for (std::size_t i = 0; i < a_count; ++i)
            elements[i] = static_cast<std::uint32_t>(i);
        a_values.Resync();
    }

    // A write after a view copies only the chunk it touches (and the chunk table once a build); the view keeps the rest shared and
    // stays what it was.
    void TestChunkCopyOnWrite()
    {
        Small values;
        values.BeginBuild(0);
        Fill(values, kSmallChunk * 10);
        values.BeginBuild(~0ull);
        auto view = values.View();
        const std::vector<std::uint32_t> seen = *view.elements;
        const auto before = values.CopyStats();
        assert(values.Set(130, 7));  // chunk 2
        auto stats = values.CopyStats();
        assert(stats.tableCopies == before.tableCopies + 1 && stats.chunkCopies == before.chunkCopies + 1);
        assert(stats.bytesCopied == before.bytesCopied + kSmallChunk * sizeof(std::uint32_t));
        assert(values.Set(140, 8));  // the same chunk: already the array's own
        assert(values.CopyStats().chunkCopies == stats.chunkCopies && values.CopyStats().tableCopies == stats.tableCopies);
        assert(values.Set(5, 9));  // chunk 0
        assert(values.CopyStats().chunkCopies == stats.chunkCopies + 1);
        assert(!values.Set(300, 300));  // equal bytes: nothing written, nothing copied
        assert(values.CopyStats().chunkCopies == stats.chunkCopies + 1);
        // The view: unchanged, its untouched chunks the very same memory as the array's, the written ones not.
        assert(*view.elements == seen);
        assert(&(*view.elements)[300] == &values.Get()[300] && &(*view.elements)[639] == &values.Get()[639]);
        assert(&(*view.elements)[130] != &values.Get()[130] && &(*view.elements)[5] != &values.Get()[5]);
        assert(values.Get()[130] == 7 && values.Get()[140] == 8 && values.Get()[5] == 9 && (*view.elements)[130] == 130);
        // No view: writes in place.
        view.Reset();
        values.BeginBuild(~0ull);
        const auto quiet = values.CopyStats();
        values.Set(600, 1);
        assert(values.CopyStats().chunkCopies == quiet.chunkCopies && values.CopyStats().tableCopies == quiet.tableCopies);
    }

    // Views stay what they were through every kind of write: element writes, growth, shrinking, push/pop across chunk ends, Clear.
    void TestViewsImmutable()
    {
        Small values;
        std::vector<std::pair<DCLF::KeptView<std::uint32_t>, std::vector<std::uint32_t>>> held;
        std::mt19937 random(77);
        for (unsigned frame = 0; frame < 600; ++frame) {
            values.BeginBuild(~0ull);
            auto& elements = values.Mutable();
            for (unsigned op = random() % 12; op > 0; --op) {
                switch (random() % 6) {
                case 0:
                    elements.resize(random() % 700, static_cast<std::uint32_t>(random()));
                    break;
                case 1:
                    elements.push_back(static_cast<std::uint32_t>(random()));
                    break;
                case 2:
                    if (!elements.empty())
                        elements.pop_back();
                    break;
                case 3:
                    if (random() % 20 == 0)
                        values.Clear();
                    break;
                default:
                    if (!elements.empty())
                        values.Set(random() % elements.size(), static_cast<std::uint32_t>(random()));
                    break;
                }
            }
            for (const auto& [view, seen] : held)
                assert(*view.elements == seen);
            held.emplace_back(values.View(), static_cast<std::vector<std::uint32_t>>(values.Get()));
            if (held.size() > 1 + random() % 6)
                held.erase(held.begin() + static_cast<std::ptrdiff_t>(random() % held.size()));
        }
    }

    // Resizing across chunk ends behaves as std::vector's: kept elements kept, new ones the fill value (never an earlier shrink's slack).
    void TestResizeAcrossChunks()
    {
        Small values;
        std::vector<std::uint32_t> reference;
        std::mt19937 random(5150);
        DCLF::KeptView<std::uint32_t> held;
        for (unsigned step = 0; step < 3000; ++step) {
            values.BeginBuild(~0ull);
            const std::size_t count = random() % 4 == 0 ? random() % 8 : random() % (kSmallChunk * 6 + 3);
            const auto fill = static_cast<std::uint32_t>(random());
            if (random() % 2) {
                values.Mutable().resize(count, fill);
                reference.resize(count, fill);
            } else {
                values.Mutable().resize(count);
                reference.resize(count);
            }
            for (unsigned n = random() % 5; n > 0 && count; --n) {
                const std::size_t i = random() % count;
                values.Mutable()[i] = reference[i] = static_cast<std::uint32_t>(random());
            }
            assert(values.Size() == reference.size() && values.Get() == reference);
            assert(std::equal(values.Get().begin(), values.Get().end(), reference.begin()));
            if (random() % 3 == 0)
                held = values.View();  // a view outstanding across the next resize
        }
    }

    // ForEachRun's runs are exactly what was marked, and the chunked spans (KeptView::ForEachSpan/Emit) cover exactly those runs, each
    // within one chunk.
    void TestRunsMatchMarks()
    {
        Small values;
        std::mt19937 random(31337);
        values.BeginBuild(0);
        Fill(values, 1000);
        std::uint64_t holder = values.View().ContentEpoch();
        for (unsigned frame = 0; frame < 500; ++frame) {
            values.BeginBuild(holder);
            std::set<std::size_t> marked;
            for (unsigned n = random() % 60; n > 0; --n) {
                const std::size_t i = random() % values.Size();
                if (random() % 4 == 0) {
                    const std::size_t count = std::min<std::size_t>(1 + random() % 150, values.Size() - i);
                    values.MarkRange(i, count);
                    for (std::size_t k = i; k < i + count; ++k)
                        marked.insert(k);
                } else if (values.Set(i, values.Get()[i] + 1)) {
                    marked.insert(i);
                }
            }
            const auto view = values.View();
            std::set<std::size_t> runs, spans;
            view.changes.ForEachRun(holder, view.Count(), [&](std::uint64_t a_first, std::uint64_t a_count) {
                for (auto k = a_first; k < a_first + a_count; ++k)
                    runs.insert(static_cast<std::size_t>(k));
            });
            const std::size_t sent = view.ForEachSpan(holder, [&](const std::uint32_t* a_data, std::size_t a_count, std::size_t a_first) {
                assert(a_count && a_first / kSmallChunk == (a_first + a_count - 1) / kSmallChunk);
                for (std::size_t k = 0; k < a_count; ++k) {
                    assert(a_data[k] == values.Get()[a_first + k]);
                    spans.insert(a_first + k);
                }
            });
            assert(runs == marked && spans == marked && sent == marked.size());
            holder = view.ContentEpoch();
        }
    }

    // Chunks a view held come back once every view has released them: the next write after a view reuses one instead of allocating.
    void TestSpareReuse()
    {
        Small values;
        values.BeginBuild(0);
        Fill(values, kSmallChunk * 4);
        values.BeginBuild(~0ull);
        const auto* original = &values.Get()[0];
        auto view = values.View();
        values.Set(0, 100);  // chunk 0 copied: the original retired, the view still holding it
        const auto first = values.CopyStats();
        assert(&values.Get()[0] != original && &(*view.elements)[0] == original);
        values.BeginBuild(~0ull);  // the view still holds it: nothing to reuse
        view = values.View();
        values.Set(1, 101);
        assert(values.CopyStats().chunkReuses == first.chunkReuses && values.CopyStats().chunkAllocations == first.chunkAllocations + 1);
        // Every view gone: the next build's write after a view takes a released chunk (the original's memory) instead of allocating.
        view.Reset();
        values.BeginBuild(~0ull);
        view = values.View();
        const auto second = values.CopyStats();
        values.Set(2, 102);
        assert(values.CopyStats().chunkReuses == second.chunkReuses + 1 && values.CopyStats().chunkAllocations == second.chunkAllocations);
        assert(values.Get()[0] == 100 && values.Get()[1] == 101 && values.Get()[2] == 102 && (*view.elements)[2] == 2);
        // Shrinking and growing again reuses the dropped chunks (no view holds them).
        view.Reset();
        values.BeginBuild(~0ull);
        const auto third = values.CopyStats();
        values.Mutable().resize(kSmallChunk);
        values.Mutable().resize(kSmallChunk * 4, 5);
        assert(values.CopyStats().chunkAllocations == third.chunkAllocations && values.CopyStats().chunkReuses == third.chunkReuses + 3);
        assert(values.Get()[kSmallChunk] == 5 && values.Get()[kSmallChunk * 4 - 1] == 5 && values.Get()[0] == 100);
        // Storage reuse off (the comparison against fresh storage): every copy allocates.
        DCLF::reuseKeptStorage = false;
        values.BeginBuild(~0ull);
        view = values.View();
        const auto fourth = values.CopyStats();
        values.Set(3, 103);
        assert(values.CopyStats().chunkAllocations == fourth.chunkAllocations + 1 && values.CopyStats().chunkReuses == fourth.chunkReuses);
        DCLF::reuseKeptStorage = true;
    }

    // Parallel writers into distinct elements after PrepareWrites (W4's Pairs.Resolve): the result equals a serial write, the marks
    // applied afterwards on one thread are the serial marks, and an outstanding view is untouched.
    void TestConcurrentDisjointWrites()
    {
        using Row = std::array<std::uint32_t, 8>;
        constexpr std::size_t kParts = 4, kRows = 1000, kThreads = 4;
        DCLF::KeptArray<Row, 16> parallel, serial;
        for (auto* rows : { &parallel, &serial }) {
            rows->BeginBuild(0);
            rows->Mutable().resize(kRows);
            rows->Resync();
        }
        std::mt19937 random(2024);
        for (unsigned frame = 0; frame < 50; ++frame) {
            std::vector<Row> next(kRows);
            for (std::size_t r = 0; r < kRows; ++r) {
                next[r] = parallel.Get()[r];
                if (random() % 3 == 0)
                    next[r][random() % next[r].size()] = static_cast<std::uint32_t>(random());
            }
            const auto parallelHolder = parallel.ContentEpoch(), serialHolder = serial.ContentEpoch();
            parallel.BeginBuild(parallelHolder);
            serial.BeginBuild(serialHolder);
            const auto view = parallel.View();
            const std::vector<Row> seen = *view.elements;
            const std::size_t first = random() % 100;  // rows below it are not written this frame
            parallel.PrepareWrites(first, kRows - first);
            std::array<std::vector<std::pair<std::size_t, std::uint64_t>>, kThreads> masks;
            std::vector<std::thread> threads;
            for (std::size_t t = 0; t < kThreads; ++t)
                threads.emplace_back([&, t] {
                    for (std::size_t r = first + t; r < kRows; r += kThreads)  // interleaved: threads share every chunk
                        if (const auto mask = parallel.WriteParts(r, next[r], kParts))
                            masks[t].emplace_back(r, mask);
                });
            for (auto& thread : threads)
                thread.join();
            std::vector<std::pair<std::size_t, std::uint64_t>> merged;
            for (const auto& mine : masks)
                merged.insert(merged.end(), mine.begin(), mine.end());
            std::sort(merged.begin(), merged.end());
            for (const auto& [r, mask] : merged)
                parallel.MarkParts(r, mask, kParts);
            for (std::size_t r = first; r < kRows; ++r)
                serial.SetParts(r, next[r], kParts);
            assert(parallel.Get() == static_cast<std::vector<Row>>(serial.Get()));
            assert(*view.elements == seen);
            std::vector<std::pair<std::uint64_t, std::uint64_t>> parallelRuns, serialRuns;
            parallel.View().changes.ForEachRun(parallelHolder, kRows * kParts, [&](auto a_first, auto a_count) { parallelRuns.emplace_back(a_first, a_count); });
            serial.View().changes.ForEachRun(serialHolder, kRows * kParts, [&](auto a_first, auto a_count) { serialRuns.emplace_back(a_first, a_count); });
            assert(parallelRuns == serialRuns);
        }
    }

    // Views of elements of one's own (not a kept array's) adopt the vector as one chunk; a whole array's views are one chunk too.
    void TestAdoptedAndWhole()
    {
        auto flat = std::make_shared<std::vector<std::uint32_t>>(std::vector<std::uint32_t>{ 1, 2, 3, 4, 5 });
        DCLF::KeptView<std::uint32_t> own;
        own.elements = flat;
        assert(own.Count() == 5 && &(*own.elements)[0] == flat->data() && *own.elements == *flat);
        std::size_t emits = 0;
        own.Emit(0, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
            ++emits;
            assert(a_data == flat->data() && a_bytes == 5 * sizeof(std::uint32_t) && a_offset == 0);
        });
        assert(emits == 1);
        DCLF::KeptView<std::uint32_t> constOwn{ std::shared_ptr<const std::vector<std::uint32_t>>(flat), {} };
        assert(constOwn.Count() == 5);
        DCLF::KeptWholeArray<std::uint32_t> whole;
        whole.BeginBuild(0);
        std::vector<std::uint32_t>& elements = whole.Mutable();
        elements.assign(10000, 3);
        whole.Resync();
        const auto view = whole.View();
        assert(&(*view.elements)[0] == whole.Get().data());
        whole.BeginBuild(~0ull);
        whole.Set(9999, 4);  // the first write of a build with a view out: the whole array copied
        assert(&(*view.elements)[0] != whole.Get().data() && (*view.elements)[9999] == 3 && whole.Get()[9999] == 4);
        assert(whole.CopyStats().bytesCopied == 10000 * sizeof(std::uint32_t));
    }
}

int main()
{
    TestUploadedEpochs<DCLF::kKeptChunkBySize>();
    TestUploadedEpochs<kSmallChunk>();
    TestUploadedEpochs<DCLF::kKeptWhole>();
    TestChunkCopyOnWrite();
    TestViewsImmutable();
    TestResizeAcrossChunks();
    TestRunsMatchMarks();
    TestSpareReuse();
    TestConcurrentDisjointWrites();
    TestAdoptedAndWhole();
    TestReferenceDrivenMaterialRetirement();
    TestActorValueMembership();
    TestPooledHolders<DCLF::kKeptChunkBySize>();
    TestPooledHolders<16>();
    TestPooledHolders<DCLF::kKeptWhole>();
    TestPartHolders<DCLF::kKeptChunkBySize>();
    TestPartHolders<4>();
    TestPartHolders<DCLF::kKeptWhole>();
    TestRetirementChain();
    std::mt19937 random(1234);
    for (unsigned trial = 0; trial < 1000; ++trial) {
        DCLF::ChangeJournal::Snapshot changes{10, 1, {}};
        const std::uint64_t count = 1 + random() % 8192;
        for (unsigned n = 0; n < 256; ++n)
            changes.entries.push_back({1 + random() % 10, random() % 9000, random() % 180});
        const auto holderEpoch = random() % 12;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reference, actual;
        DCLF::reuseKeptStorage = false;
        const auto referenceSent = changes.ForEachRun(holderEpoch, count, [&](auto first, auto size) { reference.emplace_back(first, size); });
        DCLF::reuseKeptStorage = true;
        const auto actualSent = changes.ForEachRun(holderEpoch, count, [&](auto first, auto size) { actual.emplace_back(first, size); });
        assert(actual == reference && actualSent == referenceSent);
    }
}
