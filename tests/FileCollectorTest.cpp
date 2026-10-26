#include "FileCollector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Bytes = FileCollector::Bytes;
using Range = FileCollector::Range;

namespace {

Bytes MakeFile(size_t size, uint32_t seed) {
    Bytes data(size);
    std::mt19937 rng(seed);
    for (auto& b : data)
        b = static_cast<uint8_t>(rng());
    return data;
}

Bytes Slice(const Bytes& src, size_t pos, size_t len) {
    return Bytes(src.begin() + pos, src.begin() + pos + len);
}

Bytes Str(const char* s) {
    return Bytes(s, s + std::strlen(s));
}

}  // namespace

// ---------- Базовые сценарии ----------

TEST(FileCollector, ExampleFromTask) {
    FileCollector fc;
    fc.CollectFile(1, 10);
    fc.OnNewChunk(1, 0, Str("ABCD"));
    fc.OnNewChunk(1, 2, Str("CDEF"));
    EXPECT_FALSE(fc.TryTakeFile(1).has_value());
    fc.OnNewChunk(1, 6, Str("GHIJ"));

    auto file = fc.TryTakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(*file, Str("ABCDEFGHIJ"));
}

TEST(FileCollector, OutOfOrderChunks) {
    FileCollector fc;
    fc.CollectFile(1, 9);
    fc.OnNewChunk(1, 6, Str("GHI"));
    fc.OnNewChunk(1, 0, Str("ABC"));
    fc.OnNewChunk(1, 3, Str("DEF"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("ABCDEFGHI"));
}

TEST(FileCollector, DuplicatesAndContainedChunks) {
    FileCollector fc;
    fc.CollectFile(1, 6);
    fc.OnNewChunk(1, 0, Str("ABCD"));
    fc.OnNewChunk(1, 0, Str("ABCD"));  // дубликат
    fc.OnNewChunk(1, 1, Str("BC"));    // целиком внутри
    EXPECT_EQ(fc.GetProgress(1)->received, 4u);
    fc.OnNewChunk(1, 3, Str("DEF"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("ABCDEF"));
}

TEST(FileCollector, ChunkBridgesSeveralGaps) {
    FileCollector fc;
    fc.CollectFile(1, 10);
    fc.OnNewChunk(1, 1, Str("B"));
    fc.OnNewChunk(1, 4, Str("EF"));
    fc.OnNewChunk(1, 8, Str("I"));
    EXPECT_EQ(fc.GetReceivedRanges(1), (std::vector<Range>{{1, 2}, {4, 6}, {8, 9}}));

    fc.OnNewChunk(1, 0, Str("ABCDEFGHIJ"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("ABCDEFGHIJ"));
}

TEST(FileCollector, AdjacentRangesAreMerged) {
    FileCollector fc;
    fc.CollectFile(1, 10);
    fc.OnNewChunk(1, 0, Str("AB"));
    fc.OnNewChunk(1, 4, Str("EF"));
    fc.OnNewChunk(1, 2, Str("CD"));
    EXPECT_EQ(fc.GetReceivedRanges(1), (std::vector<Range>{{0, 6}}));
}

TEST(FileCollector, SingleChunkWholeFile) {
    FileCollector fc;
    const Bytes src = MakeFile(1 << 20, 7);
    fc.CollectFile(1, src.size());
    fc.OnNewChunk(1, 0, Bytes(src));
    EXPECT_EQ(fc.TryTakeFile(1), src);
}

TEST(FileCollector, ZeroSizeFileIsReadyImmediately) {
    FileCollector fc;
    fc.CollectFile(1, 0);
    auto file = fc.TryTakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_TRUE(file->empty());
}

TEST(FileCollector, ChunksBeforeCollectFileAreBuffered) {
    FileCollector fc;
    fc.OnNewChunk(1, 3, Str("DEF"));
    fc.OnNewChunk(1, 0, Str("ABCD"));
    EXPECT_FALSE(fc.TryTakeFile(1).has_value());
    fc.CollectFile(1, 6);
    EXPECT_EQ(fc.TryTakeFile(1), Str("ABCDEF"));
}

TEST(FileCollector, InvalidChunksAreIgnored) {
    FileCollector fc;
    fc.CollectFile(1, 4);
    fc.OnNewChunk(1, 2, Str("CDE"));                   // выходит за границу
    fc.OnNewChunk(1, SIZE_MAX, Str("X"));              // переполнение pos + len
    fc.OnNewChunk(1, 0, Bytes{});                      // пустой
    EXPECT_EQ(fc.GetProgress(1)->received, 0u);
    fc.OnNewChunk(1, 0, Str("ABCD"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("ABCD"));
}

TEST(FileCollector, UnknownFile) {
    FileCollector fc;
    uint8_t buf[1];
    EXPECT_FALSE(fc.TryTakeFile(42).has_value());
    EXPECT_FALSE(fc.GetProgress(42).has_value());
    EXPECT_FALSE(fc.ReadRange(42, 0, buf));
    EXPECT_TRUE(fc.GetReceivedRanges(42).empty());
}

TEST(FileCollector, RepeatedCollectFileIsIgnored) {
    FileCollector fc;
    fc.CollectFile(1, 4);
    fc.OnNewChunk(1, 0, Str("AB"));
    fc.CollectFile(1, 100);  // не сбрасывает прогресс
    fc.OnNewChunk(1, 2, Str("CD"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("ABCD"));
}

TEST(FileCollector, MultipleFilesAreIndependent) {
    FileCollector fc;
    fc.CollectFile(1, 3);
    fc.CollectFile(2, 3);
    fc.OnNewChunk(1, 0, Str("AAA"));
    fc.OnNewChunk(2, 0, Str("BBB"));
    EXPECT_EQ(fc.TryTakeFile(2), Str("BBB"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("AAA"));
}

// ---------- Интерфейс получения ----------

TEST(FileCollector, TakeOnlyOnce) {
    FileCollector fc;
    fc.CollectFile(1, 2);
    fc.OnNewChunk(1, 0, Str("AB"));
    EXPECT_TRUE(fc.TryTakeFile(1).has_value());
    EXPECT_FALSE(fc.TryTakeFile(1).has_value());
    EXPECT_FALSE(fc.GetProgress(1).has_value());
}

TEST(FileCollector, LateChunksAfterTakeAreIgnored) {
    FileCollector fc;
    fc.CollectFile(1, 2);
    fc.OnNewChunk(1, 0, Str("AB"));
    ASSERT_TRUE(fc.TryTakeFile(1).has_value());
    fc.OnNewChunk(1, 0, Str("AB"));
    EXPECT_FALSE(fc.TryTakeFile(1).has_value());
}

TEST(FileCollector, IdCanBeReusedAfterTake) {
    FileCollector fc;
    fc.CollectFile(1, 2);
    fc.OnNewChunk(1, 0, Str("AB"));
    ASSERT_TRUE(fc.TryTakeFile(1).has_value());

    fc.CollectFile(1, 3);
    fc.OnNewChunk(1, 0, Str("XYZ"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("XYZ"));
}

TEST(FileCollector, TakeDoesNotCopyBuffer) {
    FileCollector fc;
    fc.CollectFile(1, 4);
    fc.OnNewChunk(1, 0, Str("ABCD"));
    // Буфер, выделенный в CollectFile, отдаётся вызывающему как есть.
    auto file = fc.TryTakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->capacity(), 4u);
}

TEST(FileCollector, PartialReadRange) {
    FileCollector fc;
    fc.CollectFile(1, 10);
    fc.OnNewChunk(1, 2, Str("CDEF"));

    Bytes out(3);
    EXPECT_TRUE(fc.ReadRange(1, 3, out));
    EXPECT_EQ(out, Str("DEF"));

    EXPECT_FALSE(fc.ReadRange(1, 1, out));   // байт 1 ещё не получен
    EXPECT_FALSE(fc.ReadRange(1, 9, out));   // за границей файла

    fc.OnNewChunk(1, 0, Str("ABCDEFGHIJ"));
    Bytes all(10);
    EXPECT_TRUE(fc.ReadRange(1, 0, all));    // готовый файл читается без изъятия
    EXPECT_EQ(all, Str("ABCDEFGHIJ"));
    EXPECT_EQ(fc.TryTakeFile(1), Str("ABCDEFGHIJ"));
}

TEST(FileCollector, ProgressReporting) {
    FileCollector fc;
    fc.CollectFile(1, 10);
    fc.OnNewChunk(1, 0, Str("ABCD"));
    fc.OnNewChunk(1, 2, Str("CDEF"));
    auto p = fc.GetProgress(1);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->received, 6u);
    EXPECT_EQ(p->total, 10u);
    EXPECT_FALSE(p->complete);
}

TEST(FileCollector, WaitTimesOut) {
    FileCollector fc;
    fc.CollectFile(1, 10);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(fc.WaitAndTakeFile(1, 50ms).has_value());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 50ms);
}

// ---------- Многопоточные тесты ----------

TEST(FileCollectorMT, WaiterWakesUpWhenFileCompletes) {
    FileCollector fc;
    std::optional<Bytes> result;
    std::thread waiter([&] { result = fc.WaitAndTakeFile(1, 5s); });

    std::this_thread::sleep_for(20ms);
    fc.CollectFile(1, 4);
    fc.OnNewChunk(1, 2, Str("CD"));
    fc.OnNewChunk(1, 0, Str("AB"));
    waiter.join();

    EXPECT_EQ(result, Str("ABCD"));
}

TEST(FileCollectorMT, OnlyOneWaiterGetsTheFile) {
    FileCollector fc;
    fc.CollectFile(1, 1024);
    std::atomic<int> winners{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 8; ++i)
        waiters.emplace_back([&] {
            if (fc.WaitAndTakeFile(1, 5s))
                ++winners;
        });

    fc.OnNewChunk(1, 0, MakeFile(1024, 1));
    for (auto& t : waiters)
        t.join();
    EXPECT_EQ(winners.load(), 1);
}

// Много потоков пишут один файл случайными пересекающимися чанками.
TEST(FileCollectorMT, OneFileManyThreadsOverlappingChunks) {
    constexpr size_t kSize = 4 << 20;
    constexpr int kThreads = 8;
    const Bytes src = MakeFile(kSize, 123);

    FileCollector fc;
    fc.CollectFile(1, kSize);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            std::mt19937_64 rng(t);
            std::uniform_int_distribution<size_t> lenDist(1, 64 * 1024);
            // Каждый поток покрывает весь файл своим случайным разбиением,
            // отправляя чанки в перемешанном порядке и с нахлёстом.
            std::vector<std::pair<size_t, size_t>> parts;
            for (size_t pos = 0; pos < kSize;) {
                const size_t len = std::min(lenDist(rng), kSize - pos);
                const size_t overlap = std::min<size_t>(pos, rng() % 128);
                parts.emplace_back(pos - overlap, len + overlap);
                pos += len;
            }
            std::shuffle(parts.begin(), parts.end(), rng);
            for (auto [pos, len] : parts)
                fc.OnNewChunk(1, pos, Slice(src, pos, len));
        });

    std::thread reader([&] {
        Bytes buf(256);
        for (int i = 0; i < 2000; ++i) {
            const size_t pos = (static_cast<size_t>(i) * 7919) % (kSize - buf.size());
            if (fc.ReadRange(1, pos, buf)) {
                ASSERT_TRUE(std::equal(buf.begin(), buf.end(), src.begin() + pos));
            }
            (void)fc.GetReceivedRanges(1);
            (void)fc.GetProgress(1);
        }
    });

    for (auto& t : threads)
        t.join();
    reader.join();

    auto file = fc.TryTakeFile(1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(*file, src);
}

// Много файлов параллельно, часть чанков приходит до CollectFile.
TEST(FileCollectorMT, ManyFilesConcurrently) {
    constexpr uint32_t kFiles = 64;
    constexpr size_t kSize = 64 * 1024;
    constexpr size_t kChunk = 4096;
    constexpr int kThreads = 8;

    std::vector<Bytes> sources;
    for (uint32_t id = 0; id < kFiles; ++id)
        sources.push_back(MakeFile(kSize + id, id));

    FileCollector fc;
    std::atomic<uint32_t> nextFile{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 rng(t);
            for (uint32_t id; (id = nextFile.fetch_add(1)) < kFiles;) {
                const Bytes& src = sources[id];
                std::vector<size_t> positions;
                for (size_t pos = 0; pos < src.size(); pos += kChunk)
                    positions.push_back(pos);
                std::shuffle(positions.begin(), positions.end(), rng);

                const size_t registerAt = rng() % positions.size();
                for (size_t i = 0; i < positions.size(); ++i) {
                    if (i == registerAt)
                        fc.CollectFile(id, src.size());
                    const size_t pos = positions[i];
                    fc.OnNewChunk(id, pos, Slice(src, pos, std::min(kChunk, src.size() - pos)));
                }
            }
        });

    std::vector<std::optional<Bytes>> results(kFiles);
    std::vector<std::thread> consumers;
    for (uint32_t id = 0; id < kFiles; ++id)
        consumers.emplace_back([&, id] { results[id] = fc.WaitAndTakeFile(id, 30s); });

    for (auto& t : threads)
        t.join();
    for (auto& t : consumers)
        t.join();

    for (uint32_t id = 0; id < kFiles; ++id) {
        ASSERT_TRUE(results[id].has_value()) << "file " << id;
        EXPECT_EQ(*results[id], sources[id]) << "file " << id;
    }
}
