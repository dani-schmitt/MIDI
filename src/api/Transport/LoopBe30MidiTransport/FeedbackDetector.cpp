// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include <sal.h>
#include "FeedbackDetector.h"

#include <algorithm>

std::optional<LoopBe30NormalizedMidiMessage> LoopBe30NormalizeMidi1Ump(
    uint32_t firstWord) noexcept
{
    const auto messageType = static_cast<uint8_t>((firstWord >> 28) & 0x0f);
    const auto status = static_cast<uint8_t>((firstWord >> 16) & 0xff);

    uint8_t byteCount{};
    if (messageType == 0x2)
    {
        const auto family = static_cast<uint8_t>(status & 0xf0);
        if (family < 0x80 || family > 0xe0) return std::nullopt;
        byteCount = family == 0xc0 || family == 0xd0 ? 2 : 3;
    }
    else if (messageType == 0x1)
    {
        // SysEx7 is UMP type 3. F0/F7 in a System UMP are not valid short
        // messages and are intentionally excluded from v1 analysis.
        if (status == 0xf0 || status == 0xf7) return std::nullopt;
        switch (status)
        {
        case 0xf2: byteCount = 3; break;
        case 0xf1:
        case 0xf3: byteCount = 2; break;
        default: byteCount = 1; break;
        }
    }
    else
    {
        return std::nullopt;
    }

    const uint32_t mask = byteCount == 1 ? 0x00ff0000u :
        byteCount == 2 ? 0x00ffff00u : 0x00ffffffu;
    return LoopBe30NormalizedMidiMessage{
        (static_cast<uint32_t>(byteCount) << 24) | (firstWord & mask), byteCount };
}

void LoopBe30FeedbackAnalyzer::Reset() noexcept
{
    m_rateBegin = 0;
    m_rateCount = 0;
    m_rateBytes = 0;
    m_signatureCount = 0;
    m_startedAt = 0;
    m_started = false;
}

void LoopBe30FeedbackAnalyzer::Prune(int64_t timestampMicroseconds) noexcept
{
    const auto cutoff = timestampMicroseconds - WindowMicroseconds;
    while (m_rateCount != 0 && m_rateHistory[m_rateBegin].Timestamp < cutoff)
    {
        m_rateBytes -= m_rateHistory[m_rateBegin].Bytes;
        m_rateBegin = (m_rateBegin + 1) % RateHistoryCapacity;
        --m_rateCount;
    }
}

LoopBe30FeedbackAnalyzer::RepetitionResult
LoopBe30FeedbackAnalyzer::AnalyzeRepetition() const noexcept
{
    RepetitionResult result{};
    const auto available = static_cast<size_t>(
        (std::min)(m_signatureCount, static_cast<uint64_t>(SignatureHistoryCapacity)));
    const auto firstAbsolute = m_signatureCount - available;
    size_t bestMatches{};
    size_t bestComparisons{};

    for (size_t period = 1; period <= 8; ++period)
    {
        if (available <= period) continue;
        const auto comparisons = available - period;
        if (comparisons < 16) continue;

        size_t matches{};
        for (size_t offset = period; offset < available; ++offset)
        {
            const auto current = m_signatures[(firstAbsolute + offset) % SignatureHistoryCapacity];
            const auto previous = m_signatures[(firstAbsolute + offset - period) % SignatureHistoryCapacity];
            if (current == previous) ++matches;
        }

        if (bestComparisons == 0 || matches * bestComparisons > bestMatches * comparisons)
        {
            bestMatches = matches;
            bestComparisons = comparisons;
            result.BestPeriod = static_cast<uint8_t>(period);
            result.ComparisonCount = static_cast<uint8_t>(comparisons);
        }
        if (matches * 10 >= comparisons * 9) result.Repeating = true;
    }

    if (bestComparisons != 0)
    {
        result.BestMatchPercent = static_cast<uint8_t>(
            (bestMatches * 100 + bestComparisons / 2) / bestComparisons);
    }
    return result;
}

bool LoopBe30FeedbackAnalyzer::Observe(
    int64_t timestampMicroseconds,
    LoopBe30NormalizedMidiMessage message) noexcept
{
    if (!m_started)
    {
        m_started = true;
        m_startedAt = timestampMicroseconds;
    }
    Prune(timestampMicroseconds);

    if (m_rateCount == RateHistoryCapacity)
    {
        m_rateBytes -= m_rateHistory[m_rateBegin].Bytes;
        m_rateBegin = (m_rateBegin + 1) % RateHistoryCapacity;
        --m_rateCount;
    }
    const auto end = (m_rateBegin + m_rateCount) % RateHistoryCapacity;
    m_rateHistory[end] = RateEntry{ timestampMicroseconds, message.ByteCount };
    ++m_rateCount;
    m_rateBytes += message.ByteCount;
    m_signatures[m_signatureCount % SignatureHistoryCapacity] = message.Signature;
    ++m_signatureCount;

    if (timestampMicroseconds - m_startedAt < WindowMicroseconds) return false;

    // The normal detector requires both rate and repetition. A second,
    // deliberately high ceiling catches mixed-source amplification where new
    // messages are interleaved with the loop and prevent a short period from
    // reaching 90%. SysEx7 and non-MIDI-1 UMP never enter m_rateBytes.
    if (m_rateBytes >= RunawayRateThresholdBytes) return true;
    return m_rateBytes >= RateThresholdBytes && AnalyzeRepetition().Repeating;
}

LoopBe30DetectorDiagnostics LoopBe30FeedbackAnalyzer::Diagnostics(
    int64_t timestampMicroseconds,
    bool enabled,
    LoopBe30DetectorState latchedState) noexcept
{
    Prune(timestampMicroseconds);
    const auto repetition = AnalyzeRepetition();
    LoopBe30DetectorDiagnostics result{
        m_rateBytes,
        repetition.BestPeriod,
        repetition.BestMatchPercent,
        repetition.ComparisonCount,
        LoopBe30DetectorState::WaitingForTraffic };

    if (!enabled) result.State = LoopBe30DetectorState::Disabled;
    else if (latchedState == LoopBe30DetectorState::FeedbackDetected ||
        latchedState == LoopBe30DetectorState::QueueOverload ||
        latchedState == LoopBe30DetectorState::DeliveryFailure) result.State = latchedState;
    else if (m_rateBytes == 0) result.State = LoopBe30DetectorState::WaitingForTraffic;
    else if (!m_started || timestampMicroseconds - m_startedAt < WindowMicroseconds)
        result.State = LoopBe30DetectorState::Observing;
    else if (m_rateBytes < RateThresholdBytes)
        result.State = LoopBe30DetectorState::BelowRateThreshold;
    else result.State = LoopBe30DetectorState::PatternNotRepeating;
    return result;
}
