// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "LoopBe30FixedQueue.h"

enum class LoopBe30SafetyReason : uint8_t
{
    None,
    Feedback,
    QueueOverload,
    DeliveryFailure,
};

struct LoopBe30TransportStatus
{
    uint32_t ManualMuteMask{};
    uint32_t SafetyMuteMask{};
    uint32_t EffectiveMuteMask{};
    uint32_t FeedbackMuteMask{};
    uint32_t QueueOverloadMask{};
    uint32_t DeliveryFailureMask{};
    uint32_t Generation{};
    bool FeedbackDetectionEnabled{};
    std::array<LoopBe30DetectorDiagnostics, LOOPBE30_MAX_PORT_COUNT> Diagnostics{};
    struct DeliveryDiagnostics
    {
        uint32_t ActiveCallbacks{};
        uint32_t AcceptedMessages{};
        uint32_t DequeuedMessages{};
        uint32_t CallbackAttempts{};
        uint32_t CallbackSuccesses{};
        uint32_t CallbackFailures{};
        uint32_t LastCallbackResult{};
        uint32_t LastCallbackOptions{};
    };
    std::array<DeliveryDiagnostics, LOOPBE30_MAX_PORT_COUNT> Delivery{};
};

struct LoopBe30QueuedMessage
{
    std::array<uint8_t, 16> Bytes{};
    uint8_t Size{};
    MessageOptionFlags Options{};
    LONGLONG Timestamp{};
};

class LoopBe30Engine
{
public:
    using TrialMuteAppliedCallback = std::function<HRESULT()>;
    using SafetyMuteAppliedCallback = std::function<HRESULT(uint8_t)>;

    ~LoopBe30Engine();
    HRESULT Initialize(_In_ bool feedbackDetectionEnabled);
    HRESULT PreparePort(_In_ uint8_t portIndex, _In_ bool muted);
    void RemoveLastPreparedPort() noexcept;
    HRESULT Start();
    void Shutdown() noexcept;

    uint8_t PortCount() const noexcept { return m_portCount; }
    HRESULT QueueOutgoingUmp(
        _In_ uint8_t portIndex,
        _In_ MessageOptionFlags optionFlags,
        _In_reads_bytes_(size) void const* message,
        _In_ UINT size,
        _In_ LONGLONG timestamp) noexcept;
    HRESULT RegisterCallback(
        _In_ uint8_t portIndex,
        _In_ IMidiCallback* callback,
        _In_ LONGLONG callbackContext,
        _Out_ uint64_t* registrationId) noexcept;
    void UnregisterCallback(_In_ uint8_t portIndex, _In_ uint64_t registrationId) noexcept;

    void NotifyPortOpened() noexcept;
    LoopBe30TrialStatus GetTrialStatus() noexcept;
    bool TrialMuteLocked() noexcept;
    void SetTrialMuteAppliedCallback(_In_ TrialMuteAppliedCallback callback);
    void SetSafetyMuteAppliedCallback(_In_ SafetyMuteAppliedCallback callback);

    HRESULT SetPortMuted(_In_ uint8_t portIndex, _In_ bool muted) noexcept;
    void RestorePortConfiguredMuteState(_In_ uint8_t portIndex, _In_ bool muted) noexcept;
    void RestorePortSafetyMute(
        _In_ uint8_t portIndex,
        _In_ LoopBe30TransportStatus const& previousStatus) noexcept;
    bool IsPortConfiguredMuted(_In_ uint8_t portIndex) const noexcept;
    bool IsPortEffectivelyMuted(_In_ uint8_t portIndex) const noexcept;
    uint32_t ConfiguredMuteMask() const noexcept;
    uint32_t EffectiveMuteMask() const noexcept;

    HRESULT SetFeedbackDetectionEnabled(_In_ bool enabled) noexcept;
    bool FeedbackDetectionEnabled() const noexcept;
    LoopBe30TransportStatus GetStatus() noexcept;

private:
    struct CallbackRegistration
    {
        uint64_t Id{};
        wil::com_ptr_nothrow<IMidiCallback> Callback;
        LONGLONG Context{};
    };

    struct PortContext
    {
        explicit PortContext(uint8_t index) : PortIndex(index) {}
        uint8_t PortIndex{};
        LoopBe30FixedQueue<LoopBe30QueuedMessage, LOOPBE30_QUEUE_CAPACITY> Queue{};
        std::mutex CallbackMutex;
        std::array<CallbackRegistration, LOOPBE30_MAX_CALLBACKS_PER_PORT> Callbacks{};
        uint64_t NextCallbackRegistrationId{ 1 };
        std::atomic<bool> ConfiguredMuted{};
        std::atomic<bool> SafetyMuted{};
        std::atomic<bool> Muted{};
        std::atomic<LoopBe30SafetyReason> SafetyReason{ LoopBe30SafetyReason::None };
        std::atomic<uint32_t> ActiveCallbacks{};
        std::atomic<uint32_t> AcceptedMessages{};
        std::atomic<uint32_t> DequeuedMessages{};
        std::atomic<uint32_t> CallbackAttempts{};
        std::atomic<uint32_t> CallbackSuccesses{};
        std::atomic<uint32_t> CallbackFailures{};
        std::atomic<uint32_t> LastCallbackResult{ static_cast<uint32_t>(S_OK) };
        std::atomic<uint32_t> LastCallbackOptions{};
        LoopBe30FeedbackAnalyzer Analyzer{};
        LoopBe30DetectorDiagnostics Diagnostics{};
    };

    PortContext* GetPort(_In_ uint8_t portIndex) const noexcept;
    bool HasQueuedMessagesLocked() const noexcept;
    bool TryDequeueRoundRobinLocked(
        _Out_ uint8_t& portIndex,
        _Out_ LoopBe30QueuedMessage& message) noexcept;
    void WorkerMain(_In_ std::stop_token stopToken) noexcept;
    void Dispatch(_In_ PortContext& port, _In_ LoopBe30QueuedMessage const& message) noexcept;
    void RequestSafetyMute(_In_ uint8_t portIndex, _In_ LoopBe30SafetyReason reason) noexcept;
    void ResetDetector(_In_ PortContext& port) noexcept;
    bool RefreshTrialEnforcement() noexcept;
    void ApplyTrialMute() noexcept;
    static int64_t CurrentMicroseconds() noexcept;

    mutable std::mutex m_lifecycleMutex;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueWakeup;
    std::array<std::unique_ptr<PortContext>, LOOPBE30_MAX_PORT_COUNT> m_ports{};
    uint8_t m_portCount{};
    uint8_t m_nextPort{};
    std::atomic<bool> m_started{};
    std::atomic<bool> m_stopping{ true };
    std::atomic<bool> m_feedbackDetectionEnabled{ true };
    std::atomic<bool> m_trialTrafficBlocked{};
    std::atomic<uint32_t> m_pendingSafetyPropertyMask{};
    std::atomic<uint32_t> m_generation{};
    std::mutex m_callbackMutex;
    TrialMuteAppliedCallback m_trialMuteAppliedCallback;
    SafetyMuteAppliedCallback m_safetyMuteAppliedCallback;
    LoopBe30TrialState m_trialState;
    std::jthread m_worker;
};
