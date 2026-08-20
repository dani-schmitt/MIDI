// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

#include <chrono>
#include <cstring>

LoopBe30Engine::~LoopBe30Engine()
{
    Shutdown();
}

HRESULT LoopBe30Engine::Initialize(bool feedbackDetectionEnabled)
{
    std::scoped_lock lock(m_lifecycleMutex);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), m_started.load());
    m_feedbackDetectionEnabled.store(feedbackDetectionEnabled);
    const auto trialResult = m_trialState.Initialize();
    if (FAILED(trialResult))
    {
        // Trial state is deliberately fail-closed. Retail Initialize always succeeds.
        m_trialTrafficBlocked.store(true);
    }
    return S_OK;
}

HRESULT LoopBe30Engine::PreparePort(uint8_t portIndex, bool muted)
{
    std::scoped_lock lock(m_lifecycleMutex);
    RETURN_HR_IF(E_INVALIDARG, portIndex >= LOOPBE30_MAX_PORT_COUNT || portIndex != m_portCount);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), m_started.load());
    auto port = std::make_unique<PortContext>(portIndex);
    RETURN_IF_NULL_ALLOC(port);
    port->ConfiguredMuted.store(muted);
    port->Muted.store(muted || m_trialTrafficBlocked.load());
    m_ports[portIndex] = std::move(port);
    ++m_portCount;
    return S_OK;
}

void LoopBe30Engine::RemoveLastPreparedPort() noexcept
{
    std::scoped_lock lock(m_lifecycleMutex);
    if (m_started.load() || m_portCount == 0) return;
    --m_portCount;
    m_ports[m_portCount].reset();
}

HRESULT LoopBe30Engine::Start()
{
    std::scoped_lock lock(m_lifecycleMutex);
    if (m_started.exchange(true)) return S_OK;
    m_stopping.store(false);
    try
    {
        m_worker = std::jthread([this](std::stop_token token) { WorkerMain(token); });
    }
    catch (...)
    {
        m_started.store(false);
        m_stopping.store(true);
        return wil::ResultFromCaughtException();
    }
    return S_OK;
}

void LoopBe30Engine::Shutdown() noexcept
{
    std::unique_lock lifecycleLock(m_lifecycleMutex);
    if (!m_started.exchange(false) && !m_worker.joinable())
    {
        m_trialState.Shutdown();
        return;
    }
    m_stopping.store(true);
    m_queueWakeup.notify_all();
    auto worker = std::move(m_worker);
    lifecycleLock.unlock();
    if (worker.joinable())
    {
        worker.request_stop();
        worker.join();
    }
    lifecycleLock.lock();
    for (auto& port : m_ports) port.reset();
    m_portCount = 0;
    m_nextPort = 0;
    m_trialState.Shutdown();
}

LoopBe30Engine::PortContext* LoopBe30Engine::GetPort(uint8_t portIndex) const noexcept
{
    return portIndex < m_portCount ? m_ports[portIndex].get() : nullptr;
}

HRESULT LoopBe30Engine::QueueOutgoingUmp(
    uint8_t portIndex,
    MessageOptionFlags optionFlags,
    void const* message,
    UINT size,
    LONGLONG timestamp) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, message);
    RETURN_HR_IF(E_INVALIDARG, size == 0 || (size % sizeof(uint32_t)) != 0);
    RETURN_HR_IF(E_INVALIDARG, size > LOOPBE30_QUEUE_CAPACITY * sizeof(uint32_t));
    auto port = GetPort(portIndex);
    RETURN_HR_IF_NULL(E_INVALIDARG, port);

    auto const* words = static_cast<uint32_t const*>(message);
    const auto totalWords = static_cast<size_t>(size / sizeof(uint32_t));
    std::array<LoopBe30QueuedMessage, LOOPBE30_QUEUE_CAPACITY> staged{};
    size_t messageCount{};
    for (size_t offset = 0; offset < totalWords;)
    {
        const auto wordsInMessage = internal::GetUmpLengthInMidiWordsFromFirstWord(words[offset]);
        RETURN_HR_IF(E_INVALIDARG, wordsInMessage == 0 || wordsInMessage > 4 ||
            offset + wordsInMessage > totalWords);
        const auto bytesInMessage = wordsInMessage * sizeof(uint32_t);
        auto& queued = staged[messageCount];
        std::memcpy(queued.Bytes.data(), words + offset, bytesInMessage);
        queued.Size = static_cast<uint8_t>(bytesInMessage);
        // This is an asynchronous loopback delivery, not completion of the
        // client's outgoing send. In particular, propagating
        // WaitForSendComplete into the incoming callback can make the service
        // wait on (or discard) its own looped-back send. ipMIDI similarly
        // creates fresh incoming callback metadata instead of echoing the
        // outbound flags.
        queued.Options = static_cast<MessageOptionFlags>(
            optionFlags & ~MessageOptionFlags_WaitForSendComplete);
        queued.Timestamp = timestamp;
        offset += wordsInMessage;
        ++messageCount;
    }

    // A mute is a successful sink. Validation above still keeps malformed UMP
    // buffers from entering or hiding behind the muted state.
    if (port->Muted.load(std::memory_order_acquire) || !RefreshTrialEnforcement()) return S_OK;

    bool overflow{};
    {
        std::scoped_lock lock(m_queueMutex);
        overflow = !port->Queue.TryPushBatchOrClear(
            std::span<LoopBe30QueuedMessage const>(staged.data(), messageCount));
    }

    if (overflow) RequestSafetyMute(portIndex, LoopBe30SafetyReason::QueueOverload);
    else port->AcceptedMessages.fetch_add(static_cast<uint32_t>(messageCount), std::memory_order_relaxed);
    m_queueWakeup.notify_one();
    return S_OK;
}

HRESULT LoopBe30Engine::RegisterCallback(
    uint8_t portIndex,
    IMidiCallback* callback,
    LONGLONG callbackContext,
    uint64_t* registrationId) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, callback);
    RETURN_HR_IF_NULL(E_INVALIDARG, registrationId);
    auto port = GetPort(portIndex);
    RETURN_HR_IF_NULL(E_INVALIDARG, port);
    std::scoped_lock lock(port->CallbackMutex);
    for (auto& entry : port->Callbacks)
    {
        if (entry.Callback == nullptr)
        {
            entry.Id = port->NextCallbackRegistrationId++;
            entry.Callback = callback;
            entry.Context = callbackContext;
            port->ActiveCallbacks.fetch_add(1, std::memory_order_relaxed);
            *registrationId = entry.Id;
            return S_OK;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
}

void LoopBe30Engine::UnregisterCallback(uint8_t portIndex, uint64_t registrationId) noexcept
{
    auto port = GetPort(portIndex);
    if (port == nullptr || registrationId == 0) return;
    std::scoped_lock lock(port->CallbackMutex);
    for (auto& entry : port->Callbacks)
    {
        if (entry.Id == registrationId)
        {
            entry.Callback.reset();
            entry.Id = 0;
            entry.Context = 0;
            port->ActiveCallbacks.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
    }
}

bool LoopBe30Engine::HasQueuedMessagesLocked() const noexcept
{
    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        if (m_ports[index] != nullptr && !m_ports[index]->Queue.Empty()) return true;
    }
    return false;
}

bool LoopBe30Engine::TryDequeueRoundRobinLocked(
    uint8_t& portIndex,
    LoopBe30QueuedMessage& message) noexcept
{
    if (m_portCount == 0) return false;
    for (uint8_t checked = 0; checked < m_portCount; ++checked)
    {
        const auto index = static_cast<uint8_t>((m_nextPort + checked) % m_portCount);
        auto& port = *m_ports[index];
        if (!port.Queue.TryPop(message)) continue;
        portIndex = index;
        m_nextPort = static_cast<uint8_t>((index + 1) % m_portCount);
        return true;
    }
    return false;
}

void LoopBe30Engine::WorkerMain(std::stop_token stopToken) noexcept
{
    // The MIDI Service callback is a COM interface. This worker is created by
    // std::jthread, so it has no COM apartment unless we initialize one here.
    // Keep this aligned with ipMIDI's receiver worker: callbacks are delivered
    // from an MTA and the apartment remains active for the worker's lifetime.
    auto coInitialize = wil::CoInitializeEx(COINIT_MULTITHREADED);

    while (!stopToken.stop_requested() && !m_stopping.load())
    {
        RefreshTrialEnforcement();
        const auto notificationEvent = m_trialState.NotificationEvent();
        if (notificationEvent != WSA_INVALID_EVENT &&
            WSAWaitForMultipleEvents(1, &notificationEvent, TRUE, 0, FALSE) == WSA_WAIT_EVENT_0)
        {
            m_trialState.HandleRegistryNotification();
            RefreshTrialEnforcement();
        }

        const auto pending = m_pendingSafetyPropertyMask.exchange(0);
        if (pending != 0)
        {
            SafetyMuteAppliedCallback callback;
            {
                std::scoped_lock lock(m_callbackMutex);
                callback = m_safetyMuteAppliedCallback;
            }
            if (callback)
            {
                for (uint8_t index = 0; index < m_portCount; ++index)
                {
                    if ((pending & (1u << index)) != 0) LOG_IF_FAILED(callback(index));
                }
            }
        }

        uint8_t portIndex{};
        LoopBe30QueuedMessage message{};
        {
            std::unique_lock lock(m_queueMutex);
            m_queueWakeup.wait_for(lock, std::chrono::milliseconds(20), [this, &stopToken]
            {
                return stopToken.stop_requested() || m_stopping.load() ||
                    m_pendingSafetyPropertyMask.load() != 0 || HasQueuedMessagesLocked();
            });
            if (stopToken.stop_requested() || m_stopping.load()) break;
            if (!TryDequeueRoundRobinLocked(portIndex, message)) continue;
        }

        auto port = GetPort(portIndex);
        if (port == nullptr || port->Muted.load() || !RefreshTrialEnforcement()) continue;
        port->DequeuedMessages.fetch_add(1, std::memory_order_relaxed);

        bool feedbackDetected{};
        if (m_feedbackDetectionEnabled.load())
        {
            uint32_t firstWord{};
            std::memcpy(&firstWord, message.Bytes.data(), sizeof(firstWord));
            if (auto normalized = LoopBe30NormalizeMidi1Ump(firstWord))
            {
                std::scoped_lock lock(m_queueMutex);
                feedbackDetected = port->Analyzer.Observe(CurrentMicroseconds(), *normalized);
                port->Diagnostics = port->Analyzer.Diagnostics(
                    CurrentMicroseconds(), true, LoopBe30DetectorState::WaitingForTraffic);
            }
        }

        if (feedbackDetected)
        {
            RequestSafetyMute(portIndex, LoopBe30SafetyReason::Feedback);
            continue;
        }
        Dispatch(*port, message);
    }
}

void LoopBe30Engine::Dispatch(
    PortContext& port,
    LoopBe30QueuedMessage const& message) noexcept
{
    // A looped-back message is a newly received message. Do not reuse the
    // timestamp from the client's outgoing WinMM/UMP write here. In particular,
    // legacy WinMM sends commonly arrive with position 0 and this endpoint is
    // intentionally published with GenerateIncomingTimestamps disabled. A zero
    // timestamp therefore survives every service transform and the legacy input
    // client never reports the message. ipMIDI establishes the same receive-side
    // boundary after recvfrom() by assigning the current MIDI/QPC timestamp.
    const auto incomingTimestamp = internal::GetCurrentMidiTimestamp();

    std::array<CallbackRegistration, LOOPBE30_MAX_CALLBACKS_PER_PORT> callbacks{};
    {
        std::scoped_lock lock(port.CallbackMutex);
        size_t target{};
        for (auto const& entry : port.Callbacks)
        {
            if (entry.Callback != nullptr) callbacks[target++] = entry;
        }
    }

    for (auto const& callback : callbacks)
    {
        if (callback.Callback == nullptr) break;
        // This is a newly received single-group UMP. Use the same callback
        // metadata as the proven ipMIDI receive dispatcher instead of echoing
        // any client-side send options.
        constexpr auto incomingOptions = MessageOptionFlags_ContextContainsGroupIndex;
        port.LastCallbackOptions.store(static_cast<uint32_t>(incomingOptions), std::memory_order_relaxed);
        port.CallbackAttempts.fetch_add(1, std::memory_order_relaxed);
        const auto result = callback.Callback->Callback(
            incomingOptions,
            const_cast<uint8_t*>(message.Bytes.data()),
            message.Size,
            incomingTimestamp,
            0);
        port.LastCallbackResult.store(static_cast<uint32_t>(result), std::memory_order_relaxed);
        if (FAILED(result))
        {
            port.CallbackFailures.fetch_add(1, std::memory_order_relaxed);
            RequestSafetyMute(port.PortIndex, LoopBe30SafetyReason::DeliveryFailure);
            return;
        }
        port.CallbackSuccesses.fetch_add(1, std::memory_order_relaxed);
    }
}

void LoopBe30Engine::RequestSafetyMute(
    uint8_t portIndex,
    LoopBe30SafetyReason reason) noexcept
{
    auto port = GetPort(portIndex);
    if (port == nullptr) return;
    LoopBe30SafetyReason expected = LoopBe30SafetyReason::None;
    if (!port->SafetyReason.compare_exchange_strong(expected, reason)) return;
    port->SafetyMuted.store(true);
    port->Muted.store(true);
    {
        std::scoped_lock lock(m_queueMutex);
        switch (reason)
        {
        case LoopBe30SafetyReason::Feedback:
            port->Diagnostics.State = LoopBe30DetectorState::FeedbackDetected;
            break;
        case LoopBe30SafetyReason::QueueOverload:
            port->Diagnostics.State = LoopBe30DetectorState::QueueOverload;
            break;
        case LoopBe30SafetyReason::DeliveryFailure:
            port->Diagnostics.State = LoopBe30DetectorState::DeliveryFailure;
            break;
        default: break;
        }
    }
    m_pendingSafetyPropertyMask.fetch_or(1u << portIndex);
    m_generation.fetch_add(1);
    m_queueWakeup.notify_one();
}

void LoopBe30Engine::ResetDetector(PortContext& port) noexcept
{
    port.Analyzer.Reset();
    port.Diagnostics = {};
    port.Diagnostics.State = m_feedbackDetectionEnabled.load()
        ? LoopBe30DetectorState::WaitingForTraffic
        : LoopBe30DetectorState::Disabled;
}

HRESULT LoopBe30Engine::SetPortMuted(uint8_t portIndex, bool muted) noexcept
{
    auto port = GetPort(portIndex);
    RETURN_HR_IF_NULL(E_INVALIDARG, port);
    if (!muted && TrialMuteLocked())
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);

    port->ConfiguredMuted.store(muted);
    if (!muted)
    {
        port->SafetyMuted.store(false);
        port->SafetyReason.store(LoopBe30SafetyReason::None);
    }
    port->Muted.store(muted || port->SafetyMuted.load() || m_trialTrafficBlocked.load());
    {
        std::scoped_lock lock(m_queueMutex);
        port->Queue.Clear();
        ResetDetector(*port);
    }
    m_generation.fetch_add(1);
    return S_OK;
}

void LoopBe30Engine::RestorePortConfiguredMuteState(uint8_t portIndex, bool muted) noexcept
{
    auto port = GetPort(portIndex);
    if (port == nullptr) return;
    port->ConfiguredMuted.store(muted);
    port->Muted.store(muted || port->SafetyMuted.load() || m_trialTrafficBlocked.load());
}

void LoopBe30Engine::RestorePortSafetyMute(
    uint8_t portIndex,
    LoopBe30TransportStatus const& status) noexcept
{
    auto port = GetPort(portIndex);
    if (port == nullptr) return;
    const auto bit = 1u << portIndex;
    port->SafetyMuted.store((status.SafetyMuteMask & bit) != 0);
    LoopBe30SafetyReason reason = LoopBe30SafetyReason::None;
    if ((status.FeedbackMuteMask & bit) != 0) reason = LoopBe30SafetyReason::Feedback;
    else if ((status.QueueOverloadMask & bit) != 0) reason = LoopBe30SafetyReason::QueueOverload;
    else if ((status.DeliveryFailureMask & bit) != 0) reason = LoopBe30SafetyReason::DeliveryFailure;
    port->SafetyReason.store(reason);
    port->Muted.store(port->ConfiguredMuted.load() || port->SafetyMuted.load() || m_trialTrafficBlocked.load());
}

bool LoopBe30Engine::IsPortConfiguredMuted(uint8_t portIndex) const noexcept
{
    auto port = GetPort(portIndex);
    return port != nullptr && port->ConfiguredMuted.load();
}

bool LoopBe30Engine::IsPortEffectivelyMuted(uint8_t portIndex) const noexcept
{
    auto port = GetPort(portIndex);
    return port != nullptr && port->Muted.load();
}

uint32_t LoopBe30Engine::ConfiguredMuteMask() const noexcept
{
    uint32_t mask{};
    for (uint8_t index = 0; index < m_portCount; ++index)
        if (m_ports[index]->ConfiguredMuted.load()) mask |= 1u << index;
    return mask;
}

uint32_t LoopBe30Engine::EffectiveMuteMask() const noexcept
{
    uint32_t mask{};
    for (uint8_t index = 0; index < m_portCount; ++index)
        if (m_ports[index]->Muted.load()) mask |= 1u << index;
    return mask;
}

HRESULT LoopBe30Engine::SetFeedbackDetectionEnabled(bool enabled) noexcept
{
    m_feedbackDetectionEnabled.store(enabled);
    {
        std::scoped_lock lock(m_queueMutex);
        for (uint8_t index = 0; index < m_portCount; ++index) ResetDetector(*m_ports[index]);
    }
    m_generation.fetch_add(1);
    return S_OK;
}

bool LoopBe30Engine::FeedbackDetectionEnabled() const noexcept
{
    return m_feedbackDetectionEnabled.load();
}

LoopBe30TransportStatus LoopBe30Engine::GetStatus() noexcept
{
    LoopBe30TransportStatus status{};
    status.FeedbackDetectionEnabled = m_feedbackDetectionEnabled.load();
    status.Generation = m_generation.load();
    std::scoped_lock lock(m_queueMutex);
    const auto now = CurrentMicroseconds();
    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        auto& port = *m_ports[index];
        const auto bit = 1u << index;
        if (port.ConfiguredMuted.load()) status.ManualMuteMask |= bit;
        if (port.SafetyMuted.load()) status.SafetyMuteMask |= bit;
        if (port.Muted.load()) status.EffectiveMuteMask |= bit;
        switch (port.SafetyReason.load())
        {
        case LoopBe30SafetyReason::Feedback: status.FeedbackMuteMask |= bit; break;
        case LoopBe30SafetyReason::QueueOverload: status.QueueOverloadMask |= bit; break;
        case LoopBe30SafetyReason::DeliveryFailure: status.DeliveryFailureMask |= bit; break;
        default: break;
        }
        auto latched = port.Diagnostics.State;
        status.Diagnostics[index] = port.Analyzer.Diagnostics(now, status.FeedbackDetectionEnabled, latched);
        auto& delivery = status.Delivery[index];
        delivery.ActiveCallbacks = port.ActiveCallbacks.load(std::memory_order_relaxed);
        delivery.AcceptedMessages = port.AcceptedMessages.load(std::memory_order_relaxed);
        delivery.DequeuedMessages = port.DequeuedMessages.load(std::memory_order_relaxed);
        delivery.CallbackAttempts = port.CallbackAttempts.load(std::memory_order_relaxed);
        delivery.CallbackSuccesses = port.CallbackSuccesses.load(std::memory_order_relaxed);
        delivery.CallbackFailures = port.CallbackFailures.load(std::memory_order_relaxed);
        delivery.LastCallbackResult = port.LastCallbackResult.load(std::memory_order_relaxed);
        delivery.LastCallbackOptions = port.LastCallbackOptions.load(std::memory_order_relaxed);
    }
    return status;
}

void LoopBe30Engine::NotifyPortOpened() noexcept
{
    if (FAILED(m_trialState.StartOnFirstPortOpen())) m_trialTrafficBlocked.store(true);
    RefreshTrialEnforcement();
    m_queueWakeup.notify_one();
}

LoopBe30TrialStatus LoopBe30Engine::GetTrialStatus() noexcept
{
    RefreshTrialEnforcement();
    return m_trialState.GetStatus();
}

bool LoopBe30Engine::TrialMuteLocked() noexcept
{
    RefreshTrialEnforcement();
    return m_trialTrafficBlocked.load();
}

bool LoopBe30Engine::RefreshTrialEnforcement() noexcept
{
    if (m_trialState.IsTrafficAllowed() && !m_trialTrafficBlocked.load()) return true;
    if (!m_trialTrafficBlocked.exchange(true)) ApplyTrialMute();
    return false;
}

void LoopBe30Engine::ApplyTrialMute() noexcept
{
    for (uint8_t index = 0; index < m_portCount; ++index) m_ports[index]->Muted.store(true);
    m_generation.fetch_add(1);
    TrialMuteAppliedCallback callback;
    {
        std::scoped_lock lock(m_callbackMutex);
        callback = m_trialMuteAppliedCallback;
    }
    if (callback) LOG_IF_FAILED(callback());
}

void LoopBe30Engine::SetTrialMuteAppliedCallback(TrialMuteAppliedCallback callback)
{
    std::scoped_lock lock(m_callbackMutex);
    m_trialMuteAppliedCallback = std::move(callback);
}

void LoopBe30Engine::SetSafetyMuteAppliedCallback(SafetyMuteAppliedCallback callback)
{
    std::scoped_lock lock(m_callbackMutex);
    m_safetyMuteAppliedCallback = std::move(callback);
}

int64_t LoopBe30Engine::CurrentMicroseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
