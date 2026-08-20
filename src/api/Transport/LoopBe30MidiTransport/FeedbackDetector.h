// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

enum class LoopBe30DetectorState : uint8_t
{
    Disabled,
    WaitingForTraffic,
    Observing,
    BelowRateThreshold,
    PatternNotRepeating,
    FeedbackDetected,
    QueueOverload,
    DeliveryFailure,
};

struct LoopBe30NormalizedMidiMessage
{
    uint32_t Signature{};
    uint8_t ByteCount{};
};

struct LoopBe30DetectorDiagnostics
{
    uint32_t BytesPerSecond{};
    uint8_t BestRepeatingPeriod{};
    uint8_t BestMatchPercent{};
    uint8_t ComparisonCount{};
    LoopBe30DetectorState State{ LoopBe30DetectorState::WaitingForTraffic };
};

std::optional<LoopBe30NormalizedMidiMessage> LoopBe30NormalizeMidi1Ump(
    _In_ uint32_t firstWord) noexcept;

class LoopBe30FeedbackAnalyzer
{
public:
    static constexpr int64_t WindowMicroseconds = 1'000'000;
    static constexpr uint32_t RateThresholdBytes = 800;
    // A mixed-source feedback loop can destroy the short repeating pattern
    // while still amplifying traffic far beyond MIDI 1.0 wire speed. Treat
    // that sustained rate as runaway traffic independently of repetition.
    static constexpr uint32_t RunawayRateThresholdBytes = 8'000;

    void Reset() noexcept;
    bool Observe(
        _In_ int64_t timestampMicroseconds,
        _In_ LoopBe30NormalizedMidiMessage message) noexcept;
    LoopBe30DetectorDiagnostics Diagnostics(
        _In_ int64_t timestampMicroseconds,
        _In_ bool enabled,
        _In_ LoopBe30DetectorState latchedState) noexcept;

private:
    struct RateEntry
    {
        int64_t Timestamp{};
        uint8_t Bytes{};
    };

    struct RepetitionResult
    {
        uint8_t BestPeriod{};
        uint8_t BestMatchPercent{};
        uint8_t ComparisonCount{};
        bool Repeating{};
    };

    static constexpr size_t RateHistoryCapacity = 4096;
    static constexpr size_t SignatureHistoryCapacity = 32;

    void Prune(_In_ int64_t timestampMicroseconds) noexcept;
    RepetitionResult AnalyzeRepetition() const noexcept;

    std::array<RateEntry, RateHistoryCapacity> m_rateHistory{};
    size_t m_rateBegin{};
    size_t m_rateCount{};
    uint32_t m_rateBytes{};
    std::array<uint32_t, SignatureHistoryCapacity> m_signatures{};
    uint64_t m_signatureCount{};
    int64_t m_startedAt{};
    bool m_started{};
};
