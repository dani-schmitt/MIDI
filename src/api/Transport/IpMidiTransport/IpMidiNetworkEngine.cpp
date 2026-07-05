// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License

#include "pch.h"

HRESULT IpMidiNetworkEngine::Initialize()
{
    std::scoped_lock lifecycleLock(m_lifecycleMutex);
    if (m_initialized)
    {
        return S_OK;
    }

    RETURN_IF_FAILED(CreateSharedResources());
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

    m_sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sendSocket == INVALID_SOCKET)
    {
        const auto error = WSAGetLastError();
        CloseSharedResources();
        return HRESULT_FROM_WIN32(error);
    }

    u_char multicastTtl = 1;
    u_char multicastLoopback = 0;
    if (setsockopt(m_sendSocket, IPPROTO_IP, IP_MULTICAST_TTL,
            reinterpret_cast<char*>(&multicastTtl), sizeof(multicastTtl)) == SOCKET_ERROR ||
        setsockopt(m_sendSocket, IPPROTO_IP, IP_MULTICAST_LOOP,
            reinterpret_cast<char*>(&multicastLoopback), sizeof(multicastLoopback)) == SOCKET_ERROR)
    {
        const auto error = WSAGetLastError();
        CloseSharedResources();
        return HRESULT_FROM_WIN32(error);
    }

    m_stopEvent = WSACreateEvent();
    if (m_stopEvent == WSA_INVALID_EVENT)
    {
        const auto error = WSAGetLastError();
        CloseSharedResources();
        return HRESULT_FROM_WIN32(error);
    }

    RefreshLocalIpv4Addresses();
    return S_OK;
}

HRESULT IpMidiNetworkEngine::PreparePort(uint8_t portIndex)
{
    std::scoped_lock lifecycleLock(m_lifecycleMutex);
    RETURN_HR_IF(E_UNEXPECTED, !m_initialized || m_started);
    RETURN_HR_IF(E_INVALIDARG, portIndex >= IP_MIDI_MAX_PORT_COUNT || portIndex != m_portCount);

    try
    {
        auto port = std::make_unique<PortContext>(portIndex);
        RETURN_IF_FAILED(CreateReceiveResources(*port));
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
            WSASetEvent(m_stopEvent);
            m_outgoingWakeup.notify_all();
            m_incomingWakeup.notify_all();
            return wil::ResultFromCaughtException();
        }
    }

    m_started = true;
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
    m_outgoingWakeup.notify_all();
    m_incomingWakeup.notify_all();
    m_senderThread.request_stop();
    m_receiverThread.request_stop();
    m_dispatcherThread.request_stop();

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
    if (m_winsockStarted)
    {
        WSACleanup();
        m_winsockStarted = false;
    }
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

    IpMidiPacket packet{};
    packet.Timestamp = timestamp;
    {
        std::scoped_lock converterLock(port->OutgoingConverterMutex);
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
    {
        std::scoped_lock queueLock(m_outgoingMutex);
        if (port.OutgoingQueue.full())
        {
            ++port.OutgoingDropCount;
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI outgoing queue overflow", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"));
            return false;
        }
        port.OutgoingQueue.push_back(packet);
    }
    m_outgoingWakeup.notify_one();
    return true;
}

bool IpMidiNetworkEngine::TryEnqueueIncoming(PortContext& port, IpMidiPacket const& packet)
{
    {
        std::scoped_lock queueLock(m_incomingMutex);
        if (port.IncomingQueue.full())
        {
            ++port.IncomingDropCount;
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_WARNING,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI incoming queue overflow", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"));
            return false;
        }
        port.IncomingQueue.push_back(packet);
    }
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

        destination.sin_port = htons(m_ports[portIndex]->UdpPort);
        const auto sent = sendto(m_sendSocket, reinterpret_cast<char const*>(packet.Bytes.data()), packet.Length, 0,
            reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        if (sent == SOCKET_ERROR || sent != packet.Length)
        {
            TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                TraceLoggingWideString(L"ipMIDI multicast send failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                TraceLoggingUInt8(static_cast<uint8_t>(portIndex + 1), "port number"),
                TraceLoggingInt32(WSAGetLastError(), "winsock error"));
        }
    }
}

void IpMidiNetworkEngine::ReceiverWorker(std::stop_token stopToken)
{
    std::array<WSAEVENT, IP_MIDI_MAX_PORT_COUNT + 1> events{};
    events[0] = m_stopEvent;
    for (uint8_t index = 0; index < m_portCount; ++index) events[index + 1] = m_ports[index]->ReceiveEvent;
    const DWORD eventCount = static_cast<DWORD>(m_portCount + 1);

    while (!stopToken.stop_requested() && !m_stopping)
    {
        const auto waitResult = WSAWaitForMultipleEvents(eventCount, events.data(), FALSE, WSA_INFINITE, FALSE);
        if (waitResult == WSA_WAIT_FAILED) break;
        const auto eventIndex = waitResult - WSA_WAIT_EVENT_0;
        if (eventIndex == 0 || eventIndex >= eventCount) break;

        auto& port = *m_ports[eventIndex - 1];
        WSANETWORKEVENTS networkEvents{};
        if (WSAEnumNetworkEvents(port.ReceiveSocket, port.ReceiveEvent, &networkEvents) == SOCKET_ERROR) continue;
        if ((networkEvents.lNetworkEvents & FD_READ) == 0 || networkEvents.iErrorCode[FD_READ_BIT] != 0) continue;

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
                    TraceLoggingWrite(MidiIpMidiTransportTelemetryProvider::Provider(), MIDI_TRACE_EVENT_ERROR,
                        TraceLoggingString(__FUNCTION__, MIDI_TRACE_EVENT_LOCATION_FIELD),
                        TraceLoggingWideString(L"ipMIDI multicast receive failed", MIDI_TRACE_EVENT_MESSAGE_FIELD),
                        TraceLoggingUInt8(static_cast<uint8_t>(port.PortIndex + 1), "port number"),
                        TraceLoggingInt32(error, "winsock error"));
                }
                break;
            }
            if (received == 0)
            {
                ++port.MalformedIncomingCount;
                continue;
            }
            if (IsLocalAddress(source.sin_addr.s_addr)) continue;

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
        DispatchIncomingPacket(*m_ports[portIndex], packet);
    }
}

void IpMidiNetworkEngine::DispatchIncomingPacket(PortContext& port, IpMidiPacket const& packet)
{
    std::array<uint32_t, IP_MIDI_MAX_DATAGRAM_SIZE> umpWords{};
    size_t umpWordCount = 0;
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
    if (umpWordCount == 0) return;

    std::scoped_lock dispatchLock(port.CallbackDispatchMutex);
    if (m_stopping) return;
    for (auto const& registration : port.Callbacks)
    {
        if (registration.Id != 0 && registration.Callback != nullptr)
        {
            LOG_IF_FAILED(registration.Callback->Callback(
                MessageOptionFlags_ContextContainsGroupIndex,
                umpWords.data(),
                static_cast<UINT>(umpWordCount * sizeof(uint32_t)),
                packet.Timestamp,
                0));
        }
    }
}
