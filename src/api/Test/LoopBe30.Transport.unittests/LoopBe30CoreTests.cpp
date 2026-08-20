#include <Windows.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>

#include "loopbe30_transport_defs.h"
#include "FeedbackDetector.h"
#include "LoopBe30FixedQueue.h"

namespace
{
    void Expect(bool condition, char const* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    uint32_t Midi1(uint8_t status, uint8_t data1, uint8_t data2)
    {
        return (0x2u << 28) | (static_cast<uint32_t>(status) << 16) |
            (static_cast<uint32_t>(data1) << 8) | data2;
    }

    void TestCatalog()
    {
        Expect(LOOPBE30_MIN_PORT_COUNT == 0, "minimum count");
        Expect(LOOPBE30_DEFAULT_PORT_COUNT == 2, "default count");
        Expect(LOOPBE30_MAX_PORT_COUNT == 30, "maximum count");
        Expect(LOOPBE30_ENDPOINT_NAMES[0] == L"01. Internal MIDI", "port 1 name");
        Expect(LOOPBE30_ENDPOINT_NAMES[8] == L"09. Internal MIDI", "port 9 name");
        Expect(LOOPBE30_ENDPOINT_NAMES[9] == L"10. Internal MIDI", "port 10 name");
        Expect(LOOPBE30_ENDPOINT_NAMES[29] == L"30. Internal MIDI", "port 30 name");
        Expect(LOOPBE30_ENDPOINT_UNIQUE_IDS[0] == L"nerdsdeLoopBe30Port01", "port 1 id");
        Expect(LOOPBE30_ENDPOINT_UNIQUE_IDS[29] == L"nerdsdeLoopBe30Port30", "port 30 id");
        std::set<std::wstring_view> names;
        std::set<std::wstring_view> identifiers;
        for (size_t index = 0; index < LOOPBE30_MAX_PORT_COUNT; ++index)
        {
            Expect(names.insert(LOOPBE30_ENDPOINT_NAMES[index]).second, "duplicate name");
            Expect(identifiers.insert(LOOPBE30_ENDPOINT_UNIQUE_IDS[index]).second, "duplicate id");
            for (size_t other = index + 1; other < LOOPBE30_MAX_PORT_COUNT; ++other)
                Expect(!InlineIsEqualGUID(LOOPBE30_ENDPOINT_ASSOCIATION_IDS[index],
                    LOOPBE30_ENDPOINT_ASSOCIATION_IDS[other]), "duplicate association GUID");
        }
    }

    bool FeedPattern(size_t period, size_t count, int64_t spacing)
    {
        LoopBe30FeedbackAnalyzer analyzer;
        bool triggered{};
        for (size_t index = 0; index < count; ++index)
        {
            auto message = *LoopBe30NormalizeMidi1Ump(Midi1(
                0x90, static_cast<uint8_t>(index % period), 100));
            triggered = analyzer.Observe(static_cast<int64_t>(index) * spacing, message) || triggered;
        }
        return triggered;
    }

    void TestDetector()
    {
        Expect(LoopBe30NormalizeMidi1Ump(Midi1(0x90, 60, 100))->ByteCount == 3, "note length");
        Expect(LoopBe30NormalizeMidi1Ump(Midi1(0xc0, 10, 0))->ByteCount == 2, "program length");
        Expect(LoopBe30NormalizeMidi1Ump((0x1u << 28) | (0xf8u << 16))->ByteCount == 1, "clock length");
        Expect(!LoopBe30NormalizeMidi1Ump(0x30000000u), "SysEx7 ignored");
        Expect(!LoopBe30NormalizeMidi1Ump(0x40000000u), "MIDI2 ignored");
        Expect(!FeedPattern(1, 266, 1'100'000 / 265), "below 800 bytes");
        Expect(FeedPattern(1, 401, 2'500), "period 1");
        Expect(FeedPattern(8, 401, 2'500), "period 8");
        Expect(!FeedPattern(9, 401, 2'500), "period 9");

        LoopBe30FeedbackAnalyzer nonRepeating;
        bool triggered{};
        for (size_t index = 0; index < 401; ++index)
        {
            const auto status = static_cast<uint8_t>(0x80 + ((index / 128) % 7) * 0x10);
            auto message = *LoopBe30NormalizeMidi1Ump(Midi1(status,
                static_cast<uint8_t>(index % 128), static_cast<uint8_t>((index * 37) % 128)));
            triggered = nonRepeating.Observe(static_cast<int64_t>(index) * 2'500, message) || triggered;
        }
        Expect(!triggered, "high-rate non-repeating");

        LoopBe30FeedbackAnalyzer runawayNonRepeating;
        triggered = false;
        for (size_t index = 0; index < 3'100; ++index)
        {
            const auto status = static_cast<uint8_t>(0x80 + ((index / 128) % 7) * 0x10);
            auto message = *LoopBe30NormalizeMidi1Ump(Midi1(status,
                static_cast<uint8_t>(index % 128), static_cast<uint8_t>((index * 37) % 128)));
            triggered = runawayNonRepeating.Observe(
                static_cast<int64_t>(index) * 325, message) || triggered;
        }
        Expect(triggered, "extreme-rate mixed non-repeating runaway");

        LoopBe30FeedbackAnalyzer clock;
        auto clockMessage = *LoopBe30NormalizeMidi1Ump((0x1u << 28) | (0xf8u << 16));
        for (size_t index = 0; index < 150; ++index)
            Expect(!clock.Observe(static_cast<int64_t>(index) * 10'000, clockMessage), "normal clock");
        clock.Reset();
        Expect(clock.Diagnostics(2'000'000, true,
            LoopBe30DetectorState::WaitingForTraffic).BytesPerSecond == 0, "detector reset");
    }

    struct QueueItem { uint32_t Value{}; int64_t Timestamp{}; };

    void TestQueue()
    {
        LoopBe30FixedQueue<QueueItem, 4> queue;
        const std::array first{ QueueItem{ 1, 10 }, QueueItem{ 2, 20 } };
        Expect(queue.TryPushBatchOrClear(first), "initial atomic admission");
        const std::array overflow{ QueueItem{ 3, 30 }, QueueItem{ 4, 40 }, QueueItem{ 5, 50 } };
        Expect(!queue.TryPushBatchOrClear(overflow), "overflow rejection");
        Expect(queue.Empty(), "overflow clears affected queue");

        const std::array ordered{ QueueItem{ 7, 70 }, QueueItem{ 8, 80 }, QueueItem{ 9, 90 } };
        Expect(queue.TryPushBatchOrClear(ordered), "ordered batch admitted");
        QueueItem item{};
        for (auto const& expected : ordered)
        {
            Expect(queue.TryPop(item), "ordered pop");
            Expect(item.Value == expected.Value && item.Timestamp == expected.Timestamp,
                "ordering and timestamp preservation");
        }

        LoopBe30FixedQueue<QueueItem, 4> otherPort;
        Expect(otherPort.TryPushBatchOrClear(std::span<QueueItem const>(ordered.data(), 1)),
            "isolated port admission");
        Expect(queue.Empty() && otherPort.Count() == 1, "port queue isolation");
    }
}

int wmain()
{
    try
    {
        TestCatalog();
        TestDetector();
        TestQueue();
        std::wcout << L"All LoopBe30 transport core tests passed.\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "LoopBe30 test failure: " << error.what() << '\n';
        return 1;
    }
}
