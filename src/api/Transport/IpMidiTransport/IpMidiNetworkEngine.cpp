// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

namespace
{
    constexpr uint32_t SafetyFaultOverflow = 0x1;
    constexpr uint32_t SafetyFaultReceive = 0x2;
    constexpr uint32_t SafetyFaultSend = 0x4;

    bool RecordOverflowDrop(
        std::array<uint64_t, IP_MIDI_OVERFLOW_TRIP_COUNT>& timestamps,
        size_t& head,
        size_t& count,
        uint64_t now) noexcept
    {
        while (count > 0 &&
            now - timestamps[head] > IP_MIDI_OVERFLOW_WINDOW_MILLISECONDS)
        {
            head = (head + 1) % timestamps.size();
            --count;
        }
        if (count < timestamps.size())
        {
            timestamps[(head + count) % timestamps.size()] = now;
            ++count;
        }
        return count >= IP_MIDI_OVERFLOW_TRIP_COUNT;
    }
}

HRESULT IpMidiNetworkEngine::Initialize(bool loopbackEnabled)
{
    std::scoped_lock lifecycleLock(m_lifecycleMutex);
    if (m_initialized)
    {
        return S_OK;
    }

    m_loopbackEnabled = loopbackEnabled;
    RETURN_IF_FAILED(CreateSharedResources());
    const auto trialInitializeResult = m_trialState.Initialize();
    if (FAILED(trialInitializeResult))
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI Trial runtime state initialization failed; traffic is disabled",
                MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingHResult(trialInitializeResult, MIDI_TRACE_EVENT_HRESULT_FIELD));
    }
    RefreshTrialEnforcement();
    m_initialized = true;
    m_stopping = true;
    return S_OK;
}

HRESULT IpMidiNetworkEngine::CreateSharedResources()
{
    WSADATA winsockData{};
    const auto startupResult = WSAStartup(MAKEWORD(2, 2), &winsockData);
    if (startupResult != 0)
    {
        return HRESULT_FROM_WIN32(startupResult);
    }
    m_winsockStarted = true;

    const auto sendSocketResult = CreateConfiguredSendSocket(m_sendSocket);
    if (FAILED(sendSocketResult))
    {
        CloseSharedResources();
        return sendSocketResult;
    }

    m_stopEvent = WSACreateEvent();
    if (m_stopEvent == WSA_INVALID_EVENT)
    {
        const auto error = WSAGetLastError();
        CloseSharedResources();
        return HRESULT_FROM_WIN32(error);
    }

    m_reconfigureEvent = WSACreateEvent();
    if (m_reconfigureEvent == WSA_INVALID_EVENT)
    {
        const auto error = WSAGetLastError();
        CloseSharedResources();
        return HRESULT_FROM_WIN32(error);
    }

    RefreshLocalIpv4Addresses();
    return S_OK;
}

HRESULT IpMidiNetworkEngine::CreateConfiguredSendSocket(SOCKET& sendSocket)
{
    sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sendSocket == INVALID_SOCKET)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    const u_char multicastTtl = 1;
    const u_char multicastLoopback = m_loopbackEnabled ? 1 : 0;
    if (setsockopt(sendSocket, IPPROTO_IP, IP_MULTICAST_TTL,
            reinterpret_cast<char const*>(&multicastTtl), sizeof(multicastTtl)) == SOCKET_ERROR ||
        setsockopt(sendSocket, IPPROTO_IP, IP_MULTICAST_LOOP,
            reinterpret_cast<char const*>(&multicastLoopback), sizeof(multicastLoopback)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        closesocket(sendSocket);
        sendSocket = INVALID_SOCKET;
        return HRESULT_FROM_WIN32(error);
    }
    return S_OK;
}

HRESULT IpMidiNetworkEngine::RecreateSendSocket()
{
    std::scoped_lock sendSocketLock(m_sendSocketMutex);
    SOCKET replacement{ INVALID_SOCKET };
    RETURN_IF_FAILED(CreateConfiguredSendSocket(replacement));
    const auto previous = m_sendSocket;
    m_sendSocket = replacement;
    if (previous != INVALID_SOCKET)
    {
        closesocket(previous);
    }
    return S_OK;
}

HRESULT IpMidiNetworkEngine::SendPacketWithRecovery(
    sockaddr_in const& destination,
    IpMidiPacket const& packet)
{
    std::scoped_lock sendSocketLock(m_sendSocketMutex);
    auto sendPacket = [&]() noexcept
    {
        if (m_sendSocket == INVALID_SOCKET)
        {
            WSASetLastError(WSAENOTSOCK);
            return SOCKET_ERROR;
        }
        return sendto(m_sendSocket, reinterpret_cast<char const*>(packet.Bytes.data()), packet.Length, 0,
            reinterpret_cast<sockaddr const*>(&destination), sizeof(destination));
    };

    auto sent = sendPacket();
    if (sent == packet.Length)
    {
        return S_OK;
    }

    const auto initialError = sent == SOCKET_ERROR ? WSAGetLastError() : WSAEMSGSIZE;
    SOCKET replacement{ INVALID_SOCKET };
    const auto createResult = CreateConfiguredSendSocket(replacement);
    if (FAILED(createResult))
    {
        return createResult;
    }

    const auto previous = m_sendSocket;
    m_sendSocket = replacement;
    if (previous != INVALID_SOCKET)
    {
        closesocket(previous);
    }

    sent = sendPacket();
    if (sent == packet.Length)
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI shared send socket recovered", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingInt32(initialError, "initial winsock error"));
        return S_OK;
    }
    return HRESULT_FROM_WIN32(sent == SOCKET_ERROR ? WSAGetLastError() : WSAEMSGSIZE);
}

HRESULT IpMidiNetworkEngine::PreparePort(uint8_t portIndex, bool muted)
{
    std::scoped_lock lifecycleLock(m_lifecycleMutex);
    RETURN_HR_IF(E_UNEXPECTED, !m_initialized || m_started);
    RETURN_HR_IF(E_INVALIDARG, portIndex >= IP_MIDI_MAX_PORT_COUNT || portIndex != m_portCount);

    try
    {
        RefreshTrialEnforcement();
        auto port = std::make_unique<PortContext>(portIndex);
        port->ConfiguredMuted = muted;
        const bool effectiveMuted = muted || m_trialTrafficBlocked.load();
        port->Muted = effectiveMuted;
        if (!effectiveMuted)
        {
            RETURN_IF_FAILED(CreateReceiveResources(*port));
        }
        m_ports[portIndex] = std::move(port);
        ++m_portCount;
    }
    catch (...)
    {
        return wil::ResultFromCaughtException();
    }

    return S_OK;
}

HRESULT IpMidiNetworkEngine::CreateReceiveResources(PortContext& port)
{
    port.ReceiveSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (port.ReceiveSocket == INVALID_SOCKET)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    int reuseAddress = 1;
    if (setsockopt(port.ReceiveSocket, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<char*>(&reuseAddress), sizeof(reuseAddress)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        CloseReceiveResources(port);
        return HRESULT_FROM_WIN32(error);
    }

    sockaddr_in receiveAddress{};
    receiveAddress.sin_family = AF_INET;
    receiveAddress.sin_port = htons(port.UdpPort);
    receiveAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(port.ReceiveSocket, reinterpret_cast<sockaddr*>(&receiveAddress), sizeof(receiveAddress)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        CloseReceiveResources(port);
        return HRESULT_FROM_WIN32(error);
    }

    if (InetPtonA(AF_INET, IP_MIDI_MULTICAST_ADDRESS, &port.MulticastMembership.imr_multiaddr) != 1)
    {
        CloseReceiveResources(port);
        return HRESULT_FROM_WIN32(WSAEINVAL);
    }
    port.MulticastMembership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(port.ReceiveSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
        reinterpret_cast<char*>(&port.MulticastMembership), sizeof(port.MulticastMembership)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        CloseReceiveResources(port);
        return HRESULT_FROM_WIN32(error);
    }

    port.ReceiveEvent = WSACreateEvent();
    if (port.ReceiveEvent == WSA_INVALID_EVENT)
    {
        const auto error = WSAGetLastError();
        CloseReceiveResources(port);
        return HRESULT_FROM_WIN32(error);
    }

    if (WSAEventSelect(port.ReceiveSocket, port.ReceiveEvent, FD_READ | FD_CLOSE) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        CloseReceiveResources(port);
        return HRESULT_FROM_WIN32(error);
    }

    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI port prepared", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"),
        TraceLoggingUInt16(port.UdpPort, "udp port"));
    return S_OK;
}

HRESULT IpMidiNetworkEngine::RecoverReceiveResources(PortContext& port)
{
    CloseReceiveResources(port);
    ResetPortData(port);
    const auto result = CreateReceiveResources(port);
    if (SUCCEEDED(result))
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI receive socket recovered", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"));
    }
    return result;
}

void IpMidiNetworkEngine::RemoveLastPreparedPort()
{
    std::scoped_lock lifecycleLock(m_lifecycleMutex);
    if (m_started || m_portCount == 0)
    {
        return;
    }

    const auto index = static_cast<uint8_t>(m_portCount - 1);
    CloseReceiveResources(*m_ports[index]);
    m_ports[index].reset();
    --m_portCount;
}

HRESULT IpMidiNetworkEngine::Start()
{
    std::scoped_lock lifecycleLock(m_lifecycleMutex);
    RETURN_HR_IF(E_UNEXPECTED, !m_initialized);
    if (m_started)
    {
        return S_OK;
    }

    m_stopping = false;
    WSAResetEvent(m_stopEvent);
    WSAResetEvent(m_reconfigureEvent);
    m_trialMutePending = false;
    m_safetyMutePendingMask = 0;
    m_safetyMuteMask = 0;
    m_overflowMask = 0;
    m_receiveFailureMask = 0;
    m_sendFailureMask = 0;
    m_networkStatusGeneration = 0;

    RefreshTrialEnforcement();
    if (m_trialTrafficBlocked)
    {
        for (uint8_t index = 0; index < m_portCount; ++index)
        {
            auto& port = *m_ports[index];
            port.Muted = true;
            CloseReceiveResources(port);
            ResetPortData(port);
        }
    }

    m_started = true;
    if (m_portCount > 0)
    {
        try
        {
            m_senderThread = std::jthread(std::bind_front(&IpMidiNetworkEngine::SenderWorker, this));
            m_receiverThread = std::jthread(std::bind_front(&IpMidiNetworkEngine::ReceiverWorker, this));
            m_dispatcherThread = std::jthread(std::bind_front(&IpMidiNetworkEngine::DispatcherWorker, this));
        }
        catch (...)
        {
            m_stopping = true;
            m_started = false;
            WSASetEvent(m_stopEvent);
            m_outgoingWakeup.notify_all();
            m_incomingWakeup.notify_all();
            return wil::ResultFromCaughtException();
        }
    }

    if (m_trialMutePending && m_reconfigureEvent != WSA_INVALID_EVENT)
    {
        WSASetEvent(m_reconfigureEvent);
    }
    else if (m_trialTrafficBlocked)
    {
        NotifyTrialMuteApplied();
    }
    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI shared network workers started", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt8(m_portCount, "active port count"),
        TraceLoggingUInt8(m_portCount > 0 ? 3 : 0, "worker thread count"));
    return S_OK;
}

void IpMidiNetworkEngine::Shutdown()
{
    {
        std::scoped_lock lifecycleLock(m_lifecycleMutex);
        if (!m_initialized)
        {
            return;
        }
        m_stopping = true;
        m_started = false;
    }

    if (m_stopEvent != WSA_INVALID_EVENT)
    {
        WSASetEvent(m_stopEvent);
    }
    if (m_reconfigureEvent != WSA_INVALID_EVENT)
    {
        WSASetEvent(m_reconfigureEvent);
    }
    m_outgoingWakeup.notify_all();
    m_incomingWakeup.notify_all();
    m_senderThread.request_stop();
    m_receiverThread.request_stop();
    m_dispatcherThread.request_stop();
    m_reconfigureComplete.notify_all();

    if (m_senderThread.joinable()) m_senderThread.join();
    if (m_receiverThread.joinable()) m_receiverThread.join();
    if (m_dispatcherThread.joinable()) m_dispatcherThread.join();

    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        auto& port = *m_ports[index];
        CloseReceiveResources(port);
        port.OutgoingQueue.clear();
        port.IncomingQueue.clear();
        std::scoped_lock callbackLock(port.CallbackMutex);
        for (auto& registration : port.Callbacks)
        {
            registration.Callback.reset();
            registration.Id = 0;
        }
    }

    m_trialState.Shutdown();
    {
        std::scoped_lock callbackLock(m_trialCallbackMutex);
        m_trialMuteAppliedCallback = nullptr;
    }
    {
        std::scoped_lock callbackLock(m_safetyCallbackMutex);
        m_safetyMuteAppliedCallback = nullptr;
    }
    CloseSharedResources();
    m_initialized = false;
}

void IpMidiNetworkEngine::CloseReceiveResources(PortContext& port)
{
    if (port.ReceiveSocket != INVALID_SOCKET)
    {
        setsockopt(port.ReceiveSocket, IPPROTO_IP, IP_DROP_MEMBERSHIP,
            reinterpret_cast<char*>(&port.MulticastMembership), sizeof(port.MulticastMembership));
        closesocket(port.ReceiveSocket);
        port.ReceiveSocket = INVALID_SOCKET;
    }
    if (port.ReceiveEvent != WSA_INVALID_EVENT)
    {
        WSACloseEvent(port.ReceiveEvent);
        port.ReceiveEvent = WSA_INVALID_EVENT;
    }
}

void IpMidiNetworkEngine::CloseSharedResources()
{
    std::scoped_lock sendSocketLock(m_sendSocketMutex);
    if (m_sendSocket != INVALID_SOCKET)
    {
        closesocket(m_sendSocket);
        m_sendSocket = INVALID_SOCKET;
    }
    if (m_stopEvent != WSA_INVALID_EVENT)
    {
        WSACloseEvent(m_stopEvent);
        m_stopEvent = WSA_INVALID_EVENT;
    }
    if (m_reconfigureEvent != WSA_INVALID_EVENT)
    {
        WSACloseEvent(m_reconfigureEvent);
        m_reconfigureEvent = WSA_INVALID_EVENT;
    }
    if (m_winsockStarted)
    {
        WSACleanup();
        m_winsockStarted = false;
    }
}

HRESULT IpMidiNetworkEngine::SetLoopbackEnabled(bool enabled)
{
    std::scoped_lock sendSocketLock(m_sendSocketMutex);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE), m_stopping || m_sendSocket == INVALID_SOCKET);

    const u_char multicastLoopback = enabled ? 1 : 0;
    if (setsockopt(m_sendSocket, IPPROTO_IP, IP_MULTICAST_LOOP,
        reinterpret_cast<char const*>(&multicastLoopback), sizeof(multicastLoopback)) == SOCKET_ERROR)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    m_loopbackEnabled = enabled;
    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI local multicast loopback changed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingBool(enabled, "enabled"));
    return S_OK;
}

bool IpMidiNetworkEngine::IsPortConfiguredMuted(uint8_t portIndex) const noexcept
{
    const auto port = GetPort(portIndex);
    return port == nullptr || port->ConfiguredMuted.load();
}

bool IpMidiNetworkEngine::IsPortEffectivelyMuted(uint8_t portIndex) const noexcept
{
    const auto port = GetPort(portIndex);
    return port == nullptr || port->Muted.load();
}

uint32_t IpMidiNetworkEngine::ConfiguredMuteMask() const noexcept
{
    uint32_t mask{};
    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        if (m_ports[index]->ConfiguredMuted.load()) mask |= (1u << index);
    }
    return mask;
}

uint32_t IpMidiNetworkEngine::EffectiveMuteMask() const noexcept
{
    uint32_t mask{};
    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        if (m_ports[index]->Muted.load()) mask |= (1u << index);
    }
    return mask;
}

IpMidiNetworkStatus IpMidiNetworkEngine::GetNetworkStatus() const noexcept
{
    return IpMidiNetworkStatus
    {
        m_safetyMuteMask.load(),
        EffectiveMuteMask(),
        m_overflowMask.load(),
        m_receiveFailureMask.load(),
        m_sendFailureMask.load(),
        m_networkStatusGeneration.load()
    };
}

void IpMidiNetworkEngine::SetTrialMuteAppliedCallback(TrialMuteAppliedCallback callback)
{
    std::scoped_lock callbackLock(m_trialCallbackMutex);
    m_trialMuteAppliedCallback = std::move(callback);
}

void IpMidiNetworkEngine::SetSafetyMuteAppliedCallback(SafetyMuteAppliedCallback callback)
{
    std::scoped_lock callbackLock(m_safetyCallbackMutex);
    m_safetyMuteAppliedCallback = std::move(callback);
}

void IpMidiNetworkEngine::NotifyPortOpened() noexcept
{
    const auto startResult = m_trialState.StartOnFirstPortOpen();
    if (FAILED(startResult))
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"Unable to start or restore the ipMIDI Trial evaluation period; traffic is disabled",
                MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingHResult(startResult, MIDI_TRACE_EVENT_HRESULT_FIELD));
    }

#ifdef IPMIDI_TRIAL
    // The receiver may currently be waiting forever because no port had opened yet.
    // Wake it so it can rebuild the wait with the evaluation deadline as its timeout.
    if (m_reconfigureEvent != WSA_INVALID_EVENT)
    {
        WSASetEvent(m_reconfigureEvent);
    }
#endif
    RefreshTrialEnforcement();
}

IpMidiTrialStatus IpMidiNetworkEngine::GetTrialStatus() noexcept
{
    RefreshTrialEnforcement();
    return m_trialState.GetStatus();
}

bool IpMidiNetworkEngine::TrialMuteLocked() noexcept
{
    RefreshTrialEnforcement();
    return m_trialTrafficBlocked.load();
}

bool IpMidiNetworkEngine::RefreshTrialEnforcement() noexcept
{
    if (m_trialState.IsTrafficAllowed())
    {
        return true;
    }

    if (!m_trialTrafficBlocked.exchange(true))
    {
        ApplyTrialTrafficBlock();
    }
    return false;
}

void IpMidiNetworkEngine::ApplyTrialTrafficBlock() noexcept
{
    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        m_ports[index]->Muted = true;
    }

    m_outgoingWakeup.notify_all();
    m_incomingWakeup.notify_all();
    if (m_started.load())
    {
        m_trialMutePending = true;
        if (m_reconfigureEvent != WSA_INVALID_EVENT)
        {
            WSASetEvent(m_reconfigureEvent);
        }
    }
    const auto status = m_trialState.GetStatus();
    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI Trial traffic disabled and forced mute requested", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt8(static_cast<uint8_t>(status.State), "trial state"));
}

void IpMidiNetworkEngine::ApplyTrialMuteOnReceiverThread() noexcept
{
    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        auto& port = *m_ports[index];
        port.Muted = true;
        {
            std::scoped_lock sendLock(port.SendMutex);
        }
        {
            std::scoped_lock dispatchLock(port.CallbackDispatchMutex);
        }
        CloseReceiveResources(port);
        ResetPortData(port);
    }

    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"All active ipMIDI Trial ports are effectively muted", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt8(m_portCount, "active port count"));
    NotifyTrialMuteApplied();
}

void IpMidiNetworkEngine::NotifyTrialMuteApplied() noexcept
{
    TrialMuteAppliedCallback callback;
    {
        std::scoped_lock callbackLock(m_trialCallbackMutex);
        callback = m_trialMuteAppliedCallback;
    }
    if (callback)
    {
        LOG_IF_FAILED(callback());
    }
}

void IpMidiNetworkEngine::RequestSafetyMute(uint8_t portIndex, uint32_t reasonMask) noexcept
{
    auto port = GetPort(portIndex);
    if (port == nullptr || m_trialTrafficBlocked.load())
    {
        return;
    }

    const uint32_t portBit = 1u << portIndex;
    bool statusChanged{};
    if ((reasonMask & SafetyFaultOverflow) != 0)
    {
        statusChanged = (m_overflowMask.fetch_or(portBit) & portBit) == 0 || statusChanged;
    }
    if ((reasonMask & SafetyFaultReceive) != 0)
    {
        statusChanged = (m_receiveFailureMask.fetch_or(portBit) & portBit) == 0 || statusChanged;
    }
    if ((reasonMask & SafetyFaultSend) != 0)
    {
        statusChanged = (m_sendFailureMask.fetch_or(portBit) & portBit) == 0 || statusChanged;
    }

    const bool newlyLatched = !port->SafetyMuted.exchange(true);
    port->Muted = true;
    m_safetyMuteMask.fetch_or(portBit);
    m_safetyMutePendingMask.fetch_or(portBit);
    if (newlyLatched || statusChanged)
    {
        ++m_networkStatusGeneration;
    }

    m_outgoingWakeup.notify_all();
    m_incomingWakeup.notify_all();
    if (m_reconfigureEvent != WSA_INVALID_EVENT)
    {
        WSASetEvent(m_reconfigureEvent);
    }

    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
        TraceLoggingWideString(L"ipMIDI port safety mute requested", MIDI_TRACE_EVENT_MESSAGE_FIELD),
        TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
        TraceLoggingUInt32(reasonMask, "fault reason mask"),
        TraceLoggingBool(newlyLatched, "newly latched"));
}

void IpMidiNetworkEngine::ApplyPendingSafetyMutesOnReceiverThread() noexcept
{
    const auto pendingMask = m_safetyMutePendingMask.exchange(0);
    for (uint8_t index = 0; index < m_portCount; ++index)
    {
        if ((pendingMask & (1u << index)) == 0)
        {
            continue;
        }

        auto& port = *m_ports[index];
        port.SafetyMuted = true;
        port.Muted = true;
        {
            std::scoped_lock sendLock(port.SendMutex);
        }
        {
            std::scoped_lock dispatchLock(port.CallbackDispatchMutex);
        }
        CloseReceiveResources(port);
        ResetPortData(port);
        NotifySafetyMuteApplied(index);
    }
}

void IpMidiNetworkEngine::ClearSafetyFault(PortContext& port) noexcept
{
    const uint32_t portBit = 1u << port.PortIndex;
    const bool wasSafetyMuted = port.SafetyMuted.exchange(false);
    m_safetyMutePendingMask.fetch_and(~portBit);
    m_safetyMuteMask.fetch_and(~portBit);
    m_overflowMask.fetch_and(~portBit);
    m_receiveFailureMask.fetch_and(~portBit);
    m_sendFailureMask.fetch_and(~portBit);
    if (wasSafetyMuted)
    {
        ++m_networkStatusGeneration;
    }
}

void IpMidiNetworkEngine::NotifySafetyMuteApplied(uint8_t portIndex) noexcept
{
    SafetyMuteAppliedCallback callback;
    {
        std::scoped_lock callbackLock(m_safetyCallbackMutex);
        callback = m_safetyMuteAppliedCallback;
    }
    if (callback)
    {
        LOG_IF_FAILED(callback(portIndex));
    }
}

void IpMidiNetworkEngine::ResetPortData(PortContext& port)
{
    {
        std::scoped_lock queueLock(m_outgoingMutex);
        port.OutgoingQueue.clear();
        port.OutgoingOverflowTimestampHead = 0;
        port.OutgoingOverflowTimestampCount = 0;
    }
    {
        std::scoped_lock queueLock(m_incomingMutex);
        port.IncomingQueue.clear();
        port.IncomingOverflowTimestampHead = 0;
        port.IncomingOverflowTimestampCount = 0;
    }
    {
        std::scoped_lock converterLock(port.OutgoingConverterMutex);
        port.UmpToByteStream = umpToBytestream{};
    }
    {
        std::scoped_lock converterLock(port.IncomingConverterMutex);
        port.ByteStreamToUmp = bytestreamToUMP{};
        port.ByteStreamToUmp.defaultGroup = 0;
    }
}

HRESULT IpMidiNetworkEngine::SetPortMuted(uint8_t portIndex, bool muted)
{
    auto port = GetPort(portIndex);
    RETURN_HR_IF(E_INVALIDARG, port == nullptr);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE), m_stopping || !m_started);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY), TrialMuteLocked());
    if (port->ConfiguredMuted.load() == muted && (muted || !port->SafetyMuted.load())) return S_OK;

    std::unique_lock reconfigureLock(m_reconfigureMutex);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_BUSY), m_reconfigurePending);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY), TrialMuteLocked());

    const bool previousConfiguredMuted = port->ConfiguredMuted.load();
    const bool previousSafetyMuted = port->SafetyMuted.load();
    port->ConfiguredMuted = muted;

    if (muted)
    {
        port->Muted = true;
        {
            // Wait for an already-entered sendto to finish. Future sends recheck Muted while holding this lock.
            std::scoped_lock sendLock(port->SendMutex);
        }
        std::scoped_lock dispatchLock(port->CallbackDispatchMutex);
        ResetPortData(*port);
    }
    else
    {
        // Keep the port muted while clearing state. The receiver thread will make it live
        // immediately after recreating the receive socket, before rebuilding its wait set.
        ResetPortData(*port);
    }

    m_reconfigurePortIndex = portIndex;
    m_reconfigureMuted = muted;
    m_reconfigureResult = E_PENDING;
    m_reconfigureFinished = false;
    m_reconfigurePending = true;
    if (!WSASetEvent(m_reconfigureEvent))
    {
        m_reconfigurePending = false;
        port->ConfiguredMuted = previousConfiguredMuted;
        port->SafetyMuted = previousSafetyMuted;
        port->Muted = m_trialTrafficBlocked.load() || previousSafetyMuted || previousConfiguredMuted;
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    m_reconfigureComplete.wait(reconfigureLock, [this]
        { return m_reconfigureFinished || m_stopping.load(); });
    if (m_stopping && !m_reconfigureFinished)
    {
        return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    }

    const auto result = m_reconfigureResult;
    if (FAILED(result))
    {
        port->ConfiguredMuted = previousConfiguredMuted;
        port->SafetyMuted = previousSafetyMuted;
        port->Muted = m_trialTrafficBlocked.load() || previousSafetyMuted || previousConfiguredMuted;
    }

    if (SUCCEEDED(result))
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_INFO,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"ipMIDI port mute state changed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
            TraceLoggingBool(muted, "muted"));
    }
    else
    {
        TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
            TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
            TraceLoggingWideString(L"Unable to change ipMIDI port mute state", MIDI_TRACE_EVENT_MESSAGE_FIELD),
            TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
            TraceLoggingBool(muted, "muted"),
            TraceLoggingHResult(result, MIDI_TRACE_EVENT_HRESULT_FIELD));
    }
    return result;
}

void IpMidiNetworkEngine::RestorePortConfiguredMuteState(uint8_t portIndex, bool muted) noexcept
{
    auto port = GetPort(portIndex);
    if (port == nullptr) return;
    port->ConfiguredMuted = muted;
    if (!m_trialTrafficBlocked.load())
    {
        port->Muted = m_trialTrafficBlocked.load() || port->SafetyMuted.load() || muted;
    }
}

void IpMidiNetworkEngine::RestorePortSafetyMute(
    uint8_t portIndex,
    IpMidiNetworkStatus const& previousStatus) noexcept
{
    const uint32_t portBit = 1u << portIndex;
    if ((previousStatus.SafetyMuteMask & portBit) == 0)
    {
        return;
    }

    uint32_t reasonMask{};
    if ((previousStatus.OverflowMask & portBit) != 0) reasonMask |= SafetyFaultOverflow;
    if ((previousStatus.ReceiveFailureMask & portBit) != 0) reasonMask |= SafetyFaultReceive;
    if ((previousStatus.SendFailureMask & portBit) != 0) reasonMask |= SafetyFaultSend;
    RequestSafetyMute(portIndex, reasonMask == 0 ? SafetyFaultReceive : reasonMask);
}

void IpMidiNetworkEngine::RefreshLocalIpv4Addresses()
{
    m_localIpv4AddressCount = 0;
    ULONG bufferLength = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, nullptr, &bufferLength) != ERROR_BUFFER_OVERFLOW)
    {
        return;
    }

    std::vector<uint8_t> buffer(bufferLength);
    auto addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addresses, &bufferLength) != NO_ERROR)
    {
        return;
    }

    for (auto adapter = addresses; adapter != nullptr && m_localIpv4AddressCount < m_localIpv4Addresses.size(); adapter = adapter->Next)
    {
        for (auto unicast = adapter->FirstUnicastAddress;
            unicast != nullptr && m_localIpv4AddressCount < m_localIpv4Addresses.size(); unicast = unicast->Next)
        {
            if (unicast->Address.lpSockaddr->sa_family == AF_INET)
            {
                const auto address = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                m_localIpv4Addresses[m_localIpv4AddressCount++] = address->sin_addr.s_addr;
            }
        }
    }
}

bool IpMidiNetworkEngine::IsLocalAddress(ULONG address) const
{
    for (uint8_t index = 0; index < m_localIpv4AddressCount; ++index)
    {
        if (m_localIpv4Addresses[index] == address) return true;
    }
    return false;
}

IpMidiNetworkEngine::PortContext* IpMidiNetworkEngine::GetPort(uint8_t portIndex) const noexcept
{
    if (portIndex >= m_portCount) return nullptr;
    return m_ports[portIndex].get();
}

HRESULT IpMidiNetworkEngine::QueueOutgoingUmp(uint8_t portIndex, PVOID message, UINT size, LONGLONG timestamp)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, message);
    RETURN_HR_IF(E_INVALIDARG, size == 0 || size % sizeof(uint32_t) != 0);
    if (m_stopping) return S_OK;

    auto port = GetPort(portIndex);
    RETURN_HR_IF(E_INVALIDARG, port == nullptr);
    if (!RefreshTrialEnforcement()) return S_OK;

    IpMidiPacket packet{};
    packet.Timestamp = timestamp;
    {
        std::unique_lock converterLock(port->OutgoingConverterMutex);
        if (port->Muted) return S_OK;
        if (!m_trialState.IsTrafficAllowed())
        {
            converterLock.unlock();
            RefreshTrialEnforcement();
            return S_OK;
        }
        const auto words = static_cast<uint32_t const*>(message);
        const auto wordCount = size / sizeof(uint32_t);
        for (UINT offset = 0; offset < wordCount;)
        {
            const auto messageType = internal::GetUmpMessageTypeFromFirstWord(words[offset]);
            const auto messageWordCount = internal::GetUmpLengthInMidiWordsFromFirstWord(words[offset]);
            if ((messageType != 0x1 && messageType != 0x2 && messageType != 0x3) ||
                messageWordCount == 0 || offset + messageWordCount > wordCount)
            {
                ++port->UnsupportedOutgoingCount;
                return S_OK;
            }

            for (uint8_t wordIndex = 0; wordIndex < messageWordCount; ++wordIndex)
            {
                port->UmpToByteStream.UMPStreamParse(words[offset + wordIndex]);
                while (port->UmpToByteStream.availableBS())
                {
                    if (packet.Length == packet.Bytes.size())
                    {
                        ++port->UnsupportedOutgoingCount;
                        return S_OK;
                    }
                    packet.Bytes[packet.Length++] = port->UmpToByteStream.readBS();
                }
            }
            offset += messageWordCount;
        }
    }

    if (packet.Length > 0) TryEnqueueOutgoing(*port, packet);
    return S_OK;
}

HRESULT IpMidiNetworkEngine::RegisterCallback(uint8_t portIndex, IMidiCallback* callback, uint64_t* registrationId)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, callback);
    RETURN_HR_IF_NULL(E_INVALIDARG, registrationId);
    auto port = GetPort(portIndex);
    RETURN_HR_IF(E_INVALIDARG, port == nullptr);

    std::scoped_lock dispatchLock(port->CallbackDispatchMutex);
    std::scoped_lock callbackLock(port->CallbackMutex);
    RETURN_HR_IF(E_UNEXPECTED, m_stopping.load());
    for (auto& registration : port->Callbacks)
    {
        if (registration.Id == 0)
        {
            registration.Id = port->NextCallbackRegistrationId++;
            registration.Callback = callback;
            *registrationId = registration.Id;
            return S_OK;
        }
    }
    return E_OUTOFMEMORY;
}

void IpMidiNetworkEngine::UnregisterCallback(uint8_t portIndex, uint64_t registrationId)
{
    auto port = GetPort(portIndex);
    if (port == nullptr || registrationId == 0) return;

    std::scoped_lock dispatchLock(port->CallbackDispatchMutex);
    std::scoped_lock callbackLock(port->CallbackMutex);
    for (auto& registration : port->Callbacks)
    {
        if (registration.Id == registrationId)
        {
            registration.Callback.reset();
            registration.Id = 0;
            return;
        }
    }
}

bool IpMidiNetworkEngine::TryEnqueueOutgoing(PortContext& port, IpMidiPacket const& packet)
{
    if (port.Muted || !RefreshTrialEnforcement()) return false;
    bool queueWasFull{};
    bool tripSafetyMute{};
    {
        std::unique_lock queueLock(m_outgoingMutex);
        if (port.Muted) return false;
        if (!m_trialState.IsTrafficAllowed())
        {
            queueLock.unlock();
            RefreshTrialEnforcement();
            return false;
        }
        if (port.OutgoingQueue.full())
        {
            queueWasFull = true;
            ++port.OutgoingDropCount;
            const auto now = GetTickCount64();
            tripSafetyMute = RecordOverflowDrop(
                port.OutgoingOverflowTimestamps,
                port.OutgoingOverflowTimestampHead,
                port.OutgoingOverflowTimestampCount,
                now);
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI outgoing queue overflow", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"),
                TraceLoggingUInt32(static_cast<uint32_t>(port.OutgoingOverflowTimestampCount), "window drop count"));
        }
        else
        {
            port.OutgoingQueue.push_back(packet);
        }
    }
    if (tripSafetyMute) RequestSafetyMute(port.PortIndex, SafetyFaultOverflow);
    if (queueWasFull) return false;
    m_outgoingWakeup.notify_one();
    return true;
}

bool IpMidiNetworkEngine::TryEnqueueIncoming(PortContext& port, IpMidiPacket const& packet)
{
    if (port.Muted || !RefreshTrialEnforcement()) return false;
    bool queueWasFull{};
    bool tripSafetyMute{};
    {
        std::unique_lock queueLock(m_incomingMutex);
        if (port.Muted) return false;
        if (!m_trialState.IsTrafficAllowed())
        {
            queueLock.unlock();
            RefreshTrialEnforcement();
            return false;
        }
        if (port.IncomingQueue.full())
        {
            queueWasFull = true;
            ++port.IncomingDropCount;
            const auto now = GetTickCount64();
            tripSafetyMute = RecordOverflowDrop(
                port.IncomingOverflowTimestamps,
                port.IncomingOverflowTimestampHead,
                port.IncomingOverflowTimestampCount,
                now);
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI incoming queue overflow", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"),
                TraceLoggingUInt32(static_cast<uint32_t>(port.IncomingOverflowTimestampCount), "window drop count"));
        }
        else
        {
            port.IncomingQueue.push_back(packet);
        }
    }
    if (tripSafetyMute) RequestSafetyMute(port.PortIndex, SafetyFaultOverflow);
    if (queueWasFull) return false;
    m_incomingWakeup.notify_one();
    return true;
}

bool IpMidiNetworkEngine::HasOutgoingPackets() const
{
    for (uint8_t index = 0; index < m_portCount; ++index)
        if (!m_ports[index]->OutgoingQueue.empty()) return true;
    return false;
}

bool IpMidiNetworkEngine::HasIncomingPackets() const
{
    for (uint8_t index = 0; index < m_portCount; ++index)
        if (!m_ports[index]->IncomingQueue.empty()) return true;
    return false;
}

bool IpMidiNetworkEngine::TryDequeueOutgoing(uint8_t& portIndex, IpMidiPacket& packet)
{
    for (uint8_t count = 0; count < m_portCount; ++count)
    {
        const auto index = static_cast<uint8_t>((m_nextOutgoingPort + count) % m_portCount);
        auto& queue = m_ports[index]->OutgoingQueue;
        if (m_ports[index]->Muted)
        {
            queue.clear();
            continue;
        }
        if (!queue.empty())
        {
            packet = queue.front();
            queue.pop_front();
            portIndex = index;
            m_nextOutgoingPort = static_cast<uint8_t>((index + 1) % m_portCount);
            return true;
        }
    }
    return false;
}

bool IpMidiNetworkEngine::TryDequeueIncoming(uint8_t& portIndex, IpMidiPacket& packet)
{
    for (uint8_t count = 0; count < m_portCount; ++count)
    {
        const auto index = static_cast<uint8_t>((m_nextIncomingPort + count) % m_portCount);
        auto& queue = m_ports[index]->IncomingQueue;
        if (m_ports[index]->Muted)
        {
            queue.clear();
            continue;
        }
        if (!queue.empty())
        {
            packet = queue.front();
            queue.pop_front();
            portIndex = index;
            m_nextIncomingPort = static_cast<uint8_t>((index + 1) % m_portCount);
            return true;
        }
    }
    return false;
}

void IpMidiNetworkEngine::SenderWorker(std::stop_token stopToken)
{
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    if (InetPtonA(AF_INET, IP_MIDI_MULTICAST_ADDRESS, &destination.sin_addr) != 1) return;

    while (!stopToken.stop_requested() && !m_stopping)
    {
        uint8_t portIndex{};
        IpMidiPacket packet{};
        {
            std::unique_lock queueLock(m_outgoingMutex);
            m_outgoingWakeup.wait(queueLock, [this, &stopToken]
                { return m_stopping || stopToken.stop_requested() || HasOutgoingPackets(); });
            if (m_stopping || stopToken.stop_requested()) break;
            if (!TryDequeueOutgoing(portIndex, packet)) continue;
        }

        if (!RefreshTrialEnforcement()) continue;
        auto& port = *m_ports[portIndex];
        std::unique_lock sendLock(port.SendMutex);
        if (port.Muted) continue;
        if (!m_trialState.IsTrafficAllowed())
        {
            sendLock.unlock();
            RefreshTrialEnforcement();
            continue;
        }
        destination.sin_port = htons(port.UdpPort);
        const auto sendResult = SendPacketWithRecovery(destination, packet);
        sendLock.unlock();
        if (FAILED(sendResult))
        {
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI multicast send failed after socket recovery", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
                TraceLoggingHResult(sendResult, MIDI_TRACE_EVENT_HRESULT_FIELD));
            RequestSafetyMute(portIndex, SafetyFaultSend);
        }
    }
}

void IpMidiNetworkEngine::ReceiverWorker(std::stop_token stopToken)
{
    auto coInitialize = wil::CoInitializeEx(COINIT_MULTITHREADED);
    while (!stopToken.stop_requested() && !m_stopping)
    {
        std::array<WSAEVENT, IP_MIDI_MAX_PORT_COUNT + 3> events{};
        std::array<PortContext*, IP_MIDI_MAX_PORT_COUNT + 3> eventPorts{};
        DWORD eventCount = 2;
        DWORD trialEventIndex = MAXDWORD;
        events[0] = m_stopEvent;
        events[1] = m_reconfigureEvent;
        if (m_trialState.NotificationEvent() != WSA_INVALID_EVENT)
        {
            trialEventIndex = eventCount;
            events[eventCount++] = m_trialState.NotificationEvent();
        }
        for (uint8_t index = 0; index < m_portCount; ++index)
        {
            auto& port = *m_ports[index];
            if (!port.Muted && port.ReceiveEvent != WSA_INVALID_EVENT)
            {
                events[eventCount] = port.ReceiveEvent;
                eventPorts[eventCount] = &port;
                ++eventCount;
            }
        }

        DWORD waitTimeout = WSA_INFINITE;
        if (RefreshTrialEnforcement())
        {
            waitTimeout = m_trialState.RemainingWaitMilliseconds();
        }
        const auto waitResult = WSAWaitForMultipleEvents(eventCount, events.data(), FALSE, waitTimeout, FALSE);
        if (waitResult == WSA_WAIT_TIMEOUT)
        {
            RefreshTrialEnforcement();
            continue;
        }
        if (waitResult == WSA_WAIT_FAILED)
        {
            const auto waitError = HRESULT_FROM_WIN32(WSAGetLastError());
            std::unique_lock reconfigureLock(m_reconfigureMutex);
            if (m_reconfigurePending)
            {
                m_reconfigureResult = waitError;
                m_reconfigurePending = false;
                m_reconfigureFinished = true;
                reconfigureLock.unlock();
                m_reconfigureComplete.notify_all();
            }
            break;
        }
        const auto eventIndex = waitResult - WSA_WAIT_EVENT_0;
        if (eventIndex == 0 || eventIndex >= eventCount) break;
        if (eventIndex == trialEventIndex)
        {
            m_trialState.HandleRegistryNotification();
            RefreshTrialEnforcement();
            continue;
        }
        if (eventIndex == 1)
        {
            WSAResetEvent(m_reconfigureEvent);
            if (m_trialMutePending.exchange(false))
            {
                ApplyTrialMuteOnReceiverThread();
                std::unique_lock reconfigureLock(m_reconfigureMutex);
                if (m_reconfigurePending)
                {
                    m_reconfigureResult = HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
                    m_reconfigurePending = false;
                    m_reconfigureFinished = true;
                    reconfigureLock.unlock();
                    m_reconfigureComplete.notify_all();
                }
                continue;
            }

            if (m_safetyMutePendingMask.load() != 0)
            {
                ApplyPendingSafetyMutesOnReceiverThread();
            }

            std::unique_lock reconfigureLock(m_reconfigureMutex);
            if (m_reconfigurePending)
            {
                auto& port = *m_ports[m_reconfigurePortIndex];
                if (m_trialTrafficBlocked.load())
                {
                    port.Muted = true;
                    m_reconfigureResult = HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
                }
                else if (m_reconfigureMuted)
                {
                    CloseReceiveResources(port);
                    m_reconfigureResult = S_OK;
                }
                else
                {
                    const auto sendSocketResult = RecreateSendSocket();
                    const auto createResult = SUCCEEDED(sendSocketResult)
                        ? CreateReceiveResources(port)
                        : sendSocketResult;
                    if (SUCCEEDED(createResult) && !m_trialTrafficBlocked.load())
                    {
                        ClearSafetyFault(port);
                        port.Muted = false;
                        m_reconfigureResult = S_OK;
                    }
                    else if (SUCCEEDED(createResult))
                    {
                        CloseReceiveResources(port);
                        port.Muted = true;
                        m_reconfigureResult = HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
                    }
                    else
                    {
                        m_reconfigureResult = createResult;
                    }
                }
                m_reconfigurePending = false;
                m_reconfigureFinished = true;
                reconfigureLock.unlock();
                m_reconfigureComplete.notify_all();
            }
            continue;
        }

        auto& port = *eventPorts[eventIndex];
        if (port.Muted) continue;
        WSANETWORKEVENTS networkEvents{};
        if (WSAEnumNetworkEvents(port.ReceiveSocket, port.ReceiveEvent, &networkEvents) == SOCKET_ERROR)
        {
            const auto error = WSAGetLastError();
            const auto recoveryResult = RecoverReceiveResources(port);
            if (FAILED(recoveryResult)) RequestSafetyMute(port.PortIndex, SafetyFaultReceive);
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI receive event enumeration failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"),
                TraceLoggingInt32(error, "winsock error"),
                TraceLoggingHResult(recoveryResult, "recovery result"));
            continue;
        }
        const bool receiveEventFailed =
            ((networkEvents.lNetworkEvents & FD_READ) != 0 && networkEvents.iErrorCode[FD_READ_BIT] != 0) ||
            ((networkEvents.lNetworkEvents & FD_CLOSE) != 0);
        if (receiveEventFailed)
        {
            const auto error = networkEvents.iErrorCode[FD_READ_BIT] != 0
                ? networkEvents.iErrorCode[FD_READ_BIT]
                : networkEvents.iErrorCode[FD_CLOSE_BIT];
            const auto recoveryResult = RecoverReceiveResources(port);
            if (FAILED(recoveryResult)) RequestSafetyMute(port.PortIndex, SafetyFaultReceive);
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI receive socket event failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"),
                TraceLoggingInt32(error, "winsock error"),
                TraceLoggingHResult(recoveryResult, "recovery result"));
            continue;
        }
        if ((networkEvents.lNetworkEvents & FD_READ) == 0) continue;

        for (;;)
        {
            IpMidiPacket packet{};
            sockaddr_in source{};
            int sourceLength = sizeof(source);
            const auto received = recvfrom(port.ReceiveSocket,
                reinterpret_cast<char*>(packet.Bytes.data()), static_cast<int>(packet.Bytes.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &sourceLength);
            if (received == SOCKET_ERROR)
            {
                const auto error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) break;
                if (!m_stopping)
                {
                    const auto recoveryResult = RecoverReceiveResources(port);
                    if (FAILED(recoveryResult)) RequestSafetyMute(port.PortIndex, SafetyFaultReceive);
                    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                        TraceLoggingWideString(L"ipMIDI multicast receive failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                        TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"),
                        TraceLoggingInt32(error, "winsock error"),
                        TraceLoggingHResult(recoveryResult, "recovery result"));
                }
                break;
            }
            if (received == 0)
            {
                ++port.MalformedIncomingCount;
                continue;
            }
            if (!RefreshTrialEnforcement()) continue;
            if (port.Muted) continue;
            if (!m_loopbackEnabled && IsLocalAddress(source.sin_addr.s_addr)) continue;

            packet.Length = static_cast<uint16_t>(received);
            packet.Timestamp = internal::GetCurrentMidiTimestamp();
            TryEnqueueIncoming(port, packet);
        }
    }
}

void IpMidiNetworkEngine::DispatcherWorker(std::stop_token stopToken)
{
    while (!stopToken.stop_requested() && !m_stopping)
    {
        uint8_t portIndex{};
        IpMidiPacket packet{};
        {
            std::unique_lock queueLock(m_incomingMutex);
            m_incomingWakeup.wait(queueLock, [this, &stopToken]
                { return m_stopping || stopToken.stop_requested() || HasIncomingPackets(); });
            if (m_stopping || stopToken.stop_requested()) break;
            if (!TryDequeueIncoming(portIndex, packet)) continue;
        }
        if (!RefreshTrialEnforcement()) continue;
        DispatchIncomingPacket(*m_ports[portIndex], packet);
    }
}

void IpMidiNetworkEngine::DispatchIncomingPacket(PortContext& port, IpMidiPacket const& packet)
{
    if (port.Muted || !RefreshTrialEnforcement()) return;
    std::array<uint32_t, IP_MIDI_MAX_DATAGRAM_SIZE> umpWords{};
    size_t umpWordCount = 0;
    {
        std::unique_lock converterLock(port.IncomingConverterMutex);
        if (port.Muted) return;
        if (!m_trialState.IsTrafficAllowed())
        {
            converterLock.unlock();
            RefreshTrialEnforcement();
            return;
        }
        for (uint16_t index = 0; index < packet.Length; ++index)
        {
            port.ByteStreamToUmp.bytestreamParse(packet.Bytes[index]);
            while (port.ByteStreamToUmp.availableUMP())
            {
                if (umpWordCount == umpWords.size())
                {
                    ++port.MalformedIncomingCount;
                    return;
                }
                umpWords[umpWordCount++] = port.ByteStreamToUmp.readUMP();
            }
        }
    }
    if (umpWordCount == 0) return;
    if (!RefreshTrialEnforcement()) return;

    std::unique_lock dispatchLock(port.CallbackDispatchMutex);
    if (m_stopping || port.Muted) return;
    for (auto const& registration : port.Callbacks)
    {
        if (registration.Id != 0 && registration.Callback != nullptr)
        {
            if (!m_trialState.IsTrafficAllowed())
            {
                dispatchLock.unlock();
                RefreshTrialEnforcement();
                return;
            }
            LOG_IF_FAILED(registration.Callback->Callback(
                MessageOptionFlags_ContextContainsGroupIndex,
                umpWords.data(),
                static_cast<UINT>(umpWordCount * sizeof(uint32_t)),
                packet.Timestamp,
                0));
        }
    }
}
